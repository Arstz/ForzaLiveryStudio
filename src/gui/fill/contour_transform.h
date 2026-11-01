#pragma once

#include "contour_data.h"

#include <QTransform>

namespace gui {

fls::scene::ContourData transformedContourData(fls::scene::ContourData data, const QTransform &transform);

} // namespace gui
