#include "selection_geometry.h"

#include "shape_geometry_store.h"

#include <QRegion>
#include <algorithm>

namespace gui {

bool vectorHasSoftAlpha(const ShapeGeometry &geometry) {
    return std::any_of(geometry.triangles.cbegin(), geometry.triangles.cend(), [](const ShapeTriangle &triangle) {
        const bool visible = triangle.alpha0 > 0.0 || triangle.alpha1 > 0.0 || triangle.alpha2 > 0.0;

        return visible && (triangle.alpha0 < 1.0 || triangle.alpha1 < 1.0 || triangle.alpha2 < 1.0);
    });
}

bool rasterHasSoftAlpha(const QImage &image) {
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < pixels.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            const int alpha = qAlpha(row[x]);
            if (alpha > 0 && alpha < 255) {
                return true;
            }
        }
    }

    return false;
}

QPainterPath vectorSelectionBody(const ShapeGeometry &geometry) {
    QPainterPath path;

    path.setFillRule(Qt::WindingFill);
    if (geometry.triangles.isEmpty()) {
        path.addRect(-geometry.width * 0.5, -geometry.height * 0.5,
                     geometry.width, geometry.height);

        return path;
    }
    for (const ShapeTriangle &triangle : geometry.triangles) {
        if (triangle.alpha0 <= 0.0 && triangle.alpha1 <= 0.0 && triangle.alpha2 <= 0.0) {
            continue;
        }
        QPolygonF polygon({triangle.p0, triangle.p1, triangle.p2});
        const QPointF first = triangle.p1 - triangle.p0;
        const QPointF second = triangle.p2 - triangle.p0;
        const double area = first.x() * second.y() - first.y() * second.x();

        if (area == 0.0) {
            continue;
        }
        if (area < 0.0) {
            std::swap(polygon[1], polygon[2]);
        }
        path.addPolygon(polygon);
        path.closeSubpath();
    }

    return path.simplified();
}

QPainterPath rasterSelectionBody(const QImage &image) {
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    QRegion region;
    QPainterPath path;
    QTransform imageToLocal;

    for (int y = 0; y < pixels.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        int x = 0;

        while (x < pixels.width()) {
            while (x < pixels.width() && qAlpha(row[x]) == 0) {
                ++x;
            }
            const int start = x;
            while (x < pixels.width() && qAlpha(row[x]) > 0) {
                ++x;
            }
            if (x > start) {
                region += QRect(start, y, x - start, 1);
            }
        }
    }
    path.setFillRule(Qt::WindingFill);
    for (const QRect &rect : region) {
        path.addRect(QRectF(rect));
    }
    imageToLocal.translate(-pixels.width() * 0.5, pixels.height() * 0.5);
    imageToLocal.scale(1.0, -1.0);

    return imageToLocal.map(path.simplified());
}

} // namespace gui
