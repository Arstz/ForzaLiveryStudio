#include "differential_cover_internal.h"

#include <algorithm>
#include <cmath>

namespace gui::cover {

namespace {

constexpr int kMaximumContinuityPlacements = 16;
constexpr int kMaximumContinuityEdges = 2;
constexpr double kContinuityMinimumTurn =
    0.17453292519943295;
constexpr double kContinuityMinimumAlignment =
    0.08726646259971647;
constexpr double kContinuityTargetBandFactor = 2.0;
constexpr double kContinuityExposureToleranceFactor = 0.05;
constexpr double kContinuityTangentWeight = 0.25;
constexpr double kContinuityMinimumEnergyImprovement = 1e-4;
constexpr double kContinuityAngleAllowance = 1e-4;
constexpr double kContinuityMaximumCoverageLossRatio = 0.0005;
constexpr double kContinuityPlacementCoverageLossRatio = 0.02;
constexpr double kContinuityBoundaryDistanceAllowanceFactor = 0.25;
constexpr std::array<double, 5> kContinuityAlignmentFractions{
    0.05,
    0.1,
    0.25,
    0.5,
    1.0,
};
constexpr std::array<double, 3> kContinuityScaleFactors{
    0.995,
    0.99,
    0.98,
};

struct ContinuityBoundaryLocation {
    QPointF point;
    QPointF tangent;
    double distance =
        std::numeric_limits<double>::infinity();
};

struct ContinuityMetrics {
    double energy = 0.0;
    double maximumTurn = 0.0;
    int kinkCount = 0;
};

struct AlignmentEdge {
    QPointF contact;
    QPointF targetPoint;
    double rotation = 0.0;
    double score = 0.0;
};

struct ContinuityPlacement {
    double score = 0.0;
    int index = -1;
};

QPointF continuityNormalized(
    const QPointF &vector) {
    const double length = std::hypot(
        vector.x(), vector.y());

    return length > kGeometryEpsilon
        ? vector / length : QPointF{};
}

double continuityCross(
    const QPointF &left,
    const QPointF &right) {
    return left.x() * right.y()
        - left.y() * right.x();
}

double continuityLineAngle(
    const QPointF &left,
    const QPointF &right) {
    if (left.isNull() || right.isNull()) {
        return kPi * 0.5;
    }

    return std::acos(
        std::clamp(
            std::abs(
                QPointF::dotProduct(
                    left, right)),
            0.0, 1.0));
}

ContinuityBoundaryLocation
closestContinuityBoundary(
    const QPointF &point,
    const Polygons &polygons) {
    ContinuityBoundaryLocation result;
    for (const QPolygonF &polygon : polygons) {
        for (int index = 0;
             index < polygon.size(); ++index) {
            const QPointF start = polygon[index];
            const QPointF end =
                polygon[(index + 1)
                        % polygon.size()];
            const QPointF edge = end - start;
            const double lengthSquared =
                QPointF::dotProduct(edge, edge);
            const double parameter =
                lengthSquared > kGeometryEpsilon
                ? std::clamp(
                    QPointF::dotProduct(
                        point - start, edge)
                        / lengthSquared,
                    0.0, 1.0)
                : 0.0;
            const QPointF closest =
                start + edge * parameter;
            const double distance =
                QLineF(point, closest).length();
            if (distance
                >= result.distance
                    - kGeometryEpsilon) {
                continue;
            }
            result.point = closest;
            result.tangent =
                continuityNormalized(edge);
            result.distance = distance;
        }
    }

    return result;
}

bool nearAuthoredCorner(
    const QPointF &point,
    const QVector<ContourFeature> &features,
    double boundaryTolerance) {
    return std::any_of(
        features.cbegin(), features.cend(),
        [&](const ContourFeature &feature) {
            return feature.kind
                    == ContourFeatureKind::Corner
                && QLineF(
                    point,
                    feature.position).length()
                    <= std::max(
                        boundaryTolerance,
                        feature.captureRadius);
        });
}

ContinuityMetrics contourContinuityMetrics(
    const Polygons &coverage,
    const Polygons &target,
    const QVector<ContourFeature> &features,
    const FillOptions &options) {
    ContinuityMetrics result;
    const double targetBand =
        options.boundaryTolerance
        * kContinuityTargetBandFactor;
    for (const QPolygonF &polygon : coverage) {
        if (polygon.size() < 3) {
            continue;
        }
        for (int index = 0;
             index < polygon.size(); ++index) {
            const QPointF point = polygon[index];
            const QPointF incoming =
                continuityNormalized(
                    point
                    - polygon[
                        (index + polygon.size() - 1)
                        % polygon.size()]);
            const QPointF outgoing =
                continuityNormalized(
                    polygon[
                        (index + 1)
                        % polygon.size()]
                    - point);
            if (incoming.isNull()
                || outgoing.isNull()
                || nearAuthoredCorner(
                    point, features,
                    options.boundaryTolerance)) {
                continue;
            }
            const ContinuityBoundaryLocation targetLocation =
                closestContinuityBoundary(
                    point, target);
            if (targetLocation.distance
                > targetBand) {
                continue;
            }
            const double turn = std::acos(
                std::clamp(
                    QPointF::dotProduct(
                        incoming, outgoing),
                    -1.0, 1.0));
            if (turn
                <= kContinuityMinimumTurn) {
                continue;
            }
            const double turnExcess =
                turn - kContinuityMinimumTurn;
            const double incomingError =
                continuityLineAngle(
                    incoming,
                    targetLocation.tangent);
            const double outgoingError =
                continuityLineAngle(
                    outgoing,
                    targetLocation.tangent);
            result.energy +=
                turnExcess * turnExcess
                + kContinuityTangentWeight
                    * (incomingError * incomingError
                       + outgoingError * outgoingError);
            result.maximumTurn =
                std::max(
                    result.maximumTurn, turn);
            ++result.kinkCount;
        }
    }

    return result;
}

Affine rotatedContinuityTransform(
    const Placement &placement,
    const QPointF &pivot,
    double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const Affine &source = placement.transform;
    Affine result;
    result.a = cosine * source.a
        - sine * source.b;
    result.b = sine * source.a
        + cosine * source.b;
    result.c = cosine * source.c
        - sine * source.d;
    result.d = sine * source.c
        + cosine * source.d;
    result.e = pivot.x()
        + cosine * (source.e - pivot.x())
        - sine * (source.f - pivot.y());
    result.f = pivot.y()
        + sine * (source.e - pivot.x())
        + cosine * (source.f - pivot.y());
    if (placement.anchor.has_value()) {
        const CandidateAnchor &anchor =
            *placement.anchor;
        result.e = anchor.target.x()
            - result.a * anchor.source.x
            - result.c * anchor.source.y;
        result.f = anchor.target.y()
            - result.b * anchor.source.x
            - result.d * anchor.source.y;
    }

    return result;
}

Affine scaledContinuityTransform(
    const Placement &placement,
    const QPointF &pivot,
    double scale) {
    const Affine &source = placement.transform;
    Affine result = source;
    result.a *= scale;
    result.b *= scale;
    result.c *= scale;
    result.d *= scale;
    result.e = pivot.x()
        + (source.e - pivot.x()) * scale;
    result.f = pivot.y()
        + (source.f - pivot.y()) * scale;
    if (placement.anchor.has_value()) {
        const CandidateAnchor &anchor =
            *placement.anchor;
        result.e = anchor.target.x()
            - result.a * anchor.source.x
            - result.c * anchor.source.y;
        result.f = anchor.target.y()
            - result.b * anchor.source.x
            - result.d * anchor.source.y;
    }

    return result;
}

QVector<AlignmentEdge> continuityAlignmentEdges(
    const QPolygonF &footprint,
    const Polygons &coverage,
    const Polygons &target,
    const QVector<ContourFeature> &features,
    const FillOptions &options) {
    QVector<AlignmentEdge> result;
    const double exposureTolerance =
        std::max(
            kGeometryEpsilon,
            options.boundaryTolerance
                * kContinuityExposureToleranceFactor);
    const double targetBand =
        options.boundaryTolerance
        * kContinuityTargetBandFactor;
    for (int index = 0;
         index < footprint.size(); ++index) {
        const QPointF start = footprint[index];
        const QPointF end =
            footprint[(index + 1)
                      % footprint.size()];
        const QPointF edge = end - start;
        const double edgeLength =
            std::hypot(edge.x(), edge.y());
        if (edgeLength
            < options.boundaryTolerance) {
            continue;
        }
        const QPointF contact =
            (start + end) * 0.5;
        if (closestContinuityBoundary(
                contact, coverage).distance
                > exposureTolerance
            || nearAuthoredCorner(
                contact, features,
                options.boundaryTolerance)) {
            continue;
        }
        const ContinuityBoundaryLocation targetLocation =
            closestContinuityBoundary(
                contact, target);
        if (targetLocation.distance
                > targetBand
            || targetLocation.tangent.isNull()) {
            continue;
        }
        const QPointF edgeTangent =
            edge / edgeLength;
        QPointF targetTangent =
            targetLocation.tangent;
        if (QPointF::dotProduct(
                edgeTangent, targetTangent)
            < 0.0) {
            targetTangent = -targetTangent;
        }
        const double rotation = std::atan2(
            continuityCross(
                edgeTangent, targetTangent),
            QPointF::dotProduct(
                edgeTangent, targetTangent));
        if (std::abs(rotation)
            < kContinuityMinimumAlignment) {
            continue;
        }
        result.push_back({
            contact,
            targetLocation.point,
            rotation,
            edgeLength * std::abs(rotation),
        });
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const AlignmentEdge &left,
           const AlignmentEdge &right) {
            return left.score > right.score;
        });
    if (result.size()
        > kMaximumContinuityEdges) {
        result.resize(
            kMaximumContinuityEdges);
    }

    return result;
}

QVector<Affine> continuityProposals(
    const Placement &placement,
    const QPolygonF &footprint,
    const Polygons &coverage,
    const Polygons &target,
    const QVector<ContourFeature> &features,
    const FillOptions &options) {
    QVector<Affine> result;
    const QVector<AlignmentEdge> edges =
        continuityAlignmentEdges(
            footprint, coverage, target,
            features, options);
    if (edges.isEmpty()) {
        return result;
    }
    const QPointF pivot =
        placement.anchor.has_value()
        ? placement.anchor->target
        : footprint.boundingRect().center();
    for (const AlignmentEdge &edge : edges) {
        for (const double fraction :
             kContinuityAlignmentFractions) {
            result.push_back(
                rotatedContinuityTransform(
                    placement, pivot,
                    edge.rotation * fraction));
            if (!placement.anchor.has_value()) {
                Affine translated =
                    placement.transform;
                const QPointF translation =
                    (edge.targetPoint
                     - edge.contact)
                    * fraction;
                translated.e += translation.x();
                translated.f += translation.y();
                result.push_back(translated);
            }
        }
    }
    for (const double scale :
         kContinuityScaleFactors) {
        result.push_back(
            scaledContinuityTransform(
                placement, pivot, scale));
    }

    return result;
}

bool betterContinuity(
    const ContinuityMetrics &left,
    const ContinuityMetrics &right) {
    if (left.kinkCount != right.kinkCount) {
        return left.kinkCount
            < right.kinkCount;
    }

    return left.energy
        < right.energy
            - kContinuityMinimumEnergyImprovement;
}

} // namespace

QVector<PruneCandidate> pruneCandidateOrder(
    const QVector<Placement> &placements,
    const Polygons &mustCover,
    const ExactCoverState &currentState) {
    QVector<PruneCandidate> result;
    result.reserve(placements.size());
    for (int index = 0; index < placements.size(); ++index) {
        Polygons reducedFootprints =
            currentState.footprints;
        reducedFootprints.removeAt(index);
        const Polygons reducedCoverage =
            unionPolygons(reducedFootprints);
        const Polygons reducedResidual =
            differencePolygons(
                mustCover, reducedCoverage);
        const double reducedResidualArea =
            polygonSetArea(reducedResidual);
        const Placement &placement = placements[index];
        result.push_back({
            placement.transform,
            index,
            placement.shapeId,
            std::max(
                0.0,
                reducedResidualArea
                    - currentState.residualArea),
        });
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const PruneCandidate &left,
           const PruneCandidate &right) {
            if (std::abs(left.uniqueArea - right.uniqueArea)
                > kGeometryEpsilon) {
                return left.uniqueArea < right.uniqueArea;
            }
            if (left.shapeId != right.shapeId) {
                return left.shapeId < right.shapeId;
            }
            if (lexicographicTransformLess(
                    left.transform, right.transform)) {
                return true;
            }
            if (lexicographicTransformLess(
                    right.transform, left.transform)) {
                return false;
            }
            return left.index < right.index;
        });

    return result;
}

QVector<PruneNeighbor> pruneNeighbors(
    const QPolygonF &removedFootprint,
    const ExactCoverState &trialState) {
    QVector<PruneNeighbor> result;
    const Polygons removed{removedFootprint};
    const QRectF removedBounds =
        removedFootprint.boundingRect();
    for (int index = 0;
         index < trialState.footprints.size(); ++index) {
        const QPolygonF &footprint =
            trialState.footprints[index];
        const QRectF footprintBounds =
            footprint.boundingRect();
        if (!footprintBounds.intersects(removedBounds)) {
            continue;
        }
        const double overlapArea = polygonSetArea(
            intersectionPolygons(
                Polygons{footprint}, removed));
        const QPointF centerDelta =
            footprintBounds.center()
            - removedBounds.center();
        result.push_back({
            index,
            overlapArea,
            QPointF::dotProduct(
                centerDelta, centerDelta),
        });
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const PruneNeighbor &left,
           const PruneNeighbor &right) {
            if (std::abs(
                    left.overlapArea - right.overlapArea)
                > kGeometryEpsilon) {
                return left.overlapArea > right.overlapArea;
            }
            if (std::abs(
                    left.distanceSquared
                    - right.distanceSquared)
                > kGeometryEpsilon) {
                return left.distanceSquared
                    < right.distanceSquared;
            }
            return left.index < right.index;
        });

    return result;
}

void accumulateCandidateProfile(
    const CandidateProfile &candidateProfile,
    FillProfile *profile) {
    const qint64 workerNanoseconds =
        candidateProfile.totalNanoseconds > 0
        ? candidateProfile.totalNanoseconds
        : candidateProfile.legalizationNanoseconds;
    profile->candidateWorkerSeconds +=
        static_cast<double>(
            workerNanoseconds) * 1e-9;
    profile->adamEvaluationWorkerSeconds +=
        static_cast<double>(
            candidateProfile.adamEvaluationNanoseconds) * 1e-9;
    profile->legalizationWorkerSeconds +=
        static_cast<double>(
            candidateProfile.legalizationNanoseconds) * 1e-9;
    profile->adamEvaluations +=
        candidateProfile.adamEvaluations;
    profile->legalizationEvaluations +=
        candidateProfile.legalizationEvaluations;
}

Candidate optimizeExistingPlacement(
    const Placement &placement,
    const QVector<ShapeMesh> &catalog,
    const Polygons &subject,
    const Polygons &target,
    const Polygons &legalEnvelope,
    const QVector<ContourFeature> &features,
    const FillOptions &options,
    GpuAreaEvaluator *gpuEvaluator,
    QThreadPool *candidatePool,
    FillProfile *profile,
    const std::function<bool()> &cancelled,
    bool *wasCancelled) {
    Candidate result;
    const ShapeMesh *shape =
        shapeById(catalog, placement.shapeId);
    if (shape == nullptr || subject.isEmpty()) {
        return result;
    }
    CandidateJob job;
    job.shape = shape;
    job.featureAssignments =
        placementFeatureAssignments(
            *shape,
            placement.transform,
            placement.ownedFeatureIds,
            features);
    job.transform = placement.transform;
    job.anchor = placement.anchor;
    job.hasTransform = true;
    const std::vector<CandidateJob> jobs{job};
    const EvaluationBounds subjectBounds{
        individualPolygonBounds(subject),
        individualPolygonBounds(target),
    };
    const DistanceSeed seed = distanceSeed(subject);
    ++profile->candidateJobs;
    QElapsedTimer batchTimer;
    batchTimer.start();
    bool usedGpu = false;
    bool gpuCancelled = false;
    if (gpuEvaluator != nullptr
        && gpuEvaluator->available()
        && gpuEvaluator->setSubjects(
            subject, target)) {
        std::vector<Candidate> gpuResults;
        usedGpu = optimizeCandidatesGpu(
            jobs, subject,
            target, legalEnvelope,
            subjectBounds,
            options, seed, gpuEvaluator, candidatePool,
            profile, cancelled, &gpuResults,
            &gpuCancelled);
        if (usedGpu && !gpuResults.empty()
            && gpuResults.front().valid) {
            CandidateProfile exactProfile;
            result = legalCandidate(
                *shape, gpuResults.front().transform,
                subject, target,
                legalEnvelope,
                subjectBounds,
                options, &exactProfile,
                job.featureAssignments,
                job.anchor.has_value()
                    ? &*job.anchor : nullptr);
            accumulateCandidateProfile(
                exactProfile, profile);
        }
    }
    if (!usedGpu && !gpuCancelled) {
        const CandidateJobResult cpuResult =
            optimizeCandidate(
                job, subject, target,
                legalEnvelope,
                subjectBounds,
                options, seed, cancelled);
        result = cpuResult.candidate;
        accumulateCandidateProfile(
            cpuResult.profile, profile);
    }
    profile->candidateBatchWallSeconds +=
        static_cast<double>(
            batchTimer.nsecsElapsed()) * 1e-9;
    *wasCancelled = gpuCancelled
        || (cancelled && cancelled());

    return result;
}

bool sameTransform(
    const Affine &left,
    const Affine &right) {
    const auto leftValues = affineValues(left);
    const auto rightValues = affineValues(right);
    for (int index = 0;
         index < kGradientCount; ++index) {
        if (std::abs(
                leftValues[index] - rightValues[index])
            > kGeometryEpsilon) {
            return false;
        }
    }

    return true;
}

bool acceptablePruneState(
    const ExactCoverState &state,
    const CoverErrorMetrics &metrics,
    const CoverErrorMetrics &acceptedMetrics,
    double residualLimit,
    double outsideLimit,
    const FillOptions &options) {
    return state.residualArea
            <= residualLimit + kGeometryEpsilon
        && state.outsideArea
            <= outsideLimit + kGeometryEpsilon
        && metrics.outsideTargetArea
            <= acceptedMetrics.outsideTargetArea
                + options.epsSpill
                + kGeometryEpsilon
        && legalEnvelopeArea(
            metrics.outsideEnvelopeArea,
            options)
        && legalOutwardDistance(
            metrics.maximumOutwardDistance,
            options);
}

bool acceptableNudgeMetrics(
    const ExactCoverState &state,
    const CoverErrorMetrics &metrics,
    const ExactCoverState &acceptedState,
    const CoverErrorMetrics &acceptedMetrics,
    double minimumAreaImprovement,
    const FillOptions &options) {
    const double distanceAllowance =
        options.boundaryTolerance
        * kNudgeDistanceAllowanceRatio;

    return state.residualArea
            <= acceptedState.residualArea
                - minimumAreaImprovement
        && state.outsideArea
            <= acceptedState.outsideArea
                + kGeometryEpsilon
        && metrics.outsideTargetArea
            <= acceptedMetrics.outsideTargetArea
                + kGeometryEpsilon
        && legalEnvelopeArea(
            metrics.outsideEnvelopeArea,
            options)
        && legalOutwardDistance(
            metrics.maximumOutwardDistance,
            options)
        && metrics.maximumOutwardDistance
            <= acceptedMetrics.maximumOutwardDistance
                + kGeometryEpsilon
        && metrics.meanBoundaryDistance
            <= acceptedMetrics.meanBoundaryDistance
                + distanceAllowance
        && metrics.boundaryDistanceRms
            <= acceptedMetrics.boundaryDistanceRms
                + distanceAllowance
        && metrics.boundaryDistance95
            <= acceptedMetrics.boundaryDistance95
                + distanceAllowance
        && metrics.boundaryFScore
            >= acceptedMetrics.boundaryFScore
                - kGeometryEpsilon
        && metrics.representedFeatureWeight
            >= acceptedMetrics.representedFeatureWeight
                - kGeometryEpsilon
        && metrics.tversky
            >= acceptedMetrics.tversky
                - kGeometryEpsilon;
}

bool acceptablePruneContinuity(
    const ContinuityMetrics &metrics,
    const ContinuityMetrics &acceptedMetrics) {
    return metrics.kinkCount
            <= acceptedMetrics.kinkCount
        && metrics.maximumTurn
            <= acceptedMetrics.maximumTurn
                + kContinuityAngleAllowance
        && metrics.energy
            <= acceptedMetrics.energy
                + kContinuityMinimumEnergyImprovement;
}

bool nudgePlacements(
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
    const std::function<void(const FillProgress &)> &progress) {
    QElapsedTimer nudgeTimer;
    nudgeTimer.start();
    FillOptions nudgeOptions = options;
    CoverErrorMetrics acceptedMetrics =
        evaluateCoverMetrics(
            mustCover, mayCover,
            currentState->coverage,
            features, options);
    const double minimumAreaImprovement =
        std::max(
            kGeometryEpsilon,
            options.epsArea
                * kNudgeAreaImprovementRatio);
    bool changed = false;

    nudgeOptions.adamIterations =
        std::min(
            options.adamIterations,
            kNudgeAdamIterations);
    for (int offset = 0;
         offset < placements->size(); ++offset) {
        if (cancelled && cancelled()) {
            profile->nudgeWallSeconds +=
                static_cast<double>(
                    nudgeTimer.nsecsElapsed()) * 1e-9;
            return false;
        }
        const int index = placements->size()
            - offset - 1;
        Polygons fixedFootprints =
            currentState->footprints;
        fixedFootprints.removeAt(index);
        const Polygons fixedCoverage =
            unionPolygons(fixedFootprints);
        const Polygons subject =
            differencePolygons(
                mustCover, fixedCoverage);
        if (polygonSetArea(subject)
            < options.epsGain) {
            continue;
        }
        ++profile->nudgeOptimizations;
        bool optimizationCancelled = false;
        const Candidate optimized =
            optimizeExistingPlacement(
                (*placements)[index],
                catalog, subject,
                mustCover, mayCover,
                features,
                nudgeOptions,
                gpuEvaluator, candidatePool,
                profile, cancelled,
                &optimizationCancelled);
        if (optimizationCancelled) {
            profile->nudgeWallSeconds +=
                static_cast<double>(
                    nudgeTimer.nsecsElapsed()) * 1e-9;
            return false;
        }
        if (!optimized.valid
            || sameTransform(
                optimized.transform,
                (*placements)[index].transform)) {
            continue;
        }
        QVector<Placement> trial = *placements;
        trial[index].transform =
            optimized.transform;
        const ExactCoverState trialState =
            exactCoverState(
                trial, catalog,
                mustCover, mayCover,
                targetArea);
        if (trialState.residualArea
                > currentState->residualArea
                    - minimumAreaImprovement
            || trialState.outsideArea
                > currentState->outsideArea
                    + kGeometryEpsilon) {
            continue;
        }
        const CoverErrorMetrics trialMetrics =
            evaluateCoverMetrics(
                mustCover, mayCover,
                trialState.coverage,
                features, options);
        if (!acceptableNudgeMetrics(
                trialState, trialMetrics,
                *currentState, acceptedMetrics,
                minimumAreaImprovement,
                options)) {
            continue;
        }
        *placements = std::move(trial);
        *currentState = trialState;
        acceptedMetrics = trialMetrics;
        assignFeatureOwnership(
            placements, catalog,
            currentState->coverage,
            features);
        ++profile->nudgedPlacements;
        changed = true;
    }
    if (changed) {
        refreshPlacementGains(
            placements, catalog, mustCover);
        if (progress) {
            progress({
                static_cast<int>(placements->size()),
                targetArea,
                currentState->coveredArea,
                currentState->residualArea,
                static_cast<double>(
                    elapsed->elapsed()) / 1000.0,
            });
        }
    }
    profile->nudgeWallSeconds +=
        static_cast<double>(
            nudgeTimer.nsecsElapsed()) * 1e-9;

    return true;
}

bool stabilizePlacementContinuity(
    QVector<Placement> *placements,
    ExactCoverState *currentState,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    const QVector<ContourFeature> &features,
    double targetArea,
    const FillOptions &options,
    FillProfile *profile,
    QElapsedTimer *elapsed,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress) {
    QElapsedTimer continuityTimer;
    continuityTimer.start();
    ContinuityMetrics acceptedContinuity =
        contourContinuityMetrics(
            currentState->coverage,
            mustCover, features, options);
    CoverErrorMetrics acceptedMetrics =
        evaluateCoverMetrics(
            mustCover, mayCover,
            currentState->coverage,
            features, options);
    QVector<ContinuityPlacement> order;
    const double residualLimit =
        currentState->residualArea
        + std::max(
            options.epsArea,
            targetArea
                * kContinuityMaximumCoverageLossRatio);
    bool changed = false;

    profile->preContinuityEnergy =
        acceptedContinuity.energy;
    profile->preContinuityKinks =
        acceptedContinuity.kinkCount;
    order.reserve(placements->size());
    for (int index = 0;
         index < placements->size(); ++index) {
        const QVector<AlignmentEdge> edges =
            continuityAlignmentEdges(
                currentState->footprints[index],
                currentState->coverage,
                mustCover, features, options);
        double score = 0.0;
        for (const AlignmentEdge &edge : edges) {
            score += edge.score;
        }
        if (score > kGeometryEpsilon) {
            order.push_back({score, index});
        }
    }
    std::stable_sort(
        order.begin(), order.end(),
        [](const ContinuityPlacement &left,
           const ContinuityPlacement &right) {
            if (std::abs(
                    left.score - right.score)
                > kGeometryEpsilon) {
                return left.score > right.score;
            }
            return left.index < right.index;
        });
    if (order.size()
        > kMaximumContinuityPlacements) {
        order.resize(
            kMaximumContinuityPlacements);
    }

    for (const ContinuityPlacement &entry : order) {
        if (cancelled && cancelled()) {
            profile->continuityWallSeconds +=
                static_cast<double>(
                    continuityTimer.nsecsElapsed()) * 1e-9;
            return false;
        }
        const int index = entry.index;
        const QVector<Affine> proposals =
            continuityProposals(
                (*placements)[index],
                currentState->footprints[index],
                currentState->coverage,
                mustCover, features, options);
        Polygons fixedFootprints =
            currentState->footprints;
        fixedFootprints.removeAt(index);
        const Polygons fixedCoverage =
            unionPolygons(fixedFootprints);
        const double reducedResidualArea =
            polygonSetArea(
                differencePolygons(
                    mustCover,
                    fixedCoverage));
        const double uniqueArea =
            std::max(
                0.0,
                reducedResidualArea
                    - currentState->residualArea);
        const double placementResidualLimit =
            std::min(
                residualLimit,
                currentState->residualArea
                    + std::max(
                        options.epsArea,
                        uniqueArea
                            * kContinuityPlacementCoverageLossRatio));
        QVector<Placement> bestPlacements;
        ExactCoverState bestState;
        CoverErrorMetrics bestMetrics;
        ContinuityMetrics bestContinuity =
            acceptedContinuity;
        bool haveBest = false;
        for (const Affine &proposal : proposals) {
            if (cancelled && cancelled()) {
                profile->continuityWallSeconds +=
                    static_cast<double>(
                        continuityTimer.nsecsElapsed()) * 1e-9;
                return false;
            }
            ++profile->continuityProposals;
            QVector<Placement> trial = *placements;
            trial[index].transform = proposal;
            ExactCoverState trialState =
                exactCoverState(
                    trial, catalog,
                    mustCover, mayCover,
                    targetArea);
            if (trialState.residualArea
                    > placementResidualLimit
                        + kGeometryEpsilon
                || trialState.outsideArea
                    > currentState->outsideArea
                        + kGeometryEpsilon) {
                continue;
            }
            const ContinuityMetrics trialContinuity =
                contourContinuityMetrics(
                    trialState.coverage,
                    mustCover, features,
                    options);
            if (!betterContinuity(
                    trialContinuity,
                    acceptedContinuity)
                || trialContinuity.maximumTurn
                    > acceptedContinuity.maximumTurn
                        + kGeometryEpsilon) {
                continue;
            }
            const CoverErrorMetrics trialMetrics =
                evaluateCoverMetrics(
                    mustCover, mayCover,
                    trialState.coverage,
                    features, options);
            const double distanceAllowance =
                options.boundaryTolerance
                * kContinuityBoundaryDistanceAllowanceFactor;
            if (trialMetrics.outsideTargetArea
                    > acceptedMetrics.outsideTargetArea
                        + kGeometryEpsilon
                || !legalEnvelopeArea(
                    trialMetrics.outsideEnvelopeArea,
                    options)
                || !legalOutwardDistance(
                    trialMetrics.maximumOutwardDistance,
                    options)
                || trialMetrics.maximumOutwardDistance
                    > acceptedMetrics.maximumOutwardDistance
                        + kGeometryEpsilon
                || trialMetrics.meanBoundaryDistance
                    > acceptedMetrics.meanBoundaryDistance
                        + distanceAllowance
                || trialMetrics.boundaryDistance95
                    > acceptedMetrics.boundaryDistance95
                        + distanceAllowance
                || trialMetrics.representedFeatureWeight
                    + kGeometryEpsilon
                    < acceptedMetrics
                          .representedFeatureWeight) {
                continue;
            }
            if (!haveBest
                || betterContinuity(
                    trialContinuity,
                    bestContinuity)
                || (!betterContinuity(
                        bestContinuity,
                        trialContinuity)
                    && trialState.residualArea
                        < bestState.residualArea
                            - kGeometryEpsilon)) {
                bestPlacements =
                    std::move(trial);
                bestState =
                    std::move(trialState);
                bestMetrics = trialMetrics;
                bestContinuity =
                    trialContinuity;
                haveBest = true;
            }
        }
        if (!haveBest) {
            continue;
        }
        *placements =
            std::move(bestPlacements);
        *currentState =
            std::move(bestState);
        acceptedMetrics = bestMetrics;
        acceptedContinuity =
            bestContinuity;
        assignFeatureOwnership(
            placements, catalog,
            currentState->coverage,
            features);
        ++profile->stabilizedPlacements;
        changed = true;
    }
    if (changed) {
        refreshPlacementGains(
            placements, catalog, mustCover);
        if (progress) {
            progress({
                static_cast<int>(placements->size()),
                targetArea,
                currentState->coveredArea,
                currentState->residualArea,
                static_cast<double>(
                    elapsed->elapsed()) / 1000.0,
            });
        }
    }
    profile->postContinuityEnergy =
        acceptedContinuity.energy;
    profile->postContinuityKinks =
        acceptedContinuity.kinkCount;
    profile->continuityWallSeconds +=
        static_cast<double>(
            continuityTimer.nsecsElapsed()) * 1e-9;

    return true;
}

bool prunePlacements(
    QVector<Placement> *placements,
    ExactCoverState *currentState,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    const QVector<ContourFeature> &features,
    double targetArea,
    const FillOptions &options,
    FillProfile *profile,
    QElapsedTimer *elapsed,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress) {
    QElapsedTimer pruneTimer;
    pruneTimer.start();
    const double residualLimit =
        std::max(
            currentState->residualArea
                + options.epsArea,
            targetArea
                * (1.0
                   - kStructuralMinimumCompactCoverageRatio));
    const double outsideLimit =
        currentState->outsideArea + options.epsSpill;
    const CoverErrorMetrics acceptedMetrics =
        evaluateCoverMetrics(
            mustCover, mayCover,
            currentState->coverage,
            features, options);
    ContinuityMetrics acceptedContinuity =
        contourContinuityMetrics(
            currentState->coverage,
            mustCover, features, options);
    const FeatureMatchSummary acceptedFeatureMatches =
        matchContourFeatures(
            currentState->coverage,
            features);
    QSet<int> requiredCornerFeatureIds;
    for (const int featureId :
         acceptedFeatureMatches.representedIds) {
        const auto found = std::find_if(
            features.cbegin(), features.cend(),
            [featureId](
                const ContourFeature &feature) {
                return feature.id == featureId;
            });
        if (found != features.cend()
            && found->kind
                == ContourFeatureKind::Corner) {
            requiredCornerFeatureIds.insert(
                featureId);
        }
    }
    const auto acceptableTrial =
        [&](const ExactCoverState &state,
            const CoverErrorMetrics &metrics,
            const ContinuityMetrics &continuity) {
            const FeatureMatchSummary featureMatches =
                matchContourFeatures(
                    state.coverage,
                    features);
            const QSet<int> representedIds(
                featureMatches.representedIds.cbegin(),
                featureMatches.representedIds.cend());
            return acceptablePruneState(
                    state, metrics,
                    acceptedMetrics,
                    residualLimit,
                    outsideLimit,
                    options)
                && acceptablePruneContinuity(
                    continuity,
                    acceptedContinuity)
                && std::all_of(
                    requiredCornerFeatureIds.cbegin(),
                    requiredCornerFeatureIds.cend(),
                    [&](int featureId) {
                        return representedIds.contains(
                            featureId);
                    });
        };
    while (!placements->isEmpty()) {
        if (cancelled && cancelled()) {
            profile->pruneWallSeconds +=
                static_cast<double>(
                    pruneTimer.nsecsElapsed()) * 1e-9;
            return false;
        }
        ++profile->prunePasses;
        const QVector<PruneCandidate> order =
            pruneCandidateOrder(
                *placements, mustCover,
                *currentState);
        bool removedInPass = false;
        for (const PruneCandidate &pruneCandidate : order) {
            if (cancelled && cancelled()) {
                profile->pruneWallSeconds +=
                    static_cast<double>(
                        pruneTimer.nsecsElapsed()) * 1e-9;
                return false;
            }
            ++profile->pruneAttempts;
            QVector<Placement> trial = *placements;
            const Placement removed =
                trial.takeAt(pruneCandidate.index);
            const ShapeMesh *removedShape =
                shapeById(catalog, removed.shapeId);
            if (removedShape == nullptr) {
                continue;
            }
            const QPolygonF removedFootprint =
                transformedBoundary(
                    *removedShape, removed.transform);
            ExactCoverState trialState =
                exactCoverState(
                    trial, catalog, mustCover,
                    mayCover, targetArea);
            CoverErrorMetrics trialMetrics =
                evaluateCoverMetrics(
                    mustCover, mayCover,
                    trialState.coverage,
                    features, options);
            ContinuityMetrics trialContinuity =
                contourContinuityMetrics(
                    trialState.coverage,
                    mustCover, features,
                    options);
            QSet<int> adjustedIndices;
            if (!acceptableTrial(
                    trialState,
                    trialMetrics,
                    trialContinuity)) {
                const QVector<PruneNeighbor> neighbors =
                    pruneNeighbors(
                        removedFootprint, trialState);
                for (const PruneNeighbor &neighbor : neighbors) {
                    const QVector<Affine> proposals =
                        continuityProposals(
                            trial[neighbor.index],
                            trialState.footprints[
                                neighbor.index],
                            trialState.coverage,
                            mustCover, features,
                            options);
                    if (proposals.isEmpty()) {
                        continue;
                    }
                    QVector<Placement> bestTrial;
                    ExactCoverState bestState;
                    CoverErrorMetrics bestMetrics;
                    ContinuityMetrics bestContinuity;
                    bool haveBest = false;
                    for (const Affine &proposal : proposals) {
                        if (cancelled && cancelled()) {
                            profile->pruneWallSeconds +=
                                static_cast<double>(
                                    pruneTimer.nsecsElapsed()) * 1e-9;
                            return false;
                        }
                        ++profile->continuityProposals;
                        QVector<Placement> adjustedTrial =
                            trial;
                        adjustedTrial[
                            neighbor.index].transform =
                                proposal;
                        ExactCoverState adjustedState =
                            exactCoverState(
                                adjustedTrial,
                                catalog,
                                mustCover,
                                mayCover,
                                targetArea);
                        CoverErrorMetrics adjustedMetrics =
                            evaluateCoverMetrics(
                                mustCover,
                                mayCover,
                                adjustedState.coverage,
                                features,
                                options);
                        ContinuityMetrics adjustedContinuity =
                            contourContinuityMetrics(
                                adjustedState.coverage,
                                mustCover,
                                features,
                                options);
                        if (!acceptableTrial(
                                adjustedState,
                                adjustedMetrics,
                                adjustedContinuity)) {
                            continue;
                        }
                        if (!haveBest
                            || betterContinuity(
                                adjustedContinuity,
                                bestContinuity)
                            || (!betterContinuity(
                                    bestContinuity,
                                    adjustedContinuity)
                                && adjustedState
                                       .residualArea
                                    < bestState
                                          .residualArea
                                        - kGeometryEpsilon)) {
                            bestTrial =
                                std::move(
                                    adjustedTrial);
                            bestState =
                                std::move(
                                    adjustedState);
                            bestMetrics =
                                adjustedMetrics;
                            bestContinuity =
                                adjustedContinuity;
                            haveBest = true;
                        }
                    }
                    if (!haveBest) {
                        continue;
                    }
                    trial = std::move(bestTrial);
                    trialState = std::move(bestState);
                    trialMetrics = bestMetrics;
                    trialContinuity =
                        bestContinuity;
                    adjustedIndices.insert(neighbor.index);
                    break;
                }
            }
            if (!acceptableTrial(
                    trialState,
                    trialMetrics,
                    trialContinuity)) {
                continue;
            }
            *placements = std::move(trial);
            *currentState = std::move(trialState);
            assignFeatureOwnership(
                placements, catalog,
                currentState->coverage,
                features);
            refreshPlacementGains(
                placements, catalog, mustCover);
            acceptedContinuity =
                trialContinuity;
            ++profile->prunedPlacements;
            profile->adjustedPlacements +=
                adjustedIndices.size();
            profile->stabilizedPlacements +=
                adjustedIndices.size();
            removedInPass = true;
            if (progress) {
                progress({
                    static_cast<int>(placements->size()),
                    targetArea,
                    currentState->coveredArea,
                    currentState->residualArea,
                    static_cast<double>(
                        elapsed->elapsed()) / 1000.0,
                });
            }
            break;
        }
        if (!removedInPass) {
            break;
        }
    }
    profile->postContinuityEnergy =
        acceptedContinuity.energy;
    profile->postContinuityKinks =
        acceptedContinuity.kinkCount;
    profile->pruneWallSeconds +=
        static_cast<double>(
            pruneTimer.nsecsElapsed()) * 1e-9;

    return true;
}

std::uint64_t derivedSeed(const Polygons &polygons) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const QPolygonF &polygon : polygons) {
        for (const QPointF &point : polygon) {
            const qint64 x = std::llround(point.x() * kClipperScale);
            const qint64 y = std::llround(point.y() * kClipperScale);
            result ^= static_cast<std::uint64_t>(x);
            result *= 1099511628211ULL;
            result ^= static_cast<std::uint64_t>(y);
            result *= 1099511628211ULL;
        }
    }

    return result;
}

bool validOptions(const FillOptions &options) {
    return options.budget > 0 && options.adamIterations > 0
        && options.restarts >= 0
        && std::isfinite(options.spillWeight) && options.spillWeight > 0.0
        && std::isfinite(options.epsArea) && options.epsArea >= 0.0
        && std::isfinite(options.epsGain) && options.epsGain > 0.0
        && std::isfinite(options.epsSpill) && options.epsSpill >= 0.0
        && std::isfinite(options.adamLearningRate)
        && options.adamLearningRate > 0.0
        && std::isfinite(
            options.inactivityTimeoutSeconds)
        && options.inactivityTimeoutSeconds >= 0.0
        && std::isfinite(
            options.boundaryTolerance)
        && options.boundaryTolerance > 0.0
        && std::isfinite(
            options.areaWindowRatio)
        && options.areaWindowRatio > 0.0
        && options.areaWindowRatio <= 1.0
        && std::isfinite(
            options.tverskyAlpha)
        && options.tverskyAlpha >= 0.0
        && std::isfinite(
            options.tverskyBeta)
        && options.tverskyBeta > 0.0
        && std::isfinite(
            options.featureWeight)
        && options.featureWeight >= 0.0
        && options.featureRestarts >= 0;
}

void mergeRepairProfile(
    const FillProfile &repair,
    FillProfile *profile) {
    profile->greedySetupWallSeconds +=
        repair.greedySetupWallSeconds;
    profile->candidateBatchWallSeconds +=
        repair.candidateBatchWallSeconds;
    profile->candidateWorkerSeconds +=
        repair.candidateWorkerSeconds;
    profile->adamEvaluationWorkerSeconds +=
        repair.adamEvaluationWorkerSeconds;
    profile->legalizationWorkerSeconds +=
        repair.legalizationWorkerSeconds;
    profile->residualUpdateWallSeconds +=
        repair.residualUpdateWallSeconds;
    profile->finalMeasurementWallSeconds +=
        repair.finalMeasurementWallSeconds;
    profile->gpuEvaluationWallSeconds +=
        repair.gpuEvaluationWallSeconds;
    profile->candidateJobs += repair.candidateJobs;
    profile->adamEvaluations += repair.adamEvaluations;
    profile->legalizationEvaluations +=
        repair.legalizationEvaluations;
    profile->gpuBatches += repair.gpuBatches;
    profile->gpuIntersectionTasks +=
        repair.gpuIntersectionTasks;
    profile->wholeComponentJobs +=
        repair.wholeComponentJobs;
    profile->hardEdgeCandidates +=
        repair.hardEdgeCandidates;
    profile->featureCandidateJobs +=
        repair.featureCandidateJobs;
    profile->featureCandidateRejections +=
        repair.featureCandidateRejections;
    profile->selectionInsufficientGainRejections +=
        repair.selectionInsufficientGainRejections;
    profile->selectionEnvelopeRejections +=
        repair.selectionEnvelopeRejections;
    profile->selectionOutwardDistanceRejections +=
        repair.selectionOutwardDistanceRejections;
    profile->selectionFeatureRejections +=
        repair.selectionFeatureRejections;
    profile->complexitySelections +=
        repair.complexitySelections;
    profile->localComponentPlacements +=
        repair.localComponentPlacements;
    profile->wholeComponentPlacements +=
        repair.wholeComponentPlacements;
    profile->hardEdgePlacements +=
        repair.hardEdgePlacements;
    profile->featureSelectedPlacements +=
        repair.featureSelectedPlacements;
    profile->workerThreads = std::max(
        profile->workerThreads,
        repair.workerThreads);
    if (profile->gpuError.isEmpty()) {
        profile->gpuError = repair.gpuError;
    }
}

} // namespace gui::cover
