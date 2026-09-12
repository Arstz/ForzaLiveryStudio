#pragma once

#include "catalog_cover.h"

namespace gui::compact {

inline constexpr double kDefaultBoundaryAllowance = 2.0;
inline constexpr double kDefaultAreaErrorRatio = 0.02;
inline constexpr int kDefaultShapeBudget = 3000;
inline constexpr int kDefaultEvaluationBudget = 60000;
inline constexpr double kDefaultInwardAllowance = 0.5;
inline constexpr double kDefaultObservationScale = 1.0;

struct FillOptions {
    QVector<PenPlacement> initialPlacements;
    std::function<void(int, int, int)> workProgress;
    int shapeBudget = kDefaultShapeBudget;
    int evaluationBudget = kDefaultEvaluationBudget;
    double boundaryAllowance = kDefaultBoundaryAllowance;
    double areaErrorRatio = kDefaultAreaErrorRatio;
    double inwardAllowance = kDefaultInwardAllowance;
    double observationScale = kDefaultObservationScale;
};

catalog::FillResult fillRegion(const PenFillRequest &request,
                               const QVector<catalog::Primitive> &primitives,
                               const FillOptions &options = {},
                               const std::function<bool()> &cancelled = {},
                               const std::function<void(int, double, double)> &progress = {});

} // namespace gui::compact
