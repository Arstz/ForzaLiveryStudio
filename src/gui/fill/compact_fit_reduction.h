#pragma once

#include "compact_fit_quality.h"

namespace gui::compact {

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
