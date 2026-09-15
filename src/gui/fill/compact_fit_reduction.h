#pragma once

#include "compact_fit_quality.h"

namespace gui::compact {

struct ReductionState {
    catalog::Polygons coverage;
    catalog::Polygons deepMissing;
    catalog::Polygons observedHoles;
    QVector<double> cornerDistances;
    BoundaryMetrics metrics;
    double missingArea = 0.0;
    double spillArea = 0.0;
};

ReductionState reductionState(const catalog::Polygons &coverage, const catalog::Polygons &target,
                                const BoundaryModel &boundary, double inwardAllowance);
bool nonWorseningReduction(const ReductionState &after, const ReductionState &before,
                            const BoundaryMetrics &target);

} // namespace gui::compact
