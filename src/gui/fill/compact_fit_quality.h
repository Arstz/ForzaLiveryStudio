#pragma once

#include "catalog_cover_internal.h"
#include <limits>

namespace gui::compact {

struct BoundaryMetrics {
    int cornerDefects = 0;
    int samples = 0;
    int components = 0;
    int holes = 0;
    double tangentEnergy = 0.0;
    double turnEnergy = 0.0;
    double maximumExcessTurn = 0.0;
    double maximumDistance = 0.0;
    double maximumCornerDistance = 0.0;
    double cornerEnergy = 0.0;
};

struct BoundaryReference {
    QPointF point;
    QPointF tangent;
    bool corner = false;
    double distance = 0.0;
};

class BoundaryModel {
public:
    struct ObservationWindow {
        catalog::Polygons unchanged;
        catalog::Polygons neighborhood;
        catalog::Polygons clip;
        QRectF additionBounds;
    };

    explicit BoundaryModel(const catalog::Polygons &target, double observationScale);
    BoundaryReference reference(const QPointF &point) const;
    catalog::Polygons observationSupport(const catalog::Polygons &coverage) const;
    ObservationWindow observationWindow(const catalog::Polygons &unchanged,
                                         const catalog::Polygons &unchangedObserved,
                                         const QRectF &addedBounds) const;
    catalog::Polygons observationSupport(const catalog::Polygons &addition, const ObservationWindow &window) const;
    BoundaryMetrics measure(const catalog::Polygons &coverage) const;
    BoundaryMetrics measure(const catalog::Polygons &coverage, const catalog::Polygons &observed) const;
    double energy(const BoundaryMetrics &metrics) const;
    QJsonObject diagnostics(const BoundaryMetrics &metrics) const;
    QJsonObject performance() const;
    double perimeter() const;
    const QVector<QPointF> &protectedCorners() const;

private:
    struct Loop {
        QPolygonF points;
        QVector<double> lengths;
        double perimeter = 0.0;
    };
    struct Edge {
        QPointF start;
        QPointF delta;
        int loop = 0;
        double squaredLength = 0.0;
        double offset = 0.0;
    };
    struct Location {
        QPointF point;
        int loop = 0;
        double offset = 0.0;
        double squaredDistance = std::numeric_limits<double>::infinity();
    };

    static Loop makeLoop(const QPolygonF &points);
    static QPointF pointAt(const Loop &loop, double offset);
    Location closest(const QPointF &point) const;
    bool nearCorner(const QPointF &point, double radius) const;
    qint64 cellKey(int x, int y) const;

    QVector<Loop> loops_;
    QVector<Edge> edges_;
    QVector<QPointF> corners_;
    QHash<qint64, QVector<int>> cells_;
    QHash<qint64, QVector<QPointF>> cornerCells_;
    QRectF bounds_;
    mutable qint64 cornerNanoseconds_ = 0;
    mutable qint64 closingNanoseconds_ = 0;
    mutable qint64 samplingNanoseconds_ = 0;
    mutable int measurements_ = 0;
    double scale_ = 1.0;
    double cellSize_ = 1.0;
};

} // namespace gui::compact
