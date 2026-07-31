#include "differential_cover_internal.h"

#include <algorithm>
#include <cmath>

namespace gui::cover {

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
    job.hasTransform = true;
    const std::vector<CandidateJob> jobs{job};
    const EvaluationBounds subjectBounds{
        individualPolygonBounds(subject),
        individualPolygonBounds(target),
    };
    const DistanceSeed seed = distanceSeed(subject);
    ++profile->candidateJobs;
    ++profile->pruneOptimizations;
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
                options, &exactProfile);
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
    double targetArea,
    const FillOptions &options) {
    const double tverskyAllowance =
        options.epsArea
        / std::max(
            targetArea,
            kGeometryEpsilon);

    return state.residualArea
            <= residualLimit + kGeometryEpsilon
        && state.outsideArea
            <= outsideLimit + kGeometryEpsilon
        && legalEnvelopeArea(
            metrics.outsideEnvelopeArea,
            options)
        && legalOutwardDistance(
            metrics.maximumOutwardDistance,
            options)
        && metrics.representedFeatureWeight
            >= acceptedMetrics
                   .representedFeatureWeight
                - kGeometryEpsilon
        && metrics.tversky
            >= acceptedMetrics.tversky
                - tverskyAllowance
                - kGeometryEpsilon;
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
    GpuAreaEvaluator *gpuEvaluator,
    QThreadPool *candidatePool,
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
            QSet<int> adjustedIndices;
            if (!acceptablePruneState(
                    trialState,
                    trialMetrics,
                    acceptedMetrics,
                    residualLimit,
                    outsideLimit,
                    targetArea,
                    options)) {
                const QVector<PruneNeighbor> neighbors =
                    pruneNeighbors(
                        removedFootprint, trialState);
                for (const PruneNeighbor &neighbor : neighbors) {
                    Polygons fixedFootprints =
                        trialState.footprints;
                    fixedFootprints.removeAt(
                        neighbor.index);
                    const Polygons fixedCoverage =
                        unionPolygons(fixedFootprints);
                    const Polygons fixedResidual =
                        differencePolygons(
                            mustCover, fixedCoverage);
                    bool optimizationCancelled = false;
                    const Candidate optimized =
                        optimizeExistingPlacement(
                            trial[neighbor.index],
                            catalog, fixedResidual,
                            mustCover, mayCover,
                            features,
                            options,
                            gpuEvaluator, candidatePool,
                            profile, cancelled,
                            &optimizationCancelled);
                    if (optimizationCancelled) {
                        profile->pruneWallSeconds +=
                            static_cast<double>(
                                pruneTimer.nsecsElapsed()) * 1e-9;
                        return false;
                    }
                    if (!optimized.valid
                        || sameTransform(
                            optimized.transform,
                            trial[neighbor.index].transform)) {
                        continue;
                    }
                    QVector<Placement> adjustedTrial = trial;
                    adjustedTrial[neighbor.index].transform =
                        optimized.transform;
                    ExactCoverState adjustedState =
                        exactCoverState(
                            adjustedTrial, catalog,
                            mustCover, mayCover,
                            targetArea);
                    const CoverErrorMetrics
                        adjustedMetrics =
                            evaluateCoverMetrics(
                                mustCover,
                                mayCover,
                                adjustedState.coverage,
                                features,
                                options);
                    if (adjustedState.residualArea
                            >= trialState.residualArea
                                - kGeometryEpsilon
                        || adjustedState.outsideArea
                            > outsideLimit
                                + kGeometryEpsilon
                        || !acceptablePruneState(
                            adjustedState,
                            adjustedMetrics,
                            acceptedMetrics,
                            residualLimit,
                            outsideLimit,
                            targetArea,
                            options)) {
                        continue;
                    }
                    trial = std::move(adjustedTrial);
                    trialState = std::move(adjustedState);
                    trialMetrics =
                        adjustedMetrics;
                    adjustedIndices.insert(neighbor.index);
                    if (acceptablePruneState(
                            trialState,
                            trialMetrics,
                            acceptedMetrics,
                            residualLimit,
                            outsideLimit,
                            targetArea,
                            options)) {
                        break;
                    }
                }
            }
            if (!acceptablePruneState(
                    trialState,
                    trialMetrics,
                    acceptedMetrics,
                    residualLimit,
                    outsideLimit,
                    targetArea,
                    options)) {
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
            ++profile->prunedPlacements;
            profile->adjustedPlacements +=
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
