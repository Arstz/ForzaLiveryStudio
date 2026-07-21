#pragma once

#include "region_extract.h"

#include <QtGui>

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
    int nearbyConflictRejectCount = 0;
    int conflictIsolatedSourceCount = 0;
    int absorbedRegionCount = 0;
    int orderingEdgeCount = 0;
    int validationMismatchPixels = 0;
    bool fallback = false;
};

RegionLayerPlan buildRegionLayerPlan(const RegionExtractionResult &regions);

} // namespace gui
