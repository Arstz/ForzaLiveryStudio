#include "region_fill.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr double kGeometryEpsilon = 1e-9;
constexpr int kBoundarySamplesPerCurve = 32;

// One boundary op of a subpath: a straight segment to a corner (Line) or a
// cubic bezier (Cubic, with two controls and an endpoint).
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

struct ConvertiblePoint {
    PenPoint point;
    bool removable = false;
};

double signedArea(const QPolygonF &polygon)
{
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon[i];
        const QPointF &b = polygon[(i + 1) % polygon.size()];
        result += a.x() * b.y() - a.y() * b.x();
    }
    return result * 0.5;
}

QPointF cubicPoint(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p3, double t)
{
    const double u = 1.0 - t;
    return p0 * (u * u * u)
        + c1 * (3.0 * u * u * t)
        + c2 * (3.0 * u * t * t)
        + p3 * (t * t * t);
}

// Split a QPainterPath into subpaths, preserving Line/Cubic element types.
QVector<Subpath> toSubpaths(const QPainterPath &path, double closureTolerance = 1e-6)
{
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

QPolygonF flattenSubpath(const Subpath &subpath, int curveSamples = 8)
{
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

double perpendicularDistance(const QPointF &point, const QPointF &a, const QPointF &b)
{
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

double pointToClosedPolylineDistance(const QPointF &point, const QPolygonF &polyline)
{
    if (polyline.isEmpty()) {
        return std::numeric_limits<double>::infinity();
    }
    double best = std::numeric_limits<double>::infinity();
    for (int i = 0; i < polyline.size(); ++i) {
        best = std::min(best,
                        perpendicularDistance(point,
                                              polyline[i],
                                              polyline[(i + 1) % polyline.size()]));
    }
    return best;
}

double boundaryDeviation(const QPolygonF &left, const QPolygonF &right)
{
    if (left.size() < 3 || right.size() < 3) {
        return std::numeric_limits<double>::infinity();
    }
    double result = 0.0;
    for (const QPointF &point : left) {
        result = std::max(result, pointToClosedPolylineDistance(point, right));
    }
    for (const QPointF &point : right) {
        result = std::max(result, pointToClosedPolylineDistance(point, left));
    }
    return result;
}

QPolygonF flattenPenContour(const PenContour &contour, int curveSamples)
{
    QPolygonF polygon;
    if (contour.segments.isEmpty()) {
        return polygon;
    }
    polygon.push_back(contour.segments.front().start);
    for (const PenBoundarySegment &segment : contour.segments) {
        if (!segment.curved) {
            polygon.push_back(segment.end);
            continue;
        }
        for (int step = 1; step <= curveSamples; ++step) {
            const double t = static_cast<double>(step) / curveSamples;
            const double u = 1.0 - t;
            polygon.push_back(segment.start * (u * u)
                              + segment.control * (2.0 * u * t)
                              + segment.end * (t * t));
        }
    }
    while (polygon.size() > 1
           && QLineF(polygon.back(), polygon.front()).length() <= kGeometryEpsilon) {
        polygon.removeLast();
    }
    return polygon;
}

QVector<PenPoint> penPoints(const QVector<ConvertiblePoint> &points)
{
    QVector<PenPoint> result;
    result.reserve(points.size());
    for (const ConvertiblePoint &point : points) {
        result.push_back(point.point);
    }
    return result;
}

double removalDisplacement(const QVector<ConvertiblePoint> &points, int index)
{
    if (points.size() < 3 || index < 0 || index >= points.size()
        || !points[index].removable
        || points[index].point.kind != PenPointKind::Hard) {
        return std::numeric_limits<double>::infinity();
    }
    const int previous = (index + points.size() - 1) % points.size();
    const int next = (index + 1) % points.size();
    if (points[previous].point.kind != PenPointKind::Soft
        || points[next].point.kind != PenPointKind::Soft) {
        return std::numeric_limits<double>::infinity();
    }
    const QPointF implied =
        (points[previous].point.position + points[next].point.position) * 0.5;
    return QLineF(points[index].point.position, implied).length();
}

bool sameOrientation(double referenceArea, double candidateArea)
{
    return (referenceArea > kGeometryEpsilon && candidateArea > kGeometryEpsilon)
        || (referenceArea < -kGeometryEpsilon && candidateArea < -kGeometryEpsilon);
}

QVector<ConvertiblePoint> initialPenPoints(const Subpath &subpath,
                                           double closureTolerance)
{
    QVector<ConvertiblePoint> points;
    if (subpath.ops.isEmpty()) {
        return points;
    }
    const int opCount = subpath.ops.size();
    const bool startRemovable = subpath.ops.back().kind == Op::Cubic
        && subpath.ops.front().kind == Op::Cubic;
    points.push_back({{subpath.start, PenPointKind::Hard}, startRemovable});
    QPointF previous = subpath.start;
    for (int i = 0; i < opCount; ++i) {
        const Op &op = subpath.ops[i];
        const bool closesAtStart = i == opCount - 1
            && QLineF(op.end, subpath.start).length() <= closureTolerance;
        if (op.kind == Op::Line) {
            if (!closesAtStart) {
                points.push_back({{op.end, PenPointKind::Hard}, false});
            }
        } else {
            const QPointF control =
                (op.control1 * 3.0 + op.control2 * 3.0 - previous - op.end) * 0.25;
            points.push_back({{control, PenPointKind::Soft}, false});
            if (!closesAtStart) {
                const Op &next = subpath.ops[(i + 1) % opCount];
                points.push_back({{op.end, PenPointKind::Hard},
                                  next.kind == Op::Cubic});
            }
        }
        previous = op.end;
    }
    return points;
}

void protectCyclicSeam(QVector<ConvertiblePoint> *points)
{
    const bool hasProtectedHard = std::any_of(points->cbegin(),
                                              points->cend(),
                                              [](const ConvertiblePoint &point) {
        return point.point.kind == PenPointKind::Hard && !point.removable;
    });
    if (hasProtectedHard) {
        return;
    }
    int seam = -1;
    double largestDisplacement = -1.0;
    for (int i = 0; i < points->size(); ++i) {
        const double displacement = removalDisplacement(*points, i);
        if (std::isfinite(displacement) && displacement > largestDisplacement) {
            largestDisplacement = displacement;
            seam = i;
        }
    }
    if (seam >= 0) {
        (*points)[seam].removable = false;
    }
}

struct OuterSelection {
    Subpath subpath;
    QPolygonF polygon;
    QString error;
};

OuterSelection selectClosedOuter(const QPainterPath &outline,
                                 double closureTolerance)
{
    OuterSelection result;
    const QVector<Subpath> subpaths = toSubpaths(outline, closureTolerance);
    if (subpaths.isEmpty()) {
        result.error = QStringLiteral("Region outline has no contour");
        return result;
    }
    QVector<QPolygonF> polygons;
    polygons.reserve(subpaths.size());
    for (const Subpath &subpath : subpaths) {
        if (!subpath.closed) {
            result.error = QStringLiteral("Region outline contains an open contour");
            return result;
        }
        QPolygonF polygon = flattenSubpath(subpath, kBoundarySamplesPerCurve);
        while (polygon.size() > 1
               && QLineF(polygon.back(), polygon.front()).length() <= closureTolerance) {
            polygon.removeLast();
        }
        if (polygon.size() < 3 || std::abs(signedArea(polygon)) <= kGeometryEpsilon) {
            result.error = QStringLiteral("Region outline contains a degenerate contour");
            return result;
        }
        polygons.push_back(std::move(polygon));
    }

    QVector<int> outerIndices;
    for (int i = 0; i < polygons.size(); ++i) {
        bool contained = false;
        const QPointF probe = polygons[i].front();
        for (int j = 0; j < polygons.size(); ++j) {
            if (i != j && polygons[j].containsPoint(probe, Qt::OddEvenFill)) {
                contained = true;
                break;
            }
        }
        if (!contained) {
            outerIndices.push_back(i);
        }
    }
    if (outerIndices.size() != 1) {
        result.error = outerIndices.isEmpty()
            ? QStringLiteral("Region outline has no outer contour")
            : QStringLiteral("Region outline contains multiple outer contours");
        return result;
    }
    result.subpath = subpaths[outerIndices.front()];
    result.polygon = polygons[outerIndices.front()];
    return result;
}

// RDP on an open polyline [first, last] inclusive; appends kept points (except
// the first) to `out`.
void rdpRecurse(const QPolygonF &points, int first, int last, double epsilon, QPolygonF &out)
{
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

// Corridor RDP recursion: keep the chord when it is within epsilon OR when it
// runs entirely through free (occluded) space, otherwise recurse.
void rdpCorridorRecurse(const QPolygonF &points, int first, int last, double epsilon,
                        const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace,
                        QPolygonF &out)
{
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

// The flattened outer (largest-area) subpath of an outline, as a simple
// polygon with any closing duplicate vertex removed. Empty on failure.
QPolygonF largestFlattenedContour(const QPainterPath &outline)
{
    const QVector<Subpath> subpaths = toSubpaths(outline);
    if (subpaths.isEmpty()) {
        return {};
    }
    int outer = 0;
    double outerArea = -1.0;
    QPolygonF outerPolygon;
    for (int i = 0; i < subpaths.size(); ++i) {
        const QPolygonF polygon = flattenSubpath(subpaths[i]);
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
    const RegionPenConversionOptions &options)
{
    RegionPenConversionResult result;
    if (!std::isfinite(options.mergeTolerance) || options.mergeTolerance < 0.0
        || !std::isfinite(options.closureTolerance) || options.closureTolerance <= 0.0
        || options.maxOptimizedPointCount < 0) {
        result.error = QStringLiteral("Region Pen conversion options are invalid");
        return result;
    }

    const OuterSelection outer = selectClosedOuter(outline, options.closureTolerance);
    if (!outer.error.isEmpty()) {
        result.error = outer.error;
        return result;
    }

    QVector<ConvertiblePoint> working =
        initialPenPoints(outer.subpath, options.closureTolerance);
    protectCyclicSeam(&working);
    result.originalPointCount = working.size();
    const PenContour baseline = buildPenContour(penPoints(working));
    if (!baseline.valid()) {
        result.error = baseline.error.isEmpty()
            ? QStringLiteral("Region outline does not form a valid Pen contour")
            : baseline.error;
        return result;
    }
    if (working.size() > options.maxOptimizedPointCount) {
        result.points = penPoints(working);
        result.optimizationSkipped = true;
        return result;
    }

    const QPolygonF baselinePolygon =
        flattenPenContour(baseline, kBoundarySamplesPerCurve);
    const double referenceArea = signedArea(baselinePolygon);
    result.baselineDeviation = boundaryDeviation(outer.polygon, baselinePolygon);
    result.maximumDeviation = result.baselineDeviation;
    const double allowedDeviation = result.baselineDeviation + options.mergeTolerance;

    struct Candidate {
        int pointIndex = -1;
        double displacement = 0.0;
        int previous = -1;
        int next = -1;
    };
    QVector<Candidate> candidates;
    QVector<int> candidateAtPoint(working.size(), -1);
    for (int i = 0; i < working.size(); ++i) {
        const double displacement = removalDisplacement(working, i);
        if (std::isfinite(displacement)
            && displacement <= options.mergeTolerance + kGeometryEpsilon) {
            candidateAtPoint[i] = candidates.size();
            candidates.push_back({i, displacement});
        }
    }
    for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
        const int pointIndex = candidates[candidateIndex].pointIndex;
        for (int offset = 1; offset < working.size(); ++offset) {
            const int nextPoint = (pointIndex + offset) % working.size();
            if (working[nextPoint].point.kind != PenPointKind::Hard) {
                continue;
            }
            const int nextCandidate = candidateAtPoint[nextPoint];
            if (nextCandidate >= 0) {
                candidates[candidateIndex].next = nextCandidate;
                candidates[nextCandidate].previous = candidateIndex;
            }
            break;
        }
    }

    QVector<int> order;
    order.reserve(candidates.size());
    for (int i = 0; i < candidates.size(); ++i) {
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        const Candidate &a = candidates[left];
        const Candidate &b = candidates[right];
        if (std::abs(a.displacement - b.displacement) > kGeometryEpsilon) {
            return a.displacement < b.displacement;
        }
        return a.pointIndex < b.pointIndex;
    });

    QVector<int> parents(candidates.size(), -1);
    QVector<double> clusterErrors(candidates.size(), 0.0);
    QVector<bool> accepted(candidates.size(), false);
    QVector<bool> removedPoints(working.size(), false);
    const auto findRoot = [&](int candidateIndex) {
        int root = candidateIndex;
        while (parents[root] != root) {
            root = parents[root];
        }
        int current = candidateIndex;
        while (parents[current] != current) {
            const int parent = parents[current];
            parents[current] = root;
            current = parent;
        }
        return root;
    };
    for (const int candidateIndex : order) {
        const Candidate &candidate = candidates[candidateIndex];
        const int leftRoot = candidate.previous >= 0 && accepted[candidate.previous]
            ? findRoot(candidate.previous)
            : -1;
        const int rightRoot = candidate.next >= 0 && accepted[candidate.next]
            ? findRoot(candidate.next)
            : -1;
        double combinedError = candidate.displacement;
        if (leftRoot >= 0) {
            combinedError += clusterErrors[leftRoot];
        }
        if (rightRoot >= 0 && rightRoot != leftRoot) {
            combinedError += clusterErrors[rightRoot];
        }
        if (combinedError > options.mergeTolerance + kGeometryEpsilon) {
            continue;
        }
        accepted[candidateIndex] = true;
        removedPoints[candidate.pointIndex] = true;
        parents[candidateIndex] = candidateIndex;
        clusterErrors[candidateIndex] = combinedError;
        if (leftRoot >= 0) {
            parents[leftRoot] = candidateIndex;
        }
        if (rightRoot >= 0 && rightRoot != leftRoot) {
            parents[rightRoot] = candidateIndex;
        }
    }

    QVector<ConvertiblePoint> simplified;
    simplified.reserve(working.size());
    for (int i = 0; i < working.size(); ++i) {
        if (!removedPoints[i]) {
            simplified.push_back(working[i]);
        }
    }
    const PenContour simplifiedContour = buildPenContour(penPoints(simplified));
    if (simplifiedContour.valid()) {
        const QPolygonF candidatePolygon =
            flattenPenContour(simplifiedContour, kBoundarySamplesPerCurve);
        const double deviation = boundaryDeviation(outer.polygon, candidatePolygon);
        if (sameOrientation(referenceArea, signedArea(candidatePolygon))
            && std::isfinite(deviation)
            && deviation <= allowedDeviation + kGeometryEpsilon) {
            working = std::move(simplified);
            result.maximumDeviation = deviation;
            result.removedHardPoints = std::count(removedPoints.cbegin(),
                                                  removedPoints.cend(),
                                                  true);
        }
    }

    result.points = penPoints(working);
    return result;
}

PenFillResult fillRegionOutline(const QPainterPath &outline,
                                const QVector<PenPrimitive> &primitives,
                                double boundaryTolerance,
                                const std::function<bool()> &cancelled)
{
    PenFillResult result;
    const RegionPenConversionResult conversion = regionOutlineToPenPoints(outline);
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
    return fillPenPath(request, cancelled);
}

QPolygonF simplifyClosedPolygon(const QPolygonF &polygon, double epsilon)
{
    if (epsilon <= 0.0 || polygon.size() <= 4) {
        return polygon;
    }
    // Anchor the RDP at the two extreme (farthest-apart) vertices so the closed
    // loop is split into two open chains, each simplified independently.
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
    // Reorder so the loop starts at vertex 0 and includes anchorB as a split.
    QPolygonF result;
    result.push_back(chain[0]);
    rdpRecurse(chain, 0, anchorB, epsilon, result);
    // Second half: anchorB back around to the start (closing vertex == chain[0]).
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

QPolygonF simplifyClosedPolygonCorridor(
    const QPolygonF &polygon, double epsilon,
    const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace)
{
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

QPolygonF regionOuterContour(const QPainterPath &outline)
{
    return largestFlattenedContour(outline);
}

PenFillResult fillPolygonMesh(const QPolygonF &polygon,
                              const PolygonMeshSources &sources,
                              const std::function<bool()> &cancelled)
{
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
    for (const PolygonMeshPlacement &placement : mesh.placements) {
        PenPlacement penPlacement;
        penPlacement.shapeId = placement.shapeId;
        penPlacement.transform = placement.transform;
        result.placements.push_back(penPlacement);
    }
    return result;
}

PenFillResult fillRegionOutlineMesh(const QPainterPath &outline,
                                    const PolygonMeshSources &sources,
                                    double simplifyEpsilon,
                                    const std::function<bool()> &cancelled)
{
    QPolygonF polygon = largestFlattenedContour(outline);
    if (polygon.size() >= 3) {
        polygon = simplifyClosedPolygon(polygon, simplifyEpsilon);
    }
    return fillPolygonMesh(polygon, sources, cancelled);
}

} // namespace gui
