#include "pen_fill.h"
#include "polygon_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr double kEpsilon = 1e-8;
constexpr int kCurveSamples = 32;
constexpr int kInteriorCurveSamples = 128;
constexpr int kSquareShapeId = 101;
constexpr int kCircleShapeId = 102;
constexpr int kTriangleShapeId = 103;

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
    if (polygon.size() < 3) {
        return 0.0;
    }
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        result += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    }
    return result * 0.5;
}

QPointF polygonInteriorSample(const QPolygonF &polygon)
{
    if (polygon.size() < 3) {
        return polygon.isEmpty() ? QPointF() : polygon.front();
    }
    const QRectF bounds = polygon.boundingRect();
    const double orientationSign = signedArea(polygon) >= 0.0 ? 1.0 : -1.0;
    const double baseOffset = std::max(1e-9, std::min(bounds.width(), bounds.height()) * 1e-6);
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF a = polygon[i];
        const QPointF b = polygon[(i + 1) % polygon.size()];
        const QPointF edge = b - a;
        const double length = std::hypot(edge.x(), edge.y());
        if (length <= kEpsilon) {
            continue;
        }
        const QPointF left(-edge.y() / length, edge.x() / length);
        const QPointF middle = (a + b) * 0.5;
        for (double sign : {orientationSign, -orientationSign}) {
            const QPointF sample = middle + left * sign * std::min(baseOffset, length * 1e-4);
            if (polygon.containsPoint(sample, Qt::OddEvenFill)) {
                return sample;
            }
        }
    }
    constexpr int grid = 16;
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
            const QPointF sample(bounds.left() + bounds.width() * (x + 0.5) / grid,
                                 bounds.top() + bounds.height() * (y + 0.5) / grid);
            if (polygon.containsPoint(sample, Qt::OddEvenFill)) {
                return sample;
            }
        }
    }
    return bounds.center();
}

double pathArea(const QPainterPath &path)
{
    const QVector<QPolygonF> polygons = path.toSubpathPolygons();
    double result = 0.0;
    for (int i = 0; i < polygons.size(); ++i) {
        const QPointF sample = polygonInteriorSample(polygons[i]);
        int depth = 0;
        for (int j = 0; j < polygons.size(); ++j) {
            if (i != j && polygons[j].containsPoint(sample, Qt::OddEvenFill)) {
                ++depth;
            }
        }
        const double area = std::abs(signedArea(polygons[i]));
        result += (depth % 2 == 0) ? area : -area;
    }
    return std::max(0.0, result);
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

QPolygonF interiorMeshPolygon(const PenContour &contour)
{
    QPolygonF raw;
    if (contour.segments.isEmpty()) {
        return raw;
    }
    raw.push_back(contour.segments.front().start);
    for (const PenBoundarySegment &segment : contour.segments) {
        const int samples = segment.curved ? kInteriorCurveSamples : 1;
        for (int i = 1; i <= samples; ++i) {
            raw.push_back(segmentPoint(segment, static_cast<double>(i) / samples));
        }
    }

    const double orientationSign = signedArea(raw) >= 0.0 ? 1.0 : -1.0;
    QPolygonF result;
    result.reserve(raw.size());
    result.push_back(contour.segments.front().start);
    for (const PenBoundarySegment &segment : contour.segments) {
        const int samples = segment.curved ? kInteriorCurveSamples : 1;
        const QPointF curvature = segment.start - segment.control * 2.0 + segment.end;
        const QPointF localSagitta = -curvature / (4.0 * samples * samples);
        for (int i = 1; i <= samples; ++i) {
            const double t = static_cast<double>(i) / samples;
            QPointF point = segmentPoint(segment, t);
            if (segment.curved && i < samples) {
                const QPointF tangent = (segment.control - segment.start) * (2.0 * (1.0 - t))
                    + (segment.end - segment.control) * (2.0 * t);
                const double tangentLength = std::hypot(tangent.x(), tangent.y());
                if (tangentLength > kEpsilon) {
                    const QPointF inward(-tangent.y() * orientationSign / tangentLength,
                                         tangent.x() * orientationSign / tangentLength);
                    const double inset = std::hypot(localSagitta.x(), localSagitta.y()) * 2.1
                        + kEpsilon;
                    point += inward * inset;
                }
            }
            result.push_back(point);
        }
    }
    return result;
}

QPainterPath polygonPath(const QPolygonF &polygon)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    if (!polygon.isEmpty()) {
        path.addPolygon(polygon);
        path.closeSubpath();
    }
    return path;
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
        for (int i = 0; i + 1 < polygon.size(); ++i) {
            best = std::min(best, distanceSquaredToSegment(point, polygon[i], polygon[i + 1]));
        }
        if (polygon.size() > 2 && polygon.front() != polygon.back()) {
            best = std::min(best, distanceSquaredToSegment(point, polygon.back(), polygon.front()));
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
    const double t = cross(c - a, s) / denominator;
    if (intersection != nullptr) {
        *intersection = a + r * t;
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
            const bool adjacentSegments = std::abs(a.segment - b.segment) == 1
                || (a.segment == 0 && b.segment == lastSegment)
                || (b.segment == 0 && a.segment == lastSegment);
            if (adjacentSegments) {
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
            bool duplicate = false;
            for (const QPointF &existing : crossings) {
                if (QLineF(existing, point).length() <= tolerance * 2.0) {
                    duplicate = true;
                    break;
                }
            }
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

QTransform edgeTransform(const QRectF &bounds,
                         const QPointF &start,
                         const QPointF &end,
                         const QPointF &normal,
                         double depth)
{
    const QPointF delta = end - start;
    const double width = std::max(bounds.width(), kEpsilon);
    const double height = std::max(bounds.height(), kEpsilon);
    const double m11 = delta.x() / width;
    const double m12 = delta.y() / width;
    const double m21 = normal.x() * depth / height;
    const double m22 = normal.y() * depth / height;
    const double dx = start.x() - m11 * bounds.left() - m21 * bounds.top();
    const double dy = start.y() - m12 * bounds.left() - m22 * bounds.top();
    return QTransform(m11, m12, m21, m22, dx, dy);
}

QTransform centeredTransform(const QRectF &bounds,
                             const QPointF &center,
                             double rotationDegrees,
                             double scaleX,
                             double scaleY);

bool pathContained(const QPainterPath &candidate,
                   const QPainterPath &container,
                   const QVector<QPolygonF> &containerPolygons,
                   double tolerance)
{
    if (candidate.isEmpty()) {
        return false;
    }
    const QRectF candidateBounds = candidate.boundingRect();
    if (!container.boundingRect().adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(candidateBounds)) {
        return false;
    }
    const QVector<QPolygonF> candidatePolygons = candidate.toSubpathPolygons();
    for (const QPolygonF &polygon : candidatePolygons) {
        for (const QPointF &point : polygon) {
            if (!container.contains(point) && distanceToPolygons(point, containerPolygons) > tolerance) {
                return false;
            }
        }
    }
    const QPainterPath outside = candidate.subtracted(container);
    if (!outside.isEmpty()) {
        const double numericalArea = std::max(1e-10, pathArea(candidate) * 1e-9);
        if (pathArea(outside) > numericalArea) {
            return false;
        }
    }
    return true;
}

struct MatchScore {
    bool full = false;
    double length = 0.0;
};

MatchScore boundaryMatch(const PenBoundarySegment &segment,
                         const QPainterPath &candidate,
                         double tolerance)
{
    const QVector<QPolygonF> boundaries = candidate.toSubpathPolygons();
    const QVector<QPointF> samples = sampleSegment(segment, kCurveSamples);
    MatchScore score;
    score.full = true;
    double run = 0.0;
    double bestRun = 0.0;
    bool previousMatched = distanceToPolygons(samples.front(), boundaries) <= tolerance;
    score.full = previousMatched;
    for (int i = 1; i < samples.size(); ++i) {
        const bool matched = distanceToPolygons(samples[i], boundaries) <= tolerance;
        score.full = score.full && matched;
        const double length = QLineF(samples[i - 1], samples[i]).length();
        if (matched && previousMatched) {
            run += length;
            bestRun = std::max(bestRun, run);
        } else {
            run = 0.0;
        }
        previousMatched = matched;
    }
    score.length = bestRun;
    return score;
}

bool betterBoundaryPlacement(const PenPlacement &candidate, const PenPlacement &best)
{
    if (candidate.fullBoundaryMatch != best.fullBoundaryMatch) {
        return candidate.fullBoundaryMatch;
    }
    if (candidate.fullBoundaryMatch && std::abs(candidate.area - best.area) > kEpsilon) {
        return candidate.area > best.area;
    }
    if (!candidate.fullBoundaryMatch
        && std::abs(candidate.matchedLength - best.matchedLength) > kEpsilon) {
        return candidate.matchedLength > best.matchedLength;
    }
    if (std::abs(candidate.area - best.area) > kEpsilon) {
        return candidate.area > best.area;
    }
    return candidate.shapeId < best.shapeId;
}

void evaluateBoundaryTransform(const PenPrimitive &primitive,
                               const PenBoundarySegment &segment,
                               const QTransform &transform,
                               const QPainterPath &container,
                               const QVector<QPolygonF> &containerPolygons,
                               double containmentTolerance,
                               double matchTolerance,
                               PenPlacement *best)
{
    if (!transform.isAffine() || std::abs(transform.determinant()) <= kEpsilon) {
        return;
    }
    const QPainterPath candidate = transform.map(primitive.silhouette);
    if (!pathContained(candidate, container, containerPolygons, containmentTolerance)) {
        return;
    }
    const MatchScore match = boundaryMatch(segment, candidate, matchTolerance);
    PenPlacement placement;
    placement.shapeId = primitive.shapeId;
    placement.transform = transform;
    placement.area = primitive.area * std::abs(transform.determinant());
    placement.matchedLength = match.length;
    placement.fullBoundaryMatch = match.full;
    if (best->shapeId == 0 || betterBoundaryPlacement(placement, *best)) {
        *best = placement;
    }
}

PenPlacement bestBoundaryPlacement(const PenBoundarySegment &segment,
                                   const QVector<PenPrimitive> &primitives,
                                   const QPainterPath &container,
                                   double boundaryTolerance,
                                   const std::function<bool()> &cancelled)
{
    PenPlacement best;
    const double containmentTolerance = std::max(1e-6, boundaryTolerance * 0.01);
    const double matchTolerance = std::max(1e-5, boundaryTolerance * 0.125);
    const QPointF chord = segment.end - segment.start;
    const double chordLength = std::max(QLineF(segment.start, segment.end).length(),
                                        boundaryTolerance * 0.01);
    QPointF normal(-chord.y(), chord.x());
    const double normalLength = std::hypot(normal.x(), normal.y());
    if (normalLength > kEpsilon) {
        normal /= normalLength;
    }

    const QPointF middle = segmentPoint(segment, 0.5);
    const QVector<QPolygonF> containerPolygons = container.toSubpathPolygons();
    for (const PenPrimitive &primitive : primitives) {
        if (cancelled && cancelled()) {
            return {};
        }

        for (double sign : {-1.0, 1.0}) {
            const QPointF direction = normal * sign;
            const std::array<double, 12> depthFactors = {
                0.01, 0.02, 0.04, 0.08, 0.16, 0.32,
                0.64, 1.0, 1.5, 2.0, 3.0, 4.0,
            };
            for (double factor : depthFactors) {
                evaluateBoundaryTransform(primitive,
                                          segment,
                                          edgeTransform(primitive.bounds,
                                                        segment.start + direction * containmentTolerance,
                                                        segment.end + direction * containmentTolerance,
                                                        direction,
                                                        chordLength * factor),
                                          container,
                                          containerPolygons,
                                          containmentTolerance,
                                          matchTolerance,
                                          &best);
            }
        }

        if (!segment.curved) {
            continue;
        }
        for (const QPolygonF &rawPolygon : primitive.contours) {
            QPolygonF polygon = rawPolygon;
            if (polygon.size() > 1 && polygon.front() == polygon.back()) {
                polygon.removeLast();
            }
            if (polygon.size() < 3) {
                continue;
            }
            const int sampleCount = std::min(16, static_cast<int>(polygon.size()));
            QVector<QPointF> sampled;
            sampled.reserve(sampleCount);
            for (int i = 0; i < sampleCount; ++i) {
                sampled.push_back(polygon[(i * polygon.size()) / sampleCount]);
            }
            for (int startIndex = 0; startIndex < sampled.size(); ++startIndex) {
                for (int direction : {-1, 1}) {
                    for (int span = 2; span < sampled.size(); ++span) {
                        const QPointF a = sampled[startIndex];
                        const QPointF b = sampled[(startIndex + direction * (span / 2) + sampled.size() * 4) % sampled.size()];
                        const QPointF c = sampled[(startIndex + direction * span + sampled.size() * 4) % sampled.size()];
                        bool ok = false;
                        const QTransform transform = affineFromTriangles(a,
                                                                         b,
                                                                         c,
                                                                         segment.start,
                                                                         middle,
                                                                         segment.end,
                                                                         &ok);
                        if (ok) {
                            evaluateBoundaryTransform(primitive,
                                                      segment,
                                                      transform,
                                                      container,
                                                      containerPolygons,
                                                      containmentTolerance,
                                                      matchTolerance,
                                                      &best);
                        }
                    }
                }
            }
        }
    }
    if (best.shapeId == 0 && normalLength > kEpsilon) {
        const std::array<double, 7> radiusFactors = {0.08, 0.04, 0.02, 0.01, 0.005, 0.001, 0.0001};
        for (double radiusFactor : radiusFactors) {
            const double radius = std::max(chordLength * radiusFactor, containmentTolerance * 2.0);
            for (double sign : {-1.0, 1.0}) {
                const QPointF center = middle + normal * sign * (radius + containmentTolerance);
                for (const PenPrimitive &primitive : primitives) {
                    const double extent = std::max(primitive.bounds.width(), primitive.bounds.height());
                    if (extent <= kEpsilon) {
                        continue;
                    }
                    const double scale = radius * 2.0 / extent;
                    for (int angle : {0, 45, 90, 135}) {
                        evaluateBoundaryTransform(primitive,
                                                  segment,
                                                  centeredTransform(primitive.bounds,
                                                                    center,
                                                                    angle,
                                                                    scale,
                                                                    scale),
                                                  container,
                                                  containerPolygons,
                                                  containmentTolerance,
                                                  matchTolerance,
                                                  &best);
                    }
                }
            }
            if (best.shapeId != 0) {
                break;
            }
        }
    }
    return best;
}

QTransform centeredTransform(const QRectF &bounds,
                             const QPointF &center,
                             double rotationDegrees,
                             double scaleX,
                             double scaleY)
{
    QTransform result;
    result.translate(center.x(), center.y());
    result.rotate(rotationDegrees);
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
    hull.reserve(points.size() * 2);
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
        const QRectF rect(-geometry.width * 0.5,
                          -geometry.height * 0.5,
                          geometry.width,
                          geometry.height);
        result.silhouette.addRect(rect);
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
    primitives.reserve(3);
    for (const PenPrimitive &primitive : request.primitives) {
        if (isAllowedPenShape(primitive.shapeId)) {
            primitives.push_back(primitive);
        }
    }
    const PolygonMeshSources meshSources = penMeshSources(primitives);
    if (primitives.isEmpty() || meshSources.triangle.size() != 3) {
        result.error = QStringLiteral("Triangle geometry is unavailable for the Pen interior mesh");
        return result;
    }
    const PenContour contour = buildPenContour(request.points, request.boundaryTolerance * 0.25);
    if (!contour.valid()) {
        result.error = contour.error.isEmpty() ? QStringLiteral("Invalid Pen contour") : contour.error;
        return result;
    }

    QPainterPath filled;
    filled.setFillRule(Qt::WindingFill);
    for (const PenBoundarySegment &segment : contour.segments) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        PenPlacement placement = bestBoundaryPlacement(segment,
                                                       primitives,
                                                       contour.path,
                                                       request.boundaryTolerance,
                                                       cancelled);
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        if (placement.shapeId == 0) {
            result.error = QStringLiteral("No contained Primitive fits boundary segment %1")
                               .arg(result.boundaryPlacementCount + 1);
            return result;
        }
        result.placements.push_back(placement);
        ++result.boundaryPlacementCount;
        const PenPrimitive *primitive = primitiveForId(primitives, placement.shapeId);
        if (primitive != nullptr) {
            filled = filled.united(placement.transform.map(primitive->silhouette));
        }
    }

    // Subtract the chosen boundary shapes before meshing. Each remaining polygon
    // is ear-clipped once, so this accounts for current coverage without bringing
    // back the old iterative residual-fitting loop.
    const QPolygonF fillPolygon = interiorMeshPolygon(contour);
    const QPainterPath meshDomain = polygonPath(fillPolygon);
    const QPainterPath residual = meshDomain.subtracted(filled);
    const double contourArea = pathArea(contour.path);
    const double numericalArea = std::max(1e-8, contourArea * 1e-8);
    QVector<PolygonMeshPlacement> meshPlacements;
    const auto meshPolygons = [&](const QVector<QPolygonF> &polygons,
                                  bool outerSubpathsOnly,
                                  QString *error) {
        for (const QPolygonF &residualPolygon : polygons) {
            if (std::abs(signedArea(residualPolygon)) <= numericalArea) {
                continue;
            }
            if (outerSubpathsOnly
                && !residual.contains(polygonInteriorSample(residualPolygon))) {
                continue;
            }
            PolygonMeshRequest meshRequest;
            meshRequest.points.reserve(residualPolygon.size());
            for (const QPointF &point : residualPolygon) {
                meshRequest.points.push_back(point);
            }
            meshRequest.sources = meshSources;
            meshRequest.mergeSquares = false;
            const PolygonMeshResult meshResult = meshPolygon(meshRequest, cancelled);
            if (meshResult.cancelled || (cancelled && cancelled())) {
                return false;
            }
            if (!meshResult.error.isEmpty()) {
                if (error != nullptr) {
                    *error = meshResult.error;
                }
                return false;
            }
            meshPlacements += meshResult.placements;
        }
        return true;
    };

    QString meshError;
    if (!meshPolygons(residual.toFillPolygons(), false, &meshError)) {
        if (cancelled && cancelled()) {
            result.placements.clear();
            result.boundaryPlacementCount = 0;
            result.cancelled = true;
            return result;
        }
        // QPainterPath represents holes in fill-polygons with a doubled bridge,
        // which is intentionally not a simple polygon. Mesh outer subpaths in
        // that case; any covered hole is already occupied by a boundary shape.
        meshPlacements.clear();
        meshError.clear();
        if (!meshPolygons(residual.toSubpathPolygons(), true, &meshError)) {
            result.error = QStringLiteral("Could not mesh the Pen interior: %1")
                               .arg(meshError);
            return result;
        }
    }

    QPainterPath coverage = filled;
    QPainterPath interiorCoverage;
    interiorCoverage.setFillRule(Qt::WindingFill);
    const bool hasCurves = std::any_of(contour.segments.begin(), contour.segments.end(), [](const auto &segment) {
        return segment.curved;
    });
    for (const PolygonMeshPlacement &meshPlacement : meshPlacements) {
        if (cancelled && cancelled()) {
            result.placements.clear();
            result.boundaryPlacementCount = 0;
            result.cancelled = true;
            return result;
        }
        const PenPrimitive *primitive = primitiveForId(primitives, meshPlacement.shapeId);
        if (primitive == nullptr) {
            result.error = QStringLiteral("Pen mesh selected unavailable Primitive %1")
                               .arg(meshPlacement.shapeId);
            return result;
        }
        const QPainterPath placedPath = meshPlacement.transform.map(primitive->silhouette);
        const double placedArea = pathArea(placedPath);
        const double newArea = pathArea(placedPath.subtracted(coverage));
        if (newArea <= std::max(1e-12, placedArea * 1e-10)) {
            continue;
        }
        PenPlacement placement;
        placement.shapeId = meshPlacement.shapeId;
        placement.transform = meshPlacement.transform;
        placement.area = primitive->area * std::abs(meshPlacement.transform.determinant());
        result.placements.push_back(placement);
        interiorCoverage = interiorCoverage.united(placedPath);
        coverage = coverage.united(placedPath);
    }

    const double outsideArea = pathArea(interiorCoverage.subtracted(contour.path));
    // QPainterPath re-flattens quadratics for Boolean operations. Comparing each
    // dense mesh triangle (or their union) back to that independently flattened
    // path produces false overhangs even though the mesh came from the inset
    // sampled contour. Exact validation remains useful for all-hard paths.
    if (!hasCurves && outsideArea > numericalArea) {
        result.error = QStringLiteral("The generated Pen mesh extended beyond its contour");
        return result;
    }
    result.unfilled = contour.path.subtracted(coverage);
    if (!hasCurves && pathArea(result.unfilled) > numericalArea) {
        result.error = QStringLiteral("The generated Pen mesh did not fill its contour");
    }
    return result;
}

} // namespace gui
