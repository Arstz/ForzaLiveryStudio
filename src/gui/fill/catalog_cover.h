#pragma once

#include "pen_fill.h"

namespace gui::catalog {

inline constexpr int kDefaultShapeBudget = 3000;
inline constexpr int kDefaultCandidateLimit = 256;
inline constexpr int kDefaultSearchNodes = 12000;
inline constexpr int kDefaultRefinementSteps = 10;

struct Primitive {
    PenPrimitive shape;
    bool reserve = false;
};

struct FillOptions {
    int shapeBudget = kDefaultShapeBudget;
    int candidateLimit = kDefaultCandidateLimit;
    int searchNodes = kDefaultSearchNodes;
    int refinementSteps = kDefaultRefinementSteps;
    bool retainFailedFill = false;
};

struct FillResult {
    PenFillResult fill;
    QJsonObject diagnostics;
};

QVector<Primitive> buildCatalog(const ShapeGeometryStore &geometry,
                               QString *error = nullptr);

FillResult fillRegion(
    const PenFillRequest &request,
    const QVector<Primitive> &primitives,
    const FillOptions &options = {},
    const std::function<bool()> &cancelled = {},
    const std::function<void(int, double, double)> &progress = {});

} // namespace gui::catalog
