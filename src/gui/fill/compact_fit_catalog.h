#pragma once

#include "pen_fill.h"
#include <array>
#include <memory>

namespace gui::catalog {

inline constexpr int kDefaultShapeBudget = 3000;
inline constexpr int kDefaultCandidateLimit = 256;
inline constexpr int kDefaultSearchNodes = 12000;
inline constexpr int kDefaultRefinementSteps = 10;

enum class ShapeTask {
    Curves,
    Interior,
    Corners,
    StraightEdges,
    CornerTriangles,
    Replacements,
    CutoutReplacements,
    GroupReplacements,
    WholeRegion,
    GapPatches,
    ResidualRepair,
    PatchConsolidation,
    ExactReplacements,
    Count
};

inline constexpr int kShapeTaskCount = static_cast<int>(ShapeTask::Count);

struct Primitive {
    PenPrimitive shape;
    std::shared_ptr<const QJsonObject> configuration;
    std::array<int, kShapeTaskCount> taskOrder{};
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
QVector<Primitive> buildCatalog(const ShapeGeometryStore &geometry,
                               const QString &configurationPath, QString *error);
bool usesTask(const Primitive &primitive, ShapeTask task);
QVector<int> shapeIdsForTask(const QVector<Primitive> &primitives, ShapeTask task);
QVector<Primitive> primitivesForTask(const QVector<Primitive> &primitives, ShapeTask task);

FillResult fillRegion(
    const PenFillRequest &request,
    const QVector<Primitive> &primitives,
    const FillOptions &options = {},
    const std::function<bool()> &cancelled = {},
    const std::function<void(int, double, double)> &progress = {});

} // namespace gui::catalog
