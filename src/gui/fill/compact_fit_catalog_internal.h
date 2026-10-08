#pragma once

#include "compact_fit_catalog.h"

#include <array>
#include <memory>
#include <stdexcept>

namespace gui::catalog {

inline constexpr double kCoordinateScale = 1000000.0;
inline constexpr double kVerificationClearance = 4.0 / kCoordinateScale;
inline constexpr double kMaximumCoordinate = 100000000.0;
inline constexpr double kMinimumTolerance = 0.0001;
inline constexpr double kGeometryFraction = 0.2;
inline constexpr double kEnvelopeFraction = 0.6;
inline constexpr double kMinimumDeterminant = 1e-12;

using Polygons = QVector<QPolygonF>;

class PointContainment {
public:
    explicit PointContainment(const Polygons &polygons);
    bool contains(const QPointF &point) const;

private:
    struct Edge {
        QPointF start;
        QPointF end;
        double length = 0.0;
    };

    QPainterPath path_;
    QRectF bounds_;
    QVector<Edge> edges_;
    QVector<QVector<int>> bands_;
    double step_ = 1.0;
};

struct Region {
    Polygons required;
    Polygons permitted;
    Polygons visible;
    Polygons spillFree;
    Polygons leeway;
    QPainterPath requiredPath;
    QPainterPath permittedPath;
    QPainterPath spillFreePath;
    std::shared_ptr<const PointContainment> spillFreeContainment;
    QRectF bounds;
    bool flexibleBoundary = false;
    double area = 0.0;
    double originalArea = 0.0;
    double tolerance = 0.0;
};

struct Candidate {
    PenPlacement placement;
    Polygons polygons;
    QPainterPath path;
    QRectF bounds;
    double gain = 0.0;
    double spill = 0.0;
};

QJsonObject geometryPerformance();

double signedArea(const QPolygonF &polygon);
double area(const Polygons &polygons);
Polygons unite(const Polygons &polygons);
Polygons subtract(const Polygons &subject, const Polygons &clip);
Polygons intersect(const Polygons &subject, const Polygons &clip);
QPainterPath painterPath(const Polygons &polygons);
QPolygonF convexHull(QPolygonF points);
Polygons mapped(const PenPrimitive &shape, const QTransform &transform);
Polygons expanded(const Polygons &polygons, double radius);
Polygons interiorSupport(const Polygons &polygons, double allowance);
QTransform emittedTransform(const QTransform &transform);
QTransform affineFromAnchors(const std::array<QPointF, 3> &source,
                             const std::array<QPointF, 3> &target);
Region buildRegion(const PenFillRequest &request,
                   const std::function<bool()> &cancelled);
Region leewayAdjustedRegion(const Region &region, const Polygons &leeway,
                            double outwardAllowance);
QVector<Candidate> completeCover(const Region &region,
                                const QVector<Primitive> &primitives,
                                const std::function<bool()> &cancelled);
QVector<Candidate> compactCover(const PenFillRequest &request, const Region &region,
                               const QVector<Primitive> &primitives,
                               const QVector<Candidate> &proposals,
                               const std::function<bool()> &cancelled,
                               QJsonObject *diagnostics);

} // namespace gui::catalog
