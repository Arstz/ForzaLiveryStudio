#pragma once

#include "compact_fit_catalog_internal.h"
#include <limits>
#include <memory>

namespace gui::compact {

inline constexpr double kMaximumBoundaryTurn = 0.6;

struct BoundaryMetrics {
    QVector<double> cornerDistances;
    QVector<QPointF> cornerOffsets;
    int cornerDefects = 0;
    int samples = 0;
    int components = 0;
    int holes = 0;
    int matchedCorners = 0;
    int unmatchedCorners = 0;
    double tangentEnergy = 0.0;
    double turnEnergy = 0.0;
    double maximumExcessTurn = 0.0;
    double maximumDistance = 0.0;
    double maximumCornerDistance = 0.0;
    double maximumCornerAngleError = 0.0;
    double cornerEnergy = 0.0;
};

struct BoundaryReference {
    QPointF point;
    QPointF tangent;
    bool corner = false;
    double distance = 0.0;
};

struct BoundaryDefect {
    QPointF point;
    double energy = 0.0;
    double excessTurn = 0.0;
};

struct BoundaryExclusion {
    QPointF point;
    double radius = 0.0;
};

class BoundaryModel;

struct BoundaryOcclusion {
    std::shared_ptr<const catalog::PointContainment> interior;
    std::shared_ptr<const BoundaryModel> boundary;

    bool contains(const QPointF &point, double radius) const;
};

std::shared_ptr<const BoundaryOcclusion> boundaryOcclusion(const catalog::Polygons &polygons, double observationScale);

class BoundaryModel {
public:
    struct ObservationWindow {
        catalog::Polygons unchanged;
        catalog::Polygons neighborhood;
        catalog::Polygons clip;
        QRectF additionBounds;
    };

    explicit BoundaryModel(const catalog::Polygons &target, double observationScale,
                            const QVector<BoundaryExclusion> &exclusions = {}, bool vertexSamples = false,
                            const QVector<catalog::BoundaryCorner> &authoredCorners = {},
                            const std::shared_ptr<const BoundaryOcclusion> &occlusion = {});
    BoundaryReference reference(const QPointF &point) const;
    bool withinDistance(const QPointF &point, double radius) const;
    catalog::Polygons observationSupport(const catalog::Polygons &coverage) const;
    ObservationWindow observationWindow(const catalog::Polygons &unchanged,
                                         const catalog::Polygons &unchangedObserved,
                                         const QRectF &addedBounds) const;
    catalog::Polygons observationSupport(const catalog::Polygons &addition, const ObservationWindow &window) const;
    BoundaryMetrics measure(const catalog::Polygons &coverage) const;
    BoundaryMetrics measure(const catalog::Polygons &coverage, const catalog::Polygons &observed) const;
    BoundaryMetrics measure(const catalog::Polygons &coverage, const catalog::Polygons &observed,
                              const QRectF &window, QVector<BoundaryDefect> *defects = nullptr) const;
    double energy(const BoundaryMetrics &metrics) const;
    QJsonObject diagnostics(const BoundaryMetrics &metrics) const;
    QJsonObject performance() const;
    double perimeter() const;
    const QVector<QPointF> &protectedCorners() const;

private:
    struct Loop {
        QPolygonF points;
        QVector<double> lengths;
        QVector<int> bins;
        double perimeter = 0.0;
    };
    struct Edge {
        QPointF start;
        QPointF delta;
        int loop = 0;
        double squaredLength = 0.0;
        double length = 0.0;
        double offset = 0.0;
    };
    struct Location {
        QPointF point;
        int loop = 0;
        double offset = 0.0;
        double squaredDistance = std::numeric_limits<double>::infinity();
    };
    struct Sample {
        double offset = 0.0;
        double spacing = 0.0;
    };
    struct Corner {
        QPointF point;
        QPointF incoming;
        QPointF outgoing;
        int loop = 0;
        double offset = 0.0;
    };

    static Loop makeLoop(const QPolygonF &points);
    static QPointF pointAt(const Loop &loop, double offset);
    static double supportedTurn(const Loop &loop, double offset, double radius);
    static QVector<Sample> loopSamples(const Loop &loop, double step, bool vertices);
    Location closest(const QPointF &point, int loop = -1, double spanStart = 0,
        double spanLength = std::numeric_limits<double>::infinity()) const;
    int correspondingLoop(const Loop &loop) const;
    bool nearCorner(const QPointF &point, double radius) const;
    bool visibleCorner(const Loop &loop, double offset) const;
    bool excluded(const QPointF &point) const;

    QVector<Loop> loops_;
    QVector<Edge> edges_;
    QVector<QPointF> corners_;
    QVector<Corner> cornerFeatures_;
    QVector<BoundaryExclusion> exclusions_;
    std::shared_ptr<const BoundaryOcclusion> occlusion_;
    QVector<QVector<int>> cells_;
    QVector<QVector<QPointF>> cornerCells_;
    catalog::Polygons target_;
    mutable std::shared_ptr<const BoundaryModel> observedReference_;
    QRectF bounds_;
    mutable qint64 cornerNanoseconds_ = 0;
    mutable qint64 closingNanoseconds_ = 0;
    mutable qint64 samplingNanoseconds_ = 0;
    mutable int measurements_ = 0;
    int columns_ = 0;
    int rows_ = 0;
    bool vertexSamples_ = false;
    double scale_ = 1.0;
    double cellSize_ = 1.0;
};

} // namespace gui::compact
