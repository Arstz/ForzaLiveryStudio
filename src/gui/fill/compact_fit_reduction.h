#pragma once

#include "compact_fit_quality.h"
#include "compact_fit.h"
#include <QSet>
#include <optional>

namespace gui::compact {

class CoverageOwnership {
public:
    void synchronize(const QVector<catalog::Polygons> &pieces);
    catalog::Polygons exclusive(const QVector<int> &members) const;
    void erase(int index);
    bool failed(const QVector<int> &members, bool broad) const;
    void rememberFailure(const QVector<int> &members, bool broad);
    QJsonObject diagnostics() const;

private:
    static QString groupKey(const QVector<int> &members, bool broad);

    QVector<catalog::Polygons> pieces_;
    QVector<QRectF> bounds_;
    QVector<QVector<int>> neighbors_;
    mutable QVector<std::optional<catalog::Polygons>> exclusive_;
    QSet<QString> failures_;
    mutable int computations_ = 0;
    mutable int hits_ = 0;
    mutable int failureHits_ = 0;
    int rebuilds_ = 0;
    int invalidations_ = 0;
};

struct ReductionState {
    catalog::Polygons coverage;
    catalog::Polygons observed;
    catalog::Polygons deepMissing;
    catalog::Polygons observedHoles;
    QVector<double> cornerDistances;
    BoundaryMetrics metrics;
    double missingArea = 0.0;
    double spillArea = 0.0;
};

struct ExactReductionResult {
    QVector<PenPlacement> placements;
    QJsonObject diagnostics;
};

ExactReductionResult reduceExactCoverage(const QVector<PenPlacement> &placements,
    const QVector<catalog::Primitive> &primitives, const QVector<ReusableCandidate> &candidates,
    const std::function<bool()> &cancelled = {});

ReductionState reductionState(const catalog::Polygons &coverage,
                              const catalog::Polygons &required,
                              const catalog::Polygons &visibleTarget,
                              const catalog::Polygons &leeway,
                              const BoundaryModel &boundary,
                              double inwardAllowance);
catalog::Polygons repairResidual(const ReductionState &state, const catalog::Polygons &target,
                                 const catalog::Polygons &interior, const BoundaryMetrics &targetMetrics);
bool nonWorseningReduction(const ReductionState &after, const ReductionState &before,
                            const BoundaryMetrics &target);
bool preservesCoverage(const ReductionState &after, const ReductionState &before,
                        const BoundaryMetrics &target, double cornerAllowance, bool growing = false);

} // namespace gui::compact
