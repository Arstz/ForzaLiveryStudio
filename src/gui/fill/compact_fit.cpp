#include "compact_fit.h"
#include "catalog_cover_internal.h"
#include "compact_fit_quality.h"
#include "compact_fit_budget.h"
#include "compact_fit_reduction.h"
#include "compact_fit_gpu_rank.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <queue>

namespace gui::compact {
namespace {

constexpr int kRefinementLevels = 7;
constexpr int kMovesPerLevel = 6;
constexpr int kRefitPasses = 3;
constexpr int kMergeRounds = 64;
constexpr int kMergeNeighbors = 4;
constexpr int kRetainedSeeds = 6;
constexpr int kSeedOrientations = 16;
constexpr int kPrimaryOrientations = 8;
constexpr int kPrimaryRetainedSeeds = 3;
constexpr int kDeletionTrials = 2;
constexpr int kMinimumGpuOwnershipPieces = 16;
constexpr int kGpuExactRankingCandidates = 12;
constexpr int kGpuPipelinePasses = 3;
constexpr int kGpuBalancedPasses = 2;
constexpr int kGpuGroupCandidates = 8;
constexpr int kGpuGroupFragments = 12;
constexpr int kRefitNeighbors = 4;
constexpr double kGpuRepairInnerWeight = 256.0;
constexpr double kGpuMaximumTurnRegression = 0.1;
constexpr double kGpuMinimumFootprint = 100.0;
constexpr double kGpuMaximumCornerRegression = 0.5;
constexpr double kDeepErrorWeight = 16.0;
constexpr double kInitialStepFraction = 0.04;
constexpr double kScoreEpsilon = 1e-7;
constexpr double kMissingWeight = 4.0;
constexpr double kBoundaryWeight = 8.0;
constexpr double kGapRepairWeight = 128.0;
constexpr double kRefinementExtent = 512.0;
constexpr double kBoundaryEnergyPerLength = 0.025;
constexpr double kLengthPerDefect = 80.0;
constexpr int kWorkReportInterval = 256;
constexpr int kMaximumMergeRefinements = 12;
constexpr double kMaximumExcessTurn = 0.6;
constexpr double kConvexitySlack = 0.005;
constexpr double kCornerWorkShare = 0.5;
constexpr double kOwnershipClearance = 1e-5;
constexpr int kResidualTrialsPerComponent = 384;
constexpr int kResidualCenterTrials = 256;
constexpr int kRepairNeighbors = 4;
constexpr int kReuseCandidates = 48;
constexpr int kReuseFirstChoices = 4;
constexpr int kReuseSecondChoices = 8;
constexpr int kJointTrialsPerPair = 192;
constexpr double kRepairConnectionBonus = 1.25;
constexpr double kLocalWindowExtent = 1.5;
constexpr double kLocalWindowFraction = 0.75;
constexpr double kJointRepairMinimumFraction = 0.5;
constexpr double kRepairPlacementCost = 4.0;
constexpr int kRepairRetainedMoves = 4;
constexpr int kBroadRepairWork = 512;
constexpr int kBroadRepairMinimumWork = kResidualTrialsPerComponent * 4;
constexpr int kBroadRepairClusters = 8;
constexpr int kBroadRepairMembers = 12;
constexpr double kBroadRepairRadius = 64.0;
constexpr double kBroadRepairFootprint = 64.0;
constexpr std::array<int, 28> kSearchShapeIds = {101, 102, 103, 109, 110, 120, 122, 124,
    126, 127, 128, 129, 130, 136, 139, 812, 901, 930, 2110, 2113, 2117, 2118,
    2123, 2133, 2134, 2135, 2136, 2321};

using catalog::Polygons;

struct Piece {
    PenPlacement placement;
    Polygons polygons;
    QRectF bounds;
};

struct Objective {
    Polygons target;
    Polygons visibleTarget;
    Polygons preferred;
    Polygons inner;
    Polygons outer;
    Polygons spillFree;
    Polygons leeway;
    std::shared_ptr<BoundaryModel> boundary;
    BoundaryMetrics targetMetrics;
    std::optional<ReductionState> reductionBaseline;
    mutable CoverageOwnership ownership;
#ifdef FLS_HAS_CUDA
    mutable std::shared_ptr<gpu::RasterRanker> gpuRanker;
#endif
    std::shared_ptr<const QVector<ReusableCandidate>> replacementCandidates;
    mutable QJsonObject coverageRejections;
    mutable QJsonObject connectorDiagnostics;
    mutable QJsonObject gpuPipelineCandidate;
    std::function<void(int)> workProgress;
    mutable int evaluations = 0;
    mutable int qualityEvaluations = 0;
    mutable int qualityBoundsRejected = 0;
    mutable int mergeRefinements = 0;
    mutable int approximateReductions = 0;
    mutable int refitPlacements = 0;
    mutable int coverageRejected = 0;
    mutable int residualInsertions = 0;
    mutable int residualConnectors = 0;
    mutable int neighborhoodRepairs = 0;
    mutable int topologyRepairs = 0;
    mutable int feasibleCheckpoints = 0;
    mutable int feasibleRestores = 0;
    mutable int jointMoves = 0;
    mutable int exactReductions = 0;
    mutable int ownershipMergeTrials = 0;
    mutable int ownershipMerges = 0;
    mutable int reusedCandidates = 0;
    mutable int reuseChecks = 0;
    mutable int reusedMerges = 0;
    mutable int reusedPairs = 0;
    mutable int localContexts = 0;
    mutable int localEvaluations = 0;
    mutable int globalMoveChecks = 0;
    mutable int jointRepairTrials = 0;
    mutable int jointRepairs = 0;
    mutable int repairClusters = 0;
    mutable int repairGroupReplacements = 0;
    mutable int repairBenefitRejected = 0;
    mutable int broadRepairTrials = 0;
    mutable int broadRepairReplacements = 0;
    mutable int gpuRankerFallbacks = 0;
    mutable int gpuRefinementBatches = 0;
    mutable int gpuRefinementCandidates = 0;
    mutable int gpuRefinementCommits = 0;
    mutable int gpuRefinementCheckpoints = 0;
    mutable int gpuPipelinePasses = 0;
    mutable int gpuPipelinePruned = 0;
    mutable int gpuPipelineRestores = 0;
    mutable int gpuPipelineExpanded = 0;
    mutable int gpuGroupTrials = 0;
    mutable int gpuGroupReplacements = 0;
    int evaluationLimit = 0;
    int defectLimit = 0;
    bool gpuRankingEnabled = false;
    mutable bool gpuRankingDisabled = false;
    double allowance = 0.0;
    double areaBudget = 0.0;
    double inwardAllowance = 0.0;
    double fittingInwardAllowance = 0.0;
    double qualityLimit = 0.0;
    double cornerAllowance = 0.0;
    double boundaryWeight = kBoundaryWeight;
};

struct Context {
    Polygons residual;
    Polygons others;
    Polygons innerResidual;
    Polygons target;
    Polygons globalParts;
    QRectF window;
    QRectF measuredWindow;
    mutable std::optional<Polygons> globalOthers;
    mutable std::optional<Polygons> inwardResidual;
    mutable std::optional<Polygons> observedOthers;
    mutable std::optional<BoundaryModel::ObservationWindow> observationWindow;
    mutable std::optional<Polygons> spillExclusion;
    const Objective *objective = nullptr;
};

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

Piece makePiece(const PenPrimitive &shape, const QTransform &transform) {
    Piece result;
    result.placement.shapeId = shape.shapeId;
    result.placement.transform = catalog::emittedTransform(transform);
    result.polygons = catalog::mapped(shape, result.placement.transform);
    result.bounds = catalog::painterPath(result.polygons).boundingRect();
    result.placement.area = catalog::area(result.polygons);

    return result;
}

#ifdef FLS_HAS_CUDA
gpu::Geometry gpuGeometry(const Polygons &polygons) {
    gpu::Geometry result;
    const int firstLoop = static_cast<int>(result.loops.size());
    for (const QPolygonF &polygon : polygons) {
        const int firstPoint = static_cast<int>(result.points.size());
        for (const QPointF &point : polygon) {
            result.points.push_back({static_cast<float>(point.x()),
                static_cast<float>(point.y())});
        }
        result.loops.push_back({firstPoint, static_cast<int>(polygon.size())});
    }
    result.pieces.push_back({firstLoop,
        static_cast<int>(result.loops.size()) - firstLoop});

    return result;
}

gpu::Geometry gpuGeometry(const QVector<Piece> &pieces) {
    gpu::Geometry result;
    for (const Piece &piece : pieces) {
        const int firstLoop = static_cast<int>(result.loops.size());
        for (const QPolygonF &polygon : piece.polygons) {
            const int firstPoint = static_cast<int>(result.points.size());
            for (const QPointF &point : polygon) {
                result.points.push_back({static_cast<float>(point.x()),
                    static_cast<float>(point.y())});
            }
            result.loops.push_back({firstPoint, static_cast<int>(polygon.size())});
        }
        result.pieces.push_back({firstLoop,
            static_cast<int>(result.loops.size()) - firstLoop});
    }

    return result;
}

gpu::Affine gpuAffine(const QTransform &transform) {
    return {static_cast<float>(transform.m11()),
        static_cast<float>(transform.m12()),
        static_cast<float>(transform.m21()),
        static_cast<float>(transform.m22()),
        static_cast<float>(transform.dx()),
        static_cast<float>(transform.dy())};
}

bool prepareGpuRanker(const Objective &objective) {
    if (!objective.gpuRankingEnabled || objective.gpuRankingDisabled) {
        return false;
    }
    if (!objective.gpuRanker) {
        objective.gpuRanker = gpu::createRasterRanker(
            gpuGeometry(objective.preferred), gpuGeometry(objective.target),
            gpuGeometry(objective.inner), gpuGeometry(objective.outer),
            gpuGeometry(objective.spillFree), objective.cornerAllowance * 0.5);
    }
    if (!objective.gpuRanker
        || !objective.gpuRanker->stats().error.empty()) {
        objective.gpuRankingDisabled = true;
        ++objective.gpuRankerFallbacks;
        return false;
    }

    return true;
}
#endif

Polygons support(const QVector<Piece> &pieces, const QVector<int> &excluded = {}) {
    Polygons result;
    for (int index = 0; index < pieces.size(); ++index) {
        if (!excluded.contains(index)) {
            result += pieces[index].polygons;
        }
    }

    return catalog::unite(result);
}

Polygons visibleSupport(const Polygons &coverage, const Objective &objective) {
    return objective.leeway.isEmpty()
        ? coverage : catalog::subtract(coverage, objective.leeway);
}

ReductionState reductionStateFor(const Polygons &coverage, const Objective &objective) {
    return reductionState(coverage, objective.target, objective.visibleTarget,
        objective.leeway, *objective.boundary, objective.inwardAllowance);
}

Context contextFor(const QVector<Piece> &pieces, const QVector<int> &excluded,
                   const Objective &objective) {
    Context result;
    result.objective = &objective;
    result.target = objective.target;
    result.others = support(pieces, excluded);
    result.residual = catalog::subtract(objective.preferred, result.others);
    result.innerResidual = catalog::subtract(objective.inner, result.others);

    return result;
}

Context localContextFor(const QVector<Piece> &pieces, const QVector<int> &excluded,
                        const Objective &objective) {
    QRectF bounds;
    for (int index : excluded) {
        bounds = bounds.isNull() ? pieces[index].bounds : bounds.united(pieces[index].bounds);
    }
    const auto targetBounds = catalog::painterPath(objective.target).boundingRect();
    const double extent = std::max(bounds.width(), bounds.height());
    const double inset = objective.cornerAllowance * 4.0 + catalog::kVerificationClearance * 4.0;
    const double margin = extent * kLocalWindowExtent + objective.allowance * 4.0
        + objective.cornerAllowance * 16.0 + objective.inwardAllowance * 4.0;
    const auto window = bounds.adjusted(-margin, -margin, margin, margin);
    const auto overlap = window.intersected(targetBounds);
    if (excluded.isEmpty() || overlap.width() * overlap.height()
            >= targetBounds.width() * targetBounds.height() * kLocalWindowFraction) {
        return contextFor(pieces, excluded, objective);
    }
    Context result;
    Polygons nearby;
    const Polygons clip{QPolygonF(window)};
    result.objective = &objective;
    result.window = window;
    result.measuredWindow = window.adjusted(inset, inset, -inset, -inset);
    result.target = catalog::intersect(objective.target, clip);
    for (int index = 0; index < pieces.size(); ++index) {
        if (!excluded.contains(index)) {
            result.globalParts += pieces[index].polygons;
            if (pieces[index].bounds.intersects(window)) {
                nearby += pieces[index].polygons;
            }
        }
    }
    result.others = catalog::intersect(catalog::unite(nearby), clip);
    result.residual = catalog::subtract(catalog::intersect(objective.preferred, clip), result.others);
    result.innerResidual = catalog::subtract(catalog::intersect(objective.inner, clip), result.others);
    ++objective.localContexts;

    return result;
}

Polygons completeCoverage(const Context &context, const Polygons &addition) {
    if (context.window.isNull()) {
        return catalog::unite(context.others + addition);
    }
    if (!context.globalOthers) {
        context.globalOthers = catalog::unite(context.globalParts);
    }

    return catalog::unite(*context.globalOthers + addition);
}

void synchronizeOwnership(const QVector<Piece> &pieces, const Objective &objective) {
    QVector<Polygons> polygons;
    polygons.reserve(pieces.size());
    for (const auto &piece : pieces) {
        polygons.push_back(piece.polygons);
    }
    objective.ownership.synchronize(polygons);
}

double gain(const Piece &piece, const Context &context) {
    if (context.objective->evaluations >= context.objective->evaluationLimit) {
        return -std::numeric_limits<double>::infinity();
    }
    ++context.objective->evaluations;
    if (context.objective->workProgress && context.objective->evaluations % kWorkReportInterval == 0) {
        context.objective->workProgress(context.objective->evaluations);
    }
    const auto exclusive = catalog::subtract(piece.polygons, context.others);
    const double covered = catalog::area(catalog::intersect(piece.polygons, context.residual));
    const double deepCovered = catalog::area(catalog::intersect(piece.polygons, context.innerResidual));
    const double spill = catalog::area(catalog::subtract(exclusive, context.objective->spillFree));
    const double deepSpill = catalog::area(catalog::subtract(exclusive, context.objective->outer));

    return kMissingWeight * covered - spill + kDeepErrorWeight * (deepCovered - deepSpill);
}

double boundaryCost(const Polygons &coverage, const Objective &objective) {
    const double missingBeyondAllowance = catalog::area(catalog::subtract(objective.target,
        catalog::expanded(coverage, objective.fittingInwardAllowance)));

    return objective.boundaryWeight * objective.boundary->energy(
        objective.boundary->measure(visibleSupport(coverage, objective)))
        + kGapRepairWeight * missingBeyondAllowance;
}

double qualityGain(const Piece &piece, const Context &context,
                   double minimumScore = -std::numeric_limits<double>::infinity()) {
    if (context.objective->evaluations >= context.objective->evaluationLimit) {
        return -std::numeric_limits<double>::infinity();
    }
    ++context.objective->evaluations;
    context.objective->localEvaluations += !context.window.isNull();
    if (context.objective->workProgress && context.objective->evaluations % kWorkReportInterval == 0) {
        context.objective->workProgress(context.objective->evaluations);
    }
    if ((!context.window.isNull() && !context.measuredWindow.contains(piece.bounds))
        || !catalog::subtract(piece.polygons, context.objective->outer).isEmpty()) {
        return -std::numeric_limits<double>::infinity();
    }
    if (!context.spillExclusion) {
        context.spillExclusion = catalog::unite(context.others + context.objective->spillFree);
    }
    const double covered = catalog::area(catalog::intersect(piece.polygons, context.residual));
    const double deepCovered = catalog::area(catalog::intersect(piece.polygons, context.innerResidual));
    const double spill = catalog::area(catalog::subtract(piece.polygons, *context.spillExclusion));
    const double areaGain = kMissingWeight * covered + kDeepErrorWeight * deepCovered - spill;
    if (!context.inwardResidual) {
        context.inwardResidual = catalog::subtract(context.target,
            catalog::expanded(context.others, context.objective->fittingInwardAllowance));
    }
    const double missingBeyondAllowance = catalog::area(catalog::subtract(*context.inwardResidual,
        catalog::expanded(piece.polygons, context.objective->fittingInwardAllowance)));
    if (areaGain - kGapRepairWeight * missingBeyondAllowance <= minimumScore) {
        ++context.objective->qualityBoundsRejected;
        return -std::numeric_limits<double>::infinity();
    }
    const auto coverage = catalog::unite(context.others + piece.polygons);
    if (!context.observedOthers && !context.others.isEmpty()) {
        context.observedOthers = context.objective->boundary->observationSupport(
            visibleSupport(context.others, *context.objective));
    }
    if (context.observedOthers && (!context.observationWindow || !context.observationWindow->additionBounds.contains(piece.bounds))) {
        context.observationWindow = context.objective->boundary->observationWindow(
            visibleSupport(context.others, *context.objective), *context.observedOthers, piece.bounds);
    }
    const auto observed = context.observedOthers
        ? context.objective->boundary->observationSupport(
            visibleSupport(piece.polygons, *context.objective), *context.observationWindow)
        : context.objective->boundary->observationSupport(
            visibleSupport(coverage, *context.objective));
    ++context.objective->qualityEvaluations;

    return areaGain - context.objective->boundaryWeight * context.objective->boundary->energy(
        context.objective->boundary->measure(
            visibleSupport(coverage, *context.objective), observed, context.measuredWindow))
        - kGapRepairWeight * missingBeyondAllowance;
}

double error(const Polygons &coverage, const Objective &objective) {
    const Polygons visibleCoverage = visibleSupport(coverage, objective);

    return catalog::area(catalog::subtract(objective.visibleTarget, visibleCoverage))
        + catalog::area(catalog::subtract(visibleCoverage, objective.visibleTarget));
}

bool acceptable(const Polygons &coverage, const Objective &objective) {
    if (coverage.isEmpty() || error(coverage, objective) > objective.areaBudget) {
        return false;
    }
    if (!catalog::subtract(coverage, objective.outer).isEmpty()) {
        return false;
    }

    if (!catalog::subtract(objective.target, catalog::expanded(coverage, objective.inwardAllowance)).isEmpty()) {
        return false;
    }
    const auto metrics = objective.boundary->measure(
        visibleSupport(coverage, objective));

    return objective.boundary->energy(metrics) <= objective.qualityLimit
        && metrics.maximumExcessTurn <= kMaximumExcessTurn
        && metrics.maximumCornerDistance <= objective.cornerAllowance
        && metrics.cornerDefects <= objective.defectLimit
        && metrics.components == objective.targetMetrics.components
        && metrics.holes == objective.targetMetrics.holes;
}

struct ReductionContext {
    ReductionState state;
    bool verified = false;
};

bool coverageMoveAllowed(const ReductionState &after, const ReductionState &before, const Objective &objective, bool growing = false) {
    QString reason;
    if (!catalog::subtract(after.coverage, objective.outer).isEmpty()) {
        reason = QStringLiteral("outer envelope");
    } else if (!preservesCoverage(after, before, objective.targetMetrics, objective.cornerAllowance, growing)) {
        reason = QStringLiteral("coverage or corner");
    }
    if (!reason.isEmpty()) {
        objective.coverageRejections[reason] = objective.coverageRejections.value(reason).toInt() + 1;
    }

    return reason.isEmpty();
}

ReductionContext reductionContext(const QVector<Piece> &pieces, const Objective &objective) {
    const auto coverage = support(pieces);

    return {reductionStateFor(coverage, objective),
        acceptable(coverage, objective)};
}

bool reductionAllowed(const Polygons &coverage, const ReductionContext &before, const Objective &objective) {
    if (acceptable(coverage, objective)) {
        return true;
    }
    if (before.verified || !objective.reductionBaseline
        || !catalog::subtract(coverage, objective.outer).isEmpty()) {
        return false;
    }
    const auto after = reductionStateFor(coverage, objective);
    if (nonWorseningReduction(after, before.state, objective.targetMetrics)
        && nonWorseningReduction(after, *objective.reductionBaseline, objective.targetMetrics)) {
        ++objective.approximateReductions;
        return true;
    }

    return false;
}

QTransform movedTransform(const Piece &piece, int parameter, double amount) {
    const QPointF center = piece.bounds.center();
    QTransform movement;
    movement.translate(center.x(), center.y());
    if (parameter == 0) {
        movement.translate(amount, 0.0);
    } else if (parameter == 1) {
        movement.translate(0.0, amount);
    } else if (parameter == 2) {
        movement.scale(std::exp(amount), 1.0);
    } else if (parameter == 3) {
        movement.scale(1.0, std::exp(amount));
    } else if (parameter == 4) {
        movement.rotateRadians(amount);
    } else {
        movement.shear(amount, 0.0);
    }
    movement.translate(-center.x(), -center.y());

    return piece.placement.transform * movement;
}

#ifdef FLS_HAS_CUDA
std::optional<Piece> refineGpu(Piece piece, const PenPrimitive &shape,
                              const Context &context,
                              const std::function<bool()> &cancelled,
                              int evaluationLimit,
                              const std::function<void(const Piece &,
                                  const ReductionState &)> &accepted,
                              const gpu::Geometry *coverageOverride = nullptr,
                              const gpu::AdditionWeights &weights = {},
                              bool persistentCoverage = false) {
    const Piece originalPiece = piece;
    const int limit = std::min(evaluationLimit, context.objective->evaluationLimit);
    const auto stopRefinement = [&] {
        return stopped(cancelled) || context.objective->evaluations >= limit;
    };
    if (stopRefinement() || !prepareGpuRanker(*context.objective)) {
        return {};
    }
    gpu::Geometry localCoverage;
    bool prepared = false;
    if (persistentCoverage) {
        prepared = context.objective->gpuRanker->prepareReplacementCoverage(
            gpuGeometry(QVector<Piece>{piece}));
    } else {
        if (!coverageOverride) {
            localCoverage = gpuGeometry(context.others);
        }
        const gpu::Geometry &coverage = coverageOverride ? *coverageOverride : localCoverage;
        prepared = context.objective->gpuRanker->prepareAdditionCoverage(coverage);
    }
    if (!prepared) {
        context.objective->gpuRankingDisabled = true;
        ++context.objective->gpuRankerFallbacks;
        return {};
    }
    const gpu::Geometry primitive = gpuGeometry(shape.contours);
    std::vector<double> currentScore;
    if (!context.objective->gpuRanker->evaluateTransforms(
            primitive, {gpuAffine(piece.placement.transform)}, &currentScore, weights)
        || currentScore.size() != 1) {
        context.objective->gpuRankingDisabled = true;
        ++context.objective->gpuRankerFallbacks;
        return {};
    }
    ++context.objective->evaluations;
    double best = currentScore.front();
    bool changed = false;
    const double extent = std::max(piece.bounds.width(), piece.bounds.height());
    const double initialFraction = kInitialStepFraction
        * std::min(1.0, kRefinementExtent / extent);
    const int levels = extent > kRefinementExtent
        ? std::max(kRefinementLevels, 1 + static_cast<int>(std::ceil(std::log2(
            initialFraction * extent
            / (context.objective->fittingInwardAllowance * 0.25)))))
        : kRefinementLevels;
    for (int level = 0; level < levels && !stopRefinement(); ++level) {
        const int levelLimit = context.objective->evaluations
            + (limit - context.objective->evaluations) / (levels - level);
        const double fraction = std::ldexp(initialFraction, -level);
        for (int iteration = 0; iteration < kMovesPerLevel
                && !stopRefinement() && context.objective->evaluations < levelLimit;
             ++iteration) {
            QVector<QTransform> trials;
            std::vector<gpu::Affine> numericTrials;
            const int available = levelLimit - context.objective->evaluations;
            const double minimumArea = kGpuMinimumFootprint
                * std::pow(context.objective->cornerAllowance * 2.0, 2.0);
            for (int parameter = 0; parameter < 6 && trials.size() < available; ++parameter) {
                for (double sign : {-1.0, 1.0}) {
                    const double step = sign * fraction * (parameter < 2 ? extent : 1.0);
                    const auto transform = movedTransform(piece, parameter, step);
                    const double area = shape.area * std::abs(transform.determinant());
                    if (std::abs(transform.determinant()) >= catalog::kMinimumDeterminant
                        && area >= minimumArea) {
                        trials.push_back(transform);
                        numericTrials.push_back(gpuAffine(transform));
                    }
                    if (trials.size() >= available) {
                        break;
                    }
                }
            }
            if (trials.isEmpty()) {
                break;
            }
            std::vector<double> approximate;
            if (!context.objective->gpuRanker->evaluateTransforms(
                    primitive, numericTrials, &approximate, weights)
                || approximate.size() != static_cast<size_t>(trials.size())) {
                context.objective->gpuRankingDisabled = true;
                ++context.objective->gpuRankerFallbacks;
                return piece;
            }
            ++context.objective->gpuRefinementBatches;
            context.objective->gpuRefinementCandidates += trials.size();
            int selected = -1;
            double selectedScore = best;
            for (int index = 0; index < trials.size(); ++index) {
                if (approximate[index] > selectedScore + kScoreEpsilon) {
                    selected = index;
                    selectedScore = approximate[index];
                }
            }
            for (int index = 0; index < trials.size()
                    && context.objective->evaluations < levelLimit
                    && !stopped(cancelled); ++index) {
                ++context.objective->evaluations;
                if (context.objective->workProgress
                    && context.objective->evaluations % kWorkReportInterval == 0) {
                    context.objective->workProgress(context.objective->evaluations);
                }
            }
            if (selected < 0) {
                break;
            }
            piece = makePiece(shape, trials[selected]);
            best = selectedScore;
            changed = true;
            ++context.objective->gpuRefinementCommits;
        }
    }
    if (persistentCoverage && changed
        && !context.objective->gpuRanker->commitReplacement(
            gpuGeometry(QVector<Piece>{originalPiece}),
            gpuGeometry(QVector<Piece>{piece}))) {
        context.objective->gpuRankingDisabled = true;
        ++context.objective->gpuRankerFallbacks;
        return {};
    }
    if (changed && accepted && !stopped(cancelled)) {
        ++context.objective->gpuRefinementCheckpoints;
        accepted(piece, reductionStateFor(completeCoverage(context, piece.polygons),
            *context.objective));
    }

    return piece;
}
#endif

Piece refine(Piece piece, const PenPrimitive &shape, const Context &context,
              const std::function<bool()> &cancelled, bool boundaryAware = false,
              int evaluationLimit = std::numeric_limits<int>::max(),
              const std::function<void(const Piece &, const ReductionState &)> &accepted = {}) {
#ifdef FLS_HAS_CUDA
    if (boundaryAware) {
        const auto batched = refineGpu(piece, shape, context, cancelled,
            evaluationLimit, accepted);
        if (batched) {
            return *batched;
        }
    }
#endif
    const int limit = std::min(evaluationLimit, context.objective->evaluationLimit);
    const auto stopRefinement = [&] {
        return stopped(cancelled) || context.objective->evaluations >= limit;
    };
    if (stopRefinement()) {
        return piece;
    }
    const auto evaluate = [&](const Piece &candidate, double threshold) {
        return boundaryAware ? qualityGain(candidate, context, threshold) : gain(candidate, context);
    };
    double best = evaluate(piece, -std::numeric_limits<double>::infinity());
    std::optional<ReductionState> safeState;
    const double extent = std::max(piece.bounds.width(), piece.bounds.height());
    const double initialFraction = boundaryAware ? kInitialStepFraction * std::min(1.0, kRefinementExtent / extent) : kInitialStepFraction;
    const int levels = boundaryAware && extent > kRefinementExtent
        ? std::max(kRefinementLevels, 1 + static_cast<int>(std::ceil(std::log2(initialFraction * extent
            / (context.objective->fittingInwardAllowance * 0.25))))) : kRefinementLevels;
    for (int level = 0; level < levels && !stopRefinement(); ++level) {
        const int levelLimit = boundaryAware ? context.objective->evaluations
            + (limit - context.objective->evaluations) / (levels - level) : limit;
        const auto stopLevel = [&] {
            return stopRefinement() || context.objective->evaluations >= levelLimit;
        };
        const double fraction = std::ldexp(initialFraction, -level);
        for (int iteration = 0; iteration < kMovesPerLevel && !stopLevel(); ++iteration) {
            bool changed = false;
            for (int parameter = 0; parameter < 6 && !stopLevel(); ++parameter) {
                for (double sign : {-1.0, 1.0}) {
                    if (stopLevel()) {
                        break;
                    }
                    const double step = sign * fraction * (parameter < 2 ? extent : 1.0);
                    const auto transform = movedTransform(piece, parameter, step);
                    if (std::abs(transform.determinant()) < catalog::kMinimumDeterminant) {
                        continue;
                    }
                    Piece trial = makePiece(shape, transform);
                    const double score = evaluate(trial, best + kScoreEpsilon);
                    if (score > best + kScoreEpsilon) {
                        if (boundaryAware) {
                            if (!safeState) {
                                safeState = reductionStateFor(completeCoverage(context, piece.polygons),
                                    *context.objective);
                            }
                            ++context.objective->globalMoveChecks;
                            auto after = reductionStateFor(completeCoverage(context, trial.polygons),
                                *context.objective);
                            if (!coverageMoveAllowed(after, *safeState, *context.objective)) {
                                ++context.objective->coverageRejected;
                                continue;
                            }
                            safeState = std::move(after);
                        }
                        piece = std::move(trial);
                        best = score;
                        if (accepted && safeState) {
                            accepted(piece, *safeState);
                        }
                        changed = true;
                    }
                }
            }
            if (!changed) {
                break;
            }
        }
    }

    return piece;
}

const PenPrimitive &primitiveFor(int id, const QVector<catalog::Primitive> &primitives) {
    const auto found = std::find_if(primitives.begin(), primitives.end(), [id](const auto &entry) {
        return entry.shape.shapeId == id;
    });
    if (found == primitives.end()) {
        throw std::runtime_error("Compact fit seed is outside the opaque dictionary");
    }

    return found->shape;
}

double boundaryDistanceSquared(const Polygons &polygons, const QPointF &point) {
    double result = std::numeric_limits<double>::infinity();
    for (const auto &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            const auto delta = polygon[(index + 1) % polygon.size()] - polygon[index];
            const double length = QPointF::dotProduct(delta, delta);
            const double parameter = length > catalog::kMinimumDeterminant
                ? std::clamp(QPointF::dotProduct(point - polygon[index], delta) / length, 0.0, 1.0) : 0.0;
            const auto displacement = polygon[index] + delta * parameter - point;
            result = std::min(result, QPointF::dotProduct(displacement, displacement));
        }
    }

    return result;
}

QVector<double> refitWeights(const QVector<Piece> &pieces, const Objective &objective) {
    QVector<double> weights(pieces.size(), 0.0);
    if (pieces.isEmpty()) {
        return weights;
    }
    const BoundaryModel output(visibleSupport(support(pieces), objective),
        objective.cornerAllowance * 2.0);
    for (const auto &corner : objective.boundary->protectedCorners()) {
        const auto reference = output.reference(corner);
        if (reference.distance <= objective.cornerAllowance) {
            continue;
        }
        QVector<int> owners;
        for (int index = 0; index < pieces.size(); ++index) {
            if (pieces[index].bounds.adjusted(-kOwnershipClearance, -kOwnershipClearance,
                kOwnershipClearance, kOwnershipClearance).contains(reference.point)
                && boundaryDistanceSquared(pieces[index].polygons, reference.point) <= kOwnershipClearance * kOwnershipClearance) {
                owners.push_back(index);
            }
        }
        for (int owner : owners) {
            weights[owner] += reference.distance * reference.distance / owners.size();
        }
    }
    const double sum = std::accumulate(weights.cbegin(), weights.cend(), 0.0);
    for (auto &weight : weights) {
        weight = sum > 0 ? (1.0 - kCornerWorkShare) / pieces.size() + kCornerWorkShare * weight / sum
            : 1.0 / pieces.size();
    }

    return weights;
}

void refit(QVector<Piece> *pieces, const Objective &objective,
            const QVector<catalog::Primitive> &primitives,
            const std::function<bool()> &cancelled, int passes = kRefitPasses,
            const std::function<void(int, const Piece &, const ReductionState &)> &accepted = {}) {
    for (int pass = 0; pass < passes && !stopped(cancelled); ++pass) {
        const int passLimit = objective.evaluations
            + (objective.evaluationLimit - objective.evaluations) / (passes - pass);
        const auto weights = refitWeights(*pieces, objective);
        QVector<int> order(pieces->size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int first, int second) { return weights[first] > weights[second]; });
        double remainingWeight = std::accumulate(weights.cbegin(), weights.cend(), 0.0);
        for (int position = 0; position < order.size() && !stopped(cancelled); ++position) {
            const int index = order[position];
            const int pieceLimit = objective.evaluations
                + (position + 1 == order.size() ? passLimit - objective.evaluations
                    : static_cast<int>((passLimit - objective.evaluations) * weights[index] / remainingWeight));
            remainingWeight -= weights[index];
            if (pieceLimit <= objective.evaluations) {
                continue;
            }
            ++objective.refitPlacements;
            const auto context = localContextFor(*pieces, {index}, objective);
            (*pieces)[index] = refine((*pieces)[index],
                primitiveFor((*pieces)[index].placement.shapeId, primitives), context, cancelled, true, pieceLimit,
                [&](const Piece &piece, const ReductionState &state) {
                    if (accepted) {
                        accepted(index, piece, state);
                    }
                });
        }
    }
}

void refitFeasible(QVector<Piece> *pieces, const Objective &objective,
                    const QVector<catalog::Primitive> &primitives, const std::function<bool()> &cancelled) {
    const auto baseline = reductionStateFor(support(*pieces), objective);
    const double areaLimit = std::max(objective.areaBudget, baseline.missingArea + baseline.spillArea);
    const auto cost = [&](const ReductionState &state) {
        return kMissingWeight * state.missingArea + state.spillArea
            + kGapRepairWeight * catalog::area(state.deepMissing)
            + objective.boundaryWeight * objective.boundary->energy(state.metrics);
    };
    auto retained = *pieces;
    double bestCost = cost(baseline);
    bool endpointRetained = true;
    refit(pieces, objective, primitives, cancelled, 1,
        [&](int index, const Piece &piece, const ReductionState &state) {
            endpointRetained = false;
            if (state.missingArea + state.spillArea > areaLimit + kScoreEpsilon) {
                return;
            }
            const double candidateCost = cost(state);
            if (candidateCost < bestCost - kScoreEpsilon && coverageMoveAllowed(state, baseline, objective)) {
                retained = *pieces;
                retained[index] = piece;
                bestCost = candidateCost;
                endpointRetained = true;
                ++objective.feasibleCheckpoints;
            }
        });
    if (!endpointRetained) {
        ++objective.feasibleRestores;
    }
    *pieces = std::move(retained);
}

void refitNeighborhood(QVector<Piece> *pieces, const QRectF &changed,
                        const Objective &objective, const QVector<catalog::Primitive> &primitives,
                        const std::function<bool()> &cancelled) {
    QVector<std::pair<double, int>> neighbors;
    for (int index = 0; index < pieces->size(); ++index) {
        const QPointF delta = (*pieces)[index].bounds.center() - changed.center();
        neighbors.push_back({QPointF::dotProduct(delta, delta), index});
    }
    std::sort(neighbors.begin(), neighbors.end());
    for (int neighbor = 0; neighbor < std::min(kRefitNeighbors, static_cast<int>(neighbors.size())) && !stopped(cancelled); ++neighbor) {
        const int index = neighbors[neighbor].second;
        const auto context = localContextFor(*pieces, {index}, objective);
        (*pieces)[index] = refine((*pieces)[index], primitiveFor((*pieces)[index].placement.shapeId, primitives),
            context, cancelled, true);
    }
}

double removalScore(const Polygons &exclusive, const Objective &objective) {
    return kMissingWeight * catalog::area(catalog::intersect(exclusive, objective.preferred))
        - catalog::area(catalog::subtract(exclusive, objective.target))
        + kDeepErrorWeight * (catalog::area(catalog::intersect(exclusive, objective.inner))
            - catalog::area(catalog::subtract(exclusive, objective.outer)));
}

QVector<int> removalOrder(const QVector<Piece> &pieces, const Objective &objective,
                          const std::function<bool()> &cancelled) {
    QVector<std::pair<double, int>> scores;
    QVector<int> result;
    synchronizeOwnership(pieces, objective);
#ifdef FLS_HAS_CUDA
    if (objective.gpuRankingEnabled && !objective.gpuRankingDisabled
        && pieces.size() >= kMinimumGpuOwnershipPieces && !stopped(cancelled)) {
        std::vector<double> approximate;
        if (prepareGpuRanker(objective)
            && objective.gpuRanker->evaluate(gpuGeometry(pieces), &approximate)
            && approximate.size() == static_cast<size_t>(pieces.size())) {
            const int candidates = std::min(static_cast<int>(pieces.size()), std::max(0,
                objective.evaluationLimit - objective.evaluations));
            for (int index = 0; index < pieces.size(); ++index) {
                scores.push_back({approximate[index], index});
            }
            std::sort(scores.begin(), scores.end());
            scores.resize(candidates);
            for (int index = 0; index < candidates; ++index) {
                ++objective.evaluations;
                if (objective.workProgress
                    && objective.evaluations % kWorkReportInterval == 0) {
                    objective.workProgress(objective.evaluations);
                }
            }
            QVector<std::pair<double, int>> exact;
            const int reranked = std::min(kGpuExactRankingCandidates,
                static_cast<int>(scores.size()));
            for (int position = 0; position < reranked; ++position) {
                exact.push_back({removalScore(
                    objective.ownership.exclusive({scores[position].second}), objective),
                    scores[position].second});
            }
            std::sort(exact.begin(), exact.end());
            for (const auto &entry : exact) {
                result.push_back(entry.second);
            }
            for (int position = reranked; position < scores.size(); ++position) {
                result.push_back(scores[position].second);
            }

            return result;
        }
        if (!objective.gpuRankingDisabled) {
            objective.gpuRankingDisabled = true;
            ++objective.gpuRankerFallbacks;
        }
    }
#endif
    for (int index = 0; index < pieces.size() && !stopped(cancelled); ++index) {
        if (objective.evaluations >= objective.evaluationLimit) {
            break;
        }
        const auto exclusive = objective.ownership.exclusive({index});
        const double score = removalScore(exclusive, objective);
        ++objective.evaluations;
        if (objective.workProgress && objective.evaluations % kWorkReportInterval == 0) {
            objective.workProgress(objective.evaluations);
        }
        scores.push_back({score, index});
    }
    std::sort(scores.begin(), scores.end());
    for (const auto &entry : scores) {
        result.push_back(entry.second);
    }

    return result;
}

void prune(QVector<Piece> *pieces, const Objective &objective,
           const std::function<bool()> &cancelled) {
    bool changed = true;
    while (changed && pieces->size() > 1 && !stopped(cancelled)) {
        const auto before = reductionContext(*pieces, objective);
        changed = false;
        for (int index : removalOrder(*pieces, objective, cancelled)) {
            if (stopped(cancelled)) {
                break;
            }
            const auto remaining = support(*pieces, {index});
            const bool identical = objective.ownership.exclusive({index}).isEmpty()
                && catalog::subtract(before.state.coverage, remaining).isEmpty()
                && catalog::subtract(remaining, before.state.coverage).isEmpty();
            if (identical || reductionAllowed(remaining, before, objective)) {
                objective.exactReductions += identical;
                objective.ownership.erase(index);
                pieces->removeAt(index);
                changed = true;
                break;
            }
        }
    }
}

#ifdef FLS_HAS_CUDA
bool pruneGpu(QVector<Piece> *pieces, const Objective &objective,
              const std::function<bool()> &cancelled) {
    while (pieces->size() > 1 && !stopped(cancelled)) {
        const int available = objective.evaluationLimit - objective.evaluations;
        if (available < pieces->size() || !prepareGpuRanker(objective)) {
            break;
        }
        std::vector<double> scores;
        if (!objective.gpuRanker->evaluate(gpuGeometry(*pieces), &scores)
            || scores.size() != static_cast<size_t>(pieces->size())) {
            objective.gpuRankingDisabled = true;
            ++objective.gpuRankerFallbacks;
            return false;
        }
        int selected = 0;
        for (int index = 1; index < pieces->size(); ++index) {
            if (scores[index] < scores[selected]) {
                selected = index;
            }
        }
        objective.evaluations += pieces->size();
        if (objective.workProgress) {
            objective.workProgress(objective.evaluations);
        }
        if (scores[selected] > kScoreEpsilon) {
            break;
        }
        pieces->removeAt(selected);
        ++objective.gpuPipelinePruned;
    }

    return true;
}

QTransform boundsTransform(const PenPrimitive &shape, const QPolygonF &points,
                           double angle, bool reflected);

bool gpuReplacementAllowed(const ReductionState &before,
                           const ReductionState &after,
                           const Objective &objective) {
    const double beforeError = before.missingArea + before.spillArea;
    const double afterError = after.missingArea + after.spillArea;

    return !after.coverage.isEmpty()
        && catalog::area(catalog::subtract(after.coverage, objective.outer))
            <= std::pow(objective.cornerAllowance * 0.5, 2.0) + kScoreEpsilon
        && afterError <= std::max(objective.areaBudget, beforeError) + kScoreEpsilon
        && catalog::area(after.deepMissing)
            <= catalog::area(before.deepMissing) + kScoreEpsilon
        && objective.boundary->energy(after.metrics)
            <= objective.boundary->energy(before.metrics) + kScoreEpsilon
        && after.metrics.maximumCornerDistance
            <= before.metrics.maximumCornerDistance + kGpuMaximumCornerRegression
        && after.metrics.maximumExcessTurn
            <= before.metrics.maximumExcessTurn + kGpuMaximumTurnRegression
        && after.metrics.cornerDefects <= before.metrics.cornerDefects
        && std::abs(after.metrics.components - objective.targetMetrics.components)
            <= std::abs(before.metrics.components - objective.targetMetrics.components)
        && std::abs(after.metrics.holes - objective.targetMetrics.holes)
            <= std::abs(before.metrics.holes - objective.targetMetrics.holes);
}

bool replaceSmallGroupsGpu(QVector<Piece> *pieces, const Objective &objective,
                           const QVector<catalog::Primitive> &primitives,
                           const std::function<bool()> &cancelled) {
    struct RankedReplacement {
        Piece piece;
        double score = 0.0;
    };
    QVector<int> fragments;
    const double minimumArea = kGpuMinimumFootprint
        * std::pow(objective.cornerAllowance * 2.0, 2.0);
    for (int index = 0; index < pieces->size(); ++index) {
        if ((*pieces)[index].placement.area < minimumArea) {
            fragments.push_back(index);
        }
    }
    std::stable_sort(fragments.begin(), fragments.end(), [&](int first, int second) {
        return (*pieces)[first].placement.area < (*pieces)[second].placement.area;
    });
    fragments.resize(std::min(kGpuGroupFragments, static_cast<int>(fragments.size())));
    for (int fragmentPosition = 0;
         fragmentPosition < fragments.size() && !stopped(cancelled); ++fragmentPosition) {
        const int fragment = fragments[fragmentPosition];
        if (fragment >= pieces->size()) {
            continue;
        }
        int neighbor = -1;
        double neighborDistance = std::numeric_limits<double>::infinity();
        for (int index = 0; index < pieces->size(); ++index) {
            if (index == fragment) {
                continue;
            }
            const QPointF delta = (*pieces)[index].bounds.center()
                - (*pieces)[fragment].bounds.center();
            const double distance = QPointF::dotProduct(delta, delta);
            if (distance < neighborDistance) {
                neighbor = index;
                neighborDistance = distance;
            }
        }
        if (neighbor < 0) {
            continue;
        }
        const QVector<Piece> group{(*pieces)[fragment], (*pieces)[neighbor]};
        QPolygonF groupPoints;
        for (const Piece &member : group) {
            for (const QPolygonF &polygon : member.polygons) {
                groupPoints += polygon;
            }
        }
        if (!objective.gpuRanker->prepareReplacementCoverage(gpuGeometry(group))) {
            return false;
        }
        const QTransform neighborTransform = (*pieces)[neighbor].placement.transform;
        const double angle = std::atan2(neighborTransform.m12(),
            neighborTransform.m11());
        const bool reflected = neighborTransform.determinant() < 0;
        QVector<RankedReplacement> ranked;
        for (const auto &primitive : primitives) {
            if (objective.evaluations >= objective.evaluationLimit) {
                break;
            }
            std::vector<gpu::Affine> numericTransforms;
            QVector<QTransform> transforms;
            for (double offset : {0.0, -0.12, 0.12}) {
                const QTransform transform = boundsTransform(primitive.shape,
                    groupPoints, angle + offset, reflected);
                const double area = primitive.shape.area
                    * std::abs(transform.determinant());
                if (std::isfinite(area) && area >= minimumArea) {
                    transforms.push_back(transform);
                    numericTransforms.push_back(gpuAffine(transform));
                }
            }
            if (transforms.empty()) {
                continue;
            }
            const int available = objective.evaluationLimit - objective.evaluations;
            if (transforms.size() > available) {
                transforms.resize(available);
                numericTransforms.resize(static_cast<size_t>(available));
            }
            std::vector<double> scores;
            if (!objective.gpuRanker->evaluateTransforms(
                    gpuGeometry(primitive.shape.contours), numericTransforms, &scores,
                    {4.0, 64.0, 4.0, 32.0, 4.0})) {
                return false;
            }
            objective.evaluations += static_cast<int>(scores.size());
            objective.gpuGroupTrials += static_cast<int>(scores.size());
            for (int index = 0; index < transforms.size(); ++index) {
                if (!std::isfinite(scores[index])) {
                    continue;
                }
                RankedReplacement replacement{
                    makePiece(primitive.shape, transforms[index]), scores[index]};
                const auto position = std::lower_bound(ranked.begin(), ranked.end(),
                    replacement.score, [](const RankedReplacement &entry, double score) {
                        return entry.score > score;
                    });
                ranked.insert(position, std::move(replacement));
                if (ranked.size() > kGpuGroupCandidates) {
                    ranked.removeLast();
                }
            }
        }
        const ReductionState before = reductionStateFor(support(*pieces), objective);
        bool replaced = false;
        for (const RankedReplacement &replacement : ranked) {
            QVector<Piece> trial = *pieces;
            trial[neighbor] = replacement.piece;
            trial.removeAt(fragment);
            const ReductionState after = reductionStateFor(support(trial), objective);
            if (gpuReplacementAllowed(before, after, objective)) {
                *pieces = std::move(trial);
                ++objective.gpuGroupReplacements;
                replaced = true;
                break;
            }
        }
        if (replaced) {
            if (!objective.gpuRanker->preparePlacementCoverage(gpuGeometry(*pieces))) {
                return false;
            }
            for (int &index : fragments) {
                if (index > fragment) {
                    --index;
                }
            }
        }
    }

    return true;
}

bool optimizeGpu(QVector<Piece> *pieces, const Objective &objective,
                 const QVector<catalog::Primitive> &primitives,
                 const std::function<bool()> &cancelled) {
    const QVector<Piece> original = *pieces;
    Context context;
    context.objective = &objective;
    const double minimumArea = kGpuMinimumFootprint
        * std::pow(objective.cornerAllowance * 2.0, 2.0);
    for (Piece &piece : *pieces) {
        if (piece.placement.area >= minimumArea
            || piece.placement.area <= catalog::kMinimumDeterminant) {
            continue;
        }
        Piece expanded;
        QTransform expansion;
        const double scale = std::sqrt(minimumArea / piece.placement.area);

        expansion.translate(piece.bounds.center().x(), piece.bounds.center().y());
        expansion.scale(scale, scale);
        expansion.translate(-piece.bounds.center().x(), -piece.bounds.center().y());
        expanded = makePiece(primitiveFor(piece.placement.shapeId, primitives),
            piece.placement.transform * expansion);
        if (catalog::subtract(expanded.polygons, objective.outer).isEmpty()) {
            piece = std::move(expanded);
            ++objective.gpuPipelineExpanded;
        }
    }
    if (!prepareGpuRanker(objective)) {
        *pieces = original;
        return false;
    }
    const int groupReserve = pieces->size() > kMinimumGpuOwnershipPieces
        ? std::min(3000, objective.evaluationLimit / 10) : 0;
    const int refinementLimit = objective.evaluationLimit - groupReserve;
    for (int pass = 0; pass < kGpuPipelinePasses && !stopped(cancelled); ++pass) {
        const gpu::AdditionWeights weights{4.0,
            pass < kGpuBalancedPasses ? 16.0 : kGpuRepairInnerWeight,
            4.0, 32.0};
        const bool repairPass = pass >= kGpuBalancedPasses;
        if (!objective.gpuRanker->preparePlacementCoverage(gpuGeometry(*pieces))) {
            objective.gpuRankingDisabled = true;
            ++objective.gpuRankerFallbacks;
            *pieces = original;
            return false;
        }
        for (int index = 0; index < pieces->size() && !stopped(cancelled); ++index) {
            const int remainingPieces = pieces->size() - index
                + (kGpuPipelinePasses - pass - 1) * pieces->size();
            const int pieceLimit = objective.evaluations
                + (refinementLimit - objective.evaluations)
                    / std::max(1, remainingPieces);
            if (pieceLimit <= objective.evaluations) {
                break;
            }
            const auto refined = refineGpu((*pieces)[index],
                primitiveFor((*pieces)[index].placement.shapeId, primitives),
                context, cancelled, pieceLimit, {}, nullptr, weights, true);
            if (!refined) {
                *pieces = original;
                return false;
            }
            (*pieces)[index] = *refined;
        }
        ++objective.gpuPipelinePasses;
        if (!repairPass && !pruneGpu(pieces, objective, cancelled)) {
            *pieces = original;
            return false;
        }
    }
    if (!replaceSmallGroupsGpu(pieces, objective, primitives, cancelled)) {
        *pieces = original;
        return false;
    }
    const auto before = reductionStateFor(support(original), objective);
    const auto afterCoverage = support(*pieces);
    const auto after = reductionStateFor(afterCoverage, objective);
    const double outsideEnvelope = catalog::area(
        catalog::subtract(afterCoverage, objective.outer));
    objective.gpuPipelineCandidate = {
        {QStringLiteral("count"), pieces->size()},
        {QStringLiteral("missingArea"), after.missingArea},
        {QStringLiteral("spillArea"), after.spillArea},
        {QStringLiteral("deepMissingArea"), catalog::area(after.deepMissing)},
        {QStringLiteral("outsideEnvelope"), outsideEnvelope},
        {QStringLiteral("boundary"), objective.boundary->diagnostics(after.metrics)}};
    const double beforeError = before.missingArea + before.spillArea;
    const double afterError = after.missingArea + after.spillArea;
    const double beforeDeep = catalog::area(before.deepMissing);
    const double afterDeep = catalog::area(after.deepMissing);
    const double beforeEnergy = objective.boundary->energy(before.metrics);
    const double afterEnergy = objective.boundary->energy(after.metrics);
    const double envelopeTolerance = std::pow(objective.cornerAllowance * 0.5, 2.0);
    const bool aggregateImprovement = !after.coverage.isEmpty()
        && outsideEnvelope <= envelopeTolerance + kScoreEpsilon
        && afterError <= std::max(objective.areaBudget, beforeError) + kScoreEpsilon
        && afterDeep <= beforeDeep + kScoreEpsilon
        && afterEnergy <= beforeEnergy + kScoreEpsilon
        && after.metrics.maximumCornerDistance
            <= std::max(objective.cornerAllowance,
                before.metrics.maximumCornerDistance)
                    + kGpuMaximumCornerRegression
        && after.metrics.maximumExcessTurn
            <= before.metrics.maximumExcessTurn + kGpuMaximumTurnRegression
        && after.metrics.cornerDefects <= before.metrics.cornerDefects
        && std::abs(after.metrics.components - objective.targetMetrics.components)
            <= std::abs(before.metrics.components - objective.targetMetrics.components)
        && std::abs(after.metrics.holes - objective.targetMetrics.holes)
            <= std::abs(before.metrics.holes - objective.targetMetrics.holes);
    if (!acceptable(afterCoverage, objective) && !aggregateImprovement) {
        *pieces = original;
        ++objective.gpuPipelineRestores;
    }

    return true;
}
#endif

QTransform boundsTransform(const PenPrimitive &shape, const QPolygonF &points,
                            double angle, bool reflected) {
    QTransform frame;
    frame.rotateRadians(angle);
    const QRectF bounds = frame.inverted().map(points).boundingRect();
    QTransform transform;
    transform.translate(bounds.center().x(), bounds.center().y());
    transform.scale((reflected ? -1.0 : 1.0) * bounds.width() / shape.bounds.width(),
                    bounds.height() / shape.bounds.height());
    transform.translate(-shape.bounds.center().x(), -shape.bounds.center().y());

    return transform * frame;
}

QTransform momentFrame(const Polygons &polygons) {
    double twiceArea = 0.0;
    double firstX = 0.0;
    double firstY = 0.0;
    double secondX = 0.0;
    double secondY = 0.0;
    double mixed = 0.0;
    const QPointF origin = catalog::painterPath(polygons).boundingRect().center();
    for (const auto &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF a = polygon[index] - origin;
            const QPointF b = polygon[(index + 1) % polygon.size()] - origin;
            const double cross = a.x() * b.y() - b.x() * a.y();
            twiceArea += cross;
            firstX += (a.x() + b.x()) * cross;
            firstY += (a.y() + b.y()) * cross;
            secondX += (a.x() * a.x() + a.x() * b.x() + b.x() * b.x()) * cross;
            secondY += (a.y() * a.y() + a.y() * b.y() + b.y() * b.y()) * cross;
            mixed += (2 * a.x() * a.y() + a.x() * b.y() + b.x() * a.y() + 2 * b.x() * b.y()) * cross;
        }
    }
    if (std::abs(twiceArea) < catalog::kMinimumDeterminant) {
        return {};
    }
    const double x = firstX / (3 * twiceArea);
    const double y = firstY / (3 * twiceArea);
    const double horizontal = std::sqrt(std::max(catalog::kMinimumDeterminant,
        secondX / (6 * twiceArea) - x * x));
    const double shear = (mixed / (12 * twiceArea) - x * y) / horizontal;
    const double vertical = std::sqrt(std::max(catalog::kMinimumDeterminant,
        secondY / (6 * twiceArea) - y * y - shear * shear));

    return QTransform(horizontal, shear, 0, vertical, x + origin.x(), y + origin.y());
}

QPointF radialAnchor(const Polygons &polygons, const QTransform &normalization) {
    QPointF result;
    double maximum = -1.0;
    for (const auto &polygon : polygons) {
        for (const auto &point : polygon) {
            const auto normalized = normalization.map(point);
            const double radius = QPointF::dotProduct(normalized, normalized);
            if (radius > maximum) {
                maximum = radius;
                result = normalized;
            }
        }
    }

    return result;
}

QVector<Piece> replacementSeeds(const Polygons &target, const Context &context,
                                 const QVector<catalog::Primitive> &primitives,
                                 const std::function<bool()> &cancelled, bool broad, bool *complete = nullptr) {
    QVector<std::pair<double, Piece>> ranked(primitives.size(), {-std::numeric_limits<double>::infinity(), {}});
    QVector<QTransform> sourceFrames;
    QVector<Piece> result;
    std::optional<Piece> recognized;
    QPolygonF points;
    if (complete) {
        *complete = false;
    }
    for (const auto &polygon : target) {
        points += polygon;
    }
    if (points.size() < 3 || stopped(cancelled)) {
        return result;
    }
    const auto targetFrame = momentFrame(target);
    const auto targetAnchor = radialAnchor(target, targetFrame.inverted());
    const int proposalLimit = context.objective->evaluations
        + static_cast<int>(static_cast<qint64>(context.objective->evaluationLimit - context.objective->evaluations) * 2 / 3);
    const auto stopProposals = [&] {
        return recognized.has_value() || stopped(cancelled) || context.objective->evaluations >= proposalLimit;
    };
    const auto consider = [&](int index, const QTransform &transform) {
        if (stopProposals()) {
            return;
        }
        auto piece = makePiece(primitives[index].shape, transform);
        const double score = gain(piece, context);
        if (broad && context.others.isEmpty() && acceptable(piece.polygons, *context.objective)) {
            recognized = piece;
        }
        if (score > ranked[index].first + kScoreEpsilon) {
            ranked[index] = {score, std::move(piece)};
        }
    };
    if (broad) {
        for (int index = 0; index < primitives.size() && !stopProposals(); ++index) {
            const auto &primitive = primitives[index];
            const auto sourceFrame = momentFrame(primitive.shape.contours).inverted();
            const auto sourceAnchor = radialAnchor(primitive.shape.contours, sourceFrame);
            sourceFrames.push_back(sourceFrame);
            for (bool reflected : {false, true}) {
                const double angle = std::atan2(targetAnchor.y(), targetAnchor.x())
                    - std::atan2(sourceAnchor.y(), reflected ? -sourceAnchor.x() : sourceAnchor.x());
                QTransform orientation;
                orientation.rotateRadians(angle);
                orientation.scale(reflected ? -1.0 : 1.0, 1.0);
                consider(index, sourceFrame * orientation * targetFrame);
            }
        }
    }
    const int orientations = broad ? kSeedOrientations : kPrimaryOrientations;
    for (int orientation = 0; orientation < orientations && !stopProposals(); ++orientation) {
        const double angle = orientation * std::numbers::pi * (broad ? 2.0 : 1.0) / orientations;
        for (int index = 0; index < primitives.size() && !stopProposals(); ++index) {
            for (bool reflected : {false, true}) {
                consider(index, boundsTransform(primitives[index].shape, points, angle, reflected));
                if (broad && index < sourceFrames.size()) {
                    QTransform frame;
                    frame.rotateRadians(angle);
                    frame.scale(reflected ? -1.0 : 1.0, 1.0);
                    consider(index, sourceFrames[index] * frame * targetFrame);
                }
            }
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &first, const auto &second) {
        return first.first > second.first;
    });
    if (recognized) {
        return {*recognized};
    }
    bool completed = !stopped(cancelled) && context.objective->evaluations < proposalLimit;
    const int retained = std::min(broad ? kRetainedSeeds : kPrimaryRetainedSeeds, static_cast<int>(ranked.size()));
    for (int index = 0; index < retained; ++index) {
        auto &entry = ranked[index];
        if (!std::isfinite(entry.first)) {
            continue;
        }
        const auto &primitive = primitiveFor(entry.second.placement.shapeId, primitives);
        const int localLimit = context.objective->evaluations
            + (context.objective->evaluationLimit - context.objective->evaluations) / (retained - index);
        const auto stopRefinement = [&] {
            return stopped(cancelled) || context.objective->evaluations >= localLimit;
        };
        result.push_back(refine(std::move(entry.second), primitive, context, stopRefinement));
        completed = completed && !stopRefinement();
    }
    if (complete) {
        *complete = completed;
    }

    return result;
}

std::optional<QPointF> residualCenter(const Polygons &component, const std::function<bool()> &cancelled) {
    struct Cell {
        QRectF bounds;
        double distance = 0.0;
        double upper = 0.0;
        int order = 0;
    };
    const auto path = catalog::painterPath(component);
    const auto compare = [](const Cell &first, const Cell &second) {
        return first.upper == second.upper ? first.order > second.order : first.upper < second.upper;
    };
    std::priority_queue<Cell, std::vector<Cell>, decltype(compare)> pending(compare);
    std::optional<QPointF> result;
    double best = 0.0;
    int order = 0;
    const auto append = [&](const QRectF &bounds) {
        const double distance = std::sqrt(boundaryDistanceSquared(component, bounds.center()))
            * (path.contains(bounds.center()) ? 1.0 : -1.0);
        pending.push({bounds, distance, distance + std::hypot(bounds.width(), bounds.height()) * 0.5, order++});
    };
    append(path.boundingRect());
    for (int trial = 0; trial < kResidualCenterTrials && !pending.empty() && !stopped(cancelled); ++trial) {
        const auto cell = pending.top();
        pending.pop();
        if (cell.distance > best) {
            best = cell.distance;
            result = cell.bounds.center();
        }
        if (cell.upper <= best + catalog::kVerificationClearance) {
            continue;
        }
        const auto &bounds = cell.bounds;
        if (bounds.width() >= bounds.height()) {
            append(QRectF(bounds.topLeft(), QSizeF(bounds.width() * 0.5, bounds.height())));
            append(QRectF(QPointF(bounds.center().x(), bounds.top()), QSizeF(bounds.width() * 0.5, bounds.height())));
        } else {
            append(QRectF(bounds.topLeft(), QSizeF(bounds.width(), bounds.height() * 0.5)));
            append(QRectF(QPointF(bounds.left(), bounds.center().y()), QSizeF(bounds.width(), bounds.height() * 0.5)));
        }
    }

    return result;
}

Piece stretchedToward(const Piece &piece, const PenPrimitive &shape, const QPointF &point,
                       int axis, double fraction, double padding) {
    bool invertible = false;
    const auto inverse = piece.placement.transform.inverted(&invertible);
    if (!invertible) {
        return piece;
    }
    const auto target = inverse.map(point);
    const auto bounds = shape.bounds;
    const double coordinate = axis == 0 ? target.x() : target.y();
    const double center = axis == 0 ? bounds.center().x() : bounds.center().y();
    const double extent = axis == 0 ? bounds.width() : bounds.height();
    const double direction = coordinate >= center ? 1.0 : -1.0;
    const double worldScale = axis == 0 ? std::hypot(piece.placement.transform.m11(), piece.placement.transform.m12())
        : std::hypot(piece.placement.transform.m21(), piece.placement.transform.m22());
    if (extent <= 0.0 || worldScale <= catalog::kMinimumDeterminant) {
        return piece;
    }
    const double growth = std::max(0.0, std::abs(coordinate - center) - extent * 0.5)
        + padding / worldScale;
    const double factor = 1.0 + fraction * growth / extent;
    const auto anchor = axis == 0 ? QPointF(center - direction * extent * 0.5, bounds.center().y())
        : QPointF(bounds.center().x(), center - direction * extent * 0.5);
    QTransform stretch;
    stretch.translate(anchor.x(), anchor.y());
    stretch.scale(axis == 0 ? factor : 1.0, axis == 1 ? factor : 1.0);
    stretch.translate(-anchor.x(), -anchor.y());

    return makePiece(shape, stretch * piece.placement.transform);
}

bool repairQualityAllowed(const ReductionState &after, const ReductionState &before,
                          const Objective &objective) {
    return coverageMoveAllowed(after, before, objective)
        && after.missingArea <= before.missingArea + kScoreEpsilon
        && after.missingArea + after.spillArea
            <= std::max(objective.areaBudget, before.missingArea + before.spillArea) + kScoreEpsilon
        && std::abs(after.metrics.holes - objective.targetMetrics.holes)
            <= std::abs(before.metrics.holes - objective.targetMetrics.holes)
        && objective.boundary->energy(after.metrics)
            <= std::max(objective.qualityLimit, objective.boundary->energy(before.metrics)) + kScoreEpsilon;
}

QPointF repairAnchor(const Piece &piece, const QPointF &gap, const Objective &objective) {
    QPointF result = piece.bounds.center();
    double best = -1.0;
    for (const auto &polygon : piece.polygons) {
        const int stride = std::max(1, static_cast<int>(polygon.size()) / 16);
        for (int index = 0; index < polygon.size(); index += stride) {
            const auto &point = polygon[index];
            const auto distance = QLineF(point, gap).length();
            if (distance > best && objective.boundary->reference(point).distance <= objective.allowance) {
                best = distance;
                result = point;
            }
        }
    }

    return result;
}

bool repairJointNeighborhood(QVector<Piece> *pieces, const QVector<std::pair<double, int>> &neighbors,
    const Polygons &component, const QPointF &center, const ReductionState &before,
    const Objective &objective, const QVector<catalog::Primitive> &primitives,
    int evaluationLimit, const std::function<bool()> &cancelled) {
    struct Move {
        Piece piece;
        double gain = 0.0;
    };
    const double componentArea = catalog::area(component);
    const int count = std::min(kRepairNeighbors, static_cast<int>(neighbors.size()));
    QVector<QVector<Move>> moves(count);
    QVector<Piece> best;
    double bestGain = kScoreEpsilon;
    const auto stop = [&] {
        return stopped(cancelled) || objective.evaluations >= evaluationLimit;
    };
    const auto charge = [&] {
        ++objective.evaluations;
        ++objective.jointRepairTrials;
        if (objective.workProgress && objective.evaluations % kWorkReportInterval == 0) {
            objective.workProgress(objective.evaluations);
        }
    };
    const auto consider = [&](const QVector<int> &members, const QVector<Piece> &proposed, const Context &context) {
        Polygons polygons;
        for (const auto &piece : proposed) {
            polygons += piece.polygons;
        }
        const auto addition = catalog::unite(polygons);
        const double gained = catalog::area(catalog::intersect(addition, component));
        if (gained <= bestGain || gained < componentArea * kJointRepairMinimumFraction) {
            return;
        }
        ++objective.globalMoveChecks;
        const auto after = reductionStateFor(completeCoverage(context, addition), objective);
        if (after.missingArea >= before.missingArea - kScoreEpsilon
            || !repairQualityAllowed(after, before, objective)) {
            return;
        }
        best = *pieces;
        for (int index = 0; index < members.size(); ++index) {
            best[members[index]] = proposed[index];
        }
        bestGain = gained;
    };
    const int singleLimit = objective.evaluations + (evaluationLimit - objective.evaluations) / 2;
    for (int neighbor = 0; neighbor < count && !stop(); ++neighbor) {
        const int member = neighbors[neighbor].second;
        const auto &original = (*pieces)[member];
        const auto &shape = primitiveFor(original.placement.shapeId, primitives);
        const auto anchor = repairAnchor(original, center, objective);
        const auto context = localContextFor(*pieces, {member}, objective);
        const double extent = std::max(original.bounds.width(), original.bounds.height());
        const double distance = std::sqrt(neighbors[neighbor].first) + objective.inwardAllowance;
        const double step = std::clamp(distance / std::max(extent, objective.inwardAllowance), 0.002, 0.08);
        const int limit = objective.evaluations + std::max(0, singleLimit - objective.evaluations) / (count - neighbor);
        for (int variant = 0; variant < 16 && objective.evaluations < limit && !stop(); ++variant) {
            charge();
            Piece candidate;
            if (variant < 4) {
                candidate = stretchedToward(original, shape, center, variant % 2,
                    variant < 2 ? 1.0 : 0.5, objective.inwardAllowance);
            } else {
                const int parameter = (variant - 4) / 2;
                const double amount = ((variant - 4) % 2 ? 1.0 : -1.0) * step;
                QTransform movement;
                movement.translate(anchor.x(), anchor.y());
                if (parameter == 0) {
                    movement.translate(amount * extent, 0.0);
                } else if (parameter == 1) {
                    movement.translate(0.0, amount * extent);
                } else if (parameter == 2) {
                    movement.scale(std::exp(amount), 1.0);
                } else if (parameter == 3) {
                    movement.scale(1.0, std::exp(amount));
                } else if (parameter == 4) {
                    movement.rotateRadians(amount);
                } else {
                    movement.shear(amount, 0.0);
                }
                movement.translate(-anchor.x(), -anchor.y());
                candidate = makePiece(shape, original.placement.transform * movement);
            }
            if (!catalog::subtract(candidate.polygons, objective.outer).isEmpty()) {
                continue;
            }
            const double gained = catalog::area(catalog::intersect(candidate.polygons, component));
            if (gained <= kScoreEpsilon) {
                continue;
            }
            moves[neighbor].push_back({candidate, gained});
            consider({member}, {candidate}, context);
        }
        std::stable_sort(moves[neighbor].begin(), moves[neighbor].end(), [](const auto &first, const auto &second) {
            return first.gain > second.gain;
        });
        if (moves[neighbor].size() > kRepairRetainedMoves) {
            moves[neighbor].resize(kRepairRetainedMoves);
        }
    }
    for (int left = 0; left < count && !stop(); ++left) {
        for (int right = left + 1; right < count && !stop(); ++right) {
            const QVector<int> members{neighbors[left].second, neighbors[right].second};
            const auto context = localContextFor(*pieces, members, objective);
            for (const auto &first : moves[left]) {
                for (const auto &second : moves[right]) {
                    if (stop()) {
                        break;
                    }
                    charge();
                    consider(members, {first.piece, second.piece}, context);
                }
            }
        }
    }
    if (best.isEmpty() || stopped(cancelled)) {
        return false;
    }
    *pieces = std::move(best);
    ++objective.jointRepairs;

    return true;
}

void repairResiduals(QVector<Piece> *pieces, const Objective &objective,
                     const QVector<catalog::Primitive> &primitives, int shapeBudget,
                     int evaluationLimit, const std::function<bool()> &cancelled) {
    const auto stopRepair = [&] {
        return stopped(cancelled) || objective.evaluations >= evaluationLimit;
    };
    const auto connectorEvent = [&](const QString &reason) {
        objective.connectorDiagnostics[reason] = objective.connectorDiagnostics.value(reason).toInt() + 1;
    };
    while (!stopRepair()) {
        const auto before = reductionStateFor(support(*pieces), objective);
        const auto residual = repairResidual(before, objective.target, objective.inner, objective.targetMetrics);
        auto components = residual;
        std::stable_sort(components.begin(), components.end(), [](const auto &first, const auto &second) {
            return catalog::signedArea(first) > catalog::signedArea(second);
        });
        bool inserted = false;
        for (const auto &outline : components) {
            if (catalog::signedArea(outline) <= kScoreEpsilon || stopRepair()) {
                continue;
            }
            auto component = catalog::intersect({outline}, residual);
            const auto center = residualCenter(component, stopRepair);
            if (!center) {
                continue;
            }
            const int componentLimit = objective.evaluations
                + std::min(kResidualTrialsPerComponent, evaluationLimit - objective.evaluations);
            QVector<std::pair<double, int>> neighbors;
            for (int index = 0; index < pieces->size(); ++index) {
                neighbors.push_back({boundaryDistanceSquared((*pieces)[index].polygons, *center), index});
            }
            std::sort(neighbors.begin(), neighbors.end());
            if (!neighbors.isEmpty()) {
                const double scale = objective.cornerAllowance * 2.0;
                const double radius = std::clamp(std::sqrt((*pieces)[neighbors.front().second].placement.area) * 0.25,
                    scale * 8.0, scale * 64.0);
                const auto bounds = catalog::painterPath(component).boundingRect().adjusted(-radius, -radius, radius, radius);
                const auto clustered = catalog::intersect(residual, {QPolygonF(bounds)});
                if (catalog::area(clustered) > catalog::area(component) + kScoreEpsilon) {
                    component = clustered;
                    ++objective.repairClusters;
                }
            }
            const int jointLimit = objective.evaluations + (componentLimit - objective.evaluations) / 3;
            if (repairJointNeighborhood(pieces, neighbors, component, *center, before,
                    objective, primitives, jointLimit, stopRepair)) {
                inserted = true;
                break;
            }
            const auto gapBounds = catalog::painterPath(component).boundingRect();
            const double clearance = std::sqrt(boundaryDistanceSquared(objective.target, *center));
            const double side = std::sqrt(2.0) * clearance * 0.99;
            const double padding = objective.inwardAllowance * 2.0;
            const QRectF localBounds = gapBounds.adjusted(-padding, -padding, padding, padding);
            const QRectF safeBounds(*center - QPointF(side, side) * 0.5, QSizeF(side, side));
            const BoundaryModel currentBoundary(catalog::intersect(
                visibleSupport(before.coverage, objective), objective.visibleTarget), objective.inwardAllowance);
            const QPointF connection = currentBoundary.reference(*center).point;
            const QPointF delta = connection - *center;
            const double bridgeAngle = std::atan2(delta.y(), delta.x());
            const QPointF tangent(std::cos(bridgeAngle), std::sin(bridgeAngle));
            const QPointF normal(-tangent.y(), tangent.x());
            const double bridgeRadius = std::max(catalog::kVerificationClearance * 4.0,
                std::min(padding, clearance * 0.5));
            const QPointF start = *center - tangent * padding;
            const QPointF end = connection + tangent * padding;
            const std::array<QPolygonF, 3> frames{
                QPolygonF({localBounds.topLeft(), localBounds.topRight(), localBounds.bottomRight(), localBounds.bottomLeft()}),
                QPolygonF({safeBounds.topLeft(), safeBounds.topRight(), safeBounds.bottomRight(), safeBounds.bottomLeft()}),
                QPolygonF({start - normal * bridgeRadius, end - normal * bridgeRadius,
                    end + normal * bridgeRadius, start + normal * bridgeRadius})};
            const double componentArea = catalog::area(component);
            QVector<Piece> best;
            Polygons replacementOthers;
            QVector<int> bestReplacements;
            bool bestConnector = false;
            std::optional<Piece> connector;
            const auto square = std::find_if(primitives.cbegin(), primitives.cend(), [](const auto &primitive) {
                return primitive.shape.shapeId == 101;
            });
            if (square != primitives.cend() && pieces->size() < shapeBudget) {
                for (double width : {1.0, 0.5, 0.125}) {
                    if (stopRepair() || objective.evaluations >= componentLimit) {
                        break;
                    }
                    ++objective.evaluations;
                    connectorEvent(QStringLiteral("attempts"));
                    QTransform narrowing;
                    narrowing.translate((start.x() + end.x()) * 0.5, (start.y() + end.y()) * 0.5);
                    narrowing.rotateRadians(bridgeAngle);
                    narrowing.scale(1.0, width);
                    narrowing.rotateRadians(-bridgeAngle);
                    narrowing.translate(-(start.x() + end.x()) * 0.5, -(start.y() + end.y()) * 0.5);
                    auto bridge = makePiece(square->shape, boundsTransform(square->shape, frames[2], bridgeAngle, false) * narrowing);
                    if (!catalog::subtract(bridge.polygons, objective.outer).isEmpty()) {
                        connectorEvent(QStringLiteral("outer envelope"));
                    } else if (catalog::intersect(bridge.polygons, before.coverage).isEmpty()) {
                        connectorEvent(QStringLiteral("no support overlap"));
                    } else {
                        connector = std::move(bridge);
                        connectorEvent(QStringLiteral("legal"));
                        break;
                    }
                }
            } else {
                connectorEvent(square == primitives.cend() ? QStringLiteral("square unavailable") : QStringLiteral("shape limit"));
            }
            double bestGain = kScoreEpsilon;
            double bestScore = 0.0;
            double bestMissingGain = 0.0;
            bool bestConnected = false;
            const auto consider = [&](const PenPrimitive &shape, const QTransform &transform, const QVector<int> &replacements = {}) {
                if (stopRepair() || objective.evaluations >= componentLimit) {
                    return;
                }
                if (pieces->size() + 1 - replacements.size() > shapeBudget) {
                    return;
                }
                ++objective.evaluations;
                if (objective.workProgress && objective.evaluations % kWorkReportInterval == 0) {
                    objective.workProgress(objective.evaluations);
                }
                auto piece = makePiece(shape, transform);
                if (!catalog::subtract(piece.polygons, objective.outer).isEmpty()) {
                    return;
                }
                QVector<Piece> proposal{piece};
                auto proposed = piece.polygons;
                const auto &unchanged = replacements.isEmpty() ? before.coverage : replacementOthers;
                const bool overlaps = !catalog::intersect(proposed, unchanged).isEmpty();
                if (!overlaps && connector) {
                    if (pieces->size() + 2 - replacements.size() > shapeBudget) {
                        connectorEvent(QStringLiteral("compound shape limit"));
                    } else if (catalog::intersect(proposed, connector->polygons).isEmpty()) {
                        connectorEvent(QStringLiteral("no body overlap"));
                    } else if (catalog::intersect(connector->polygons, unchanged).isEmpty()) {
                        connectorEvent(QStringLiteral("removed support"));
                    } else {
                        proposal.push_back(*connector);
                        proposed = catalog::unite(proposed + connector->polygons);
                        connectorEvent(QStringLiteral("compound trials"));
                    }
                }
                const double gained = catalog::area(catalog::intersect(proposed, residual));
                if (gained <= kScoreEpsilon && replacements.size() <= 1) {
                    if (proposal.size() > 1) {
                        connectorEvent(QStringLiteral("compound no residual gain"));
                    }
                    return;
                }
                const int added = proposal.size() - replacements.size();
                const double shapeCost = kRepairPlacementCost * std::pow(objective.cornerAllowance * 2.0, 2.0);
                const double placementCost = 1.0 + std::max(0, added);
                const double upperScore = gained * kRepairConnectionBonus / placementCost - added * shapeCost;
                if (!best.isEmpty() && upperScore < bestScore - kScoreEpsilon) {
                    if (proposal.size() > 1) {
                        connectorEvent(QStringLiteral("compound score bound"));
                    }
                    return;
                }
                const auto after = reductionStateFor(catalog::unite(unchanged + proposed), objective);
                const double missingGain = before.missingArea - after.missingArea;
                const bool connected = std::abs(after.metrics.components - objective.targetMetrics.components)
                    <= std::abs(before.metrics.components - objective.targetMetrics.components);
                if (!connected || std::abs(after.metrics.holes - objective.targetMetrics.holes)
                    > std::abs(before.metrics.holes - objective.targetMetrics.holes)
                    || missingGain < -kScoreEpsilon || after.missingArea + after.spillArea
                        > std::max(objective.areaBudget, before.missingArea + before.spillArea) + kScoreEpsilon
                    || !repairQualityAllowed(after, before, objective)) {
                    if (proposal.size() > 1) {
                        connectorEvent(QStringLiteral("compound coverage or topology"));
                    }
                    return;
                }
                const bool topologyImproved = after.metrics.components < before.metrics.components
                    || catalog::area(after.observedHoles) < catalog::area(before.observedHoles) - kScoreEpsilon;
                const double deepGain = catalog::area(before.deepMissing) - catalog::area(after.deepMissing);
                const double visibleGain = missingGain + kMissingWeight * deepGain
                    + catalog::area(before.observedHoles) - catalog::area(after.observedHoles);
                if (added > 0 && visibleGain <= added * shapeCost && deepGain <= kScoreEpsilon) {
                    ++objective.repairBenefitRejected;
                    return;
                }
                const double score = gained * (topologyImproved ? kRepairConnectionBonus : 1.0) / placementCost - added * shapeCost;
                const bool improves = best.isEmpty() || score > bestScore + kScoreEpsilon
                    || (std::abs(score - bestScore) <= kScoreEpsilon && missingGain > bestMissingGain);
                if (improves) {
                    if (bestConnector) {
                        connectorEvent(QStringLiteral("compound superseded"));
                    }
                    if (proposal.size() > 1) {
                        connectorEvent(QStringLiteral("compound finalists"));
                    }
                    bestGain = gained;
                    bestScore = score;
                    bestMissingGain = missingGain;
                    bestConnected = connected;
                    bestReplacements = replacements;
                    bestConnector = proposal.size() > 1;
                    best = std::move(proposal);
                } else if (proposal.size() > 1) {
                    connectorEvent(QStringLiteral("compound lower score"));
                }
            };
            const int neighborhoodLimit = objective.evaluations + (componentLimit - objective.evaluations) / 3;
            const int groupLimit = objective.evaluations + (neighborhoodLimit - objective.evaluations) / 2;
            QVector<int> members;
            for (int neighbor = 0; neighbor < std::min(kRepairNeighbors, static_cast<int>(neighbors.size())); ++neighbor) {
                members.push_back(neighbors[neighbor].second);
                if (members.size() < 2) {
                    continue;
                }
                replacementOthers = support(*pieces, members);
                Polygons removed;
                for (int member : members) {
                    removed += (*pieces)[member].polygons;
                }
                const auto necessary = catalog::unite(catalog::subtract(catalog::unite(removed), replacementOthers) + component);
                const int groupsRemaining = std::min(kRepairNeighbors, static_cast<int>(neighbors.size())) - neighbor;
                const int currentGroupLimit = objective.evaluations
                    + std::max(0, groupLimit - objective.evaluations) / groupsRemaining;
                QPolygonF anchors;
                for (const auto &polygon : necessary) {
                    anchors += polygon;
                }
                for (const auto &primitive : primitives) {
                    if (stopRepair() || objective.evaluations >= currentGroupLimit) {
                        break;
                    }
                    const double angle = std::atan2((*pieces)[members.front()].placement.transform.m12(),
                        (*pieces)[members.front()].placement.transform.m11());
                    for (double rotation : {angle, 0.0}) {
                        if (stopRepair() || objective.evaluations >= currentGroupLimit) {
                            break;
                        }
                        consider(primitive.shape, boundsTransform(primitive.shape, anchors, rotation, false), members);
                    }
                }
            }
            for (int neighbor = 0; neighbor < std::min(kRepairNeighbors, static_cast<int>(neighbors.size()))
                && !stopRepair() && objective.evaluations < neighborhoodLimit; ++neighbor) {
                const int index = neighbors[neighbor].second;
                const auto &original = (*pieces)[index];
                const int neighborLimit = objective.evaluations + (neighborhoodLimit - objective.evaluations)
                    / (std::min(kRepairNeighbors, static_cast<int>(neighbors.size())) - neighbor);
                replacementOthers = support(*pieces, {index});
                QPolygonF anchors;
                for (const auto &polygon : original.polygons) {
                    anchors += polygon;
                }
                for (const auto &polygon : component) {
                    anchors += polygon;
                }
                const double angle = std::atan2(original.placement.transform.m12(), original.placement.transform.m11());
                QVector<const PenPrimitive *> shapes;
                const auto same = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &primitive) {
                    return primitive.shape.shapeId == original.placement.shapeId;
                });
                if (same != primitives.cend()) {
                    shapes.push_back(&same->shape);
                }
                for (const auto &primitive : primitives) {
                    if (primitive.shape.shapeId != original.placement.shapeId) {
                        shapes.push_back(&primitive.shape);
                    }
                }
                for (const auto *shape : shapes) {
                    for (double rotation : {angle, 0.0, angle + std::numbers::pi / 2.0}) {
                        if (stopRepair() || objective.evaluations >= neighborLimit) {
                            break;
                        }
                        consider(*shape, boundsTransform(*shape, anchors, rotation, false), {index});
                    }
                    if (stopRepair() || objective.evaluations >= neighborLimit) {
                        break;
                    }
                }
            }
            for (double scale : {1.0, 2.0, 0.5}) {
                if (bestConnected && bestGain >= componentArea - kScoreEpsilon) {
                    break;
                }
                for (int orientation = 0; orientation < 4; ++orientation) {
                    if (bestConnected && bestGain >= componentArea - kScoreEpsilon) {
                        break;
                    }
                    for (const auto &primitive : primitives) {
                        for (int frame = 0; frame < static_cast<int>(frames.size()); ++frame) {
                            const auto &anchors = frames[frame];
                            const auto bounds = anchors.boundingRect();
                            QTransform sizing;
                            sizing.translate(bounds.center().x(), bounds.center().y());
                            sizing.scale(scale, scale);
                            sizing.translate(-bounds.center().x(), -bounds.center().y());
                            consider(primitive.shape, boundsTransform(primitive.shape, anchors,
                                orientation * std::numbers::pi / 4.0 + (frame == 2 ? bridgeAngle : 0.0), false) * sizing);
                            if (stopRepair() || objective.evaluations >= componentLimit) {
                                break;
                            }
                        }
                        if (stopRepair() || objective.evaluations >= componentLimit) {
                            break;
                        }
                    }
                }
            }
            if (!best.isEmpty()) {
                objective.residualInsertions += std::max(0, static_cast<int>(best.size() - bestReplacements.size()));
                objective.residualConnectors += bestConnector;
                if (bestConnector) {
                    connectorEvent(QStringLiteral("accepted"));
                }
                if (!bestReplacements.isEmpty()) {
                    std::sort(bestReplacements.begin(), bestReplacements.end(), std::greater<int>());
                    for (int member : bestReplacements) {
                        pieces->removeAt(member);
                    }
                    ++objective.neighborhoodRepairs;
                    objective.repairGroupReplacements += bestReplacements.size() > best.size();
                }
                if (catalog::intersect(component, before.deepMissing).isEmpty()) {
                    ++objective.topologyRepairs;
                }
                *pieces += best;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            break;
        }
    }
}

void consolidateRepairPatches(QVector<Piece> *pieces, const Objective &objective,
    const QVector<catalog::Primitive> &primitives, int evaluationLimit,
    const std::function<bool()> &cancelled) {
    const auto circle = std::find_if(primitives.cbegin(), primitives.cend(), [](const auto &primitive) {
        return primitive.shape.shapeId == 102;
    });
    if (circle == primitives.cend() || pieces->size() < 4) {
        return;
    }
    const double radius = kBroadRepairRadius * objective.cornerAllowance * 2.0;
    const double footprintLimit = kBroadRepairFootprint * std::pow(objective.cornerAllowance * 2.0, 2.0);
    QVector<bool> visited(pieces->size(), false);
    int clusters = 0;
    for (int anchor = pieces->size() - 1; anchor >= 0 && clusters < kBroadRepairClusters
        && !stopped(cancelled) && objective.evaluations < evaluationLimit; --anchor) {
        if (visited[anchor] || (*pieces)[anchor].placement.area > footprintLimit) {
            continue;
        }
        QVector<std::pair<double, int>> nearby;
        for (int index = 0; index < pieces->size(); ++index) {
            if ((*pieces)[index].placement.area > footprintLimit) {
                continue;
            }
            const QPointF delta = (*pieces)[index].bounds.center() - (*pieces)[anchor].bounds.center();
            const double distanceSquared = QPointF::dotProduct(delta, delta);
            if (distanceSquared <= radius * radius) {
                nearby.push_back({distanceSquared, index});
            }
        }
        std::sort(nearby.begin(), nearby.end());
        if (nearby.size() < 4) {
            continue;
        }
        nearby.resize(std::min(kBroadRepairMembers, static_cast<int>(nearby.size())));
        ++clusters;
        for (const auto &[distance, index] : nearby) {
            visited[index] = true;
        }
        const auto before = reductionStateFor(support(*pieces), objective);
        QVector<Piece> best;
        int bestSaving = 0;
        double bestError = std::numeric_limits<double>::infinity();
        QVector<int> groupSizes{4, 8, static_cast<int>(nearby.size())};
        std::sort(groupSizes.begin(), groupSizes.end());
        groupSizes.erase(std::unique(groupSizes.begin(), groupSizes.end()), groupSizes.end());
        for (int size : groupSizes) {
            if (nearby.size() < size || stopped(cancelled) || objective.evaluations >= evaluationLimit) {
                continue;
            }
            QVector<int> members;
            Polygons removed;
            QPolygonF anchors;
            for (int index = 0; index < size; ++index) {
                members.push_back(nearby[index].second);
                removed += (*pieces)[nearby[index].second].polygons;
                for (const auto &polygon : (*pieces)[nearby[index].second].polygons) {
                    anchors += polygon;
                }
            }
            const auto others = support(*pieces, members);
            const auto necessary = catalog::subtract(catalog::unite(removed), others);
            const QRectF frame = anchors.boundingRect();
            QVector<std::pair<double, Piece>> ranked;
            for (double angle : {0.0, std::numbers::pi / 12.0}) {
                for (double factor : {1.0, 2.0, 3.0}) {
                    for (double shift : {-0.25, 0.0, 0.25}) {
                        if (stopped(cancelled) || objective.evaluations >= evaluationLimit) {
                            break;
                        }
                        ++objective.evaluations;
                        ++objective.broadRepairTrials;
                        QTransform adjustment;
                        adjustment.translate(frame.center().x() + shift * frame.width(), frame.center().y());
                        adjustment.scale(factor, factor);
                        adjustment.translate(-frame.center().x(), -frame.center().y());
                        auto candidate = makePiece(circle->shape,
                            boundsTransform(circle->shape, anchors, angle, false) * adjustment);
                        if (!catalog::subtract(candidate.polygons, objective.outer).isEmpty()) {
                            continue;
                        }
                        const double uncovered = catalog::area(catalog::subtract(necessary, candidate.polygons));
                        const double spill = catalog::area(catalog::subtract(candidate.polygons, objective.spillFree));
                        ranked.push_back({uncovered * kMissingWeight + spill, std::move(candidate)});
                    }
                }
            }
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) {
                return left.first < right.first;
            });
            for (int candidateIndex = 0; candidateIndex < std::min(4, static_cast<int>(ranked.size())); ++candidateIndex) {
                if (stopped(cancelled)) {
                    break;
                }
                const auto &candidate = ranked[candidateIndex].second;
                const auto after = reductionStateFor(catalog::unite(others + candidate.polygons), objective);
                if (!repairQualityAllowed(after, before, objective)) {
                    continue;
                }
                const double areaError = after.missingArea + after.spillArea;
                const int saving = size - 1;
                if (saving > bestSaving || (saving == bestSaving && areaError < bestError - kScoreEpsilon)) {
                    best = *pieces;
                    std::sort(members.begin(), members.end(), std::greater<int>());
                    for (int member : members) {
                        best.removeAt(member);
                    }
                    best.push_back(candidate);
                    bestSaving = saving;
                    bestError = areaError;
                }
            }
        }
        if (!best.isEmpty()) {
            *pieces = std::move(best);
            ++objective.broadRepairReplacements;
            return;
        }
    }
}

QVector<QVector<int>> exposedNeighbors(const QVector<Piece> &pieces, const Objective &objective) {
    QVector<QVector<int>> neighbors(pieces.size());
    for (const auto &polygon : objective.boundary->observationSupport(
             visibleSupport(support(pieces), objective))) {
        QVector<int> owners;
        for (int edge = 0; edge < polygon.size(); ++edge) {
            const QPointF point = (polygon[edge] + polygon[(edge + 1) % polygon.size()]) * 0.5;
            int owner = -1;
            double nearest = std::numeric_limits<double>::infinity();
            for (int piece = 0; piece < pieces.size(); ++piece) {
                if (!pieces[piece].bounds.adjusted(-objective.inwardAllowance, -objective.inwardAllowance,
                    objective.inwardAllowance, objective.inwardAllowance).contains(point)) {
                    continue;
                }
                for (const auto &contour : pieces[piece].polygons) {
                    for (int segment = 0; segment < contour.size(); ++segment) {
                        const auto delta = contour[(segment + 1) % contour.size()] - contour[segment];
                        const double length = QPointF::dotProduct(delta, delta);
                        const double parameter = length > catalog::kMinimumDeterminant
                            ? std::clamp(QPointF::dotProduct(point - contour[segment], delta) / length, 0.0, 1.0) : 0.0;
                        const auto displacement = contour[segment] + delta * parameter - point;
                        const double distance = QPointF::dotProduct(displacement, displacement);
                        if (distance < nearest) {
                            nearest = distance;
                            owner = piece;
                        }
                    }
                }
            }
            if (owner >= 0 && (owners.isEmpty() || owners.back() != owner)) {
                owners.push_back(owner);
            }
        }
        for (int index = 0; index < owners.size(); ++index) {
            const int first = owners[index];
            const int second = owners[(index + 1) % owners.size()];
            if (first != second && !neighbors[first].contains(second)) {
                neighbors[first].push_back(second);
                neighbors[second].push_back(first);
            }
        }
    }
    for (auto &adjacent : neighbors) {
        std::sort(adjacent.begin(), adjacent.end());
    }

    return neighbors;
}

QVector<int> boundaryDistances(const QVector<QVector<int>> &neighbors, int source) {
    QVector<int> distances(neighbors.size(), neighbors.size());
    QVector<int> queue = {source};
    distances[source] = 0;
    for (int index = 0; index < queue.size(); ++index) {
        for (int other : neighbors[queue[index]]) {
            if (distances[other] == neighbors.size()) {
                distances[other] = distances[queue[index]] + 1;
                queue.push_back(other);
            }
        }
    }

    return distances;
}

void fitBoundaryPairs(QVector<Piece> *pieces, const Objective &objective,
                      const QVector<catalog::Primitive> &primitives, int evaluationLimit,
                      const std::function<bool()> &cancelled) {
    const auto neighbors = exposedNeighbors(*pieces, objective);
    const auto weights = refitWeights(*pieces, objective);
    QVector<std::pair<int, int>> pairs;
    for (int first = 0; first < neighbors.size(); ++first) {
        for (int second : neighbors[first]) {
            if (first < second) {
                pairs.push_back({first, second});
            }
        }
    }
    std::stable_sort(pairs.begin(), pairs.end(), [&](const auto &first, const auto &second) {
        return weights[first.first] + weights[first.second] > weights[second.first] + weights[second.second];
    });
    for (int index = 0; index < pairs.size() && !stopped(cancelled) && objective.evaluations < evaluationLimit; ++index) {
        const auto [first, second] = pairs[index];
        const auto originalLeft = (*pieces)[first];
        const auto originalRight = (*pieces)[second];
        const auto context = localContextFor(*pieces, {first, second}, objective);
        const int limit = objective.evaluations + std::min(kJointTrialsPerPair,
            (evaluationLimit - objective.evaluations) / std::min(8, static_cast<int>(pairs.size()) - index));
        const auto group = [](const Piece &left, const Piece &right) {
            Piece result;
            result.polygons = catalog::unite(left.polygons + right.polygons);
            result.bounds = left.bounds.united(right.bounds);

            return result;
        };
        if (limit <= objective.evaluations) {
            continue;
        }
        auto current = group((*pieces)[first], (*pieces)[second]);
        double best = qualityGain(current, context);
        auto before = reductionStateFor(completeCoverage(context, current.polygons), objective);
        const double areaLimit = std::max(objective.areaBudget, before.missingArea + before.spillArea);
        const int previousMoves = objective.jointMoves;
        for (int level = 0; level < 4 && objective.evaluations < limit && !stopped(cancelled); ++level) {
            const double extent = std::max(current.bounds.width(), current.bounds.height());
            const double step = std::ldexp(std::min(objective.inwardAllowance * 2.0, extent * kInitialStepFraction), -level);
            for (int parameter = 0; parameter < 6 && objective.evaluations < limit && !stopped(cancelled); ++parameter) {
                for (double firstSign : {-1.0, 1.0}) {
                    for (double secondSign : {-1.0, 1.0}) {
                        if (objective.evaluations >= limit || stopped(cancelled)) {
                            break;
                        }
                        const double amount = parameter < 2 ? step : step / std::max(extent, objective.inwardAllowance);
                        auto left = makePiece(primitiveFor((*pieces)[first].placement.shapeId, primitives),
                            movedTransform((*pieces)[first], parameter, firstSign * amount));
                        auto right = makePiece(primitiveFor((*pieces)[second].placement.shapeId, primitives),
                            movedTransform((*pieces)[second], parameter, secondSign * amount));
                        auto candidate = group(left, right);
                        const double score = qualityGain(candidate, context, best + kScoreEpsilon);
                        if (score <= best + kScoreEpsilon) {
                            continue;
                        }
                        ++objective.globalMoveChecks;
                        auto after = reductionStateFor(completeCoverage(context, candidate.polygons), objective);
                        if (!coverageMoveAllowed(after, before, objective)
                            || std::abs(after.metrics.holes - objective.targetMetrics.holes)
                                > std::abs(before.metrics.holes - objective.targetMetrics.holes)) {
                            ++objective.coverageRejected;
                            continue;
                        }
                        (*pieces)[first] = std::move(left);
                        (*pieces)[second] = std::move(right);
                        current = std::move(candidate);
                        before = std::move(after);
                        best = score;
                        ++objective.jointMoves;
                    }
                }
            }
        }
        if (before.missingArea + before.spillArea > areaLimit + kScoreEpsilon) {
            (*pieces)[first] = originalLeft;
            (*pieces)[second] = originalRight;
            objective.jointMoves = previousMoves;
        }
    }
}

bool reuseCandidates(QVector<Piece> *pieces, const QVector<int> &excluded, const Polygons &required,
                       const Objective &objective, const QVector<catalog::Primitive> &primitives,
                       const ReductionContext &before, const std::function<bool()> &cancelled) {
    if (!objective.replacementCandidates || required.isEmpty()) {
        return false;
    }
    struct Choice {
        Polygons covered;
        int index = 0;
        double gain = 0.0;
    };
    const auto &pool = *objective.replacementCandidates;
    const auto bounds = catalog::painterPath(required).boundingRect();
    const double boundsArea = bounds.width() * bounds.height();
    QVector<std::pair<double, int>> ranked;
    QVector<Choice> choices;
    const auto advance = [&] {
        if (stopped(cancelled) || objective.evaluations >= objective.evaluationLimit) {
            return false;
        }
        ++objective.evaluations;
        if (objective.workProgress && objective.evaluations % kWorkReportInterval == 0) {
            objective.workProgress(objective.evaluations);
        }
        return true;
    };
    for (int index = 0; index < pool.size() && !stopped(cancelled); ++index) {
        const auto overlap = bounds.intersected(pool[index].bounds);
        if (!overlap.isEmpty()) {
            const double area = pool[index].bounds.width() * pool[index].bounds.height();
            ranked.push_back({-overlap.width() * overlap.height() / std::sqrt(std::max(area, boundsArea)), index});
        }
    }
    const int retained = std::min(kReuseCandidates, static_cast<int>(ranked.size()));
    std::partial_sort(ranked.begin(), ranked.begin() + retained, ranked.end());
    for (int position = 0; position < retained; ++position) {
        if (!advance()) {
            return false;
        }
        const int index = ranked[position].second;
        const auto available = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &primitive) {
            return primitive.shape.shapeId == pool[index].placement.shapeId;
        });
        if (available == primitives.cend()) {
            continue;
        }
        auto covered = catalog::intersect(required, pool[index].polygons);
        const double gained = catalog::area(covered);
        ++objective.reusedCandidates;
        if (gained > kScoreEpsilon) {
            choices.push_back({std::move(covered), index, gained});
        }
    }
    std::stable_sort(choices.begin(), choices.end(), [](const auto &first, const auto &second) {
        return first.gain > second.gain;
    });
    const auto accept = [&](const QVector<int> &indices) {
        if (indices.size() >= excluded.size() || !advance()) {
            return false;
        }
        QVector<Piece> trial;
        for (int index = 0; index < pieces->size(); ++index) {
            if (!excluded.contains(index)) {
                trial.push_back((*pieces)[index]);
            }
        }
        for (int index : indices) {
            const auto &candidate = pool[choices[index].index];
            trial.push_back(makePiece(primitiveFor(candidate.placement.shapeId, primitives), candidate.placement.transform));
        }
        ++objective.reuseChecks;
        if (reductionAllowed(support(trial), before, objective)) {
            *pieces = std::move(trial);
            ++objective.reusedMerges;
            objective.reusedPairs += indices.size() == 2;
            return true;
        }
        return false;
    };
    for (int first = 0; first < std::min(kReuseFirstChoices, static_cast<int>(choices.size())); ++first) {
        if (stopped(cancelled) || objective.evaluations >= objective.evaluationLimit) {
            return false;
        }
        if (accept({first})) {
            return true;
        }
        if (excluded.size() < 3) {
            continue;
        }
        QVector<std::pair<double, int>> partners;
        for (int second = 0; second < choices.size(); ++second) {
            if (second == first) {
                continue;
            }
            if (!advance()) {
                return false;
            }
            const double gained = catalog::area(catalog::subtract(choices[second].covered, choices[first].covered));
            if (gained > kScoreEpsilon) {
                partners.push_back({-gained, second});
            }
        }
        std::sort(partners.begin(), partners.end());
        for (int partner = 0; partner < std::min(kReuseSecondChoices, static_cast<int>(partners.size())); ++partner) {
            if (stopped(cancelled) || objective.evaluations >= objective.evaluationLimit) {
                return false;
            }
            if (accept({first, partners[partner].second})) {
                return true;
            }
        }
    }

    return false;
}

bool merge(QVector<Piece> *pieces, const Objective &objective,
            const QVector<catalog::Primitive> &primitives,
            const std::function<bool()> &cancelled, bool broad, bool exposed = false) {
    const auto adjacency = exposed ? exposedNeighbors(*pieces, objective) : QVector<QVector<int>>();
    const auto order = removalOrder(*pieces, objective, cancelled);
    const auto before = reductionContext(*pieces, objective);
    for (int index : order) {
        if (stopped(cancelled)) {
            return false;
        }
        QVector<std::pair<double, int>> neighbors;
        const auto distances = exposed ? boundaryDistances(adjacency, index) : QVector<int>();
        for (int other = 0; other < pieces->size(); ++other) {
            if (other != index) {
                const QPointF displacement = (*pieces)[other].bounds.center() - (*pieces)[index].bounds.center();
                const double distance = QPointF::dotProduct(displacement, displacement);
                const double score = exposed && !adjacency[index].isEmpty()
                    ? distances[other] + distance / (distance + 1.0) : distance;
                neighbors.push_back({score, other});
            }
        }
        std::sort(neighbors.begin(), neighbors.end());
        QVector<int> excluded = {index};
        for (int neighbor = 0; neighbor < std::min(kMergeNeighbors, static_cast<int>(neighbors.size())); ++neighbor) {
            if (stopped(cancelled)) {
                return false;
            }
            excluded.push_back(neighbors[neighbor].second);
            if (objective.ownership.failed(excluded, broad)) {
                continue;
            }
            const auto context = contextFor(*pieces, excluded, objective);
            const auto target = objective.ownership.exclusive(excluded);
            ++objective.ownershipMergeTrials;
            if (reuseCandidates(pieces, excluded, target, objective, primitives, before, cancelled)) {
                ++objective.ownershipMerges;
                return true;
            }
            bool complete = false;
            for (const auto &replacement : replacementSeeds(target, context, primitives, cancelled, broad, &complete)) {
                QVector<Piece> trial;
                for (int member = 0; member < pieces->size(); ++member) {
                    if (!excluded.contains(member)) {
                        trial.push_back((*pieces)[member]);
                    }
                }
                trial.push_back(replacement);
                if (reductionAllowed(support(trial), before, objective)) {
                    ++objective.ownershipMerges;
                    *pieces = std::move(trial);
                    return true;
                }
                const auto trialSupport = support(trial);
                if (objective.mergeRefinements < kMaximumMergeRefinements && !stopped(cancelled)
                    && error(trialSupport, objective) < std::max(objective.areaBudget * 1.25,
                        (before.state.missingArea + before.state.spillArea) * 1.05)
                    && catalog::area(catalog::subtract(objective.target,
                        catalog::expanded(trialSupport, objective.inwardAllowance)))
                        < objective.areaBudget * 0.02 + catalog::area(before.state.deepMissing)
                    && catalog::area(catalog::subtract(trialSupport, objective.outer)) < objective.areaBudget * 0.001) {
                    ++objective.mergeRefinements;
                    trial.back() = refine(replacement, primitiveFor(replacement.placement.shapeId, primitives), context, cancelled, true);
                    if (reductionAllowed(support(trial), before, objective)) {
                        ++objective.ownershipMerges;
                        *pieces = std::move(trial);
                        return true;
                    }
                }
            }
            if (complete && !stopped(cancelled) && objective.evaluations < objective.evaluationLimit) {
                objective.ownership.rememberFailure(excluded, broad);
            }
        }
    }

    return false;
}

Objective makeObjective(const catalog::Region &region, const FillOptions &options) {
    Objective result;
    const catalog::Region leewayRegion = catalog::leewayAdjustedRegion(
        region, options.leeway, options.boundaryAllowance);
    if (leewayRegion.required.isEmpty() || leewayRegion.visible.isEmpty()) {
        throw std::runtime_error("Contour leeway leaves no visible fillable area");
    }
    Polygons boundary;
    for (const auto &polygon : leewayRegion.required) {
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF delta(options.inwardAllowance, options.inwardAllowance);
            const QPointF start = polygon[index];
            const QPointF end = polygon[(index + 1) % polygon.size()];
            QPolygonF corners;
            for (const auto &point : {start, end}) {
                corners += QPolygonF({point - delta, point + QPointF(delta.x(), -delta.y()),
                    point + delta, point + QPointF(-delta.x(), delta.y())});
            }
            boundary.push_back(catalog::convexHull(corners));
        }
    }
    result.target = leewayRegion.required;
    result.visibleTarget = leewayRegion.visible;
    result.preferred = catalog::expanded(leewayRegion.required, options.inwardAllowance * 0.5);
    result.boundary = std::make_shared<BoundaryModel>(leewayRegion.visible, options.observationScale);
    result.targetMetrics = result.boundary->measure(leewayRegion.visible);
    result.qualityLimit = std::max(1.0, result.boundary->perimeter() * kBoundaryEnergyPerLength);
    result.defectLimit = std::max(1, static_cast<int>(result.boundary->perimeter() / (options.observationScale * kLengthPerDefect)));
    result.inner = catalog::subtract(leewayRegion.required, catalog::unite(boundary));
    result.outer = leewayRegion.permitted;
    result.spillFree = leewayRegion.spillFree;
    result.leeway = catalog::unite(options.leeway);
    result.allowance = options.boundaryAllowance;
    result.areaBudget = catalog::area(leewayRegion.visible) * options.areaErrorRatio;
    result.inwardAllowance = options.inwardAllowance;
    result.fittingInwardAllowance = std::max(options.inwardAllowance * 0.5,
        options.inwardAllowance - options.observationScale * 0.1);
    result.cornerAllowance = options.observationScale * 0.5;
    result.gpuRankingEnabled = options.useGpu;

    return result;
}

double hullArea(const Polygons &polygons) {
    QPolygonF points;
    for (const auto &polygon : polygons) {
        points += polygon;
    }

    return catalog::area({catalog::convexHull(points)});
}

QVector<catalog::Primitive> wholeRegionCatalog(const Objective &objective,
                                              const QVector<catalog::Primitive> &primitives) {
    QVector<catalog::Primitive> result;
    const double targetArea = catalog::area(objective.target);
    const double lower = (targetArea - objective.areaBudget)
        / std::max(hullArea(objective.outer), catalog::kMinimumDeterminant);
    const double upper = (targetArea + objective.areaBudget)
        / std::max(hullArea(objective.inner), catalog::kMinimumDeterminant);
    for (const auto &primitive : primitives) {
        const double convexity = primitive.shape.area
            / std::max(hullArea(primitive.shape.contours), catalog::kMinimumDeterminant);
        if (convexity + kConvexitySlack >= lower && convexity - kConvexitySlack <= upper) {
            result.push_back(primitive);
        }
    }

    return result;
}


} // namespace

catalog::FillResult fillRegion(const PenFillRequest &request,
                               const QVector<catalog::Primitive> &primitives,
                               const FillOptions &options,
                               const std::function<bool()> &cancelled,
                               const std::function<void(int, double, double)> &progress) {
    catalog::FillResult result;
    QJsonObject timings;
    QElapsedTimer stageTimer;
    stageTimer.start();
    const auto recordTime = [&](const QString &name) {
        timings.insert(name, stageTimer.nsecsElapsed() / 1e6);
        stageTimer.start();
    };
    try {
        if (!std::isfinite(options.boundaryAllowance) || options.boundaryAllowance <= 0
            || !std::isfinite(options.areaErrorRatio) || options.areaErrorRatio <= 0
            || options.areaErrorRatio >= 1 || options.shapeBudget < 1 || options.evaluationBudget < 1
            || !std::isfinite(options.inwardAllowance) || options.inwardAllowance <= 0
            || !std::isfinite(options.observationScale) || options.observationScale <= 0) {
            throw std::runtime_error("Compact fit requires positive contour and work allowances and an area ratio between zero and one");
        }
        const auto region = catalog::buildRegion(request, cancelled);
        auto objective = makeObjective(region, options);
        objective.replacementCandidates = options.replacementCandidates;
        objective.evaluationLimit = stageLimit(options.evaluationBudget, WorkStage::Recognition);
        const auto recognitionCatalog = wholeRegionCatalog(objective, primitives);
        int currentCount = 0;
        objective.workProgress = [&](int evaluated) {
            if (options.workProgress) {
                options.workProgress(currentCount, evaluated, options.evaluationBudget);
            }
        };
        const auto stopWork = [&] {
            return stopped(cancelled) || objective.evaluations >= objective.evaluationLimit;
        };
        QJsonObject stageWork;
        int stageStart = 0;
        int stageRefitStart = 0;
        const auto finishStage = [&](const QString &name) {
            stageWork.insert(name, QJsonObject{{QStringLiteral("start"), stageStart},
                {QStringLiteral("ceiling"), objective.evaluationLimit},
                {QStringLiteral("available"), objective.evaluationLimit - stageStart},
                {QStringLiteral("evaluations"), objective.evaluations - stageStart},
                {QStringLiteral("refitPlacements"), objective.refitPlacements - stageRefitStart},
                {QStringLiteral("stopReason"), stopped(cancelled) ? QStringLiteral("cancelled")
                    : objective.evaluations >= objective.evaluationLimit ? QStringLiteral("stage budget")
                    : QStringLiteral("search completed")}});
            stageStart = objective.evaluations;
            stageRefitStart = objective.refitPlacements;
            if (objective.workProgress) {
                objective.workProgress(objective.evaluations);
            }
        };
        PenFillRequest seedRequest = request;
        QVector<Piece> pieces;
        QVector<Piece> incumbent;
        QVector<catalog::Primitive> searchCatalog;
        QJsonArray history;
        seedRequest.primitives.clear();
        for (const auto &primitive : primitives) {
            seedRequest.primitives.push_back(primitive.shape);
            if (std::find(kSearchShapeIds.begin(), kSearchShapeIds.end(), primitive.shape.shapeId) != kSearchShapeIds.end()
                || (objective.targetMetrics.holes > 0 && primitive.shape.contours.size() > 1)) {
                searchCatalog.push_back(primitive);
            }
        }
        PenFillResult seed;
        if (options.initialPlacements.isEmpty()) {
            seed = fillPenPath(seedRequest, cancelled);
        } else {
            seed.placements = options.initialPlacements;
        }
        if (!seed.error.isEmpty()) {
            throw std::runtime_error(seed.error.toStdString());
        }
        for (const auto &placement : seed.placements) {
            pieces.push_back(makePiece(primitiveFor(placement.shapeId, primitives), placement.transform));
        }
        result.diagnostics.insert(QStringLiteral("seedCount"), pieces.size());
        result.diagnostics.insert(QStringLiteral("searchCatalogSize"), searchCatalog.size());
        result.diagnostics.insert(QStringLiteral("wholeRegionCatalogSize"), recognitionCatalog.size());
        result.diagnostics.insert(QStringLiteral("seedBoundary"), objective.boundary->diagnostics(
            objective.boundary->measure(visibleSupport(support(pieces), objective))));
        if (acceptable(support(pieces), objective)) {
            incumbent = pieces;
        }
        auto report = [&] {
            currentCount = pieces.size();
            const auto coverage = support(pieces);
            const Polygons visibleCoverage = visibleSupport(coverage, objective);
            const double missing = catalog::area(catalog::subtract(objective.visibleTarget, visibleCoverage));
            const double spill = catalog::area(catalog::subtract(visibleCoverage, objective.visibleTarget));
            history.push_back(QJsonObject{{QStringLiteral("count"), pieces.size()},
                {QStringLiteral("missing"), missing}, {QStringLiteral("spill"), spill},
                {QStringLiteral("deepMissing"), catalog::area(catalog::subtract(objective.target,
                    catalog::expanded(coverage, objective.inwardAllowance)))}});
            if (acceptable(coverage, objective)
                && (incumbent.isEmpty() || pieces.size() < incumbent.size()
                    || (pieces.size() == incumbent.size() && error(coverage, objective) < error(support(incumbent), objective)))) {
                incumbent = pieces;
            }
            if (progress) {
                progress(pieces.size(), missing, spill);
            }
        };
        const auto reduceExact = [&](const QString &diagnostic, bool deletionsOnly) {
            if (stopped(cancelled) || pieces.size() < 2) {
                return;
            }
            QVector<PenPlacement> placements;
            for (const auto &piece : pieces) {
                placements.push_back(piece.placement);
            }
            try {
                const auto exact = reduceExactCoverage(placements, primitives,
                    objective.replacementCandidates ? *objective.replacementCandidates : QVector<ReusableCandidate>(),
                    cancelled, deletionsOnly);
                if (exact.placements.size() < pieces.size()) {
                    QVector<Piece> trial;
                    for (const auto &placement : exact.placements) {
                        trial.push_back(makePiece(primitiveFor(placement.shapeId, primitives), placement.transform));
                    }
                    const auto before = support(pieces);
                    const auto after = support(trial);
                    if (catalog::subtract(before, after).isEmpty() && catalog::subtract(after, before).isEmpty()) {
                        if (deletionsOnly) {
                            objective.exactReductions += pieces.size() - trial.size();
                        }
                        pieces = std::move(trial);
                        report();
                    }
                }
                result.diagnostics.insert(diagnostic, exact.diagnostics);
            } catch (const std::exception &failure) {
                result.diagnostics.insert(diagnostic + QStringLiteral("Error"), QString::fromUtf8(failure.what()));
            }
        };
        report();
        recordTime(QStringLiteral("setup"));
        bool fullGpuPipeline = false;
        bool gpuPipelineApplied = false;
        int gpuPipelineEvaluations = 0;
        if (pieces.size() > 1 && !recognitionCatalog.isEmpty() && !stopWork()) {
            const auto context = contextFor({}, {}, objective);
            for (const auto &replacement : replacementSeeds(objective.target, context, recognitionCatalog, stopWork, true)) {
                if (acceptable(replacement.polygons, objective)) {
                    pieces = {replacement};
                    report();
                    break;
                }
            }
        }
        recordTime(QStringLiteral("recognition"));
        finishStage(QStringLiteral("recognition"));
#ifdef FLS_HAS_CUDA
        if (options.useGpu && !pieces.isEmpty() && !stopped(cancelled)) {
            const int gpuStart = objective.evaluations;
            objective.evaluationLimit = options.evaluationBudget;
            fullGpuPipeline = optimizeGpu(&pieces, objective, primitives, stopWork);
            gpuPipelineApplied = fullGpuPipeline;
            if (!gpuPipelineApplied) {
                objective.evaluations = gpuStart;
            } else {
                gpuPipelineEvaluations = objective.evaluations - gpuStart;
            }
        }
#endif
        if (gpuPipelineApplied) {
            report();
            recordTime(QStringLiteral("gpuPipeline"));
            finishStage(QStringLiteral("gpuPipeline"));
            if (gpuPipelineEvaluations > options.evaluationBudget / 2
                || acceptable(support(pieces), objective)) {
                objective.evaluationLimit = objective.evaluations;
            } else {
                fullGpuPipeline = false;
                objective.evaluations -= gpuPipelineEvaluations;
            }
        }
        objective.evaluationLimit = fullGpuPipeline ? objective.evaluations
            : stageLimit(options.evaluationBudget, WorkStage::Repair);
        if (pieces.size() > 1 && !stopWork()) {
            const auto original = pieces;
            const auto originalState = reductionStateFor(support(original), objective);
            const auto score = [&](const QVector<Piece> &candidate) {
                const auto coverage = support(candidate);
                if (!catalog::subtract(coverage, objective.outer).isEmpty()) {
                    return -std::numeric_limits<double>::infinity();
                }
                return kMissingWeight * catalog::area(catalog::intersect(coverage, objective.preferred))
                    - catalog::area(catalog::subtract(coverage, objective.target))
                    - kDeepErrorWeight * catalog::area(catalog::subtract(coverage, objective.outer))
                    - boundaryCost(coverage, objective);
            };
            double bestScore = score(pieces);
            for (double padding : {0.125, 0.25, 0.5, 0.75}) {
                auto trial = original;
                for (auto &piece : trial) {
                    QTransform expansion;
                    expansion.translate(piece.bounds.center().x(), piece.bounds.center().y());
                    expansion.scale(1 + 2 * padding / std::max(piece.bounds.width(), padding),
                        1 + 2 * padding / std::max(piece.bounds.height(), padding));
                    expansion.translate(-piece.bounds.center().x(), -piece.bounds.center().y());
                    piece = makePiece(primitiveFor(piece.placement.shapeId, primitives), piece.placement.transform * expansion);
                }
                const double candidateScore = score(trial);
                const auto after = reductionStateFor(support(trial), objective);
                if (candidateScore > bestScore && coverageMoveAllowed(after, originalState, objective)
                    && after.missingArea + after.spillArea <= std::max(objective.areaBudget,
                        originalState.missingArea + originalState.spillArea) + kScoreEpsilon) {
                    pieces = std::move(trial);
                    bestScore = candidateScore;
                }
            }
            report();
        }
        const int repairLimit = objective.evaluationLimit;
        const int availableRepair = repairLimit - objective.evaluations;
        const int broadWork = availableRepair > kBroadRepairMinimumWork
            ? std::min(kBroadRepairWork, availableRepair / 8) : 0;
        const int broadStart = repairLimit - broadWork;
        objective.evaluationLimit = objective.evaluations + (broadStart - objective.evaluations) / 2;
        for (int pass = 0; pass < kRefitPasses && !stopWork(); ++pass) {
            refitFeasible(&pieces, objective, primitives, stopWork);
            report();
            if (acceptable(support(pieces), objective)) {
                break;
            }
        }
        objective.evaluationLimit = repairLimit;
        repairResiduals(&pieces, objective, searchCatalog, options.shapeBudget, broadStart, stopWork);
        report();
        consolidateRepairPatches(&pieces, objective, primitives, repairLimit, stopWork);
        report();
        if (!stopWork() && !acceptable(support(pieces), objective)) {
            refitFeasible(&pieces, objective, primitives, stopWork);
            report();
        }
        if (!fullGpuPipeline && !acceptable(support(pieces), objective) && !incumbent.isEmpty()) {
            pieces = incumbent;
        }
        recordTime(QStringLiteral("initialRefit"));
        finishStage(QStringLiteral("repair"));
        if (!fullGpuPipeline) {
            reduceExact(QStringLiteral("exactEarlyReduction"), true);
        }
        recordTime(QStringLiteral("earlyExactReduction"));
        if (!fullGpuPipeline) {
            objective.reductionBaseline = reductionStateFor(support(pieces), objective);
        }
        objective.evaluationLimit = fullGpuPipeline ? objective.evaluations
            : stageLimit(options.evaluationBudget, WorkStage::SpatialReduction);
        prune(&pieces, objective, stopWork);
        report();
        for (int round = 0; round < kMergeRounds && pieces.size() > 1 && !stopWork(); ++round) {
            bool reduced = false;
            const auto order = removalOrder(pieces, objective, stopWork);
            const auto before = reductionContext(pieces, objective);
            for (int trialIndex = 0; trialIndex < std::min(kDeletionTrials, static_cast<int>(order.size())); ++trialIndex) {
                if (stopWork()) {
                    break;
                }
                auto trial = pieces;
                const auto changed = trial[order[trialIndex]].bounds;
                trial.removeAt(order[trialIndex]);
                refitNeighborhood(&trial, changed, objective, primitives, stopWork);
                if (reductionAllowed(support(trial), before, objective)) {
                    pieces = std::move(trial);
                    reduced = true;
                    break;
                }
            }
            if (!reduced) {
                reduced = merge(&pieces, objective, searchCatalog, stopWork, false);
            }
            if (!reduced && !stopWork()) {
                reduced = merge(&pieces, objective, searchCatalog, stopWork, true);
            }
            if (!reduced) {
                break;
            }
            prune(&pieces, objective, stopWork);
            report();
        }
        finishStage(QStringLiteral("spatialReduction"));
        objective.evaluationLimit = fullGpuPipeline ? objective.evaluations
            : stageLimit(options.evaluationBudget, WorkStage::ExposedReduction);
        fitBoundaryPairs(&pieces, objective, primitives,
            objective.evaluations + (objective.evaluationLimit - objective.evaluations) / 2, stopWork);
        report();
        int exposedMerges = 0;
        while (pieces.size() > 1 && !stopWork()
                && merge(&pieces, objective, searchCatalog, stopWork, false, true)) {
            ++exposedMerges;
            prune(&pieces, objective, stopWork);
            report();
        }
        result.diagnostics.insert(QStringLiteral("exposedBoundaryMerges"), exposedMerges);
        recordTime(QStringLiteral("compaction"));
        finishStage(QStringLiteral("exposedReduction"));
        objective.evaluationLimit = fullGpuPipeline ? objective.evaluations
            : stageLimit(options.evaluationBudget, WorkStage::Polish);
        if (!fullGpuPipeline && !incumbent.isEmpty()) {
            pieces = incumbent;
        }
        if (!fullGpuPipeline && !pieces.isEmpty()) {
            auto polished = pieces;
            objective.boundaryWeight *= 4.0;
            refit(&polished, objective, primitives, stopWork, 1);
            const auto before = support(pieces);
            const auto after = support(polished);
            const bool preservesQuality = acceptable(after, objective)
                || (!acceptable(before, objective) && catalog::subtract(after, objective.outer).isEmpty()
                    && nonWorseningReduction(reductionStateFor(after, objective),
                        reductionStateFor(before, objective), objective.targetMetrics));
            if (preservesQuality
                && objective.boundary->energy(objective.boundary->measure(after))
                    < objective.boundary->energy(objective.boundary->measure(before))) {
                pieces = std::move(polished);
            }
        }
        recordTime(QStringLiteral("polish"));
        finishStage(QStringLiteral("polish"));
        if (!fullGpuPipeline) {
            reduceExact(QStringLiteral("exactFinalReduction"), false);
        }
        recordTime(QStringLiteral("exactReduction"));
        const auto coverage = support(pieces);
        result.diagnostics.insert(QStringLiteral("stageWork"), stageWork);
        result.diagnostics.insert(QStringLiteral("approximateReductions"), objective.approximateReductions);
        result.diagnostics.insert(QStringLiteral("exactReductions"), objective.exactReductions);
        result.diagnostics.insert(QStringLiteral("coverageOwnership"), objective.ownership.diagnostics());
#ifdef FLS_HAS_CUDA
        if (objective.gpuRanker) {
            const auto stats = objective.gpuRanker->stats();
            result.diagnostics.insert(QStringLiteral("gpuRasterRank"), QJsonObject{
                {QStringLiteral("adapter"), QString::fromStdString(stats.adapter)},
                {QStringLiteral("error"), QString::fromStdString(stats.error)},
                {QStringLiteral("columns"), stats.columns},
                {QStringLiteral("rows"), stats.rows},
                {QStringLiteral("cellSize"), stats.cellSize},
                {QStringLiteral("calls"), stats.calls},
                {QStringLiteral("refinementPreparations"), stats.refinementPreparations},
                {QStringLiteral("refinementCalls"), stats.refinementCalls},
                {QStringLiteral("persistentPreparations"), stats.persistentPreparations},
                {QStringLiteral("persistentCommits"), stats.persistentCommits},
                {QStringLiteral("setupMilliseconds"), stats.setupMilliseconds},
                {QStringLiteral("evaluationMilliseconds"), stats.evaluationMilliseconds},
                {QStringLiteral("refinementMilliseconds"), stats.refinementMilliseconds},
                {QStringLiteral("fallbacks"), objective.gpuRankerFallbacks}});
        }
#endif
        result.diagnostics.insert(QStringLiteral("gpuRefinementBatches"),
            objective.gpuRefinementBatches);
        result.diagnostics.insert(QStringLiteral("gpuRefinementCandidates"),
            objective.gpuRefinementCandidates);
        result.diagnostics.insert(QStringLiteral("gpuRefinementCommits"),
            objective.gpuRefinementCommits);
        result.diagnostics.insert(QStringLiteral("gpuRefinementCheckpoints"),
            objective.gpuRefinementCheckpoints);
        result.diagnostics.insert(QStringLiteral("gpuPipelinePasses"),
            objective.gpuPipelinePasses);
        result.diagnostics.insert(QStringLiteral("gpuPipelinePruned"),
            objective.gpuPipelinePruned);
        result.diagnostics.insert(QStringLiteral("gpuPipelineRestores"),
            objective.gpuPipelineRestores);
        result.diagnostics.insert(QStringLiteral("gpuPipelineExpanded"),
            objective.gpuPipelineExpanded);
        result.diagnostics.insert(QStringLiteral("gpuGroupTrials"),
            objective.gpuGroupTrials);
        result.diagnostics.insert(QStringLiteral("gpuGroupReplacements"),
            objective.gpuGroupReplacements);
        result.diagnostics.insert(QStringLiteral("gpuPipelineCandidate"),
            objective.gpuPipelineCandidate);
        result.diagnostics.insert(QStringLiteral("optimizerBackend"),
            fullGpuPipeline ? QStringLiteral("CUDA raster")
            : gpuPipelineApplied ? QStringLiteral("CUDA raster + CPU exact")
            : QStringLiteral("CPU exact"));
        result.diagnostics.insert(QStringLiteral("gpuPipelineEvaluations"), gpuPipelineEvaluations);
        result.diagnostics.insert(QStringLiteral("ownershipMergeTrials"), objective.ownershipMergeTrials);
        result.diagnostics.insert(QStringLiteral("ownershipMerges"), objective.ownershipMerges);
        result.diagnostics.insert(QStringLiteral("reusePoolSize"), objective.replacementCandidates ? objective.replacementCandidates->size() : 0);
        result.diagnostics.insert(QStringLiteral("reusedCandidates"), objective.reusedCandidates);
        result.diagnostics.insert(QStringLiteral("reuseChecks"), objective.reuseChecks);
        result.diagnostics.insert(QStringLiteral("reusedMerges"), objective.reusedMerges);
        result.diagnostics.insert(QStringLiteral("reusedPairs"), objective.reusedPairs);
        result.diagnostics.insert(QStringLiteral("localContexts"), objective.localContexts);
        result.diagnostics.insert(QStringLiteral("localEvaluations"), objective.localEvaluations);
        result.diagnostics.insert(QStringLiteral("globalMoveChecks"), objective.globalMoveChecks);
        result.diagnostics.insert(QStringLiteral("jointRepairTrials"), objective.jointRepairTrials);
        result.diagnostics.insert(QStringLiteral("jointRepairs"), objective.jointRepairs);
        result.diagnostics.insert(QStringLiteral("repairClusters"), objective.repairClusters);
        result.diagnostics.insert(QStringLiteral("repairGroupReplacements"), objective.repairGroupReplacements);
        result.diagnostics.insert(QStringLiteral("repairBenefitRejected"), objective.repairBenefitRejected);
        result.diagnostics.insert(QStringLiteral("broadRepairTrials"), objective.broadRepairTrials);
        result.diagnostics.insert(QStringLiteral("broadRepairReplacements"), objective.broadRepairReplacements);
        result.diagnostics.insert(QStringLiteral("coverageRejected"), objective.coverageRejected);
        result.diagnostics.insert(QStringLiteral("coverageRejections"), objective.coverageRejections);
        result.diagnostics.insert(QStringLiteral("residualInsertions"), objective.residualInsertions);
        result.diagnostics.insert(QStringLiteral("residualConnectors"), objective.residualConnectors);
        result.diagnostics.insert(QStringLiteral("connectorDiagnostics"), objective.connectorDiagnostics);
        result.diagnostics.insert(QStringLiteral("neighborhoodRepairs"), objective.neighborhoodRepairs);
        result.diagnostics.insert(QStringLiteral("topologyRepairs"), objective.topologyRepairs);
        result.diagnostics.insert(QStringLiteral("feasibleCheckpoints"), objective.feasibleCheckpoints);
        result.diagnostics.insert(QStringLiteral("feasibleRestores"), objective.feasibleRestores);
        result.diagnostics.insert(QStringLiteral("jointBoundaryMoves"), objective.jointMoves);
        result.diagnostics.insert(QStringLiteral("qualityReference"), QStringLiteral("matched observation support; raw protected corners"));
        result.fill.cancelled = stopped(cancelled);
        result.fill.shapeLimit = options.shapeBudget;
        const Polygons visibleCoverage = visibleSupport(coverage, objective);
        result.fill.targetArea = catalog::area(objective.visibleTarget);
        result.fill.coveredArea = catalog::area(catalog::intersect(visibleCoverage, objective.visibleTarget));
        result.fill.outsideArea = catalog::area(catalog::subtract(visibleCoverage, objective.visibleTarget));
        result.fill.unfilled = catalog::painterPath(catalog::subtract(objective.visibleTarget, visibleCoverage));
        result.diagnostics.insert(QStringLiteral("history"), history);
        result.diagnostics.insert(QStringLiteral("boundaryAllowance"), options.boundaryAllowance);
        result.diagnostics.insert(QStringLiteral("areaErrorRatio"), options.areaErrorRatio);
        result.diagnostics.insert(QStringLiteral("leewayArea"), catalog::area(objective.leeway));
        result.diagnostics.insert(QStringLiteral("requiredTargetArea"), catalog::area(objective.target));
        result.diagnostics.insert(QStringLiteral("visibleTargetArea"), result.fill.targetArea);
        result.diagnostics.insert(QStringLiteral("inwardAllowance"), options.inwardAllowance);
        result.diagnostics.insert(QStringLiteral("fittingInwardAllowance"), objective.fittingInwardAllowance);
        result.diagnostics.insert(QStringLiteral("targetBoundary"), objective.boundary->diagnostics(objective.targetMetrics));
        result.diagnostics.insert(QStringLiteral("observationScale"), options.observationScale);
        result.diagnostics.insert(QStringLiteral("maximumExcessTurnLimit"), kMaximumExcessTurn);
        result.diagnostics.insert(QStringLiteral("cornerDistanceLimit"), objective.cornerAllowance);
        result.diagnostics.insert(QStringLiteral("cornerDefectLimit"), objective.defectLimit);
        result.diagnostics.insert(QStringLiteral("missingBeyondInward"), catalog::area(catalog::subtract(objective.target,
            catalog::expanded(coverage, objective.inwardAllowance))));
        const auto finalState = reductionStateFor(coverage, objective);
        result.diagnostics.insert(QStringLiteral("repairResidualArea"), catalog::area(
            repairResidual(finalState, objective.target, objective.inner, objective.targetMetrics)));
        result.diagnostics.insert(QStringLiteral("observedHoleDefectArea"), catalog::area(finalState.observedHoles));
        result.diagnostics.insert(QStringLiteral("outsideEnvelope"), catalog::area(catalog::subtract(coverage, objective.outer)));
        result.diagnostics.insert(QStringLiteral("boundaryQuality"), objective.boundary->diagnostics(
            objective.boundary->measure(visibleCoverage)));
        result.diagnostics.insert(QStringLiteral("boundaryEnergyLimit"), objective.qualityLimit);
        result.diagnostics.insert(QStringLiteral("qualityEvaluations"), objective.qualityEvaluations);
        result.diagnostics.insert(QStringLiteral("qualityBoundsRejected"), objective.qualityBoundsRejected);
        result.diagnostics.insert(QStringLiteral("qualityTimings"), objective.boundary->performance());
        result.diagnostics.insert(QStringLiteral("mergeRefinements"), objective.mergeRefinements);
        result.diagnostics.insert(QStringLiteral("evaluations"), objective.evaluations);
        result.diagnostics.insert(QStringLiteral("evaluationBudget"), options.evaluationBudget);
        result.diagnostics.insert(QStringLiteral("evaluationBudgetReached"), objective.evaluations >= options.evaluationBudget);
        result.diagnostics.insert(QStringLiteral("geometryModel"), QStringLiteral("triangle union, 1e-6 grid, reconstructed float transforms"));
        result.diagnostics.insert(QStringLiteral("displacementMetric"), QStringLiteral("asymmetric support distance, L-infinity world units"));
        QJsonObject histogram;
        for (const auto &piece : pieces) {
            const auto key = QString::number(piece.placement.shapeId);
            histogram[key] = histogram[key].toInt() + 1;
        }
        result.diagnostics.insert(QStringLiteral("shapeCounts"), histogram);
        const auto metrics = objective.boundary->measure(visibleCoverage);
        const double areaError = result.fill.targetArea - result.fill.coveredArea + result.fill.outsideArea;
        QStringList failedChecks;
        if (coverage.isEmpty()) {
            failedChecks.push_back(QStringLiteral("nonempty coverage"));
        }
        if (areaError > objective.areaBudget) {
            failedChecks.push_back(QStringLiteral("area error"));
        }
        if (result.diagnostics.value(QStringLiteral("missingBeyondInward")).toDouble() > 0) {
            failedChecks.push_back(QStringLiteral("interior coverage"));
        }
        if (result.diagnostics.value(QStringLiteral("outsideEnvelope")).toDouble() > 0) {
            failedChecks.push_back(QStringLiteral("outward margin"));
        }
        if (objective.boundary->energy(metrics) > objective.qualityLimit
            || metrics.maximumExcessTurn > kMaximumExcessTurn || metrics.cornerDefects > objective.defectLimit) {
            failedChecks.push_back(QStringLiteral("contour continuity"));
        }
        if (metrics.maximumCornerDistance > objective.cornerAllowance) {
            failedChecks.push_back(QStringLiteral("sharp-corner position"));
        }
        if (metrics.components != objective.targetMetrics.components || metrics.holes != objective.targetMetrics.holes) {
            failedChecks.push_back(QStringLiteral("region topology"));
        }
        result.diagnostics.insert(QStringLiteral("failedChecks"), QJsonArray::fromStringList(failedChecks));
        result.diagnostics.insert(QStringLiteral("areaError"), areaError);
        result.diagnostics.insert(QStringLiteral("areaErrorLimit"), objective.areaBudget);
        result.diagnostics.insert(QStringLiteral("approximationVerified"), acceptable(coverage, objective));
        recordTime(QStringLiteral("verification"));
        if (!result.fill.cancelled) {
            for (const auto &piece : pieces) {
                result.fill.placements.push_back(piece.placement);
            }
        }
        if (!result.fill.cancelled && !acceptable(coverage, objective)) {
            throw std::runtime_error(QStringLiteral("Compact fit failed: %1. The outward margin does not relax the other limits.")
                .arg(failedChecks.join(QStringLiteral(", "))).toStdString());
        }
        if (pieces.size() > options.shapeBudget) {
            throw std::runtime_error("Compact fit exceeds the shape budget");
        }
    } catch (const std::exception &exception) {
        result.fill.error = QString::fromUtf8(exception.what());
        result.fill.cancelled = stopped(cancelled);
        if (!options.retainFailedFill || result.fill.cancelled) {
            result.fill.placements.clear();
        }
    }
    result.diagnostics.insert(QStringLiteral("retainedAfterError"),
        !result.fill.error.isEmpty() && !result.fill.placements.isEmpty());
    result.diagnostics.insert(QStringLiteral("stageMilliseconds"), timings);

    return result;
}

} // namespace gui::compact
