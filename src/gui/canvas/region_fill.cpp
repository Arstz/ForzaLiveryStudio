#include "region_fill.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

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
QVector<Subpath> toSubpaths(const QPainterPath &path)
{
    QVector<Subpath> subpaths;
    Subpath current;
    bool have = false;
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        if (element.isMoveTo()) {
            if (have) {
                subpaths.push_back(current);
            }
            current = Subpath{};
            current.start = QPointF(element.x, element.y);
            have = true;
        } else if (element.isLineTo()) {
            Op op;
            op.kind = Op::Line;
            op.end = QPointF(element.x, element.y);
            current.ops.push_back(op);
        } else if (element.type == QPainterPath::CurveToElement) {
            Op op;
            op.kind = Op::Cubic;
            op.control1 = QPointF(element.x, element.y);
            op.control2 = QPointF(path.elementAt(i + 1).x, path.elementAt(i + 1).y);
            op.end = QPointF(path.elementAt(i + 2).x, path.elementAt(i + 2).y);
            current.ops.push_back(op);
            i += 2;
        }
    }
    if (have) {
        subpaths.push_back(current);
    }
    return subpaths;
}

QPolygonF flattenSubpath(const Subpath &subpath)
{
    QPolygonF polygon;
    polygon.push_back(subpath.start);
    QPointF previous = subpath.start;
    for (const Op &op : subpath.ops) {
        if (op.kind == Op::Line) {
            polygon.push_back(op.end);
        } else {
            for (int step = 1; step <= 8; ++step) {
                polygon.push_back(cubicPoint(previous, op.control1, op.control2, op.end, step / 8.0));
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
    const double t = ((point.x() - a.x()) * ab.x() + (point.y() - a.y()) * ab.y()) / lengthSquared;
    const QPointF projection = a + t * ab;
    return QLineF(point, projection).length();
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

QVector<PenPoint> regionOutlineToPenPoints(const QPainterPath &outline)
{
    const QVector<Subpath> subpaths = toSubpaths(outline);
    if (subpaths.isEmpty()) {
        return {};
    }

    // Fill the largest-area subpath (the outer contour); holes are overpainted.
    int outer = 0;
    double outerArea = -1.0;
    for (int i = 0; i < subpaths.size(); ++i) {
        const double area = std::abs(signedArea(flattenSubpath(subpaths[i])));
        if (area > outerArea) {
            outerArea = area;
            outer = i;
        }
    }
    const Subpath &subpath = subpaths[outer];

    QVector<PenPoint> points;
    points.push_back({subpath.start, PenPointKind::Hard});
    QPointF previous = subpath.start;
    for (const Op &op : subpath.ops) {
        if (op.kind == Op::Line) {
            points.push_back({op.end, PenPointKind::Hard});
        } else {
            // Single-quadratic approximation of the cubic; its control point is
            // a Soft Pen point, the on-curve endpoint stays a Hard anchor.
            const QPointF control = (op.control1 * 3.0 + op.control2 * 3.0 - previous - op.end) * 0.25;
            points.push_back({control, PenPointKind::Soft});
            points.push_back({op.end, PenPointKind::Hard});
        }
        previous = op.end;
    }

    // Drop the closing point if it coincides with the start (closed subpath).
    while (points.size() > 1
           && points.back().kind == PenPointKind::Hard
           && QLineF(points.back().position, points.front().position).length() <= 1e-6) {
        points.removeLast();
    }
    return points;
}

PenFillResult fillRegionOutline(const QPainterPath &outline,
                                const QVector<PenPrimitive> &primitives,
                                double boundaryTolerance,
                                const std::function<bool()> &cancelled)
{
    PenFillResult result;
    const QVector<PenPoint> points = regionOutlineToPenPoints(outline);
    if (points.size() < 3) {
        result.error = QStringLiteral("Region outline has no fillable contour");
        return result;
    }
    PenFillRequest request;
    request.points = points;
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
