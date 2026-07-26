#include "differentiable_cover.h"
#include "differentiable_cover_gpu.h"
#include "pen_fill.h"

#include <QtCore>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

using namespace gui;

bool check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
    }

    return condition;
}

const cover::ShapeMesh *shapeById(const QVector<cover::ShapeMesh> &catalog,
                                  int shapeId) {
    const auto found = std::find_if(
        catalog.cbegin(), catalog.cend(),
        [shapeId](const cover::ShapeMesh &shape) {
            return shape.id == shapeId;
        });

    return found == catalog.cend() ? nullptr : &*found;
}

cover::Affine variedTransform(const cover::Affine &source,
                              int parameter,
                              double delta) {
    cover::Affine result = source;
    double *values[] = {
        &result.a, &result.b, &result.c,
        &result.d, &result.e, &result.f,
    };
    *values[parameter] += delta;

    return result;
}

bool testCatalogAndGradient(const QVector<cover::ShapeMesh> &catalog) {
    if (!check(catalog.size() == 9, "analytic catalog did not load nine shapes")) {
        return false;
    }
    const cover::ShapeMesh *shape = shapeById(catalog, 101);
    if (!check(shape != nullptr && shape->valid(),
               "square analytic geometry is unavailable")) {
        return false;
    }

    QPolygonF subject{
        QPointF(-15.0, -18.0),
        QPointF(22.0, -18.0),
        QPointF(22.0, 19.0),
        QPointF(-15.0, 19.0),
    };
    QPolygonF legalSubject{
        QPointF(-10.0, -12.0),
        QPointF(14.0, -12.0),
        QPointF(14.0, 13.0),
        QPointF(-10.0, 13.0),
    };
    cover::Affine transform;
    transform.a = 0.35;
    transform.b = 0.07;
    transform.c = -0.05;
    transform.d = 0.28;
    transform.e = 3.0;
    transform.f = -2.0;
    const cover::Polygons subjects{subject};
    const cover::Polygons legalSubjects{legalSubject};
    const cover::AreaGradient analytic =
        cover::evaluateAreaGradient(*shape, transform, subjects, legalSubjects);
    constexpr double kDifferenceStep = 1e-5;
    constexpr double kGradientTolerance = 2e-3;
    for (int parameter = 0; parameter < 6; ++parameter) {
        const cover::AreaGradient plus = cover::evaluateAreaGradient(
            *shape, variedTransform(transform, parameter, kDifferenceStep),
            subjects, legalSubjects);
        const cover::AreaGradient minus = cover::evaluateAreaGradient(
            *shape, variedTransform(transform, parameter, -kDifferenceStep),
            subjects, legalSubjects);
        const double coveredDifference =
            (plus.covered - minus.covered) / (2.0 * kDifferenceStep);
        const double spillDifference =
            (plus.spill - minus.spill) / (2.0 * kDifferenceStep);
        const double coveredScale = std::max(1.0, std::abs(coveredDifference));
        const double spillScale = std::max(1.0, std::abs(spillDifference));
        if (!check(std::abs(analytic.coveredGradient[parameter] - coveredDifference)
                       <= kGradientTolerance * coveredScale,
                   "analytic covered-area gradient differs from finite difference")) {
            return false;
        }
        if (!check(std::abs(analytic.spillGradient[parameter] - spillDifference)
                       <= kGradientTolerance * spillScale,
                   "analytic spill-area gradient differs from finite difference")) {
            return false;
        }
    }

    std::unique_ptr<cover::GpuAreaEvaluator> gpu =
        cover::createGpuAreaEvaluator(catalog);
    if (gpu->available()) {
#ifdef FH6_HAS_CUDA
        if (!check(
                gpu->stats().backend.contains(QStringLiteral("CUDA")),
                "CUDA was not selected as the first GPU backend")) {
            return false;
        }
#endif
        QVector<cover::GpuEvaluationRequest> requests;
        for (const cover::ShapeMesh &catalogShape : catalog) {
            requests.push_back({&catalogShape, transform});
            requests.push_back({
                &catalogShape,
                variedTransform(transform, 4, 0.25),
            });
        }
        QVector<cover::AreaGradient> gpuEvaluations;
        if (!check(gpu->setSubjects(subjects, legalSubjects)
                       && gpu->evaluate(requests, &gpuEvaluations)
                       && gpuEvaluations.size() == requests.size(),
                   "GPU area-gradient evaluation failed")) {
            return false;
        }
        for (int evaluationIndex = 0;
             evaluationIndex < gpuEvaluations.size(); ++evaluationIndex) {
            const cover::AreaGradient expected =
                cover::evaluateAreaGradient(
                    *requests[evaluationIndex].shape,
                    requests[evaluationIndex].transform,
                    subjects, legalSubjects);
            const cover::AreaGradient &actual =
                gpuEvaluations[evaluationIndex];
            const double areaTolerance =
                gpu->usesDoublePrecision() ? 1e-9 : 1e-4;
            const double gradientTolerance =
                gpu->usesDoublePrecision() ? 1e-8 : 2e-3;
            const double coveredScale =
                std::max(1.0, std::abs(expected.covered));
            const double spillScale =
                std::max(1.0, std::abs(expected.spill));
            if (!check(
                    std::abs(actual.covered - expected.covered)
                            <= areaTolerance * coveredScale
                        && std::abs(actual.spill - expected.spill)
                            <= areaTolerance * spillScale,
                    "GPU area values differ from the CPU evaluator")) {
                return false;
            }
            for (int parameter = 0; parameter < 6; ++parameter) {
                const double coveredGradientScale = std::max(
                    1.0, std::abs(expected.coveredGradient[parameter]));
                const double spillGradientScale = std::max(
                    1.0, std::abs(expected.spillGradient[parameter]));
                if (!check(
                        std::abs(
                                actual.coveredGradient[parameter]
                                - expected.coveredGradient[parameter])
                                <= gradientTolerance
                                    * coveredGradientScale
                            && std::abs(
                                actual.spillGradient[parameter]
                                - expected.spillGradient[parameter])
                                <= gradientTolerance
                                    * spillGradientScale,
                        "GPU area gradients differ from the CPU evaluator")) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool loadLoggedContour(QVector<PenPoint> *points, bool *available) {
    const QString path = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("pen_fill.log"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *available = false;
        std::cout << "pen_fill.log is unavailable; logged-contour test skipped\n";
        return true;
    }
    *available = true;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!check(parseError.error == QJsonParseError::NoError
                   && document.isObject(),
               "pen_fill.log is invalid")) {
        return false;
    }
    const QJsonArray sourcePoints =
        document.object().value(QStringLiteral("request")).toObject()
            .value(QStringLiteral("points")).toArray();
    for (const QJsonValue &value : sourcePoints) {
        const QJsonObject object = value.toObject();
        const QJsonArray position =
            object.value(QStringLiteral("position")).toArray();
        if (position.size() != 2) {
            continue;
        }
        PenPoint point;
        point.position = QPointF(position[0].toDouble(), position[1].toDouble());
        point.kind = object.value(QStringLiteral("kind")).toString()
                == QStringLiteral("soft")
            ? PenPointKind::Soft
            : PenPointKind::Hard;
        points->push_back(point);
    }

    return check(points->size() >= 3, "pen_fill.log contains no usable contour");
}

bool repeatablePlacement(const cover::Placement &left,
                         const cover::Placement &right) {
    constexpr double kTransformTolerance = 1e-10;
    return left.shapeId == right.shapeId
        && std::abs(left.transform.a - right.transform.a) <= kTransformTolerance
        && std::abs(left.transform.b - right.transform.b) <= kTransformTolerance
        && std::abs(left.transform.c - right.transform.c) <= kTransformTolerance
        && std::abs(left.transform.d - right.transform.d) <= kTransformTolerance
        && std::abs(left.transform.e - right.transform.e) <= kTransformTolerance
        && std::abs(left.transform.f - right.transform.f) <= kTransformTolerance;
}

QPainterPath loggedContourPath(const QVector<PenPoint> &points) {
    const auto firstHard = std::find_if(
        points.cbegin(), points.cend(),
        [](const PenPoint &point) {
            return point.kind == PenPointKind::Hard;
        });
    if (firstHard == points.cend()) {
        return {};
    }
    QVector<PenPoint> ordered;
    ordered.reserve(points.size());
    const int firstIndex = static_cast<int>(
        std::distance(points.cbegin(), firstHard));
    for (int i = 0; i < points.size(); ++i) {
        ordered.push_back(points[(firstIndex + i) % points.size()]);
    }

    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    result.moveTo(ordered.front().position);
    int index = 1;
    while (index <= ordered.size()) {
        const PenPoint &next = ordered[index % ordered.size()];
        if (next.kind == PenPointKind::Hard) {
            result.lineTo(next.position);
            ++index;
            continue;
        }
        const PenPoint &after = ordered[(index + 1) % ordered.size()];
        const QPointF end = after.kind == PenPointKind::Hard
            ? after.position
            : (next.position + after.position) * 0.5;
        result.quadTo(next.position, end);
        index += after.kind == PenPointKind::Hard ? 2 : 1;
    }
    result.closeSubpath();

    return result;
}

bool testLoggedContour(const QVector<cover::ShapeMesh> &catalog) {
    QVector<PenPoint> points;
    bool available = false;
    if (!loadLoggedContour(&points, &available)) {
        return false;
    }
    if (!available) {
        return true;
    }
    const QPainterPath contour = loggedContourPath(points);
    if (!check(!contour.isEmpty(), "logged Pen contour is invalid")) {
        return false;
    }
    const cover::Polygons polygons =
        cover::polygonsFromPainterPath(contour);
    if (!check(!polygons.isEmpty(), "logged Pen contour did not flatten")) {
        return false;
    }

    cover::FillInput input;
    input.mustCover = polygons;
    input.mayCover = polygons;
    cover::FillOptions options;
    bool budgetOk = false;
    const int requestedBudget =
        qEnvironmentVariableIntValue("FH6_DIFFERENTIABLE_TEST_BUDGET", &budgetOk);
    options.budget = budgetOk && requestedBudget > 0 ? requestedBudget : 2;
    bool iterationsOk = false;
    const int requestedIterations =
        qEnvironmentVariableIntValue("FH6_DIFFERENTIABLE_TEST_ITERATIONS",
                                     &iterationsOk);
    options.adamIterations =
        iterationsOk && requestedIterations > 0 ? requestedIterations : 12;
    bool restartsOk = false;
    const int requestedRestarts =
        qEnvironmentVariableIntValue("FH6_DIFFERENTIABLE_TEST_RESTARTS",
                                     &restartsOk);
    options.restarts =
        restartsOk && requestedRestarts >= 0 ? requestedRestarts : 0;
    options.seed = 0x5a17;
    QVector<cover::FillProgress> progress;
    const cover::FillResult first =
        cover::analyticCoverFill(
            input, catalog, options, {},
            [&progress](const cover::FillProgress &update) {
                progress.push_back(update);
            });
    const cover::FillResult second =
        cover::analyticCoverFill(input, catalog, options);
    if (!check(first.error.isEmpty() && second.error.isEmpty(),
               "logged contour solver returned an error")
        || !check(!first.placements.isEmpty(),
                  "logged contour solver produced no placements")
        || !check(std::isfinite(first.coveredArea)
                      && std::isfinite(first.residualArea)
                      && std::isfinite(first.outsideArea),
                  "logged contour solver metrics are not finite")
        || !check(first.outsideArea
                          <= first.placements.size() * options.epsSpill + 1e-6,
                  "logged contour solver exceeded spill tolerance")
        || !check(first.placements.size() == second.placements.size(),
                  "logged contour solver placement count is not repeatable")
        || !check(std::isfinite(first.profile.totalWallSeconds)
                      && first.profile.totalWallSeconds > 0.0
                      && std::isfinite(
                          first.profile.candidateBatchWallSeconds)
                      && first.profile.candidateBatchWallSeconds > 0.0,
                  "logged contour solver profiling times are invalid")
        || !check(first.profile.greedySteps > 0
                      && first.profile.workerThreads > 0
                      && first.profile.candidateJobs > 0
                      && first.profile.adamEvaluations > 0
                      && first.profile.legalizationEvaluations > 0,
                  "logged contour solver profiling counts are invalid")
        || !check(
            first.profile.evaluationBackend
                    != QStringLiteral("CPU")
                ? (first.profile.gpuBatches > 0
                   && first.profile.gpuEvaluationWallSeconds > 0.0
                   && !first.profile.gpuAdapter.isEmpty())
                : (first.profile.adamEvaluationWorkerSeconds > 0.0
                   && first.profile.legalizationWorkerSeconds > 0.0),
            "logged contour solver backend profiling is invalid")
        || !check(!progress.isEmpty() && progress.front().placementCount == 0,
                  "logged contour solver did not report initial progress")
        || !check(progress.back().placementCount == first.placements.size(),
                  "logged contour solver progress placement count is incomplete")
        || !check(std::any_of(
                      progress.cbegin(), progress.cend(),
                      [](const cover::FillProgress &update) {
                          return std::isfinite(update.etaSeconds)
                              && update.etaSeconds >= 0.0;
                      }),
                  "logged contour solver did not produce an ETA")) {
        return false;
    }
    for (int i = 0; i < progress.size(); ++i) {
        const cover::FillProgress &update = progress[i];
        if (!check(std::isfinite(update.targetArea)
                       && std::isfinite(update.coveredArea)
                       && std::isfinite(update.residualArea)
                       && std::isfinite(update.elapsedSeconds)
                       && (update.etaSeconds < 0.0
                           || std::isfinite(update.etaSeconds)),
                   "logged contour solver progress metrics are invalid")
            || !check(i == 0
                          || (update.placementCount
                                  >= progress[i - 1].placementCount
                              && update.coveredArea
                                  >= progress[i - 1].coveredArea
                              && update.residualArea
                                  <= progress[i - 1].residualArea),
                      "logged contour solver progress is not monotonic")) {
            return false;
        }
    }
    for (int i = 0; i < first.placements.size(); ++i) {
        if (!check(repeatablePlacement(first.placements[i], second.placements[i]),
                   "logged contour solver transforms are not repeatable")) {
            return false;
        }
    }
    std::cout << "logged contour: " << first.placements.size()
              << " placements, " << first.coveredArea
              << " covered, " << first.residualArea << " residual, "
              << qPrintable(first.profile.evaluationBackend)
              << ", " << first.profile.gpuBatches << " GPU batches";
    if (!first.profile.gpuError.isEmpty()) {
        std::cout << ", fallback: " << qPrintable(first.profile.gpuError);
    }
    std::cout << '\n';

    return true;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ShapeGeometryStore geometry;
    QString error;
    if (!check(geometry.loadDefault(&error),
               qPrintable(QStringLiteral("geometry load failed: %1").arg(error)))) {
        return 1;
    }
    const QVector<cover::ShapeMesh> catalog =
        cover::buildShapeCatalog(geometry, &error);
    if (!check(error.isEmpty(),
               qPrintable(QStringLiteral("catalog build failed: %1").arg(error)))
        || !testCatalogAndGradient(catalog)
        || !testLoggedContour(catalog)) {
        return 1;
    }

    std::cout << "Differentiable cover tests passed\n";
    return 0;
}
