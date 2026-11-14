#include "color_edge_snap.h"

#include <QLineF>
#include <QPainter>
#include <QTextStream>

#include <cmath>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

QImage boundaryImage(QColor first, QColor second) {
    QImage image(128, 128, QImage::Format_ARGB32_Premultiplied);
    image.fill(first);
    for (int y = 0; y < image.height(); ++y)
        for (int x = image.width() / 2; x < image.width(); ++x)
            image.setPixelColor(x, y, second);
    return image;
}

void requireNear(QPointF actual, QPointF expected, const char *message) {
    require(QLineF(actual, expected).length() < 1e-6, message);
}

void testBoundaries() {
    const QPointF cursor(70, 40.25);
    for (const auto &image : {boundaryImage(Qt::black, Qt::white),
                             boundaryImage(Qt::red, Qt::green),
                             boundaryImage(Qt::transparent, Qt::black)}) {
        requireNear(gui::snapToColorEdge(image, {}, cursor, 12), {64, cursor.y()}, "Color or alpha edge was missed");
        requireNear(gui::snapToColorEdge(image, {}, cursor, 3), cursor, "Snap exceeded the search radius");
    }
    requireNear(gui::snapToColorEdge(boundaryImage(Qt::white, Qt::white), {}, cursor, 12), cursor,
                "Uniform image moved the cursor");
    requireNear(gui::snapToColorEdge(boundaryImage(QColor(100, 100, 100), QColor(110, 110, 110)), {}, cursor, 12), cursor,
                "Low contrast moved the cursor");
}

void testTransforms() {
    const QImage image = boundaryImage(Qt::black, Qt::white);
    for (double scale : {0.05, 0.5, 1.0, 20.0}) {
        QTransform transform;
        transform.translate(180, 120);
        transform.rotate(37);
        transform.scale(-scale, scale * 1.5);
        const QPointF edge = transform.map(QPointF(64, 60.25));
        const QPointF normal = transform.map(QPointF(65, 60.25)) - edge;
        const QPointF cursor = edge + normal / QLineF({}, normal).length() * 4.0;
        const QPointF snapped = gui::snapToColorEdge(image, transform, cursor, 12);
        require(QLineF(snapped, cursor).length() <= 12.0, "Transformed snap exceeded its canvas radius");
        const QPointF imagePoint = transform.inverted().map(snapped);
        require(QLineF(snapped, transform.map(QPointF(64, imagePoint.y()))).length() < 1.0,
                "Transformed edge was misplaced by more than a canvas pixel");
    }
    QTransform singular;
    singular.scale(0, 1);
    requireNear(gui::snapToColorEdge(image, singular, {64, 40}, 12), {64, 40}, "Singular transform moved the cursor");
    requireNear(gui::snapToColorEdge({}, {}, {64, 40}, 12), {64, 40}, "Missing image moved the cursor");
}

void testCompositedPixels() {
    QImage image = boundaryImage(Qt::black, Qt::white);
    {
        QPainter painter(&image);
        painter.fillRect(QRect(0, 0, 80, 128), Qt::white);
        painter.fillRect(QRect(80, 0, 48, 128), Qt::black);
    }
    requireNear(gui::snapToColorEdge(image, {}, {70, 40.25}, 12), {80, 40.25},
                "Covered boundary attracted snapping instead of the visible edge");
    {
        QPainter painter(&image);
        painter.fillRect(image.rect(), Qt::white);
        painter.setOpacity(0.05);
        painter.fillRect(QRect(64, 0, 64, 128), Qt::black);
    }
    requireNear(gui::snapToColorEdge(image, {}, {70, 40.25}, 12), {70, 40.25},
                "Snapping ignored the displayed opacity");

    image = boundaryImage(Qt::black, Qt::white).copy(QRect(48, 20, 40, 40));
    image.setDevicePixelRatio(2.0);
    const QTransform imageToScreen(0.5, 0.0, 0.0, 0.5, 24.0, 10.0);
    requireNear(gui::snapToColorEdge(image, imageToScreen, {37, 20.25}, 12), {32, 20.25},
                "Cropped canvas pixels lost their screen position or display scale");
}

} // namespace

int main() {
    try {
        testBoundaries();
        testTransforms();
        testCompositedPixels();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "Color edge snapping tests passed\n";
    return 0;
}
