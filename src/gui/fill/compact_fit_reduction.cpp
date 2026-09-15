#include "compact_fit_reduction.h"
#include <algorithm>
#include <cmath>

namespace gui::compact {
namespace {

constexpr double kComparisonSlack = 1e-7;

bool noGreater(double after, double before) {
    return std::isfinite(after) && after <= before + kComparisonSlack;
}

} // namespace

ReductionState reductionState(const catalog::Polygons &coverage, const catalog::Polygons &target,
                                const BoundaryModel &boundary, double inwardAllowance) {
    const auto observed = boundary.observationSupport(coverage);
    const BoundaryModel output(coverage, inwardAllowance);
    ReductionState result;
    result.coverage = coverage;
    result.deepMissing = catalog::subtract(target, catalog::expanded(coverage, inwardAllowance));
    result.metrics = boundary.measure(coverage, observed);
    for (auto polygon : observed) {
        if (catalog::signedArea(polygon) < 0) {
            std::reverse(polygon.begin(), polygon.end());
            result.observedHoles.push_back(std::move(polygon));
        }
    }
    result.observedHoles = catalog::intersect(catalog::unite(result.observedHoles), target);
    for (const auto &corner : boundary.protectedCorners()) {
        result.cornerDistances.push_back(output.reference(corner).distance);
    }
    result.missingArea = catalog::area(catalog::subtract(target, coverage));
    result.spillArea = catalog::area(catalog::subtract(coverage, target));

    return result;
}

bool nonWorseningReduction(const ReductionState &after, const ReductionState &before,
                            const BoundaryMetrics &target) {
    const auto &next = after.metrics;
    const auto &previous = before.metrics;
    if (after.coverage.isEmpty() || !noGreater(after.missingArea, before.missingArea)
        || !noGreater(after.spillArea, before.spillArea)
        || !noGreater(next.tangentEnergy, previous.tangentEnergy)
        || !noGreater(next.turnEnergy, previous.turnEnergy)
        || !noGreater(next.cornerEnergy, previous.cornerEnergy)
        || !noGreater(next.maximumDistance, previous.maximumDistance)
        || !noGreater(next.maximumExcessTurn, previous.maximumExcessTurn)
        || !noGreater(next.maximumCornerDistance, previous.maximumCornerDistance)
        || next.cornerDefects > previous.cornerDefects
        || std::abs(next.components - target.components) > std::abs(previous.components - target.components)
        || std::abs(next.holes - target.holes) > std::abs(previous.holes - target.holes)) {
        return false;
    }

    if (after.cornerDistances.size() != before.cornerDistances.size()) {
        return false;
    }
    for (int index = 0; index < after.cornerDistances.size(); ++index) {
        if (!noGreater(after.cornerDistances[index], before.cornerDistances[index])) {
            return false;
        }
    }

    return catalog::area(catalog::subtract(after.deepMissing, before.deepMissing)) <= kComparisonSlack
        && catalog::area(catalog::subtract(after.observedHoles, before.observedHoles)) <= kComparisonSlack;
}

} // namespace gui::compact
