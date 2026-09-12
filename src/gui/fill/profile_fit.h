#pragma once

#include "compact_fit.h"

namespace gui::profile {

catalog::FillResult fillRegion(const PenFillRequest &request,
                               const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options = {},
                               const std::function<bool()> &cancelled = {},
                               const std::function<void(int, double, double)> &progress = {});

} // namespace gui::profile
