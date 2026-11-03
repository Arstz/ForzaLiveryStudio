#include "selection_geometry.h"
#include "shape_geometry_store.h"

#include <QCoreApplication>
#include <QTextStream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void appendRectangle(gui::ShapeGeometry &geometry, const QRectF &rect) {
    geometry.triangles.push_back({rect.topLeft(), rect.topRight(), rect.bottomRight()});
    geometry.triangles.push_back({rect.topLeft(), rect.bottomLeft(), rect.bottomRight()});
}

void testVectorBody() {
    gui::ShapeGeometry geometry;
    geometry.triangles.push_back({{0, 0}, {10, 0}, {0, 10}});
    geometry.triangles.push_back({{20, 20}, {30, 20}, {20, 30}, 0, 0, 0});
    const QPainterPath body = gui::vectorSelectionBody(geometry);

    require(body.contains({2, 2}), "triangle body must be selectable");
    require(!body.contains({8, 8}), "empty triangle corners must not be selectable");
    require(!body.contains({22, 22}), "transparent triangles must not be selectable");
    require(body.intersects(QRectF(7, 1, 1, 1)), "marquee must select a touched edge away from the center");
    require(!body.intersects(QRectF(8, 8, 1, 1)), "marquee must ignore empty corners");
}

void testCompoundBody() {
    gui::ShapeGeometry geometry;
    appendRectangle(geometry, QRectF(0, 0, 10, 3));
    appendRectangle(geometry, QRectF(0, 7, 10, 3));
    appendRectangle(geometry, QRectF(0, 3, 3, 4));
    appendRectangle(geometry, QRectF(7, 3, 3, 4));
    appendRectangle(geometry, QRectF(1, 1, 1, 1));
    const QPainterPath body = gui::vectorSelectionBody(geometry);
    QTransform transform;

    require(body.contains({1.5, 1.5}), "overlapping triangles must remain filled");
    require(body.contains({8, 5}), "mixed winding must preserve the body");
    require(!body.contains({5, 5}), "compound shape holes must remain empty");
    require(!body.intersects(QRectF(4, 4, 2, 2)), "marquee wholly inside a hole must not select");
    require(body.intersects(QRectF(-1, -1, 12, 12)), "marquee enclosing a shape must select");
    transform.translate(40, 60);
    transform.rotate(37);
    transform.shear(0.4, 0);
    transform.scale(-2, 0.5);
    const QPainterPath transformed = transform.map(body);
    require(transformed.contains(transform.map(QPointF(8, 5))), "transformed body must remain selectable");
    require(!transformed.contains(transform.map(QPointF(5, 5))), "transformed holes must remain empty");
}

void testRasterBody() {
    QImage image(4, 4, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    image.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    image.setPixelColor(1, 0, QColor(255, 0, 0, 1));
    image.setPixelColor(0, 1, QColor(255, 0, 0, 255));
    const QPainterPath body = gui::rasterSelectionBody(image);

    require(body.contains({-1.5, 1.5}), "raster alpha must match renderer vertical orientation");
    require(body.contains({-0.5, 1.5}), "partially transparent raster pixels must be selectable");
    require(body.contains({-1.5, 0.5}), "adjacent alpha runs must remain filled");
    require(!body.contains({-0.5, 0.5}), "transparent raster pixels must remain empty");
    require(!body.contains({-1.5, -1.5}), "raster transparency must not be vertically mirrored");
    require(body.intersects(QRectF(-1.8, 1.2, 0.2, 0.2)), "marquee must select raster alpha");
    require(!body.intersects(QRectF(0.2, 0.2, 0.4, 0.4)), "marquee must ignore raster transparency");
    image.fill(Qt::transparent);
    require(gui::rasterSelectionBody(image).isEmpty(), "transparent images must have no selection body");
    require(gui::rasterSelectionBody(QImage()).isEmpty(), "missing images must have no selection body");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        testVectorBody();
        testCompoundBody();
        testRasterBody();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "Selection geometry checks passed\n";
    return 0;
}
