#pragma once

#include <QtCore>
#include <QtGui/QPainterPath>
#include <algorithm>
#include <cmath>

namespace gui {

struct FillBoundarySegment {
    QPointF start;
    QPointF control;
    QPointF end;
    bool curved = false;
    QPointF control2;

    QPointF point(double t) const {
        const double u = 1.0 - t;
        return curved ? start * (u * u * u) + control * (3 * u * u * t) +
                            control2 * (3 * u * t * t) + end * (t * t * t)
                      : start * u + end * t;
    }
    QPointF derivative(double t) const {
        const double u = 1.0 - t;
        return curved ? (control - start) * (3 * u * u) + (control2 - control) * (6 * u * t) +
                            (end - control2) * (3 * t * t)
                      : end - start;
    }
    QPointF secondDerivative(double t) const {
        return curved ? (control2 - control * 2 + start) * (6 * (1 - t)) +
                            (end - control2 * 2 + control) * (6 * t)
                      : QPointF{};
    }
    double controlLength() const {
        return curved ? QLineF(start, control).length() + QLineF(control, control2).length() +
                            QLineF(control2, end).length()
                      : QLineF(start, end).length();
    }
    double flatness() const {
        if (!curved)
            return 0.0;
        // Distance to the chord's parameter-matched controls also bounds collinear overshoot.
        return std::max(QLineF(control, start + (end - start) / 3).length(),
                        QLineF(control2, start + (end - start) * (2.0 / 3)).length());
    }
    std::pair<FillBoundarySegment, FillBoundarySegment> split(double t = 0.5) const {
        const QPointF a = start * (1 - t) + control * t, b = control * (1 - t) + control2 * t;
        const QPointF c = control2 * (1 - t) + end * t, d = a * (1 - t) + b * t,
                      e = b * (1 - t) + c * t;
        const QPointF m = d * (1 - t) + e * t;
        if (!curved) {
            const QPointF p = point(t);
            return {{start, {}, p, false, {}}, {p, {}, end, false, {}}};
        }
        return {{start, a, m, true, d}, {m, e, end, true, c}};
    }
    void appendTo(QPainterPath &path) const {
        if (curved)
            path.cubicTo(control, control2, end);
        else
            path.lineTo(end);
    }
    double signedArea() const {
        // Three-point Gauss quadrature integrates cross(B,B') exactly (degree <= 5).
        const auto crossAt = [this](double t) {
            const auto p = point(t), d = derivative(t);
            return p.x() * d.y() - p.y() * d.x();
        };
        const double h = std::sqrt(3.0 / 5.0) * 0.5;
        return (crossAt(0.5 - h) * 5 + crossAt(0.5) * 8 + crossAt(0.5 + h) * 5) / 36;
    }
};

} // namespace gui
