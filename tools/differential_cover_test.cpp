#include "differential_cover.h"
#include "differential_cover_gpu.h"
#include "pen_fill.h"

#include <QtCore>

#include <algorithm>
#include <atomic>
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
    constexpr int kExpectedCatalogSize = 12;
    constexpr std::array<int, 3> kExpandedShapeIds = {
        2103, 2104, 2133,
    };
    if (!check(catalog.size() == kExpectedCatalogSize,
               "default analytic catalog did not load twelve shapes")) {
        return false;
    }
    for (const int shapeId : kExpandedShapeIds) {
        if (!check(shapeById(catalog, shapeId) != nullptr,
                   "expanded analytic geometry is unavailable")) {
            return false;
        }
    }
    for (const cover::ShapeMesh &catalogShape :
         catalog) {
        if (!check(
                !catalogShape.features.isEmpty(),
                "catalog shape has no salient features")) {
            return false;
        }
        int previousBoundaryIndex = -1;
        for (const cover::ShapeFeature &feature :
             catalogShape.features) {
            if (!check(
                    std::isfinite(
                        feature.position.x)
                        && std::isfinite(
                            feature.position.y)
                        && feature.boundaryIndex
                            > previousBoundaryIndex,
                    "catalog features are invalid or unordered")) {
                return false;
            }
            previousBoundaryIndex =
                feature.boundaryIndex;
        }
        std::cout
            << "shape "
            << catalogShape.id
            << ": "
            << catalogShape.features.size()
            << " salient features\n";
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
#ifdef FLS_HAS_CUDA
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

QVector<cover::ContourSpan> contourSpans(
    const QPolygonF &polygon) {
    QVector<cover::ContourSpan> result;
    result.reserve(polygon.size());
    for (int index = 0;
         index < polygon.size(); ++index) {
        result.push_back({
            polygon[index],
            {},
            polygon[
                (index + 1)
                % polygon.size()],
            false,
        });
    }

    return result;
}

bool sameFeature(
    const cover::ContourFeature &left,
    const cover::ContourFeature &right) {
    return left.position == right.position
        && left.incomingTangent
            == right.incomingTangent
        && left.outgoingTangent
            == right.outgoingTangent
        && left.captureRadius
            == right.captureRadius
        && left.weight == right.weight
        && left.id == right.id
        && left.kind == right.kind;
}

bool testWeightedMetrics() {
    constexpr double kExtent = 10.0;
    constexpr double kShift = 1.0;
    const QPolygonF targetPolygon{
        QPointF(0.0, 0.0),
        QPointF(kExtent, 0.0),
        QPointF(kExtent, kExtent),
        QPointF(0.0, kExtent),
    };
    const cover::Polygons target{
        targetPolygon,
    };
    const QVector<cover::ContourFeature>
        firstFeatures =
            cover::extractContourFeatures(
                contourSpans(targetPolygon),
                kShift);
    const QVector<cover::ContourFeature>
        secondFeatures =
            cover::extractContourFeatures(
                contourSpans(targetPolygon),
                kShift);
    if (!check(
            firstFeatures.size() == 4
                && secondFeatures.size()
                    == firstFeatures.size()
                && std::equal(
                    firstFeatures.cbegin(),
                    firstFeatures.cend(),
                    secondFeatures.cbegin(),
                    sameFeature),
            "target feature extraction is not deterministic")) {
        return false;
    }
    double totalWeight = 0.0;
    for (int index = 0;
         index < firstFeatures.size();
         ++index) {
        totalWeight +=
            firstFeatures[index].weight;
        if (!check(
                firstFeatures[index].id
                        == index
                    && firstFeatures[index].kind
                        == cover::ContourFeatureKind::Corner,
                "square target feature is invalid")) {
            return false;
        }
    }
    if (!check(
            std::abs(totalWeight - 1.0)
                <= 1e-12,
            "target feature weights are not normalized")) {
        return false;
    }

    cover::FillOptions options;
    options.boundaryTolerance = kShift;
    const cover::Polygons envelope =
        cover::expandedCoverEnvelope(
            target, kShift);
    const cover::CoverErrorMetrics identical =
        cover::evaluateCoverMetrics(
            target, envelope, target,
            firstFeatures, options);
    if (!check(
            identical.missingArea <= 1e-9
                && identical.outsideTargetArea
                    <= 1e-9
                && identical.outsideEnvelopeArea
                    <= 1e-9
                && std::abs(
                    identical.tversky - 1.0)
                    <= 1e-12
                && identical.meanBoundaryDistance
                    <= 1e-9
                && identical.representedFeatures
                    == firstFeatures.size(),
            "identical weighted-cover metrics are invalid")) {
        return false;
    }
    const cover::CoverErrorMetrics empty =
        cover::evaluateCoverMetrics(
            target, envelope, {},
            firstFeatures, options);
    if (!check(
            std::isfinite(
                empty.meanBoundaryDistance)
                && std::isfinite(
                    empty.boundaryDistanceRms)
                && std::isfinite(
                    empty.boundaryDistance95)
                && std::isfinite(
                    empty.meanFeatureDistance)
                && std::isfinite(
                    empty.featureDistance95)
                && std::isfinite(
                    empty.maximumFeatureDistance),
            "empty weighted-cover metrics are not finite")) {
        return false;
    }

    const cover::Polygons inward{
        QPolygonF{
            QPointF(0.0, 0.0),
            QPointF(kExtent - kShift, 0.0),
            QPointF(
                kExtent - kShift,
                kExtent),
            QPointF(0.0, kExtent),
        },
    };
    const cover::Polygons outward{
        QPolygonF{
            QPointF(0.0, 0.0),
            QPointF(kExtent + kShift, 0.0),
            QPointF(
                kExtent + kShift,
                kExtent),
            QPointF(0.0, kExtent),
        },
    };
    const cover::CoverErrorMetrics missing =
        cover::evaluateCoverMetrics(
            target, envelope, inward,
            firstFeatures, options);
    const cover::CoverErrorMetrics spill =
        cover::evaluateCoverMetrics(
            target, envelope, outward,
            firstFeatures, options);

    return check(
        std::abs(missing.missingArea - 10.0)
                <= 1e-9
            && std::abs(
                spill.outsideTargetArea
                - 10.0)
                <= 1e-9
            && spill.outsideEnvelopeArea
                <= 1e-9
            && spill.maximumOutwardDistance
                <= kShift + 1e-9
            && spill.tversky
                > missing.tversky,
        "weighted metrics do not prefer bounded spill over an equal gap");
}

bool testWeightedSquare(
    const QVector<cover::ShapeMesh> &catalog) {
    constexpr double kExtent = 50.0;
    const QPolygonF polygon{
        QPointF(-kExtent, -kExtent),
        QPointF(kExtent, -kExtent),
        QPointF(kExtent, kExtent),
        QPointF(-kExtent, kExtent),
    };
    cover::FillInput input;
    input.mustCover = {polygon};
    input.boundarySpans =
        contourSpans(polygon);
    cover::FillOptions options;
    options.boundaryTolerance = 0.5;
    options.useGpu = false;
    options.useWeightedContour = true;
    input.mayCover =
        cover::expandedCoverEnvelope(
            input.mustCover,
            options.boundaryTolerance);
    const cover::FillResult result =
        cover::analyticCoverFill(
            input, catalog, options);
    std::cout
        << "weighted square metrics: "
        << result.metrics.representedFeatures
        << "/"
        << result.metrics.totalFeatures
        << " features, Tversky "
        << result.metrics.tversky
        << ", boundary p95 "
        << result.metrics.boundaryDistance95
        << ", maximum feature error "
        << result.metrics.maximumFeatureDistance
        << ", maximum outward distance "
        << result.metrics.maximumOutwardDistance
        << '\n';

    return check(
        result.error.isEmpty()
            && !result.placements.isEmpty()
            && result.metrics.totalFeatures == 4
            && result.metrics.representedFeatures
                == 4
            && result.metrics.outsideEnvelopeArea
                <= options.epsSpill + 1e-9
            && result.metrics.maximumOutwardDistance
                <= options.boundaryTolerance
                    + 1e-9,
        "weighted square did not retain its contour metrics");
}

bool testWeightedCandidatePath(
    const QVector<cover::ShapeMesh> &catalog) {
    constexpr double kExtent = 40.0;
    constexpr double kBow = 12.0;
    const QPolygonF polygon{
        QPointF(-kExtent, -kExtent),
        QPointF(kExtent, -kExtent),
        QPointF(kExtent, kExtent),
        QPointF(-kExtent, kExtent),
    };
    QVector<cover::ContourSpan> spans =
        contourSpans(polygon);
    spans[0].curved = true;
    spans[0].control =
        QPointF(0.0, -kExtent - kBow);
    spans[1].curved = true;
    spans[1].control =
        QPointF(kExtent + kBow, 0.0);
    spans[2].curved = true;
    spans[2].control =
        QPointF(0.0, kExtent + kBow);
    spans[3].curved = true;
    spans[3].control =
        QPointF(-kExtent - kBow, 0.0);

    cover::FillInput input;
    input.mustCover = {polygon};
    input.boundarySpans = spans;
    cover::FillOptions options;
    options.budget = 1;
    options.adamIterations = 4;
    options.restarts = 0;
    options.featureRestarts = 2;
    options.boundaryTolerance = 0.5;
    options.inactivityTimeoutSeconds = 0.0;
    options.useGpu = false;
    options.useWeightedContour = true;
    input.mayCover =
        cover::expandedCoverEnvelope(
            input.mustCover,
            options.boundaryTolerance);
    const cover::FillResult result =
        cover::analyticCoverFill(
            input, catalog, options);

    return check(
        result.error.isEmpty()
            && result.profile.featureCandidateJobs
                == 2
            && result.profile.evaluationBackend
                == QStringLiteral(
                    "CPU weighted contour")
            && result.metrics.totalFeatures == 4,
        "weighted analytic path did not run feature jobs");
}

bool testReflectedPlacementUnion(
    const QVector<cover::ShapeMesh> &catalog) {
    constexpr int kSquareShapeId = 101;
    constexpr double kReflectionOffset = 300.0;
    const cover::ShapeMesh *shape =
        shapeById(catalog, kSquareShapeId);
    if (!check(shape != nullptr,
               "square analytic geometry is unavailable")) {
        return false;
    }

    cover::Placement direct;
    direct.shapeId = kSquareShapeId;
    cover::Placement reflected;
    reflected.shapeId = kSquareShapeId;
    reflected.transform.a = -1.0;
    reflected.transform.e = kReflectionOffset;
    const double directArea =
        cover::placementUnionAreaForTesting(
            {direct}, catalog);
    const double reflectedArea =
        cover::placementUnionAreaForTesting(
            {reflected}, catalog);
    const double combinedArea =
        cover::placementUnionAreaForTesting(
            {direct, reflected}, catalog);
    const double expectedArea =
        directArea + reflectedArea;
    const double tolerance =
        std::max(1.0, expectedArea) * 1e-10;

    return check(
        directArea > 0.0
            && reflectedArea > 0.0
            && std::abs(
                combinedArea - expectedArea)
                <= tolerance,
        "reflected placement reduced exact union coverage");
}

bool testPartialTermination(
    const QVector<cover::ShapeMesh> &catalog) {
    constexpr int kSquareShapeId = 101;
    constexpr double kExtent = 100.0;
    const cover::ShapeMesh *shape =
        shapeById(catalog, kSquareShapeId);
    if (!check(shape != nullptr,
               "square analytic geometry is unavailable")) {
        return false;
    }

    cover::FillInput input;
    input.mustCover = {
        QPolygonF{
            QPointF(-kExtent, -kExtent),
            QPointF(kExtent, -kExtent),
            QPointF(kExtent, kExtent),
            QPointF(-kExtent, kExtent),
        },
    };
    input.mayCover = input.mustCover;
    cover::FillOptions options;
    options.budget = 2;
    options.adamIterations = 12;
    options.restarts = 0;
    options.inactivityTimeoutSeconds = 0.0;
    options.useGpu = false;
    const QVector<cover::ShapeMesh> squareCatalog{
        *shape,
    };
    std::atomic_bool cancellationRequested{false};
    const cover::FillResult cancelled =
        cover::analyticCoverFill(
            input, squareCatalog, options,
            [&cancellationRequested]() {
                return cancellationRequested.load(
                    std::memory_order_relaxed);
            },
            [&cancellationRequested](
                const cover::FillProgress &progress) {
                if (progress.placementCount > 0) {
                    cancellationRequested.store(
                        true,
                        std::memory_order_relaxed);
                }
            });
    if (!check(
            cancelled.cancelled
                && !cancelled.timedOut
                && !cancelled.placements.isEmpty()
                && cancelled.coveredArea > 0.0,
            "cancelled cover did not retain its partial fill")) {
        return false;
    }

    options.inactivityTimeoutSeconds = 1e-6;
    const cover::FillResult timedOut =
        cover::analyticCoverFill(
            input, squareCatalog, options);

    return check(
        timedOut.cancelled
            && timedOut.timedOut,
        "inactive cover did not time out");
}

bool testStructuralT(
    const QVector<cover::ShapeMesh> &catalog) {
    const QPointF origin(30.0, -40.0);
    const QPointF firstAxis(80.0, 12.0);
    const QPointF secondAxis(18.0, 95.0);
    const auto mapPoint =
        [&](double first, double second) {
            return origin
                + firstAxis * first
                + secondAxis * second;
        };
    const QPolygonF contour{
        mapPoint(1.0, 0.0),
        mapPoint(2.0, 0.0),
        mapPoint(2.0, 2.0),
        mapPoint(4.0, 2.0),
        mapPoint(4.0, 3.0),
        mapPoint(0.0, 3.0),
        mapPoint(0.0, 2.0),
        mapPoint(1.0, 2.0),
    };
    cover::FillInput input;
    input.mustCover = {contour};
    input.mayCover = {contour};
    input.boundarySpans.reserve(
        contour.size());
    for (int index = 0;
         index < contour.size(); ++index) {
        input.boundarySpans.push_back({
            contour[index],
            {},
            contour[
                (index + 1)
                % contour.size()],
            false,
        });
    }
    cover::FillOptions options;
    options.budget = 2;
    options.useGpu = false;
    const cover::FillResult result =
        cover::analyticCoverFill(
            input, catalog, options);
    if (!check(
            result.error.isEmpty(),
            "structural T returned an error")
        || !check(
            result.profile.structuralAccepted,
            "structural T was not accepted")
        || !check(
            result.placements.size() == 2,
            "structural T did not use two rectangles")
        || !check(
            std::all_of(
                result.placements.cbegin(),
                result.placements.cend(),
                [](const cover::Placement &placement) {
                    return placement.shapeId == 101;
                }),
            "structural T used a non-rectangle shape")
        || !check(
            result.residualArea
                <= options.epsArea + 1e-6,
            "structural T left residual area")
        || !check(
            result.outsideArea
                <= options.epsSpill + 1e-6,
            "structural T exceeded spill tolerance")) {
        return false;
    }

    return true;
}

bool testApproximateStructuralT(
    const QVector<cover::ShapeMesh> &catalog) {
    const QPointF origin(-20.0, 15.0);
    const QPointF firstAxis(75.0, -8.0);
    const QPointF secondAxis(22.0, 90.0);
    const auto mapPoint =
        [&](double first, double second) {
            return origin
                + firstAxis * first
                + secondAxis * second;
        };
    const QPolygonF vertices{
        mapPoint(1.0, 0.0),
        mapPoint(2.0, 0.005),
        mapPoint(2.005, 2.0),
        mapPoint(4.0, 2.01),
        mapPoint(4.0, 3.0),
        mapPoint(0.0, 3.005),
        mapPoint(-0.005, 2.0),
        mapPoint(0.995, 1.995),
    };
    QVector<cover::ContourSpan>
        boundarySpans;
    boundarySpans.reserve(vertices.size());
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.moveTo(vertices.front());
    for (int index = 0;
         index < vertices.size(); ++index) {
        const QPointF start =
            vertices[index];
        const QPointF end =
            vertices[
                (index + 1)
                % vertices.size()];
        if (index == 4) {
            const QPointF control =
                mapPoint(2.0, 3.015);
            boundarySpans.push_back({
                start,
                control,
                end,
                true,
            });
            path.quadTo(control, end);
        } else {
            boundarySpans.push_back({
                start,
                {},
                end,
                false,
            });
            path.lineTo(end);
        }
    }
    path.closeSubpath();
    cover::FillInput input;
    input.mustCover =
        cover::polygonsFromPainterPath(path);
    input.boundarySpans =
        boundarySpans;
    cover::FillOptions options;
    options.budget = 2;
    options.useGpu = false;
    options.useWeightedContour = true;
    input.mayCover =
        cover::expandedCoverEnvelope(
            input.mustCover,
            options.boundaryTolerance);
    const cover::FillResult result =
        cover::analyticCoverFill(
            input, catalog, options);
    std::cout
        << "approximate structural T: "
        << qPrintable(
               result.profile.structuralReason)
        << ", rectangles="
        << result.profile.structuralRectangles
        << ", coverage="
        << result.profile.structuralCoverageRatio
        << ", residual="
        << result.profile.structuralResidualArea
        << ", residual thickness="
        << result.profile.structuralResidualThickness
        << ", outside="
        << result.profile.structuralOutsideArea
        << ", accepted="
        << result.profile.structuralAccepted
        << ", placements="
        << result.placements.size()
        << ", backend="
        << qPrintable(
               result.profile.evaluationBackend)
        << ", final outside="
        << result.outsideArea
        << '\n';

    return check(
        result.error.isEmpty()
            && result.profile.structuralAccepted
            && result.placements.size() == 2
            && result.profile.evaluationBackend
                == QStringLiteral("Structural cover")
            && result.profile.structuralCoverageRatio
                >= 0.98
            && result.profile.structuralOutsideArea
                <= options.epsSpill + 1e-6,
        "weighted approximate structural T was not reduced to two rectangles");
}

bool testCompactPolygonMesh(
    const QVector<cover::ShapeMesh> &catalog) {
    const QPointF origin(45.0, -70.0);
    const QPointF firstAxis(55.0, 13.0);
    const QPointF secondAxis(-11.0, 60.0);
    const auto mapPoint =
        [&](double first, double second) {
            return origin
                + firstAxis * first
                + secondAxis * second;
        };
    const QPolygonF contour{
        mapPoint(-3.0, 0.0),
        mapPoint(-1.0, 0.0),
        mapPoint(0.0, 2.0),
        mapPoint(1.0, 0.0),
        mapPoint(3.0, 0.0),
        mapPoint(1.0, 3.0),
        mapPoint(1.0, 6.0),
        mapPoint(-1.0, 6.0),
        mapPoint(-1.0, 3.0),
    };
    cover::FillInput input;
    input.mustCover = {contour};
    input.mayCover = input.mustCover;
    input.boundarySpans.reserve(
        contour.size());
    for (int index = 0;
         index < contour.size(); ++index) {
        input.boundarySpans.push_back({
            contour[index],
            {},
            contour[
                (index + 1)
                % contour.size()],
            false,
        });
    }
    cover::FillOptions options;
    options.budget = 20;
    options.useGpu = false;
    const cover::FillResult result =
        cover::analyticCoverFill(
            input, catalog, options);

    return check(
        result.error.isEmpty()
            && !result.profile.structuralAccepted
            && result.profile.meshAccepted
            && result.profile.meshPlacements
                == result.placements.size()
            && result.profile.meshCoverageRatio
                >= 0.98
            && result.profile.evaluationBackend
                == QStringLiteral("Polygon mesh")
            && std::all_of(
                result.placements.cbegin(),
                result.placements.cend(),
                [](const cover::Placement &placement) {
                    return placement.shapeId == 101
                        || placement.shapeId == 103;
                }),
        "multi-axis contour did not use its compact polygon mesh");
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

QPainterPath loggedContourPath(
    const QVector<PenPoint> &points,
    QVector<cover::ContourSpan> *boundarySpans) {
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
    QPointF current = ordered.front().position;
    int index = 1;
    while (index <= ordered.size()) {
        const PenPoint &next = ordered[index % ordered.size()];
        if (next.kind == PenPointKind::Hard) {
            boundarySpans->push_back({
                current,
                {},
                next.position,
                false,
            });
            result.lineTo(next.position);
            current = next.position;
            ++index;
            continue;
        }
        const PenPoint &after = ordered[(index + 1) % ordered.size()];
        const QPointF end = after.kind == PenPointKind::Hard
            ? after.position
            : (next.position + after.position) * 0.5;
        boundarySpans->push_back({
            current,
            next.position,
            end,
            true,
        });
        result.quadTo(next.position, end);
        current = end;
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
    QVector<cover::ContourSpan> boundarySpans;
    const QPainterPath contour = loggedContourPath(points, &boundarySpans);
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
    input.boundarySpans = boundarySpans;
    cover::FillOptions options;
    bool budgetOk = false;
    const int requestedBudget =
        qEnvironmentVariableIntValue("FLS_DIFFERENTIAL_TEST_BUDGET", &budgetOk);
    options.budget = budgetOk && requestedBudget > 0 ? requestedBudget : 2;
    bool iterationsOk = false;
    const int requestedIterations =
        qEnvironmentVariableIntValue("FLS_DIFFERENTIAL_TEST_ITERATIONS",
                                     &iterationsOk);
    options.adamIterations =
        iterationsOk && requestedIterations > 0 ? requestedIterations : 12;
    bool restartsOk = false;
    const int requestedRestarts =
        qEnvironmentVariableIntValue("FLS_DIFFERENTIAL_TEST_RESTARTS",
                                     &restartsOk);
    options.restarts =
        restartsOk && requestedRestarts >= 0 ? requestedRestarts : 0;
    options.seed = 0;
    bool repeatOk = false;
    const int requestedRepeat =
        qEnvironmentVariableIntValue(
            "FLS_DIFFERENTIAL_TEST_REPEAT", &repeatOk);
    const bool repeat = !repeatOk || requestedRepeat != 0;
    QVector<cover::FillProgress> progress;
    const cover::FillResult first =
        cover::analyticCoverFill(
            input, catalog, options, {},
            [&progress](const cover::FillProgress &update) {
                progress.push_back(update);
            });
    const cover::FillResult second =
        repeat
        ? cover::analyticCoverFill(input, catalog, options)
        : first;
    const bool structural =
        first.profile.structuralAccepted;
    const bool mesh =
        first.profile.meshAccepted;
    const bool compactPlan =
        structural || mesh;
    std::cout
        << "logged contour structural: "
        << qPrintable(
               first.profile.structuralReason)
        << ", rectangles="
        << first.profile.structuralRectangles
        << ", coverage="
        << first.profile.structuralCoverageRatio
        << ", residual="
        << first.profile.structuralResidualArea
        << ", residual thickness="
        << first.profile.structuralResidualThickness
        << ", outside="
        << first.profile.structuralOutsideArea
        << '\n';
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
                      && (compactPlan
                              || first.profile.candidateBatchWallSeconds
                                  > 0.0),
                  "logged contour solver profiling times are invalid")
        || !check((compactPlan
                       ? ((structural
                               && first.profile.structuralRectangles > 0
                               && first.profile.structuralCoverageRatio
                                   >= 0.9)
                          || (mesh
                              && first.profile.meshPlacements > 0
                              && first.profile.meshCoverageRatio
                                  >= 0.98))
                       : (first.profile.greedySteps > 0
                          && first.profile.workerThreads > 0
                          && first.profile.candidateJobs > 0
                          && first.profile.adamEvaluations > 0
                          && first.profile.legalizationEvaluations > 0
                          && first.profile.prunePasses > 0
                          && first.profile.pruneAttempts > 0))
                      && std::isfinite(first.profile.pruneWallSeconds)
                      && first.profile.pruneWallSeconds >= 0.0
                      && std::isfinite(
                          first.profile.repairWallSeconds)
                      && first.profile.repairWallSeconds >= 0.0
                      && first.profile.repairTargetArea >= 0.0
                      && first.profile.repairCoveredArea >= 0.0
                      && first.profile.repairCoveredArea
                          <= first.profile.repairTargetArea + 1e-6
                      && first.profile.postRepairNewGapArea
                          <= first.profile.repairTargetArea + 1e-6,
                   "logged contour solver profiling counts are invalid")
        || !check(
            compactPlan
                ? ((structural
                        && first.profile.evaluationBackend
                            == QStringLiteral("Structural cover"))
                   || (mesh
                       && first.profile.evaluationBackend
                           == QStringLiteral("Polygon mesh")))
                : (first.profile.evaluationBackend
                           != QStringLiteral("CPU")
                       ? (first.profile.gpuBatches > 0
                          && first.profile.gpuEvaluationWallSeconds > 0.0
                          && !first.profile.gpuAdapter.isEmpty())
                       : (first.profile.adamEvaluationWorkerSeconds > 0.0
                          && first.profile.legalizationWorkerSeconds > 0.0)),
            "logged contour solver backend profiling is invalid")
        || !check(!progress.isEmpty() && progress.front().placementCount == 0,
                  "logged contour solver did not report initial progress")
        || !check(progress.back().placementCount == first.placements.size(),
                  "logged contour solver progress placement count is incomplete")
        || !check(progress.back().elapsedSeconds >= 0.0,
                  "logged contour solver did not report elapsed time")) {
        return false;
    }
    enum class ProgressPhase {
        Greedy,
        Prune,
        Repair,
    };
    ProgressPhase progressPhase =
        ProgressPhase::Greedy;
    double peakCoveredArea = 0.0;
    for (int i = 0; i < progress.size(); ++i) {
        const cover::FillProgress &update = progress[i];
        peakCoveredArea =
            std::max(peakCoveredArea, update.coveredArea);
        if (i > 0
            && update.placementCount
                < progress[i - 1].placementCount) {
            progressPhase = ProgressPhase::Prune;
        } else if (i > 0
                   && progressPhase
                       == ProgressPhase::Prune
                   && update.placementCount
                       > progress[i - 1].placementCount) {
            progressPhase = ProgressPhase::Repair;
        }
        if (!check(std::isfinite(update.targetArea)
                       && std::isfinite(update.coveredArea)
                       && std::isfinite(update.residualArea)
                       && std::isfinite(update.elapsedSeconds)
                       && update.elapsedSeconds >= 0.0,
                   "logged contour solver progress metrics are invalid")
            || !check(
                i == 0
                    || (progressPhase
                            == ProgressPhase::Prune
                        ? (update.placementCount
                               <= progress[i - 1].placementCount
                           && update.coveredArea
                               >= peakCoveredArea
                                   - options.epsArea
                                   - 1e-6)
                        : (update.placementCount
                               >= progress[i - 1].placementCount
                           && update.coveredArea
                               >= progress[i - 1].coveredArea
                           && update.residualArea
                               <= progress[i - 1].residualArea)),
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
              << ", " << first.profile.totalWallSeconds << " seconds, "
              << first.profile.gpuBatches << " GPU batches, "
              << first.profile.complexitySelections
              << " complexity selections, "
              << first.profile.localComponentPlacements
              << " local-component placements, "
              << first.profile.wholeComponentPlacements
              << " whole-component placements, "
              << first.profile.hardEdgePlacements
              << " hard-edge placements from "
              << first.profile.hardEdgeCandidates
              << " hard-edge candidates, "
              << first.profile.structuralRectangles
              << " structural rectangles ("
              << qPrintable(
                     first.profile.structuralReason)
              << "), "
              << first.profile.meshPlacements
              << " mesh placements ("
              << qPrintable(
                     first.profile.meshReason)
              << ", coverage "
              << first.profile.meshCoverageRatio
              << ", scale "
              << first.profile.meshScale
              << "), "
              << first.profile.prunedPlacements
              << " pruned placements after "
              << first.profile.pruneAttempts
              << " attempts and "
              << first.profile.pruneOptimizations
              << " survivor optimizations, "
              << first.profile.repairTargetArea
              << " repair target, "
              << first.profile.repairPlacements
              << " repair placements covering "
              << first.profile.repairCoveredArea
              << ", " << first.profile.postRepairNewGapArea
              << " newly exposed area remaining";
    if (!first.profile.gpuError.isEmpty()) {
        std::cout << ", fallback: " << qPrintable(first.profile.gpuError);
    }
    std::cout << '\n';

    return true;
}

bool testWeightedLoggedContour(
    const QVector<cover::ShapeMesh> &catalog) {
    constexpr double kBoundaryTolerance = 0.1;
    constexpr double kCompactCoverageRatio = 0.98;
    constexpr double kEnvelopeAreaTolerance = 1e-6;
    QVector<PenPoint> points;
    bool available = false;
    if (!loadLoggedContour(&points, &available)) {
        return false;
    }
    if (!available) {
        return true;
    }

    QVector<cover::ContourSpan> boundarySpans;
    const QPainterPath contour =
        loggedContourPath(
            points, &boundarySpans);
    const cover::Polygons polygons =
        cover::polygonsFromPainterPath(
            contour);
    if (!check(
            !polygons.isEmpty(),
            "weighted logged Pen contour did not flatten")) {
        return false;
    }

    cover::FillInput input;
    input.mustCover = polygons;
    input.boundarySpans = boundarySpans;
    cover::FillOptions options;
    const auto configuredInteger =
        [](const char *name, int fallback) {
            bool valid = false;
            const int value =
                qEnvironmentVariableIntValue(
                    name, &valid);
            return valid && value >= 0
                ? value
                : fallback;
        };
    options.budget = configuredInteger(
        "FLS_DIFFERENTIAL_WEIGHTED_TEST_BUDGET",
        2);
    options.adamIterations = configuredInteger(
        "FLS_DIFFERENTIAL_WEIGHTED_TEST_ITERATIONS",
        12);
    options.restarts = configuredInteger(
        "FLS_DIFFERENTIAL_WEIGHTED_TEST_RESTARTS",
        0);
    options.featureRestarts = configuredInteger(
        "FLS_DIFFERENTIAL_WEIGHTED_TEST_FEATURE_RESTARTS",
        2);
    options.boundaryTolerance =
        kBoundaryTolerance;
    const auto configuredDouble =
        [](const char *name, double fallback) {
            bool valid = false;
            const double value =
                qEnvironmentVariable(name)
                    .toDouble(&valid);
            return valid
                ? value
                : fallback;
        };
    options.areaWindowRatio =
        configuredDouble(
            "FLS_DIFFERENTIAL_WEIGHTED_TEST_AREA_WINDOW",
            options.areaWindowRatio);
    options.featureWeight =
        configuredDouble(
            "FLS_DIFFERENTIAL_WEIGHTED_TEST_FEATURE_WEIGHT",
            options.featureWeight);
    options.inactivityTimeoutSeconds = 0.0;
    options.useGpu = true;
    options.useWeightedContour = true;
    input.mayCover =
        cover::expandedCoverEnvelope(
            input.mustCover,
            options.boundaryTolerance);
    bool targetResidualValid = false;
    const double targetResidual =
        qEnvironmentVariable(
            "FLS_DIFFERENTIAL_WEIGHTED_TEST_TARGET_RESIDUAL")
            .toDouble(
                &targetResidualValid);
    std::atomic_bool stopRequested = false;
    int targetPlacementCount = 0;
    const cover::FillResult result =
        cover::analyticCoverFill(
            input, catalog, options,
            [&stopRequested]() {
                return stopRequested.load(
                    std::memory_order_relaxed);
            },
            [&](const cover::FillProgress &progress) {
                if (targetResidualValid
                    && progress.placementCount > 0
                    && progress.residualArea
                        <= targetResidual) {
                    targetPlacementCount =
                        progress.placementCount;
                    stopRequested.store(
                        true,
                        std::memory_order_relaxed);
                }
            });
    const bool finiteMetrics =
        std::isfinite(
            result.metrics.meanBoundaryDistance)
        && std::isfinite(
            result.metrics.boundaryDistanceRms)
        && std::isfinite(
            result.metrics.boundaryDistance95)
        && std::isfinite(
            result.metrics.meanFeatureDistance)
        && std::isfinite(
            result.metrics.featureDistance95)
        && std::isfinite(
            result.metrics.maximumFeatureDistance);
    const bool compactStructural =
        result.profile.structuralAccepted
        && result.profile.structuralCoverageRatio
            >= kCompactCoverageRatio;
    const bool backendProfileValid =
        compactStructural
        || result.profile.evaluationBackend
            == QStringLiteral(
                "CPU weighted contour")
        || (result.profile.gpuBatches > 0
            && result.profile
                   .gpuEvaluationWallSeconds
                > 0.0);

    std::cout
        << "weighted logged contour: "
        << result.placements.size()
        << " placements, "
        << result.coveredArea
        << " covered, "
        << qPrintable(
               result.profile
                   .evaluationBackend)
        << ", "
        << result.residualArea
        << " residual, "
        << result.profile.totalWallSeconds
        << " seconds, "
        << result.profile.gpuBatches
        << " GPU batches, "
        << result.profile
               .selectionEnvelopeRejections
        << " envelope rejections, "
        << result.profile
               .selectionOutwardDistanceRejections
        << " distance rejections, "
        << result.profile
               .selectionFeatureRejections
        << " feature rejections\n";
    if (targetResidualValid) {
        std::cout
            << "weighted logged contour target: "
            << targetPlacementCount
            << " placements at residual "
            << targetResidual
            << '\n';
    }

    return check(
        result.error.isEmpty()
            && !result.placements.isEmpty()
            && finiteMetrics
            && backendProfileValid
            && (compactStructural
                || (result.metrics.outsideEnvelopeArea
                        <= kEnvelopeAreaTolerance
                            + 1e-9
                    && result.metrics.maximumOutwardDistance
                        <= options.boundaryTolerance
                            + 1e-5)),
        "weighted logged contour produced no legal placement");
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
        || !testWeightedMetrics()
        || !testWeightedSquare(catalog)
        || !testWeightedCandidatePath(catalog)
        || !testReflectedPlacementUnion(catalog)
        || !testPartialTermination(catalog)
        || !testStructuralT(catalog)
        || !testApproximateStructuralT(catalog)
        || !testCompactPolygonMesh(catalog)
        || !testLoggedContour(catalog)
        || !testWeightedLoggedContour(catalog)) {
        return 1;
    }

    std::cout << "Differential cover tests passed\n";
    return 0;
}
