#include "pen_fill.h"
#include "polygon_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace gui {
namespace {

constexpr double kEpsilon = 1e-8;
constexpr int kCurveSamples = 32;
constexpr int kSquareShapeId = 101;
constexpr int kCircleShapeId = 102;
constexpr int kTriangleShapeId = 103;
constexpr double kMaximumSpillRatio = 0.10;

bool isAllowedPenShape(int shapeId)
{
    return shapeId == kSquareShapeId
        || shapeId == kCircleShapeId
        || shapeId == kTriangleShapeId;
}

double cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

double signedArea(const QPolygonF &polygon)
{
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        result += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    }
    return result * 0.5;
}

double pathArea(const QPainterPath &path)
{
    double result = 0.0;
    for (const QPolygonF &polygon : path.simplified().toSubpathPolygons()) {
        result += signedArea(polygon);
    }
    return std::abs(result);
}

QPointF segmentPoint(const PenBoundarySegment &segment, double t)
{
    if (!segment.curved) {
        return segment.start * (1.0 - t) + segment.end * t;
    }
    const double u = 1.0 - t;
    return segment.start * (u * u)
        + segment.control * (2.0 * u * t)
        + segment.end * (t * t);
}

QVector<QPointF> sampleSegment(const PenBoundarySegment &segment, int samples)
{
    QVector<QPointF> result;
    result.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        result.push_back(segmentPoint(segment, static_cast<double>(i) / samples));
    }
    return result;
}

QPolygonF flattenedContour(const PenContour &contour, int curvedSamples)
{
    QPolygonF result;
    if (contour.segments.isEmpty()) {
        return result;
    }
    result.push_back(contour.segments.front().start);
    for (const PenBoundarySegment &segment : contour.segments) {
        const int samples = segment.curved ? curvedSamples : 1;
        for (int i = 1; i <= samples; ++i) {
            result.push_back(segmentPoint(segment, static_cast<double>(i) / samples));
        }
    }
    if (result.size() > 1 && result.front() == result.back()) {
        result.removeLast();
    }
    return result;
}

double distanceSquaredToSegment(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double length2 = QPointF::dotProduct(ab, ab);
    if (length2 <= kEpsilon) {
        return QPointF::dotProduct(point - a, point - a);
    }
    const double t = std::clamp(QPointF::dotProduct(point - a, ab) / length2, 0.0, 1.0);
    const QPointF delta = point - (a + ab * t);
    return QPointF::dotProduct(delta, delta);
}

double distanceToPolygons(const QPointF &point, const QVector<QPolygonF> &polygons)
{
    double best = std::numeric_limits<double>::max();
    for (const QPolygonF &polygon : polygons) {
        for (int i = 0; i < polygon.size(); ++i) {
            best = std::min(best,
                            distanceSquaredToSegment(point,
                                                     polygon[i],
                                                     polygon[(i + 1) % polygon.size()]));
        }
    }
    return best == std::numeric_limits<double>::max() ? best : std::sqrt(best);
}

int orientation(const QPointF &a, const QPointF &b, const QPointF &c, double epsilon)
{
    const double value = cross(b - a, c - a);
    if (std::abs(value) <= epsilon) {
        return 0;
    }
    return value > 0.0 ? 1 : -1;
}

bool pointOnSegment(const QPointF &point, const QPointF &a, const QPointF &b, double epsilon)
{
    return orientation(a, b, point, epsilon) == 0
        && point.x() >= std::min(a.x(), b.x()) - epsilon
        && point.x() <= std::max(a.x(), b.x()) + epsilon
        && point.y() >= std::min(a.y(), b.y()) - epsilon
        && point.y() <= std::max(a.y(), b.y()) + epsilon;
}

bool lineIntersection(const QPointF &a,
                      const QPointF &b,
                      const QPointF &c,
                      const QPointF &d,
                      double epsilon,
                      QPointF *intersection)
{
    const int o1 = orientation(a, b, c, epsilon);
    const int o2 = orientation(a, b, d, epsilon);
    const int o3 = orientation(c, d, a, epsilon);
    const int o4 = orientation(c, d, b, epsilon);
    if (o1 == o2 && o1 != 0) {
        return false;
    }
    if (o3 == o4 && o3 != 0) {
        return false;
    }
    if (o1 == 0 && !pointOnSegment(c, a, b, epsilon)) {
        return false;
    }
    if (o2 == 0 && !pointOnSegment(d, a, b, epsilon)) {
        return false;
    }
    if (o3 == 0 && !pointOnSegment(a, c, d, epsilon)) {
        return false;
    }
    if (o4 == 0 && !pointOnSegment(b, c, d, epsilon)) {
        return false;
    }
    const QPointF r = b - a;
    const QPointF s = d - c;
    const double denominator = cross(r, s);
    if (std::abs(denominator) <= epsilon) {
        if (intersection != nullptr) {
            *intersection = (a + b + c + d) * 0.25;
        }
        return true;
    }
    if (intersection != nullptr) {
        *intersection = a + r * (cross(c - a, s) / denominator);
    }
    return true;
}

QVector<QPointF> contourCrossings(const QVector<PenBoundarySegment> &segments, double tolerance)
{
    struct FlatEdge {
        QPointF a;
        QPointF b;
        int segment = 0;
        int sample = 0;
        int samples = 0;
    };
    QVector<FlatEdge> edges;
    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        const PenBoundarySegment &segment = segments[segmentIndex];
        int samples = segment.curved ? kCurveSamples : 1;
        if (segment.curved) {
            const double deviation = std::sqrt(distanceSquaredToSegment(segment.control,
                                                                         segment.start,
                                                                         segment.end));
            samples = std::clamp(static_cast<int>(std::ceil(std::sqrt(std::max(1.0, deviation / std::max(tolerance, 1e-5))))) * 2,
                                 8,
                                 128);
        }
        const QVector<QPointF> points = sampleSegment(segment, samples);
        for (int i = 0; i < samples; ++i) {
            edges.push_back({points[i], points[i + 1], segmentIndex, i, samples});
        }
    }
    QVector<QPointF> crossings;
    const int lastSegment = segments.size() - 1;
    for (int i = 0; i < edges.size(); ++i) {
        for (int j = i + 1; j < edges.size(); ++j) {
            const FlatEdge &a = edges[i];
            const FlatEdge &b = edges[j];
            if (a.segment == b.segment && std::abs(a.sample - b.sample) <= 1) {
                continue;
            }
            const bool adjacent = std::abs(a.segment - b.segment) == 1
                || (a.segment == 0 && b.segment == lastSegment)
                || (b.segment == 0 && a.segment == lastSegment);
            if (adjacent) {
                const bool sharedEndpoint = (a.b - b.a).manhattanLength() <= tolerance
                    || (a.a - b.b).manhattanLength() <= tolerance
                    || (a.a - b.a).manhattanLength() <= tolerance
                    || (a.b - b.b).manhattanLength() <= tolerance;
                if (sharedEndpoint) {
                    continue;
                }
            }
            QPointF point;
            if (!lineIntersection(a.a, a.b, b.a, b.b, tolerance * 0.1, &point)) {
                continue;
            }
            const bool duplicate = std::any_of(crossings.begin(), crossings.end(), [&](const QPointF &existing) {
                return QLineF(existing, point).length() <= tolerance * 2.0;
            });
            if (!duplicate) {
                crossings.push_back(point);
            }
        }
    }
    return crossings;
}

QTransform affineFromTriangles(const QPointF &a,
                               const QPointF &b,
                               const QPointF &c,
                               const QPointF &p,
                               const QPointF &q,
                               const QPointF &r,
                               bool *ok)
{
    const QPointF d1 = b - a;
    const QPointF d2 = c - a;
    const QPointF e1 = q - p;
    const QPointF e2 = r - p;
    const double determinant = cross(d1, d2);
    if (std::abs(determinant) <= kEpsilon) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }
    const double m11 = (e1.x() * d2.y() - e2.x() * d1.y()) / determinant;
    const double m21 = (-e1.x() * d2.x() + e2.x() * d1.x()) / determinant;
    const double m12 = (e1.y() * d2.y() - e2.y() * d1.y()) / determinant;
    const double m22 = (-e1.y() * d2.x() + e2.y() * d1.x()) / determinant;
    const double dx = p.x() - m11 * a.x() - m21 * a.y();
    const double dy = p.y() - m12 * a.x() - m22 * a.y();
    if (ok != nullptr) {
        *ok = std::isfinite(m11) && std::isfinite(m12) && std::isfinite(m21)
            && std::isfinite(m22) && std::isfinite(dx) && std::isfinite(dy);
    }
    return QTransform(m11, m12, m21, m22, dx, dy);
}

QTransform centeredTransform(const QRectF &bounds,
                             const QPointF &center,
                             double scaleX,
                             double scaleY)
{
    QTransform result;
    result.translate(center.x(), center.y());
    result.scale(scaleX, scaleY);
    result.translate(-bounds.center().x(), -bounds.center().y());
    return result;
}

QPolygonF primitiveHull(const PenPrimitive &primitive)
{
    QVector<QPointF> points;
    for (const QPolygonF &contour : primitive.contours) {
        for (const QPointF &point : contour) {
            const bool duplicate = std::any_of(points.begin(), points.end(), [&](const QPointF &existing) {
                return QLineF(existing, point).length() <= kEpsilon;
            });
            if (!duplicate) {
                points.push_back(point);
            }
        }
    }
    if (points.size() < 3) {
        return {};
    }
    std::sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) {
        if (a.x() != b.x()) {
            return a.x() < b.x();
        }
        return a.y() < b.y();
    });
    QVector<QPointF> hull;
    for (const QPointF &point : points) {
        while (hull.size() >= 2
               && cross(hull.back() - hull[hull.size() - 2], point - hull.back()) <= kEpsilon) {
            hull.removeLast();
        }
        hull.push_back(point);
    }
    const int lowerSize = hull.size();
    for (int i = points.size() - 2; i >= 0; --i) {
        while (hull.size() > lowerSize
               && cross(hull.back() - hull[hull.size() - 2], points[i] - hull.back()) <= kEpsilon) {
            hull.removeLast();
        }
        hull.push_back(points[i]);
    }
    hull.removeLast();
    return QPolygonF(hull);
}

PolygonMeshSources penMeshSources(const QVector<PenPrimitive> &primitives)
{
    PolygonMeshSources sources;
    for (const PenPrimitive &primitive : primitives) {
        if (primitive.shapeId == kSquareShapeId) {
            sources.square = primitiveHull(primitive);
        } else if (primitive.shapeId == kTriangleShapeId) {
            sources.triangle = primitiveHull(primitive);
        }
    }
    return sources;
}

const PenPrimitive *primitiveForId(const QVector<PenPrimitive> &primitives, int shapeId)
{
    for (const PenPrimitive &primitive : primitives) {
        if (primitive.shapeId == shapeId) {
            return &primitive;
        }
    }
    return nullptr;
}

struct CurvePlacement {
    PenPlacement placement;
    QPainterPath path;
    double boundaryError = 0.0;
    double outsideArea = 0.0;
    double insideArea = 0.0;
};

std::optional<QTransform> circleThroughPoints(const PenPrimitive &primitive,
                                              const QPointF &a,
                                              const QPointF &b,
                                              const QPointF &c)
{
    const double determinant = 2.0 * (a.x() * (b.y() - c.y())
                                      + b.x() * (c.y() - a.y())
                                      + c.x() * (a.y() - b.y()));
    if (std::abs(determinant) <= kEpsilon) {
        return std::nullopt;
    }
    const double a2 = QPointF::dotProduct(a, a);
    const double b2 = QPointF::dotProduct(b, b);
    const double c2 = QPointF::dotProduct(c, c);
    const QPointF center((a2 * (b.y() - c.y()) + b2 * (c.y() - a.y())
                          + c2 * (a.y() - b.y())) / determinant,
                         (a2 * (c.x() - b.x()) + b2 * (a.x() - c.x())
                          + c2 * (b.x() - a.x())) / determinant);
    const double radius = QLineF(center, a).length();
    if (!std::isfinite(radius) || radius <= kEpsilon) {
        return std::nullopt;
    }
    return centeredTransform(primitive.bounds,
                             center,
                             radius * 2.0 / std::max(primitive.bounds.width(), kEpsilon),
                             radius * 2.0 / std::max(primitive.bounds.height(), kEpsilon));
}

std::optional<QTransform> ellipseThroughCurve(const PenPrimitive &primitive,
                                              const QPointF &start,
                                              const QPointF &middle,
                                              const QPointF &end,
                                              bool lowerHalf)
{
    const QPointF sourceStart(primitive.bounds.left(), primitive.bounds.center().y());
    const QPointF sourceMiddle(primitive.bounds.center().x(),
                               lowerHalf ? primitive.bounds.bottom() : primitive.bounds.top());
    const QPointF sourceEnd(primitive.bounds.right(), primitive.bounds.center().y());
    bool ok = false;
    const QTransform transform = affineFromTriangles(sourceStart,
                                                     sourceMiddle,
                                                     sourceEnd,
                                                     start,
                                                     middle,
                                                     end,
                                                     &ok);
    return ok ? std::optional<QTransform>(transform) : std::nullopt;
}

std::optional<CurvePlacement> evaluateCurvePlacement(const PenPrimitive &primitive,
                                                     const PenBoundarySegment &segment,
                                                     const QTransform &transform,
                                                     const QPainterPath &target)
{
    if (!transform.isAffine() || std::abs(transform.determinant()) <= kEpsilon) {
        return std::nullopt;
    }
    CurvePlacement result;
    result.path = transform.map(primitive.silhouette).simplified();
    const double area = pathArea(result.path);
    if (area <= kEpsilon) {
        return std::nullopt;
    }
    result.insideArea = pathArea(result.path.intersected(target));
    result.outsideArea = std::max(0.0, area - result.insideArea);
    const double targetArea = pathArea(target);
    if (targetArea <= kEpsilon
        || result.outsideArea / targetArea > kMaximumSpillRatio + 1e-9) {
        return std::nullopt;
    }
    const QVector<QPolygonF> boundaries = result.path.toSubpathPolygons();
    for (const QPointF &point : sampleSegment(segment, kCurveSamples)) {
        result.boundaryError = std::max(result.boundaryError,
                                        distanceToPolygons(point, boundaries));
    }
    result.placement.shapeId = primitive.shapeId;
    result.placement.transform = transform;
    result.placement.area = area;
    return result;
}

bool betterCurvePlacement(const CurvePlacement &candidate, const CurvePlacement &best)
{
    if (std::abs(candidate.boundaryError - best.boundaryError) > kEpsilon) {
        return candidate.boundaryError < best.boundaryError;
    }
    if (std::abs(candidate.outsideArea - best.outsideArea) > kEpsilon) {
        return candidate.outsideArea < best.outsideArea;
    }
    if (std::abs(candidate.insideArea - best.insideArea) > kEpsilon) {
        return candidate.insideArea > best.insideArea;
    }
    return candidate.placement.shapeId < best.placement.shapeId;
}

std::optional<CurvePlacement> outwardCurvePlacement(const PenPrimitive &circle,
                                                    const PenBoundarySegment &segment,
                                                    const QPainterPath &target)
{
    const QPointF middle = segmentPoint(segment, 0.5);
    QVector<QTransform> transforms;
    if (const auto circleTransform = circleThroughPoints(circle,
                                                         segment.start,
                                                         middle,
                                                         segment.end)) {
        transforms.push_back(*circleTransform);
    }
    for (bool lowerHalf : {false, true}) {
        if (const auto ellipseTransform = ellipseThroughCurve(circle,
                                                              segment.start,
                                                              middle,
                                                              segment.end,
                                                              lowerHalf)) {
            transforms.push_back(*ellipseTransform);
        }
    }
    std::optional<CurvePlacement> best;
    for (const QTransform &transform : transforms) {
        const auto candidate = evaluateCurvePlacement(circle, segment, transform, target);
        if (candidate && (!best || betterCurvePlacement(*candidate, *best))) {
            best = candidate;
        }
    }
    return best;
}

} // namespace

PenContour buildPenContour(const QVector<PenPoint> &points, double flatnessTolerance)
{
    PenContour result;
    if (points.size() < 3) {
        result.error = QStringLiteral("A Pen path needs at least three points");
        return result;
    }
    int firstHard = -1;
    for (int i = 0; i < points.size(); ++i) {
        if (points[i].kind == PenPointKind::Hard) {
            firstHard = i;
            break;
        }
    }
    if (firstHard < 0) {
        result.error = QStringLiteral("A Pen path needs at least one hard point");
        return result;
    }
    QVector<PenPoint> ordered;
    ordered.reserve(points.size());
    for (int i = 0; i < points.size(); ++i) {
        ordered.push_back(points[(firstHard + i) % points.size()]);
    }
    QPointF current = ordered.front().position;
    result.path.setFillRule(Qt::WindingFill);
    result.path.moveTo(current);
    int index = 1;
    while (index <= ordered.size()) {
        const PenPoint &next = ordered[index % ordered.size()];
        if (next.kind == PenPointKind::Hard) {
            if (QLineF(current, next.position).length() > kEpsilon) {
                result.segments.push_back({current, {}, next.position, false});
                result.path.lineTo(next.position);
            }
            current = next.position;
            ++index;
            continue;
        }
        const PenPoint &after = ordered[(index + 1) % ordered.size()];
        const QPointF end = after.kind == PenPointKind::Hard
            ? after.position
            : (next.position + after.position) * 0.5;
        if (QLineF(current, end).length() > kEpsilon
            || QLineF(current, next.position).length() > kEpsilon) {
            result.segments.push_back({current, next.position, end, true});
            result.path.quadTo(next.position, end);
        }
        current = end;
        index += after.kind == PenPointKind::Hard ? 2 : 1;
    }
    result.path.closeSubpath();
    if (result.segments.size() < 2) {
        result.path = {};
        result.segments.clear();
        result.error = QStringLiteral("The Pen path has no fillable area");
        return result;
    }
    result.crossings = contourCrossings(result.segments, std::max(flatnessTolerance, 1e-5));
    if (!result.crossings.isEmpty()) {
        result.error = QStringLiteral("The Pen path crosses itself");
        return result;
    }
    if (pathArea(result.path) <= kEpsilon) {
        result.path = {};
        result.segments.clear();
        result.error = QStringLiteral("The Pen path has no fillable area");
    }
    return result;
}

PenPrimitive buildPenPrimitive(int shapeId, const ShapeGeometry &geometry)
{
    PenPrimitive result;
    result.shapeId = shapeId;
    result.silhouette.setFillRule(Qt::WindingFill);
    for (const ShapeTriangle &triangle : geometry.triangles) {
        if (triangle.alpha0 <= 0.0 && triangle.alpha1 <= 0.0 && triangle.alpha2 <= 0.0) {
            continue;
        }
        QPolygonF polygon({triangle.p0, triangle.p1, triangle.p2});
        if (signedArea(polygon) < 0.0) {
            std::swap(polygon[1], polygon[2]);
        }
        result.silhouette.addPolygon(polygon);
        result.silhouette.closeSubpath();
    }
    if (result.silhouette.isEmpty()) {
        result.silhouette.addRect(QRectF(-geometry.width * 0.5,
                                        -geometry.height * 0.5,
                                        geometry.width,
                                        geometry.height));
    }
    result.silhouette = result.silhouette.simplified();
    result.silhouette.setFillRule(Qt::WindingFill);
    result.contours = result.silhouette.toSubpathPolygons();
    result.bounds = result.silhouette.boundingRect();
    result.area = pathArea(result.silhouette);
    return result;
}

QVector<PenPrimitive> buildPenPrimitiveCatalog(const ShapeGeometryStore &geometry,
                                               int firstShapeId,
                                               int lastShapeId)
{
    QVector<PenPrimitive> result;
    for (int shapeId = firstShapeId; shapeId <= lastShapeId; ++shapeId) {
        const ShapeGeometry *shape = geometry.shape(shapeId);
        if (shape == nullptr) {
            continue;
        }
        PenPrimitive primitive = buildPenPrimitive(shapeId, *shape);
        if (!primitive.silhouette.isEmpty() && primitive.area > kEpsilon) {
            result.push_back(std::move(primitive));
        }
    }
    return result;
}

PenFillResult fillPenPath(const PenFillRequest &request,
                         const std::function<bool()> &cancelled)
{
    PenFillResult result;
    if (request.boundaryTolerance <= 0.0 || !std::isfinite(request.boundaryTolerance)) {
        result.error = QStringLiteral("Pen boundary tolerance must be positive");
        return result;
    }
    QVector<PenPrimitive> primitives;
    for (const PenPrimitive &primitive : request.primitives) {
        if (isAllowedPenShape(primitive.shapeId)) {
            primitives.push_back(primitive);
        }
    }
    const PolygonMeshSources meshSources = penMeshSources(primitives);
    const PenPrimitive *circle = primitiveForId(primitives, kCircleShapeId);
    if (!meshSources.valid() || circle == nullptr) {
        result.error = QStringLiteral("Pen Primitive geometry is unavailable");
        return result;
    }
    const PenContour contour = buildPenContour(request.points, request.boundaryTolerance * 0.25);
    if (!contour.valid()) {
        result.error = contour.error.isEmpty() ? QStringLiteral("Invalid Pen contour") : contour.error;
        return result;
    }
    result.targetArea = pathArea(contour.path);
    result.shapeLimit = request.points.size() * 2;
    const QPolygonF flattened = flattenedContour(contour, 16);
    const double orientationSign = signedArea(flattened) >= 0.0 ? 1.0 : -1.0;
    QVector<std::optional<CurvePlacement>> curvePlacements(contour.segments.size());
    QVector<int> midpointCandidates;
    for (int i = 0; i < contour.segments.size(); ++i) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        const PenBoundarySegment &segment = contour.segments[i];
        if (!segment.curved) {
            continue;
        }
        const QPointF chord = segment.end - segment.start;
        const QPointF chordMiddle = (segment.start + segment.end) * 0.5;
        const QPointF curveMiddle = segmentPoint(segment, 0.5);
        const bool outward = cross(chord, curveMiddle - chordMiddle) * orientationSign < -kEpsilon;
        if (outward) {
            curvePlacements[i] = outwardCurvePlacement(*circle, segment, contour.path);
        }
        if (!curvePlacements[i]) {
            midpointCandidates.push_back(i);
        }
    }
    const int capCount = std::count_if(curvePlacements.begin(), curvePlacements.end(), [](const auto &placement) {
        return placement.has_value();
    });
    const int baseVertices = contour.segments.size();
    const int maximumCoreVertices = std::max(3, result.shapeLimit - capCount + 2);
    const int midpointBudget = std::max(0, maximumCoreVertices - baseVertices);
    std::sort(midpointCandidates.begin(), midpointCandidates.end(), [&](int a, int b) {
        const PenBoundarySegment &left = contour.segments[a];
        const PenBoundarySegment &right = contour.segments[b];
        const double leftCurvature = std::sqrt(distanceSquaredToSegment(left.control, left.start, left.end));
        const double rightCurvature = std::sqrt(distanceSquaredToSegment(right.control, right.start, right.end));
        if (std::abs(leftCurvature - rightCurvature) > kEpsilon) {
            return leftCurvature > rightCurvature;
        }
        return a < b;
    });
    QSet<int> midpointSegments;
    for (int i = 0; i < std::min(midpointBudget, static_cast<int>(midpointCandidates.size())); ++i) {
        midpointSegments.insert(midpointCandidates[i]);
    }
    QVector<QPointF> corePoints;
    corePoints.push_back(contour.segments.front().start);
    QPainterPath coverage;
    coverage.setFillRule(Qt::WindingFill);
    for (int i = 0; i < contour.segments.size(); ++i) {
        const PenBoundarySegment &segment = contour.segments[i];
        if (curvePlacements[i]) {
            result.placements.push_back(curvePlacements[i]->placement);
            coverage = coverage.united(curvePlacements[i]->path);
        } else if (segment.curved && midpointSegments.contains(i)) {
            corePoints.push_back(segmentPoint(segment, 0.5));
        }
        corePoints.push_back(segment.end);
    }
    QVector<QPointF> normalizedCore;
    for (const QPointF &point : corePoints) {
        if (normalizedCore.isEmpty() || QLineF(normalizedCore.back(), point).length() > kEpsilon) {
            normalizedCore.push_back(point);
        }
    }
    if (normalizedCore.size() > 1
        && QLineF(normalizedCore.front(), normalizedCore.back()).length() <= kEpsilon) {
        normalizedCore.removeLast();
    }
    if (normalizedCore.size() < 3) {
        result.error = QStringLiteral("The Pen contour left no polygonal core");
        return result;
    }
    PolygonMeshRequest meshRequest;
    meshRequest.points = normalizedCore;
    meshRequest.sources = meshSources;
    meshRequest.mergeSquares = true;
    const PolygonMeshResult mesh = meshPolygon(meshRequest, cancelled);
    if (mesh.cancelled || (cancelled && cancelled())) {
        result.placements.clear();
        result.cancelled = true;
        return result;
    }
    if (!mesh.error.isEmpty()) {
        result.error = QStringLiteral("Could not fill the Pen core: %1").arg(mesh.error);
        return result;
    }
    for (const PolygonMeshPlacement &placement : mesh.placements) {
        const PenPrimitive *primitive = primitiveForId(primitives, placement.shapeId);
        if (primitive == nullptr) {
            result.error = QStringLiteral("Pen core selected unavailable Primitive %1").arg(placement.shapeId);
            return result;
        }
        PenPlacement penPlacement;
        penPlacement.shapeId = placement.shapeId;
        penPlacement.transform = placement.transform;
        penPlacement.area = primitive->area * std::abs(placement.transform.determinant());
        result.placements.push_back(penPlacement);
        coverage = coverage.united(placement.transform.map(primitive->silhouette));
    }
    if (result.placements.size() > result.shapeLimit) {
        result.error = QStringLiteral("Pen fill exceeded its shape limit");
        result.placements.clear();
        return result;
    }
    result.coveredArea = pathArea(coverage.intersected(contour.path));
    result.outsideArea = pathArea(coverage.subtracted(contour.path));
    result.unfilled = contour.path.subtracted(coverage);
    return result;
}

} // namespace gui
