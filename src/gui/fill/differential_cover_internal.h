#pragma once

#include "differential_cover_gpu.h"
#include "polygon_mesh.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <vector>

namespace gui::cover {

constexpr double kGeometryEpsilon = 1e-10;
constexpr double kClipperScale = 1000000.0;
constexpr int kGradientCount = 6;
constexpr int kRouterCandidateCount = 3;
constexpr int kMaximumDistanceDimension = 1024;
constexpr double kAdamBeta1 = 0.9;
constexpr double kAdamBeta2 = 0.999;
constexpr double kAdamEpsilon = 1e-8;
constexpr double kGradientNormLimit = 1000000.0;
constexpr double kGradientStopNorm = 1e-8;
constexpr double kRestartTranslationFraction = 0.15;
constexpr double kRestartAngleRange = 0.2;
constexpr double kRestartScaleRange = 0.12;
constexpr double kInitialRadiusFraction = 0.8;
constexpr double kMinimumAffineScale = 1e-6;
constexpr double kLegalShrinkFactor = 0.9;
constexpr int kLegalShrinkSteps = 64;
constexpr int kGpuLegalizationBatchSteps = 8;
constexpr int kGpuLegalizationInterval = 8;
constexpr double kHardBoundaryFraction = 0.6;
constexpr double kComplexityGainWindow = 1.0;
constexpr double kLocalRouterAdvantage = 0.75;
constexpr double kLocalSelectionGainAdvantage = 1.05;
constexpr double kLocalSelectionComplexityRatio = 0.95;
constexpr double kComponentComplexityWeight = 8.0;
constexpr double kHoleComplexityWeight = 16.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kMaximumShapeId = 65535;
constexpr double kStructuralDirectionSinTolerance = 0.0523359562429438;
constexpr double kStructuralMinimumAxisSeparationSin = 0.258819045102521;
constexpr double kStructuralMinimumExplainedBoundaryFraction = 0.9;
constexpr double kStructuralCoordinateToleranceFraction = 0.01;
constexpr double kStructuralMinimumCoordinateTolerance = 0.5;
constexpr double kStructuralMinimumCompactCoverageRatio = 0.98;
constexpr double kStructuralMinimumSeedCoverageRatio = 0.9;
constexpr double kStructuralMinimumLegalScale = 0.9;
constexpr double kStructuralLegalScaleStep = 0.0005;
constexpr int kStructuralMaximumGridCells = 63;
constexpr int kStructuralMaximumSupportLines = 12;
constexpr int kStructuralSearchNodeLimit = 100000;
constexpr double kMeshMinimumCompactCoverageRatio = 0.98;
constexpr double kMeshMinimumLegalScale = 0.9;
constexpr double kMeshLegalScaleStep = 0.0005;
constexpr int kMaximumHardPointJobs = 8;
constexpr double kMinimumHardTriangleQuality = 0.02;
constexpr double kCornerAngleTolerance = 0.13962634015954636;
constexpr double kShapeCornerSalience = 0.17453292519943295;
constexpr double kFeatureTangentSigma = 0.35;
constexpr double kMinimumFeatureArcFraction = 0.25;
constexpr int kMaximumBoundarySamples = 2048;
constexpr int kCatalogSmoothFeatureCount = 8;
constexpr double kEnvelopeDistanceEpsilon = 1e-5;
constexpr double kEnvelopeAreaEpsilon = 1.0 / kClipperScale;

struct Jet {
    std::array<double, kGradientCount> gradient{};
    double value = 0.0;
};

struct JetPoint {
    Jet x;
    Jet y;
};

struct DistanceSeed {
    QPointF point;
    double angle = 0.0;
    double radius = 0.0;
};

enum class CandidateOrigin {
    Greedy,
    LocalComponent,
    WholeComponent,
    HardEdge,
    Feature,
};

struct Candidate {
    Affine transform;
    int shapeId = 0;
    double covered = 0.0;
    double spill = 0.0;
    double featureReward = 0.0;
    CandidateOrigin origin = CandidateOrigin::Greedy;
    bool valid = false;
};

struct CandidateFeatureAssignment {
    ShapeFeature source;
    ContourFeature target;
};

struct CandidateInitialization {
    QPointF translationOffset;
    double angleOffset = 0.0;
    double scaleFactor = 1.0;
};

struct CandidateAnchor {
    Vec2 source;
    QPointF target;
};

struct CandidateJob {
    const ShapeMesh *shape = nullptr;
    QVector<CandidateFeatureAssignment> featureAssignments;
    CandidateInitialization initialization;
    Affine transform;
    std::optional<CandidateAnchor> anchor;
    CandidateOrigin origin = CandidateOrigin::Greedy;
    bool hasTransform = false;
};

struct FixedCandidate {
    const ShapeMesh *shape = nullptr;
    Affine transform;
};

struct CandidateProfile {
    qint64 totalNanoseconds = 0;
    qint64 adamEvaluationNanoseconds = 0;
    qint64 legalizationNanoseconds = 0;
    std::uint64_t adamEvaluations = 0;
    std::uint64_t legalizationEvaluations = 0;
};

struct CandidateJobResult {
    Candidate candidate;
    CandidateProfile profile;
};

struct GpuCandidateState {
    std::array<double, kGradientCount> values{};
    std::array<double, kGradientCount> firstMoment{};
    std::array<double, kGradientCount> secondMoment{};
    Candidate best;
    Candidate bestGpu;
    const CandidateJob *job = nullptr;
    double beta1Power = 1.0;
    double beta2Power = 1.0;
    bool active = true;
};

struct QueueNode {
    int index = 0;
    double distance = 0.0;

    bool operator>(const QueueNode &other) const {
        return distance > other.distance;
    }
};

struct IntersectionJets {
    Jet covered;
    Jet legal;
};

struct ResidualComplexity {
    double score = 0.0;
    int components = 0;
    int holes = 0;
};

struct CandidateSelection {
    Candidate candidate;
    Polygons coverage;
    Polygons residual;
    QVector<int> representedFeatureIds;
    QVector<int> newlyRepresentedFeatureIds;
    double exactGain = 0.0;
    double exposedContourArc = 0.0;
    int insufficientGainRejections = 0;
    int envelopeRejections = 0;
    int outwardDistanceRejections = 0;
    int featureRejections = 0;
    int featureCandidateRejections = 0;
    bool complexityPreferred = false;
    bool valid = false;
};

struct OrientedBounds {
    QPointF center;
    QPointF axisX;
    QPointF axisY;
    double extentX = 0.0;
    double extentY = 0.0;
    bool valid = false;
};

struct EvaluationBounds {
    QVector<QRectF> covered;
    QVector<QRectF> legal;
};

struct ExactCoverState {
    Polygons footprints;
    Polygons coverage;
    Polygons residual;
    double residualArea = 0.0;
    double coveredArea = 0.0;
    double outsideArea = 0.0;
};

struct FeatureMatchSummary {
    QVector<int> representedIds;
    QVector<double> distances;
    QVector<double> tangentErrors;
    double representedWeight = 0.0;
    double totalWeight = 0.0;
};

struct StructuralEdge {
    QPointF direction;
    double length = 0.0;
    double maximumBow = 0.0;
};

struct StructuralAxes {
    QPointF first;
    QPointF second;
    double determinant = 0.0;
    double explainedBoundaryFraction = 0.0;
    bool valid = false;
};

struct StructuralRectangle {
    quint64 mask = 0;
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

struct StructuralCoverPlan {
    QVector<Placement> placements;
    Polygons residual;
    QString reason;
    double explainedBoundaryFraction = 0.0;
    double coverageRatio = 0.0;
    double residualArea = 0.0;
    double residualThickness = 0.0;
    double outsideArea = 0.0;
    int gridCells = 0;
    int rectangleCandidates = 0;
    bool eligible = false;
    bool accepted = false;
    bool seeded = false;
    bool cancelled = false;
};

struct MeshCoverPlan {
    QVector<Placement> placements;
    Polygons residual;
    QString reason;
    double coverageRatio = 0.0;
    double residualArea = 0.0;
    double outsideArea = 0.0;
    double scale = 0.0;
    bool accepted = false;
    bool cancelled = false;
};

struct PruneCandidate {
    Affine transform;
    int index = 0;
    int shapeId = 0;
    double uniqueArea = 0.0;
};

struct PruneNeighbor {
    int index = 0;
    double overlapArea = 0.0;
    double distanceSquared = 0.0;
};

double signedArea(const QPolygonF &polygon);
double signedArea(const QVector<Vec2> &polygon);
double polygonSetArea(const Polygons &polygons);
QPolygonF normalizedPolygon(QPolygonF polygon);
QRectF polygonBounds(const Polygons &polygons);
QVector<QRectF> individualPolygonBounds(const Polygons &polygons);
AreaGradient areaGradient(const ShapeMesh &shape,
                          const Affine &transform,
                          const Polygons &coveredSubject,
                          const Polygons &legalSubject,
                          const EvaluationBounds &subjectBounds);
bool finiteGradient(const AreaGradient &evaluation);
QPolygonF transformedBoundary(const ShapeMesh &shape,
                              const Affine &transform);
Polygons differencePolygons(const Polygons &subjects,
                            const Polygons &clips);
Polygons intersectionPolygons(const Polygons &subjects,
                              const Polygons &clips);
Polygons unionPolygons(const Polygons &subjects);
ShapeMesh buildShapeMesh(int shapeId, const ShapeGeometry &geometry);
Polygons normalizedInputPolygons(const Polygons &polygons);
QVector<ShapeFeature> shapeFeatures(
    const QVector<Vec2> &boundary);

DistanceSeed distanceSeed(const Polygons &polygons);
QRectF shapeBounds(const ShapeMesh &shape);
QVector<CandidateJob> wholeComponentJobs(
    const Polygons &residual,
    const QVector<ShapeMesh> &catalog);
QVector<CandidateJob> featureAwareJobs(
    const QVector<const ShapeMesh *> &shapes,
    const QVector<ContourFeature> &targetFeatures,
    const QVector<int> &representedFeatureIds,
    const DistanceSeed &seed,
    int maximumJobs);
QVector<CandidateFeatureAssignment>
placementFeatureAssignments(
    const ShapeMesh &shape,
    const Affine &transform,
    const QVector<int> &featureIds,
    const QVector<ContourFeature> &targetFeatures);
const ShapeMesh *shapeById(const QVector<ShapeMesh> &catalog,
                           int shapeId);
QVector<FixedCandidate> polygonMeshCandidates(
    const QVector<ContourSpan> &spans,
    const QVector<ShapeMesh> &catalog,
    const std::function<bool()> &cancelled);
QVector<CandidateJob> hardPointCandidateSeeds(
    const QVector<ContourSpan> &spans,
    const QVector<ShapeMesh> &catalog,
    double boundaryTolerance);
QVector<CandidateJob> rankedHardPointJobs(
    const QVector<CandidateJob> &seeds,
    const Polygons &residual,
    const Polygons &target,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options);
bool lexicographicTransformLess(const Affine &left,
                                const Affine &right);
Candidate legalCandidate(const ShapeMesh &shape,
                         Affine transform,
                         const Polygons &residual,
                         const Polygons &target,
                         const Polygons &legalEnvelope,
                         const EvaluationBounds &subjectBounds,
                         const FillOptions &options,
                         CandidateProfile *profile,
                         const QVector<CandidateFeatureAssignment>
                             &featureAssignments = {},
                         const CandidateAnchor *anchor = nullptr);
CandidateJobResult optimizeCandidate(
    const CandidateJob &job,
    const Polygons &residual,
    const Polygons &target,
    const Polygons &legalEnvelope,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    const DistanceSeed &seed,
    const std::function<bool()> &cancelled);
std::array<double, kGradientCount> affineValues(
    const Affine &transform);
bool optimizeCandidatesGpu(
    const std::vector<CandidateJob> &jobs,
    const Polygons &residual,
    const Polygons &target,
    const Polygons &legalEnvelope,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    const DistanceSeed &seed,
    GpuAreaEvaluator *evaluator,
    QThreadPool *candidatePool,
    FillProfile *profile,
    const std::function<bool()> &cancelled,
    std::vector<Candidate> *results,
    bool *wasCancelled);
CandidateInitialization candidateInitialization(
    const DistanceSeed &seed,
    int restart,
    std::mt19937_64 *random);
ResidualComplexity residualComplexity(const Polygons &polygons);
Polygons componentAtPoint(const Polygons &polygons,
                          const QPointF &point);
double descriptorDistance(const ShapeMesh &shape,
                          const Polygons &polygons);
QVector<const ShapeMesh *> routedShapes(
    const Polygons &residual,
    const QVector<ShapeMesh> &catalog,
    bool useRouter);
CandidateSelection selectCandidate(
    const QVector<Candidate> &candidates,
    const QVector<ShapeMesh> &catalog,
    const Polygons &target,
    const Polygons &legalEnvelope,
    const Polygons &currentCoverage,
    const Polygons &residual,
    const QVector<ContourFeature> &features,
    const FeatureMatchSummary &currentFeatureMatches,
    const FillOptions &options);

Polygons placementFootprints(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog);
ExactCoverState exactCoverState(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea);
void refreshPlacementGains(
    QVector<Placement> *placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover);
FeatureMatchSummary matchContourFeatures(
    const Polygons &coverage,
    const QVector<ContourFeature> &features);
double permittedOutsideEnvelopeArea(
    const FillOptions &options);
bool legalEnvelopeArea(
    double outsideEnvelopeArea,
    const FillOptions &options);
bool legalOutwardDistance(
    double maximumOutwardDistance,
    const FillOptions &options);
double exposedFeatureArc(
    const Polygons &coverage,
    const ContourFeature &feature);
void assignFeatureOwnership(
    QVector<Placement> *placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &coverage,
    const QVector<ContourFeature> &features);
MeshCoverPlan meshCoverPlan(
    const QVector<FixedCandidate> &candidates,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea,
    const FillOptions &options,
    const std::function<bool()> &cancelled);
StructuralCoverPlan structuralCoverPlan(
    const QVector<QVector<ContourSpan>> &boundaryLoops,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea,
    const FillOptions &options,
    const std::function<bool()> &cancelled);

bool prunePlacements(
    QVector<Placement> *placements,
    ExactCoverState *currentState,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    const QVector<ContourFeature> &features,
    double targetArea,
    const FillOptions &options,
    GpuAreaEvaluator *gpuEvaluator,
    QThreadPool *candidatePool,
    FillProfile *profile,
    QElapsedTimer *elapsed,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress);
std::uint64_t derivedSeed(const Polygons &polygons);
bool validOptions(const FillOptions &options);
void mergeRepairProfile(const FillProfile &repair,
                        FillProfile *profile);

} // namespace gui::cover
