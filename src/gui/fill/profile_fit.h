#pragma once

#include "compact_fit.h"

namespace gui::catalog { struct Region; }

namespace gui::profile {

struct TraceCandidate {
    PenPlacement placement;
    QPolygonF profile;
};

QVector<TraceCandidate> traceCandidates(const QPainterPath &centerline,
                                       const QVector<catalog::Primitive> &primitives,
                                       double width, double maximumError, bool useGpu, int *remainingTrials,
                                       const std::function<bool()> &cancelled = {});

catalog::FillResult seedRegion(const catalog::Region &region,
                               const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options,
                               const std::function<bool()> &cancelled = {},
                               QVector<compact::ReusableCandidate> *candidates = nullptr,
                               const std::function<void(const QString &)> &phaseProgress = {});

catalog::FillResult fillRegion(const PenFillRequest &request,
                               const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options = {},
                               const std::function<bool()> &cancelled = {},
                               const std::function<void(int, double, double)> &progress = {});

} // namespace gui::profile
