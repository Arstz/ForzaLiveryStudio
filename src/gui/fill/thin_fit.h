#pragma once

#include "compact_fit.h"

namespace gui::thin {

inline constexpr double kDefaultBoundaryAllowance = 2.0;
inline constexpr int kDefaultProfileTrialBudget = 320000;
inline constexpr double kDefaultMinimumCoverage = 0.98;
inline constexpr double kDefaultMaximumThicknessRatio = 1.6;
inline constexpr double kDefaultPreferredThicknessRatio = 1.15;

struct FillOptions {
    std::function<void(int, int, int)> workProgress;
    QVector<QPolygonF> leeway;
    int shapeBudget = compact::kDefaultShapeBudget;
    int profileTrialBudget = kDefaultProfileTrialBudget;
    bool useGpu = true;
    double boundaryAllowance = kDefaultBoundaryAllowance;
    double minimumCoverage = kDefaultMinimumCoverage;
    double maximumThicknessRatio = kDefaultMaximumThicknessRatio;
    double preferredThicknessRatio = kDefaultPreferredThicknessRatio;
};

QVector<catalog::Primitive> buildCatalog(const ShapeGeometryStore &geometry, QString *error = nullptr);
catalog::FillResult fillRegion(const PenFillRequest &request,
                               const QVector<catalog::Primitive> &primitives,
                               const FillOptions &options = {},
                               const std::function<bool()> &cancelled = {});
catalog::FillResult fillPolygons(const QVector<QPolygonF> &polygons,
                                const QVector<catalog::Primitive> &primitives,
                                const FillOptions &options = {},
                                const std::function<bool()> &cancelled = {});
catalog::FillResult fillLiningPath(const QVector<PenPoint> &points, double width,
                                   const QVector<catalog::Primitive> &primitives,
                                   const FillOptions &options = {},
                                   const std::function<bool()> &cancelled = {});

} // namespace gui::thin
