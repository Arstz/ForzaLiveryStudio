#pragma once

#include "pen_fill.h"

namespace gui {

QVector<PenPrimitive> buildCurvePrimitiveCatalog(
    const ShapeGeometryStore &geometry,
    QString *error = nullptr);

} // namespace gui
