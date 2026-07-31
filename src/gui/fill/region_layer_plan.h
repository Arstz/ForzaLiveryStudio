#pragma once

#include "region_extract.h"

#include <QtGui>

#include <functional>

namespace gui {

struct RegionLayerUnit {
    QColor color;
    QPainterPath outline;
    QVector<int> sourceRegionIndices;
    QVector<int> absorbedRegionIndices;
    double area = 0.0;
};

struct RegionLayerPlan {
    QVector<RegionLayerUnit> units;
    QStringList diagnostics;
    QString fallbackReason;
    int inputRegionCount = 0;
    int sameColorMergeCount = 0;
    int nearbySameColorMergeCount = 0;
    int edgeComponentMergeCount = 0;
    int hierarchicalContourMergeCount = 0;
    int nearbyConflictRejectCount = 0;
    int nearbyForeignOwnerRejectCount = 0;
    int adjacentConflictRejectCount = 0;
    int containmentConflictRejectCount = 0;
    int absorptionConflictRejectCount = 0;
    int suppressedOperationCount = 0;
    int dependencyCycleRebuildCount = 0;
    int conflictIsolatedSourceCount = 0;
    int absorbedRegionCount = 0;
    int largeContainedAbsorptionCount = 0;
    int morphologicalClosingCount = 0;
    int convexSimplificationCount = 0;
    int geometryPointReductionCount = 0;
    int dangerousCycleBreakCount = 0;
    int orderingEdgeCount = 0;
    int validationMismatchPixels = 0;
    bool fallback = false;
    bool cancelled = false;
};

struct RegionLayerPlanVariants {
    RegionLayerPlan safe;
    RegionLayerPlan dangerous;
};

using RegionLayerPlanProgress = std::function<void(
    const QString &phase, int completed, int total)>;

RegionLayerPlanVariants buildRegionLayerPlanVariants(
    const RegionExtractionResult &regions,
    const RegionLayerPlanProgress &progress = {},
    const std::function<bool()> &cancelled = {});
RegionLayerPlan buildRegionLayerPlan(const RegionExtractionResult &regions);

} // namespace gui
