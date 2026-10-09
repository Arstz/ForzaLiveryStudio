#pragma once

#include "pen_fill.h"

namespace gui {

int estimateRegionShapeCount(const QPainterPath &outline);
PenFillResult fitSingleRegionPrimitive(const QPainterPath &outline,
                                      const QVector<PenPrimitive> &primitives,
                                      double boundaryTolerance = 0.0);

} // namespace gui
