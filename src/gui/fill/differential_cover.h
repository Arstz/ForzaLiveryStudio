#pragma once

#include "fill_contour.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

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
inline constexpr double kDefaultBoundaryTolerance = 0.1;
inline constexpr double kDefaultAreaWindowRatio = 0.875;
inline constexpr double kDefaultTverskyAlpha = 0.35;
inline constexpr double kDefaultTverskyBeta = 1.0;
inline constexpr double kDefaultFeatureWeight = 1.0;
inline constexpr int kDefaultFeatureRestarts = 12;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

enum class ContourFeatureKind {
    Corner,
    SmoothJunction,
};

struct ContourFeature {
    QPointF position;
    QPointF incomingTangent;
    QPointF outgoingTangent;
    double captureRadius = 0.0;
    double weight = 0.0;
    int id = 0;
    ContourFeatureKind kind = ContourFeatureKind::Corner;
};

struct ShapeFeature {
    Vec2 position;
    Vec2 incomingTangent;
    Vec2 outgoingTangent;
    double arcPosition = 0.0;
    int boundaryIndex = 0;
    ContourFeatureKind kind = ContourFeatureKind::Corner;
};

struct ShapeMesh {
    QVector<Vec2> vertices;
    QVector<std::array<int, 3>> triangles;
    QVector<Vec2> boundary;
    QVector<ShapeFeature> features;
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

struct CandidateAnchor {
    Vec2 source;
    QPointF target;
};

struct Placement {
    Affine transform;
    int shapeId = 0;
    double coveredArea = 0.0;
    QVector<int> ownedFeatureIds;
    double exposedContourArc = 0.0;
    std::optional<CandidateAnchor> anchor;
};

using Polygons = QVector<QPolygonF>;

using ContourSpan = FillBoundarySegment;

struct FillInput {
    Polygons mustCover;
    Polygons mayCover;
    QVector<ContourSpan> boundarySpans;
    QVector<QVector<ContourSpan>> boundaryLoops;
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
    double boundaryTolerance = kDefaultBoundaryTolerance;
    double areaWindowRatio = kDefaultAreaWindowRatio;
    double tverskyAlpha = kDefaultTverskyAlpha;
    double tverskyBeta = kDefaultTverskyBeta;
    double featureWeight = kDefaultFeatureWeight;
    int featureRestarts = kDefaultFeatureRestarts;
    std::uint64_t seed = 0;
    bool useRouter = true;
    bool useGpu = true;
    bool useWeightedContour = false;
};

struct CoverErrorMetrics {
    double missingArea = 0.0;
    double outsideTargetArea = 0.0;
    double outsideEnvelopeArea = 0.0;
    double tversky = 0.0;
    double meanBoundaryDistance = 0.0;
    double boundaryDistanceRms = 0.0;
    double boundaryDistance95 = 0.0;
    double maximumOutwardDistance = 0.0;
    double boundaryFScore = 0.0;
    double representedFeatureWeight = 0.0;
    double totalFeatureWeight = 0.0;
    double meanFeatureDistance = 0.0;
    double featureDistance95 = 0.0;
    double maximumFeatureDistance = 0.0;
    double meanTangentError = 0.0;
    double maximumTangentError = 0.0;
    int representedFeatures = 0;
    int totalFeatures = 0;
    int placementCount = 0;
};

struct FillProfile {
    QString structuralReason;
    QString meshReason;
    double areaWindowRatio = 0.0;
    double totalWallSeconds = 0.0;
    double greedySetupWallSeconds = 0.0;
    double candidateBatchWallSeconds = 0.0;
    double candidateWorkerSeconds = 0.0;
    double adamEvaluationWorkerSeconds = 0.0;
    double legalizationWorkerSeconds = 0.0;
    double residualUpdateWallSeconds = 0.0;
    double finalMeasurementWallSeconds = 0.0;
    double gpuEvaluationWallSeconds = 0.0;
    double nudgeWallSeconds = 0.0;
    double pruneWallSeconds = 0.0;
    double repairWallSeconds = 0.0;
    double continuityWallSeconds = 0.0;
    double preContinuityEnergy = 0.0;
    double postContinuityEnergy = 0.0;
    double preNudgeResidualArea = 0.0;
    double postNudgeResidualArea = 0.0;
    double prePruneResidualArea = 0.0;
    double postPruneResidualArea = 0.0;
    double repairTargetArea = 0.0;
    double postRepairNewGapArea = 0.0;
    double repairCoveredArea = 0.0;
    double structuralExplainedBoundaryFraction = 0.0;
    double structuralCoverageRatio = 0.0;
    double structuralResidualArea = 0.0;
    double structuralResidualThickness = 0.0;
    double structuralOutsideArea = 0.0;
    double meshCoverageRatio = 0.0;
    double meshResidualArea = 0.0;
    double meshOutsideArea = 0.0;
    double meshScale = 0.0;
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
    std::uint64_t featureCandidateJobs = 0;
    std::uint64_t featureCandidateRejections = 0;
    std::uint64_t selectionInsufficientGainRejections = 0;
    std::uint64_t selectionEnvelopeRejections = 0;
    std::uint64_t selectionOutwardDistanceRejections = 0;
    std::uint64_t selectionFeatureRejections = 0;
    std::uint64_t nudgeOptimizations = 0;
    std::uint64_t pruneAttempts = 0;
    std::uint64_t pruneOptimizations = 0;
    std::uint64_t continuityProposals = 0;
    int greedySteps = 0;
    int complexitySelections = 0;
    int localComponentPlacements = 0;
    int wholeComponentPlacements = 0;
    int hardEdgePlacements = 0;
    int featureSelectedPlacements = 0;
    int nudgedPlacements = 0;
    int prunedPlacements = 0;
    int adjustedPlacements = 0;
    int prunePasses = 0;
    int preContinuityKinks = 0;
    int postContinuityKinks = 0;
    int stabilizedPlacements = 0;
    int repairSteps = 0;
    int repairPlacements = 0;
    int structuralGridCells = 0;
    int structuralRectangleCandidates = 0;
    int structuralRectangles = 0;
    int meshPlacements = 0;
    int workerThreads = 0;
    bool structuralAccepted = false;
    bool structuralSeeded = false;
    bool meshAccepted = false;
};

struct FillResult {
    QVector<Placement> placements;
    Polygons residual;
    QString error;
    FillProfile profile;
    CoverErrorMetrics metrics;
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

QVector<ContourFeature> extractContourFeatures(
    const QVector<ContourSpan> &spans,
    double boundaryTolerance);

Polygons expandedCoverEnvelope(
    const Polygons &target,
    double distance);

CoverErrorMetrics evaluateCoverMetrics(
    const Polygons &target,
    const Polygons &legalEnvelope,
    const Polygons &coverage,
    const QVector<ContourFeature> &features,
    const FillOptions &options);

#ifdef FLS_DIFFERENTIAL_COVER_TESTS
double placementUnionAreaForTesting(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog);
#endif

} // namespace gui::cover
