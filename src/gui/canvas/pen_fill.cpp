#include "pen_fill.h"
#include "polygon_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace gui {
namespace {

constexpr double kEpsilon = 1e-8;
constexpr int kCurveSamples = 32;
constexpr int kSquareShapeId = 101;
constexpr int kCircleShapeId = 102;
constexpr int kTriangleShapeId = 103;
constexpr int kHalfCircleShapeId = 109;
constexpr int kFangShapeId = 127;
constexpr int kConcaveArcShapeId = 129;
constexpr int kQuarterCircleShapeId = 130;
constexpr int kGarlicShapeId = 139;
constexpr int kToothShapeId = 2123;
constexpr double kConcaveArcInnerRadiusRatio = 0.65;
constexpr double kMaximumCurveOutsideRatio = 1e-5;
constexpr double kMaximumTangentJump = 0.35;
constexpr double kMaximumCurvatureJump = 0.75;
constexpr double kMaximumSpanErrorRatio = 0.05;
constexpr int kSpanSamples = 64;
constexpr int kMaximumSpanEvaluations = 2048;

int curveEvaluationBudget(int segmentCount) {
    if (segmentCount > 384) {
        return 4;
    }
    if (segmentCount > 192) {
        return 8;
    }
    if (segmentCount > 96) {
        return 16;
    }
    if (segmentCount > 48) {
        return 32;
    }
    return kMaximumSpanEvaluations;
}

bool isAllowedPenShape(int shapeId) {
    return shapeId == kSquareShapeId
        || shapeId == kCircleShapeId
        || shapeId == kTriangleShapeId
        || shapeId == kHalfCircleShapeId
        || shapeId == kFangShapeId
        || shapeId == kConcaveArcShapeId
        || shapeId == kQuarterCircleShapeId
        || shapeId == kGarlicShapeId
        || shapeId == kToothShapeId;
}

bool isCurveShape(int shapeId) {
    return shapeId == kCircleShapeId
        || shapeId == kHalfCircleShapeId
        || shapeId == kFangShapeId
        || shapeId == kConcaveArcShapeId
        || shapeId == kQuarterCircleShapeId
        || shapeId == kGarlicShapeId
        || shapeId == kToothShapeId;
}

double cross(const QPointF &a, const QPointF &b) {
    return a.x() * b.y() - a.y() * b.x();
}

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        result += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    }
    return result * 0.5;
}

double pathArea(const QPainterPath &path) {
    double result = 0.0;
    for (const QPolygonF &polygon : path.simplified().toSubpathPolygons()) {
        result += signedArea(polygon);
    }
    return std::abs(result);
}

QPointF segmentPoint(const PenBoundarySegment &segment, double t) {
    if (!segment.curved) {
        return segment.start * (1.0 - t) + segment.end * t;
    }
    const double u = 1.0 - t;
    return segment.start * (u * u)
        + segment.control * (2.0 * u * t)
        + segment.end * (t * t);
}

QVector<QPointF> sampleSegment(const PenBoundarySegment &segment, int samples) {
    QVector<QPointF> result;
    result.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        result.push_back(segmentPoint(segment, static_cast<double>(i) / samples));
    }
    return result;
}

QPointF segmentDerivative(const PenBoundarySegment &segment, double t) {
    if (!segment.curved) {
        return segment.end - segment.start;
    }
    return (segment.control - segment.start) * (2.0 * (1.0 - t))
        + (segment.end - segment.control) * (2.0 * t);
}

double segmentCurvature(const PenBoundarySegment &segment, double t) {
    if (!segment.curved) {
        return 0.0;
    }
    const QPointF derivative = segmentDerivative(segment, t);
    const double speed = std::hypot(derivative.x(), derivative.y());
    if (speed <= kEpsilon) {
        return 0.0;
    }
    const QPointF second = (segment.end - segment.control * 2.0 + segment.start) * 2.0;
    return cross(derivative, second) / (speed * speed * speed);
}

bool isOutwardCurve(const PenBoundarySegment &segment, double orientationSign) {
    if (!segment.curved) {
        return false;
    }
    const QPointF chord = segment.end - segment.start;
    const QPointF chordMiddle = (segment.start + segment.end) * 0.5;
    const QPointF curveMiddle = segmentPoint(segment, 0.5);
    return cross(chord, curveMiddle - chordMiddle) * orientationSign < -kEpsilon;
}

double junctionSeparation(const PenBoundarySegment &left,
                          const PenBoundarySegment &right) {
    const QPointF incoming = segmentDerivative(left, 1.0);
    const QPointF outgoing = segmentDerivative(right, 0.0);
    const double incomingLength = std::hypot(incoming.x(), incoming.y());
    const double outgoingLength = std::hypot(outgoing.x(), outgoing.y());
    if (incomingLength <= kEpsilon || outgoingLength <= kEpsilon) {
        return std::numeric_limits<double>::max();
    }
    const double cosine = std::clamp(QPointF::dotProduct(incoming, outgoing)
                                         / (incomingLength * outgoingLength),
                                     -1.0,
                                     1.0);
    const double tangentJump = std::acos(cosine);
    const double leftCurvature = segmentCurvature(left, 1.0);
    const double rightCurvature = segmentCurvature(right, 0.0);
    const double curvatureScale = std::max({std::abs(leftCurvature),
                                            std::abs(rightCurvature),
                                            1.0 / std::max(1.0, (incomingLength + outgoingLength) * 0.5)});
    const double curvatureJump = std::abs(leftCurvature - rightCurvature) / curvatureScale;
    return tangentJump / kMaximumTangentJump
        + curvatureJump / kMaximumCurvatureJump;
}

bool curvatureBreak(const PenBoundarySegment &left,
                    const PenBoundarySegment &right) {
    const QPointF incoming = segmentDerivative(left, 1.0);
    const QPointF outgoing = segmentDerivative(right, 0.0);
    const double incomingLength = std::hypot(incoming.x(), incoming.y());
    const double outgoingLength = std::hypot(outgoing.x(), outgoing.y());
    if (incomingLength <= kEpsilon || outgoingLength <= kEpsilon) {
        return true;
    }
    const double cosine = std::clamp(QPointF::dotProduct(incoming, outgoing)
                                         / (incomingLength * outgoingLength),
                                     -1.0,
                                     1.0);
    if (std::acos(cosine) > kMaximumTangentJump) {
        return true;
    }
    const double leftCurvature = segmentCurvature(left, 1.0);
    const double rightCurvature = segmentCurvature(right, 0.0);
    if ((leftCurvature < -kEpsilon && rightCurvature > kEpsilon)
        || (leftCurvature > kEpsilon && rightCurvature < -kEpsilon)) {
        return true;
    }
    const double curvatureScale = std::max({std::abs(leftCurvature),
                                            std::abs(rightCurvature),
                                            1.0 / std::max(1.0, (incomingLength + outgoingLength) * 0.5)});
    return std::abs(leftCurvature - rightCurvature) / curvatureScale
        > kMaximumCurvatureJump;
}

QVector<QPointF> sampleSegmentSpan(const QVector<PenBoundarySegment> &segments,
                                   int first,
                                   int last) {
    const int segmentCount = last - first + 1;
    const int samplesPerSegment = std::clamp(kSpanSamples / std::max(1, segmentCount), 2, 8);
    QVector<QPointF> result;
    result.reserve(segmentCount * samplesPerSegment + 1);
    result.push_back(segments[first].start);
    for (int i = first; i <= last; ++i) {
        for (int sample = 1; sample <= samplesPerSegment; ++sample) {
            result.push_back(segmentPoint(segments[i],
                                          static_cast<double>(sample) / samplesPerSegment));
        }
    }
    return result;
}

QPolygonF flattenedContour(const PenContour &contour, int curvedSamples) {
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

double distanceSquaredToSegment(const QPointF &point, const QPointF &a, const QPointF &b) {
    const QPointF ab = b - a;
    const double length2 = QPointF::dotProduct(ab, ab);
    if (length2 <= kEpsilon) {
        return QPointF::dotProduct(point - a, point - a);
    }
    const double t = std::clamp(QPointF::dotProduct(point - a, ab) / length2, 0.0, 1.0);
    const QPointF delta = point - (a + ab * t);
    return QPointF::dotProduct(delta, delta);
}

double distanceToPolygons(const QPointF &point, const QVector<QPolygonF> &polygons) {
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

int orientation(const QPointF &a, const QPointF &b, const QPointF &c, double epsilon) {
    const double value = cross(b - a, c - a);
    if (std::abs(value) <= epsilon) {
        return 0;
    }
    return value > 0.0 ? 1 : -1;
}

bool pointOnSegment(const QPointF &point, const QPointF &a, const QPointF &b, double epsilon) {
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
                      QPointF *intersection) {
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

QVector<QPointF> contourCrossings(const QVector<PenBoundarySegment> &segments, double tolerance) {
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
                               bool *ok) {
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
                             double scaleY) {
    QTransform result;
    result.translate(center.x(), center.y());
    result.scale(scaleX, scaleY);
    result.translate(-bounds.center().x(), -bounds.center().y());
    return result;
}

QPolygonF primitiveHull(const PenPrimitive &primitive) {
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

PolygonMeshSources penMeshSources(const QVector<PenPrimitive> &primitives) {
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

const PenPrimitive *primitiveForId(const QVector<PenPrimitive> &primitives, int shapeId) {
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

struct ArcProfile {
    QPointF start;
    QPointF middle;
    QPointF end;
    QPointF coreMiddle;
};

struct CurvePrimitive {
    const PenPrimitive *primitive = nullptr;
    QVector<ArcProfile> profiles;
};

QPointF supportPoint(const PenPrimitive &primitive, const QPointF &direction) {
    QPointF result;
    double bestProjection = std::numeric_limits<double>::lowest();
    for (const QPolygonF &contour : primitive.contours) {
        for (const QPointF &point : contour) {
            const double projection = QPointF::dotProduct(point, direction);
            if (projection > bestProjection) {
                bestProjection = projection;
                result = point;
            }
        }
    }
    return result;
}

QVector<ArcProfile> primitiveArcProfiles(const PenPrimitive &primitive) {
    QVector<ArcProfile> result;
    const QRectF bounds = primitive.bounds;
    if (primitive.shapeId == kHalfCircleShapeId) {
        result.push_back({{bounds.left(), bounds.bottom()},
                          {bounds.center().x(), bounds.top()},
                          {bounds.right(), bounds.bottom()},
                          {bounds.center().x(), bounds.bottom()}});
        return result;
    }
    const QPointF center(bounds.left(), bounds.bottom());
    if (primitive.shapeId == kQuarterCircleShapeId) {
        const double radiusX = bounds.width();
        const double radiusY = bounds.height();
        result.push_back({{center.x(), center.y() - radiusY},
                          {center.x() + radiusX / std::numbers::sqrt2,
                           center.y() - radiusY / std::numbers::sqrt2},
                          {center.x() + radiusX, center.y()},
                          center});
        return result;
    }
    if (primitive.shapeId == kConcaveArcShapeId) {
        const double innerRadiusX = bounds.width() * kConcaveArcInnerRadiusRatio;
        const double innerRadiusY = bounds.height() * kConcaveArcInnerRadiusRatio;
        result.push_back({{center.x(), center.y() - innerRadiusY},
                          {center.x() + innerRadiusX / std::numbers::sqrt2,
                           center.y() - innerRadiusY / std::numbers::sqrt2},
                          {center.x() + innerRadiusX, center.y()},
                          {center.x() + bounds.width() / std::numbers::sqrt2,
                           center.y() - bounds.height() / std::numbers::sqrt2}});
        return result;
    }
    if (primitive.shapeId != kFangShapeId
        && primitive.shapeId != kGarlicShapeId
        && primitive.shapeId != kToothShapeId) {
        return result;
    }
    constexpr int directions = 4;
    std::optional<ArcProfile> best;
    double bestDepthRatio = -1.0;
    for (int i = 0; i < directions; ++i) {
        const double angle = 2.0 * std::numbers::pi * i / directions;
        const QPointF axis(std::cos(angle), std::sin(angle));
        const QPointF perpendicular(-axis.y(), axis.x());
        ArcProfile profile{supportPoint(primitive, -axis - perpendicular),
                           supportPoint(primitive, -axis),
                           supportPoint(primitive, -axis + perpendicular),
                           supportPoint(primitive, axis)};
        if (std::abs(cross(profile.middle - profile.start,
                           profile.end - profile.start)) <= kEpsilon) {
            continue;
        }
        const QPointF chord = profile.end - profile.start;
        const double chordLength = std::hypot(chord.x(), chord.y());
        const double middleSide = cross(chord, profile.middle - profile.start);
        const double coreSide = cross(chord, profile.coreMiddle - profile.start);
        if (middleSide * coreSide >= 0.0 || chordLength <= kEpsilon) {
            continue;
        }
        const double depthRatio = std::abs(coreSide)
            / std::max(std::abs(middleSide), kEpsilon);
        if (depthRatio > bestDepthRatio) {
            bestDepthRatio = depthRatio;
            best = profile;
        }
    }
    if (best) {
        result.push_back(*best);
        result.push_back({best->end, best->middle, best->start, best->coreMiddle});
    }
    return result;
}

std::optional<QTransform> arcThroughPoints(const ArcProfile &profile,
                                           const QPointF &start,
                                           const QPointF &middle,
                                           const QPointF &end) {
    bool ok = false;
    const QTransform transform = affineFromTriangles(profile.start,
                                                      profile.middle,
                                                      profile.end,
                                                      start,
                                                      middle,
                                                      end,
                                                      &ok);
    return ok ? std::optional<QTransform>(transform) : std::nullopt;
}

QVector<QTransform> arcTransforms(const ArcProfile &profile,
                                  const QPointF &start,
                                  const QPointF &middle,
                                  const QPointF &end,
                                  double maximumInset) {
    const auto base = arcThroughPoints(profile, start, middle, end);
    if (!base) {
        return {};
    }
    QVector<QTransform> result{*base};
    QPointF inward = base->map(profile.coreMiddle) - middle;
    const double inwardLength = std::hypot(inward.x(), inward.y());
    if (inwardLength <= kEpsilon || maximumInset <= kEpsilon) {
        return result;
    }
    inward /= inwardLength;
    for (const double fraction : {0.25, 0.5, 0.75, 1.0}) {
        const QPointF offset = inward * (maximumInset * fraction);
        if (const auto transform = arcThroughPoints(profile,
                                                    start + offset,
                                                    middle + offset,
                                                    end + offset)) {
            result.push_back(*transform);
        }
    }
    return result;
}

std::optional<QTransform> circleThroughPoints(const PenPrimitive &primitive,
                                              const QPointF &a,
                                              const QPointF &b,
                                              const QPointF &c) {
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
                                              bool lowerHalf) {
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

std::optional<QTransform> ellipseLeastSquares(const PenPrimitive &primitive,
                                              const QVector<QPointF> &samples) {
    if (samples.size() < 3) {
        return std::nullopt;
    }
    const QPointF start = samples.front();
    const QPointF end = samples.back();
    if (QLineF(start, end).length() <= kEpsilon) {
        return std::nullopt;
    }
    QVector<double> distances(samples.size(), 0.0);
    for (int i = 1; i < samples.size(); ++i) {
        distances[i] = distances[i - 1] + QLineF(samples[i - 1], samples[i]).length();
    }
    const double totalLength = distances.back();
    if (totalLength <= kEpsilon) {
        return std::nullopt;
    }
    const QPointF center = (start + end) * 0.5;
    const QPointF horizontal = (end - start) * 0.5;
    QPointF vertical;
    double denominator = 0.0;
    for (int i = 1; i + 1 < samples.size(); ++i) {
        const double parameter = distances[i] / totalLength;
        const double x = -std::cos(std::numbers::pi * parameter);
        const double y = std::sin(std::numbers::pi * parameter);
        vertical += (samples[i] - center - horizontal * x) * y;
        denominator += y * y;
    }
    if (denominator <= kEpsilon) {
        return std::nullopt;
    }
    vertical /= denominator;
    const QPointF sourceStart(primitive.bounds.left(), primitive.bounds.center().y());
    const QPointF sourceMiddle(primitive.bounds.center().x(), primitive.bounds.bottom());
    const QPointF sourceEnd(primitive.bounds.right(), primitive.bounds.center().y());
    bool ok = false;
    const QTransform transform = affineFromTriangles(sourceStart,
                                                     sourceMiddle,
                                                     sourceEnd,
                                                     start,
                                                     center + vertical,
                                                     end,
                                                     &ok);
    return ok ? std::optional<QTransform>(transform) : std::nullopt;
}

std::optional<CurvePlacement> evaluateCurvePlacement(const PenPrimitive &primitive,
                                                     const QVector<QPointF> &samples,
                                                     const QTransform &transform,
                                                     const QPainterPath &target,
                                                     double targetArea,
                                                     double maximumBoundaryError = std::numeric_limits<double>::max()) {
    if (!transform.isAffine() || std::abs(transform.determinant()) <= kEpsilon) {
        return std::nullopt;
    }
    CurvePlacement result;
    result.path = transform.map(primitive.silhouette).simplified();
    const double area = pathArea(result.path);
    if (area <= kEpsilon) {
        return std::nullopt;
    }
    const QVector<QPolygonF> boundaries = result.path.toSubpathPolygons();
    for (const QPointF &point : samples) {
        result.boundaryError = std::max(result.boundaryError,
                                        distanceToPolygons(point, boundaries));
        if (result.boundaryError > maximumBoundaryError) {
            return std::nullopt;
        }
    }
    result.insideArea = pathArea(result.path.intersected(target));
    result.outsideArea = std::max(0.0, area - result.insideArea);
    if (targetArea <= kEpsilon
        || result.outsideArea / targetArea > kMaximumCurveOutsideRatio) {
        return std::nullopt;
    }
    result.placement.shapeId = primitive.shapeId;
    result.placement.transform = transform;
    result.placement.area = area;
    return result;
}

bool betterCurvePlacement(const CurvePlacement &candidate, const CurvePlacement &best) {
    if (std::abs(candidate.insideArea - best.insideArea) > kEpsilon) {
        return candidate.insideArea > best.insideArea;
    }
    if (std::abs(candidate.boundaryError - best.boundaryError) > kEpsilon) {
        return candidate.boundaryError < best.boundaryError;
    }
    if (std::abs(candidate.outsideArea - best.outsideArea) > kEpsilon) {
        return candidate.outsideArea < best.outsideArea;
    }
    return candidate.placement.shapeId < best.placement.shapeId;
}

std::optional<CurvePlacement> outwardCurvePlacement(const QVector<CurvePrimitive> &caps,
                                                     const PenBoundarySegment &segment,
                                                     const QPainterPath &target,
                                                     double targetArea,
                                                     double maximumBoundaryError) {
    const QVector<QPointF> samples = sampleSegment(segment, kCurveSamples);
    const QPointF middle = segmentPoint(segment, 0.5);
    std::optional<CurvePlacement> best;
    for (const CurvePrimitive &cap : caps) {
        const PenPrimitive *primitive = cap.primitive;
        QVector<QTransform> transforms;
        if (primitive->shapeId == kCircleShapeId) {
            if (const auto circleTransform = circleThroughPoints(*primitive,
                                                                 segment.start,
                                                                 middle,
                                                                 segment.end)) {
                transforms.push_back(*circleTransform);
            }
            for (bool lowerHalf : {false, true}) {
                if (const auto ellipseTransform = ellipseThroughCurve(*primitive,
                                                                      segment.start,
                                                                      middle,
                                                                      segment.end,
                                                                      lowerHalf)) {
                    transforms.push_back(*ellipseTransform);
                }
            }
        } else {
            for (const ArcProfile &profile : cap.profiles) {
                transforms += arcTransforms(profile,
                                            segment.start,
                                            middle,
                                            segment.end,
                                            maximumBoundaryError);
            }
        }
        for (const QTransform &transform : transforms) {
            const auto candidate = evaluateCurvePlacement(*primitive,
                                                          samples,
                                                          transform,
                                                          target,
                                                          targetArea,
                                                          maximumBoundaryError);
            if (candidate && (!best || betterCurvePlacement(*candidate, *best))) {
                best = candidate;
            }
        }
    }
    return best;
}

bool chordInsideTarget(const QPointF &start,
                       const QPointF &end,
                       const QPainterPath &target) {
    for (int i = 1; i < 8; ++i) {
        const QPointF point = start * (1.0 - static_cast<double>(i) / 8.0)
            + end * (static_cast<double>(i) / 8.0);
        if (!target.contains(point)) {
            return false;
        }
    }
    return true;
}

struct InwardCurvePlacement {
    CurvePlacement curve;
    QPointF coreMiddle;
};

std::optional<InwardCurvePlacement> inwardCurvePlacement(
    const QVector<CurvePrimitive> &arcs,
    const PenBoundarySegment &segment,
    const QPainterPath &target,
    double targetArea,
    double boundaryTolerance) {
    const QPointF middle = segmentPoint(segment, 0.5);
    QPolygonF controlPoints({segment.start, segment.control, segment.end});
    const QRectF bounds = controlPoints.boundingRect();
    const double diagonal = std::hypot(bounds.width(), bounds.height());
    const double maximumError = std::max(boundaryTolerance,
                                         diagonal * kMaximumSpanErrorRatio);
    const QVector<QPointF> samples = sampleSegment(segment, kCurveSamples);
    std::optional<InwardCurvePlacement> best;
    for (const CurvePrimitive &arc : arcs) {
        for (const ArcProfile &profile : arc.profiles) {
            for (const QTransform &transform : arcTransforms(profile,
                                                             segment.start,
                                                             middle,
                                                             segment.end,
                                                             maximumError)) {
                const auto placement = evaluateCurvePlacement(*arc.primitive,
                                                              samples,
                                                              transform,
                                                              target,
                                                              targetArea,
                                                              maximumError);
                if (!placement) {
                    continue;
                }
                const QPointF coreMiddle = transform.map(profile.coreMiddle);
                if (!target.contains(coreMiddle)
                    || !chordInsideTarget(segment.start, coreMiddle, target)
                    || !chordInsideTarget(coreMiddle, segment.end, target)) {
                    continue;
                }
                InwardCurvePlacement candidate{*placement, coreMiddle};
                if (!best || betterCurvePlacement(candidate.curve, best->curve)) {
                    best = std::move(candidate);
                }
            }
        }
    }
    return best;
}

std::optional<CurvePlacement> outwardSpanPlacement(const QVector<CurvePrimitive> &caps,
                                                    const QVector<PenBoundarySegment> &segments,
                                                   int first,
                                                   int last,
                                                   const QPainterPath &target,
                                                   double targetArea,
                                                   double maximumBoundaryError) {
    const QVector<QPointF> samples = sampleSegmentSpan(segments, first, last);
    if (!chordInsideTarget(samples.front(), samples.back(), target)) {
        return std::nullopt;
    }
    const QPointF middle = samples[samples.size() / 2];
    const QPointF chordMiddle = (samples.front() + samples.back()) * 0.5;
    std::optional<CurvePlacement> best;
    for (const CurvePrimitive &cap : caps) {
        const PenPrimitive *primitive = cap.primitive;
        QVector<QTransform> transforms;
        if (primitive->shapeId == kCircleShapeId) {
            if (const auto fit = ellipseLeastSquares(*primitive, samples)) {
                transforms.push_back(*fit);
            }
            if (const auto circleTransform = circleThroughPoints(*primitive,
                                                                 samples.front(),
                                                                 middle,
                                                                 samples.back())) {
                transforms.push_back(*circleTransform);
            }
            for (double scale : {0.80, 0.85, 0.90, 0.95, 1.0, 1.05}) {
                const QPointF apex = chordMiddle + (middle - chordMiddle) * scale;
                if (const auto ellipseTransform = ellipseThroughCurve(*primitive,
                                                                      samples.front(),
                                                                      apex,
                                                                      samples.back(),
                                                                      false)) {
                    transforms.push_back(*ellipseTransform);
                }
            }
        } else {
            for (const ArcProfile &profile : cap.profiles) {
                transforms += arcTransforms(profile,
                                            samples.front(),
                                            middle,
                                            samples.back(),
                                            maximumBoundaryError);
            }
        }
        for (const QTransform &transform : transforms) {
            const auto candidate = evaluateCurvePlacement(*primitive,
                                                          samples,
                                                          transform,
                                                          target,
                                                          targetArea,
                                                          maximumBoundaryError);
            if (candidate && (!best || betterCurvePlacement(*candidate, *best))) {
                best = candidate;
            }
        }
    }
    return best;
}

QVector<PenBoundarySegment> curvatureOrderedSegments(const QVector<PenBoundarySegment> &segments,
                                                     double orientationSign) {
    if (segments.size() < 2) {
        return segments;
    }
    int seam = 0;
    double bestSeparation = -1.0;
    for (int i = 0; i < segments.size(); ++i) {
        const PenBoundarySegment &left = segments[(i + segments.size() - 1) % segments.size()];
        const PenBoundarySegment &right = segments[i];
        double separation = junctionSeparation(left, right);
        if (!isOutwardCurve(left, orientationSign)
            || !isOutwardCurve(right, orientationSign)) {
            separation += 1000.0;
        }
        if (separation > bestSeparation) {
            bestSeparation = separation;
            seam = i;
        }
    }
    QVector<PenBoundarySegment> result;
    result.reserve(segments.size());
    for (int i = 0; i < segments.size(); ++i) {
        result.push_back(segments[(seam + i) % segments.size()]);
    }
    return result;
}

struct CurveSpanPlacement {
    int first = 0;
    int last = 0;
    CurvePlacement curve;
};

struct CoreLayout {
    QVector<QPointF> points;
    QVector<int> spanAtStart;
};

enum class CoreFitKind {
    None,
    Span,
    InwardCurve,
};

struct SpanSelectionCost {
    bool valid = false;
    int fallbackSegments = 0;
    int placementCount = 0;
    double boundaryError = 0.0;
    double outsideArea = 0.0;
    int decision = -1;
};

bool betterSpanSelection(const SpanSelectionCost &candidate,
                         const SpanSelectionCost &best) {
    if (!best.valid) {
        return true;
    }
    if (candidate.fallbackSegments != best.fallbackSegments) {
        return candidate.fallbackSegments < best.fallbackSegments;
    }
    if (candidate.placementCount != best.placementCount) {
        return candidate.placementCount < best.placementCount;
    }
    if (std::abs(candidate.boundaryError - best.boundaryError) > kEpsilon) {
        return candidate.boundaryError < best.boundaryError;
    }
    return candidate.outsideArea < best.outsideArea;
}

QVector<CurveSpanPlacement> selectCurveSpans(const QVector<CurvePrimitive> &caps,
                                              const QVector<PenBoundarySegment> &segments,
                                             const QPainterPath &target,
                                             double targetArea,
                                             double orientationSign,
                                             double boundaryTolerance,
                                             const std::function<bool()> &cancelled,
                                             bool *wasCancelled) {
    const int count = segments.size();
    QVector<bool> outward(count, false);
    QVector<CurveSpanPlacement> candidates;
    QVector<QVector<int>> candidatesAt(count);
    QVector<int> outwardIndices;
    for (int i = 0; i < count; ++i) {
        outward[i] = isOutwardCurve(segments[i], orientationSign);
        if (outward[i]) {
            outwardIndices.push_back(i);
        }
    }
    std::sort(outwardIndices.begin(), outwardIndices.end(), [&](int left, int right) {
        const PenBoundarySegment &a = segments[left];
        const PenBoundarySegment &b = segments[right];
        const double aChord = QLineF(a.start, a.end).length();
        const double bChord = QLineF(b.start, b.end).length();
        const double aBow = std::sqrt(distanceSquaredToSegment(a.control, a.start, a.end));
        const double bBow = std::sqrt(distanceSquaredToSegment(b.control, b.start, b.end));
        const double aImportance = aChord * std::max(aBow, boundaryTolerance * 0.25);
        const double bImportance = bChord * std::max(bBow, boundaryTolerance * 0.25);
        if (std::abs(aImportance - bImportance) > kEpsilon) {
            return aImportance > bImportance;
        }
        return left < right;
    });
    const int maximumEvaluations = curveEvaluationBudget(count);
    const int maximumSingleEvaluations = count > 96
        ? maximumEvaluations * 2 / 3
        : maximumEvaluations;
    int evaluations = 0;
    for (const int i : std::as_const(outwardIndices)) {
        if (evaluations >= maximumSingleEvaluations) {
            break;
        }
        if (cancelled && cancelled()) {
            *wasCancelled = true;
            return {};
        }
        QPolygonF controlPoints({segments[i].start,
                                 segments[i].control,
                                 segments[i].end});
        const QRectF bounds = controlPoints.boundingRect();
        const double diagonal = std::hypot(bounds.width(), bounds.height());
        const double maximumError = std::max(boundaryTolerance,
                                             diagonal * kMaximumSpanErrorRatio);
        ++evaluations;
        if (const auto placement = outwardCurvePlacement(caps,
                                                         segments[i],
                                                         target,
                                                         targetArea,
                                                         maximumError)) {
            candidatesAt[i].push_back(candidates.size());
            candidates.push_back({i, i, *placement});
        }
    }
    int runStart = 0;
    while (runStart < count) {
        while (runStart < count && !outward[runStart]) {
            ++runStart;
        }
        if (runStart >= count) {
            break;
        }
        int runEnd = runStart;
        while (runEnd + 1 < count
               && outward[runEnd + 1]
               && !curvatureBreak(segments[runEnd], segments[runEnd + 1])) {
            ++runEnd;
        }
        const int runLength = runEnd - runStart + 1;
        const int maximumSpanLength = std::min(runLength, count - 2);
        bool acceptedWholeRun = false;
        for (int spanLength = maximumSpanLength;
             spanLength >= 2 && evaluations < maximumEvaluations;
             --spanLength) {
            for (int first = runStart;
                 first + spanLength - 1 <= runEnd && evaluations < maximumEvaluations;
                 ++first) {
                if (cancelled && cancelled()) {
                    *wasCancelled = true;
                    return {};
                }
                const int last = first + spanLength - 1;
                QPolygonF spanControlPoints;
                for (int i = first; i <= last; ++i) {
                    spanControlPoints.push_back(segments[i].start);
                    spanControlPoints.push_back(segments[i].control);
                    spanControlPoints.push_back(segments[i].end);
                }
                const QRectF bounds = spanControlPoints.boundingRect();
                const double diagonal = std::hypot(bounds.width(), bounds.height());
                const double maximumError = std::max(boundaryTolerance,
                                                     diagonal * kMaximumSpanErrorRatio);
                ++evaluations;
                const auto placement = outwardSpanPlacement(caps,
                                                             segments,
                                                             first,
                                                             last,
                                                             target,
                                                             targetArea,
                                                             maximumError);
                if (!placement) {
                    continue;
                }
                candidatesAt[first].push_back(candidates.size());
                candidates.push_back({first, last, *placement});
                acceptedWholeRun = first == runStart && last == runEnd;
            }
            if (acceptedWholeRun) {
                break;
            }
        }
        runStart = runEnd + 1;
    }
    QVector<SpanSelectionCost> costs(count + 1);
    costs[count].valid = true;
    for (int i = count - 1; i >= 0; --i) {
        SpanSelectionCost best = costs[i + 1];
        best.fallbackSegments += outward[i] ? 1 : 0;
        best.decision = -1;
        for (int candidateIndex : candidatesAt[i]) {
            const CurveSpanPlacement &candidate = candidates[candidateIndex];
            SpanSelectionCost cost = costs[candidate.last + 1];
            if (!cost.valid) {
                continue;
            }
            ++cost.placementCount;
            cost.boundaryError += candidate.curve.boundaryError;
            cost.outsideArea += candidate.curve.outsideArea;
            cost.decision = candidateIndex;
            if (betterSpanSelection(cost, best)) {
                best = cost;
            }
        }
        best.valid = true;
        costs[i] = best;
    }
    QVector<CurveSpanPlacement> selected;
    for (int i = 0; i < count;) {
        const int decision = costs[i].decision;
        if (decision < 0) {
            ++i;
            continue;
        }
        selected.push_back(candidates[decision]);
        i = candidates[decision].last + 1;
    }
    return selected;
}

} // namespace

PenContour buildPenContour(const QVector<PenPoint> &points, double flatnessTolerance) {
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

PenPrimitive buildPenPrimitive(int shapeId, const ShapeGeometry &geometry) {
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
                                               int lastShapeId) {
    QVector<PenPrimitive> result;
    for (int shapeId = firstShapeId; shapeId <= lastShapeId; ++shapeId) {
        if (!isAllowedPenShape(shapeId)) {
            continue;
        }
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
                         const std::function<bool()> &cancelled) {
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
    QVector<CurvePrimitive> outwardCaps;
    QVector<CurvePrimitive> inwardCaps;
    for (const PenPrimitive &primitive : primitives) {
        if (!isCurveShape(primitive.shapeId)) {
            continue;
        }
        CurvePrimitive candidate{&primitive, primitiveArcProfiles(primitive)};
        outwardCaps.push_back(candidate);
        if (primitive.shapeId == kFangShapeId
            || primitive.shapeId == kConcaveArcShapeId
            || primitive.shapeId == kGarlicShapeId
            || primitive.shapeId == kToothShapeId) {
            inwardCaps.push_back(std::move(candidate));
        }
    }
    if (!meshSources.valid() || outwardCaps.isEmpty() || inwardCaps.isEmpty()) {
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
    const QVector<PenBoundarySegment> segments = curvatureOrderedSegments(contour.segments,
                                                                          orientationSign);
    bool selectionCancelled = false;
    const QVector<CurveSpanPlacement> curveSpans = selectCurveSpans(outwardCaps,
                                                                    segments,
                                                                   contour.path,
                                                                   result.targetArea,
                                                                   orientationSign,
                                                                   request.boundaryTolerance,
                                                                   cancelled,
                                                                   &selectionCancelled);
    if (selectionCancelled) {
        result.cancelled = true;
        result.error = QStringLiteral("Pen curve-span selection timed out");
        return result;
    }
    QVector<bool> spanCovered(segments.size(), false);
    for (const CurveSpanPlacement &span : curveSpans) {
        for (int i = span.first; i <= span.last; ++i) {
            spanCovered[i] = true;
        }
    }
    QVector<std::optional<InwardCurvePlacement>> inwardCurves(segments.size());
    if (!inwardCaps.isEmpty()) {
        QVector<int> inwardCandidates;
        for (int i = 0; i < segments.size(); ++i) {
            if (segments[i].curved
                && !spanCovered[i]
                && !isOutwardCurve(segments[i], orientationSign)) {
                inwardCandidates.push_back(i);
            }
        }
        std::sort(inwardCandidates.begin(), inwardCandidates.end(), [&](int left, int right) {
            const PenBoundarySegment &a = segments[left];
            const PenBoundarySegment &b = segments[right];
            const double aImportance = QLineF(a.start, a.end).length()
                * std::max(std::sqrt(distanceSquaredToSegment(a.control, a.start, a.end)),
                           request.boundaryTolerance * 0.25);
            const double bImportance = QLineF(b.start, b.end).length()
                * std::max(std::sqrt(distanceSquaredToSegment(b.control, b.start, b.end)),
                           request.boundaryTolerance * 0.25);
            if (std::abs(aImportance - bImportance) > kEpsilon) {
                return aImportance > bImportance;
            }
            return left < right;
        });
        const int maximumInwardEvaluations = curveEvaluationBudget(segments.size());
        for (int candidateIndex = 0;
             candidateIndex < std::min(maximumInwardEvaluations,
                                       static_cast<int>(inwardCandidates.size()));
             ++candidateIndex) {
            if (cancelled && cancelled()) {
                result.cancelled = true;
                result.error = QStringLiteral("Pen inward-curve selection timed out");
                return result;
            }
            const int i = inwardCandidates[candidateIndex];
            inwardCurves[i] = inwardCurvePlacement(inwardCaps,
                                                   segments[i],
                                                   contour.path,
                                                   result.targetArea,
                                                   request.boundaryTolerance);
        }
    }
    QVector<bool> activeSpans(curveSpans.size(), true);
    QVector<bool> activeInwardCurves(segments.size(), false);
    for (int i = 0; i < inwardCurves.size(); ++i) {
        activeInwardCurves[i] = inwardCurves[i].has_value();
    }
    const auto coreLayout = [&](const QVector<bool> &enabledSpans,
                                const QVector<bool> &enabledInwardCurves) {
        CoreLayout layout;
        layout.spanAtStart.fill(-1, segments.size());
        QVector<bool> activeSpanCovered(segments.size(), false);
        int activeSpanCount = 0;
        int activeInwardCount = 0;
        int activeRemovedVertices = 0;
        for (int spanIndex = 0; spanIndex < curveSpans.size(); ++spanIndex) {
            if (!enabledSpans[spanIndex]) {
                continue;
            }
            const CurveSpanPlacement &span = curveSpans[spanIndex];
            layout.spanAtStart[span.first] = spanIndex;
            activeRemovedVertices += span.last - span.first;
            ++activeSpanCount;
            for (int i = span.first; i <= span.last; ++i) {
                activeSpanCovered[i] = true;
            }
        }
        QVector<int> activeMidpointCandidates;
        for (int i = 0; i < segments.size(); ++i) {
            if (enabledInwardCurves[i]) {
                ++activeInwardCount;
            } else if (segments[i].curved && !activeSpanCovered[i]) {
                activeMidpointCandidates.push_back(i);
            }
        }
        const int capCount = activeSpanCount + activeInwardCount;
        const int baseVertices = segments.size() - activeRemovedVertices + activeInwardCount;
        const int maximumCoreVertices = std::max(3, result.shapeLimit - capCount + 2);
        const int midpointBudget = std::max(0, maximumCoreVertices - baseVertices);
        std::sort(activeMidpointCandidates.begin(),
                  activeMidpointCandidates.end(),
                  [&](int a, int b) {
            const PenBoundarySegment &left = segments[a];
            const PenBoundarySegment &right = segments[b];
            const double leftCurvature = std::sqrt(
                distanceSquaredToSegment(left.control, left.start, left.end));
            const double rightCurvature = std::sqrt(
                distanceSquaredToSegment(right.control, right.start, right.end));
            if (std::abs(leftCurvature - rightCurvature) > kEpsilon) {
                return leftCurvature > rightCurvature;
            }
            return a < b;
        });
        QSet<int> midpointSegments;
        for (int i = 0;
             i < std::min(midpointBudget, static_cast<int>(activeMidpointCandidates.size()));
             ++i) {
            midpointSegments.insert(activeMidpointCandidates[i]);
        }
        layout.points.push_back(segments.front().start);
        for (int i = 0; i < segments.size();) {
            const int spanIndex = layout.spanAtStart[i];
            if (spanIndex >= 0) {
                const CurveSpanPlacement &span = curveSpans[spanIndex];
                layout.points.push_back(segments[span.last].end);
                i = span.last + 1;
                continue;
            }
            const PenBoundarySegment &segment = segments[i];
            if (enabledInwardCurves[i]) {
                layout.points.push_back(inwardCurves[i]->coreMiddle);
            } else if (segment.curved && midpointSegments.contains(i)) {
                layout.points.push_back(segmentPoint(segment, 0.5));
            }
            layout.points.push_back(segment.end);
            ++i;
        }
        QVector<QPointF> normalized;
        normalized.reserve(layout.points.size());
        for (const QPointF &point : std::as_const(layout.points)) {
            if (normalized.isEmpty() || QLineF(normalized.back(), point).length() > kEpsilon) {
                normalized.push_back(point);
            }
        }
        if (normalized.size() > 1
            && QLineF(normalized.front(), normalized.back()).length() <= kEpsilon) {
            normalized.removeLast();
        }
        layout.points = std::move(normalized);
        return layout;
    };
    CoreLayout layout = coreLayout(activeSpans, activeInwardCurves);
    PolygonContour coreContour = buildPolygonContour(layout.points);
    while (!coreContour.crossings.isEmpty()) {
        CoreFitKind bestKind = CoreFitKind::None;
        int bestIndex = -1;
        int bestScore = std::numeric_limits<int>::max();
        CoreLayout bestLayout;
        PolygonContour bestContour;
        const auto consider = [&](CoreFitKind kind, int index) {
            CoreLayout candidateLayout = coreLayout(activeSpans, activeInwardCurves);
            PolygonContour candidateContour = buildPolygonContour(candidateLayout.points);
            const int score = candidateContour.valid()
                ? -1
                : (!candidateContour.crossings.isEmpty()
                       ? candidateContour.crossings.size()
                       : std::numeric_limits<int>::max());
            if (score < bestScore) {
                bestKind = kind;
                bestIndex = index;
                bestScore = score;
                bestLayout = std::move(candidateLayout);
                bestContour = std::move(candidateContour);
            }
        };
        for (int i = 0; i < activeSpans.size() && bestScore != -1; ++i) {
            if (!activeSpans[i]) {
                continue;
            }
            activeSpans[i] = false;
            consider(CoreFitKind::Span, i);
            activeSpans[i] = true;
            if (cancelled && cancelled()) {
                result.cancelled = true;
                result.error = QStringLiteral("Pen core repair timed out");
                return result;
            }
        }
        for (int i = 0; i < activeInwardCurves.size() && bestScore != -1; ++i) {
            if (!activeInwardCurves[i]) {
                continue;
            }
            activeInwardCurves[i] = false;
            consider(CoreFitKind::InwardCurve, i);
            activeInwardCurves[i] = true;
            if (cancelled && cancelled()) {
                result.cancelled = true;
                result.error = QStringLiteral("Pen core repair timed out");
                return result;
            }
        }
        if (bestKind == CoreFitKind::None
            || bestScore == std::numeric_limits<int>::max()) {
            break;
        }
        if (bestKind == CoreFitKind::Span) {
            activeSpans[bestIndex] = false;
        } else {
            activeInwardCurves[bestIndex] = false;
        }
        layout = std::move(bestLayout);
        coreContour = std::move(bestContour);
    }
    if (layout.points.size() < 3) {
        result.error = QStringLiteral("The Pen contour left no polygonal core");
        return result;
    }
    QPainterPath coverage;
    coverage.setFillRule(Qt::WindingFill);
    for (int i = 0; i < segments.size();) {
        const int spanIndex = layout.spanAtStart[i];
        if (spanIndex >= 0) {
            const CurveSpanPlacement &span = curveSpans[spanIndex];
            result.placements.push_back(span.curve.placement);
            coverage = coverage.united(span.curve.path);
            i = span.last + 1;
            continue;
        }
        if (activeInwardCurves[i]) {
            result.placements.push_back(inwardCurves[i]->curve.placement);
            coverage = coverage.united(inwardCurves[i]->curve.path);
        }
        ++i;
    }
    PolygonMeshRequest meshRequest;
    meshRequest.points = layout.points;
    meshRequest.sources = meshSources;
    meshRequest.mergeSquares = true;
    const PolygonMeshResult mesh = meshPolygon(meshRequest, cancelled);
    if (mesh.cancelled || (cancelled && cancelled())) {
        result.placements.clear();
        result.cancelled = true;
        result.error = QStringLiteral("Pen core mesh timed out");
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
