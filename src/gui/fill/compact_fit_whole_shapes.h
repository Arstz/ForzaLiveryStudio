#pragma once

#include "compact_fit.h"

namespace gui::catalog { struct Region; }

namespace gui::compact {

QVector<ReusableCandidate> wholeShapeCandidates(const PenFillRequest &request,
    const catalog::Region &region, const QVector<catalog::Primitive> &primitives,
    const FillOptions &options, const std::function<bool()> &cancelled = {});

catalog::FillResult selectWholeShapeCover(const catalog::Region &region,
    const QVector<catalog::Primitive> &primitives, const FillOptions &options,
    const QVector<ReusableCandidate> &candidates, const std::function<bool()> &cancelled = {});

} // namespace gui::compact
