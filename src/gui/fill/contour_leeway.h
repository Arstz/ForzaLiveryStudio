#pragma once

#include "layer.h"
#include "shape_geometry_store.h"

#include <QPainterPath>

namespace gui {

QPainterPath buildContourLeeway(const fls::scene::Layer &node, const ShapeGeometryStore &geometry,
                                const QString &excludedContourId = {}, QString *error = nullptr);

} // namespace gui
