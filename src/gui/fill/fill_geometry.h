#pragma once

#include <QtCore>
#include <QtGui>

#include <algorithm>
#include <cmath>

namespace gui::fill_geometry {

inline double cross(const QPointF &a, const QPointF &b) {
    return a.x() * b.y() - a.y() * b.x();
}

inline double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        result += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    }

    return result * 0.5;
}

inline double pathArea(const QPainterPath &path) {
    double result = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        result += std::abs(signedArea(polygon));
    }
    return result;
}

inline QTransform affineFromTriangles(const QPointF &a,
                                     const QPointF &b,
                                     const QPointF &c,
                                     const QPointF &p,
                                     const QPointF &q,
                                     const QPointF &r,
                                     double epsilon,
                                     bool *ok) {
    const QPointF d1 = b - a;
    const QPointF d2 = c - a;
    const QPointF e1 = q - p;
    const QPointF e2 = r - p;
    const double determinant = cross(d1, d2);
    if (std::abs(determinant) <= epsilon) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }
    const double m11 = (e1.x() * d2.y() - e2.x() * d1.y()) / determinant;
    const double m21 = (-e1.x() * d2.x() + e2.x() * d1.x()) / determinant;
    const double m12 = (e1.y() * d2.y() - e2.y() * d1.y()) / determinant;
    const double m22 = (-e1.y() * d2.x() + e2.y() * d1.x()) / determinant;
    const double dx = p.x() - m11 * a.x() - m21 * a.y();
    const double dy = p.y() - m12 * a.x() - m22 * a.y();
    if (ok != nullptr) {
        *ok = std::isfinite(m11) && std::isfinite(m12) && std::isfinite(m21)
            && std::isfinite(m22) && std::isfinite(dx) && std::isfinite(dy);
    }
    return QTransform(m11, m12, m21, m22, dx, dy);
}

inline double distanceSquaredToSegment(const QPointF &point, const QPointF &a, const QPointF &b,
                                       double epsilon) {
    const QPointF ab = b - a;
    const double length2 = QPointF::dotProduct(ab, ab);
    if (length2 <= epsilon) {
        return QPointF::dotProduct(point - a, point - a);
    }
    const double t = std::clamp(QPointF::dotProduct(point - a, ab) / length2, 0.0, 1.0);
    const QPointF delta = point - (a + ab * t);

    return QPointF::dotProduct(delta, delta);
}

inline QPolygonF convexHullFromSortedPoints(const QVector<QPointF> &points, double epsilon) {
    QVector<QPointF> hull;
    hull.reserve(points.size() * 2);

    for (const QPointF &point : points) {
        while (hull.size() >= 2
               && cross(hull.back() - hull[hull.size() - 2], point - hull.back()) <= epsilon) {
            hull.removeLast();
        }
        hull.push_back(point);
    }
    const int lowerSize = hull.size();
    for (int i = points.size() - 2; i >= 0; --i) {
        while (hull.size() > lowerSize
               && cross(hull.back() - hull[hull.size() - 2], points[i] - hull.back()) <= epsilon) {
            hull.removeLast();
        }
        hull.push_back(points[i]);
    }
    hull.removeLast();

    return QPolygonF(hull);
}

} // namespace gui::fill_geometry
