#pragma once

#include "compact_fit.h"

namespace gui::thin {

inline constexpr double kDefaultBoundaryAllowance = 2.0;
inline constexpr int kDefaultProfileTrialBudget = 320000;
inline constexpr int kDefaultQualityTimeBudgetMilliseconds = 120000;
inline constexpr double kDefaultMinimumCoverage = 0.98;
inline constexpr double kDefaultMaximumThicknessRatio = 1.6;
inline constexpr double kDefaultPreferredThicknessRatio = 1.15;
inline constexpr int kQualityProfileTrialBudget = 2000000;
inline constexpr int kQualityEvaluationBudget = 120000;
inline constexpr int kQualityTimeBudgetMilliseconds = 180000;
inline constexpr double kQualityMaximumThicknessRatio = 2.0;
inline constexpr double kQualityPreferredThicknessRatio = 1.6;

struct FillOptions {
    std::function<void(const QString &)> phaseProgress;
    std::function<void(int, int, int)> workProgress;
    QVector<QPolygonF> leeway;
    QVector<QPolygonF> protectedEmpty;
    QVector<QPolygonF> thicknessReference;
    int shapeBudget = compact::kDefaultShapeBudget;
    int profileTrialBudget = kDefaultProfileTrialBudget;
    int qualityEvaluationBudget = 0;
    int qualityTimeBudgetMilliseconds = kDefaultQualityTimeBudgetMilliseconds;
    bool useGpu = true;
    double boundaryAllowance = kDefaultBoundaryAllowance;
    double minimumCoverage = kDefaultMinimumCoverage;
    double maximumThicknessRatio = kDefaultMaximumThicknessRatio;
    double preferredThicknessRatio = kDefaultPreferredThicknessRatio;
};

FillOptions qualityOptions();
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
