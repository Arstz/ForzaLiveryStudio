#pragma once

#include "compact_fit.h"

namespace gui::catalog { struct Region; }

namespace gui::profile {

QVector<PenPlacement> spanCandidates(const QPainterPath &centerline, double width,
                                      const QVector<catalog::Primitive> &primitives,
                                      double maximumError, bool useGpu,
                                      const std::function<bool()> &cancelled = {});

catalog::FillResult seedRegion(const catalog::Region &region,
                               const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options,
                               const std::function<bool()> &cancelled = {},
                               QVector<compact::ReusableCandidate> *candidates = nullptr);

catalog::FillResult fillRegion(const PenFillRequest &request,
                               const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options = {},
                               const std::function<bool()> &cancelled = {},
                               const std::function<void(int, double, double)> &progress = {});

} // namespace gui::profile
