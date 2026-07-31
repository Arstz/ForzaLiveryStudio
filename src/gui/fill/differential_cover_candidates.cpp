#include "differential_cover_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <tuple>
#include <utility>

namespace gui::cover {

QRectF shapeBounds(const ShapeMesh &shape) {
    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const Vec2 &point : shape.boundary) {
        minimumX = std::min(minimumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumX = std::max(maximumX, point.x);
        maximumY = std::max(maximumY, point.y);
    }

    return QRectF(QPointF(minimumX, minimumY),
                  QPointF(maximumX, maximumY));
}

Affine initialTransform(const ShapeMesh &shape,
                        const DistanceSeed &seed,
                        double angleOffset,
                        double scaleFactor,
                        const QPointF &translationOffset) {
    const QRectF bounds = shapeBounds(shape);
    const QPointF localCenter = bounds.center();
    double sourceRadius = 0.0;
    for (const Vec2 &point : shape.boundary) {
        sourceRadius = std::max(sourceRadius,
                                QLineF(QPointF(point.x, point.y), localCenter).length());
    }
    const double scale = std::max(
        kMinimumAffineScale,
        seed.radius * kInitialRadiusFraction
            / std::max(sourceRadius, kGeometryEpsilon) * scaleFactor);
    const double angle = seed.angle + angleOffset;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    Affine result;
    result.a = cosine * scale;
    result.b = sine * scale;
    result.c = -sine * scale;
    result.d = cosine * scale;
    result.e = seed.point.x() + translationOffset.x()
        - result.a * localCenter.x() - result.c * localCenter.y();
    result.f = seed.point.y() + translationOffset.y()
        - result.b * localCenter.x() - result.d * localCenter.y();

    return result;
}

Affine initialTransform(const CandidateJob &job,
                        const DistanceSeed &seed) {
    if (job.hasTransform) {
        return job.transform;
    }

    return initialTransform(
        *job.shape,
        seed,
        job.initialization.angleOffset,
        job.initialization.scaleFactor,
        job.initialization.translationOffset);
}

OrientedBounds orientedBounds(const QVector<QPointF> &points) {
    OrientedBounds result;
    if (points.size() < 3) {
        return result;
    }

    QPointF mean;
    for (const QPointF &point : points) {
        mean += point;
    }
    mean /= points.size();
    double covarianceXX = 0.0;
    double covarianceXY = 0.0;
    double covarianceYY = 0.0;
    for (const QPointF &point : points) {
        const QPointF centered = point - mean;
        covarianceXX += centered.x() * centered.x();
        covarianceXY += centered.x() * centered.y();
        covarianceYY += centered.y() * centered.y();
    }
    const double angle = 0.5 * std::atan2(
        2.0 * covarianceXY, covarianceXX - covarianceYY);
    result.axisX = QPointF(std::cos(angle), std::sin(angle));
    result.axisY = QPointF(-result.axisX.y(), result.axisX.x());
    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const QPointF &point : points) {
        const double projectedX =
            QPointF::dotProduct(point, result.axisX);
        const double projectedY =
            QPointF::dotProduct(point, result.axisY);
        minimumX = std::min(minimumX, projectedX);
        minimumY = std::min(minimumY, projectedY);
        maximumX = std::max(maximumX, projectedX);
        maximumY = std::max(maximumY, projectedY);
    }
    result.extentX = maximumX - minimumX;
    result.extentY = maximumY - minimumY;
    result.center =
        result.axisX * ((minimumX + maximumX) * 0.5)
        + result.axisY * ((minimumY + maximumY) * 0.5);
    result.valid =
        result.extentX > kGeometryEpsilon
        && result.extentY > kGeometryEpsilon;

    return result;
}

Affine orientedBoundsTransform(const OrientedBounds &source,
                               const OrientedBounds &target,
                               double axisXSign,
                               double axisYSign) {
    const QPointF targetAxisX = target.axisX * axisXSign;
    const QPointF targetAxisY = target.axisY * axisYSign;
    const double scaleX = target.extentX / source.extentX;
    const double scaleY = target.extentY / source.extentY;
    Affine result;
    result.a =
        targetAxisX.x() * scaleX * source.axisX.x()
        + targetAxisY.x() * scaleY * source.axisY.x();
    result.b =
        targetAxisX.y() * scaleX * source.axisX.x()
        + targetAxisY.y() * scaleY * source.axisY.x();
    result.c =
        targetAxisX.x() * scaleX * source.axisX.y()
        + targetAxisY.x() * scaleY * source.axisY.y();
    result.d =
        targetAxisX.y() * scaleX * source.axisX.y()
        + targetAxisY.y() * scaleY * source.axisY.y();
    result.e =
        target.center.x()
        - result.a * source.center.x()
        - result.c * source.center.y();
    result.f =
        target.center.y()
        - result.b * source.center.x()
        - result.d * source.center.y();

    return result;
}

QVector<CandidateJob> wholeComponentJobs(
    const Polygons &residual,
    const QVector<ShapeMesh> &catalog) {
    QVector<QPolygonF> outerPolygons;
    for (const QPolygonF &polygon : residual) {
        if (signedArea(polygon) > kGeometryEpsilon) {
            outerPolygons.push_back(polygon);
        }
    }
    if (outerPolygons.isEmpty() && !residual.isEmpty()) {
        outerPolygons.push_back(residual.front());
    }

    QVector<CandidateJob> result;
    for (const QPolygonF &polygon : outerPolygons) {
        const OrientedBounds target =
            orientedBounds(QVector<QPointF>(polygon.cbegin(), polygon.cend()));
        if (!target.valid) {
            continue;
        }
        for (const ShapeMesh &shape : catalog) {
            QVector<QPointF> shapePoints;
            shapePoints.reserve(shape.boundary.size());
            for (const Vec2 &point : shape.boundary) {
                shapePoints.push_back(QPointF(point.x, point.y));
            }
            const OrientedBounds source = orientedBounds(shapePoints);
            if (!source.valid) {
                continue;
            }
            CandidateJob job;
            job.shape = &shape;
            job.transform = orientedBoundsTransform(
                source, target, 1.0, 1.0);
            job.origin = CandidateOrigin::WholeComponent;
            job.hasTransform = true;
            result.push_back(job);
        }
    }

    return result;
}

const ShapeMesh *shapeById(const QVector<ShapeMesh> &catalog,
                           int shapeId) {
    const auto found = std::find_if(
        catalog.cbegin(), catalog.cend(),
        [shapeId](const ShapeMesh &shape) {
            return shape.id == shapeId;
        });

    return found == catalog.cend() ? nullptr : &*found;
}

QPolygonF shapePolygon(const ShapeMesh &shape) {
    QPolygonF result;
    result.reserve(shape.boundary.size());
    for (const Vec2 &point : shape.boundary) {
        result.push_back(QPointF(point.x, point.y));
    }

    return result;
}

Affine fromQTransform(const QTransform &transform) {
    return {
        transform.m11(),
        transform.m12(),
        transform.m21(),
        transform.m22(),
        transform.dx(),
        transform.dy(),
    };
}

QVector<FixedCandidate> hardEdgeCandidates(
    const QVector<ContourSpan> &boundarySpans,
    const QVector<ShapeMesh> &catalog,
    const std::function<bool()> &cancelled) {
    QVector<FixedCandidate> result;
    if (boundarySpans.size() < 3) {
        return result;
    }

    double straightLength = 0.0;
    double totalLength = 0.0;
    PolygonMeshRequest request;
    request.points.reserve(boundarySpans.size());
    for (const ContourSpan &span : boundarySpans) {
        request.points.push_back(span.start);
        const double chordLength =
            QLineF(span.start, span.end).length();
        const double spanLength = span.curved
            ? QLineF(span.start, span.control).length()
                + QLineF(span.control, span.end).length()
            : chordLength;
        totalLength += spanLength;
        if (!span.curved) {
            straightLength += chordLength;
        }
    }
    if (straightLength
        < totalLength * kHardBoundaryFraction) {
        return result;
    }

    const ShapeMesh *square = shapeById(catalog, 101);
    const ShapeMesh *triangle = shapeById(catalog, 103);
    if (square == nullptr || triangle == nullptr) {
        return result;
    }
    request.sources.square = shapePolygon(*square);
    request.sources.triangle = shapePolygon(*triangle);
    const PolygonMeshResult mesh = meshPolygon(request, cancelled);
    if (!mesh.error.isEmpty() || mesh.cancelled) {
        return result;
    }

    result.reserve(mesh.placements.size());
    for (const PolygonMeshPlacement &placement : mesh.placements) {
        const ShapeMesh *shape = shapeById(catalog, placement.shapeId);
        if (shape != nullptr) {
            result.push_back({
                shape,
                fromQTransform(placement.transform),
            });
        }
    }

    return result;
}

std::array<double, kGradientCount> affineValues(const Affine &transform) {
    return {
        transform.a, transform.b, transform.c,
        transform.d, transform.e, transform.f,
    };
}

Affine affineFromValues(const std::array<double, kGradientCount> &values) {
    return {
        values[0], values[1], values[2],
        values[3], values[4], values[5],
    };
}

bool lexicographicTransformLess(const Affine &left, const Affine &right) {
    return affineValues(left) < affineValues(right);
}

bool betterCandidate(const Candidate &left, const Candidate &right) {
    if (!left.valid) {
        return false;
    }
    if (!right.valid) {
        return true;
    }
    if (std::abs(left.covered - right.covered) > kGeometryEpsilon) {
        return left.covered > right.covered;
    }
    if (left.shapeId != right.shapeId) {
        return left.shapeId < right.shapeId;
    }

    return lexicographicTransformLess(left.transform, right.transform);
}

Candidate legalCandidate(const ShapeMesh &shape,
                         Affine transform,
                         const Polygons &residual,
                         const Polygons &mayCover,
                         const EvaluationBounds &subjectBounds,
                         const FillOptions &options,
                         CandidateProfile *profile) {
    QElapsedTimer timer;
    timer.start();
    Candidate result;
    result.shapeId = shape.id;
    for (int step = 0; step <= kLegalShrinkSteps; ++step) {
        ++profile->legalizationEvaluations;
        const AreaGradient evaluation =
            areaGradient(shape, transform, residual, mayCover, subjectBounds);
        if (finiteGradient(evaluation)
            && evaluation.spill <= options.epsSpill
            && evaluation.covered >= options.epsGain) {
            result.transform = transform;
            result.covered = evaluation.covered;
            result.spill = std::max(0.0, evaluation.spill);
            result.valid = true;
            profile->legalizationNanoseconds += timer.nsecsElapsed();

            return result;
        }
        transform.a *= kLegalShrinkFactor;
        transform.b *= kLegalShrinkFactor;
        transform.c *= kLegalShrinkFactor;
        transform.d *= kLegalShrinkFactor;
    }
    profile->legalizationNanoseconds += timer.nsecsElapsed();

    return result;
}

CandidateJobResult optimizeCandidate(
    const CandidateJob &job,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    const DistanceSeed &seed,
    const std::function<bool()> &cancelled) {
    QElapsedTimer jobTimer;
    jobTimer.start();
    CandidateJobResult result;
    const ShapeMesh &shape = *job.shape;
    std::array<double, kGradientCount> values =
        affineValues(initialTransform(job, seed));
    std::array<double, kGradientCount> firstMoment{};
    std::array<double, kGradientCount> secondMoment{};
    result.candidate = legalCandidate(
        shape, affineFromValues(values), residual, mayCover,
        subjectBounds, options, &result.profile);
    double beta1Power = 1.0;
    double beta2Power = 1.0;
    for (int iteration = 1; iteration <= options.adamIterations; ++iteration) {
        if (cancelled && cancelled()) {
            result.candidate = {};
            result.profile.totalNanoseconds = jobTimer.nsecsElapsed();
            return result;
        }
        const Affine transform = affineFromValues(values);
        QElapsedTimer evaluationTimer;
        evaluationTimer.start();
        const AreaGradient evaluation =
            areaGradient(shape, transform, residual, mayCover, subjectBounds);
        result.profile.adamEvaluationNanoseconds +=
            evaluationTimer.nsecsElapsed();
        ++result.profile.adamEvaluations;
        if (!finiteGradient(evaluation)) {
            break;
        }

        std::array<double, kGradientCount> scoreGradient{};
        double gradientNormSquared = 0.0;
        for (int parameter = 0; parameter < kGradientCount; ++parameter) {
            scoreGradient[parameter] = evaluation.coveredGradient[parameter]
                - options.spillWeight * evaluation.spillGradient[parameter];
            gradientNormSquared += scoreGradient[parameter]
                * scoreGradient[parameter];
        }
        const double gradientNorm = std::sqrt(gradientNormSquared);
        if (gradientNorm <= kGradientStopNorm) {
            break;
        }
        if (gradientNorm > kGradientNormLimit) {
            const double factor = kGradientNormLimit / gradientNorm;
            for (double &gradient : scoreGradient) {
                gradient *= factor;
            }
        }

        beta1Power *= kAdamBeta1;
        beta2Power *= kAdamBeta2;
        for (int parameter = 0; parameter < kGradientCount; ++parameter) {
            firstMoment[parameter] = kAdamBeta1 * firstMoment[parameter]
                + (1.0 - kAdamBeta1) * scoreGradient[parameter];
            secondMoment[parameter] = kAdamBeta2 * secondMoment[parameter]
                + (1.0 - kAdamBeta2) * scoreGradient[parameter]
                    * scoreGradient[parameter];
            const double correctedFirst = firstMoment[parameter]
                / (1.0 - beta1Power);
            const double correctedSecond = secondMoment[parameter]
                / (1.0 - beta2Power);
            values[parameter] += options.adamLearningRate * correctedFirst
                / (std::sqrt(correctedSecond) + kAdamEpsilon);
        }
        const Candidate candidate = legalCandidate(
            shape, affineFromValues(values), residual, mayCover,
            subjectBounds, options, &result.profile);
        if (betterCandidate(candidate, result.candidate)) {
            result.candidate = candidate;
        }
    }
    result.profile.totalNanoseconds = jobTimer.nsecsElapsed();

    return result;
}

bool evaluateGpuBatch(
    GpuAreaEvaluator *evaluator,
    const QVector<GpuEvaluationRequest> &requests,
    QVector<AreaGradient> *evaluations,
    FillProfile *profile,
    bool legalization) {
    const bool optimizerBackend =
        evaluator->supportsOptimizerEvaluation();
    if (!evaluator->evaluate(requests, evaluations)
        || (!legalization && optimizerBackend
            && !evaluator->supportsOptimizerEvaluation())) {
        return false;
    }
    if (legalization) {
        profile->legalizationEvaluations += requests.size();
    } else {
        profile->adamEvaluations += requests.size();
    }

    return true;
}

void evaluateCpuBatch(
    const QVector<GpuEvaluationRequest> &requests,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    QThreadPool *candidatePool,
    QVector<AreaGradient> *evaluations,
    FillProfile *profile) {
    evaluations->fill({}, requests.size());
    std::vector<qint64> durations(
        static_cast<size_t>(requests.size()));
    for (int requestIndex = 0; requestIndex < requests.size();
         ++requestIndex) {
        candidatePool->start([&, requestIndex]() {
            QElapsedTimer timer;
            timer.start();
            const GpuEvaluationRequest &request = requests[requestIndex];
            (*evaluations)[requestIndex] = areaGradient(
                *request.shape, request.transform,
                residual, mayCover, subjectBounds);
            durations[static_cast<size_t>(requestIndex)] =
                timer.nsecsElapsed();
        });
    }
    candidatePool->waitForDone();
    for (const qint64 duration : durations) {
        const double seconds = static_cast<double>(duration) * 1e-9;
        profile->candidateWorkerSeconds += seconds;
        profile->adamEvaluationWorkerSeconds += seconds;
    }
    profile->adamEvaluations += requests.size();
}

bool legalCandidatesGpu(
    const QVector<const ShapeMesh *> &shapes,
    const QVector<Affine> &initialTransforms,
    const FillOptions &options,
    GpuAreaEvaluator *evaluator,
    FillProfile *profile,
    QVector<Candidate> *candidates,
    const std::function<bool()> &cancelled,
    bool *wasCancelled) {
    QVector<int> pending;
    pending.reserve(shapes.size());
    for (int index = 0; index < shapes.size(); ++index) {
        pending.push_back(index);
    }
    QVector<Affine> transforms = initialTransforms;
    candidates->fill({}, shapes.size());
    for (int step = 0;
         step <= kLegalShrinkSteps && !pending.isEmpty();
         step += kGpuLegalizationBatchSteps) {
        if (cancelled && cancelled()) {
            *wasCancelled = true;
            return true;
        }
        const int batchSteps = std::min(
            kGpuLegalizationBatchSteps,
            kLegalShrinkSteps - step + 1);
        QVector<GpuEvaluationRequest> requests;
        requests.reserve(pending.size() * batchSteps);
        for (const int pendingIndex : pending) {
            Affine transform = transforms[pendingIndex];
            for (int batchStep = 0; batchStep < batchSteps; ++batchStep) {
                requests.push_back({
                    shapes[pendingIndex],
                    transform,
                });
                transform.a *= kLegalShrinkFactor;
                transform.b *= kLegalShrinkFactor;
                transform.c *= kLegalShrinkFactor;
                transform.d *= kLegalShrinkFactor;
            }
        }
        QVector<AreaGradient> evaluations;
        if (!evaluateGpuBatch(
                evaluator, requests, &evaluations, profile, true)) {
            return false;
        }

        QVector<int> nextPending;
        nextPending.reserve(pending.size());
        for (int pendingOffset = 0; pendingOffset < pending.size();
             ++pendingOffset) {
            const int pendingIndex = pending[pendingOffset];
            bool accepted = false;
            for (int batchStep = 0; batchStep < batchSteps; ++batchStep) {
                const int requestIndex =
                    pendingOffset * batchSteps + batchStep;
                const AreaGradient &evaluation = evaluations[requestIndex];
                const double spillSlack =
                    evaluator->usesDoublePrecision()
                    ? 0.0
                    : std::max(
                          0.05,
                          std::abs(evaluation.covered) * 1e-4);
                const double minimumCovered =
                    evaluator->usesDoublePrecision()
                    ? options.epsGain
                    : options.epsGain * 0.5;
                if (!finiteGradient(evaluation)
                    || evaluation.spill > options.epsSpill + spillSlack
                    || evaluation.covered < minimumCovered) {
                    continue;
                }
                Candidate candidate;
                candidate.transform = requests[requestIndex].transform;
                candidate.shapeId = shapes[pendingIndex]->id;
                candidate.covered = evaluation.covered;
                candidate.spill = std::max(0.0, evaluation.spill);
                candidate.valid = true;
                (*candidates)[pendingIndex] = candidate;
                accepted = true;
                break;
            }
            if (accepted) {
                continue;
            }
            for (int batchStep = 0; batchStep < batchSteps; ++batchStep) {
                transforms[pendingIndex].a *= kLegalShrinkFactor;
                transforms[pendingIndex].b *= kLegalShrinkFactor;
                transforms[pendingIndex].c *= kLegalShrinkFactor;
                transforms[pendingIndex].d *= kLegalShrinkFactor;
            }
            nextPending.push_back(pendingIndex);
        }
        pending = std::move(nextPending);
    }

    return true;
}

void exactCandidatesCpuBatch(
    const QVector<const ShapeMesh *> &shapes,
    const QVector<Candidate> &gpuCandidates,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    QThreadPool *candidatePool,
    FillProfile *profile,
    QVector<Candidate> *candidates) {
    candidates->fill({}, gpuCandidates.size());
    std::vector<CandidateProfile> profiles(
        static_cast<size_t>(gpuCandidates.size()));
    for (int index = 0; index < gpuCandidates.size(); ++index) {
        if (!gpuCandidates[index].valid) {
            continue;
        }
        candidatePool->start([&, index]() {
            (*candidates)[index] = legalCandidate(
                *shapes[index], gpuCandidates[index].transform,
                residual, mayCover, subjectBounds, options,
                &profiles[static_cast<size_t>(index)]);
        });
    }
    candidatePool->waitForDone();
    for (const CandidateProfile &candidateProfile : profiles) {
        const double seconds = static_cast<double>(
            candidateProfile.legalizationNanoseconds) * 1e-9;
        profile->candidateWorkerSeconds += seconds;
        profile->legalizationWorkerSeconds += seconds;
        profile->legalizationEvaluations +=
            candidateProfile.legalizationEvaluations;
    }
}

bool optimizeCandidatesGpu(
    const std::vector<CandidateJob> &jobs,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    const DistanceSeed &seed,
    GpuAreaEvaluator *evaluator,
    QThreadPool *candidatePool,
    FillProfile *profile,
    const std::function<bool()> &cancelled,
    std::vector<Candidate> *results,
    bool *wasCancelled) {
    QVector<GpuCandidateState> states;
    states.reserve(static_cast<qsizetype>(jobs.size()));
    for (const CandidateJob &job : jobs) {
        GpuCandidateState state;
        state.job = &job;
        state.values = affineValues(initialTransform(job, seed));
        states.push_back(state);
    }

    QVector<const ShapeMesh *> initialShapes;
    QVector<Affine> initialTransforms;
    initialShapes.reserve(states.size());
    initialTransforms.reserve(states.size());
    for (int index = 0; index < states.size(); ++index) {
        initialShapes.push_back(states[index].job->shape);
        initialTransforms.push_back(affineFromValues(states[index].values));
    }
    QVector<Candidate> initialGpuCandidates;
    if (!legalCandidatesGpu(
            initialShapes, initialTransforms, options, evaluator, profile,
            &initialGpuCandidates, cancelled, wasCancelled)) {
        return false;
    }
    QVector<Candidate> initialCandidates = initialGpuCandidates;
    if (!evaluator->usesDoublePrecision()) {
        exactCandidatesCpuBatch(
            initialShapes, initialGpuCandidates, residual, mayCover,
            subjectBounds, options, candidatePool, profile,
            &initialCandidates);
    }
    for (int index = 0; index < states.size(); ++index) {
        states[index].best = initialCandidates[index];
        states[index].bestGpu = initialGpuCandidates[index];
    }

    for (int iteration = 1; iteration <= options.adamIterations;
         ++iteration) {
        if (*wasCancelled || (cancelled && cancelled())) {
            *wasCancelled = true;
            break;
        }

        QVector<int> activeIndices;
        QVector<GpuEvaluationRequest> requests;
        for (int index = 0; index < states.size(); ++index) {
            if (!states[index].active) {
                continue;
            }
            activeIndices.push_back(index);
            requests.push_back({
                states[index].job->shape,
                affineFromValues(states[index].values),
            });
        }
        if (requests.isEmpty()) {
            break;
        }

        QVector<AreaGradient> evaluations;
        if (evaluator->supportsOptimizerEvaluation()) {
            if (!evaluateGpuBatch(
                    evaluator, requests, &evaluations,
                    profile, false)) {
                return false;
            }
        } else {
            evaluateCpuBatch(
                requests, residual, mayCover, subjectBounds,
                candidatePool, &evaluations, profile);
        }
        QVector<int> legalIndices;
        QVector<const ShapeMesh *> legalShapes;
        QVector<Affine> legalTransforms;
        for (int requestIndex = 0; requestIndex < activeIndices.size();
             ++requestIndex) {
            GpuCandidateState &state =
                states[activeIndices[requestIndex]];
            const AreaGradient &evaluation = evaluations[requestIndex];
            if (!finiteGradient(evaluation)) {
                state.active = false;
                continue;
            }

            std::array<double, kGradientCount> scoreGradient{};
            double gradientNormSquared = 0.0;
            for (int parameter = 0; parameter < kGradientCount;
                 ++parameter) {
                scoreGradient[parameter] =
                    evaluation.coveredGradient[parameter]
                    - options.spillWeight
                        * evaluation.spillGradient[parameter];
                gradientNormSquared += scoreGradient[parameter]
                    * scoreGradient[parameter];
            }
            const double gradientNorm = std::sqrt(gradientNormSquared);
            if (gradientNorm <= kGradientStopNorm) {
                state.active = false;
                continue;
            }
            if (gradientNorm > kGradientNormLimit) {
                const double factor = kGradientNormLimit / gradientNorm;
                for (double &gradient : scoreGradient) {
                    gradient *= factor;
                }
            }

            state.beta1Power *= kAdamBeta1;
            state.beta2Power *= kAdamBeta2;
            for (int parameter = 0; parameter < kGradientCount;
                 ++parameter) {
                state.firstMoment[parameter] =
                    kAdamBeta1 * state.firstMoment[parameter]
                    + (1.0 - kAdamBeta1) * scoreGradient[parameter];
                state.secondMoment[parameter] =
                    kAdamBeta2 * state.secondMoment[parameter]
                    + (1.0 - kAdamBeta2) * scoreGradient[parameter]
                        * scoreGradient[parameter];
                const double correctedFirst =
                    state.firstMoment[parameter]
                    / (1.0 - state.beta1Power);
                const double correctedSecond =
                    state.secondMoment[parameter]
                    / (1.0 - state.beta2Power);
                state.values[parameter] +=
                    options.adamLearningRate * correctedFirst
                    / (std::sqrt(correctedSecond) + kAdamEpsilon);
            }
            legalIndices.push_back(activeIndices[requestIndex]);
            legalShapes.push_back(state.job->shape);
            legalTransforms.push_back(affineFromValues(state.values));
        }
        if (legalIndices.isEmpty()) {
            continue;
        }

        QVector<Candidate> gpuLegalCandidates;
        if (!legalCandidatesGpu(
                legalShapes, legalTransforms, options, evaluator,
                profile, &gpuLegalCandidates, cancelled, wasCancelled)) {
            return false;
        }
        QVector<int> competitiveIndices;
        QVector<const ShapeMesh *> competitiveShapes;
        QVector<Candidate> competitiveGpuCandidates;
        for (int index = 0; index < legalIndices.size(); ++index) {
            GpuCandidateState &state = states[legalIndices[index]];
            if (betterCandidate(
                    gpuLegalCandidates[index], state.bestGpu)) {
                state.bestGpu = gpuLegalCandidates[index];
                if (evaluator->usesDoublePrecision()) {
                    if (betterCandidate(
                            gpuLegalCandidates[index], state.best)) {
                        state.best = gpuLegalCandidates[index];
                    }
                    continue;
                }
                competitiveIndices.push_back(legalIndices[index]);
                competitiveShapes.push_back(state.job->shape);
                competitiveGpuCandidates.push_back(
                    gpuLegalCandidates[index]);
            }
        }
        QVector<Candidate> competitiveCandidates;
        exactCandidatesCpuBatch(
            competitiveShapes, competitiveGpuCandidates,
            residual, mayCover, subjectBounds, options,
            candidatePool, profile, &competitiveCandidates);
        for (int index = 0; index < competitiveIndices.size(); ++index) {
            GpuCandidateState &state = states[competitiveIndices[index]];
            if (betterCandidate(competitiveCandidates[index], state.best)) {
                state.best = competitiveCandidates[index];
            }
        }
    }

    results->clear();
    results->reserve(jobs.size());
    for (const GpuCandidateState &state : states) {
        results->push_back(*wasCancelled ? Candidate{} : state.best);
    }

    return true;
}

CandidateInitialization candidateInitialization(
    const DistanceSeed &seed,
    int restart,
    std::mt19937_64 *random) {
    CandidateInitialization result;
    if (restart == 0) {
        return result;
    }

    std::uniform_real_distribution<double> unitDistribution(-1.0, 1.0);
    result.angleOffset = unitDistribution(*random) * kRestartAngleRange;
    result.scaleFactor =
        1.0 + unitDistribution(*random) * kRestartScaleRange;
    result.translationOffset =
        QPointF(unitDistribution(*random), unitDistribution(*random))
        * (seed.radius * kRestartTranslationFraction);

    return result;
}

double polygonPerimeter(const Polygons &polygons) {
    double result = 0.0;
    for (const QPolygonF &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            result += QLineF(
                polygon[index],
                polygon[(index + 1) % polygon.size()]).length();
        }
    }

    return result;
}

ResidualComplexity residualComplexity(const Polygons &polygons) {
    ResidualComplexity result;
    QVector<QPointF> points;
    for (const QPolygonF &polygon : polygons) {
        points += polygon;
        if (signedArea(polygon) >= 0.0) {
            ++result.components;
        } else {
            ++result.holes;
        }
    }
    if (!polygons.isEmpty() && result.components == 0) {
        result.components = polygons.size();
        result.holes = 0;
    }
    const double area = std::max(
        kGeometryEpsilon, polygonSetArea(polygons));
    const double perimeter = polygonPerimeter(polygons);
    result.score =
        perimeter * perimeter / (4.0 * kPi * area)
        + kComponentComplexityWeight
            * std::max(0, result.components - 1)
        + kHoleComplexityWeight * result.holes;

    return result;
}

Polygons componentAtPoint(const Polygons &polygons,
                          const QPointF &point) {
    int outerIndex = -1;
    double outerArea = std::numeric_limits<double>::max();
    for (int index = 0; index < polygons.size(); ++index) {
        const double area = signedArea(polygons[index]);
        if (area > kGeometryEpsilon
            && std::abs(area) < outerArea
            && polygons[index].containsPoint(point, Qt::OddEvenFill)) {
            outerIndex = index;
            outerArea = std::abs(area);
        }
    }
    if (outerIndex < 0) {
        for (int index = 0; index < polygons.size(); ++index) {
            const double area = std::abs(signedArea(polygons[index]));
            if (area > kGeometryEpsilon
                && (outerIndex < 0 || area > outerArea)) {
                outerIndex = index;
                outerArea = area;
            }
        }
    }
    if (outerIndex < 0) {
        return polygons;
    }

    Polygons result{polygons[outerIndex]};
    const QPolygonF &outer = polygons[outerIndex];
    for (int index = 0; index < polygons.size(); ++index) {
        if (index == outerIndex || polygons[index].isEmpty()
            || signedArea(polygons[index]) >= 0.0) {
            continue;
        }
        if (outer.containsPoint(
                polygons[index].front(), Qt::OddEvenFill)) {
            result.push_back(polygons[index]);
        }
    }

    return result;
}

double descriptorAspect(const QRectF &bounds) {
    const double minimum = std::max(
        kGeometryEpsilon,
        std::min(bounds.width(), bounds.height()));
    const double maximum =
        std::max(bounds.width(), bounds.height());

    return maximum / minimum;
}

double descriptorDistance(const ShapeMesh &shape,
                          const Polygons &residual) {
    const QRectF residualBounds = polygonBounds(residual);
    const QRectF shapeBoundsValue = shapeBounds(shape);
    const double residualArea = std::max(
        kGeometryEpsilon,
        residualBounds.width() * residualBounds.height());
    const double shapeArea = std::max(
        kGeometryEpsilon,
        shapeBoundsValue.width() * shapeBoundsValue.height());
    const double residualFill =
        polygonSetArea(residual) / residualArea;
    const double shapeFill = shape.area / shapeArea;

    return std::abs(
               std::log(descriptorAspect(shapeBoundsValue))
               - std::log(descriptorAspect(residualBounds)))
        + 2.0 * std::abs(shapeFill - residualFill);
}

QVector<const ShapeMesh *> routedShapes(const Polygons &residual,
                                        const QVector<ShapeMesh> &catalog,
                                        bool useRouter) {
    QVector<const ShapeMesh *> result;
    for (const ShapeMesh &shape : catalog) {
        if (shape.valid()) {
            result.push_back(&shape);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const ShapeMesh *left, const ShapeMesh *right) {
                  return left->id < right->id;
              });
    if (!useRouter || result.size() <= kRouterCandidateCount) {
        return result;
    }

    std::stable_sort(result.begin(), result.end(),
                     [&residual](
                         const ShapeMesh *left, const ShapeMesh *right) {
                         const double leftDistance =
                             descriptorDistance(*left, residual);
                         const double rightDistance =
                             descriptorDistance(*right, residual);
                         if (std::abs(leftDistance - rightDistance)
                             > kGeometryEpsilon) {
                             return leftDistance < rightDistance;
                         }
                         return left->id < right->id;
                     });
    result.resize(kRouterCandidateCount);

    return result;
}

Candidate fixedCandidate(const FixedCandidate &fixed,
                         const Polygons &residual,
                         const Polygons &mayCover,
                         const EvaluationBounds &subjectBounds,
                         const FillOptions &options,
                         CandidateProfile *profile) {
    QElapsedTimer timer;
    timer.start();
    const AreaGradient evaluation = areaGradient(
        *fixed.shape, fixed.transform,
        residual, mayCover, subjectBounds);
    ++profile->legalizationEvaluations;
    profile->legalizationNanoseconds += timer.nsecsElapsed();
    Candidate result;
    if (!finiteGradient(evaluation)
        || evaluation.spill > options.epsSpill
        || evaluation.covered < options.epsGain) {
        return result;
    }
    result.transform = fixed.transform;
    result.shapeId = fixed.shape->id;
    result.covered = evaluation.covered;
    result.spill = std::max(0.0, evaluation.spill);
    result.origin = CandidateOrigin::HardEdge;
    result.valid = true;

    return result;
}

CandidateSelection selectCandidate(
    const QVector<Candidate> &candidates,
    const QVector<ShapeMesh> &catalog,
    const Polygons &residual,
    double epsGain) {
    CandidateSelection result;
    const bool havePrimary = std::any_of(
        candidates.cbegin(), candidates.cend(),
        [](const Candidate &candidate) {
            return candidate.valid
                && candidate.origin
                    != CandidateOrigin::LocalComponent;
        });
    if (!havePrimary) {
        return result;
    }
    double maximumCovered = 0.0;
    for (const Candidate &candidate : candidates) {
        if (candidate.valid
            && candidate.origin
                != CandidateOrigin::LocalComponent) {
            maximumCovered =
                std::max(maximumCovered, candidate.covered);
        }
    }
    if (maximumCovered < epsGain) {
        return result;
    }

    struct ScoredCandidate {
        Candidate candidate;
        Polygons residual;
        double exactGain = 0.0;
        double complexity = 0.0;
    };
    QVector<ScoredCandidate> scored;
    const double previousArea = polygonSetArea(residual);
    for (const Candidate &candidate : candidates) {
        if (!candidate.valid) {
            continue;
        }
        const bool local =
            candidate.origin == CandidateOrigin::LocalComponent;
        if ((local
             && candidate.covered
                 < maximumCovered
                     * kLocalSelectionGainAdvantage)
            || (!local
                && candidate.covered
                    < maximumCovered
                        * kComplexityGainWindow)) {
            continue;
        }
        const ShapeMesh *shape = shapeById(catalog, candidate.shapeId);
        if (shape == nullptr) {
            continue;
        }
        const Polygons footprint{
            transformedBoundary(*shape, candidate.transform),
        };
        Polygons nextResidual =
            differencePolygons(residual, footprint);
        const double exactGain =
            previousArea - polygonSetArea(nextResidual);
        if (!std::isfinite(exactGain) || exactGain < epsGain) {
            continue;
        }
        scored.push_back({
            candidate,
            std::move(nextResidual),
            exactGain,
            0.0,
        });
    }
    if (scored.isEmpty()) {
        return result;
    }

    double primaryComplexity =
        std::numeric_limits<double>::max();
    for (ScoredCandidate &candidate : scored) {
        candidate.complexity =
            residualComplexity(candidate.residual).score;
        if (candidate.candidate.origin
            != CandidateOrigin::LocalComponent) {
            primaryComplexity = std::min(
                primaryComplexity, candidate.complexity);
        }
    }
    int areaWinner = -1;
    for (int index = 0; index < scored.size(); ++index) {
        if (scored[index].candidate.origin
                == CandidateOrigin::LocalComponent
            && scored[index].complexity
                > primaryComplexity
                    * kLocalSelectionComplexityRatio) {
            continue;
        }
        if (areaWinner < 0
            || betterCandidate(
                scored[index].candidate,
                scored[areaWinner].candidate)) {
            areaWinner = index;
        }
    }
    if (areaWinner < 0) {
        return result;
    }
    int winner = -1;
    for (int index = 0; index < scored.size(); ++index) {
        if (std::abs(
                scored[index].candidate.covered
                - scored[areaWinner].candidate.covered)
            > kGeometryEpsilon) {
            continue;
        }
        if (scored[index].candidate.origin
                == CandidateOrigin::LocalComponent
            && scored[index].complexity
                > primaryComplexity
                    * kLocalSelectionComplexityRatio) {
            continue;
        }
        if (winner < 0
            || scored[index].complexity
                < scored[winner].complexity - kGeometryEpsilon
            || (std::abs(
                    scored[index].complexity
                    - scored[winner].complexity)
                    <= kGeometryEpsilon
                && betterCandidate(
                    scored[index].candidate,
                    scored[winner].candidate))) {
            winner = index;
        }
    }
    result.candidate = scored[winner].candidate;
    result.candidate.covered = scored[winner].exactGain;
    result.residual = std::move(scored[winner].residual);
    result.exactGain = scored[winner].exactGain;
    result.complexityPreferred = winner != areaWinner;
    result.valid = true;

    return result;
}

} // namespace gui::cover
