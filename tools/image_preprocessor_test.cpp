#include "image_preprocessor.h"

#include <QImage>
#include <QtCore>

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
            if (!require(first.pixelColor(x, y).alpha() == source.pixelColor(x, y).alpha(),
                         "preprocessing changed the alpha channel")) {
                return 1;
            }
        }
    }

    QImage flat(12, 9, QImage::Format_ARGB32);
    flat.fill(qRgba(42, 120, 210, 173));
    const QImage flatResult = gui::preprocessImage(flat, settings);
    if (!require(flatResult.size() == flat.size(), "single-colour preprocessing failed")
        || !require(flatResult.pixelColor(4, 4).alpha() == 173, "single-colour alpha was not preserved")) {
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
    settings.minimumColorFraction = 0.05;
    const gui::ImagePreprocessResult bucketed = gui::preprocessImageDetailed(rareColor, settings);
    if (!require(bucketed.retainedColorCount == 1, "rare HSV cluster was not bucketed")
        || !require(bucketed.image.pixelColor(99, 0).red() > bucketed.image.pixelColor(99, 0).blue(),
                    "rare color was not mapped to the retained cluster")) {
        return 1;
    }
    return 0;
}
