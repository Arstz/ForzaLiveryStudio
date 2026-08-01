#include "differential_cover_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <random>

namespace gui::cover {

FillResult analyticCoverFillInternal(
    const FillInput &input,
    const QVector<ShapeMesh> &catalog,
    const FillOptions &options,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress,
    bool postProcess) {
    FillResult result;
    result.profile.areaWindowRatio =
        options.areaWindowRatio;
    const Polygons mustCover = normalizedInputPolygons(input.mustCover);
    const Polygons mayCover = normalizedInputPolygons(input.mayCover);
    QVector<QVector<ContourSpan>> boundaryLoops = input.boundaryLoops;
    if (boundaryLoops.isEmpty() && !input.boundarySpans.isEmpty()) {
        boundaryLoops.push_back(input.boundarySpans);
    }
    QVector<ContourFeature> targetFeatures;
    if (options.useWeightedContour) {
        for (const QVector<ContourSpan> &loop : boundaryLoops) {
            QVector<ContourFeature> features = extractContourFeatures(
                loop, options.boundaryTolerance);
            for (ContourFeature &feature : features) {
                feature.id = targetFeatures.size();
                targetFeatures.push_back(std::move(feature));
            }
        }
    }
    if (mustCover.isEmpty() || mayCover.isEmpty()) {
        result.error = QStringLiteral("Differential cover input is empty");
        return result;
    }
    if (!validOptions(options)) {
        result.error = QStringLiteral("Differential cover options are invalid");
        return result;
    }
    if (catalog.isEmpty()
        || std::any_of(catalog.cbegin(), catalog.cend(),
                       [](const ShapeMesh &shape) { return !shape.valid(); })) {
        result.error = QStringLiteral("Differential cover catalog is invalid");
        return result;
    }

    const double targetArea = polygonSetArea(mustCover);
    const auto measureResult =
        [&]() {
            const Polygons coverage =
                unionPolygons(
                    placementFootprints(
                        result.placements,
                        catalog));
            result.metrics =
                evaluateCoverMetrics(
                    mustCover, mayCover,
                    coverage, targetFeatures,
                    options);
            result.metrics.placementCount =
                result.placements.size();
            result.outsideArea =
                result.metrics.outsideTargetArea;
            assignFeatureOwnership(
                &result.placements,
                catalog, coverage,
                targetFeatures);
        };
    QElapsedTimer elapsed;
    elapsed.start();
    if (progress) {
        progress({
            0,
            targetArea,
            0.0,
            targetArea,
            0.0,
        });
    }
    StructuralCoverPlan structural =
        structuralCoverPlan(
            boundaryLoops, catalog,
            mustCover, mayCover,
            targetArea, options, cancelled);
    const bool compactStructuralCover =
        structural.coverageRatio
        >= kStructuralMinimumCompactCoverageRatio
            - kGeometryEpsilon;
    if (structural.accepted
        && !compactStructuralCover
        && !targetFeatures.isEmpty()) {
        const Polygons coverage =
            unionPolygons(
                placementFootprints(
                    structural.placements,
                    catalog));
        const CoverErrorMetrics metrics =
            evaluateCoverMetrics(
                mustCover, mayCover,
                coverage, targetFeatures,
                options);
        const double requiredFeatureWeight =
            metrics.totalFeatureWeight * 0.9;
        if (!legalEnvelopeArea(
                metrics.outsideEnvelopeArea,
                options)
            || !legalOutwardDistance(
                metrics.maximumOutwardDistance,
                options)
            || metrics.representedFeatureWeight
                < requiredFeatureWeight
                    - kGeometryEpsilon) {
            structural.accepted = false;
            structural.seeded =
                structural.coverageRatio
                    >= kStructuralMinimumSeedCoverageRatio;
            structural.reason =
                QStringLiteral(
                    "structural contour metrics require weighted refinement");
        }
    }
    result.profile.structuralReason =
        structural.reason;
    result.profile.structuralExplainedBoundaryFraction =
        structural.explainedBoundaryFraction;
    result.profile.structuralCoverageRatio =
        structural.coverageRatio;
    result.profile.structuralResidualArea =
        structural.residualArea;
    result.profile.structuralResidualThickness =
        structural.residualThickness;
    result.profile.structuralOutsideArea =
        structural.outsideArea;
    result.profile.structuralGridCells =
        structural.gridCells;
    result.profile.structuralRectangleCandidates =
        structural.rectangleCandidates;
    result.profile.structuralRectangles =
        structural.placements.size();
    result.profile.structuralAccepted =
        structural.accepted;
    result.profile.structuralSeeded =
        structural.seeded;
    if (structural.cancelled) {
        result.cancelled = true;
        result.residual = mustCover;
        result.residualArea = targetArea;
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        measureResult();
        return result;
    }
    if (structural.accepted) {
        result.placements =
            structural.placements;
        result.residual =
            structural.residual;
        result.residualArea =
            structural.residualArea;
        result.coveredArea =
            targetArea - result.residualArea;
        result.outsideArea =
            structural.outsideArea;
        result.stalled =
            result.residualArea > options.epsArea;
        result.profile.evaluationBackend =
            QStringLiteral("Structural cover");
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        if (progress) {
            progress({
                static_cast<int>(
                    result.placements.size()),
                targetArea,
                result.coveredArea,
                result.residualArea,
                static_cast<double>(
                    elapsed.elapsed()) / 1000.0,
            });
        }
        measureResult();
        return result;
    }
    if (structural.seeded) {
        result.placements =
            structural.placements;
        result.residual =
            structural.residual;
        if (progress) {
            progress({
                static_cast<int>(
                    result.placements.size()),
                targetArea,
                targetArea
                    - structural.residualArea,
                structural.residualArea,
                static_cast<double>(
                    elapsed.elapsed()) / 1000.0,
            });
        }
    } else {
        result.residual = mustCover;
    }
    const QVector<FixedCandidate> hardCandidates =
        hardEdgeCandidates(
            boundaryLoops.isEmpty()
                ? QVector<ContourSpan>{}
                : boundaryLoops.front(),
            catalog, cancelled);
    result.profile.hardEdgeCandidates =
        hardCandidates.size();
    MeshCoverPlan mesh =
        meshCoverPlan(
            hardCandidates, catalog,
            mustCover, mayCover,
            targetArea, options,
            cancelled);
    if (mesh.accepted
        && !targetFeatures.isEmpty()) {
        const Polygons coverage =
            unionPolygons(
                placementFootprints(
                    mesh.placements,
                    catalog));
        const CoverErrorMetrics metrics =
            evaluateCoverMetrics(
                mustCover, mayCover,
                coverage, targetFeatures,
                options);
        const double requiredFeatureWeight =
            metrics.totalFeatureWeight * 0.9;
        if (!legalEnvelopeArea(
                metrics.outsideEnvelopeArea,
                options)
            || !legalOutwardDistance(
                metrics.maximumOutwardDistance,
                options)
            || metrics.representedFeatureWeight
                < requiredFeatureWeight
                    - kGeometryEpsilon) {
            mesh.accepted = false;
            mesh.reason =
                QStringLiteral(
                    "mesh contour metrics require weighted refinement");
        }
    }
    result.profile.meshReason =
        mesh.reason;
    result.profile.meshCoverageRatio =
        mesh.coverageRatio;
    result.profile.meshResidualArea =
        mesh.residualArea;
    result.profile.meshOutsideArea =
        mesh.outsideArea;
    result.profile.meshScale =
        mesh.scale;
    result.profile.meshPlacements =
        mesh.placements.size();
    result.profile.meshAccepted =
        mesh.accepted;
    if (mesh.cancelled) {
        result.cancelled = true;
        result.residual = mustCover;
        result.residualArea = targetArea;
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        measureResult();
        return result;
    }
    if (mesh.accepted) {
        result.placements =
            mesh.placements;
        result.residual =
            mesh.residual;
        result.residualArea =
            mesh.residualArea;
        result.coveredArea =
            targetArea - result.residualArea;
        result.outsideArea =
            mesh.outsideArea;
        result.stalled =
            result.residualArea > options.epsArea;
        result.profile.evaluationBackend =
            QStringLiteral("Polygon mesh");
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        if (progress) {
            progress({
                static_cast<int>(
                    result.placements.size()),
                targetArea,
                result.coveredArea,
                result.residualArea,
                static_cast<double>(
                    elapsed.elapsed()) / 1000.0,
            });
        }
        measureResult();
        return result;
    }
    const std::uint64_t seedValue = options.seed == 0
        ? derivedSeed(mustCover) : options.seed;
    std::mt19937_64 random(seedValue);
    QThreadPool candidatePool;
    candidatePool.setMaxThreadCount(
        std::max(1, QThread::idealThreadCount() - 1));
    result.profile.workerThreads = candidatePool.maxThreadCount();
    std::unique_ptr<GpuAreaEvaluator> gpuEvaluator =
        options.useGpu
            ? createGpuAreaEvaluator(catalog)
            : nullptr;
    const auto evaluationBackendName =
        [&](const QString &backend) {
            if (targetFeatures.isEmpty()) {
                return backend;
            }
            if (backend == QStringLiteral("CPU")) {
                return QStringLiteral(
                    "CPU weighted contour");
            }

            return backend
                + QStringLiteral(
                    " weighted contour");
        };
    if (gpuEvaluator != nullptr && gpuEvaluator->available()) {
        result.profile.evaluationBackend =
            evaluationBackendName(
                gpuEvaluator->stats().backend);
    } else {
        result.profile.evaluationBackend =
            evaluationBackendName(
                QStringLiteral("CPU"));
    }
    auto updateGpuProfile = [&]() {
        if (gpuEvaluator == nullptr) {
            return;
        }
        const GpuEvaluatorStats stats = gpuEvaluator->stats();
        result.profile.gpuAdapter = stats.adapter;
        result.profile.gpuError = stats.error;
        result.profile.gpuBatches = stats.batches;
        result.profile.gpuIntersectionTasks = stats.intersectionTasks;
        result.profile.gpuEvaluationWallSeconds = stats.wallSeconds;
        result.profile.evaluationBackend =
            evaluationBackendName(
                stats.backend);
        if (!stats.error.isEmpty()
            && stats.backend != QStringLiteral("CPU")) {
            result.profile.evaluationBackend +=
                QStringLiteral(" with fallback");
        }
    };
    Polygons currentCoverage =
        unionPolygons(
            placementFootprints(
                result.placements,
                catalog));
    FeatureMatchSummary currentFeatureMatches =
        matchContourFeatures(
            currentCoverage,
            targetFeatures);
    while (polygonSetArea(result.residual) > options.epsArea
           && result.placements.size() < options.budget) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.residualArea = polygonSetArea(result.residual);
            result.coveredArea = targetArea - result.residualArea;
            result.profile.totalWallSeconds =
                static_cast<double>(elapsed.nsecsElapsed()) * 1e-9;
            updateGpuProfile();
            measureResult();
            return result;
        }

        ++result.profile.greedySteps;
        QElapsedTimer setupTimer;
        setupTimer.start();
        const EvaluationBounds subjectBounds{
            individualPolygonBounds(result.residual),
            individualPolygonBounds(mustCover),
        };
        const DistanceSeed seed = distanceSeed(result.residual);
        const Polygons routedComponent =
            componentAtPoint(result.residual, seed.point);
        QVector<const ShapeMesh *> candidates =
            routedShapes(result.residual, catalog, options.useRouter);
        const ResidualComplexity structure =
            residualComplexity(result.residual);
        QSet<const ShapeMesh *> localFallbackShapes;
        if (options.useRouter && structure.components > 1) {
            const QVector<const ShapeMesh *> localCandidates =
                routedShapes(
                    routedComponent, catalog, options.useRouter);
            double globalComponentDistance =
                std::numeric_limits<double>::max();
            for (const ShapeMesh *shape : candidates) {
                globalComponentDistance = std::min(
                    globalComponentDistance,
                    descriptorDistance(
                        *shape, routedComponent));
            }
            for (const ShapeMesh *shape : localCandidates) {
                if (!candidates.contains(shape)
                    && descriptorDistance(
                           *shape, routedComponent)
                        < globalComponentDistance
                            * kLocalRouterAdvantage) {
                    candidates.push_back(shape);
                    localFallbackShapes.insert(shape);
                    break;
                }
            }
        }
        std::vector<CandidateJob> jobs;
        jobs.reserve(static_cast<size_t>(
            candidates.size() * (options.restarts + 1)));
        std::mt19937_64 localRandom;
        bool localRandomReady = false;
        for (const ShapeMesh *shape : candidates) {
            const bool local =
                localFallbackShapes.contains(shape);
            if (local && !localRandomReady) {
                localRandom = random;
                localRandomReady = true;
            }
            for (int restart = 0; restart <= options.restarts; ++restart) {
                CandidateJob job;
                job.shape = shape;
                job.initialization =
                    candidateInitialization(
                        seed, restart,
                        local ? &localRandom : &random);
                job.origin = local
                    ? CandidateOrigin::LocalComponent
                    : CandidateOrigin::Greedy;
                jobs.push_back(job);
            }
        }
        if (result.placements.isEmpty()) {
            const QVector<CandidateJob> componentJobs =
                wholeComponentJobs(result.residual, catalog);
            result.profile.wholeComponentJobs +=
                componentJobs.size();
            for (const CandidateJob &job : componentJobs) {
                jobs.push_back(job);
            }
        }
        const QVector<CandidateJob> featureJobs =
            featureAwareJobs(
                candidates, targetFeatures,
                currentFeatureMatches
                    .representedIds,
                seed,
                options.featureRestarts);
        result.profile.featureCandidateJobs +=
            featureJobs.size();
        for (const CandidateJob &job :
             featureJobs) {
            jobs.push_back(job);
        }
        result.profile.greedySetupWallSeconds +=
            static_cast<double>(setupTimer.nsecsElapsed()) * 1e-9;
        result.profile.candidateJobs += jobs.size();
        std::vector<CandidateJobResult> jobResults(jobs.size());
        QElapsedTimer candidateBatchTimer;
        candidateBatchTimer.start();
        bool usedGpu = false;
        bool gpuCancelled = false;
        if (gpuEvaluator != nullptr && gpuEvaluator->available()
            && gpuEvaluator->setSubjects(
                result.residual,
                mustCover)) {
            std::vector<Candidate> gpuResults;
            usedGpu = optimizeCandidatesGpu(
                jobs, result.residual,
                mustCover, mayCover,
                subjectBounds,
                options, seed, gpuEvaluator.get(), &candidatePool,
                &result.profile, cancelled,
                &gpuResults, &gpuCancelled);
            if (usedGpu) {
                for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
                    if (!gpuResults[jobIndex].valid) {
                        continue;
                    }
                    CandidateProfile exactProfile;
                    jobResults[jobIndex].candidate = legalCandidate(
                        *jobs[jobIndex].shape,
                        gpuResults[jobIndex].transform,
                        result.residual,
                        mustCover, mayCover,
                        subjectBounds,
                        options, &exactProfile,
                        jobs[jobIndex]
                            .featureAssignments);
                    jobResults[jobIndex].profile = exactProfile;
                }
            }
        }
        if (!usedGpu && !gpuCancelled) {
            for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
                candidatePool.start([&, jobIndex]() {
                    const CandidateJob &job = jobs[jobIndex];
                    jobResults[jobIndex] = optimizeCandidate(
                        job, result.residual,
                        mustCover, mayCover,
                        subjectBounds,
                        options, seed, cancelled);
                });
            }
            candidatePool.waitForDone();
        }
        QVector<Candidate> rankedCandidates;
        rankedCandidates.reserve(
            static_cast<qsizetype>(jobResults.size())
            + hardCandidates.size());
        for (size_t jobIndex = 0;
             jobIndex < jobResults.size(); ++jobIndex) {
            jobResults[jobIndex].candidate.origin =
                jobs[jobIndex].origin;
            rankedCandidates.push_back(
                jobResults[jobIndex].candidate);
        }
        for (const FixedCandidate &fixed : hardCandidates) {
            CandidateProfile fixedProfile;
            rankedCandidates.push_back(fixedCandidate(
                fixed, result.residual,
                mustCover, mayCover,
                subjectBounds, options, &fixedProfile));
            result.profile.candidateWorkerSeconds +=
                static_cast<double>(
                    fixedProfile.legalizationNanoseconds) * 1e-9;
            result.profile.legalizationWorkerSeconds +=
                static_cast<double>(
                    fixedProfile.legalizationNanoseconds) * 1e-9;
            result.profile.legalizationEvaluations +=
                fixedProfile.legalizationEvaluations;
        }
        result.profile.candidateBatchWallSeconds +=
            static_cast<double>(candidateBatchTimer.nsecsElapsed()) * 1e-9;
        for (const CandidateJobResult &jobResult : jobResults) {
            result.profile.candidateWorkerSeconds +=
                static_cast<double>(jobResult.profile.totalNanoseconds) * 1e-9;
            result.profile.adamEvaluationWorkerSeconds +=
                static_cast<double>(
                    jobResult.profile.adamEvaluationNanoseconds) * 1e-9;
            result.profile.legalizationWorkerSeconds +=
                static_cast<double>(
                    jobResult.profile.legalizationNanoseconds) * 1e-9;
            result.profile.adamEvaluations +=
                jobResult.profile.adamEvaluations;
            result.profile.legalizationEvaluations +=
                jobResult.profile.legalizationEvaluations;
        }
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.residualArea = polygonSetArea(result.residual);
            result.coveredArea = targetArea - result.residualArea;
            result.profile.totalWallSeconds =
                static_cast<double>(elapsed.nsecsElapsed()) * 1e-9;
            updateGpuProfile();
            measureResult();
            return result;
        }
        QElapsedTimer residualTimer;
        residualTimer.start();
        CandidateSelection selection = selectCandidate(
            rankedCandidates, catalog,
            mustCover, mayCover,
            currentCoverage,
            result.residual, targetFeatures,
            currentFeatureMatches,
            options);
        result.profile.featureCandidateRejections +=
            selection
                .featureCandidateRejections;
        result.profile
            .selectionInsufficientGainRejections +=
            selection
                .insufficientGainRejections;
        result.profile.selectionEnvelopeRejections +=
            selection.envelopeRejections;
        result.profile
            .selectionOutwardDistanceRejections +=
            selection
                .outwardDistanceRejections;
        result.profile.selectionFeatureRejections +=
            selection.featureRejections;
        result.profile.residualUpdateWallSeconds +=
            static_cast<double>(residualTimer.nsecsElapsed()) * 1e-9;
        if (!selection.valid) {
            result.stalled = true;
            break;
        }
        if (selection.complexityPreferred) {
            ++result.profile.complexitySelections;
        }
        if (selection.candidate.origin
            == CandidateOrigin::LocalComponent) {
            ++result.profile.localComponentPlacements;
        } else if (selection.candidate.origin
            == CandidateOrigin::WholeComponent) {
            ++result.profile.wholeComponentPlacements;
        } else if (selection.candidate.origin
                   == CandidateOrigin::HardEdge) {
            ++result.profile.hardEdgePlacements;
        }
        if (!selection
                 .newlyRepresentedFeatureIds
                 .isEmpty()) {
            ++result.profile
                  .featureSelectedPlacements;
        }

        result.placements.push_back({
            selection.candidate.transform,
            selection.candidate.shapeId,
            selection.exactGain,
            selection
                .newlyRepresentedFeatureIds,
            selection.exposedContourArc,
        });
        currentCoverage =
            std::move(selection.coverage);
        currentFeatureMatches =
            matchContourFeatures(
                currentCoverage,
                targetFeatures);
        result.residual = std::move(selection.residual);
        const double nextArea = polygonSetArea(result.residual);
        if (progress) {
            const double elapsedSeconds =
                static_cast<double>(elapsed.elapsed()) / 1000.0;
            progress({
                static_cast<int>(result.placements.size()),
                targetArea,
                std::max(0.0, targetArea - nextArea),
                nextArea,
                elapsedSeconds,
            });
        }
    }

    const bool greedyBudgetHit =
        result.placements.size() >= options.budget
        && polygonSetArea(result.residual) > options.epsArea;
    ExactCoverState coverState = exactCoverState(
        result.placements, catalog,
        mustCover, mayCover, targetArea);
    assignFeatureOwnership(
        &result.placements,
        catalog,
        coverState.coverage,
        targetFeatures);
    FillProfile repairProfile;
    bool haveRepairProfile = false;
    Polygons prePruneResidual;
    if (postProcess) {
        result.profile.prePruneResidualArea =
            coverState.residualArea;
        prePruneResidual = coverState.residual;
        if (!prunePlacements(
                &result.placements, &coverState,
                catalog, mustCover, mayCover,
                targetFeatures,
                targetArea, options, gpuEvaluator.get(),
                &candidatePool, &result.profile, &elapsed,
                cancelled, progress)) {
            result.cancelled = true;
        }
        result.profile.postPruneResidualArea =
            coverState.residualArea;
        Polygons repairTarget;
        const double compactResidualLimit =
            targetArea
            * (1.0
               - kStructuralMinimumCompactCoverageRatio);
        if (coverState.residualArea
                > compactResidualLimit
                    + kGeometryEpsilon) {
            repairTarget =
                differencePolygons(
                    coverState.residual,
                    prePruneResidual);
        }
        result.profile.repairTargetArea =
            polygonSetArea(repairTarget);
        const int remainingBudget =
            options.budget
            - static_cast<int>(
                result.placements.size());
        if (!result.cancelled
            && result.profile.repairTargetArea
                > options.epsArea
            && remainingBudget > 0) {
            FillInput repairInput;
            repairInput.mustCover = repairTarget;
            repairInput.mayCover = mayCover;
            FillOptions repairOptions = options;
            repairOptions.budget = remainingBudget;
            const int placementOffset =
                result.placements.size();
            const double coveredOffset =
                coverState.coveredArea;
            const auto repairProgress =
                [&, placementOffset, coveredOffset](
                    const FillProgress &update) {
                    if (!progress) {
                        return;
                    }
                    const double combinedCovered =
                        std::min(
                            targetArea,
                            coveredOffset
                                + update.coveredArea);
                    progress({
                        placementOffset
                            + update.placementCount,
                        targetArea,
                        combinedCovered,
                        std::max(
                            0.0,
                            targetArea - combinedCovered),
                        static_cast<double>(
                            elapsed.elapsed()) / 1000.0,
                    });
                };
            QElapsedTimer repairTimer;
            repairTimer.start();
            FillResult repair =
                analyticCoverFillInternal(
                    repairInput, catalog,
                    repairOptions, cancelled,
                    repairProgress, false);
            result.profile.repairWallSeconds =
                static_cast<double>(
                    repairTimer.nsecsElapsed()) * 1e-9;
            result.profile.repairPlacements =
                static_cast<int>(
                    repair.placements.size());
            result.profile.repairSteps =
                repair.profile.greedySteps;
            result.profile.repairCoveredArea =
                repair.coveredArea;
            repairProfile = repair.profile;
            haveRepairProfile = true;
            const CoverErrorMetrics
                preRepairMetrics =
                    evaluateCoverMetrics(
                        mustCover, mayCover,
                        coverState.coverage,
                        targetFeatures,
                        options);
            for (const Placement &placement :
                 repair.placements) {
                result.placements.push_back(
                    placement);
            }
            const Polygons repairedCoverage =
                unionPolygons(
                    placementFootprints(
                        result.placements,
                        catalog));
            const CoverErrorMetrics
                repairedMetrics =
                    evaluateCoverMetrics(
                        mustCover, mayCover,
                        repairedCoverage,
                        targetFeatures,
                        options);
            if (repairedMetrics
                    .representedFeatureWeight
                    + kGeometryEpsilon
                < preRepairMetrics
                      .representedFeatureWeight
                || !legalEnvelopeArea(
                    repairedMetrics
                        .outsideEnvelopeArea,
                    options)
                || !legalOutwardDistance(
                    repairedMetrics
                        .maximumOutwardDistance,
                    options)) {
                result.placements.resize(
                    placementOffset);
                result.profile.repairPlacements =
                    0;
                result.profile.repairCoveredArea =
                    0.0;
            }
            result.cancelled = repair.cancelled;
            result.stalled = repair.stalled;
            if (!repair.error.isEmpty()) {
                result.error = repair.error;
            }
        }
    }
    QElapsedTimer finalTimer;
    finalTimer.start();
    coverState = exactCoverState(
        result.placements, catalog,
        mustCover, mayCover, targetArea);
    result.residual = std::move(coverState.residual);
    result.residualArea = coverState.residualArea;
    result.coveredArea = coverState.coveredArea;
    result.outsideArea = coverState.outsideArea;
    measureResult();
    if (postProcess) {
        result.profile.postRepairNewGapArea =
            polygonSetArea(
                differencePolygons(
                    coverState.residual,
                    prePruneResidual));
    }
    result.budgetHit = greedyBudgetHit
        || (result.placements.size() >= options.budget
            && result.residualArea > options.epsArea);
    result.profile.finalMeasurementWallSeconds =
        static_cast<double>(finalTimer.nsecsElapsed()) * 1e-9;
    result.profile.totalWallSeconds =
        static_cast<double>(elapsed.nsecsElapsed()) * 1e-9;
    updateGpuProfile();
    if (haveRepairProfile) {
        mergeRepairProfile(
            repairProfile, &result.profile);
    }
    if (postProcess && progress
        && result.profile.repairPlacements > 0) {
        progress({
            static_cast<int>(
                result.placements.size()),
            targetArea,
            result.coveredArea,
            result.residualArea,
            static_cast<double>(
                elapsed.elapsed()) / 1000.0,
        });
    }

    return result;
}

FillResult analyticCoverFill(
    const FillInput &input,
    const QVector<ShapeMesh> &catalog,
    const FillOptions &options,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress) {
    using ActivityClock =
        std::chrono::steady_clock;
    const auto activityNanoseconds = []() {
        return std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                ActivityClock::now()
                    .time_since_epoch())
            .count();
    };
    std::atomic<qint64> lastActivity{
        activityNanoseconds(),
    };
    std::atomic_bool timedOut{false};
    int previousPlacementCount = -1;
    double previousCoveredArea = -1.0;
    const qint64 timeoutNanoseconds =
        static_cast<qint64>(
            options.inactivityTimeoutSeconds
            * 1e9);
    const auto stopRequested = [&]() {
        if (cancelled && cancelled()) {
            return true;
        }
        if (timeoutNanoseconds <= 0) {
            return false;
        }
        const bool expired =
            activityNanoseconds()
                - lastActivity.load(
                    std::memory_order_relaxed)
            >= timeoutNanoseconds;
        if (expired) {
            timedOut.store(
                true,
                std::memory_order_relaxed);
        }

        return expired;
    };
    const auto activityProgress =
        [&](const FillProgress &update) {
            const bool changed =
                update.placementCount
                    != previousPlacementCount
                || std::abs(
                       update.coveredArea
                       - previousCoveredArea)
                    > kGeometryEpsilon;
            previousPlacementCount =
                update.placementCount;
            previousCoveredArea =
                update.coveredArea;
            if (changed) {
                lastActivity.store(
                    activityNanoseconds(),
                    std::memory_order_relaxed);
            }
            if (progress) {
                progress(update);
            }
        };
    FillResult result =
        analyticCoverFillInternal(
            input, catalog, options,
            stopRequested,
            activityProgress, true);
    result.timedOut =
        timedOut.load(
            std::memory_order_relaxed);
    result.cancelled =
        result.cancelled || result.timedOut;

    return result;
}

} // namespace gui::cover
