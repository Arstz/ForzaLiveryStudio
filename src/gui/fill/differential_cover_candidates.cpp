#include "differential_cover_internal.h"
#include "pen_fill.h"

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

            const QRectF targetBounds = polygon.boundingRect();
            const QRectF sourceBounds = shapeBounds(shape);
            if (targetBounds.width() > kGeometryEpsilon
                && targetBounds.height() > kGeometryEpsilon
                && sourceBounds.width() > kGeometryEpsilon
                && sourceBounds.height() > kGeometryEpsilon) {
                CandidateJob axisAlignedJob;
                axisAlignedJob.shape = &shape;
                axisAlignedJob.transform.a =
                    targetBounds.width() / sourceBounds.width();
                axisAlignedJob.transform.d =
                    targetBounds.height() / sourceBounds.height();
                axisAlignedJob.transform.e =
                    targetBounds.center().x()
                    - axisAlignedJob.transform.a
                        * sourceBounds.center().x();
                axisAlignedJob.transform.f =
                    targetBounds.center().y()
                    - axisAlignedJob.transform.d
                        * sourceBounds.center().y();
                axisAlignedJob.origin =
                    CandidateOrigin::WholeComponent;
                axisAlignedJob.hasTransform = true;
                result.push_back(axisAlignedJob);
            }
        }
    }

    return result;
}

QPointF shapeFeaturePoint(
    const ShapeFeature &feature) {
    return {
        feature.position.x,
        feature.position.y,
    };
}

QPointF shapeFeatureTangent(
    const ShapeFeature &feature) {
    return {
        feature.outgoingTangent.x,
        feature.outgoingTangent.y,
    };
}

bool compatibleFeature(
    const ShapeFeature &source,
    const ContourFeature &target) {
    return target.kind
               == ContourFeatureKind::SmoothJunction
        || source.kind
               == ContourFeatureKind::Corner;
}

Affine singleFeatureTransform(
    const ShapeMesh &shape,
    const ShapeFeature &source,
    const ContourFeature &target,
    const DistanceSeed &seed) {
    const QRectF bounds = shapeBounds(shape);
    const QPointF center = bounds.center();
    double radius = 0.0;
    for (const Vec2 &point : shape.boundary) {
        radius = std::max(
            radius,
            QLineF(
                center,
                QPointF(
                    point.x, point.y)).length());
    }
    const double scale = std::max(
        kMinimumAffineScale,
        seed.radius * kInitialRadiusFraction
            / std::max(
                radius, kGeometryEpsilon));
    const QPointF sourceTangent =
        shapeFeatureTangent(source);
    const double angle =
        std::atan2(
            target.outgoingTangent.y(),
            target.outgoingTangent.x())
        - std::atan2(
            sourceTangent.y(),
            sourceTangent.x());
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const QPointF sourcePoint =
        shapeFeaturePoint(source);
    Affine result;
    result.a = cosine * scale;
    result.b = sine * scale;
    result.c = -sine * scale;
    result.d = cosine * scale;
    result.e =
        target.position.x()
        - result.a * sourcePoint.x()
        - result.c * sourcePoint.y();
    result.f =
        target.position.y()
        - result.b * sourcePoint.x()
        - result.d * sourcePoint.y();

    return result;
}

std::optional<Affine> pairedFeatureTransform(
    const ShapeFeature &firstSource,
    const ShapeFeature &secondSource,
    const ContourFeature &firstTarget,
    const ContourFeature &secondTarget) {
    const QPointF sourceDelta =
        shapeFeaturePoint(secondSource)
        - shapeFeaturePoint(firstSource);
    const QPointF targetDelta =
        secondTarget.position
        - firstTarget.position;
    const double sourceLength =
        std::hypot(
            sourceDelta.x(),
            sourceDelta.y());
    const double targetLength =
        std::hypot(
            targetDelta.x(),
            targetDelta.y());
    if (sourceLength <= kGeometryEpsilon
        || targetLength <= kGeometryEpsilon) {
        return std::nullopt;
    }

    const double scale =
        targetLength / sourceLength;
    const double angle =
        std::atan2(
            targetDelta.y(),
            targetDelta.x())
        - std::atan2(
            sourceDelta.y(),
            sourceDelta.x());
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const QPointF sourcePoint =
        shapeFeaturePoint(firstSource);
    Affine result;
    result.a = cosine * scale;
    result.b = sine * scale;
    result.c = -sine * scale;
    result.d = cosine * scale;
    result.e =
        firstTarget.position.x()
        - result.a * sourcePoint.x()
        - result.c * sourcePoint.y();
    result.f =
        firstTarget.position.y()
        - result.b * sourcePoint.x()
        - result.d * sourcePoint.y();

    return result;
}

bool sameInitialTransform(
    const Affine &left,
    const Affine &right) {
    const std::array<double, kGradientCount>
        leftValues{
            left.a, left.b, left.c,
            left.d, left.e, left.f,
        };
    const std::array<double, kGradientCount>
        rightValues{
            right.a, right.b, right.c,
            right.d, right.e, right.f,
        };
    for (int index = 0;
         index < kGradientCount; ++index) {
        const double scale = std::max({
            1.0,
            std::abs(leftValues[index]),
            std::abs(rightValues[index]),
        });
        if (std::abs(
                leftValues[index]
                - rightValues[index])
            > scale * 1e-9) {
            return false;
        }
    }

    return true;
}

QVector<CandidateJob> featureAwareJobs(
    const QVector<const ShapeMesh *> &shapes,
    const QVector<ContourFeature> &targetFeatures,
    const QVector<int> &representedFeatureIds,
    const DistanceSeed &seed,
    int maximumJobs) {
    QVector<CandidateJob> result;
    if (maximumJobs <= 0
        || targetFeatures.isEmpty()) {
        return result;
    }

    QSet<int> represented(
        representedFeatureIds.cbegin(),
        representedFeatureIds.cend());
    QVector<int> targetOrder;
    for (int index = 0;
         index < targetFeatures.size(); ++index) {
        if (!represented.contains(
                targetFeatures[index].id)) {
            targetOrder.push_back(index);
        }
    }
    std::stable_sort(
        targetOrder.begin(),
        targetOrder.end(),
        [&](int left, int right) {
            const ContourFeature &leftFeature =
                targetFeatures[left];
            const ContourFeature &rightFeature =
                targetFeatures[right];
            if (std::abs(
                    leftFeature.weight
                    - rightFeature.weight)
                > kGeometryEpsilon) {
                return leftFeature.weight
                    > rightFeature.weight;
            }
            return leftFeature.id
                < rightFeature.id;
        });

    auto appendJob =
        [&](CandidateJob job) {
            const bool duplicate =
                std::any_of(
                    result.cbegin(),
                    result.cend(),
                    [&](const CandidateJob &existing) {
                        return existing.shape
                                == job.shape
                            && sameInitialTransform(
                                existing.transform,
                                job.transform);
                    });
            if (!duplicate
                && result.size() < maximumJobs) {
                result.push_back(
                    std::move(job));
            }
        };
    for (const int targetIndex : targetOrder) {
        const int adjacentIndex =
            (targetIndex + 1)
            % targetFeatures.size();
        const ContourFeature &firstTarget =
            targetFeatures[targetIndex];
        const ContourFeature &secondTarget =
            targetFeatures[adjacentIndex];
        if (represented.contains(
                secondTarget.id)) {
            continue;
        }
        for (const ShapeMesh *shape : shapes) {
            for (int sourceIndex = 0;
                 sourceIndex
                     < shape->features.size();
                 ++sourceIndex) {
                const ShapeFeature &firstSource =
                    shape->features[sourceIndex];
                const ShapeFeature &secondSource =
                    shape->features[
                        (sourceIndex + 1)
                        % shape->features.size()];
                if (!compatibleFeature(
                        firstSource, firstTarget)
                    || !compatibleFeature(
                        secondSource,
                        secondTarget)) {
                    continue;
                }
                const std::optional<Affine>
                    transform =
                        pairedFeatureTransform(
                            firstSource,
                            secondSource,
                            firstTarget,
                            secondTarget);
                if (!transform.has_value()) {
                    continue;
                }
                CandidateJob job;
                job.shape = shape;
                job.featureAssignments = {
                    {firstSource, firstTarget},
                    {secondSource, secondTarget},
                };
                job.transform = *transform;
                job.origin =
                    CandidateOrigin::Feature;
                job.hasTransform = true;
                appendJob(std::move(job));
                if (result.size()
                    >= maximumJobs) {
                    return result;
                }
            }
        }
    }
    for (const int targetIndex : targetOrder) {
        const ContourFeature &target =
            targetFeatures[targetIndex];
        for (const ShapeMesh *shape : shapes) {
            for (const ShapeFeature &source :
                 shape->features) {
                if (!compatibleFeature(
                        source, target)) {
                    continue;
                }
                CandidateJob job;
                job.shape = shape;
                job.featureAssignments = {
                    {source, target},
                };
                job.transform =
                    singleFeatureTransform(
                        *shape, source,
                        target, seed);
                job.origin =
                    CandidateOrigin::Feature;
                job.hasTransform = true;
                appendJob(std::move(job));
                if (result.size()
                    >= maximumJobs) {
                    return result;
                }
            }
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

QVector<FixedCandidate> polygonMeshCandidates(
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

namespace {

constexpr int kMaximumInitialFillJobs = 24;

PenPrimitive penPrimitiveFromShape(
    const ShapeMesh &shape) {
    PenPrimitive result;
    result.shapeId = shape.id;
    QPolygonF polygon = shapePolygon(shape);
    if (signedArea(polygon) < 0.0) {
        std::reverse(
            polygon.begin(), polygon.end());
    }
    result.silhouette.setFillRule(
        Qt::WindingFill);
    result.silhouette.addPolygon(polygon);
    result.silhouette.closeSubpath();
    result.silhouette =
        result.silhouette.simplified();
    result.silhouette.setFillRule(
        Qt::WindingFill);
    result.contours =
        result.silhouette.toSubpathPolygons();
    result.bounds =
        result.silhouette.boundingRect();
    result.area = shape.area;

    return result;
}

QVector<PenLoop> penLoopsFromBoundary(
    const QVector<QVector<ContourSpan>> &boundaryLoops) {
    QVector<PenLoop> result;
    result.reserve(boundaryLoops.size());
    for (int loopIndex = 0;
         loopIndex < boundaryLoops.size();
         ++loopIndex) {
        const QVector<ContourSpan> &spans =
            boundaryLoops[loopIndex];
        if (spans.size() < 2) {
            return {};
        }
        PenLoop loop;
        loop.kind = loopIndex == 0
            ? PenLoopKind::Outer
            : PenLoopKind::Cutout;
        loop.points.reserve(spans.size() * 2);
        for (const ContourSpan &span : spans) {
            loop.points.push_back({
                span.start,
                PenPointKind::Hard,
            });
            if (span.curved) {
                loop.points.push_back({
                    span.control,
                    PenPointKind::Soft,
                });
            }
        }
        result.push_back(std::move(loop));
    }

    return result;
}

} // namespace

bool sameCandidateSeed(
    const CandidateJob &job,
    const ShapeMesh &shape,
    const Affine &transform) {
    if (job.shape == nullptr
        || job.shape->id != shape.id) {
        return false;
    }
    const std::array<double, kGradientCount>
        jobValues = affineValues(job.transform);
    const std::array<double, kGradientCount>
        transformValues = affineValues(transform);
    for (int parameter = 0;
         parameter < kGradientCount;
         ++parameter) {
        if (std::abs(
                jobValues[parameter]
                - transformValues[parameter])
            > kGeometryEpsilon) {
            return false;
        }
    }

    return true;
}

AnalyticSeedPlan analyticSeedPlan(
    const QVector<QVector<ContourSpan>> &boundaryLoops,
    const QVector<ShapeMesh> &catalog,
    double boundaryTolerance,
    const std::function<bool()> &cancelled) {
    AnalyticSeedPlan result;
    const QVector<PenLoop> loops =
        penLoopsFromBoundary(boundaryLoops);
    if (loops.isEmpty()) {
        result.reason =
            QStringLiteral("boundary geometry is unavailable");
        return result;
    }
    PenFillRequest request;
    request.loops = loops;
    request.boundaryTolerance =
        boundaryTolerance;
    request.discardNegligiblePlacements = false;
    request.primitives.reserve(catalog.size());
    for (const ShapeMesh &shape : catalog) {
        request.primitives.push_back(
            penPrimitiveFromShape(shape));
    }
    const PenFillResult fill =
        fillPenPath(request, cancelled);
    result.cancelled =
        fill.cancelled
        || (cancelled && cancelled());
    if (result.cancelled) {
        result.reason =
            QStringLiteral("cancelled");
        return result;
    }
    if (!fill.error.isEmpty()) {
        result.reason = fill.error;
        return result;
    }
    result.jobs.reserve(fill.placements.size());
    for (const PenPlacement &placement :
         fill.placements) {
        const ShapeMesh *shape =
            shapeById(
                catalog,
                placement.shapeId);
        if (shape == nullptr) {
            continue;
        }
        CandidateJob job;
        job.shape = shape;
        job.transform =
            fromQTransform(
                placement.transform);
        job.origin =
            CandidateOrigin::AnalyticSeed;
        job.hasTransform = true;
        const bool duplicate =
            std::any_of(
                result.jobs.cbegin(),
                result.jobs.cend(),
                [&](const CandidateJob &existing) {
                    return sameCandidateSeed(
                        existing,
                        *job.shape,
                        job.transform);
                });
        if (!duplicate) {
            result.placements.push_back({
                job.transform,
                job.shape->id,
                0.0,
            });
            result.jobs.push_back(
                std::move(job));
        }
    }
    result.reason = result.jobs.isEmpty()
        ? QStringLiteral("analytic fill produced no placements")
        : QStringLiteral("analytic fill seed candidates");

    return result;
}

QVector<CandidateJob> rankedInitialFillJobs(
    const QVector<CandidateJob> &seeds,
    const Polygons &residual,
    const Polygons &target,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options) {
    struct RankedJob {
        CandidateJob job;
        double score = 0.0;
        double covered = 0.0;
    };

    QVector<RankedJob> ranked;
    ranked.reserve(seeds.size());
    for (const CandidateJob &seed : seeds) {
        if (seed.shape == nullptr
            || !seed.hasTransform) {
            continue;
        }
        const AreaGradient evaluation =
            areaGradient(
                *seed.shape,
                seed.transform,
                residual, target,
                subjectBounds);
        if (!finiteGradient(evaluation)
            || evaluation.covered
                < options.epsGain) {
            continue;
        }
        ranked.push_back({
            seed,
            evaluation.covered
                - options.spillWeight
                    * std::max(
                        0.0,
                        evaluation.spill),
            evaluation.covered,
        });
    }
    std::stable_sort(
        ranked.begin(), ranked.end(),
        [](const RankedJob &left,
           const RankedJob &right) {
            if (std::abs(
                    left.score - right.score)
                > kGeometryEpsilon) {
                return left.score > right.score;
            }
            if (std::abs(
                    left.covered
                    - right.covered)
                > kGeometryEpsilon) {
                return left.covered
                    > right.covered;
            }
            if (left.job.shape->id
                != right.job.shape->id) {
                return left.job.shape->id
                    < right.job.shape->id;
            }

            return lexicographicTransformLess(
                left.job.transform,
                right.job.transform);
        });

    QVector<CandidateJob> result;
    const int count = std::min(
        static_cast<int>(ranked.size()),
        kMaximumInitialFillJobs);
    result.reserve(count);
    for (int index = 0;
         index < count; ++index) {
        result.push_back(
            std::move(ranked[index].job));
    }

    return result;
}

namespace {

constexpr int kMaximumHardPointJobs = 8;
constexpr int kCurveRunLengthSamples = 16;
constexpr double kMinimumHardTriangleQuality = 0.02;
constexpr double kMinimumCurveEllipseQuality = 0.01;
constexpr double kSoftCurveJoinTolerance = 1e-7;
constexpr double kHardPointProbeSpillWeight = 2.0;
constexpr std::array<double, 3> kHardPointProbeScales{
    1.0,
    1.5,
    2.0,
};

struct CurveRunMiddle {
    QPointF position;
    QPointF tangent;
};

double hardPointCross(const QPointF &left,
                      const QPointF &right) {
    return left.x() * right.y()
        - left.y() * right.x();
}

double hardPointPolygonArea(
    const QVector<ContourSpan> &spans) {
    double twiceArea = 0.0;
    for (int index = 0; index < spans.size(); ++index) {
        const QPointF &left = spans[index].start;
        const QPointF &right =
            spans[(index + 1) % spans.size()].start;
        twiceArea += hardPointCross(left, right);
    }

    return twiceArea * 0.5;
}

double hardTriangleQuality(const QPointF &first,
                           const QPointF &middle,
                           const QPointF &last) {
    const QPointF firstEdge = middle - first;
    const QPointF secondEdge = last - middle;
    const QPointF thirdEdge = first - last;
    const double edgeSquares =
        QPointF::dotProduct(firstEdge, firstEdge)
        + QPointF::dotProduct(secondEdge, secondEdge)
        + QPointF::dotProduct(thirdEdge, thirdEdge);
    if (edgeSquares <= kGeometryEpsilon) {
        return 0.0;
    }
    const double twiceArea =
        std::abs(hardPointCross(firstEdge, secondEdge));

    return 2.0 * std::sqrt(3.0)
        * twiceArea / edgeSquares;
}

QPointF quadraticPoint(
    const ContourSpan &span,
    double parameter) {
    const double inverse = 1.0 - parameter;

    return span.start * (inverse * inverse)
        + span.control
            * (2.0 * inverse * parameter)
        + span.end * (parameter * parameter);
}

QPointF quadraticTangent(
    const ContourSpan &span,
    double parameter) {
    const QPointF derivative =
        (span.control - span.start)
            * (1.0 - parameter)
        + (span.end - span.control)
            * parameter;
    const double length = std::hypot(
        derivative.x(), derivative.y());

    return length > kGeometryEpsilon
        ? derivative / length : QPointF{};
}

double curveSpanLength(
    const ContourSpan &span) {
    QPointF previous = span.start;
    double result = 0.0;
    for (int sample = 1;
         sample <= kCurveRunLengthSamples;
         ++sample) {
        const double parameter =
            static_cast<double>(sample)
            / static_cast<double>(
                kCurveRunLengthSamples);
        const QPointF point =
            quadraticPoint(span, parameter);
        result += QLineF(previous, point).length();
        previous = point;
    }

    return result;
}

bool softCurveJoin(
    const ContourSpan &left,
    const ContourSpan &right) {
    if (!left.curved || !right.curved
        || QLineF(left.end, right.start).length()
            > kSoftCurveJoinTolerance) {
        return false;
    }
    const QPointF expected =
        (left.control + right.control) * 0.5;

    return QLineF(left.end, expected).length()
        <= kSoftCurveJoinTolerance;
}

bool outwardCurveRun(
    const QVector<ContourSpan> &spans,
    int first,
    int count,
    double orientation) {
    const QPointF runStart = spans[first].start;
    const ContourSpan &lastSpan =
        spans[(first + count - 1) % spans.size()];
    const QPointF runChord =
        lastSpan.end - runStart;
    if (std::hypot(
            runChord.x(), runChord.y())
        <= kGeometryEpsilon) {
        return false;
    }
    for (int offset = 0;
         offset < count; ++offset) {
        const ContourSpan &span =
            spans[(first + offset) % spans.size()];
        const QPointF spanChord =
            span.end - span.start;
        const QPointF spanMiddle =
            quadraticPoint(span, 0.5);
        const QPointF chordMiddle =
            (span.start + span.end) * 0.5;
        if (hardPointCross(
                spanChord,
                spanMiddle - chordMiddle)
                    * orientation
                >= -kGeometryEpsilon
            || hardPointCross(
                runChord,
                span.control - runStart)
                    * orientation
                >= -kGeometryEpsilon) {
            return false;
        }
    }

    return true;
}

std::optional<CurveRunMiddle> curveRunMiddle(
    const QVector<ContourSpan> &spans,
    int first,
    int count) {
    QVector<double> lengths;
    lengths.reserve(count);
    double totalLength = 0.0;
    for (int offset = 0;
         offset < count; ++offset) {
        const double length = curveSpanLength(
            spans[(first + offset) % spans.size()]);
        lengths.push_back(length);
        totalLength += length;
    }
    if (totalLength <= kGeometryEpsilon) {
        return std::nullopt;
    }

    const double middleLength = totalLength * 0.5;
    double accumulatedLength = 0.0;
    for (int offset = 0;
         offset < count; ++offset) {
        const ContourSpan &span =
            spans[(first + offset) % spans.size()];
        if (accumulatedLength + lengths[offset]
                < middleLength
            && offset + 1 < count) {
            accumulatedLength += lengths[offset];
            continue;
        }
        const double localTarget =
            middleLength - accumulatedLength;
        QPointF previous = span.start;
        double localLength = 0.0;
        for (int sample = 1;
             sample <= kCurveRunLengthSamples;
             ++sample) {
            const double parameter =
                static_cast<double>(sample)
                / static_cast<double>(
                    kCurveRunLengthSamples);
            const QPointF point =
                quadraticPoint(span, parameter);
            const double segmentLength =
                QLineF(previous, point).length();
            if (localLength + segmentLength
                    >= localTarget
                || sample
                    == kCurveRunLengthSamples) {
                const double fraction =
                    segmentLength > kGeometryEpsilon
                    ? std::clamp(
                        (localTarget - localLength)
                            / segmentLength,
                        0.0, 1.0)
                    : 0.0;
                const double previousParameter =
                    static_cast<double>(sample - 1)
                    / static_cast<double>(
                        kCurveRunLengthSamples);
                const double middleParameter =
                    previousParameter
                    + fraction
                        / static_cast<double>(
                            kCurveRunLengthSamples);
                const QPointF tangent =
                    quadraticTangent(
                        span, middleParameter);
                if (QPointF::dotProduct(
                        tangent, tangent)
                    <= kGeometryEpsilon) {
                    return std::nullopt;
                }

                return CurveRunMiddle{
                    quadraticPoint(
                        span, middleParameter),
                    tangent,
                };
            }
            localLength += segmentLength;
            previous = point;
        }
    }

    return std::nullopt;
}

std::optional<Affine> affineFromPointTriples(
    const Vec2 &sourceFirst,
    const Vec2 &sourceSecond,
    const Vec2 &sourceThird,
    const QPointF &targetFirst,
    const QPointF &targetSecond,
    const QPointF &targetThird) {
    const Vec2 sourceEdgeFirst{
        sourceSecond.x - sourceFirst.x,
        sourceSecond.y - sourceFirst.y,
    };
    const Vec2 sourceEdgeSecond{
        sourceThird.x - sourceFirst.x,
        sourceThird.y - sourceFirst.y,
    };
    const QPointF targetEdgeFirst =
        targetSecond - targetFirst;
    const QPointF targetEdgeSecond =
        targetThird - targetFirst;
    const double determinant =
        sourceEdgeFirst.x * sourceEdgeSecond.y
        - sourceEdgeFirst.y * sourceEdgeSecond.x;
    if (std::abs(determinant)
        <= kGeometryEpsilon) {
        return std::nullopt;
    }

    Affine result;
    result.a =
        (targetEdgeFirst.x()
             * sourceEdgeSecond.y
         - targetEdgeSecond.x()
             * sourceEdgeFirst.y)
        / determinant;
    result.c =
        (-targetEdgeFirst.x()
             * sourceEdgeSecond.x
         + targetEdgeSecond.x()
             * sourceEdgeFirst.x)
        / determinant;
    result.b =
        (targetEdgeFirst.y()
             * sourceEdgeSecond.y
         - targetEdgeSecond.y()
             * sourceEdgeFirst.y)
        / determinant;
    result.d =
        (-targetEdgeFirst.y()
             * sourceEdgeSecond.x
         + targetEdgeSecond.y()
             * sourceEdgeFirst.x)
        / determinant;
    result.e = targetFirst.x()
        - result.a * sourceFirst.x
        - result.c * sourceFirst.y;
    result.f = targetFirst.y()
        - result.b * sourceFirst.x
        - result.d * sourceFirst.y;
    const std::array<double, kGradientCount> values =
        affineValues(result);
    if (!std::all_of(
            values.cbegin(), values.cend(),
            [](double value) {
                return std::isfinite(value);
            })) {
        return std::nullopt;
    }

    return result;
}

const ShapeFeature *boundaryFeature(
    const ShapeMesh &shape,
    int boundaryIndex) {
    const auto found = std::find_if(
        shape.features.cbegin(),
        shape.features.cend(),
        [boundaryIndex](
            const ShapeFeature &feature) {
            return feature.boundaryIndex
                == boundaryIndex;
        });

    return found == shape.features.cend()
        ? nullptr : &*found;
}

int nearestCircleFeature(
    const QVector<ShapeFeature> &features,
    double arcPosition) {
    int result = -1;
    double bestDistance =
        std::numeric_limits<double>::max();
    for (int index = 0;
         index < features.size(); ++index) {
        const double directDistance =
            std::abs(
                features[index].arcPosition
                - arcPosition);
        const double distance = std::min(
            directDistance,
            1.0 - directDistance);
        if (distance
            < bestDistance
                - kGeometryEpsilon) {
            result = index;
            bestDistance = distance;
        }
    }

    return result;
}

std::optional<std::array<int, 3>>
circleArcFeatureIndices(
    const ShapeMesh &circle) {
    if (circle.features.size() < 4) {
        return std::nullopt;
    }
    const int middle = 0;
    const double middleArc =
        circle.features[middle].arcPosition;
    const double firstArc =
        std::fmod(middleArc + 0.75, 1.0);
    const double lastArc =
        std::fmod(middleArc + 0.25, 1.0);
    const int first = nearestCircleFeature(
        circle.features, firstArc);
    const int last = nearestCircleFeature(
        circle.features, lastArc);
    if (first < 0
        || last < 0
        || first == middle
        || last == middle
        || first == last) {
        return std::nullopt;
    }

    return std::array<int, 3>{
        first,
        middle,
        last,
    };
}

void appendHardPointSeed(
    QVector<CandidateJob> *jobs,
    const ShapeMesh &shape,
    int anchorIndex,
    const ContourFeature &target,
    const std::optional<Affine> &transform) {
    const ShapeFeature *source =
        boundaryFeature(shape, anchorIndex);
    if (source == nullptr
        || !transform.has_value()) {
        return;
    }

    CandidateJob job;
    job.shape = &shape;
    job.featureAssignments = {
        {*source, target},
    };
    job.transform = *transform;
    job.anchor = CandidateAnchor{
        source->position,
        target.position,
    };
    job.origin = CandidateOrigin::HardEdge;
    job.hasTransform = true;
    jobs->push_back(std::move(job));
}

Affine scaledHardPointTransform(
    const CandidateJob &seed,
    double scale) {
    Affine result = seed.transform;
    result.a *= scale;
    result.b *= scale;
    result.c *= scale;
    result.d *= scale;
    if (seed.anchor.has_value()) {
        const CandidateAnchor &anchor =
            *seed.anchor;
        result.e = anchor.target.x()
            - result.a * anchor.source.x
            - result.c * anchor.source.y;
        result.f = anchor.target.y()
            - result.b * anchor.source.x
            - result.d * anchor.source.y;
    }

    return result;
}

} // namespace

QVector<CandidateJob> hardPointCandidateSeeds(
    const QVector<ContourSpan> &spans,
    const QVector<ShapeMesh> &catalog,
    double boundaryTolerance) {
    QVector<CandidateJob> result;
    if (spans.size() < 3) {
        return result;
    }
    const ShapeMesh *circle = shapeById(catalog, 102);
    const ShapeMesh *square = shapeById(catalog, 101);
    const ShapeMesh *triangle = shapeById(catalog, 103);
    if (square == nullptr
        || triangle == nullptr
        || square->boundary.size() != 4
        || triangle->boundary.size() != 3) {
        return result;
    }
    const QVector<ContourFeature> features =
        extractContourFeatures(
            spans, boundaryTolerance);
    if (features.size() != spans.size()) {
        return result;
    }
    const double orientation =
        hardPointPolygonArea(spans);
    if (std::abs(orientation)
        <= kGeometryEpsilon) {
        return result;
    }

    const std::optional<std::array<int, 3>>
        circleFeatures = circle != nullptr
        ? circleArcFeatureIndices(*circle)
        : std::nullopt;

    result.reserve(spans.size() * 3);
    for (int index = 0;
         index < spans.size(); ++index) {
        const int previous =
            (index + spans.size() - 1)
            % spans.size();
        const int next =
            (index + 1) % spans.size();
        if (features[previous].kind
                != ContourFeatureKind::Corner
            || features[index].kind
                != ContourFeatureKind::Corner
            || features[next].kind
                != ContourFeatureKind::Corner) {
            continue;
        }
        const QPointF &first =
            features[previous].position;
        const QPointF &middle =
            features[index].position;
        const QPointF &last =
            features[next].position;
        const double turn = hardPointCross(
            middle - first,
            last - middle);
        if (turn * orientation
                <= kGeometryEpsilon
            || hardTriangleQuality(
                   first, middle, last)
                < kMinimumHardTriangleQuality) {
            continue;
        }

        appendHardPointSeed(
            &result, *triangle, 1,
            features[index],
            affineFromPointTriples(
                triangle->boundary[0],
                triangle->boundary[1],
                triangle->boundary[2],
                first, middle, last));
        appendHardPointSeed(
            &result, *square, 0,
            features[index],
            affineFromPointTriples(
                square->boundary[0],
                square->boundary[1],
                square->boundary[3],
                middle, last, first));
    }
    if (circle != nullptr
        && circleFeatures.has_value()) {
        const ShapeFeature &sourceFirst =
            circle->features[
                (*circleFeatures)[0]];
        const ShapeFeature &sourceMiddle =
            circle->features[
                (*circleFeatures)[1]];
        const ShapeFeature &sourceLast =
            circle->features[
                (*circleFeatures)[2]];
        for (int index = 0;
             index < spans.size(); ++index) {
            const int previous =
                (index + spans.size() - 1)
                % spans.size();
            if (!spans[index].curved
                || softCurveJoin(
                    spans[previous], spans[index])) {
                continue;
            }
            int count = 1;
            while (count < spans.size()) {
                const int left =
                    (index + count - 1)
                    % spans.size();
                const int right =
                    (index + count)
                    % spans.size();
                if (!softCurveJoin(
                        spans[left], spans[right])) {
                    break;
                }
                ++count;
            }
            const ContourSpan &lastSpan =
                spans[
                    (index + count - 1)
                    % spans.size()];
            const std::optional<CurveRunMiddle>
                middle = curveRunMiddle(
                    spans, index, count);
            if (!middle.has_value()
                || !outwardCurveRun(
                    spans, index, count,
                    orientation)
                || hardTriangleQuality(
                       spans[index].start,
                       middle->position,
                       lastSpan.end)
                    < kMinimumCurveEllipseQuality) {
                continue;
            }
            ContourFeature target =
                features[index];
            target.position = middle->position;
            target.incomingTangent =
                middle->tangent;
            target.outgoingTangent =
                middle->tangent;
            target.kind =
                ContourFeatureKind::SmoothJunction;
            appendHardPointSeed(
                &result, *circle,
                sourceMiddle.boundaryIndex,
                target,
                affineFromPointTriples(
                    sourceFirst.position,
                    sourceMiddle.position,
                    sourceLast.position,
                    spans[index].start,
                    middle->position,
                    lastSpan.end));
        }
    }

    return result;
}

QVector<CandidateJob> rankedHardPointJobs(
    const QVector<CandidateJob> &seeds,
    const Polygons &residual,
    const Polygons &target,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options) {
    struct RankedJob {
        CandidateJob job;
        double score = 0.0;
        double covered = 0.0;
    };

    QVector<RankedJob> ranked;
    ranked.reserve(seeds.size());
    for (const CandidateJob &seed : seeds) {
        std::optional<RankedJob> best;
        for (const double scale :
             kHardPointProbeScales) {
            CandidateJob probe = seed;
            probe.transform =
                scaledHardPointTransform(
                    seed, scale);
            const AreaGradient evaluation =
                areaGradient(
                    *probe.shape,
                    probe.transform,
                    residual, target,
                    subjectBounds);
            if (!finiteGradient(evaluation)
                || evaluation.covered
                    < options.epsGain) {
                continue;
            }
            const double score =
                evaluation.covered
                - kHardPointProbeSpillWeight
                    * std::max(
                        0.0,
                        evaluation.spill);
            if (score < options.epsGain) {
                continue;
            }
            RankedJob rankedProbe{
                std::move(probe),
                score,
                evaluation.covered,
            };
            if (!best.has_value()
                || rankedProbe.score
                    > best->score
                        + kGeometryEpsilon
                || (std::abs(
                        rankedProbe.score
                        - best->score)
                    <= kGeometryEpsilon
                    && rankedProbe.covered
                        > best->covered
                            + kGeometryEpsilon)) {
                best = std::move(
                    rankedProbe);
            }
        }
        if (best.has_value()) {
            ranked.push_back(
                std::move(*best));
        }
    }
    std::stable_sort(
        ranked.begin(), ranked.end(),
        [](const RankedJob &left,
           const RankedJob &right) {
            if (std::abs(
                    left.score - right.score)
                > kGeometryEpsilon) {
                return left.score > right.score;
            }
            if (std::abs(
                    left.covered - right.covered)
                > kGeometryEpsilon) {
                return left.covered
                    > right.covered;
            }
            if (left.job.shape->id
                != right.job.shape->id) {
                return left.job.shape->id
                    < right.job.shape->id;
            }

            return lexicographicTransformLess(
                left.job.transform,
                right.job.transform);
        });

    QVector<CandidateJob> result;
    const int count = std::min(
        static_cast<int>(ranked.size()),
        kMaximumHardPointJobs);
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        result.push_back(
            std::move(ranked[index].job));
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

QPointF transformedFeaturePoint(
    const ShapeFeature &feature,
    const Affine &transform) {
    return {
        transform.a * feature.position.x
            + transform.c * feature.position.y
            + transform.e,
        transform.b * feature.position.x
            + transform.d * feature.position.y
            + transform.f,
    };
}

QPointF transformedFeatureTangent(
    const Vec2 &tangent,
    const Affine &transform) {
    const QPointF mapped(
        transform.a * tangent.x
            + transform.c * tangent.y,
        transform.b * tangent.x
            + transform.d * tangent.y);
    const double length =
        std::hypot(
            mapped.x(), mapped.y());
    if (length <= kGeometryEpsilon) {
        return {};
    }

    return mapped / length;
}

QVector<CandidateFeatureAssignment>
placementFeatureAssignments(
    const ShapeMesh &shape,
    const Affine &transform,
    const QVector<int> &featureIds,
    const QVector<ContourFeature> &targetFeatures) {
    QVector<CandidateFeatureAssignment> result;
    QSet<int> usedBoundaryIndices;
    for (const int featureId : featureIds) {
        const auto foundTarget =
            std::find_if(
                targetFeatures.cbegin(),
                targetFeatures.cend(),
                [featureId](
                    const ContourFeature &feature) {
                    return feature.id
                        == featureId;
                });
        if (foundTarget
            == targetFeatures.cend()) {
            continue;
        }
        const ShapeFeature *bestSource = nullptr;
        double bestDistance =
            std::numeric_limits<double>::max();
        for (const ShapeFeature &source :
             shape.features) {
            if (usedBoundaryIndices.contains(
                    source.boundaryIndex)
                || !compatibleFeature(
                    source, *foundTarget)) {
                continue;
            }
            const double distance =
                QLineF(
                    transformedFeaturePoint(
                        source, transform),
                    foundTarget->position).length();
            if (distance
                    < bestDistance
                        - kGeometryEpsilon
                || (std::abs(
                        distance - bestDistance)
                        <= kGeometryEpsilon
                    && (bestSource == nullptr
                        || source.boundaryIndex
                            < bestSource
                                  ->boundaryIndex))) {
                bestSource = &source;
                bestDistance = distance;
            }
        }
        if (bestSource != nullptr) {
            usedBoundaryIndices.insert(
                bestSource->boundaryIndex);
            result.push_back({
                *bestSource,
                *foundTarget,
            });
        }
    }

    return result;
}

double tangentErrorSquared(
    const CandidateFeatureAssignment &assignment,
    const Affine &transform) {
    const QPointF sourceIncoming =
        transformedFeatureTangent(
            assignment.source.incomingTangent,
            transform);
    const QPointF sourceOutgoing =
        transformedFeatureTangent(
            assignment.source.outgoingTangent,
            transform);
    const double incomingError =
        std::acos(
            std::clamp(
                QPointF::dotProduct(
                    sourceIncoming,
                    assignment.target
                        .incomingTangent),
                -1.0, 1.0));
    const double outgoingError =
        std::acos(
            std::clamp(
                QPointF::dotProduct(
                    sourceOutgoing,
                    assignment.target
                        .outgoingTangent),
                -1.0, 1.0));
    if (assignment.target.kind
        == ContourFeatureKind::SmoothJunction) {
        return outgoingError
            * outgoingError;
    }

    return 0.5
        * (incomingError * incomingError
           + outgoingError * outgoingError);
}

double featurePotential(
    const QVector<CandidateFeatureAssignment>
        &assignments,
    const Affine &transform,
    const FillOptions &options,
    bool areaScaled) {
    double result = 0.0;
    for (const CandidateFeatureAssignment
             &assignment : assignments) {
        const QPointF mapped =
            transformedFeaturePoint(
                assignment.source,
                transform);
        const QPointF difference =
            mapped
            - assignment.target.position;
        const double radius = std::max(
            assignment.target.captureRadius,
            kGeometryEpsilon);
        const double positionReward =
            std::exp(
                -QPointF::dotProduct(
                    difference, difference)
                / (2.0 * radius * radius));
        const double tangentReward =
            std::exp(
                -tangentErrorSquared(
                    assignment, transform)
                / (2.0
                   * kFeatureTangentSigma
                   * kFeatureTangentSigma));
        double contribution =
            assignment.target.weight
            * positionReward
            * tangentReward;
        if (areaScaled) {
            contribution *=
                options.featureWeight
                * std::max(
                    options.epsGain,
                    kPi * radius * radius);
        }
        result += contribution;
    }

    return result;
}

std::array<double, kGradientCount>
featurePotentialGradient(
    const QVector<CandidateFeatureAssignment>
        &assignments,
    const Affine &transform,
    const FillOptions &options) {
    constexpr double kDifferenceStep = 1e-5;
    std::array<double, kGradientCount> result{};
    if (assignments.isEmpty()) {
        return result;
    }

    const std::array<double, kGradientCount> values =
        affineValues(transform);
    for (int parameter = 0;
         parameter < kGradientCount; ++parameter) {
        const double step =
            kDifferenceStep
            * std::max(
                1.0,
                std::abs(values[parameter]));
        auto plus = values;
        auto minus = values;
        plus[parameter] += step;
        minus[parameter] -= step;
        result[parameter] =
            (featurePotential(
                 assignments,
                 affineFromValues(plus),
                 options, true)
             - featurePotential(
                 assignments,
                 affineFromValues(minus),
                 options, true))
            / (2.0 * step);
    }

    return result;
}

const CandidateAnchor *candidateAnchor(
    const CandidateJob &job) {
    return job.anchor.has_value()
        ? &*job.anchor : nullptr;
}

void projectCandidateAnchor(
    const CandidateAnchor &anchor,
    Affine *transform) {
    transform->e = anchor.target.x()
        - transform->a * anchor.source.x
        - transform->c * anchor.source.y;
    transform->f = anchor.target.y()
        - transform->b * anchor.source.x
        - transform->d * anchor.source.y;
}

void projectCandidateAnchor(
    const CandidateAnchor &anchor,
    std::array<double, kGradientCount> *values) {
    (*values)[4] = anchor.target.x()
        - (*values)[0] * anchor.source.x
        - (*values)[2] * anchor.source.y;
    (*values)[5] = anchor.target.y()
        - (*values)[1] * anchor.source.x
        - (*values)[3] * anchor.source.y;
}

void constrainCandidateGradient(
    const CandidateAnchor &anchor,
    std::array<double, kGradientCount> *gradient) {
    (*gradient)[0] -=
        anchor.source.x * (*gradient)[4];
    (*gradient)[2] -=
        anchor.source.y * (*gradient)[4];
    (*gradient)[1] -=
        anchor.source.x * (*gradient)[5];
    (*gradient)[3] -=
        anchor.source.y * (*gradient)[5];
    (*gradient)[4] = 0.0;
    (*gradient)[5] = 0.0;
}

void shrinkCandidateTransform(
    Affine *transform,
    const QVector<CandidateFeatureAssignment>
        &featureAssignments,
    const CandidateAnchor *anchor) {
    transform->a *= kLegalShrinkFactor;
    transform->b *= kLegalShrinkFactor;
    transform->c *= kLegalShrinkFactor;
    transform->d *= kLegalShrinkFactor;
    if (anchor != nullptr) {
        projectCandidateAnchor(
            *anchor, transform);
        return;
    }
    if (featureAssignments.isEmpty()) {
        return;
    }

    const ShapeFeature &featureAnchor =
        featureAssignments.front().source;
    const QPointF anchorPosition =
        transformedFeaturePoint(
            featureAnchor, *transform);
    transform->e +=
        featureAssignments
            .front().target.position.x()
        - anchorPosition.x();
    transform->f +=
        featureAssignments
            .front().target.position.y()
        - anchorPosition.y();
}

bool betterOptimizedCandidate(
    const Candidate &left,
    const Candidate &right,
    const QVector<CandidateFeatureAssignment>
        &assignments,
    const FillOptions &options) {
    if (assignments.isEmpty()) {
        return betterCandidate(left, right);
    }
    if (!left.valid) {
        return false;
    }
    if (!right.valid) {
        return true;
    }
    const double leftScore =
        left.covered
        - options.spillWeight * left.spill
        + featurePotential(
            assignments,
            left.transform,
            options, true);
    const double rightScore =
        right.covered
        - options.spillWeight * right.spill
        + featurePotential(
            assignments,
            right.transform,
            options, true);
    if (std::abs(leftScore - rightScore)
        > kGeometryEpsilon) {
        return leftScore > rightScore;
    }

    return betterCandidate(left, right);
}

Candidate legalCandidate(const ShapeMesh &shape,
                         Affine transform,
                         const Polygons &residual,
                         const Polygons &target,
                         const Polygons &legalEnvelope,
                         const EvaluationBounds &subjectBounds,
                         const FillOptions &options,
                         CandidateProfile *profile,
                         const QVector<CandidateFeatureAssignment>
                             &featureAssignments,
                         const CandidateAnchor *anchor) {
    QElapsedTimer timer;
    timer.start();
    Candidate result;
    if (anchor != nullptr) {
        result.anchor = *anchor;
    }
    result.shapeId = shape.id;
    for (int step = 0; step <= kLegalShrinkSteps; ++step) {
        ++profile->legalizationEvaluations;
        const AreaGradient evaluation =
            areaGradient(
                shape, transform,
                residual, target,
                subjectBounds);
        const double outsideEnvelopeArea =
            polygonSetArea(
                differencePolygons(
                    Polygons{
                        transformedBoundary(
                            shape, transform),
                    },
                    legalEnvelope));
        if (finiteGradient(evaluation)
            && legalEnvelopeArea(
                outsideEnvelopeArea,
                options)
            && evaluation.covered >= options.epsGain) {
            result.transform = transform;
            result.covered = evaluation.covered;
            result.spill = std::max(0.0, evaluation.spill);
            result.featureReward =
                featurePotential(
                    featureAssignments,
                    transform,
                    options, false);
            result.valid = true;
            profile->legalizationNanoseconds += timer.nsecsElapsed();

            return result;
        }
        shrinkCandidateTransform(
            &transform, featureAssignments,
            anchor);
    }
    profile->legalizationNanoseconds += timer.nsecsElapsed();

    return result;
}

CandidateJobResult optimizeCandidate(
    const CandidateJob &job,
    const Polygons &residual,
    const Polygons &target,
    const Polygons &legalEnvelope,
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
    if (job.anchor.has_value()) {
        projectCandidateAnchor(
            *job.anchor, &values);
    }
    std::array<double, kGradientCount> firstMoment{};
    std::array<double, kGradientCount> secondMoment{};
    result.candidate = legalCandidate(
        shape, affineFromValues(values),
        residual, target, legalEnvelope,
        subjectBounds, options, &result.profile,
        job.featureAssignments,
        candidateAnchor(job));
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
            areaGradient(
                shape, transform,
                residual, target,
                subjectBounds);
        result.profile.adamEvaluationNanoseconds +=
            evaluationTimer.nsecsElapsed();
        ++result.profile.adamEvaluations;
        if (!finiteGradient(evaluation)) {
            break;
        }

        std::array<double, kGradientCount> scoreGradient{};
        const std::array<double, kGradientCount>
            featureGradient =
                featurePotentialGradient(
                    job.featureAssignments,
                    transform, options);
        double gradientNormSquared = 0.0;
        for (int parameter = 0; parameter < kGradientCount; ++parameter) {
            scoreGradient[parameter] = evaluation.coveredGradient[parameter]
                - options.spillWeight * evaluation.spillGradient[parameter]
                + featureGradient[parameter];
            gradientNormSquared += scoreGradient[parameter]
                * scoreGradient[parameter];
        }
        if (job.anchor.has_value()) {
            constrainCandidateGradient(
                *job.anchor,
                &scoreGradient);
            gradientNormSquared = 0.0;
            for (const double gradient : scoreGradient) {
                gradientNormSquared +=
                    gradient * gradient;
            }
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
        if (job.anchor.has_value()) {
            projectCandidateAnchor(
                *job.anchor, &values);
        }
        const Candidate candidate = legalCandidate(
            shape, affineFromValues(values),
            residual, target, legalEnvelope,
            subjectBounds, options, &result.profile,
            job.featureAssignments,
            candidateAnchor(job));
        if (betterOptimizedCandidate(
                candidate, result.candidate,
                job.featureAssignments,
                options)) {
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
    const QVector<const CandidateJob *> &jobs,
    const QVector<Affine> &initialTransforms,
    const FillOptions &options,
    GpuAreaEvaluator *evaluator,
    FillProfile *profile,
    QVector<Candidate> *candidates,
    const std::function<bool()> &cancelled,
    bool *wasCancelled) {
    QVector<int> pending;
    pending.reserve(jobs.size());
    for (int index = 0; index < jobs.size(); ++index) {
        pending.push_back(index);
    }
    QVector<Affine> transforms = initialTransforms;
    candidates->fill({}, jobs.size());
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
                    jobs[pendingIndex]->shape,
                    transform,
                });
                shrinkCandidateTransform(
                    &transform,
                    jobs[pendingIndex]
                        ->featureAssignments,
                    candidateAnchor(
                        *jobs[pendingIndex]));
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
                const double maximumOutside =
                    permittedOutsideEnvelopeArea(
                        options);
                if (!finiteGradient(evaluation)
                    || evaluation.spill
                        > maximumOutside
                            + spillSlack
                    || evaluation.covered < minimumCovered) {
                    continue;
                }
                Candidate candidate;
                candidate.transform = requests[requestIndex].transform;
                candidate.shapeId =
                    jobs[pendingIndex]->shape->id;
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
                shrinkCandidateTransform(
                    &transforms[pendingIndex],
                    jobs[pendingIndex]
                        ->featureAssignments,
                    candidateAnchor(
                        *jobs[pendingIndex]));
            }
            nextPending.push_back(pendingIndex);
        }
        pending = std::move(nextPending);
    }

    return true;
}

bool scoreCandidatesGpu(
    const QVector<const CandidateJob *> &jobs,
    GpuAreaEvaluator *evaluator,
    FillProfile *profile,
    QVector<Candidate> *candidates) {
    QVector<int> candidateIndices;
    QVector<GpuEvaluationRequest> requests;
    candidateIndices.reserve(candidates->size());
    requests.reserve(candidates->size());
    for (int index = 0;
         index < candidates->size(); ++index) {
        if (!candidates->at(index).valid) {
            continue;
        }
        candidateIndices.push_back(index);
        requests.push_back({
            jobs[index]->shape,
            candidates->at(index).transform,
        });
    }
    if (requests.isEmpty()) {
        return true;
    }

    QVector<AreaGradient> evaluations;
    if (!evaluateGpuBatch(
            evaluator, requests,
            &evaluations, profile, false)) {
        return false;
    }
    for (int index = 0;
         index < evaluations.size(); ++index) {
        Candidate &candidate =
            (*candidates)[candidateIndices[index]];
        const AreaGradient &evaluation =
            evaluations[index];
        if (!finiteGradient(evaluation)
            || evaluation.covered
                < kGeometryEpsilon) {
            candidate = {};
            continue;
        }
        candidate.covered =
            evaluation.covered;
        candidate.spill =
            std::max(
                0.0,
                evaluation.spill);
    }

    return true;
}

void exactCandidatesCpuBatch(
    const QVector<const CandidateJob *> &jobs,
    const QVector<Candidate> &gpuCandidates,
    const Polygons &residual,
    const Polygons &target,
    const Polygons &legalEnvelope,
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
                *jobs[index]->shape,
                gpuCandidates[index].transform,
                residual, target, legalEnvelope,
                subjectBounds, options,
                &profiles[static_cast<size_t>(index)],
                jobs[index]->featureAssignments,
                candidateAnchor(*jobs[index]));
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
    bool *wasCancelled) {
    QVector<GpuCandidateState> states;
    states.reserve(static_cast<qsizetype>(jobs.size()));
    for (const CandidateJob &job : jobs) {
        GpuCandidateState state;
        state.job = &job;
        state.values = affineValues(initialTransform(job, seed));
        if (job.anchor.has_value()) {
            projectCandidateAnchor(
                *job.anchor,
                &state.values);
        }
        states.push_back(state);
    }

    QVector<const CandidateJob *> initialJobs;
    QVector<Affine> initialTransforms;
    initialJobs.reserve(states.size());
    initialTransforms.reserve(states.size());
    for (int index = 0; index < states.size(); ++index) {
        initialJobs.push_back(states[index].job);
        initialTransforms.push_back(affineFromValues(states[index].values));
    }
    if (!evaluator->setSubjects(
            residual, legalEnvelope)) {
        return false;
    }
    QVector<Candidate> initialGpuCandidates;
    if (!legalCandidatesGpu(
            initialJobs, initialTransforms, options, evaluator, profile,
            &initialGpuCandidates, cancelled, wasCancelled)) {
        return false;
    }
    if (!evaluator->setSubjects(
            residual, target)
        || !scoreCandidatesGpu(
            initialJobs, evaluator,
            profile, &initialGpuCandidates)) {
        return false;
    }
    QVector<Candidate> initialCandidates = initialGpuCandidates;
    if (!evaluator->usesDoublePrecision()) {
        exactCandidatesCpuBatch(
            initialJobs, initialGpuCandidates,
            residual, target, legalEnvelope,
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
                requests, residual, target, subjectBounds,
                candidatePool, &evaluations, profile);
        }
        QVector<int> legalIndices;
        QVector<const CandidateJob *> legalJobs;
        QVector<Affine> legalTransforms;
        const bool legalizationStep =
            iteration % kGpuLegalizationInterval
                    == 0
            || iteration
                == options.adamIterations;
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
            const Affine transform =
                affineFromValues(state.values);
            const std::array<double, kGradientCount>
                featureGradient =
                    featurePotentialGradient(
                        state.job
                            ->featureAssignments,
                        transform, options);
            double gradientNormSquared = 0.0;
            for (int parameter = 0; parameter < kGradientCount;
                 ++parameter) {
                scoreGradient[parameter] =
                    evaluation.coveredGradient[parameter]
                    - options.spillWeight
                        * evaluation.spillGradient[parameter]
                    + featureGradient[parameter];
                gradientNormSquared += scoreGradient[parameter]
                    * scoreGradient[parameter];
            }
            if (state.job->anchor.has_value()) {
                constrainCandidateGradient(
                    *state.job->anchor,
                    &scoreGradient);
                gradientNormSquared = 0.0;
                for (const double gradient :
                     scoreGradient) {
                    gradientNormSquared +=
                        gradient * gradient;
                }
            }
            const double gradientNorm = std::sqrt(gradientNormSquared);
            if (gradientNorm <= kGradientStopNorm) {
                state.active = false;
                legalIndices.push_back(
                    activeIndices[requestIndex]);
                legalJobs.push_back(
                    state.job);
                legalTransforms.push_back(
                    transform);
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
            if (state.job->anchor.has_value()) {
                projectCandidateAnchor(
                    *state.job->anchor,
                    &state.values);
            }
            if (legalizationStep) {
                legalIndices.push_back(
                    activeIndices[requestIndex]);
                legalJobs.push_back(
                    state.job);
                legalTransforms.push_back(
                    affineFromValues(
                        state.values));
            }
        }
        if (legalIndices.isEmpty()) {
            continue;
        }

        QVector<Candidate> gpuLegalCandidates;
        if (!evaluator->setSubjects(
                residual, legalEnvelope)
            || !legalCandidatesGpu(
                legalJobs, legalTransforms, options, evaluator,
                profile, &gpuLegalCandidates, cancelled, wasCancelled)
            || !evaluator->setSubjects(
                residual, target)
            || !scoreCandidatesGpu(
                legalJobs, evaluator,
                profile,
                &gpuLegalCandidates)) {
            return false;
        }
        QVector<int> competitiveIndices;
        QVector<const CandidateJob *> competitiveJobs;
        QVector<Candidate> competitiveGpuCandidates;
        for (int index = 0; index < legalIndices.size(); ++index) {
            GpuCandidateState &state = states[legalIndices[index]];
            if (betterOptimizedCandidate(
                    gpuLegalCandidates[index],
                    state.bestGpu,
                    state.job->featureAssignments,
                    options)) {
                state.bestGpu = gpuLegalCandidates[index];
                if (evaluator->usesDoublePrecision()) {
                    if (betterOptimizedCandidate(
                            gpuLegalCandidates[index],
                            state.best,
                            state.job
                                ->featureAssignments,
                            options)) {
                        state.best = gpuLegalCandidates[index];
                    }
                    continue;
                }
                competitiveIndices.push_back(legalIndices[index]);
                competitiveJobs.push_back(state.job);
                competitiveGpuCandidates.push_back(
                    gpuLegalCandidates[index]);
            }
        }
        QVector<Candidate> competitiveCandidates;
        exactCandidatesCpuBatch(
            competitiveJobs,
            competitiveGpuCandidates,
            residual, target, legalEnvelope,
            subjectBounds, options,
            candidatePool, profile, &competitiveCandidates);
        for (int index = 0; index < competitiveIndices.size(); ++index) {
            GpuCandidateState &state = states[competitiveIndices[index]];
            if (betterOptimizedCandidate(
                    competitiveCandidates[index],
                    state.best,
                    state.job->featureAssignments,
                    options)) {
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

CandidateSelection selectCandidate(
    const QVector<Candidate> &candidates,
    const QVector<ShapeMesh> &catalog,
    const Polygons &target,
    const Polygons &legalEnvelope,
    const Polygons &currentCoverage,
    const Polygons &residual,
    const QVector<ContourFeature> &features,
    const FeatureMatchSummary &currentFeatureMatches,
    const FillOptions &options) {
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

    struct ScoredCandidate {
        Candidate candidate;
        Polygons coverage;
        Polygons residual;
        QVector<int> representedFeatureIds;
        QVector<int> newlyRepresentedFeatureIds;
        double exactGain = 0.0;
        double representedFeatureWeight = 0.0;
        double newFeatureWeight = 0.0;
        double exposedContourArc = 0.0;
        double boundaryDistance = 0.0;
        double tversky = 0.0;
        double complexity = 0.0;
    };
    QVector<ScoredCandidate> scored;
    const double previousArea =
        polygonSetArea(residual);
    for (const Candidate &candidate : candidates) {
        if (!candidate.valid) {
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
        if (!std::isfinite(exactGain)
            || exactGain < options.epsGain) {
            ++result
                  .insufficientGainRejections;
            continue;
        }
        Polygons nextCoverage =
            currentCoverage;
        nextCoverage.push_back(
            footprint.front());
        nextCoverage =
            unionPolygons(nextCoverage);
        scored.push_back({
            candidate,
            std::move(nextCoverage),
            std::move(nextResidual),
            {},
            {},
            exactGain,
        });
    }
    if (scored.isEmpty()) {
        return result;
    }

    double maximumPrimaryGain = 0.0;
    for (const ScoredCandidate &candidate :
         scored) {
        if (candidate.candidate.origin
            != CandidateOrigin::LocalComponent) {
            maximumPrimaryGain =
                std::max(
                    maximumPrimaryGain,
                    candidate.exactGain);
        }
    }
    scored.erase(
        std::remove_if(
            scored.begin(), scored.end(),
            [&](const ScoredCandidate &candidate) {
                return candidate.candidate.origin
                           == CandidateOrigin::LocalComponent
                    && candidate.exactGain
                        < maximumPrimaryGain
                            * kLocalSelectionGainAdvantage;
            }),
        scored.end());
    if (scored.isEmpty()) {
        return result;
    }

    double maximumGain = 0.0;
    for (const ScoredCandidate &candidate :
         scored) {
        maximumGain = std::max(
            maximumGain,
            candidate.exactGain);
    }
    const double minimumCompetitiveGain =
        maximumGain
        * options.areaWindowRatio;
    scored.erase(
        std::remove_if(
            scored.begin(), scored.end(),
            [&](const ScoredCandidate &candidate) {
                return candidate.exactGain
                    < minimumCompetitiveGain
                        - kGeometryEpsilon;
            }),
        scored.end());
    if (scored.isEmpty()) {
        return result;
    }

    const QSet<int> representedBefore(
        currentFeatureMatches
            .representedIds.cbegin(),
        currentFeatureMatches
            .representedIds.cend());
    double primaryComplexity =
        std::numeric_limits<double>::max();
    for (ScoredCandidate &candidate : scored) {
        const FeatureMatchSummary featureMatches =
            matchContourFeatures(
                candidate.coverage,
                features);
        candidate.representedFeatureIds =
            featureMatches.representedIds;
        candidate.representedFeatureWeight =
            featureMatches.representedWeight;
        bool validFeatureContribution =
            featureMatches.representedWeight
                + kGeometryEpsilon
            >= currentFeatureMatches
                   .representedWeight;
        for (const int featureId :
             featureMatches.representedIds) {
            if (representedBefore.contains(
                    featureId)) {
                continue;
            }
            const auto found =
                std::find_if(
                    features.cbegin(),
                    features.cend(),
                    [featureId](
                        const ContourFeature &feature) {
                        return feature.id
                            == featureId;
                    });
            if (found == features.cend()) {
                continue;
            }
            const double exposedArc =
                exposedFeatureArc(
                    candidate.coverage,
                    *found);
            candidate.newlyRepresentedFeatureIds
                .push_back(featureId);
            candidate.newFeatureWeight +=
                found->weight;
            candidate.exposedContourArc +=
                exposedArc;
        }
        if (!validFeatureContribution) {
            candidate.exactGain =
                -1.0;
            ++result.featureRejections;
            if (candidate.candidate.origin
                == CandidateOrigin::Feature) {
                ++result
                      .featureCandidateRejections;
            }
            continue;
        }
        const CoverErrorMetrics metrics =
            evaluateCoverMetrics(
                target, legalEnvelope,
                candidate.coverage,
                features, options);
        if (!legalEnvelopeArea(
                metrics.outsideEnvelopeArea,
                options)) {
            candidate.exactGain = -1.0;
            ++result.envelopeRejections;
            if (candidate.candidate.origin
                == CandidateOrigin::Feature) {
                ++result
                      .featureCandidateRejections;
            }
            continue;
        }
        if (!legalOutwardDistance(
                metrics.maximumOutwardDistance,
                options)) {
            candidate.exactGain = -1.0;
            ++result
                  .outwardDistanceRejections;
            if (candidate.candidate.origin
                == CandidateOrigin::Feature) {
                ++result
                      .featureCandidateRejections;
            }
            continue;
        }
        candidate.boundaryDistance =
            metrics.meanBoundaryDistance;
        candidate.tversky =
            metrics.tversky;
        candidate.complexity =
            residualComplexity(candidate.residual).score;
        if (candidate.candidate.origin
            != CandidateOrigin::LocalComponent) {
            primaryComplexity = std::min(
                primaryComplexity, candidate.complexity);
        }
    }
    scored.erase(
        std::remove_if(
            scored.begin(), scored.end(),
            [](const ScoredCandidate &candidate) {
                return candidate.exactGain < 0.0;
            }),
        scored.end());
    if (scored.isEmpty()) {
        return result;
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
            || scored[index].exactGain
                > scored[areaWinner].exactGain
                    + kGeometryEpsilon
            || (std::abs(
                    scored[index].exactGain
                    - scored[areaWinner].exactGain)
                    <= kGeometryEpsilon
                && betterCandidate(
                    scored[index].candidate,
                    scored[areaWinner].candidate))) {
            areaWinner = index;
        }
    }
    if (areaWinner < 0) {
        return result;
    }
    int winner = options.useContourLeeway
        ? areaWinner : -1;
    for (int index = 0; index < scored.size(); ++index) {
        if (options.useContourLeeway) {
            break;
        }
        if (scored[index].candidate.origin
                == CandidateOrigin::LocalComponent
            && scored[index].complexity
                > primaryComplexity
                    * kLocalSelectionComplexityRatio) {
            continue;
        }
        const auto betterWeighted =
            [&](int left, int right) {
                if (std::abs(
                        scored[left].newFeatureWeight
                        - scored[right]
                              .newFeatureWeight)
                    > kGeometryEpsilon) {
                    return scored[left]
                               .newFeatureWeight
                        > scored[right]
                              .newFeatureWeight;
                }
                if (std::abs(
                        scored[left].boundaryDistance
                        - scored[right]
                              .boundaryDistance)
                    > kGeometryEpsilon) {
                    return scored[left]
                               .boundaryDistance
                        < scored[right]
                              .boundaryDistance;
                }
                if (std::abs(
                        scored[left].complexity
                        - scored[right].complexity)
                    > kGeometryEpsilon) {
                    return scored[left].complexity
                        < scored[right].complexity;
                }
                if (std::abs(
                        scored[left].exactGain
                        - scored[right].exactGain)
                    > kGeometryEpsilon) {
                    return scored[left].exactGain
                        > scored[right].exactGain;
                }
                if (std::abs(
                        scored[left].tversky
                        - scored[right].tversky)
                    > kGeometryEpsilon) {
                    return scored[left].tversky
                        > scored[right].tversky;
                }

                return betterCandidate(
                    scored[left].candidate,
                    scored[right].candidate);
            };
        if (winner < 0
            || betterWeighted(index, winner)) {
            winner = index;
        }
    }
    if (winner < 0) {
        return result;
    }
    result.candidate = scored[winner].candidate;
    result.candidate.covered = scored[winner].exactGain;
    result.coverage =
        std::move(scored[winner].coverage);
    result.residual = std::move(scored[winner].residual);
    result.representedFeatureIds =
        std::move(
            scored[winner]
                .representedFeatureIds);
    result.newlyRepresentedFeatureIds =
        std::move(
            scored[winner]
                .newlyRepresentedFeatureIds);
    result.exactGain = scored[winner].exactGain;
    result.exposedContourArc =
        scored[winner].exposedContourArc;
    result.complexityPreferred = winner != areaWinner;
    result.valid = true;

    return result;
}

} // namespace gui::cover
