#pragma once

#include "compact_fit_catalog_internal.h"

namespace gui::thin {

QVector<QPolygonF> medialSpans(const catalog::Region &region, double width,
                              const std::function<bool()> &cancelled = {});

} // namespace gui::thin
