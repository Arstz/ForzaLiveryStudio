#pragma once

#include "image_generator.h"
#include "region_layer_plan.h"

#include <cstdint>
#include <vector>

namespace gui {

QPainterPath imageMaskContour(const std::vector<std::uint8_t> &mask, const QSize &size,
                              double smoothing);

void planImageRegionTopology(const QVector<int> &labels, const QSize &size,
                             int background, const ImageGeneratorOptions &options,
                             RegionLayerPlan *plan,
                             const ImageGeneratorProgress &progress = {},
                             const std::function<bool()> &cancelled = {});

} // namespace gui
