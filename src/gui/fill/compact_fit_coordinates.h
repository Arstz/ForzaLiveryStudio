#pragma once

#include "compact_fit.h"

namespace gui::catalog {
struct Region;
}

namespace gui::compact {

using CoordinateSolver = std::function<catalog::FillResult(const PenFillRequest &, const FillOptions &,
    const std::function<void(int, double, double)> &)>;

catalog::FillResult fitInObservationUnits(const PenFillRequest &request,
    const QVector<catalog::Primitive> &primitives, const FillOptions &options,
    const std::function<bool()> &cancelled, const std::function<void(int, double, double)> &progress,
    const CoordinateSolver &solver);

bool verifyEmittedFill(const PenFillRequest &request, const QVector<catalog::Primitive> &primitives,
    const FillOptions &options, catalog::FillResult *result);
bool verifyEmittedRegion(const catalog::Region &region, const QVector<catalog::Primitive> &primitives,
    const FillOptions &options, catalog::FillResult *result);

} // namespace gui::compact
