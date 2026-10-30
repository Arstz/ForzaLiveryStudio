#pragma once

#include "compact_fit.h"

namespace gui::catalog {
struct Region;
}

namespace gui::compact {

catalog::FillResult constructiveCover(const catalog::Region &region,
    const QVector<catalog::Primitive> &primitives, const FillOptions &options,
    const std::function<bool()> &cancelled = {});

} // namespace gui::compact
