#include "image_io.h"
#include "svg_vector_objects.h"

#include <QtCore>
#include <QtGui>

namespace {

int fail(const QString &message) {
    qCritical().noquote() << message;
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);

    if (!gui::supportedImageSuffixes().contains(QStringLiteral("svg"))) {
        return fail(QStringLiteral("SVG is missing from the supported guide suffixes"));
    }

    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"16\" "
        "viewBox=\"0 0 32 16\"><rect width=\"16\" height=\"16\" fill=\"#ff0000\"/>"
        "</svg>");
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return fail(QStringLiteral("Could not create a temporary directory"));
    }
    const QString path = directory.filePath(QStringLiteral("guide.svg"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(svg) != svg.size()) {
        return fail(QStringLiteral("Could not write the SVG fixture"));
    }
    file.close();

    QByteArray format;
    QString error;
    const QImage imported = gui::readGuideImage(path, &format, &error);
    if (imported.isNull()) {
        return fail(QStringLiteral("SVG import failed: %1").arg(error));
    }
    if (format != QByteArrayLiteral("svg") || imported.size() != QSize(32, 16)) {
        return fail(QStringLiteral("SVG import returned the wrong format or dimensions"));
    }
    const QColor painted = imported.pixelColor(8, 8);
    const QColor transparent = imported.pixelColor(24, 8);
    if (painted.red() != 255 || painted.green() != 0 || painted.blue() != 0
        || painted.alpha() != 255 || transparent.alpha() != 0) {
        return fail(QStringLiteral("SVG pixels were rendered incorrectly"));
    }

    const gui::SvgVectorDocument vectorDocument =
        gui::extractSvgVectorObjects(svg, imported.size());
    if (!vectorDocument.supportsObjectSelection()
        || vectorDocument.objects.size() != 1) {
        return fail(QStringLiteral("SVG vector objects were not extracted: %1")
                        .arg(vectorDocument.fallbackReason));
    }
    if (!gui::useSvgObjectSelection(vectorDocument, false)
        || gui::useSvgObjectSelection(vectorDocument, true)) {
        return fail(QStringLiteral(
            "Explicit SVG raster fallback did not override object selection"));
    }
    const gui::SvgVectorObjectHit vectorHit =
        gui::svgVectorObjectAt(vectorDocument, QPointF(8.0, 8.0));
    if (!vectorHit.valid() || vectorHit.color != QColor(QStringLiteral("#ff0000"))
        || gui::svgVectorObjectAt(vectorDocument, QPointF(24.0, 8.0)).valid()) {
        const QRectF objectBounds = vectorDocument.objects.front().path.boundingRect();
        return fail(QStringLiteral("SVG vector object hit testing failed: valid=%1 color=%2 bounds=%3,%4 %5x%6 contains=%7")
                        .arg(vectorHit.valid())
                        .arg(vectorHit.color.name(QColor::HexArgb))
                        .arg(objectBounds.x())
                        .arg(objectBounds.y())
                        .arg(objectBounds.width())
                        .arg(objectBounds.height())
                        .arg(vectorDocument.objects.front().path.contains(QPointF(8.0, 8.0))));
    }

    const QByteArray layeredSvg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\" "
        "viewBox=\"0 0 40 20\"><g transform=\"translate(2 0)\">"
        "<rect width=\"20\" height=\"20\" fill=\"#ff0000\"/>"
        "<path d=\"M10 0 H30 V20 H10 Z\" fill=\"#0000ff\"/>"
        "</g></svg>");
    const gui::SvgVectorDocument layered =
        gui::extractSvgVectorObjects(layeredSvg, QSize(40, 20));
    const gui::SvgVectorObjectHit topmost =
        gui::svgVectorObjectAt(layered, QPointF(15.0, 10.0));
    if (!layered.supportsObjectSelection() || layered.objects.size() != 2
        || !topmost.valid() || topmost.color != QColor(QStringLiteral("#0000ff"))
        || !topmost.path.contains(QPointF(31.0, 10.0))) {
        return fail(QStringLiteral("SVG paint order or nested transform extraction failed"));
    }

    const QByteArray gradientSvg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\">"
        "<defs><linearGradient id=\"g\"><stop stop-color=\"red\"/>"
        "<stop offset=\"1\" stop-color=\"blue\"/></linearGradient></defs>"
        "<rect width=\"20\" height=\"20\" fill=\"url(#g)\"/></svg>");
    const gui::SvgVectorDocument gradient =
        gui::extractSvgVectorObjects(gradientSvg, QSize(20, 20));
    if (gradient.supportsObjectSelection() || gradient.fallbackReason.isEmpty()) {
        return fail(QStringLiteral("Unsupported SVG paints did not request raster fallback"));
    }

    const QByteArray textSvg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\">"
        "<path d=\"M31,3h38l28,28v38l-28,28h-38l-28-28v-38z\" fill=\"#aa2233\"/>"
        "<text x=\"50\" y=\"68\" font-size=\"48\" fill=\"#fff\" "
        "text-anchor=\"middle\">410</text></svg>");
    const gui::SvgVectorDocument textDocument =
        gui::extractSvgVectorObjects(textSvg, QSize(100, 100));
    if (!textDocument.supportsObjectSelection() || textDocument.objects.size() != 4) {
        return fail(QStringLiteral("SVG text was not converted to glyph outlines: %1")
                        .arg(textDocument.fallbackReason));
    }
    const gui::SvgVectorObject &middleDigit = textDocument.objects.at(2);
    if (middleDigit.color != QColor(Qt::white)
        || middleDigit.path.isEmpty()
        || !middleDigit.path.boundingRect().contains(QPointF(50.0, 50.0))) {
        return fail(QStringLiteral("SVG text glyph outlines have incorrect geometry or paint"));
    }

    error.clear();
    const QImage restored = gui::decodeGuideImage(svg, QStringLiteral("svg"), &error);
    if (restored.size() != imported.size() || restored.pixelColor(8, 8) != painted) {
        return fail(QStringLiteral("Embedded SVG decode failed: %1").arg(error));
    }

    error.clear();
    if (!gui::decodeGuideImage(QByteArrayLiteral("not svg"), QStringLiteral("svg"), &error).isNull()
        || error.isEmpty()) {
        return fail(QStringLiteral("Invalid SVG data was not rejected with an error"));
    }
    return 0;
}
