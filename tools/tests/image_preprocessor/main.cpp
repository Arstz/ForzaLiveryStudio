#include "image_preprocessor.h"

#include <QImage>
#include <QtCore>

#include <algorithm>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) {
        qCritical().noquote() << message;
        return false;
    }
    return true;
}

} // namespace

int main()
{
    QImage source(40, 28, QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const int alpha = (x * 19 + y * 11) % 256;
            line[x] = qRgba((x * 13 + y * 3) % 256,
                            (x * 5 + y * 17) % 256,
                            (x * 23 + y * 7) % 256,
                            alpha);
        }
    }

    gui::ImagePreprocessSettings settings = gui::ImagePreprocessSettings::animeDetail();
    settings.smoothingPasses = 1;
    settings.smoothingDiameter = 5;
    settings.quantizationIterations = 12;
    const gui::ImagePreprocessResult detailed = gui::preprocessImageDetailed(source, settings);
    const QImage first = detailed.image;
    const QImage second = gui::preprocessImage(source, settings);
    if (!require(!first.isNull(), "preprocessing returned a null image")
        || !require(first.size() == source.size(), "preprocessing changed image dimensions")
        || !require(first == second, "preprocessing is not deterministic")
        || !require(detailed.retainedColorCount >= 1 && detailed.retainedColorCount <= settings.colors,
                    "preprocessing returned an invalid retained-color count")) {
        return 1;
    }
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            const int expectedAlpha = source.pixelColor(x, y).alpha() == 0 ? 0 : 255;
            if (!require(first.pixelColor(x, y).alpha() == expectedAlpha,
                         "preprocessing did not normalize alpha to transparent or opaque")) {
                return 1;
            }
        }
    }

    QImage flat(12, 9, QImage::Format_ARGB32);
    flat.fill(qRgba(42, 120, 210, 173));
    const QImage flatResult = gui::preprocessImage(flat, settings);
    if (!require(flatResult.size() == flat.size(), "single-colour preprocessing failed")
        || !require(flatResult.pixelColor(4, 4).alpha() == 255, "single-colour output was not opaque")) {
        return 1;
    }
    QImage transparent(5, 5, QImage::Format_ARGB32);
    transparent.fill(Qt::transparent);
    const QImage transparentResult = gui::preprocessImage(transparent, settings);
    if (!require(transparentResult.pixelColor(2, 2).alpha() == 0,
                 "fully transparent input did not remain transparent")) {
        return 1;
    }

    QImage rareColor(100, 1, QImage::Format_ARGB32);
    rareColor.fill(qRgb(230, 35, 25));
    rareColor.setPixel(99, 0, qRgb(20, 40, 230));
    settings.colors = 2;
    settings.smoothingPasses = 0;
    settings.flattenStrength = 0.0;
    settings.saturationRestore = 0.0;
    settings.detailRestore = 0.0;
    settings.lineMode = false;
    settings.minimumColorFraction = 0.05;
    const gui::ImagePreprocessResult bucketed = gui::preprocessImageDetailed(rareColor, settings);
    if (!require(bucketed.retainedColorCount == 1, "rare HSV cluster was not bucketed")
        || !require(bucketed.image.pixelColor(99, 0).red() > bucketed.image.pixelColor(99, 0).blue(),
                    "rare color was not mapped to the retained cluster")) {
        return 1;
    }

    QImage speckled(9, 9, QImage::Format_ARGB32);
    speckled.fill(qRgb(240, 20, 20));
    speckled.setPixel(4, 4, qRgb(20, 220, 40));
    speckled.setPixel(0, 0, qRgba(0, 0, 0, 0));
    settings.fixedPalette = true;
    settings.paletteColors = {QColor(240, 20, 20), QColor(20, 220, 40)};
    settings.speckleSize = 4;
    const gui::ImagePreprocessResult cleaned = gui::preprocessImageDetailed(speckled, settings);
    if (!require(cleaned.image.pixelColor(4, 4).rgb() == QColor(240, 20, 20).rgb(),
                 "small color component was not replaced by its dominant neighbor")
        || !require(cleaned.image.pixelColor(0, 0).alpha() == 0,
                    "speckle cleanup filled a transparent pixel")) {
        return 1;
    }

    settings.fixedPalette = true;
    settings.paletteColors = {QColor(255, 0, 0), QColor(0, 255, 0)};
    settings.saturationRestore = 0.75;
    settings.detailRestore = 0.75;
    const gui::ImagePreprocessResult fixed = gui::preprocessImageDetailed(source, settings);
    if (!require(fixed.retainedPalette.size() == 2, "fixed palette size was not preserved")) {
        return 1;
    }
    for (int y = 0; y < fixed.image.height(); ++y) {
        for (int x = 0; x < fixed.image.width(); ++x) {
            const QColor color = fixed.image.pixelColor(x, y);
            if (color.alpha() == 0) {
                continue;
            }
            const bool inPalette = (color.red() == 255 && color.green() == 0 && color.blue() == 0)
                || (color.red() == 0 && color.green() == 255 && color.blue() == 0);
            if (!require(inPalette, "fixed-palette output contains a generated color")) {
                return 1;
            }
        }
    }

    settings.fixedPalette = false;
    settings.colors = 3;
    settings.paletteColors = {QColor(255, 0, 255)};
    QImage opaqueSource = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < opaqueSource.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(opaqueSource.scanLine(y));
        for (int x = 0; x < opaqueSource.width(); ++x) {
            line[x] = qRgb(qRed(line[x]), qGreen(line[x]), qBlue(line[x]));
        }
    }
    const gui::ImagePreprocessResult locked = gui::preprocessImageDetailed(opaqueSource, settings);
    const bool retainedLockedColor = std::any_of(
        locked.retainedPalette.cbegin(), locked.retainedPalette.cend(), [](const QColor &color) {
            return color.red() == 255 && color.green() == 0 && color.blue() == 255;
        });
    if (!require(retainedLockedColor, "manually locked palette color was discarded")) {
        return 1;
    }
    for (int y = 0; y < locked.image.height(); ++y) {
        for (int x = 0; x < locked.image.width(); ++x) {
            const QColor color = locked.image.pixelColor(x, y);
            if (color.alpha() == 0) {
                continue;
            }
            const bool retained = std::any_of(
                locked.retainedPalette.cbegin(), locked.retainedPalette.cend(),
                [&](const QColor &paletteColor) { return paletteColor.rgb() == color.rgb(); });
            if (!require(retained, "detail restoration introduced a non-palette color")) {
                return 1;
            }
        }
    }

    // A manual line override reserves one of the generated palette slots and
    // line pixels remain on that immutable label through cleanup.
    QImage lined(21, 11, QImage::Format_ARGB32);
    lined.fill(qRgb(245, 245, 245));
    for (int x = 2; x < 19; ++x) {
        lined.setPixel(x, 5, qRgb(12, 12, 12));
    }
    settings = gui::ImagePreprocessSettings::animeDetail();
    settings.colors = 2;
    settings.smoothingPasses = 0;
    settings.flattenStrength = 0.0;
    settings.saturationRestore = 0.0;
    settings.detailRestore = 0.0;
    settings.minimumColorFraction = 0.0;
    settings.speckleSize = 64;
    settings.edgeCleanupPasses = 2;
    settings.lineMode = true;
    settings.lineColor = QColor(20, 60, 210);
    const gui::ImagePreprocessResult separatedLine = gui::preprocessImageDetailed(lined, settings);
    if (!require(separatedLine.retainedColorCount == 2,
                 "dedicated line color did not consume one generated palette slot")
        || !require(separatedLine.image.pixelColor(10, 5).rgb() == settings.lineColor.rgb(),
                    "immutable line label did not retain the selected line color")) {
        return 1;
    }
    settings.lineColor = QColor();
    const gui::ImagePreprocessResult automaticLine = gui::preprocessImageDetailed(lined, settings);
    const QColor automaticLinePixel = automaticLine.image.pixelColor(10, 5);
    if (!require(automaticLine.retainedColorCount == 2,
                 "automatically derived line color did not reserve a palette slot")
        || !require(automaticLinePixel.lightness() < 64,
                    "automatic line color was not derived from the dark line pixels")) {
        return 1;
    }
    for (int y = 0; y < automaticLine.image.height(); ++y) {
        for (int x = 0; x < automaticLine.image.width(); ++x) {
            const QColor output = automaticLine.image.pixelColor(x, y);
            const bool retained = std::any_of(
                automaticLine.retainedPalette.cbegin(), automaticLine.retainedPalette.cend(),
                [&](const QColor &paletteColor) { return paletteColor.rgb() == output.rgb(); });
            if (!require(retained, "label pipeline emitted a non-palette RGB value")) {
                return 1;
            }
        }
    }

    // Region flattening treats a low-gradient, edge-bounded area as one region
    // and replaces all of its labels with the dominant label.
    QImage mixedRegion(20, 12, QImage::Format_ARGB32);
    mixedRegion.fill(qRgb(100, 100, 100));
    for (int y = 3; y < 9; ++y) {
        for (int x = 7; x < 12; ++x) {
            mixedRegion.setPixel(x, y, qRgb(110, 110, 110));
        }
    }
    settings.fixedPalette = true;
    settings.paletteColors = {QColor(100, 100, 100), QColor(110, 110, 110)};
    settings.lineMode = false;
    settings.edgeCleanupPasses = 0;
    settings.forceFlatFills = true;
    settings.flatFillMinimumArea = 1;
    const gui::ImagePreprocessResult flattenedRegion = gui::preprocessImageDetailed(mixedRegion, settings);
    for (int y = 0; y < flattenedRegion.image.height(); ++y) {
        for (int x = 0; x < flattenedRegion.image.width(); ++x) {
            if (!require(flattenedRegion.image.pixelColor(x, y).rgb() == QColor(100, 100, 100).rgb(),
                         "edge-bounded region was not flattened to its dominant label")) {
                return 1;
            }
        }
    }
    return 0;
}
