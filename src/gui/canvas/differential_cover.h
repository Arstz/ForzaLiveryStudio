#pragma once

#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <array>
#include <cstdint>
#include <functional>

namespace gui::cover {

inline constexpr int kDefaultBudget = 100000;
inline constexpr double kDefaultSpillWeight = 8.0;
inline constexpr double kDefaultEpsArea = 0.25;
inline constexpr double kDefaultEpsGain = 1.0;
inline constexpr double kDefaultEpsSpill = 0.25;
inline constexpr int kDefaultAdamIterations = 200;
inline constexpr double kDefaultAdamLearningRate = 0.05;
inline constexpr int kDefaultRestarts = 2;
inline constexpr double kDefaultInactivityTimeoutSeconds = 60.0;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct ShapeMesh {
    QVector<Vec2> vertices;
    QVector<std::array<int, 3>> triangles;
    QVector<Vec2> boundary;
    QString error;
    int id = 0;
    double area = 0.0;
    bool convex = false;

    bool valid() const {
        return error.isEmpty() && id > 0 && vertices.size() >= 3
            && !triangles.isEmpty() && boundary.size() >= 3 && area > 0.0;
    }
};

struct Affine {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;
};

struct Placement {
    Affine transform;
    int shapeId = 0;
    double coveredArea = 0.0;
};

using Polygons = QVector<QPolygonF>;

struct ContourSpan {
    QPointF start;
    QPointF control;
    QPointF end;
    bool curved = false;
};

struct FillInput {
    Polygons mustCover;
    Polygons mayCover;
    QVector<ContourSpan> boundarySpans;
    QImage mask;
    QRectF maskBounds;
};

struct FillOptions {
    int budget = kDefaultBudget;
    int adamIterations = kDefaultAdamIterations;
    int restarts = kDefaultRestarts;
    double spillWeight = kDefaultSpillWeight;
    double epsArea = kDefaultEpsArea;
    double epsGain = kDefaultEpsGain;
    double epsSpill = kDefaultEpsSpill;
    double adamLearningRate = kDefaultAdamLearningRate;
    double inactivityTimeoutSeconds =
        kDefaultInactivityTimeoutSeconds;
    std::uint64_t seed = 0;
    bool useRouter = true;
    bool useGpu = true;
};

struct FillProfile {
    double totalWallSeconds = 0.0;
    double greedySetupWallSeconds = 0.0;
    double candidateBatchWallSeconds = 0.0;
    double candidateWorkerSeconds = 0.0;
    double adamEvaluationWorkerSeconds = 0.0;
    double legalizationWorkerSeconds = 0.0;
    double residualUpdateWallSeconds = 0.0;
    double finalMeasurementWallSeconds = 0.0;
    double gpuEvaluationWallSeconds = 0.0;
    double pruneWallSeconds = 0.0;
    double repairWallSeconds = 0.0;
    double prePruneResidualArea = 0.0;
    double postPruneResidualArea = 0.0;
    double repairTargetArea = 0.0;
    double postRepairNewGapArea = 0.0;
    double repairCoveredArea = 0.0;
    QString evaluationBackend;
    QString gpuAdapter;
    QString gpuError;
    std::uint64_t candidateJobs = 0;
    std::uint64_t adamEvaluations = 0;
    std::uint64_t legalizationEvaluations = 0;
    std::uint64_t gpuBatches = 0;
    std::uint64_t gpuIntersectionTasks = 0;
    std::uint64_t wholeComponentJobs = 0;
    std::uint64_t hardEdgeCandidates = 0;
    std::uint64_t pruneAttempts = 0;
    std::uint64_t pruneOptimizations = 0;
    int greedySteps = 0;
    int complexitySelections = 0;
    int localComponentPlacements = 0;
    int wholeComponentPlacements = 0;
    int hardEdgePlacements = 0;
    int prunedPlacements = 0;
    int adjustedPlacements = 0;
    int prunePasses = 0;
    int repairSteps = 0;
    int repairPlacements = 0;
    int workerThreads = 0;
};

struct FillResult {
    QVector<Placement> placements;
    Polygons residual;
    QString error;
    FillProfile profile;
    double residualArea = 0.0;
    double coveredArea = 0.0;
    double outsideArea = 0.0;
    bool budgetHit = false;
    bool stalled = false;
    bool cancelled = false;
    bool timedOut = false;
};

struct FillProgress {
    int placementCount = 0;
    double targetArea = 0.0;
    double coveredArea = 0.0;
    double residualArea = 0.0;
    double elapsedSeconds = 0.0;
};

struct AreaGradient {
    std::array<double, 6> coveredGradient{};
    std::array<double, 6> spillGradient{};
    double covered = 0.0;
    double spill = 0.0;
};

QVector<ShapeMesh> buildShapeCatalog(const ShapeGeometryStore &geometry,
                                     QString *error = nullptr);

Polygons polygonsFromPainterPath(const QPainterPath &path);

QPainterPath painterPathFromPolygons(const Polygons &polygons);

AreaGradient evaluateAreaGradient(const ShapeMesh &shape,
                                  const Affine &transform,
                                  const Polygons &coveredSubject,
                                  const Polygons &legalSubject);

FillResult analyticCoverFill(
    const FillInput &input,
    const QVector<ShapeMesh> &catalog,
    const FillOptions &options = {},
    const std::function<bool()> &cancelled = {},
    const std::function<void(const FillProgress &)> &progress = {});

QTransform toQTransform(const Affine &transform);

#ifdef FLS_DIFFERENTIAL_COVER_TESTS
double placementUnionAreaForTesting(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog);
#endif

} // namespace gui::cover
