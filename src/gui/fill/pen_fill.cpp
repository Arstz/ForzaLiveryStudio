#include "pen_fill.h"
#include "differential_cover_gpu.h"
#include "polygon_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
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
constexpr double kMaximumStraightBoundaryOutsideRatio = 1e-7;
constexpr double kStraightBoundaryMinimumDepthMultiplier = 2.0;
constexpr double kMaximumCoreOutsideRatio = 1e-5;
constexpr double kMaximumNegligiblePlacementAreaRatio = 1e-3;
constexpr double kMaximumDiscardedAreaRatio = 1e-3;
constexpr double kMaximumDiscardedThicknessRatio = 0.0025;
constexpr int kInteriorCoreSteps = 16;
constexpr int kChordContainmentSamples = 32;
constexpr int kMinimumStraightBoundarySegmentCount = 8;
constexpr int kStraightStripDepthSteps = 12;
constexpr double kMaximumTangentJump = 0.35;
constexpr double kMaximumCurvatureJump = 0.75;
constexpr double kMaximumSpanErrorRatio = 0.05;
constexpr int kSpanSamples = 64;
constexpr int kMaximumSpanEvaluations = 2048;
constexpr int kMaximumCurveProfileShortlist = 32;
constexpr int kMaximumExactCurveAttempts = 12;
constexpr int kMaximumValidCurveCandidates = 4;
constexpr int kMaximumInteriorCurvePlacements = 36;
constexpr int kMaximumExactInteriorCandidates = 48;
constexpr int kMaximumExactInteriorCpuCandidates = 96;
constexpr int kMaximumStraightProfilesPerPrimitive = 1;
constexpr int kMaximumStraightTargetEdges = 8;
constexpr int kMaximumInteriorTriangleTargets = 12;
constexpr double kMaximumCoreReplacementLossRatio = 0.01;
constexpr double kMaximumCompletionOutsideRatio = 0.01;
constexpr double kMaximumCompletionResidualRatio = 0.01;
constexpr double kCompletionResidualRatio = 1e-6;
constexpr double kCompletionSpillPenalty = 2.0;
constexpr int kMaximumCompletionPlacements = 8;
constexpr double kDefaultLegalEnvelopeDistance = 1.0;

int curveEvaluationBudget(int segmentCount) {
    // A budget smaller than the contour consumed every trial on individual
    // segments and left no room for the multi-segment matches that actually
    // reduce the polygonal core. Scale with contour complexity while retaining
    // the existing hard cap.
    return std::min(1024, std::max(96, segmentCount * 3));
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

bool isCurveShape(const PenPrimitive &primitive) {
    return isCurveShape(primitive.shapeId)
        || std::any_of(
            primitive.curveSegments.cbegin(),
            primitive.curveSegments.cend(),
            [](const PenBoundarySegment &segment) { return segment.curved; });
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
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        result += std::abs(signedArea(polygon));
    }
    return result;
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

QPainterPath expandedPath(const QPainterPath &path, double distance) {
    QPainterPathStroker stroker;
    stroker.setWidth(distance * 2.0);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);

    return path.united(stroker.createStroke(path));
}

double sampledArcLength(const PenBoundarySegment &segment) {
    const QVector<QPointF> samples = sampleSegment(segment, 16);
    double length = 0.0;
    for (int index = 1; index < samples.size(); ++index) {
        length += QLineF(samples[index - 1], samples[index]).length();
    }
    return length;
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

bool tangentBreak(const PenBoundarySegment &left,
                  const PenBoundarySegment &right) {
    const QPointF incoming = segmentDerivative(left, 1.0);
    const QPointF outgoing = segmentDerivative(right, 0.0);
    const double incomingLength = std::hypot(incoming.x(), incoming.y());
    const double outgoingLength = std::hypot(outgoing.x(), outgoing.y());
    if (incomingLength <= kEpsilon || outgoingLength <= kEpsilon) {
        return true;
    }
    const double cosine = std::clamp(
        QPointF::dotProduct(incoming, outgoing)
            / (incomingLength * outgoingLength),
        -1.0, 1.0);
    return std::acos(cosine) > kMaximumTangentJump;
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

QVector<QPointF> sampleCyclicSegmentSpan(
    const QVector<PenBoundarySegment> &segments,
    int first,
    int span) {
    const int samplesPerSegment = std::clamp(
        kSpanSamples / std::max(1, span), 2, 8);
    QVector<QPointF> result;
    result.reserve(span * samplesPerSegment + 1);
    result.push_back(segments[first].start);
    for (int offset = 0; offset < span; ++offset) {
        const PenBoundarySegment &segment = segments[
            (first + offset) % segments.size()];
        for (int sample = 1; sample <= samplesPerSegment; ++sample) {
            result.push_back(segmentPoint(
                segment, static_cast<double>(sample) / samplesPerSegment));
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

QVector<QPointF> contourPairCrossings(
    const QVector<PenBoundarySegment> &left,
    const QVector<PenBoundarySegment> &right,
    double tolerance) {
    QVector<QPointF> crossings;
    for (const PenBoundarySegment &leftSegment : left) {
        const QVector<QPointF> leftPoints = sampleSegment(leftSegment, kCurveSamples);
        for (const PenBoundarySegment &rightSegment : right) {
            const QVector<QPointF> rightPoints = sampleSegment(rightSegment, kCurveSamples);
            for (int leftIndex = 0; leftIndex + 1 < leftPoints.size(); ++leftIndex) {
                for (int rightIndex = 0; rightIndex + 1 < rightPoints.size(); ++rightIndex) {
                    QPointF crossing;
                    if (!lineIntersection(leftPoints[leftIndex],
                                          leftPoints[leftIndex + 1],
                                          rightPoints[rightIndex],
                                          rightPoints[rightIndex + 1],
                                          tolerance * 0.1,
                                          &crossing)) {
                        continue;
                    }
                    const bool duplicate = std::any_of(
                        crossings.cbegin(), crossings.cend(),
                        [&](const QPointF &existing) {
                            return QLineF(existing, crossing).length() <= tolerance * 2.0;
                        });
                    if (!duplicate) {
                        crossings.push_back(crossing);
                    }
                }
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
        } else if (primitive.shapeId == kCircleShapeId) {
            sources.circle = primitiveHull(primitive);
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

bool discardNegligiblePlacements(
    QVector<PenPlacement> *placements,
    QPainterPath *coverage,
    const QVector<PenPrimitive> &primitives,
    const QPainterPath &target,
    double targetArea,
    double boundaryTolerance,
    const std::function<bool()> &cancelled) {
    if (placements->size() < 2 || targetArea <= kEpsilon) {
        return true;
    }
    struct Candidate {
        int index = -1;
        double area = 0.0;
    };

    QVector<QPainterPath> placementPaths;
    placementPaths.reserve(placements->size());
    for (const PenPlacement &placement : std::as_const(*placements)) {
        const PenPrimitive *primitive = primitiveForId(primitives, placement.shapeId);
        if (primitive == nullptr) {
            return true;
        }
        placementPaths.push_back(
            placement.transform.map(primitive->silhouette).simplified());
    }
    const double maximumPlacementArea =
        targetArea * kMaximumNegligiblePlacementAreaRatio;
    const double maximumDiscardedArea =
        targetArea * kMaximumDiscardedAreaRatio;
    const double baselineMissingArea =
        pathArea(target.subtracted(*coverage));
    const double baselineOutsideArea =
        pathArea(coverage->subtracted(target));
    const double booleanAreaNoise =
        std::max(kEpsilon, targetArea * 1e-8);
    const QRectF targetBounds = target.boundingRect();
    const double targetDiagonal =
        std::hypot(targetBounds.width(), targetBounds.height());
    const double maximumResidualThickness =
        std::max(boundaryTolerance * 4.0,
                 targetDiagonal * kMaximumDiscardedThicknessRatio);
    QVector<Candidate> candidates;
    QVector<bool> active(placements->size(), true);
    for (int i = 0; i < placements->size(); ++i) {
        if (cancelled && cancelled()) {
            return false;
        }
        if ((*placements)[i].boundaryFitKind != PenBoundaryFitKind::None) {
            continue;
        }
        const double placementArea = (*placements)[i].area;
        if (placementArea > maximumPlacementArea) {
            continue;
        }
        const QRectF placementBounds = placementPaths[i].boundingRect();
        const double placementDiameter =
            std::hypot(placementBounds.width(), placementBounds.height());
        const double placementThickness =
            placementArea * 2.0 / std::max(placementDiameter, kEpsilon);
        if (placementThickness <= maximumResidualThickness) {
            candidates.push_back({i, placementArea});
        }
    }
    std::sort(candidates.begin(),
              candidates.end(),
              [](const Candidate &left, const Candidate &right) {
        if (std::abs(left.area - right.area) > kEpsilon) {
            return left.area < right.area;
        }
        return left.index < right.index;
    });
    double discardedArea = 0.0;
    QVector<int> discardedIndices;
    for (const Candidate &candidate : std::as_const(candidates)) {
        if (discardedArea + candidate.area > maximumDiscardedArea + kEpsilon) {
            break;
        }
        active[candidate.index] = false;
        discardedArea += candidate.area;
        discardedIndices.push_back(candidate.index);
    }
    if (discardedIndices.isEmpty()) {
        return true;
    }
    const auto rebuiltCoverage = [&]() {
        QPainterPath result;
        result.setFillRule(Qt::WindingFill);
        for (int i = 0; i < placements->size(); ++i) {
            if (active[i]) {
                result = result.united(placementPaths[i]);
            }
        }
        return result;
    };
    const auto coverageIsSafe = [&](const QPainterPath &candidateCoverage) {
        const double missingArea =
            pathArea(target.subtracted(candidateCoverage));
        const double outsideArea =
            pathArea(candidateCoverage.subtracted(target));
        return missingArea <= baselineMissingArea + maximumDiscardedArea
                + booleanAreaNoise
            && outsideArea <= baselineOutsideArea + booleanAreaNoise;
    };
    QPainterPath retainedCoverage = rebuiltCoverage();
    if (cancelled && cancelled()) {
        return false;
    }
    if (!coverageIsSafe(retainedCoverage)) {
        int acceptedCount = 0;
        int rejectedCount = discardedIndices.size();
        QPainterPath acceptedCoverage;
        while (acceptedCount + 1 < rejectedCount) {
            if (cancelled && cancelled()) {
                return false;
            }
            const int trialCount = (acceptedCount + rejectedCount) / 2;
            active.fill(true);
            for (int i = 0; i < trialCount; ++i) {
                active[discardedIndices[i]] = false;
            }
            QPainterPath trialCoverage = rebuiltCoverage();
            if (coverageIsSafe(trialCoverage)) {
                acceptedCount = trialCount;
                acceptedCoverage = std::move(trialCoverage);
            } else {
                rejectedCount = trialCount;
            }
        }
        if (acceptedCount == 0) {
            if (cancelled && cancelled()) {
                return false;
            }
            active.fill(true);
            active[discardedIndices.front()] = false;
            acceptedCoverage = rebuiltCoverage();
            if (!coverageIsSafe(acceptedCoverage)) {
                return true;
            }
            acceptedCount = 1;
        }
        active.fill(true);
        for (int i = 0; i < acceptedCount; ++i) {
            active[discardedIndices[i]] = false;
        }
        retainedCoverage = std::move(acceptedCoverage);
        if (retainedCoverage.isEmpty()) {
            return true;
        }
    }
    QVector<PenPlacement> retained;
    retained.reserve(placements->size());
    for (int i = 0; i < placements->size(); ++i) {
        if (active[i]) {
            retained.push_back((*placements)[i]);
        }
    }
    *placements = std::move(retained);
    *coverage = std::move(retainedCoverage);

    return true;
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
    bool straight = false;
};

struct CurvePrimitive {
    const PenPrimitive *primitive = nullptr;
    QVector<ArcProfile> profiles;
};

struct CurveTransformCandidate {
    const PenPrimitive *primitive = nullptr;
    QTransform transform;
    QPointF coreMiddle;
    double approximateBoundaryError =
        std::numeric_limits<double>::max();
    bool hasCoreMiddle = false;
    int coveredMeshHint = -1;
    int targetGroup = -1;
};

struct EvaluatedCurveTransform {
    CurvePlacement curve;
    QPointF coreMiddle;
    bool hasCoreMiddle = false;
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
    if (!primitive.curveSegments.isEmpty()) {
        constexpr int kMaximumTemplateSpan = 8;
        const int segmentCount = primitive.curveSegments.size();
        for (int first = 0; first < segmentCount; ++first) {
            for (int span = 1;
                 span <= std::min(kMaximumTemplateSpan, segmentCount - 1);
                 ++span) {
                bool curved = false;
                for (int offset = 0; offset < span; ++offset) {
                    curved = curved
                        || primitive.curveSegments[
                            (first + offset) % segmentCount].curved;
                }
                const QVector<QPointF> samples = sampleCyclicSegmentSpan(
                    primitive.curveSegments, first, span);
                if (samples.size() < 3
                    || QLineF(samples.front(), samples.back()).length()
                        <= kEpsilon) {
                    continue;
                }
                const QPointF start = samples.front();
                const QPointF middle = samples[samples.size() / 2];
                const QPointF end = samples.back();
                const QPointF chord = end - start;
                const QPointF leftNormal(-chord.y(), chord.x());
                double curveSide = cross(chord, middle - start);
                double maximumDeviation = std::abs(curveSide);
                for (const QPointF &sample : samples) {
                    const double side = cross(chord, sample - start);
                    if (std::abs(side) > maximumDeviation) {
                        maximumDeviation = std::abs(side);
                        curveSide = side;
                    }
                }
                const double chordLength = std::hypot(chord.x(), chord.y());
                const bool straight = !curved
                    && maximumDeviation
                        <= chordLength * chordLength * 1e-7;
                if (!curved && !straight) {
                    continue;
                }
                if (straight) {
                    const QPointF midpoint = (start + end) * 0.5;
                    const double probeDistance = std::max(
                        1e-5,
                        std::hypot(primitive.bounds.width(),
                                   primitive.bounds.height()) * 1e-5);
                    const QPointF normal = leftNormal / chordLength;
                    const bool leftInside = primitive.silhouette.contains(
                        midpoint + normal * probeDistance);
                    const bool rightInside = primitive.silhouette.contains(
                        midpoint - normal * probeDistance);
                    if (leftInside != rightInside) {
                        curveSide = leftInside ? -1.0 : 1.0;
                    } else {
                        curveSide = cross(
                            chord, primitive.bounds.center() - start) >= 0.0
                            ? -1.0 : 1.0;
                    }
                }
                const QPointF inwardDirection = curveSide >= 0.0
                    ? -leftNormal : leftNormal;
                result.push_back({
                    start, middle, end,
                    supportPoint(primitive, inwardDirection),
                    straight,
                });
            }
        }
        if (!result.isEmpty()) {
            return result;
        }
    }
    const QRectF bounds = primitive.bounds;
    if (primitive.shapeId == kHalfCircleShapeId) {
        result.push_back({{bounds.left(), bounds.bottom()},
                          {bounds.center().x(), bounds.top()},
                          {bounds.right(), bounds.bottom()},
                          {bounds.center().x(), bounds.bottom()},
                          false});
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
                          center,
                          false});
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
                           center.y() - bounds.height() / std::numbers::sqrt2},
                          false});
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

struct ArcSignature {
    double chordPosition = 0.5;
    double bowRatio = 0.0;
};

ArcSignature arcSignature(const QPointF &start,
                          const QPointF &middle,
                          const QPointF &end) {
    const QPointF chord = end - start;
    const double chordLengthSquared = QPointF::dotProduct(chord, chord);
    if (chordLengthSquared <= kEpsilon) {
        return {};
    }
    return {
        QPointF::dotProduct(middle - start, chord) / chordLengthSquared,
        cross(chord, middle - start) / chordLengthSquared,
    };
}

QVector<QPair<const CurvePrimitive *, const ArcProfile *>> rankedArcProfiles(
    const QVector<CurvePrimitive> &curves,
    const QPointF &start,
    const QPointF &middle,
    const QPointF &end,
    bool includeCircle) {
    struct RankedProfile {
        const CurvePrimitive *curve = nullptr;
        const ArcProfile *profile = nullptr;
        double score = std::numeric_limits<double>::max();
    };
    const ArcSignature target = arcSignature(start, middle, end);
    QVector<RankedProfile> primary;
    QVector<RankedProfile> secondary;
    primary.reserve(curves.size());
    secondary.reserve(curves.size());
    for (const CurvePrimitive &curve : curves) {
        if ((!includeCircle
             && curve.primitive->shapeId == kCircleShapeId)
            || curve.profiles.isEmpty()) {
            continue;
        }
        QVector<RankedProfile> curveProfiles;
        curveProfiles.reserve(curve.profiles.size());
        for (const ArcProfile &profile : curve.profiles) {
            if (profile.straight) {
                continue;
            }
            const ArcSignature source = arcSignature(
                profile.start, profile.middle, profile.end);
            const double score =
                std::abs(source.chordPosition - target.chordPosition)
                + std::abs(std::abs(source.bowRatio)
                           - std::abs(target.bowRatio)) * 2.0;
            curveProfiles.push_back({&curve, &profile, score});
        }
        std::sort(
            curveProfiles.begin(), curveProfiles.end(),
            [](const RankedProfile &left, const RankedProfile &right) {
                return left.score < right.score;
            });
        if (!curveProfiles.isEmpty()) {
            primary.push_back(curveProfiles.front());
        }
        if (curveProfiles.size() > 1) {
            secondary.push_back(curveProfiles[1]);
        }
    }
    const auto lessProfile = [](const RankedProfile &left,
                                const RankedProfile &right) {
        if (std::abs(left.score - right.score) > kEpsilon) {
            return left.score < right.score;
        }
        return left.curve->primitive->shapeId
            < right.curve->primitive->shapeId;
    };
    std::sort(primary.begin(), primary.end(), lessProfile);
    std::sort(secondary.begin(), secondary.end(), lessProfile);
    QVector<QPair<const CurvePrimitive *, const ArcProfile *>> result;
    result.reserve(kMaximumCurveProfileShortlist);
    for (const RankedProfile &profile : std::as_const(primary)) {
        if (result.size() >= kMaximumCurveProfileShortlist) {
            break;
        }
        result.push_back({profile.curve, profile.profile});
    }
    for (const RankedProfile &profile : std::as_const(secondary)) {
        if (result.size() >= kMaximumCurveProfileShortlist) {
            break;
        }
        result.push_back({profile.curve, profile.profile});
    }
    return result;
}

double approximateBoundaryError(const CurveTransformCandidate &candidate,
                                const QVector<QPointF> &samples) {
    bool invertible = false;
    const QTransform inverse = candidate.transform.inverted(&invertible);
    if (!invertible) {
        return std::numeric_limits<double>::max();
    }
    const double scale = std::max(
        std::hypot(candidate.transform.m11(), candidate.transform.m12()),
        std::hypot(candidate.transform.m21(), candidate.transform.m22()));
    if (!std::isfinite(scale) || scale <= kEpsilon) {
        return std::numeric_limits<double>::max();
    }
    double result = 0.0;
    for (const QPointF &sample : samples) {
        result = std::max(
            result,
            distanceToPolygons(
                inverse.map(sample), candidate.primitive->contours) * scale);
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

QVector<CurveTransformCandidate> curveTransformCandidates(
    const QVector<CurvePrimitive> &curves,
    const QVector<QPointF> &samples,
    double maximumInset,
    bool outward) {
    if (samples.size() < 3) {
        return {};
    }
    const QPointF start = samples.front();
    const QPointF middle = samples[samples.size() / 2];
    const QPointF end = samples.back();
    QVector<CurveTransformCandidate> result;
    if (outward) {
        const auto circle = std::find_if(
            curves.cbegin(), curves.cend(), [](const CurvePrimitive &curve) {
                return curve.primitive->shapeId == kCircleShapeId;
            });
        if (circle != curves.cend()) {
            QVector<QTransform> transforms;
            if (const auto fit = ellipseLeastSquares(
                    *circle->primitive, samples)) {
                transforms.push_back(*fit);
            }
            if (const auto fit = circleThroughPoints(
                    *circle->primitive, start, middle, end)) {
                transforms.push_back(*fit);
            }
            const QPointF chordMiddle = (start + end) * 0.5;
            for (double scale : {0.85, 0.925, 1.0}) {
                const QPointF apex = chordMiddle
                    + (middle - chordMiddle) * scale;
                if (const auto fit = ellipseThroughCurve(
                        *circle->primitive, start, apex, end, false)) {
                    transforms.push_back(*fit);
                }
            }
            for (const QTransform &transform : std::as_const(transforms)) {
                result.push_back({circle->primitive, transform});
            }
        }
    }
    const auto profiles = rankedArcProfiles(
        curves, start, middle, end, !outward);
    for (const auto &[curve, profile] : profiles) {
        for (const QTransform &transform : arcTransforms(
                 *profile, start, middle, end, maximumInset)) {
            CurveTransformCandidate candidate;
            candidate.primitive = curve->primitive;
            candidate.transform = transform;
            candidate.coreMiddle = transform.map(profile->coreMiddle);
            candidate.hasCoreMiddle = true;
            result.push_back(std::move(candidate));
        }
    }
    return result;
}

std::optional<CurvePlacement> evaluateCurvePlacement(const PenPrimitive &primitive,
                                                     const QVector<QPointF> &samples,
                                                     const QTransform &transform,
                                                     const QPainterPath &target,
                                                     const QPainterPath &legalEnvelope,
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
    const double illegalArea = pathArea(result.path.subtracted(legalEnvelope));
    if (targetArea <= kEpsilon
        || illegalArea / targetArea > kMaximumCurveOutsideRatio) {
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

std::optional<EvaluatedCurveTransform> evaluateCurveTransforms(
    QVector<CurveTransformCandidate> candidates,
    const QVector<QPointF> &samples,
    const QPainterPath &target,
    const QPainterPath &legalEnvelope,
    double targetArea,
    double maximumBoundaryError,
    cover::GpuAreaEvaluator *gpuEvaluator,
    const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
    const std::function<bool()> &cancelled,
    const std::function<bool(const CurveTransformCandidate &)> &accepted,
    const std::function<void()> &exactProgress) {
    const bool exhaustiveLegacyEvaluation = gpuMeshes.isEmpty();
    candidates.erase(
        std::remove_if(
            candidates.begin(), candidates.end(),
            [&](CurveTransformCandidate &candidate) {
                candidate.approximateBoundaryError =
                    approximateBoundaryError(candidate, samples);
                return !std::isfinite(candidate.approximateBoundaryError)
                    || (!exhaustiveLegacyEvaluation
                        && candidate.approximateBoundaryError
                            > maximumBoundaryError * 1.5 + kEpsilon);
            }),
        candidates.end());
    if (candidates.isEmpty()) {
        return std::nullopt;
    }

    QVector<double> gpuScores(
        candidates.size(), -std::numeric_limits<double>::infinity());
    bool gpuRanked = false;
    if (gpuEvaluator != nullptr && gpuEvaluator->available()) {
        QVector<cover::GpuEvaluationRequest> requests;
        QVector<int> indices;
        requests.reserve(candidates.size());
        indices.reserve(candidates.size());
        for (int index = 0; index < candidates.size(); ++index) {
            const CurveTransformCandidate &candidate = candidates[index];
            const cover::ShapeMesh *mesh = gpuMeshes.value(
                candidate.primitive->shapeId, nullptr);
            if (mesh == nullptr) {
                continue;
            }
            const QTransform &transform = candidate.transform;
            requests.push_back({
                mesh,
                cover::Affine{
                    transform.m11(), transform.m12(),
                    transform.m21(), transform.m22(),
                    transform.dx(), transform.dy(),
                },
            });
            indices.push_back(index);
        }
        QVector<cover::AreaGradient> evaluations;
        if (!requests.isEmpty()
            && gpuEvaluator->evaluate(requests, &evaluations)
            && evaluations.size() == requests.size()) {
            gpuRanked = true;
            for (int index = 0; index < evaluations.size(); ++index) {
                gpuScores[indices[index]] = evaluations[index].covered
                    - evaluations[index].spill * 8.0;
            }
        }
    }
    QVector<int> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        if (gpuRanked
            && std::abs(gpuScores[left] - gpuScores[right]) > kEpsilon) {
            return gpuScores[left] > gpuScores[right];
        }
        if (!gpuRanked) {
            const double leftArea = candidates[left].primitive->area
                * std::abs(candidates[left].transform.determinant());
            const double rightArea = candidates[right].primitive->area
                * std::abs(candidates[right].transform.determinant());
            if (std::abs(leftArea - rightArea) > kEpsilon) {
                return leftArea > rightArea;
            }
        }
        const double boundaryDifference =
            candidates[left].approximateBoundaryError
            - candidates[right].approximateBoundaryError;
        if (std::abs(boundaryDifference) > kEpsilon) {
            return boundaryDifference < 0.0;
        }
        return candidates[left].primitive->shapeId
            < candidates[right].primitive->shapeId;
    });
    QVector<int> shortlist;
    if (exhaustiveLegacyEvaluation) {
        shortlist = order;
    }
    shortlist.reserve(kMaximumExactCurveAttempts);
    QSet<int> usedShapes;
    for (const int index : std::as_const(order)) {
        if (exhaustiveLegacyEvaluation) {
            break;
        }
        const int shapeId = candidates[index].primitive->shapeId;
        if (usedShapes.contains(shapeId)) {
            continue;
        }
        usedShapes.insert(shapeId);
        shortlist.push_back(index);
        if (shortlist.size() >= kMaximumExactCurveAttempts) {
            break;
        }
    }
    for (const int index : std::as_const(order)) {
        if (exhaustiveLegacyEvaluation) {
            break;
        }
        if (shortlist.size() >= kMaximumExactCurveAttempts) {
            break;
        }
        if (!shortlist.contains(index)) {
            shortlist.push_back(index);
        }
    }

    std::optional<EvaluatedCurveTransform> best;
    int validCandidateCount = 0;
    for (const int index : std::as_const(shortlist)) {
        if (cancelled && cancelled()) {
            break;
        }
        const CurveTransformCandidate &candidate = candidates[index];
        if (exactProgress) {
            exactProgress();
        }
        const auto placement = evaluateCurvePlacement(
            *candidate.primitive, samples, candidate.transform,
            target, legalEnvelope, targetArea, maximumBoundaryError);
        if (!placement || (accepted && !accepted(candidate))) {
            continue;
        }
        ++validCandidateCount;
        EvaluatedCurveTransform evaluated{
            *placement, candidate.coreMiddle, candidate.hasCoreMiddle};
        if (!best || betterCurvePlacement(
                         evaluated.curve, best->curve)) {
            best = std::move(evaluated);
        }
        if (!exhaustiveLegacyEvaluation
            && validCandidateCount >= kMaximumValidCurveCandidates) {
            break;
        }
    }
    return best;
}

struct CurveCoreOptimization {
    QVector<PenPlacement> placements;
    QPainterPath coverage;
};

QVector<PenPlacement> penPlacementsFromMesh(
    const QVector<PolygonMeshPlacement> &placements,
    const QVector<PenPrimitive> &primitives,
    QPainterPath *coverage) {
    QVector<PenPlacement> result;
    result.reserve(placements.size());
    for (const PolygonMeshPlacement &placement : placements) {
        const PenPrimitive *primitive = primitiveForId(
            primitives, placement.shapeId);
        if (primitive == nullptr) {
            return {};
        }
        result.push_back({
            placement.shapeId,
            placement.transform,
            primitive->area * std::abs(placement.transform.determinant()),
            placement.shapeId == kCircleShapeId,
        });
        if (coverage != nullptr) {
            *coverage = coverage->united(
                placement.transform.map(primitive->silhouette));
        }
    }
    return result;
}

double polygonPrincipalAngle(const QPolygonF &polygon) {
    if (polygon.isEmpty()) {
        return 0.0;
    }
    QPointF mean;
    for (const QPointF &point : polygon) {
        mean += point;
    }
    mean /= polygon.size();
    double xx = 0.0;
    double xy = 0.0;
    double yy = 0.0;
    for (const QPointF &point : polygon) {
        const QPointF centered = point - mean;
        xx += centered.x() * centered.x();
        xy += centered.x() * centered.y();
        yy += centered.y() * centered.y();
    }
    return 0.5 * std::atan2(2.0 * xy, xx - yy);
}

std::optional<QTransform> fitPrimitiveToPolygon(
    const PenPrimitive &primitive,
    const QPolygonF &polygon,
    double angle,
    double scale) {
    if (polygon.size() < 3 || primitive.bounds.width() <= kEpsilon
        || primitive.bounds.height() <= kEpsilon) {
        return std::nullopt;
    }
    const QPointF axisX(std::cos(angle), std::sin(angle));
    const QPointF axisY(-axisX.y(), axisX.x());
    double minimumX = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double minimumY = std::numeric_limits<double>::max();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const QPointF &point : polygon) {
        const double x = QPointF::dotProduct(point, axisX);
        const double y = QPointF::dotProduct(point, axisY);
        minimumX = std::min(minimumX, x);
        maximumX = std::max(maximumX, x);
        minimumY = std::min(minimumY, y);
        maximumY = std::max(maximumY, y);
    }
    const double width = (maximumX - minimumX) * scale;
    const double height = (maximumY - minimumY) * scale;
    if (width <= kEpsilon || height <= kEpsilon) {
        return std::nullopt;
    }
    const QPointF center = axisX * ((minimumX + maximumX) * 0.5)
        + axisY * ((minimumY + maximumY) * 0.5);
    const QPointF targetTopLeft = center
        - axisX * (width * 0.5) - axisY * (height * 0.5);
    bool ok = false;
    const QTransform transform = affineFromTriangles(
        primitive.bounds.topLeft(), primitive.bounds.topRight(),
        primitive.bounds.bottomLeft(), targetTopLeft,
        targetTopLeft + axisX * width,
        targetTopLeft + axisY * height, &ok);
    return ok ? std::optional<QTransform>(transform) : std::nullopt;
}

QVector<QPolygonF> residualComponents(const QPainterPath &residual) {
    QVector<QPolygonF> polygons = residual.toFillPolygons();
    for (QPolygonF &polygon : polygons) {
        while (polygon.size() > 1
               && QLineF(polygon.front(), polygon.back()).length()
                   <= kEpsilon) {
            polygon.removeLast();
        }
    }
    polygons.erase(
        std::remove_if(polygons.begin(), polygons.end(),
                       [](const QPolygonF &polygon) {
                           return polygon.size() < 3
                               || std::abs(signedArea(polygon)) <= kEpsilon;
                       }),
        polygons.end());
    std::sort(polygons.begin(), polygons.end(),
              [](const QPolygonF &left, const QPolygonF &right) {
        return std::abs(signedArea(left)) > std::abs(signedArea(right));
    });
    return polygons;
}

QPolygonF normalizedPathPolygon(QPolygonF polygon) {
    while (polygon.size() > 1
           && QLineF(polygon.front(), polygon.back()).length() <= kEpsilon) {
        polygon.removeLast();
    }
    return polygon;
}

PolygonMeshResult meshPainterPath(
    const QPainterPath &path,
    const PolygonMeshSources &sources,
    const std::function<bool()> &cancelled) {
    PolygonMeshResult result;
    if (path.isEmpty()) {
        return result;
    }
    QVector<QPolygonF> polygons;
    for (QPolygonF polygon : path.toSubpathPolygons()) {
        polygon = normalizedPathPolygon(std::move(polygon));
        if (polygon.size() >= 3
            && std::abs(signedArea(polygon)) > kEpsilon) {
            polygons.push_back(std::move(polygon));
        }
    }
    if (polygons.isEmpty()) {
        result.error = QStringLiteral("The residual has no meshable contour");
        return result;
    }

    QVector<int> parents(polygons.size(), -1);
    for (int child = 0; child < polygons.size(); ++child) {
        double parentArea = std::numeric_limits<double>::max();
        for (int candidate = 0; candidate < polygons.size(); ++candidate) {
            if (candidate == child
                || !polygons[candidate].containsPoint(
                    polygons[child].front(), Qt::OddEvenFill)) {
                continue;
            }
            const double area = std::abs(signedArea(polygons[candidate]));
            if (area < parentArea) {
                parentArea = area;
                parents[child] = candidate;
            }
        }
    }
    QVector<int> depths(polygons.size(), 0);
    for (int index = 0; index < polygons.size(); ++index) {
        int parent = parents[index];
        while (parent >= 0) {
            ++depths[index];
            parent = parents[parent];
            if (depths[index] > polygons.size()) {
                result.error = QStringLiteral("The residual contour nesting is invalid");
                return result;
            }
        }
    }

    result.contour.setFillRule(Qt::WindingFill);
    for (int outer = 0; outer < polygons.size(); ++outer) {
        if (depths[outer] % 2 != 0) {
            continue;
        }
        PolygonMeshRequest request;
        request.sources = sources;
        for (int hole = 0; hole < polygons.size(); ++hole) {
            if (parents[hole] == outer && depths[hole] % 2 != 0) {
                request.contours.push_back(
                    QVector<QPointF>(polygons[hole].cbegin(),
                                     polygons[hole].cend()));
            }
        }
        if (request.contours.isEmpty()) {
            request.points = QVector<QPointF>(polygons[outer].cbegin(),
                                              polygons[outer].cend());
        } else {
            request.contours.prepend(
                QVector<QPointF>(polygons[outer].cbegin(),
                                 polygons[outer].cend()));
        }
        request.mergeSquares = request.contours.isEmpty();
        const PolygonMeshResult component = meshPolygon(request, cancelled);
        if (component.cancelled) {
            result.cancelled = true;
            result.placements.clear();
            return result;
        }
        if (!component.error.isEmpty()) {
            result.error = component.error;
            result.placements.clear();
            return result;
        }
        result.placements += component.placements;
        result.contour = result.contour.united(component.contour);
    }
    const double allowedArea = std::max(1e-6, pathArea(path) * 1e-4);
    if (pathArea(result.contour.subtracted(path)) > allowedArea
        || pathArea(path.subtracted(result.contour)) > allowedArea) {
        result.error = QStringLiteral("The residual mesh did not match its contour");
        result.placements.clear();
    }
    return result;
}

std::optional<QPointF> inwardNormalForEdge(
    const QPointF &start,
    const QPointF &end,
    const QPainterPath &path) {
    const QPointF edge = end - start;
    const double length = std::hypot(edge.x(), edge.y());
    if (length <= kEpsilon) {
        return std::nullopt;
    }
    const QPointF normal(-edge.y() / length, edge.x() / length);
    const QPointF middle = (start + end) * 0.5;
    const QRectF bounds = path.boundingRect();
    const double probeDistance = std::max(
        1e-5, std::hypot(bounds.width(), bounds.height()) * 1e-5);
    const bool leftInside = path.contains(middle + normal * probeDistance);
    const bool rightInside = path.contains(middle - normal * probeDistance);
    if (leftInside == rightInside) {
        return std::nullopt;
    }
    return leftInside ? std::optional<QPointF>(normal)
                      : std::optional<QPointF>(-normal);
}

double interiorDepthAlongRay(const QPointF &origin,
                             const QPointF &direction,
                             const QPainterPath &path) {
    const QRectF bounds = path.boundingRect();
    const double limit = std::max(
        1e-4, std::hypot(bounds.width(), bounds.height()) * 1.5);
    constexpr int steps = 64;
    double inside = 0.0;
    double outside = limit;
    for (int step = 1; step <= steps; ++step) {
        const double distance = limit * step / steps;
        if (!path.contains(origin + direction * distance)) {
            outside = distance;
            break;
        }
        inside = distance;
    }
    for (int iteration = 0; iteration < 12; ++iteration) {
        const double middle = (inside + outside) * 0.5;
        if (path.contains(origin + direction * middle)) {
            inside = middle;
        } else {
            outside = middle;
        }
    }
    return inside;
}

QVector<QTransform> straightBoundaryTransforms(
    const QVector<ArcProfile> &profiles,
    const QPointF &targetStart,
    const QPointF &targetEnd,
    const QPainterPath &target) {
    const auto inward = inwardNormalForEdge(
        targetStart, targetEnd, target);
    const double targetLength = QLineF(targetStart, targetEnd).length();
    if (!inward || targetLength <= kEpsilon) {
        return {};
    }
    const QPointF targetMiddle = (targetStart + targetEnd) * 0.5;
    const double maximumDepth = interiorDepthAlongRay(
        targetMiddle, *inward, target);
    if (maximumDepth <= kEpsilon) {
        return {};
    }

    QVector<QTransform> result;
    QVector<const ArcProfile *> straightProfiles;
    for (const ArcProfile &profile : profiles) {
        if (profile.straight) {
            straightProfiles.push_back(&profile);
        }
    }
    std::sort(straightProfiles.begin(), straightProfiles.end(),
              [](const ArcProfile *left, const ArcProfile *right) {
        return QLineF(left->start, left->end).length()
            > QLineF(right->start, right->end).length();
    });
    straightProfiles.resize(std::min(
        kMaximumStraightProfilesPerPrimitive,
        static_cast<int>(straightProfiles.size())));
    for (const ArcProfile *profilePointer : std::as_const(straightProfiles)) {
        const ArcProfile &profile = *profilePointer;
        const QPointF sourceEdge = profile.end - profile.start;
        const double sourceLengthSquared = QPointF::dotProduct(
            sourceEdge, sourceEdge);
        if (sourceLengthSquared <= kEpsilon) {
            continue;
        }
        const double sourceDepth = std::abs(cross(
            sourceEdge, profile.coreMiddle - profile.start))
            / std::sqrt(sourceLengthSquared);
        if (sourceDepth <= kEpsilon) {
            continue;
        }
        const double sourcePosition = QPointF::dotProduct(
            profile.coreMiddle - profile.start, sourceEdge)
            / sourceLengthSquared;
        const double naturalDepth = targetLength * sourceDepth
            / std::sqrt(sourceLengthSquared);
        QVector<double> depths = {
            naturalDepth,
            maximumDepth * 0.9,
        };
        std::sort(depths.begin(), depths.end());
        depths.erase(std::unique(
                         depths.begin(), depths.end(),
                         [](double left, double right) {
                             return std::abs(left - right)
                                 <= std::max({1e-6,
                                              std::abs(left) * 1e-6,
                                              std::abs(right) * 1e-6});
                         }),
                     depths.end());
        for (const bool reversed : {false, true}) {
            const QPointF start = reversed ? targetEnd : targetStart;
            const QPointF end = reversed ? targetStart : targetEnd;
            for (const double depth : std::as_const(depths)) {
                if (depth <= kEpsilon
                    || depth > maximumDepth * 1.05 + kEpsilon) {
                    continue;
                }
                const QPointF core = start
                    + (end - start) * sourcePosition
                    + *inward * depth;
                bool ok = false;
                const QTransform transform = affineFromTriangles(
                    profile.start, profile.end, profile.coreMiddle,
                    start, end, core, &ok);
                if (ok && transform.isAffine()
                    && std::abs(transform.determinant()) > kEpsilon) {
                    result.push_back(transform);
                }
            }
        }
    }
    return result;
}

CurveCoreOptimization optimizeCurveCoreWithTemplates(
    const QVector<PolygonMeshPlacement> &baselineMesh,
    const PolygonMeshSources &meshSources,
    const QPainterPath &corePath,
    const QVector<PenPrimitive> &primitives,
    const QPainterPath &legalEnvelope,
    double targetArea,
    int shapeBudget,
    cover::GpuAreaEvaluator *gpuEvaluator,
    const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
    const std::function<bool()> &cancelled,
    const std::function<void(int, int)> &progress) {
    CurveCoreOptimization best;
    best.placements = penPlacementsFromMesh(
        baselineMesh, primitives, &best.coverage);
    if (best.placements.isEmpty() || corePath.isEmpty()
        || gpuMeshes.isEmpty() || shapeBudget <= 0) {
        return best;
    }
    const CurveCoreOptimization baseline = best;
    const cover::Polygons baselineCoveragePolygons =
        cover::polygonsFromPainterPath(baseline.coverage, 0.025);
    const cover::Polygons legalEnvelopePolygons =
        cover::polygonsFromPainterPath(legalEnvelope, 0.025);
    QVector<const PenPrimitive *> interiorPrimitives;
    QHash<int, QVector<ArcProfile>> interiorProfiles;
    for (const PenPrimitive &primitive : primitives) {
        if (primitive.shapeId == kSquareShapeId
            || primitive.shapeId == kTriangleShapeId
            || (isCurveShape(primitive)
                && !primitive.curveSegments.isEmpty())) {
            interiorPrimitives.push_back(&primitive);
            interiorProfiles.insert(
                primitive.shapeId, primitiveArcProfiles(primitive));
        }
    }
    if (interiorPrimitives.isEmpty()) {
        return best;
    }

    struct InteriorTriangleAnchor {
        const PenPrimitive *primitive = nullptr;
        std::array<QPointF, 3> points;
        double area = 0.0;
    };
    QVector<InteriorTriangleAnchor> triangleAnchors;
    for (const PenPrimitive *primitive : std::as_const(interiorPrimitives)) {
        PolygonMeshRequest primitiveMeshRequest;
        primitiveMeshRequest.sources = meshSources;
        primitiveMeshRequest.mergeSquares = false;
        primitiveMeshRequest.contours.reserve(primitive->contours.size());
        for (const QPolygonF &contour : primitive->contours) {
            primitiveMeshRequest.contours.push_back(
                QVector<QPointF>(contour.cbegin(), contour.cend()));
        }
        const PolygonMeshResult primitiveMesh = meshPolygon(
            primitiveMeshRequest, cancelled);
        if (primitiveMesh.cancelled || !primitiveMesh.error.isEmpty()) {
            continue;
        }
        QVector<InteriorTriangleAnchor> shapeAnchors;
        shapeAnchors.reserve(primitiveMesh.placements.size());
        for (const PolygonMeshPlacement &placement
             : primitiveMesh.placements) {
            if (placement.shapeId != kTriangleShapeId) {
                continue;
            }
            InteriorTriangleAnchor anchor;
            anchor.primitive = primitive;
            anchor.points = {
                placement.transform.map(meshSources.triangle[0]),
                placement.transform.map(meshSources.triangle[1]),
                placement.transform.map(meshSources.triangle[2])};
            anchor.area = std::abs(cross(
                anchor.points[1] - anchor.points[0],
                anchor.points[2] - anchor.points[0])) * 0.5;
            if (anchor.area > kEpsilon) {
                shapeAnchors.push_back(std::move(anchor));
            }
        }
        std::sort(shapeAnchors.begin(), shapeAnchors.end(),
                  [](const InteriorTriangleAnchor &left,
                     const InteriorTriangleAnchor &right) {
            return left.area > right.area;
        });
        shapeAnchors.resize(std::min(
            2, static_cast<int>(shapeAnchors.size())));
        for (InteriorTriangleAnchor &anchor : shapeAnchors) {
            triangleAnchors.push_back(std::move(anchor));
        }
    }

    QVector<QPainterPath> baselinePaths;
    QVector<cover::Polygons> baselinePathPolygons;
    QVector<double> baselineAreas;
    QVector<QRectF> baselineBounds;
    baselinePaths.reserve(baselineMesh.size());
    baselinePathPolygons.reserve(baselineMesh.size());
    baselineAreas.reserve(baselineMesh.size());
    baselineBounds.reserve(baselineMesh.size());
    for (const PolygonMeshPlacement &placement : baselineMesh) {
        const PenPrimitive *primitive = primitiveForId(
            primitives, placement.shapeId);
        if (primitive == nullptr) {
            return best;
        }
        QPainterPath path = placement.transform.map(primitive->silhouette);
        baselineAreas.push_back(pathArea(path));
        baselineBounds.push_back(path.boundingRect());
        baselinePathPolygons.push_back(
            cover::polygonsFromPainterPath(path, 0.025));
        baselinePaths.push_back(std::move(path));
    }

    QVector<bool> active(baselineMesh.size(), true);
    QVector<PenPlacement> interiorPlacements;
    QVector<QPainterPath> interiorPaths;
    QPainterPath interiorCoverage;
    interiorCoverage.setFillRule(Qt::WindingFill);
    cover::Polygons interiorCoveragePolygons;
    int bestPlacementCount = best.placements.size();
    const auto considerPlan = [&](CurveCoreOptimization plan) {
        const int placementCount = plan.placements.size();
        const cover::Polygons planPolygons =
            cover::polygonsFromPainterPath(plan.coverage, 0.025);
        const double lostBaselineArea = pathArea(
            cover::painterPathFromPolygons(cover::differencePolygons(
                baselineCoveragePolygons, planPolygons)));
        const double illegalArea = pathArea(
            cover::painterPathFromPolygons(cover::differencePolygons(
                planPolygons, legalEnvelopePolygons)));
        const bool complete = lostBaselineArea
                <= std::max(1e-6,
                            targetArea * kMaximumCoreReplacementLossRatio)
            && illegalArea
                <= std::max(1e-6, targetArea * 1e-5);
        if (!complete || placementCount > shapeBudget
            || placementCount >= bestPlacementCount) {
            return;
        }
        bestPlacementCount = placementCount;
        best = std::move(plan);
    };
    const int maximumPlacements = std::min(
        kMaximumInteriorCurvePlacements, shapeBudget);
    for (int iteration = 0; iteration < maximumPlacements; ++iteration) {
        if (cancelled && cancelled()) {
            break;
        }
        QVector<int> activeIndices;
        for (int index = 0; index < active.size(); ++index) {
            if (!active[index]) {
                continue;
            }
            activeIndices.push_back(index);
        }
        if (activeIndices.isEmpty()) {
            break;
        }
        const QPainterPath uncoveredCoverage =
            corePath.subtracted(interiorCoverage);
        if (pathArea(uncoveredCoverage)
            <= std::max(1e-6, targetArea * 1e-4)) {
            break;
        }

        QVector<QPolygonF> components = residualComponents(uncoveredCoverage);
        components.resize(std::min(1, static_cast<int>(components.size())));
        const int boundaryComponentCount = components.size();

        // Whole connected components are useful for broad shapes.  Local
        // nearest-neighbour clusters add the smaller targets needed to replace
        // groups of triangles without carving holes and remeshing fragments.
        const int anchorCount = std::min(
            2, static_cast<int>(activeIndices.size()));
        for (int anchorNumber = 0; anchorNumber < anchorCount; ++anchorNumber) {
            const int anchorPosition = anchorCount == 1
                ? 0
                : anchorNumber * (activeIndices.size() - 1)
                    / (anchorCount - 1);
            const int anchor = activeIndices[anchorPosition];
            const QPointF anchorCenter = baselineBounds[anchor].center();
            QVector<int> neighbours = activeIndices;
            std::sort(neighbours.begin(), neighbours.end(),
                      [&](int left, int right) {
                const QPointF leftOffset = baselineBounds[left].center()
                    - anchorCenter;
                const QPointF rightOffset = baselineBounds[right].center()
                    - anchorCenter;
                return QPointF::dotProduct(leftOffset, leftOffset)
                    < QPointF::dotProduct(rightOffset, rightOffset);
            });
            for (const int requestedSize : {2, 4, 6}) {
                const int clusterSize = std::min(
                    requestedSize, static_cast<int>(neighbours.size()));
                if (clusterSize < 2) {
                    continue;
                }
                QRectF bounds = baselineBounds[neighbours.front()];
                for (int index = 1; index < clusterSize; ++index) {
                    bounds = bounds.united(baselineBounds[neighbours[index]]);
                }
                if (bounds.width() <= kEpsilon
                    || bounds.height() <= kEpsilon) {
                    continue;
                }
                QPolygonF rectangle;
                rectangle << bounds.topLeft() << bounds.topRight()
                          << bounds.bottomRight() << bounds.bottomLeft();
                components.push_back(std::move(rectangle));
            }
        }

        QVector<CurveTransformCandidate> candidates;
        for (int componentIndex = 0;
             componentIndex < components.size(); ++componentIndex) {
            const QPolygonF &component = components[componentIndex];
            const double principalAngle = polygonPrincipalAngle(component);
            for (const double angle : {
                     principalAngle, principalAngle + std::numbers::pi * 0.5,
                     0.0}) {
                for (const double scale : {0.65, 1.0}) {
                    for (const PenPrimitive *primitive : interiorPrimitives) {
                        const auto transform = fitPrimitiveToPolygon(
                            *primitive, component, angle, scale);
                        if (transform) {
                            CurveTransformCandidate candidate;
                            candidate.primitive = primitive;
                            candidate.transform = *transform;
                            candidate.targetGroup = componentIndex;
                            candidates.push_back(std::move(candidate));
                        }
                    }
                }
            }
            if (componentIndex >= boundaryComponentCount) {
                continue;
            }
            QVector<int> boundaryEdges(component.size());
            std::iota(boundaryEdges.begin(), boundaryEdges.end(), 0);
            std::sort(boundaryEdges.begin(), boundaryEdges.end(),
                      [&](int left, int right) {
                return QLineF(
                           component[left],
                           component[(left + 1) % component.size()]).length()
                    > QLineF(
                           component[right],
                           component[(right + 1) % component.size()]).length();
            });
            boundaryEdges.resize(std::min(
                kMaximumStraightTargetEdges,
                static_cast<int>(boundaryEdges.size())));
            for (const int edge : std::as_const(boundaryEdges)) {
                const QPointF start = component[edge];
                const QPointF end = component[(edge + 1) % component.size()];
                for (const PenPrimitive *primitive : interiorPrimitives) {
                    const auto profiles = interiorProfiles.constFind(
                        primitive->shapeId);
                    if (profiles == interiorProfiles.cend()) {
                        continue;
                    }
                    for (const QTransform &transform
                         : straightBoundaryTransforms(
                               profiles.value(), start, end,
                               uncoveredCoverage)) {
                        CurveTransformCandidate candidate;
                        candidate.primitive = primitive;
                        candidate.transform = transform;
                        candidate.targetGroup = componentIndex;
                        candidates.push_back(std::move(candidate));
                    }
                }
            }
        }
        if (meshSources.triangle.size() == 3) {
            QVector<int> triangleTargets = activeIndices;
            std::sort(triangleTargets.begin(), triangleTargets.end(),
                      [&](int left, int right) {
                return baselineAreas[left] > baselineAreas[right];
            });
            triangleTargets.resize(std::min(
                kMaximumInteriorTriangleTargets,
                static_cast<int>(triangleTargets.size())));
            for (const int placementIndex : std::as_const(triangleTargets)) {
                if (baselineMesh[placementIndex].shapeId
                    != kTriangleShapeId) {
                    continue;
                }
                const QTransform &targetTransform =
                    baselineMesh[placementIndex].transform;
                const QPointF target0 = targetTransform.map(
                    meshSources.triangle[0]);
                const QPointF target1 = targetTransform.map(
                    meshSources.triangle[1]);
                const QPointF target2 = targetTransform.map(
                    meshSources.triangle[2]);
                for (const InteriorTriangleAnchor &anchor
                     : std::as_const(triangleAnchors)) {
                    for (const bool reflected : {false, true}) {
                        bool ok = false;
                        const QTransform transform = affineFromTriangles(
                            anchor.points[0], anchor.points[1],
                            anchor.points[2], target0,
                            reflected ? target2 : target1,
                            reflected ? target1 : target2, &ok);
                        if (!ok) {
                            continue;
                        }
                        CurveTransformCandidate candidate;
                        candidate.primitive = anchor.primitive;
                        candidate.transform = transform;
                        candidate.coveredMeshHint = placementIndex;
                        candidates.push_back(std::move(candidate));
                    }
                }
            }
        }
        if (candidates.isEmpty()) {
            break;
        }
        QVector<double> scores(
            candidates.size(), -std::numeric_limits<double>::infinity());
        QVector<double> approximateSpill(
            candidates.size(), std::numeric_limits<double>::infinity());
        bool gpuRanked = false;
        if (gpuEvaluator != nullptr && gpuEvaluator->available()) {
            const cover::Polygons residualPolygons =
                cover::polygonsFromPainterPath(uncoveredCoverage, 0.025);
            const cover::Polygons legalPolygons =
                cover::polygonsFromPainterPath(legalEnvelope, 0.025);
            if (gpuEvaluator->setSubjects(residualPolygons, legalPolygons)) {
                QVector<cover::GpuEvaluationRequest> requests;
                QVector<int> indices;
                requests.reserve(candidates.size());
                for (int index = 0; index < candidates.size(); ++index) {
                    const cover::ShapeMesh *mesh = gpuMeshes.value(
                        candidates[index].primitive->shapeId, nullptr);
                    if (mesh == nullptr) {
                        continue;
                    }
                    const QTransform &transform = candidates[index].transform;
                    requests.push_back({
                        mesh,
                        cover::Affine{
                            transform.m11(), transform.m12(),
                            transform.m21(), transform.m22(),
                            transform.dx(), transform.dy(),
                        },
                    });
                    indices.push_back(index);
                }
                QVector<cover::AreaGradient> evaluations;
                if (!requests.isEmpty()
                    && gpuEvaluator->evaluate(requests, &evaluations)
                    && evaluations.size() == requests.size()) {
                    gpuRanked = true;
                    for (int index = 0; index < evaluations.size(); ++index) {
                        scores[indices[index]] = evaluations[index].covered
                            - evaluations[index].spill * 12.0;
                        approximateSpill[indices[index]] =
                            evaluations[index].spill;
                    }
                }
            }
        }
        for (int index = 0; index < candidates.size(); ++index) {
            if (!gpuRanked || !std::isfinite(scores[index])) {
                const QPainterPath path = candidates[index].transform.map(
                    candidates[index].primitive->silhouette);
                const QRectF intersection = path.boundingRect().intersected(
                    uncoveredCoverage.boundingRect());
                scores[index] = intersection.width() * intersection.height();
            }
            if (approximateSpill[index]
                <= std::max(1e-6, targetArea * 1e-4)) {
                // Ensure approximately legal candidates are evaluated before
                // larger candidates whose apparent coverage comes from spill.
                scores[index] += targetArea * 2.0;
                if (candidates[index].coveredMeshHint >= 0) {
                    scores[index] += targetArea * 2.0;
                }
            }
        }
        QVector<int> order(candidates.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int left, int right) {
            return scores[left] > scores[right];
        });
        const int exactCandidateLimit = gpuRanked
            ? kMaximumExactInteriorCandidates
            : kMaximumExactInteriorCpuCandidates;
        QVector<int> exactOrder;
        exactOrder.reserve(exactCandidateLimit);
        QHash<int, int> shortlistedTargetCounts;
        for (const int index : std::as_const(order)) {
            const int targetGroup = candidates[index].targetGroup;
            if (targetGroup < 0
                || shortlistedTargetCounts.value(targetGroup) >= 2) {
                continue;
            }
            ++shortlistedTargetCounts[targetGroup];
            exactOrder.push_back(index);
            if (exactOrder.size() >= exactCandidateLimit) {
                break;
            }
        }
        QSet<int> shortlistedShapes;
        for (const int index : std::as_const(order)) {
            const int shapeId = candidates[index].primitive->shapeId;
            if (shortlistedShapes.contains(shapeId)) {
                continue;
            }
            shortlistedShapes.insert(shapeId);
            exactOrder.push_back(index);
            if (exactOrder.size() >= exactCandidateLimit) {
                break;
            }
        }
        for (const int index : std::as_const(order)) {
            if (exactOrder.size() >= exactCandidateLimit) {
                break;
            }
            if (!exactOrder.contains(index)) {
                exactOrder.push_back(index);
            }
        }
        int bestIndex = -1;
        QPainterPath bestPath;
        QVector<int> bestCovered;
        int bestCoveredTriangles = -1;
        int bestRetirableCount = -1;
        double bestGain = 0.0;
        double bestPlacementArea = 0.0;
        for (const int index : std::as_const(exactOrder)) {
            if (cancelled && cancelled()) {
                break;
            }
            const QPainterPath path = candidates[index].transform.map(
                candidates[index].primitive->silhouette);
            const double area = pathArea(path);
            const double illegal = pathArea(path.subtracted(legalEnvelope));
            if (area <= kEpsilon
                || illegal > std::max(1e-6, targetArea * 1e-5)) {
                continue;
            }
            const double gain = pathArea(path.intersected(uncoveredCoverage));
            if (gain <= std::max(targetArea * 1e-4, area * 0.05)) {
                continue;
            }
            cover::Polygons combinedSubjects = interiorCoveragePolygons;
            combinedSubjects += cover::polygonsFromPainterPath(path, 0.025);
            const cover::Polygons combinedCoveragePolygons =
                cover::unionPolygons(combinedSubjects);
            const double combinedIllegal = cover::polygonSetArea(
                cover::differencePolygons(
                    combinedCoveragePolygons, legalEnvelopePolygons));
            if (combinedIllegal
                > std::max(1e-6, targetArea * 1e-5)) {
                continue;
            }
            QVector<int> covered;
            QVector<double> retirementCosts;
            retirementCosts.reserve(activeIndices.size());
            int coveredTriangles = 0;
            for (const int placementIndex : std::as_const(activeIndices)) {
                const double missing = cover::polygonSetArea(
                    cover::differencePolygons(
                        baselinePathPolygons[placementIndex],
                        combinedCoveragePolygons));
                retirementCosts.push_back(missing);
                if (missing > std::max(
                        1e-6, baselineAreas[placementIndex] * 1e-3)) {
                    continue;
                }
                covered.push_back(placementIndex);
                coveredTriangles += baselineMesh[placementIndex].shapeId
                    == kTriangleShapeId ? 1 : 0;
            }
            std::sort(retirementCosts.begin(), retirementCosts.end());
            const double retirementLossLimit = std::max(
                1e-6, targetArea * kMaximumCoreReplacementLossRatio);
            double retirementLoss = 0.0;
            int retirableCount = 0;
            for (const double cost : std::as_const(retirementCosts)) {
                if (retirementLoss + cost > retirementLossLimit) {
                    break;
                }
                retirementLoss += cost;
                ++retirableCount;
            }
            const bool better = gain > bestGain + kEpsilon
                || (std::abs(gain - bestGain) <= kEpsilon
                    && retirableCount > bestRetirableCount)
                || (std::abs(gain - bestGain) <= kEpsilon
                    && retirableCount == bestRetirableCount
                    && covered.size() > bestCovered.size())
                || (std::abs(gain - bestGain) <= kEpsilon
                    && retirableCount == bestRetirableCount
                    && covered.size() == bestCovered.size()
                    && coveredTriangles > bestCoveredTriangles)
                || (std::abs(gain - bestGain) <= kEpsilon
                    && retirableCount == bestRetirableCount
                    && covered.size() == bestCovered.size()
                    && coveredTriangles == bestCoveredTriangles
                    && (bestPlacementArea <= kEpsilon
                        || area < bestPlacementArea - kEpsilon));
            if (better) {
                bestIndex = index;
                bestPath = path;
                bestCovered = std::move(covered);
                bestCoveredTriangles = coveredTriangles;
                bestRetirableCount = retirableCount;
                bestGain = gain;
                bestPlacementArea = area;
            }
        }
        if (bestIndex < 0) {
            break;
        }
        const CurveTransformCandidate &selected = candidates[bestIndex];
        interiorPlacements.push_back({
            selected.primitive->shapeId,
            selected.transform,
            selected.primitive->area
                * std::abs(selected.transform.determinant()),
            selected.primitive->shapeId == kCircleShapeId,
        });
        interiorPaths.push_back(bestPath);
        interiorCoverage = interiorCoverage.united(bestPath);
        interiorCoveragePolygons += cover::polygonsFromPainterPath(
            bestPath, 0.025);
        interiorCoveragePolygons = cover::unionPolygons(
            interiorCoveragePolygons);
        for (const int index : std::as_const(bestCovered)) {
            active[index] = false;
        }
        CurveCoreOptimization retiredPlan;
        retiredPlan.coverage.setFillRule(Qt::WindingFill);
        cover::Polygons retiredPolygons;
        struct RetirementCandidate {
            int index = -1;
            double missingArea = 0.0;
        };
        QVector<RetirementCandidate> retirementCandidates;
        retirementCandidates.reserve(baselineMesh.size());
        for (int index = 0; index < baselineMesh.size(); ++index) {
            retirementCandidates.push_back({
                index,
                cover::polygonSetArea(cover::differencePolygons(
                    baselinePathPolygons[index],
                    interiorCoveragePolygons)),
            });
        }
        std::sort(retirementCandidates.begin(), retirementCandidates.end(),
                  [&](const RetirementCandidate &left,
                      const RetirementCandidate &right) {
            if (std::abs(left.missingArea - right.missingArea) > kEpsilon) {
                return left.missingArea < right.missingArea;
            }
            return baselineMesh[left.index].shapeId == kTriangleShapeId
                && baselineMesh[right.index].shapeId != kTriangleShapeId;
        });
        QSet<int> retiredIndices;
        const double retirementLossLimit = std::max(
            1e-6, targetArea * kMaximumCoreReplacementLossRatio);
        double retirementLoss = 0.0;
        for (const RetirementCandidate &candidate
             : std::as_const(retirementCandidates)) {
            if (retirementLoss + candidate.missingArea
                > retirementLossLimit) {
                break;
            }
            retirementLoss += candidate.missingArea;
            retiredIndices.insert(candidate.index);
        }
        for (int index = 0; index < active.size(); ++index) {
            active[index] = !retiredIndices.contains(index);
        }
        for (int index = 0; index < baselineMesh.size(); ++index) {
            if (retiredIndices.contains(index)) {
                continue;
            }
            retiredPlan.placements += penPlacementsFromMesh(
                {baselineMesh[index]}, primitives, nullptr);
            retiredPolygons += cover::polygonsFromPainterPath(
                baselinePaths[index], 0.025);
        }
        for (int index = 0; index < interiorPlacements.size(); ++index) {
            retiredPlan.placements.push_back(interiorPlacements[index]);
            retiredPolygons += cover::polygonsFromPainterPath(
                interiorPaths[index], 0.025);
        }
        retiredPlan.coverage = cover::painterPathFromPolygons(
            cover::unionPolygons(retiredPolygons));
        considerPlan(std::move(retiredPlan));

        const QPainterPath residual = corePath.subtracted(interiorCoverage);
        PolygonMeshResult residualMesh;
        const bool residualRemeshIsCompact = residual.elementCount()
            <= corePath.elementCount() + 8;
        if (!residual.isEmpty() && residualRemeshIsCompact) {
            residualMesh = meshPainterPath(residual, meshSources, cancelled);
        }
        if (residualMesh.cancelled) {
            break;
        }
        if (residualRemeshIsCompact && residualMesh.error.isEmpty()) {
            QPainterPath residualCoverage;
            residualCoverage.setFillRule(Qt::WindingFill);
            const QVector<PenPlacement> residualPlacements =
                penPlacementsFromMesh(
                    residualMesh.placements, primitives, &residualCoverage);
            if (residual.isEmpty()
                || !residualPlacements.isEmpty()) {
                CurveCoreOptimization plan;
                plan.placements = interiorPlacements;
                plan.placements += residualPlacements;
                plan.coverage = interiorCoverage.united(residualCoverage);
                considerPlan(std::move(plan));
            }
        }
        if (progress) {
            progress(iteration + 1, maximumPlacements);
        }
    }
    return best;
}

std::optional<CurveCoreOptimization> optimizeCoverageWithinSpill(
    const QVector<PenPlacement> &initialPlacements,
    const QPainterPath &initialCoverage,
    const QPainterPath &target,
    const QPainterPath &legalEnvelope,
    const QVector<PenPrimitive> &primitives,
    double targetArea,
    int placementBudget,
    cover::GpuAreaEvaluator *gpuEvaluator,
    const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
    const std::function<bool()> &cancelled,
    const std::function<void(int, int)> &progress) {
    CurveCoreOptimization result;
    if (target.isEmpty()) {
        return std::nullopt;
    }
    QVector<const PenPrimitive *> candidatesPrimitives;
    for (const PenPrimitive &primitive : primitives) {
        if (primitive.shapeId == kSquareShapeId
            || primitive.shapeId == kTriangleShapeId
            || (isCurveShape(primitive)
                && !primitive.curveSegments.isEmpty())) {
            candidatesPrimitives.push_back(&primitive);
        }
    }
    if (candidatesPrimitives.isEmpty()) {
        return std::nullopt;
    }

    const cover::Polygons targetPolygons =
        cover::polygonsFromPainterPath(target, 0.025);
    const cover::Polygons legalPolygons =
        cover::polygonsFromPainterPath(legalEnvelope, 0.025);
    cover::Polygons coveragePolygons = cover::unionPolygons(
        cover::polygonsFromPainterPath(initialCoverage, 0.025));
    const double residualLimit = std::max(
        1e-6, targetArea * kCompletionResidualRatio);
    const double outsideLimit = std::max(
        1e-6, targetArea * kMaximumCompletionOutsideRatio);
    const double illegalLimit = std::max(
        cover::polygonSetArea(cover::differencePolygons(
            coveragePolygons, legalPolygons)),
        targetArea * kMaximumCoreOutsideRatio) + residualLimit;
    const int maximumPlacements = std::max(0, std::min({
        kMaximumCompletionPlacements,
        placementBudget,
        std::max(1, static_cast<int>(initialPlacements.size()) / 10),
    }));
    result.placements = initialPlacements;
    QVector<QPainterPath> placementPaths;
    placementPaths.reserve(result.placements.size());
    for (const PenPlacement &placement : std::as_const(result.placements)) {
        const PenPrimitive *primitive = primitiveForId(
            primitives, placement.shapeId);
        if (primitive == nullptr) {
            return std::nullopt;
        }
        placementPaths.push_back(
            placement.transform.map(primitive->silhouette));
    }

    for (int adjustment = 0;
         adjustment < std::min(
             48, static_cast<int>(result.placements.size()) * 2);
         ++adjustment) {
        if (cancelled && cancelled()) {
            return std::nullopt;
        }
        const double currentResidual = cover::polygonSetArea(
            cover::differencePolygons(targetPolygons, coveragePolygons));
        const double currentOutside = cover::polygonSetArea(
            cover::differencePolygons(coveragePolygons, targetPolygons));
        if (currentResidual <= residualLimit) {
            result.coverage = cover::painterPathFromPolygons(
                coveragePolygons);
            return result;
        }
        int bestPlacement = -1;
        QTransform bestTransform;
        QPainterPath bestPath;
        cover::Polygons bestCoverage;
        double bestResidual = currentResidual;
        double bestOutside = currentOutside;
        double bestUtility = 0.0;
        for (int placementIndex = 0;
             placementIndex < result.placements.size(); ++placementIndex) {
            const PenPlacement &placement = result.placements[placementIndex];
            const PenPrimitive *primitive = primitiveForId(
                primitives, placement.shapeId);
            if (primitive == nullptr) {
                continue;
            }
            const QPointF sourceCenter = primitive->bounds.center();
            const QPointF targetCenter = placement.transform.map(sourceCenter);
            const QPointF targetTopLeft = placement.transform.map(
                primitive->bounds.topLeft());
            const QPointF targetTopRight = placement.transform.map(
                primitive->bounds.topRight());
            const QPointF targetBottomLeft = placement.transform.map(
                primitive->bounds.bottomLeft());
            const QPointF targetAxisX = targetTopRight - targetTopLeft;
            const QPointF targetAxisY = targetBottomLeft - targetTopLeft;
            const double targetWidth = std::hypot(
                targetAxisX.x(), targetAxisX.y());
            const double targetHeight = std::hypot(
                targetAxisY.x(), targetAxisY.y());
            double maximumRadius = 0.0;
            for (const QPointF &corner : {
                     primitive->bounds.topLeft(),
                     primitive->bounds.topRight(),
                     primitive->bounds.bottomLeft(),
                     primitive->bounds.bottomRight()}) {
                maximumRadius = std::max(
                    maximumRadius,
                    QLineF(targetCenter,
                           placement.transform.map(corner)).length());
            }
            if (maximumRadius <= kEpsilon) {
                continue;
            }
            cover::Polygons otherSubjects;
            for (int index = 0; index < placementPaths.size(); ++index) {
                if (index != placementIndex) {
                    otherSubjects += cover::polygonsFromPainterPath(
                        placementPaths[index], 0.025);
                }
            }
            const cover::Polygons otherCoverage = cover::unionPolygons(
                otherSubjects);
            const auto considerTransform = [&](const QTransform &transform) {
                const QPainterPath path = transform.map(
                    primitive->silhouette);
                cover::Polygons trialSubjects = otherCoverage;
                trialSubjects += cover::polygonsFromPainterPath(path, 0.025);
                const cover::Polygons trialCoverage = cover::unionPolygons(
                    trialSubjects);
                if (cover::polygonSetArea(cover::differencePolygons(
                        trialCoverage, legalPolygons)) > illegalLimit) {
                    return;
                }
                const double outsideArea = cover::polygonSetArea(
                    cover::differencePolygons(
                        trialCoverage, targetPolygons));
                if (outsideArea > outsideLimit) {
                    return;
                }
                const double trialResidual = cover::polygonSetArea(
                    cover::differencePolygons(
                        targetPolygons, trialCoverage));
                const double utility = currentResidual - trialResidual
                    - kCompletionSpillPenalty
                        * std::max(0.0, outsideArea - currentOutside);
                const bool better = utility > bestUtility + kEpsilon
                    || (std::abs(utility - bestUtility) <= kEpsilon
                        && trialResidual < bestResidual - kEpsilon)
                    || (std::abs(utility - bestUtility) <= kEpsilon
                        && std::abs(trialResidual - bestResidual) <= kEpsilon
                        && outsideArea < bestOutside - kEpsilon);
                if (better) {
                    bestPlacement = placementIndex;
                    bestTransform = transform;
                    bestPath = path;
                    bestCoverage = trialCoverage;
                    bestResidual = trialResidual;
                    bestOutside = outsideArea;
                    bestUtility = utility;
                }
            };
            for (const double expansionDistance : {0.5, 1.0, 2.0, 4.0}) {
                const double scale = 1.0
                    + expansionDistance / maximumRadius;
                const auto expandedPoint = [&](const QPointF &source) {
                    const QPointF mapped = placement.transform.map(source);
                    return targetCenter + (mapped - targetCenter) * scale;
                };
                bool ok = false;
                const QTransform transform = affineFromTriangles(
                    primitive->bounds.topLeft(),
                    primitive->bounds.topRight(),
                    primitive->bounds.bottomLeft(),
                    expandedPoint(primitive->bounds.topLeft()),
                    expandedPoint(primitive->bounds.topRight()),
                    expandedPoint(primitive->bounds.bottomLeft()),
                    &ok);
                if (!ok) {
                    continue;
                }
                considerTransform(transform);
            }
            if (targetWidth > kEpsilon && targetHeight > kEpsilon) {
                const QPointF directionX = targetAxisX / targetWidth;
                const QPointF directionY = targetAxisY / targetHeight;
                for (const double distance
                     : {0.5, 1.0, 2.0, 4.0, 8.0, 16.0}) {
                    for (int side = 0; side < 4; ++side) {
                        QPointF topLeft = targetTopLeft;
                        QPointF topRight = targetTopRight;
                        QPointF bottomLeft = targetBottomLeft;
                        if (side == 0) {
                            topLeft -= directionX * distance;
                            bottomLeft -= directionX * distance;
                        } else if (side == 1) {
                            topRight += directionX * distance;
                        } else if (side == 2) {
                            topLeft -= directionY * distance;
                            topRight -= directionY * distance;
                        } else {
                            bottomLeft += directionY * distance;
                        }
                        bool ok = false;
                        const QTransform transform = affineFromTriangles(
                            primitive->bounds.topLeft(),
                            primitive->bounds.topRight(),
                            primitive->bounds.bottomLeft(),
                            topLeft, topRight, bottomLeft,
                            &ok);
                        if (ok) {
                            considerTransform(transform);
                        }
                    }
                }
            }
        }
        if (bestPlacement < 0) {
            break;
        }
        result.placements[bestPlacement].transform = bestTransform;
        const PenPrimitive *primitive = primitiveForId(
            primitives, result.placements[bestPlacement].shapeId);
        result.placements[bestPlacement].area = primitive->area
            * std::abs(bestTransform.determinant());
        placementPaths[bestPlacement] = std::move(bestPath);
        coveragePolygons = std::move(bestCoverage);
    }
    result.coverage = cover::painterPathFromPolygons(coveragePolygons);
    const double adjustedResidual = cover::polygonSetArea(
        cover::differencePolygons(targetPolygons, coveragePolygons));
    const double adjustedOutside = cover::polygonSetArea(
        cover::differencePolygons(coveragePolygons, targetPolygons));
    std::optional<CurveCoreOptimization> adjustedResult;
    if (adjustedResidual
            <= targetArea * kMaximumCompletionResidualRatio + kEpsilon
        && adjustedOutside <= outsideLimit + kEpsilon) {
        adjustedResult = result;
    }

    for (int iteration = 0; iteration < maximumPlacements; ++iteration) {
        if (cancelled && cancelled()) {
            return adjustedResult;
        }
        const cover::Polygons residualPolygons = cover::differencePolygons(
            targetPolygons, coveragePolygons);
        const double residualArea = cover::polygonSetArea(residualPolygons);
        if (residualArea <= residualLimit) {
            result.coverage = cover::painterPathFromPolygons(
                coveragePolygons);
            return result;
        }
        const QPainterPath residual = cover::painterPathFromPolygons(
            residualPolygons);
        QVector<QPolygonF> components = residualComponents(residual);
        components.resize(std::min(6, static_cast<int>(components.size())));
        if (components.isEmpty()) {
            return adjustedResult;
        }
        QVector<QPolygonF> candidateGroups = components;
        for (int left = 0; left < components.size(); ++left) {
            for (int right = left + 1; right < components.size(); ++right) {
                QPolygonF combined = components[left];
                combined += components[right];
                candidateGroups.push_back(std::move(combined));
            }
        }

        QVector<CurveTransformCandidate> candidates;
        for (int componentIndex = 0;
             componentIndex < candidateGroups.size(); ++componentIndex) {
            const QPolygonF &component = candidateGroups[componentIndex];
            const double principalAngle = polygonPrincipalAngle(component);
            for (const double angle : {
                     principalAngle,
                     principalAngle + std::numbers::pi * 0.5,
                     0.0}) {
                for (const double scale : {1.0, 1.1, 1.25, 1.5, 2.0}) {
                    for (const PenPrimitive *primitive
                         : std::as_const(candidatesPrimitives)) {
                        if (componentIndex >= components.size()
                            && primitive->shapeId != kSquareShapeId
                            && primitive->shapeId != kTriangleShapeId) {
                            continue;
                        }
                        const auto transform = fitPrimitiveToPolygon(
                            *primitive, component, angle, scale);
                        if (!transform) {
                            continue;
                        }
                        CurveTransformCandidate candidate;
                        candidate.primitive = primitive;
                        candidate.transform = *transform;
                        candidate.targetGroup = componentIndex;
                        candidates.push_back(std::move(candidate));
                    }
                }
            }
        }
        if (candidates.isEmpty()) {
            return adjustedResult;
        }

        QVector<double> scores(
            candidates.size(), -std::numeric_limits<double>::infinity());
        bool gpuRanked = false;
        if (gpuEvaluator != nullptr && gpuEvaluator->available()
            && gpuEvaluator->setSubjects(residualPolygons, legalPolygons)) {
            QVector<cover::GpuEvaluationRequest> requests;
            QVector<int> indices;
            requests.reserve(candidates.size());
            for (int index = 0; index < candidates.size(); ++index) {
                const cover::ShapeMesh *mesh = gpuMeshes.value(
                    candidates[index].primitive->shapeId, nullptr);
                if (mesh == nullptr) {
                    continue;
                }
                const QTransform &transform = candidates[index].transform;
                requests.push_back({
                    mesh,
                    cover::Affine{
                        transform.m11(), transform.m12(),
                        transform.m21(), transform.m22(),
                        transform.dx(), transform.dy(),
                    },
                });
                indices.push_back(index);
            }
            QVector<cover::AreaGradient> evaluations;
            if (!requests.isEmpty()
                && gpuEvaluator->evaluate(requests, &evaluations)
                && evaluations.size() == requests.size()) {
                gpuRanked = true;
                for (int index = 0; index < evaluations.size(); ++index) {
                    scores[indices[index]] = evaluations[index].covered
                        - evaluations[index].spill;
                }
            }
        }
        for (int index = 0; index < candidates.size(); ++index) {
            if (gpuRanked && std::isfinite(scores[index])) {
                continue;
            }
            const QPainterPath path = candidates[index].transform.map(
                candidates[index].primitive->silhouette);
            const QRectF intersection = path.boundingRect().intersected(
                residual.boundingRect());
            scores[index] = intersection.width() * intersection.height();
        }
        QVector<int> order(candidates.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int left, int right) {
            return scores[left] > scores[right];
        });
        order.resize(std::min(
            kMaximumExactInteriorCpuCandidates,
            static_cast<int>(order.size())));

        int bestIndex = -1;
        cover::Polygons bestCoverage;
        double bestResidual = residualArea;
        const double currentOutside = cover::polygonSetArea(
            cover::differencePolygons(coveragePolygons, targetPolygons));
        double bestOutside = currentOutside;
        double bestUtility = 0.0;
        for (const int index : std::as_const(order)) {
            if (cancelled && cancelled()) {
                return adjustedResult;
            }
            cover::Polygons trialSubjects = coveragePolygons;
            trialSubjects += cover::polygonsFromPainterPath(
                candidates[index].transform.map(
                    candidates[index].primitive->silhouette),
                0.025);
            const cover::Polygons trialCoverage = cover::unionPolygons(
                trialSubjects);
            if (cover::polygonSetArea(cover::differencePolygons(
                    trialCoverage, legalPolygons)) > illegalLimit) {
                continue;
            }
            const double outsideArea = cover::polygonSetArea(
                cover::differencePolygons(trialCoverage, targetPolygons));
            if (outsideArea > outsideLimit) {
                continue;
            }
            const double trialResidual = cover::polygonSetArea(
                cover::differencePolygons(targetPolygons, trialCoverage));
            const double utility = residualArea - trialResidual
                - kCompletionSpillPenalty
                    * std::max(0.0, outsideArea - currentOutside);
            const bool better = utility > bestUtility + kEpsilon
                || (std::abs(utility - bestUtility) <= kEpsilon
                    && trialResidual < bestResidual - kEpsilon)
                || (std::abs(utility - bestUtility) <= kEpsilon
                    && std::abs(trialResidual - bestResidual) <= kEpsilon
                    && outsideArea < bestOutside - kEpsilon);
            if (better) {
                bestIndex = index;
                bestCoverage = trialCoverage;
                bestResidual = trialResidual;
                bestOutside = outsideArea;
                bestUtility = utility;
            }
        }
        if (bestIndex < 0) {
            return adjustedResult;
        }
        const CurveTransformCandidate &selected = candidates[bestIndex];
        result.placements.push_back({
            selected.primitive->shapeId,
            selected.transform,
            selected.primitive->area
                * std::abs(selected.transform.determinant()),
            selected.primitive->shapeId == kCircleShapeId,
        });
        coveragePolygons = std::move(bestCoverage);
        if (progress) {
            progress(iteration + 1, maximumPlacements);
        }
    }

    const double residualArea = cover::polygonSetArea(
        cover::differencePolygons(targetPolygons, coveragePolygons));
    if (residualArea > residualLimit) {
        return adjustedResult;
    }
    result.coverage = cover::painterPathFromPolygons(coveragePolygons);

    return result;
}

std::optional<CurvePlacement> outwardCurvePlacement(const QVector<CurvePrimitive> &caps,
                                                     const PenBoundarySegment &segment,
                                                     const QPainterPath &target,
                                                     const QPainterPath &legalEnvelope,
                                                     double targetArea,
                                                     double maximumBoundaryError,
                                                     cover::GpuAreaEvaluator *gpuEvaluator,
                                                     const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
                                                     const std::function<bool()> &cancelled,
                                                     const std::function<void()> &exactProgress) {
    const QVector<QPointF> samples = sampleSegment(segment, kCurveSamples);
    const auto evaluated = evaluateCurveTransforms(
        curveTransformCandidates(
            caps, samples, maximumBoundaryError, true),
        samples, target, legalEnvelope, targetArea,
        maximumBoundaryError, gpuEvaluator, gpuMeshes, cancelled, {},
        exactProgress);
    return evaluated
        ? std::optional<CurvePlacement>(evaluated->curve) : std::nullopt;
}

bool chordInsideTarget(const QPointF &start,
                       const QPointF &end,
                       const QPainterPath &target) {
    for (int i = 1; i < kChordContainmentSamples; ++i) {
        const double fraction =
            static_cast<double>(i) / kChordContainmentSamples;
        const QPointF point = start * (1.0 - fraction) + end * fraction;
        if (!target.contains(point)) {
            return false;
        }
    }
    return true;
}

std::optional<QPointF> interiorCorePoint(const PenBoundarySegment &segment,
                                         const QPainterPath &target) {
    const QPointF curveMiddle = segmentPoint(segment, 0.5);
    for (int step = 1; step <= kInteriorCoreSteps; ++step) {
        const double fraction = static_cast<double>(step) / kInteriorCoreSteps;
        const QPointF candidate = curveMiddle
            + (segment.control - curveMiddle) * fraction;
        if (target.contains(candidate)
            && chordInsideTarget(segment.start, candidate, target)
            && chordInsideTarget(candidate, segment.end, target)) {
            return candidate;
        }
    }
    return std::nullopt;
}

struct InwardCurvePlacement {
    CurvePlacement curve;
    QPointF coreMiddle;
};

std::optional<InwardCurvePlacement> inwardCurvePlacement(
    const QVector<CurvePrimitive> &arcs,
    const PenBoundarySegment &segment,
    const QPainterPath &target,
    const QPainterPath &legalEnvelope,
    double targetArea,
    double boundaryTolerance,
    cover::GpuAreaEvaluator *gpuEvaluator,
    const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
    const std::function<bool()> &cancelled,
    const std::function<void()> &exactProgress) {
    QPolygonF controlPoints({segment.start, segment.control, segment.end});
    const QRectF bounds = controlPoints.boundingRect();
    const double diagonal = std::hypot(bounds.width(), bounds.height());
    const double maximumError = std::max(
        boundaryTolerance, diagonal * kMaximumSpanErrorRatio);
    const QVector<QPointF> samples = sampleSegment(segment, kCurveSamples);
    const auto evaluated = evaluateCurveTransforms(
        curveTransformCandidates(arcs, samples, maximumError, false),
        samples, target, legalEnvelope, targetArea, maximumError,
        gpuEvaluator, gpuMeshes, cancelled,
        [&](const CurveTransformCandidate &candidate) {
            return candidate.hasCoreMiddle
                && target.contains(candidate.coreMiddle)
                && chordInsideTarget(
                    segment.start, candidate.coreMiddle, target)
                && chordInsideTarget(
                    candidate.coreMiddle, segment.end, target);
        },
        exactProgress);
    return evaluated
        ? std::optional<InwardCurvePlacement>(
              InwardCurvePlacement{evaluated->curve, evaluated->coreMiddle})
        : std::nullopt;
}

std::optional<CurvePlacement> outwardSpanPlacement(const QVector<CurvePrimitive> &caps,
                                                    const QVector<PenBoundarySegment> &segments,
                                                   int first,
                                                    int last,
                                                   const QPainterPath &target,
                                                   const QPainterPath &legalEnvelope,
                                                   double targetArea,
                                                   double maximumBoundaryError,
                                                   cover::GpuAreaEvaluator *gpuEvaluator,
                                                   const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
                                                   const std::function<bool()> &cancelled,
                                                   const std::function<void()> &exactProgress) {
    const QVector<QPointF> samples = sampleSegmentSpan(segments, first, last);
    if (!chordInsideTarget(samples.front(), samples.back(), target)) {
        return std::nullopt;
    }
    const auto evaluated = evaluateCurveTransforms(
        curveTransformCandidates(
            caps, samples, maximumBoundaryError, true),
        samples, target, legalEnvelope, targetArea,
        maximumBoundaryError, gpuEvaluator, gpuMeshes, cancelled, {},
        exactProgress);
    return evaluated
        ? std::optional<CurvePlacement>(evaluated->curve) : std::nullopt;
}

QVector<PenBoundarySegment> curvatureOrderedSegments(
    const QVector<PenBoundarySegment> &segments) {
    if (segments.size() < 2) {
        return segments;
    }
    int seam = 0;
    double bestSeparation = -1.0;
    for (int i = 0; i < segments.size(); ++i) {
        const PenBoundarySegment &left = segments[(i + segments.size() - 1) % segments.size()];
        const PenBoundarySegment &right = segments[i];
        double separation = junctionSeparation(left, right);
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

struct StraightBoundarySpanPlacement {
    int first = 0;
    int last = 0;
    CurvePlacement shape;
    QVector<QPointF> corePoints;
    PenBoundaryFitKind kind = PenBoundaryFitKind::None;
};

void recordBoundaryOwnership(
    PenPlacement *placement,
    PenBoundaryFitKind kind,
    int loopIndex,
    int first,
    int last,
    const QVector<PenBoundarySegment> &segments) {
    placement->boundaryFitKind = kind;
    placement->boundaryLoopIndex = loopIndex;
    placement->boundaryFirstSegment = first;
    placement->boundaryLastSegment = last;
    placement->boundaryStart = segments[first].start;
    placement->boundaryEnd = segments[last].end;
    placement->exposedContourSegments = last - first + 1;
    for (int segmentIndex = first; segmentIndex <= last; ++segmentIndex) {
        const PenBoundarySegment &segment = segments[segmentIndex];
        const double arcLength = sampledArcLength(segment);

        placement->exposedContourArc += arcLength;
        if (segment.curved) {
            placement->exposedCurveArc += arcLength;
            ++placement->exposedCurveSegments;
        }
    }
}

QVector<QPointF> straightSpanCorners(
    const QVector<PenBoundarySegment> &segments,
    int first,
    int last,
    double tolerance) {
    QVector<QPointF> result;
    result.reserve(last - first + 2);
    result.push_back(segments[first].start);
    for (int segmentIndex = first; segmentIndex <= last; ++segmentIndex) {
        result.push_back(segments[segmentIndex].end);
    }

    bool changed = true;
    while (changed && result.size() > 2) {
        changed = false;
        for (int pointIndex = 1; pointIndex + 1 < result.size(); ++pointIndex) {
            const QPointF incoming = result[pointIndex] - result[pointIndex - 1];
            const QPointF outgoing = result[pointIndex + 1] - result[pointIndex];
            if (QPointF::dotProduct(incoming, outgoing) < -kEpsilon
                || distanceSquaredToSegment(
                       result[pointIndex],
                       result[pointIndex - 1],
                       result[pointIndex + 1]) > tolerance * tolerance) {
                continue;
            }
            result.remove(pointIndex);
            changed = true;
            break;
        }
    }

    return result;
}

QVector<QPointF> straightSpanSamples(
    const QVector<PenBoundarySegment> &segments,
    int first,
    int last) {
    QVector<QPointF> result;
    result.reserve((last - first + 1) * 2 + 1);
    result.push_back(segments[first].start);
    for (int segmentIndex = first; segmentIndex <= last; ++segmentIndex) {
        result.push_back(segmentPoint(segments[segmentIndex], 0.5));
        result.push_back(segments[segmentIndex].end);
    }

    return result;
}

std::optional<CurvePlacement> straightBoundaryPlacement(
    const PenPrimitive &primitive,
    const QPolygonF &source,
    const QVector<QPointF> &corners,
    const QVector<QPointF> &samples,
    const QPainterPath &target,
    double targetArea,
    double boundaryTolerance) {
    if (source.size() != corners.size() || corners.size() < 3) {
        return std::nullopt;
    }

    std::optional<CurvePlacement> best;
    const int count = source.size();
    for (int direction : {-1, 1}) {
        for (int offset = 0; offset < count; ++offset) {
            const auto sourcePoint = [&](int index) {
                return source[(offset + direction * index + count * 2) % count];
            };
            bool transformValid = false;
            const QTransform transform = affineFromTriangles(
                sourcePoint(0), sourcePoint(1), sourcePoint(count - 1),
                corners[0], corners[1], corners[count - 1],
                &transformValid);
            if (!transformValid) {
                continue;
            }
            bool verticesMatch = true;
            for (int pointIndex = 0; pointIndex < count; ++pointIndex) {
                if (QLineF(transform.map(sourcePoint(pointIndex)),
                           corners[pointIndex]).length() > boundaryTolerance) {
                    verticesMatch = false;
                    break;
                }
            }
            if (!verticesMatch) {
                continue;
            }
            const auto placement = evaluateCurvePlacement(
                primitive, samples, transform, target, target,
                targetArea, boundaryTolerance);
            if (placement
                && placement->outsideArea / targetArea
                    <= kMaximumStraightBoundaryOutsideRatio
                && (!best || betterCurvePlacement(*placement, *best))) {
                best = placement;
            }
        }
    }

    return best;
}

std::optional<StraightBoundarySpanPlacement> straightBoundaryStrip(
    const QVector<PenBoundarySegment> &segments,
    int first,
    int last,
    const PenPrimitive &square,
    const QPolygonF &source,
    const QPainterPath &target,
    double targetArea,
    double boundaryTolerance) {
    const QPointF start = segments[first].start;
    const QPointF end = segments[last].end;
    const QPointF edge = end - start;
    const double length = QLineF(start, end).length();
    const QVector<QPointF> samples = straightSpanSamples(
        segments, first, last);
    std::optional<StraightBoundarySpanPlacement> best;

    if (length <= boundaryTolerance) {
        return std::nullopt;
    }
    const QPointF normal(-edge.y() / length, edge.x() / length);
    for (double side : {-1.0, 1.0}) {
        double depth = length;
        for (int step = 0; step < kStraightStripDepthSteps; ++step) {
            const QPointF inset = normal * (side * depth);
            const QVector<QPointF> corners{
                start, end, end + inset, start + inset};
            const auto placement = straightBoundaryPlacement(
                square, source, corners, samples, target,
                targetArea, boundaryTolerance);
            if (placement) {
                StraightBoundarySpanPlacement candidate;
                candidate.first = first;
                candidate.last = last;
                candidate.shape = *placement;
                candidate.corePoints = {
                    start + inset, end + inset, end};
                candidate.kind = PenBoundaryFitKind::StraightSquare;
                if (!best || betterCurvePlacement(
                                 candidate.shape, best->shape)) {
                    best = std::move(candidate);
                }
                break;
            }
            depth *= 0.5;
            if (depth < boundaryTolerance) {
                break;
            }
        }
    }

    return best;
}

struct StraightBoundarySelectionCost {
    bool valid = false;
    int matchedSegments = 0;
    int removedVertices = 0;
    int placementCount = 0;
    double matchedArc = 0.0;
    double insideArea = 0.0;
    int decision = -1;
};

bool betterStraightBoundarySelection(
    const StraightBoundarySelectionCost &candidate,
    const StraightBoundarySelectionCost &best) {
    if (!best.valid) {
        return true;
    }
    if (std::abs(candidate.matchedArc - best.matchedArc) > kEpsilon) {
        return candidate.matchedArc > best.matchedArc;
    }
    if (candidate.removedVertices != best.removedVertices) {
        return candidate.removedVertices > best.removedVertices;
    }
    if (candidate.placementCount != best.placementCount) {
        return candidate.placementCount < best.placementCount;
    }
    if (candidate.matchedSegments != best.matchedSegments) {
        return candidate.matchedSegments > best.matchedSegments;
    }

    return candidate.insideArea > best.insideArea;
}

QVector<StraightBoundarySpanPlacement> selectStraightBoundarySpans(
    const QVector<PenBoundarySegment> &segments,
    const QVector<bool> &occupied,
    const QVector<PenPrimitive> &primitives,
    const PolygonMeshSources &meshSources,
    const QPainterPath &target,
    double targetArea,
    double boundaryTolerance,
    const std::function<bool()> &cancelled,
    bool *wasCancelled) {
    const PenPrimitive *square = primitiveForId(primitives, kSquareShapeId);
    const PenPrimitive *triangle = primitiveForId(primitives, kTriangleShapeId);
    const int count = segments.size();
    QVector<StraightBoundarySpanPlacement> candidates;
    QVector<QVector<int>> candidatesAt(count);

    if (count < kMinimumStraightBoundarySegmentCount
        || square == nullptr || triangle == nullptr || !meshSources.valid()) {
        return {};
    }
    for (int first = 0; first < count; ++first) {
        if (occupied[first] || segments[first].curved) {
            continue;
        }
        for (int last = first + 1; last < count; ++last) {
            if (cancelled && cancelled()) {
                *wasCancelled = true;
                return {};
            }
            if (occupied[last] || segments[last].curved) {
                break;
            }
            const QVector<QPointF> corners = straightSpanCorners(
                segments, first, last, boundaryTolerance);
            const PenPrimitive *primitive = nullptr;
            const QPolygonF *source = nullptr;
            PenBoundaryFitKind kind = PenBoundaryFitKind::None;
            if (corners.size() == 2 && last - first + 1 >= 4) {
                const auto strip = straightBoundaryStrip(
                    segments, first, last, *square, meshSources.square,
                    target, targetArea, boundaryTolerance);
                if (strip) {
                    candidatesAt[first].push_back(candidates.size());
                    candidates.push_back(*strip);
                }
                continue;
            } else if (corners.size() == 3) {
                primitive = triangle;
                source = &meshSources.triangle;
                kind = PenBoundaryFitKind::StraightTriangle;
            } else if (corners.size() == 4) {
                primitive = square;
                source = &meshSources.square;
                kind = PenBoundaryFitKind::StraightSquare;
            } else {
                continue;
            }
            double depth = 0.0;
            for (int pointIndex = 1;
                 pointIndex + 1 < corners.size(); ++pointIndex) {
                depth = std::max(
                    depth,
                    std::sqrt(distanceSquaredToSegment(
                        corners[pointIndex], corners.front(), corners.back())));
            }
            if (depth <= boundaryTolerance
                    * kStraightBoundaryMinimumDepthMultiplier) {
                continue;
            }
            const QVector<QPointF> samples = straightSpanSamples(
                segments, first, last);
            const auto placement = straightBoundaryPlacement(
                *primitive, *source, corners, samples, target,
                targetArea, boundaryTolerance);
            if (!placement) {
                continue;
            }
            candidatesAt[first].push_back(candidates.size());
            candidates.push_back({
                first, last, *placement, {segments[last].end}, kind});
        }
    }

    QVector<StraightBoundarySelectionCost> costs(count + 1);
    costs[count].valid = true;
    for (int segmentIndex = count - 1; segmentIndex >= 0; --segmentIndex) {
        StraightBoundarySelectionCost best = costs[segmentIndex + 1];
        best.decision = -1;
        for (int candidateIndex : candidatesAt[segmentIndex]) {
            const StraightBoundarySpanPlacement &candidate =
                candidates[candidateIndex];
            StraightBoundarySelectionCost cost = costs[candidate.last + 1];
            if (!cost.valid) {
                continue;
            }
            const int matchedSegments = candidate.last - candidate.first + 1;
            double matchedArc = 0.0;
            for (int index = candidate.first; index <= candidate.last; ++index) {
                matchedArc += sampledArcLength(segments[index]);
            }
            cost.matchedSegments += matchedSegments;
            cost.removedVertices += std::max(
                0, matchedSegments
                    - static_cast<int>(candidate.corePoints.size()));
            ++cost.placementCount;
            cost.matchedArc += matchedArc;
            cost.insideArea += candidate.shape.insideArea;
            cost.decision = candidateIndex;
            if (betterStraightBoundarySelection(cost, best)) {
                best = cost;
            }
        }
        best.valid = true;
        costs[segmentIndex] = best;
    }

    QVector<StraightBoundarySpanPlacement> selected;
    for (int segmentIndex = 0; segmentIndex < count;) {
        const int decision = costs[segmentIndex].decision;
        if (decision < 0) {
            ++segmentIndex;
            continue;
        }
        selected.push_back(candidates[decision]);
        segmentIndex = candidates[decision].last + 1;
    }

    return selected;
}

struct CoreLayout {
    QVector<QPointF> points;
    QVector<int> spanAtStart;
    QVector<int> inwardSpanAtStart;
    QVector<int> straightSpanAtStart;
    QSet<int> supportedCurveSegments;
};

enum class CoreFitKind {
    None,
    Span,
    InwardSpan,
    StraightSpan,
    InwardCurve,
};

struct SpanSelectionCost {
    bool valid = false;
    int fallbackSegments = 0;
    int placementCount = 0;
    int matchedSegments = 0;
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
    if (candidate.matchedSegments != best.matchedSegments) {
        return candidate.matchedSegments > best.matchedSegments;
    }
    if (std::abs(candidate.boundaryError - best.boundaryError) > kEpsilon) {
        return candidate.boundaryError < best.boundaryError;
    }
    return candidate.outsideArea < best.outsideArea;
}

QVector<CurveSpanPlacement> selectCurveSpans(const QVector<CurvePrimitive> &caps,
                                              const QVector<PenBoundarySegment> &segments,
                                             const QPainterPath &target,
                                             const QPainterPath &legalEnvelope,
                                             double targetArea,
                                             double orientationSign,
                                              double boundaryTolerance,
                                              const std::function<bool()> &cancelled,
                                              const std::function<void(int, int)> &progress,
                                              cover::GpuAreaEvaluator *gpuEvaluator,
                                              const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
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
    const int maximumSingleEvaluations = std::min(
        count, std::max(1, maximumEvaluations / 3));
    int evaluations = 0;
    int exactEvaluations = 0;
    const auto exactProgress = [&]() {
        ++exactEvaluations;
        if (progress) {
            progress(exactEvaluations,
                     maximumEvaluations * kMaximumExactCurveAttempts);
        }
    };
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
                                                         legalEnvelope,
                                                         targetArea,
                                                         maximumError,
                                                         gpuEvaluator,
                                                         gpuMeshes,
                                                         cancelled,
                                                         exactProgress)) {
            candidatesAt[i].push_back(candidates.size());
            candidates.push_back({i, i, *placement});
        }
    }
    int runStart = 0;
    while (runStart < count) {
        while (runStart < count
               && segments[runStart].curved
               && !outward[runStart]) {
            ++runStart;
        }
        if (runStart >= count) {
            break;
        }
        int runEnd = runStart;
        while (runEnd + 1 < count
               && (outward[runEnd + 1]
                   || !segments[runEnd + 1].curved)
               && !tangentBreak(segments[runEnd], segments[runEnd + 1])) {
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
                bool containsOutwardCurve = false;
                QPolygonF spanControlPoints;
                for (int i = first; i <= last; ++i) {
                    containsOutwardCurve = containsOutwardCurve || outward[i];
                    spanControlPoints.push_back(segments[i].start);
                    if (segments[i].curved) {
                        spanControlPoints.push_back(segments[i].control);
                    }
                    spanControlPoints.push_back(segments[i].end);
                }
                if (!containsOutwardCurve) {
                    continue;
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
                                                             legalEnvelope,
                                                             targetArea,
                                                             maximumError,
                                                             gpuEvaluator,
                                                             gpuMeshes,
                                                             cancelled,
                                                             exactProgress);
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
            cost.matchedSegments += candidate.last - candidate.first + 1;
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

struct InwardCurveSpanPlacement {
    int first = 0;
    int last = 0;
    InwardCurvePlacement curve;
};

std::optional<InwardCurvePlacement> inwardSpanPlacement(
    const QVector<CurvePrimitive> &arcs,
    const QVector<PenBoundarySegment> &segments,
    int first,
    int last,
    const QPainterPath &target,
    const QPainterPath &legalEnvelope,
    double targetArea,
    double boundaryTolerance,
    cover::GpuAreaEvaluator *gpuEvaluator,
    const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
    const std::function<bool()> &cancelled,
    const std::function<void()> &exactProgress) {
    const QVector<QPointF> samples = sampleSegmentSpan(segments, first, last);
    const QRectF bounds = QPolygonF(samples).boundingRect();
    const double maximumError = std::max(
        boundaryTolerance,
        std::hypot(bounds.width(), bounds.height())
            * kMaximumSpanErrorRatio);
    const auto evaluated = evaluateCurveTransforms(
        curveTransformCandidates(arcs, samples, maximumError, false),
        samples, target, legalEnvelope, targetArea, maximumError,
        gpuEvaluator, gpuMeshes, cancelled,
        [&](const CurveTransformCandidate &candidate) {
            return candidate.hasCoreMiddle
                && target.contains(candidate.coreMiddle)
                && chordInsideTarget(
                    samples.front(), candidate.coreMiddle, target)
                && chordInsideTarget(
                    candidate.coreMiddle, samples.back(), target);
        },
        exactProgress);
    return evaluated
        ? std::optional<InwardCurvePlacement>(
              InwardCurvePlacement{evaluated->curve, evaluated->coreMiddle})
        : std::nullopt;
}

QVector<InwardCurveSpanPlacement> selectInwardCurveSpans(
    const QVector<CurvePrimitive> &arcs,
    const QVector<PenBoundarySegment> &segments,
    const QVector<bool> &unavailable,
    const QPainterPath &target,
    const QPainterPath &legalEnvelope,
    double targetArea,
    double orientationSign,
    double boundaryTolerance,
    const std::function<bool()> &cancelled,
    const std::function<void(int, int)> &progress,
    cover::GpuAreaEvaluator *gpuEvaluator,
    const QHash<int, const cover::ShapeMesh *> &gpuMeshes,
    bool *wasCancelled) {
    const int count = segments.size();
    const int maximumEvaluations = curveEvaluationBudget(count);
    QVector<InwardCurveSpanPlacement> candidates;
    QVector<QVector<int>> candidatesAt(count);
    QVector<bool> inward(count, false);
    for (int index = 0; index < count; ++index) {
        inward[index] = !unavailable[index]
            && segments[index].curved
            && !isOutwardCurve(segments[index], orientationSign);
    }
    int evaluations = 0;
    int exactEvaluations = 0;
    const auto exactProgress = [&]() {
        ++exactEvaluations;
        if (progress) {
            progress(exactEvaluations,
                     maximumEvaluations * kMaximumExactCurveAttempts);
        }
    };
    int runStart = 0;
    while (runStart < count && evaluations < maximumEvaluations) {
        while (runStart < count && !inward[runStart]) {
            ++runStart;
        }
        if (runStart >= count) {
            break;
        }
        int runEnd = runStart;
        while (runEnd + 1 < count && inward[runEnd + 1]
               && !curvatureBreak(segments[runEnd], segments[runEnd + 1])) {
            ++runEnd;
        }
        const int runLength = runEnd - runStart + 1;
        for (int spanLength = runLength;
             spanLength >= 2 && evaluations < maximumEvaluations;
             --spanLength) {
            for (int first = runStart;
                 first + spanLength - 1 <= runEnd
                     && evaluations < maximumEvaluations;
                 ++first) {
                if (cancelled && cancelled()) {
                    *wasCancelled = true;
                    return {};
                }
                const int last = first + spanLength - 1;
                ++evaluations;
                const auto placement = inwardSpanPlacement(
                    arcs, segments, first, last, target, legalEnvelope,
                    targetArea, boundaryTolerance, gpuEvaluator, gpuMeshes,
                    cancelled,
                    exactProgress);
                if (!placement) {
                    continue;
                }
                candidatesAt[first].push_back(candidates.size());
                candidates.push_back({first, last, *placement});
            }
        }
        runStart = runEnd + 1;
    }

    QVector<SpanSelectionCost> costs(count + 1);
    costs[count].valid = true;
    for (int index = count - 1; index >= 0; --index) {
        SpanSelectionCost best = costs[index + 1];
        best.fallbackSegments += inward[index] ? 1 : 0;
        best.decision = -1;
        for (const int candidateIndex : candidatesAt[index]) {
            const InwardCurveSpanPlacement &candidate = candidates[candidateIndex];
            SpanSelectionCost cost = costs[candidate.last + 1];
            if (!cost.valid) {
                continue;
            }
            ++cost.placementCount;
            cost.matchedSegments += candidate.last - candidate.first + 1;
            cost.boundaryError += candidate.curve.curve.boundaryError;
            cost.outsideArea += candidate.curve.curve.outsideArea;
            cost.decision = candidateIndex;
            if (betterSpanSelection(cost, best)) {
                best = cost;
            }
        }
        best.valid = true;
        costs[index] = best;
    }
    QVector<InwardCurveSpanPlacement> selected;
    for (int index = 0; index < count;) {
        const int decision = costs[index].decision;
        if (decision < 0) {
            ++index;
            continue;
        }
        selected.push_back(candidates[decision]);
        index = candidates[decision].last + 1;
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
        return result;
    }
    result.loops.push_back({result.path, result.segments, PenLoopKind::Outer});
    return result;
}

PenContour buildPenContour(const QVector<PenLoop> &loops, double flatnessTolerance) {
    PenContour result;
    if (loops.isEmpty()) {
        result.error = QStringLiteral("A Pen contour needs an outer path");
        return result;
    }

    int outerIndex = -1;
    for (int loopIndex = 0; loopIndex < loops.size(); ++loopIndex) {
        if (loops[loopIndex].kind != PenLoopKind::Outer) {
            continue;
        }
        if (outerIndex >= 0) {
            result.error = QStringLiteral("A Pen contour can only have one outer path");
            return result;
        }
        outerIndex = loopIndex;
    }
    if (outerIndex < 0) {
        result.error = QStringLiteral("A Pen contour needs an outer path");
        return result;
    }
    if (outerIndex != 0) {
        result.error = QStringLiteral("The outer Pen path must be the first boundary");
        return result;
    }

    result.loops.reserve(loops.size());
    for (int loopIndex = 0; loopIndex < loops.size(); ++loopIndex) {
        PenContour loop = buildPenContour(loops[loopIndex].points, flatnessTolerance);
        if (!loop.valid()) {
            result.crossings += loop.crossings;
            result.error = loop.error.isEmpty()
                ? QStringLiteral("Invalid Pen contour boundary")
                : loop.error;
            return result;
        }
        result.loops.push_back({loop.path, loop.segments, loops[loopIndex].kind});
        result.segments += loop.segments;
    }

    const PenContourLoop &outer = result.loops[outerIndex];
    for (int leftIndex = 0; leftIndex < result.loops.size(); ++leftIndex) {
        if (leftIndex == outerIndex) {
            continue;
        }
        const PenContourLoop &cutout = result.loops[leftIndex];
        if (cutout.kind != PenLoopKind::Cutout
            || cutout.segments.isEmpty()
            || !outer.path.contains(cutout.segments.front().start)) {
            result.error = QStringLiteral("A Pen cutout must be inside the outer path");
            return result;
        }
        for (int rightIndex = leftIndex + 1;
             rightIndex < result.loops.size(); ++rightIndex) {
            if (rightIndex == outerIndex) {
                continue;
            }
            if (cutout.path.intersects(result.loops[rightIndex].path)
                || cutout.path.contains(result.loops[rightIndex].segments.front().start)
                || result.loops[rightIndex].path.contains(cutout.segments.front().start)) {
                result.error = QStringLiteral("Pen cutouts cannot overlap or contain one another");
                return result;
            }
        }
    }

    for (int leftIndex = 0; leftIndex < result.loops.size(); ++leftIndex) {
        for (int rightIndex = leftIndex + 1;
             rightIndex < result.loops.size(); ++rightIndex) {
            result.crossings += contourPairCrossings(
                result.loops[leftIndex].segments,
                result.loops[rightIndex].segments,
                std::max(flatnessTolerance, 1e-5));
        }
    }
    if (!result.crossings.isEmpty()) {
        result.error = QStringLiteral("Pen contour boundaries cross");
        return result;
    }

    const auto flattenedLoop = [](const PenContourLoop &loop) {
        PenContour contour;
        contour.segments = loop.segments;
        return flattenedContour(contour, 16);
    };
    const double outerOrientation = signedArea(flattenedLoop(result.loops[outerIndex]));
    result.path.setFillRule(Qt::WindingFill);
    for (int loopIndex = 0; loopIndex < result.loops.size(); ++loopIndex) {
        const PenContourLoop &loop = result.loops[loopIndex];
        const QPolygonF flattened = flattenedLoop(loop);
        const bool sameOrientation = signedArea(flattened) * outerOrientation >= 0.0;
        const bool reverse = loop.kind == PenLoopKind::Cutout
            ? sameOrientation
            : !sameOrientation;
        result.path.addPath(reverse ? loop.path.toReversed() : loop.path);
    }
    if (pathArea(result.path) <= kEpsilon) {
        result.path = {};
        result.error = QStringLiteral("The Pen contour has no fillable area");
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
                         const std::function<bool()> &cancelled,
                         const PenFillProgressCallback &progress) {
    PenFillResult result;
    if (request.boundaryTolerance <= 0.0
        || !std::isfinite(request.boundaryTolerance)
        || request.curveTimeBudgetMs < 0) {
        result.error = QStringLiteral("Pen fill options are invalid");
        return result;
    }
    QVector<PenPrimitive> primitives;
    for (const PenPrimitive &primitive : request.primitives) {
        if (isAllowedPenShape(primitive.shapeId)
            || !primitive.curveSegments.isEmpty()) {
            primitives.push_back(primitive);
        }
    }
    QVector<PenLoop> loops = request.loops;
    if (loops.isEmpty() && !request.points.isEmpty()) {
        loops.push_back({request.points, PenLoopKind::Outer});
    }
    const PenContour contour = buildPenContour(
        loops, request.boundaryTolerance * 0.25);
    if (!contour.valid()) {
        result.error = contour.error.isEmpty() ? QStringLiteral("Invalid Pen contour") : contour.error;
        return result;
    }
    result.curveLoopDiagnostics.resize(contour.loops.size());
    const QPainterPath target = request.targetPath.isEmpty()
        ? contour.path : request.targetPath;
    const QPainterPath legalEnvelope = request.legalEnvelope.isEmpty()
        ? (request.curveTimeBudgetMs > 0
               ? expandedPath(target, kDefaultLegalEnvelopeDistance)
               : target)
        : request.legalEnvelope.united(target);
    if (target.isEmpty() || legalEnvelope.isEmpty()) {
        result.error = QStringLiteral("Pen fill target is unavailable");
        return result;
    }
    int pointCount = 0;
    for (const PenLoop &loop : loops) {
        pointCount += loop.points.size();
    }
    result.targetArea = pathArea(target);
    result.shapeLimit = request.shapeLimit > 0
        ? request.shapeLimit : pointCount * 2;
    const PolygonMeshSources meshSources = penMeshSources(primitives);
    QElapsedTimer curveTimer;
    if (request.curveTimeBudgetMs > 0) {
        curveTimer.start();
    }
    const auto curveTimedOut = [&]() {
        return curveTimer.isValid()
            && curveTimer.elapsed() >= request.curveTimeBudgetMs;
    };
    const auto curveCancelled = [&]() {
        return (cancelled && cancelled()) || curveTimedOut();
    };
    const auto reportProgress = [&](const QString &phase,
                                    int completed,
                                    int total,
                                    int placementCount = 0,
                                    double coveredArea = 0.0) {
        if (progress) {
            progress(PenFillProgress{
                phase, completed, total, placementCount,
                result.targetArea, coveredArea,
            });
        }
    };
    reportProgress(QStringLiteral("Indexing contour spans"),
                   0, contour.segments.size());
    QHash<int, const cover::ShapeMesh *> gpuMeshes;
    for (const cover::ShapeMesh &mesh : request.curveMeshes) {
        gpuMeshes.insert(mesh.id, &mesh);
    }
    std::unique_ptr<cover::GpuAreaEvaluator> gpuEvaluator;
    if (request.useGpu && !request.curveMeshes.isEmpty()) {
        reportProgress(QStringLiteral("Initializing curve GPU"), 0, 0);
        gpuEvaluator = cover::createGpuAreaEvaluator(request.curveMeshes);
        const cover::Polygons targetPolygons = cover::polygonsFromPainterPath(
            target, request.boundaryTolerance * 0.25);
        const cover::Polygons legalPolygons = cover::polygonsFromPainterPath(
            legalEnvelope, request.boundaryTolerance * 0.25);
        if (!gpuEvaluator->available()
            || !gpuEvaluator->setSubjects(targetPolygons, legalPolygons)) {
            gpuEvaluator.reset();
        }
    }
    const auto polygonFallback = [&]() {
        PenFillResult fallback;
        fallback.targetArea = result.targetArea;
        fallback.shapeLimit = result.shapeLimit;
        fallback.timedOut = true;
        PolygonMeshRequest meshRequest;
        meshRequest.sources = meshSources;
        meshRequest.mergeSquares = true;
        for (const PenContourLoop &loop : contour.loops) {
            PenContour loopContour;
            loopContour.segments = loop.segments;
            meshRequest.contours.push_back(
                flattenedContour(loopContour, 16));
        }
        if (meshRequest.contours.isEmpty()) {
            meshRequest.contours.push_back(flattenedContour(contour, 16));
        }
        reportProgress(QStringLiteral("Curve timeout mesh fallback"),
                       0, meshRequest.contours.size());
        const PolygonMeshResult mesh = meshPolygon(meshRequest, cancelled);
        if (mesh.cancelled || (cancelled && cancelled())) {
            fallback.cancelled = true;
            fallback.error = QStringLiteral("Curve fill cancelled");
            return fallback;
        }
        if (!mesh.error.isEmpty()) {
            fallback.error = QStringLiteral(
                "Curve timeout fallback failed: %1").arg(mesh.error);
            return fallback;
        }
        QPainterPath coverage;
        coverage.setFillRule(Qt::WindingFill);
        for (const PolygonMeshPlacement &placement : mesh.placements) {
            const PenPrimitive *primitive = primitiveForId(
                primitives, placement.shapeId);
            if (primitive == nullptr) {
                fallback.error = QStringLiteral(
                    "Curve timeout fallback selected unavailable Primitive %1")
                                     .arg(placement.shapeId);
                fallback.placements.clear();
                return fallback;
            }
            fallback.placements.push_back({
                placement.shapeId,
                placement.transform,
                primitive->area
                    * std::abs(placement.transform.determinant()),
                placement.shapeId == kCircleShapeId,
            });
            coverage = coverage.united(
                placement.transform.map(primitive->silhouette));
        }
        if (fallback.placements.size() > fallback.shapeLimit) {
            fallback.error = QStringLiteral(
                "Curve timeout fallback exceeded its shape limit");
            fallback.placements.clear();
            return fallback;
        }
        fallback.coveredArea = pathArea(coverage.intersected(target));
        fallback.outsideArea = pathArea(coverage.subtracted(legalEnvelope));
        fallback.unfilled = target.subtracted(coverage);
        return fallback;
    };
    if (loops.size() > 1) {
        QVector<CurvePrimitive> outwardCaps;
        QVector<CurvePrimitive> inwardCaps;
        for (const PenPrimitive &primitive : primitives) {
            if (!isCurveShape(primitive)) {
                continue;
            }
            CurvePrimitive candidate{&primitive, primitiveArcProfiles(primitive)};
            if (candidate.profiles.isEmpty()
                && !primitive.curveSegments.isEmpty()
                && primitive.shapeId != kCircleShapeId) {
                continue;
            }
            outwardCaps.push_back(candidate);
            if (!primitive.curveSegments.isEmpty()
                || primitive.shapeId == kFangShapeId
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

        QVector<QVector<QPointF>> coreContours;
        coreContours.reserve(contour.loops.size());
        QPainterPath coverage;
        coverage.setFillRule(Qt::WindingFill);
        for (int loopIndex = 0; loopIndex < contour.loops.size(); ++loopIndex) {
            const PenContourLoop &loop = contour.loops[loopIndex];
            PenContour loopContour;
            loopContour.segments = loop.segments;
            const QPolygonF flattened = flattenedContour(loopContour, 16);
            double orientationSign = signedArea(flattened) >= 0.0 ? 1.0 : -1.0;
            if (loop.kind == PenLoopKind::Cutout) {
                orientationSign *= -1.0;
            }
            const QVector<PenBoundarySegment> segments =
                curvatureOrderedSegments(loop.segments);
            bool selectionCancelled = false;
            const QVector<CurveSpanPlacement> curveSpans = selectCurveSpans(
                outwardCaps,
                segments,
                target,
                legalEnvelope,
                result.targetArea,
                orientationSign,
                request.boundaryTolerance,
                curveCancelled,
                [&](int completed, int total) {
                    reportProgress(QStringLiteral("Evaluating curve candidates"),
                                   completed, total,
                                   result.placements.size());
                },
                gpuEvaluator.get(),
                gpuMeshes,
                &selectionCancelled);
            if (selectionCancelled) {
                if (curveTimedOut()
                    && !(cancelled && cancelled())) {
                    return polygonFallback();
                }
                result.cancelled = true;
                result.error = QStringLiteral("Pen curve-span selection timed out");
                return result;
            }

            QVector<bool> spanCovered(segments.size(), false);
            QVector<int> spanAtStart(segments.size(), -1);
            for (int spanIndex = 0; spanIndex < curveSpans.size(); ++spanIndex) {
                const CurveSpanPlacement &span = curveSpans[spanIndex];
                spanAtStart[span.first] = spanIndex;
                for (int segmentIndex = span.first;
                     segmentIndex <= span.last;
                     ++segmentIndex) {
                    spanCovered[segmentIndex] = true;
                }
            }
            const QVector<InwardCurveSpanPlacement> inwardSpans =
                selectInwardCurveSpans(
                    inwardCaps, segments, spanCovered, target,
                    legalEnvelope, result.targetArea, orientationSign,
                    request.boundaryTolerance, curveCancelled,
                    [&](int completed, int total) {
                        reportProgress(
                            QStringLiteral("Matching inward curve spans"),
                            completed, total,
                            result.placements.size());
                    },
                    gpuEvaluator.get(),
                    gpuMeshes,
                    &selectionCancelled);
            if (selectionCancelled) {
                if (curveTimedOut()
                    && !(cancelled && cancelled())) {
                    return polygonFallback();
                }
                result.cancelled = true;
                result.error = QStringLiteral(
                    "Pen inward-span selection timed out");
                return result;
            }
            QVector<int> inwardSpanAtStart(segments.size(), -1);
            for (int spanIndex = 0; spanIndex < inwardSpans.size();
                 ++spanIndex) {
                const InwardCurveSpanPlacement &span = inwardSpans[spanIndex];
                inwardSpanAtStart[span.first] = spanIndex;
                for (int segmentIndex = span.first;
                     segmentIndex <= span.last; ++segmentIndex) {
                    spanCovered[segmentIndex] = true;
                }
            }

            QVector<StraightBoundarySpanPlacement> straightSpans;
            QVector<int> straightSpanAtStart(segments.size(), -1);
            if (loop.kind == PenLoopKind::Outer
                && request.curveMeshes.isEmpty()) {
                straightSpans = selectStraightBoundarySpans(
                    segments, spanCovered, primitives, meshSources,
                    target, result.targetArea, request.boundaryTolerance,
                    curveCancelled, &selectionCancelled);
                if (selectionCancelled) {
                    if (curveTimedOut()
                        && !(cancelled && cancelled())) {
                        return polygonFallback();
                    }
                    result.cancelled = true;
                    result.error = QStringLiteral(
                        "Pen straight-boundary selection timed out");
                    return result;
                }
                for (int spanIndex = 0; spanIndex < straightSpans.size();
                     ++spanIndex) {
                    const StraightBoundarySpanPlacement &span =
                        straightSpans[spanIndex];
                    straightSpanAtStart[span.first] = spanIndex;
                    for (int segmentIndex = span.first;
                         segmentIndex <= span.last; ++segmentIndex) {
                        spanCovered[segmentIndex] = true;
                    }
                }
            }

            QVector<std::optional<InwardCurvePlacement>> inwardCurves(segments.size());
            QVector<int> inwardCandidates;
            for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
                if (segments[segmentIndex].curved
                    && !spanCovered[segmentIndex]
                    && !isOutwardCurve(segments[segmentIndex], orientationSign)) {
                    inwardCandidates.push_back(segmentIndex);
                }
            }
            std::sort(inwardCandidates.begin(), inwardCandidates.end(),
                      [&](int left, int right) {
                const PenBoundarySegment &a = segments[left];
                const PenBoundarySegment &b = segments[right];
                const double aImportance = QLineF(a.start, a.end).length()
                    * std::max(std::sqrt(distanceSquaredToSegment(
                                   a.control, a.start, a.end)),
                               request.boundaryTolerance * 0.25);
                const double bImportance = QLineF(b.start, b.end).length()
                    * std::max(std::sqrt(distanceSquaredToSegment(
                                   b.control, b.start, b.end)),
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
                if (curveCancelled()) {
                    if (curveTimedOut()
                        && !(cancelled && cancelled())) {
                        return polygonFallback();
                    }
                    result.cancelled = true;
                    result.error = QStringLiteral("Pen inward-curve selection timed out");
                    return result;
                }
                const int segmentIndex = inwardCandidates[candidateIndex];
                inwardCurves[segmentIndex] = inwardCurvePlacement(
                    inwardCaps,
                    segments[segmentIndex],
                    target,
                    legalEnvelope,
                    result.targetArea,
                    request.boundaryTolerance,
                    gpuEvaluator.get(), gpuMeshes, curveCancelled, {});
            }

            QVector<QPointF> corePoints;
            corePoints.push_back(segments.front().start);
            for (int segmentIndex = 0; segmentIndex < segments.size();) {
                const int spanIndex = spanAtStart[segmentIndex];
                if (spanIndex >= 0) {
                    const CurveSpanPlacement &span = curveSpans[spanIndex];
                    PenPlacement placement = span.curve.placement;
                    recordBoundaryOwnership(
                        &placement, PenBoundaryFitKind::OutwardSpan,
                        loopIndex, span.first, span.last, segments);
                    result.placements.push_back(std::move(placement));
                    coverage = coverage.united(span.curve.path);
                    corePoints.push_back(segments[span.last].end);
                    segmentIndex = span.last + 1;
                    continue;
                }
                const int inwardSpanIndex = inwardSpanAtStart[segmentIndex];
                if (inwardSpanIndex >= 0) {
                    const InwardCurveSpanPlacement &span =
                        inwardSpans[inwardSpanIndex];
                    PenPlacement placement = span.curve.curve.placement;
                    recordBoundaryOwnership(
                        &placement, PenBoundaryFitKind::InwardSpan,
                        loopIndex, span.first, span.last, segments);
                    result.placements.push_back(std::move(placement));
                    coverage = coverage.united(span.curve.curve.path);
                    corePoints.push_back(span.curve.coreMiddle);
                    corePoints.push_back(segments[span.last].end);
                    segmentIndex = span.last + 1;
                    continue;
                }
                const int straightSpanIndex =
                    straightSpanAtStart[segmentIndex];
                if (straightSpanIndex >= 0) {
                    const StraightBoundarySpanPlacement &span =
                        straightSpans[straightSpanIndex];
                    PenPlacement placement = span.shape.placement;
                    recordBoundaryOwnership(
                        &placement, span.kind, loopIndex,
                        span.first, span.last, segments);
                    result.placements.push_back(std::move(placement));
                    coverage = coverage.united(span.shape.path);
                    for (const QPointF &point : span.corePoints) {
                        corePoints.push_back(point);
                    }
                    segmentIndex = span.last + 1;
                    continue;
                }
                const PenBoundarySegment &segment = segments[segmentIndex];
                if (inwardCurves[segmentIndex]) {
                    PenPlacement placement =
                        inwardCurves[segmentIndex]->curve.placement;
                    recordBoundaryOwnership(
                        &placement, PenBoundaryFitKind::InwardSegment,
                        loopIndex, segmentIndex, segmentIndex, segments);
                    result.placements.push_back(std::move(placement));
                    coverage = coverage.united(inwardCurves[segmentIndex]->curve.path);
                    corePoints.push_back(inwardCurves[segmentIndex]->coreMiddle);
                } else if (segment.curved) {
                    if (!chordInsideTarget(
                            segment.start, segment.end, target)) {
                        const auto point = interiorCorePoint(segment, target);
                        corePoints.push_back(point ? *point : segment.control);
                        ++result.curveLoopDiagnostics[loopIndex]
                              .supportedCurveSegments;
                    } else {
                        ++result.curveLoopDiagnostics[loopIndex]
                              .chordedCurveSegments;
                    }
                }
                corePoints.push_back(segment.end);
                ++segmentIndex;
            }
            QVector<QPointF> normalized;
            normalized.reserve(corePoints.size());
            for (const QPointF &point : std::as_const(corePoints)) {
                if (normalized.isEmpty()
                    || QLineF(normalized.back(), point).length() > kEpsilon) {
                    normalized.push_back(point);
                }
            }
            if (normalized.size() > 1
                && QLineF(normalized.front(), normalized.back()).length() <= kEpsilon) {
                normalized.removeLast();
            }
            if (normalized.size() < 3) {
                result.error = QStringLiteral("The Pen contour left no polygonal core");
                result.placements.clear();
                return result;
            }
            coreContours.push_back(std::move(normalized));
        }

        PolygonMeshRequest meshRequest;
        meshRequest.sources = meshSources;
        meshRequest.mergeSquares = true;
        meshRequest.contours = std::move(coreContours);
        reportProgress(QStringLiteral("Meshing contour core"),
                       0, meshRequest.contours.size(),
                       result.placements.size(),
                       pathArea(coverage.intersected(target)));
        const PolygonMeshResult mesh = meshPolygon(meshRequest, cancelled);
        if (mesh.cancelled || (cancelled && cancelled())) {
            result.placements.clear();
            result.cancelled = true;
            result.error = QStringLiteral("Pen core mesh timed out");
            return result;
        }
        if (!mesh.error.isEmpty()) {
            result.placements.clear();
            result.error = QStringLiteral("Could not fill the Pen core: %1").arg(mesh.error);
            return result;
        }
        const QVector<PolygonMeshPlacement> corePlacements = optimizePolygonMeshWithEllipses(
            mesh.placements, meshSources, mesh.contour, cancelled);
        const CurveCoreOptimization optimizedCore =
            optimizeCurveCoreWithTemplates(
                corePlacements, meshSources, mesh.contour, primitives,
                legalEnvelope, result.targetArea,
                result.shapeLimit - result.placements.size(),
                gpuEvaluator.get(), gpuMeshes, curveCancelled,
                [&](int completed, int total) {
                    reportProgress(QStringLiteral("Packing curve core"),
                                   completed, total,
                                   result.placements.size() + completed);
                });
        if (optimizedCore.placements.isEmpty()) {
            result.placements.clear();
            result.error = QStringLiteral(
                "Pen core selected unavailable Primitive geometry");
            return result;
        }
        for (const PenPlacement &placement : optimizedCore.placements) {
            result.placements.push_back(placement);
        }
        coverage = coverage.united(optimizedCore.coverage);
        if (request.curveTimeBudgetMs > 0) {
            const auto completion = optimizeCoverageWithinSpill(
                result.placements, coverage, target, legalEnvelope,
                primitives, result.targetArea,
                result.shapeLimit - result.placements.size(),
                gpuEvaluator.get(), gpuMeshes, curveCancelled,
                [&](int completed, int total) {
                    reportProgress(
                        QStringLiteral("Completing exact coverage"),
                        completed, total,
                        result.placements.size() + completed);
                });
            if (completion) {
                result.placements = completion->placements;
                coverage = completion->coverage;
            }
        }
        if (result.placements.size() > result.shapeLimit) {
            result.error = QStringLiteral("Pen fill exceeded its shape limit");
            result.placements.clear();
            return result;
        }
        if (request.discardNegligiblePlacements
            && !discardNegligiblePlacements(&result.placements,
                                            &coverage,
                                            primitives,
                                            target,
                                            result.targetArea,
                                            request.boundaryTolerance,
                                            cancelled)) {
            result.placements.clear();
            result.cancelled = true;
            result.error = QStringLiteral("Pen placement cleanup timed out");
            return result;
        }
        result.coveredArea = pathArea(coverage.intersected(target));
        result.outsideArea = pathArea(coverage.subtracted(target));
        result.unfilled = target.subtracted(coverage);
        reportProgress(QStringLiteral("Cleaning placements"),
                       result.placements.size(), result.placements.size(),
                       result.placements.size(), result.coveredArea);
        return result;
    }
    QVector<CurvePrimitive> outwardCaps;
    QVector<CurvePrimitive> inwardCaps;
    for (const PenPrimitive &primitive : primitives) {
        if (!isCurveShape(primitive)) {
            continue;
        }
        CurvePrimitive candidate{&primitive, primitiveArcProfiles(primitive)};
        if (candidate.profiles.isEmpty()
            && !primitive.curveSegments.isEmpty()
            && primitive.shapeId != kCircleShapeId) {
            continue;
        }
        outwardCaps.push_back(candidate);
        if (!primitive.curveSegments.isEmpty()
            || primitive.shapeId == kFangShapeId
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
    const QPolygonF flattened = flattenedContour(contour, 16);
    const double orientationSign = signedArea(flattened) >= 0.0 ? 1.0 : -1.0;
    const QVector<PenBoundarySegment> segments = curvatureOrderedSegments(
        contour.segments);
    bool selectionCancelled = false;
    const QVector<CurveSpanPlacement> curveSpans = selectCurveSpans(outwardCaps,
                                                                    segments,
                                                                   target,
                                                                   legalEnvelope,
                                                                   result.targetArea,
                                                                   orientationSign,
                                                                    request.boundaryTolerance,
                                                                    curveCancelled,
                                                                    [&](int completed,
                                                                        int total) {
                                                                        reportProgress(
                                                                            QStringLiteral("Evaluating curve candidates"),
                                                                            completed, total);
                                                                    },
                                                                    gpuEvaluator.get(),
                                                                    gpuMeshes,
                                                                    &selectionCancelled);
    if (selectionCancelled) {
        if (curveTimedOut() && !(cancelled && cancelled())) {
            return polygonFallback();
        }
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
    const QVector<InwardCurveSpanPlacement> inwardSpans =
        selectInwardCurveSpans(
            inwardCaps, segments, spanCovered, target, legalEnvelope,
            result.targetArea, orientationSign, request.boundaryTolerance,
            curveCancelled,
            [&](int completed, int total) {
                reportProgress(QStringLiteral("Matching inward curve spans"),
                               completed, total);
            },
            gpuEvaluator.get(),
            gpuMeshes,
            &selectionCancelled);
    if (selectionCancelled) {
        if (curveTimedOut() && !(cancelled && cancelled())) {
            return polygonFallback();
        }
        result.cancelled = true;
        result.error = QStringLiteral("Pen inward-span selection timed out");
        return result;
    }
    for (const InwardCurveSpanPlacement &span : inwardSpans) {
        for (int index = span.first; index <= span.last; ++index) {
            spanCovered[index] = true;
        }
    }
    QVector<StraightBoundarySpanPlacement> straightSpans;
    if (request.curveMeshes.isEmpty()) {
        straightSpans = selectStraightBoundarySpans(
            segments, spanCovered, primitives, meshSources,
            target, result.targetArea, request.boundaryTolerance,
            curveCancelled, &selectionCancelled);
    }
    if (selectionCancelled) {
        if (curveTimedOut() && !(cancelled && cancelled())) {
            return polygonFallback();
        }
        result.cancelled = true;
        result.error = QStringLiteral(
            "Pen straight-boundary selection timed out");
        return result;
    }
    for (const StraightBoundarySpanPlacement &span : straightSpans) {
        for (int index = span.first; index <= span.last; ++index) {
            spanCovered[index] = true;
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
                                                   target,
                                                   legalEnvelope,
                                                   result.targetArea,
                                                   request.boundaryTolerance,
                                                   gpuEvaluator.get(),
                                                   gpuMeshes,
                                                   curveCancelled, {});
        }
    }
    QVector<bool> activeSpans(curveSpans.size(), true);
    QVector<bool> activeInwardSpans(inwardSpans.size(), true);
    QVector<bool> activeStraightSpans(straightSpans.size(), true);
    QVector<bool> activeInwardCurves(segments.size(), false);
    for (int i = 0; i < inwardCurves.size(); ++i) {
        activeInwardCurves[i] = inwardCurves[i].has_value();
    }
    const auto coreLayout = [&](const QVector<bool> &enabledSpans,
                                 const QVector<bool> &enabledInwardSpans,
                                 const QVector<bool> &enabledStraightSpans,
                                 const QVector<bool> &enabledInwardCurves) {
        CoreLayout layout;
        layout.spanAtStart.fill(-1, segments.size());
        layout.inwardSpanAtStart.fill(-1, segments.size());
        layout.straightSpanAtStart.fill(-1, segments.size());
        QVector<bool> activeSpanCovered(segments.size(), false);
        int activeSpanCount = 0;
        int activeInwardSpanCount = 0;
        int activeStraightSpanCount = 0;
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
        for (int spanIndex = 0; spanIndex < inwardSpans.size();
             ++spanIndex) {
            if (!enabledInwardSpans[spanIndex]) {
                continue;
            }
            const InwardCurveSpanPlacement &span = inwardSpans[spanIndex];
            layout.inwardSpanAtStart[span.first] = spanIndex;
            activeRemovedVertices += span.last - span.first;
            ++activeInwardSpanCount;
            for (int index = span.first; index <= span.last; ++index) {
                activeSpanCovered[index] = true;
            }
        }
        for (int spanIndex = 0; spanIndex < straightSpans.size();
             ++spanIndex) {
            if (!enabledStraightSpans[spanIndex]) {
                continue;
            }
            const StraightBoundarySpanPlacement &span =
                straightSpans[spanIndex];
            layout.straightSpanAtStart[span.first] = spanIndex;
            activeRemovedVertices += std::max(
                0, span.last - span.first + 1
                    - static_cast<int>(span.corePoints.size()));
            ++activeStraightSpanCount;
            for (int index = span.first; index <= span.last; ++index) {
                activeSpanCovered[index] = true;
            }
        }
        QHash<int, QPointF> interiorCorePoints;
        QVector<int> activeMidpointCandidates;
        for (int i = 0; i < segments.size(); ++i) {
            if (enabledInwardCurves[i]) {
                ++activeInwardCount;
            } else if (segments[i].curved && !activeSpanCovered[i]) {
                const QPointF curveMiddle = segmentPoint(segments[i], 0.5);
                if (chordInsideTarget(segments[i].start,
                                      segments[i].end,
                                      target)) {
                    activeMidpointCandidates.push_back(i);
                } else if (chordInsideTarget(segments[i].start,
                                             curveMiddle,
                                             target)
                           && chordInsideTarget(curveMiddle,
                                                segments[i].end,
                                                target)) {
                    interiorCorePoints.insert(i, curveMiddle);
                } else {
                    const auto point = interiorCorePoint(segments[i], target);
                    if (point) {
                        interiorCorePoints.insert(i, *point);
                    } else {
                        interiorCorePoints.insert(i, segments[i].control);
                    }
                }
            }
        }
        const int capCount = activeSpanCount + activeInwardSpanCount
            + activeStraightSpanCount + activeInwardCount;
        const int baseVertices = segments.size() - activeRemovedVertices
            + activeInwardSpanCount + activeInwardCount
            + interiorCorePoints.size();
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
        layout.supportedCurveSegments = midpointSegments;
        for (auto iterator = interiorCorePoints.cbegin();
             iterator != interiorCorePoints.cend(); ++iterator) {
            layout.supportedCurveSegments.insert(iterator.key());
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
            const int inwardSpanIndex = layout.inwardSpanAtStart[i];
            if (inwardSpanIndex >= 0) {
                const InwardCurveSpanPlacement &span =
                    inwardSpans[inwardSpanIndex];
                layout.points.push_back(span.curve.coreMiddle);
                layout.points.push_back(segments[span.last].end);
                i = span.last + 1;
                continue;
            }
            const int straightSpanIndex = layout.straightSpanAtStart[i];
            if (straightSpanIndex >= 0) {
                const StraightBoundarySpanPlacement &span =
                    straightSpans[straightSpanIndex];
                for (const QPointF &point : span.corePoints) {
                    layout.points.push_back(point);
                }
                i = span.last + 1;
                continue;
            }
            const PenBoundarySegment &segment = segments[i];
            if (enabledInwardCurves[i]) {
                layout.points.push_back(inwardCurves[i]->coreMiddle);
            } else if (interiorCorePoints.contains(i)) {
                layout.points.push_back(interiorCorePoints.value(i));
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
    CoreLayout layout = coreLayout(
        activeSpans, activeInwardSpans,
        activeStraightSpans, activeInwardCurves);
    PolygonContour coreContour = buildPolygonContour(layout.points);
    const double maximumCoreOutsideArea =
        std::max(1e-6, result.targetArea * kMaximumCoreOutsideRatio);
    const auto coreOutsideArea = [&](const PolygonContour &candidateContour) {
        if (!candidateContour.valid()) {
            return std::numeric_limits<double>::max();
        }
        return pathArea(candidateContour.path.subtracted(legalEnvelope));
    };
    double outsideCoreArea = coreOutsideArea(coreContour);
    while (!coreContour.valid() || outsideCoreArea > maximumCoreOutsideArea) {
        CoreFitKind bestKind = CoreFitKind::None;
        int bestIndex = -1;
        int bestCrossingCount = std::numeric_limits<int>::max();
        double bestOutsideArea = std::numeric_limits<double>::max();
        CoreLayout bestLayout;
        PolygonContour bestContour;
        const auto consider = [&](CoreFitKind kind, int index) {
            CoreLayout candidateLayout = coreLayout(
                activeSpans, activeInwardSpans,
                activeStraightSpans, activeInwardCurves);
            PolygonContour candidateContour = buildPolygonContour(candidateLayout.points);
            const double candidateOutsideArea =
                coreOutsideArea(candidateContour);
            const int crossingCount = candidateContour.crossings.size();
            if (crossingCount < bestCrossingCount
                || (crossingCount == bestCrossingCount
                    && candidateOutsideArea < bestOutsideArea)) {
                bestKind = kind;
                bestIndex = index;
                bestCrossingCount = crossingCount;
                bestOutsideArea = candidateOutsideArea;
                bestLayout = std::move(candidateLayout);
                bestContour = std::move(candidateContour);
            }
        };
        for (int i = 0; i < activeSpans.size()
             && (bestCrossingCount != 0
                 || bestOutsideArea > maximumCoreOutsideArea); ++i) {
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
        for (int i = 0; i < activeInwardSpans.size()
             && (bestCrossingCount != 0
                 || bestOutsideArea > maximumCoreOutsideArea); ++i) {
            if (!activeInwardSpans[i]) {
                continue;
            }
            activeInwardSpans[i] = false;
            consider(CoreFitKind::InwardSpan, i);
            activeInwardSpans[i] = true;
            if (curveCancelled()) {
                if (curveTimedOut()
                    && !(cancelled && cancelled())) {
                    return polygonFallback();
                }
                result.cancelled = true;
                result.error = QStringLiteral("Pen core repair timed out");
                return result;
            }
        }
        for (int i = 0; i < activeStraightSpans.size()
             && (bestCrossingCount != 0
                 || bestOutsideArea > maximumCoreOutsideArea); ++i) {
            if (!activeStraightSpans[i]) {
                continue;
            }
            activeStraightSpans[i] = false;
            consider(CoreFitKind::StraightSpan, i);
            activeStraightSpans[i] = true;
            if (cancelled && cancelled()) {
                result.cancelled = true;
                result.error = QStringLiteral("Pen core repair timed out");
                return result;
            }
        }
        for (int i = 0; i < activeInwardCurves.size()
             && (bestCrossingCount != 0
                 || bestOutsideArea > maximumCoreOutsideArea); ++i) {
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
        if (bestKind == CoreFitKind::None) {
            break;
        }
        if (bestKind == CoreFitKind::Span) {
            activeSpans[bestIndex] = false;
        } else if (bestKind == CoreFitKind::InwardSpan) {
            activeInwardSpans[bestIndex] = false;
        } else if (bestKind == CoreFitKind::StraightSpan) {
            activeStraightSpans[bestIndex] = false;
        } else {
            activeInwardCurves[bestIndex] = false;
        }
        layout = std::move(bestLayout);
        coreContour = std::move(bestContour);
        outsideCoreArea = bestOutsideArea;
    }
    if (layout.points.size() < 3) {
        result.error = QStringLiteral("The Pen contour left no polygonal core");
        return result;
    }
    if (!coreContour.valid()) {
        result.error = coreContour.error.isEmpty()
            ? QStringLiteral("The Pen core is invalid")
            : QStringLiteral("Could not fill the Pen core: %1").arg(coreContour.error);
        return result;
    }
    if (outsideCoreArea > maximumCoreOutsideArea) {
        result.error = QStringLiteral("The Pen core could not remain within its contour");
        return result;
    }
    QVector<bool> matchedCurveSegments(segments.size(), false);
    for (int spanIndex = 0; spanIndex < curveSpans.size(); ++spanIndex) {
        if (!activeSpans[spanIndex]) {
            continue;
        }
        for (int segmentIndex = curveSpans[spanIndex].first;
             segmentIndex <= curveSpans[spanIndex].last; ++segmentIndex) {
            matchedCurveSegments[segmentIndex] = true;
        }
    }
    for (int spanIndex = 0; spanIndex < inwardSpans.size(); ++spanIndex) {
        if (!activeInwardSpans[spanIndex]) {
            continue;
        }
        for (int segmentIndex = inwardSpans[spanIndex].first;
             segmentIndex <= inwardSpans[spanIndex].last; ++segmentIndex) {
            matchedCurveSegments[segmentIndex] = true;
        }
    }
    for (int segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex) {
        matchedCurveSegments[segmentIndex] = matchedCurveSegments[segmentIndex]
            || activeInwardCurves[segmentIndex];
        if (!segments[segmentIndex].curved
            || matchedCurveSegments[segmentIndex]) {
            continue;
        }
        if (layout.supportedCurveSegments.contains(segmentIndex)) {
            ++result.curveLoopDiagnostics[0].supportedCurveSegments;
        } else {
            ++result.curveLoopDiagnostics[0].chordedCurveSegments;
        }
    }
    QPainterPath coverage;
    coverage.setFillRule(Qt::WindingFill);
    for (int i = 0; i < segments.size();) {
        const int spanIndex = layout.spanAtStart[i];
        if (spanIndex >= 0) {
            const CurveSpanPlacement &span = curveSpans[spanIndex];
            PenPlacement placement = span.curve.placement;
            recordBoundaryOwnership(
                &placement, PenBoundaryFitKind::OutwardSpan,
                0, span.first, span.last, segments);
            result.placements.push_back(std::move(placement));
            coverage = coverage.united(span.curve.path);
            i = span.last + 1;
            continue;
        }
        const int inwardSpanIndex = layout.inwardSpanAtStart[i];
        if (inwardSpanIndex >= 0) {
            const InwardCurveSpanPlacement &span =
                inwardSpans[inwardSpanIndex];
            PenPlacement placement = span.curve.curve.placement;
            recordBoundaryOwnership(
                &placement, PenBoundaryFitKind::InwardSpan,
                0, span.first, span.last, segments);
            result.placements.push_back(std::move(placement));
            coverage = coverage.united(span.curve.curve.path);
            i = span.last + 1;
            continue;
        }
        const int straightSpanIndex = layout.straightSpanAtStart[i];
        if (straightSpanIndex >= 0) {
            const StraightBoundarySpanPlacement &span =
                straightSpans[straightSpanIndex];
            PenPlacement placement = span.shape.placement;
            recordBoundaryOwnership(
                &placement, span.kind, 0,
                span.first, span.last, segments);
            result.placements.push_back(std::move(placement));
            coverage = coverage.united(span.shape.path);
            i = span.last + 1;
            continue;
        }
        if (activeInwardCurves[i]) {
            PenPlacement placement = inwardCurves[i]->curve.placement;
            recordBoundaryOwnership(
                &placement, PenBoundaryFitKind::InwardSegment,
                0, i, i, segments);
            result.placements.push_back(std::move(placement));
            coverage = coverage.united(inwardCurves[i]->curve.path);
        }
        ++i;
    }
    PolygonMeshRequest meshRequest;
    meshRequest.points = layout.points;
    meshRequest.sources = meshSources;
    meshRequest.mergeSquares = true;
    reportProgress(QStringLiteral("Meshing contour core"),
                   0, layout.points.size(), result.placements.size(),
                   pathArea(coverage.intersected(target)));
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
    const QVector<PolygonMeshPlacement> corePlacements = optimizePolygonMeshWithEllipses(
        mesh.placements, meshSources, mesh.contour, cancelled);
    if (cancelled && cancelled()) {
        result.placements.clear();
        result.cancelled = true;
        result.error = QStringLiteral("Pen core ellipse selection timed out");
        return result;
    }
    const CurveCoreOptimization optimizedCore = optimizeCurveCoreWithTemplates(
        corePlacements, meshSources, mesh.contour, primitives,
        legalEnvelope, result.targetArea,
        result.shapeLimit - result.placements.size(),
        gpuEvaluator.get(), gpuMeshes, curveCancelled,
        [&](int completed, int total) {
            reportProgress(QStringLiteral("Packing curve core"),
                           completed, total,
                           result.placements.size() + completed);
        });
    if (optimizedCore.placements.isEmpty()) {
        result.error = QStringLiteral(
            "Pen core selected unavailable Primitive geometry");
        return result;
    }
    for (const PenPlacement &placement : optimizedCore.placements) {
        result.placements.push_back(placement);
    }
    coverage = coverage.united(optimizedCore.coverage);
    if (request.curveTimeBudgetMs > 0) {
        const auto completion = optimizeCoverageWithinSpill(
            result.placements, coverage, target, legalEnvelope,
            primitives, result.targetArea,
            result.shapeLimit - result.placements.size(),
            gpuEvaluator.get(), gpuMeshes, curveCancelled,
            [&](int completed, int total) {
                reportProgress(QStringLiteral("Completing exact coverage"),
                               completed, total,
                               result.placements.size() + completed);
            });
        if (completion) {
            result.placements = completion->placements;
            coverage = completion->coverage;
        }
    }
    if (result.placements.size() > result.shapeLimit) {
        result.error = QStringLiteral("Pen fill exceeded its shape limit");
        result.placements.clear();
        return result;
    }
    if (request.discardNegligiblePlacements
        && !discardNegligiblePlacements(&result.placements,
                                        &coverage,
                                        primitives,
                                        target,
                                        result.targetArea,
                                        request.boundaryTolerance,
                                        cancelled)) {
        result.placements.clear();
        result.cancelled = true;
        result.error = QStringLiteral("Pen placement cleanup timed out");
        return result;
    }
    result.coveredArea = pathArea(coverage.intersected(target));
    result.outsideArea = pathArea(coverage.subtracted(target));
    result.unfilled = target.subtracted(coverage);
    reportProgress(QStringLiteral("Cleaning placements"),
                   result.placements.size(), result.placements.size(),
                   result.placements.size(), result.coveredArea);
    return result;
}

} // namespace gui
