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

ReductionState reductionState(const catalog::Polygons &coverage,
                              const catalog::Polygons &required,
                              const catalog::Polygons &visibleTarget,
                              const catalog::Polygons &leeway,
                              const BoundaryModel &boundary,
                              double inwardAllowance) {
    const auto visibleCoverage = leeway.isEmpty()
        ? coverage : catalog::subtract(coverage, leeway);
    const auto observed = boundary.observationSupport(visibleCoverage);
    ReductionState result;
    result.coverage = coverage;
    result.deepMissing = catalog::subtract(required, catalog::expanded(coverage, inwardAllowance));
    result.metrics = boundary.measure(visibleCoverage, observed);
    for (auto polygon : observed) {
        if (catalog::signedArea(polygon) < 0) {
            std::reverse(polygon.begin(), polygon.end());
            result.observedHoles.push_back(std::move(polygon));
        }
    }
    result.observedHoles = catalog::intersect(catalog::unite(result.observedHoles), visibleTarget);
    catalog::Polygons intendedHoles;
    for (auto polygon : visibleTarget) {
        if (catalog::signedArea(polygon) < 0) {
            std::reverse(polygon.begin(), polygon.end());
            intendedHoles.push_back(std::move(polygon));
        }
    }
    if (!intendedHoles.isEmpty()) {
        result.observedHoles = catalog::subtract(result.observedHoles,
            catalog::expanded(catalog::unite(intendedHoles), inwardAllowance));
    }
    result.cornerDistances = result.metrics.cornerDistances;
    result.missingArea = catalog::area(catalog::subtract(visibleTarget, visibleCoverage));
    result.spillArea = catalog::area(catalog::subtract(visibleCoverage, visibleTarget));

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

bool preservesCoverage(const ReductionState &after, const ReductionState &before,
                        const BoundaryMetrics &target, double cornerAllowance, bool growing) {
    if (after.coverage.isEmpty() || after.cornerDistances.size() != before.cornerDistances.size()
        || (!growing && std::abs(after.metrics.components - target.components) > std::abs(before.metrics.components - target.components))
        || catalog::area(catalog::subtract(after.deepMissing, before.deepMissing)) > kComparisonSlack
        || catalog::area(catalog::subtract(after.observedHoles, before.observedHoles)) > kComparisonSlack) {
        return false;
    }
    for (int index = 0; index < after.cornerDistances.size(); ++index) {
        if (!noGreater(after.cornerDistances[index], std::max(cornerAllowance, before.cornerDistances[index]))) {
            return false;
        }
    }

    return true;
}

} // namespace gui::compact
