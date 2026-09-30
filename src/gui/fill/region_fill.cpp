#include "region_fill.h"
#include "cubic_contour.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <vector>

namespace gui {

void sortRegionFillLayersByDrawOrder(QVector<RegionFillLayer> *layers) {
    if (layers == nullptr) {
        return;
    }
    const auto variantRank = [](RegionFillVariant variant) {
        switch (variant) {
        case RegionFillVariant::Safe:
            return 0;
        case RegionFillVariant::Dangerous:
            return 1;
        }
        return 2;
    };
    std::stable_sort(layers->begin(), layers->end(),
                     [variantRank](const RegionFillLayer &left,
                                   const RegionFillLayer &right) {
                         if (left.variant != right.variant) {
                             return variantRank(left.variant) < variantRank(right.variant);
                         }
                         return left.drawOrder < right.drawOrder;
                     });
}

namespace {

constexpr int kBoundarySamplesPerCurve = 32;
constexpr int kCircleShapeId = 102;

struct Op {
    enum Kind { Line, Cubic } kind = Line;
    QPointF control1;
    QPointF control2;
    QPointF end;
};

struct Subpath {
    QPointF start;
    QVector<Op> ops;
    bool closed = false;
};

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon[i];
        const QPointF &b = polygon[(i + 1) % polygon.size()];
        result += a.x() * b.y() - a.y() * b.x();
    }
    return result * 0.5;
}

QPointF cubicPoint(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p3, double t) {
    const double u = 1.0 - t;
    return p0 * (u * u * u)
        + c1 * (3.0 * u * u * t)
        + c2 * (3.0 * u * t * t)
        + p3 * (t * t * t);
}

QVector<Subpath> toSubpaths(const QPainterPath &path, double closureTolerance = 1e-6) {
    QVector<Subpath> subpaths;
    Subpath current;
    bool have = false;
    const auto finishCurrent = [&]() {
        if (!have || current.ops.isEmpty()) {
            return;
        }
        current.closed = QLineF(current.ops.back().end, current.start).length()
            <= closureTolerance;
        subpaths.push_back(current);
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        if (element.isMoveTo()) {
            finishCurrent();
            current = Subpath{};
            current.start = QPointF(element.x, element.y);
            have = true;
        } else if (element.isLineTo()) {
            if (!have) {
                continue;
            }
            Op op;
            op.kind = Op::Line;
            op.end = QPointF(element.x, element.y);
            current.ops.push_back(op);
        } else if (element.type == QPainterPath::CurveToElement) {
            if (!have || i + 2 >= path.elementCount()) {
                current.ops.clear();
                continue;
            }
            Op op;
            op.kind = Op::Cubic;
            op.control1 = QPointF(element.x, element.y);
            op.control2 = QPointF(path.elementAt(i + 1).x, path.elementAt(i + 1).y);
            op.end = QPointF(path.elementAt(i + 2).x, path.elementAt(i + 2).y);
            current.ops.push_back(op);
            i += 2;
        }
    }
    finishCurrent();
    return subpaths;
}

QPolygonF flattenSubpath(const Subpath &subpath, int curveSamples = 8) {
    QPolygonF polygon;
    polygon.push_back(subpath.start);
    QPointF previous = subpath.start;
    for (const Op &op : subpath.ops) {
        if (op.kind == Op::Line) {
            polygon.push_back(op.end);
        } else {
            for (int step = 1; step <= curveSamples; ++step) {
                polygon.push_back(cubicPoint(previous,
                                             op.control1,
                                             op.control2,
                                             op.end,
                                             static_cast<double>(step) / curveSamples));
            }
        }
        previous = op.end;
    }
    return polygon;
}

double perpendicularDistance(const QPointF &point, const QPointF &a, const QPointF &b) {
    const QPointF ab = b - a;
    const double lengthSquared = ab.x() * ab.x() + ab.y() * ab.y();
    if (lengthSquared <= 1e-12) {
        return QLineF(point, a).length();
    }
    const double t = std::clamp(
        ((point.x() - a.x()) * ab.x() + (point.y() - a.y()) * ab.y()) / lengthSquared,
        0.0,
        1.0);
    const QPointF projection = a + t * ab;
    return QLineF(point, projection).length();
}

QPolygonF flattenPenContour(const PenContour &contour, int curveSamples) {
    QPolygonF result;
    for (const auto &segment : contour.segments) {
        const int n = segment.curved ? curveSamples : 1;
        for (int i = 0; i < n; ++i)
            result.push_back(segment.point(double(i) / n));
    }
    return result;
}

void rdpRecurse(const QPolygonF &points, int first, int last, double epsilon, QPolygonF &out) {
    double maxDistance = 0.0;
    int index = first;
    for (int i = first + 1; i < last; ++i) {
        const double distance = perpendicularDistance(points[i], points[first], points[last]);
        if (distance > maxDistance) {
            maxDistance = distance;
            index = i;
        }
    }
    if (maxDistance > epsilon && index > first) {
        rdpRecurse(points, first, index, epsilon, out);
        rdpRecurse(points, index, last, epsilon, out);
    } else {
        out.push_back(points[last]);
    }
}

struct CyclicRdpSlice {
    int start = 0;
    int end = 0;
};

QPolygonF approximateClosedPolygon(const QPolygonF &polygon, double epsilon) {
    constexpr int kFarthestPointPasses = 3;
    if (!std::isfinite(epsilon) || epsilon <= 0.0 || polygon.size() <= 4) {
        return polygon;
    }
    QPolygonF source;
    source.reserve(polygon.size());
    for (const QPointF &point : polygon) {
        source.push_back(QPointF(static_cast<float>(point.x()),
                                 static_cast<float>(point.y())));
    }
    QVector<CyclicRdpSlice> stack;
    QPolygonF result;
    const int sourceCount = source.size();
    const double epsilonSquared = epsilon * epsilon;
    int position = 0;
    int rightStart = 0;
    QPointF startPoint;
    bool withinEpsilon = false;

    for (int pass = 0; pass < kFarthestPointPasses; ++pass) {
        position = (position + rightStart) % sourceCount;
        startPoint = source[position];
        position = (position + 1) % sourceCount;
        double maximumDistanceSquared = 0.0;
        rightStart = 0;
        for (int offset = 1; offset < sourceCount; ++offset) {
            const QPointF point = source[position];
            position = (position + 1) % sourceCount;
            const QPointF delta = point - startPoint;
            const double distanceSquared = QPointF::dotProduct(delta, delta);
            if (distanceSquared > maximumDistanceSquared) {
                maximumDistanceSquared = distanceSquared;
                rightStart = offset;
            }
        }
        withinEpsilon = maximumDistanceSquared <= epsilonSquared;
    }
    if (!withinEpsilon) {
        const int seam = position % sourceCount;
        const int opposite = (rightStart + seam) % sourceCount;
        stack.push_back(CyclicRdpSlice{opposite, seam});
        stack.push_back(CyclicRdpSlice{seam, opposite});
    } else {
        result.push_back(startPoint);
    }

    while (!stack.isEmpty()) {
        const CyclicRdpSlice slice = stack.takeLast();
        const QPointF endPoint = source[slice.end];
        position = slice.start;
        startPoint = source[position];
        position = (position + 1) % sourceCount;
        int split = slice.start;
        if (position != slice.end) {
            const QPointF segment = endPoint - startPoint;
            const double segmentLengthSquared = QPointF::dotProduct(segment, segment);
            double maximumScaledDistance = 0.0;
            if (segmentLengthSquared > 0.0) {
                while (position != slice.end) {
                    const QPointF point = source[position];
                    position = (position + 1) % sourceCount;
                    const QPointF fromStart = point - startPoint;
                    const double projection = QPointF::dotProduct(fromStart, segment);
                    double scaledDistance = 0.0;
                    if (projection < 0.0) {
                        scaledDistance = QPointF::dotProduct(fromStart, fromStart)
                            * segmentLengthSquared;
                    } else if (projection > segmentLengthSquared) {
                        const QPointF fromEnd = point - endPoint;
                        scaledDistance = QPointF::dotProduct(fromEnd, fromEnd)
                            * segmentLengthSquared;
                    } else {
                        const double cross = fromStart.y() * segment.x()
                            - fromStart.x() * segment.y();
                        scaledDistance = cross * cross;
                    }
                    if (scaledDistance > maximumScaledDistance) {
                        maximumScaledDistance = scaledDistance;
                        split = (position + sourceCount - 1) % sourceCount;
                    }
                }
            }
            withinEpsilon = segmentLengthSquared <= 0.0
                || maximumScaledDistance <= epsilonSquared * segmentLengthSquared;
        } else {
            withinEpsilon = true;
        }
        if (withinEpsilon) {
            result.push_back(startPoint);
        } else {
            stack.push_back(CyclicRdpSlice{split, slice.end});
            stack.push_back(CyclicRdpSlice{slice.start, split});
        }
    }

    const int cleanupCount = result.size();
    int resultCount = cleanupCount;
    if (cleanupCount <= 2) {
        return polygon;
    }
    position = cleanupCount - 1;
    const auto readPoint = [&]() {
        const QPointF point = result[position];
        position = (position + 1) % cleanupCount;
        return point;
    };
    startPoint = readPoint();
    int writePosition = position;
    QPointF point = readPoint();
    for (int pass = 0; pass < cleanupCount && resultCount > 2; ++pass) {
        const QPointF endPoint = readPoint();
        const double dx = endPoint.x() - startPoint.x();
        const double dy = endPoint.y() - startPoint.y();
        const double distance = std::abs((point.x() - startPoint.x()) * dy
                                         - (point.y() - startPoint.y()) * dx);
        const double successiveInnerProduct =
            (point.x() - startPoint.x()) * (endPoint.x() - point.x())
            + (point.y() - startPoint.y()) * (endPoint.y() - point.y());
        if (distance * distance
                <= 0.5 * epsilonSquared * (dx * dx + dy * dy)
            && dx != 0.0 && dy != 0.0 && successiveInnerProduct >= 0.0) {
            --resultCount;
            startPoint = endPoint;
            result[writePosition] = startPoint;
            writePosition = (writePosition + 1) % cleanupCount;
            point = readPoint();
            ++pass;
            continue;
        }
        result[writePosition] = point;
        startPoint = point;
        writePosition = (writePosition + 1) % cleanupCount;
        point = endPoint;
    }
    result.resize(resultCount);

    return result.size() >= 3 ? result : polygon;
}

void rdpCorridorRecurse(const QPolygonF &points, int first, int last, double epsilon,
                        const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace,
                        QPolygonF &out) {
    double maxDistance = 0.0;
    int index = first;
    for (int i = first + 1; i < last; ++i) {
        const double distance = perpendicularDistance(points[i], points[first], points[last]);
        if (distance > maxDistance) {
            maxDistance = distance;
            index = i;
        }
    }
    const bool accept = maxDistance <= epsilon
        || (chordInFreeSpace && chordInFreeSpace(points[first], points[last]));
    if (!accept && index > first) {
        rdpCorridorRecurse(points, first, index, epsilon, chordInFreeSpace, out);
        rdpCorridorRecurse(points, index, last, epsilon, chordInFreeSpace, out);
    } else {
        out.push_back(points[last]);
    }
}

QPolygonF largestFlattenedContour(const QPainterPath &outline, int curveSamples = 8) {
    const QVector<Subpath> subpaths = toSubpaths(outline);
    if (subpaths.isEmpty()) {
        return {};
    }
    int outer = 0;
    double outerArea = -1.0;
    QPolygonF outerPolygon;
    for (int i = 0; i < subpaths.size(); ++i) {
        const QPolygonF polygon = flattenSubpath(subpaths[i], curveSamples);
        const double area = std::abs(signedArea(polygon));
        if (area > outerArea) {
            outerArea = area;
            outer = i;
            outerPolygon = polygon;
        }
    }
    Q_UNUSED(outer);
    while (outerPolygon.size() > 1
           && QLineF(outerPolygon.back(), outerPolygon.front()).length() <= 1e-6) {
        outerPolygon.removeLast();
    }
    return outerPolygon;
}

} // namespace

RegionPenConversionResult regionOutlineToPenPoints(
    const QPainterPath &outline,
    const RegionPenConversionOptions &options) {
    RegionPenConversionResult result;
    if (!std::isfinite(options.mergeTolerance) || options.mergeTolerance < 0 ||
        !std::isfinite(options.maximumDssim) || options.maximumDssim < 0 ||
        options.maximumDssim > 1 || !std::isfinite(options.closureTolerance) ||
        options.closureTolerance <= 0 || options.dssimSupersample < 1 ||
        options.dssimSupersample > 8 || options.maxOptimizedPointCount < 0 ||
        options.adaptiveSearchSteps < 0 || options.adaptiveSearchSteps > 16) {
        result.error = QStringLiteral("Region Pen conversion options are invalid");
        return result;
    }
    const auto loops = cubicPathLoops(outline, &result.error);
    if (loops.isEmpty())
        return result;
    result.points = loops.front().points;
    result.originalPointCount = result.points.size();
    // Authored cubic handles are retained exactly. Raster simplification happens against the mask.
    result.optimizationSkipped = true;
    return result;
}

int regionOutlinePenPointCount(const QPainterPath &outline) {
    int count = 0;
    for (const auto &loop : cubicPathLoops(outline))
        count += loop.points.size();
    return count;
}

PenFillResult fillRegionOutline(const QPainterPath &outline,
                                const QVector<PenPrimitive> &primitives,
                                double boundaryTolerance,
                                const std::function<bool()> &cancelled,
                                QPolygonF *optimizedContour,
                                RegionFillContourStats *contourStats,
                                QVector<PenPoint> *optimizedPenPoints,
                                const QSize &comparisonImageSize) {
    PenFillResult result;
    if (optimizedContour != nullptr) {
        optimizedContour->clear();
    }
    if (contourStats != nullptr) {
        *contourStats = RegionFillContourStats{};
    }
    if (optimizedPenPoints != nullptr) {
        optimizedPenPoints->clear();
    }
    RegionPenConversionOptions conversionOptions;
    conversionOptions.comparisonImageSize = comparisonImageSize;
    RegionPenConversionResult conversion =
        regionOutlineToPenPoints(outline, conversionOptions);
    if (!conversion.valid()) {
        result.error = conversion.error.isEmpty()
            ? QStringLiteral("Region outline has no fillable contour")
            : conversion.error;
        return result;
    }

    PenFillRequest request;
    request.points = conversion.points;
    request.primitives = primitives;
    request.boundaryTolerance = boundaryTolerance;
    result = fillPenPath(request, cancelled);
    if (contourStats != nullptr) {
        contourStats->originalPointCount = conversion.originalPointCount;
        contourStats->optimizedPointCount = conversion.points.size();
        contourStats->removedHardPoints = conversion.removedHardPoints;
        contourStats->removedSoftPoints = conversion.removedSoftPoints;
        contourStats->optimizationSkipped = conversion.optimizationSkipped;
        contourStats->dssim = conversion.dssim;
    }
    if (optimizedPenPoints != nullptr) {
        *optimizedPenPoints = conversion.points;
    }
    if (optimizedContour != nullptr || contourStats != nullptr) {
        const PenContour contour = buildPenContour(conversion.points);
        if (contour.valid()) {
            const QPolygonF flattened =
                flattenPenContour(contour, kBoundarySamplesPerCurve);
            if (optimizedContour != nullptr) {
                *optimizedContour = flattened;
            }
            if (contourStats != nullptr) {
                contourStats->flattenedPointCount = flattened.size();
            }
        }
    }
    return result;
}

QPolygonF simplifyClosedPolygon(const QPolygonF &polygon, double epsilon) {
    if (epsilon <= 0.0 || polygon.size() <= 4) {
        return polygon;
    }
    int anchorA = 0;
    int anchorB = 0;
    double maxSpan = -1.0;
    for (int i = 1; i < polygon.size(); ++i) {
        const double span = QLineF(polygon[0], polygon[i]).length();
        if (span > maxSpan) {
            maxSpan = span;
            anchorB = i;
        }
    }
    Q_UNUSED(anchorA);
    QPolygonF chain = polygon;
    QPolygonF result;
    result.push_back(chain[0]);
    rdpRecurse(chain, 0, anchorB, epsilon, result);
    QPolygonF secondHalf;
    for (int i = anchorB; i < chain.size(); ++i) {
        secondHalf.push_back(chain[i]);
    }
    secondHalf.push_back(chain[0]);
    QPolygonF secondSimplified;
    secondSimplified.push_back(secondHalf.front());
    rdpRecurse(secondHalf, 0, secondHalf.size() - 1, epsilon, secondSimplified);
    for (int i = 1; i + 1 < secondSimplified.size(); ++i) {
        result.push_back(secondSimplified[i]);
    }
    if (result.size() < 3) {
        return polygon;
    }
    return result;
}

QPolygonF simplifyClosedPolygonCyclic(const QPolygonF &polygon, double epsilon) {
    return approximateClosedPolygon(polygon, epsilon);
}

QVector<PenPoint> fitClosedPolygonCubic(const QPolygonF &polygon, double epsilon) {
    CubicFitOptions options;
    options.tolerance = epsilon;
    return fitCubicContour(polygon, options);
}

QPolygonF simplifyClosedPolygonCorridor(
    const QPolygonF &polygon, double epsilon,
    const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace) {
    if (polygon.size() <= 4) {
        return polygon;
    }
    int anchorB = 0;
    double maxSpan = -1.0;
    for (int i = 1; i < polygon.size(); ++i) {
        const double span = QLineF(polygon[0], polygon[i]).length();
        if (span > maxSpan) {
            maxSpan = span;
            anchorB = i;
        }
    }
    QPolygonF result;
    result.push_back(polygon[0]);
    rdpCorridorRecurse(polygon, 0, anchorB, epsilon, chordInFreeSpace, result);
    QPolygonF secondHalf;
    for (int i = anchorB; i < polygon.size(); ++i) {
        secondHalf.push_back(polygon[i]);
    }
    secondHalf.push_back(polygon[0]);
    QPolygonF secondSimplified;
    secondSimplified.push_back(secondHalf.front());
    rdpCorridorRecurse(secondHalf, 0, secondHalf.size() - 1, epsilon, chordInFreeSpace, secondSimplified);
    for (int i = 1; i + 1 < secondSimplified.size(); ++i) {
        result.push_back(secondSimplified[i]);
    }
    if (result.size() < 3) {
        return polygon;
    }
    return result;
}

QPolygonF regionOuterContour(const QPainterPath &outline) {
    return largestFlattenedContour(outline);
}

QPolygonF regionOuterContour(const QPainterPath &outline, int curveSamples) {
    if (curveSamples < 1) {
        return {};
    }

    return largestFlattenedContour(outline, curveSamples);
}

QVector<QPolygonF> regionContours(const QPainterPath &outline, int curveSamples) {
    QVector<QPolygonF> result;
    if (curveSamples < 1) {
        return result;
    }
    const QVector<Subpath> subpaths = toSubpaths(outline);
    result.reserve(subpaths.size());
    for (const Subpath &subpath : subpaths) {
        QPolygonF polygon = flattenSubpath(subpath, curveSamples);
        while (polygon.size() > 1
               && QLineF(polygon.back(), polygon.front()).length() <= 1e-6) {
            polygon.removeLast();
        }
        if (polygon.size() >= 3) {
            result.push_back(std::move(polygon));
        }
    }

    return result;
}

RegionPenLoopConversionResult regionOutlineToPenLoops(
    const QPainterPath &outline,
    const RegionPenLoopConversionOptions &options) {
    RegionPenLoopConversionResult result;
    if (outline.isEmpty() || !std::isfinite(options.curveFitTolerance) ||
        options.curveFitTolerance <= 0 || !std::isfinite(options.simplifyEpsilon) ||
        options.simplifyEpsilon <= 0 || options.curveSamples < 1) {
        result.error = QStringLiteral("Region Pen conversion options are invalid");
        return result;
    }
    if (options.requiredPixelMask) {
        CubicFitOptions fit;
        fit.tolerance = options.curveFitTolerance;
        result.loops = fitMaskContours(*options.requiredPixelMask, options.requiredPixelMaskSize,
                                       options.requiredPixelBounds, fit, &result.error);
    } else {
        result.loops = cubicPathLoops(outline, &result.error);
    }
    for (const auto &loop : result.loops)
        for (const auto &segment : penSegments(loop.points))
            result.fittedCurveSegments += segment.curved;
    return result;
}

PenFillResult fillPolygonMesh(const QPolygonF &polygon,
                              const PolygonMeshSources &sources,
                              const std::function<bool()> &cancelled) {
    PenFillResult result;
    if (!sources.valid()) {
        result.error = QStringLiteral("Square/Triangle mesh geometry is unavailable");
        return result;
    }
    if (polygon.size() < 3) {
        result.error = QStringLiteral("Region outline has no fillable contour");
        return result;
    }
    PolygonMeshRequest request;
    request.points = QVector<QPointF>(polygon.begin(), polygon.end());
    request.sources = sources;
    request.mergeSquares = true;
    const PolygonMeshResult mesh = meshPolygon(request, cancelled);
    if (mesh.cancelled) {
        result.cancelled = true;
        return result;
    }
    if (!mesh.error.isEmpty()) {
        result.error = mesh.error;
        return result;
    }
    const QVector<PolygonMeshPlacement> placements = optimizePolygonMeshWithEllipses(
        mesh.placements, sources, mesh.contour, cancelled);
    if (cancelled && cancelled()) {
        result.cancelled = true;
        return result;
    }
    for (const PolygonMeshPlacement &placement : placements) {
        PenPlacement penPlacement;
        penPlacement.shapeId = placement.shapeId;
        penPlacement.transform = placement.transform;
        penPlacement.coreEllipse = placement.shapeId == kCircleShapeId;
        result.placements.push_back(penPlacement);
    }
    return result;
}

PenFillResult fillRegionOutlineMesh(const QPainterPath &outline,
                                    const PolygonMeshSources &sources,
                                    double simplifyEpsilon,
                                    const std::function<bool()> &cancelled) {
    QPolygonF polygon = largestFlattenedContour(outline);
    if (polygon.size() >= 3) {
        polygon = simplifyClosedPolygon(polygon, simplifyEpsilon);
    }
    return fillPolygonMesh(polygon, sources, cancelled);
}

} // namespace gui
