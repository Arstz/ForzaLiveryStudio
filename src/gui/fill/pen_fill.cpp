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
constexpr double kMaximumCoreOutsideRatio = 1e-5;
constexpr double kMaximumNegligiblePlacementAreaRatio = 1e-3;
constexpr double kMaximumDiscardedAreaRatio = 1e-3;
constexpr double kMaximumDiscardedThicknessRatio = 0.0025;
constexpr int kInteriorCoreSteps = 16;
constexpr int kChordContainmentSamples = 32;
constexpr double kMaximumTangentJump = 0.35;
constexpr double kMaximumCurvatureJump = 0.75;
constexpr double kMaximumSpanErrorRatio = 0.05;
constexpr int kSpanSamples = 64;
constexpr int kMaximumSpanEvaluations = 2048;
constexpr int kMaximumCurveProfileShortlist = 16;
constexpr int kMaximumExactCurveCandidates = 8;

int curveEvaluationBudget(int segmentCount) {
    // A budget smaller than the contour consumed every trial on individual
    // segments and left no room for the multi-segment matches that actually
    // reduce the polygonal core. Scale with contour complexity while retaining
    // the existing hard cap.
    return std::min(1024, std::max(256, segmentCount * 3));
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
                if (!curved) {
                    continue;
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
                const double curveSide = cross(chord, middle - start);
                const QPointF inwardDirection = curveSide >= 0.0
                    ? -leftNormal : leftNormal;
                result.push_back({
                    start, middle, end,
                    supportPoint(primitive, inwardDirection),
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
    QVector<RankedProfile> ranked;
    ranked.reserve(curves.size() * 2);
    const int profilesPerCurve =
        curves.size() <= kMaximumCurveProfileShortlist ? 2 : 1;
    for (const CurvePrimitive &curve : curves) {
        if ((!includeCircle
             && curve.primitive->shapeId == kCircleShapeId)
            || curve.profiles.isEmpty()) {
            continue;
        }
        QVector<RankedProfile> curveProfiles;
        curveProfiles.reserve(curve.profiles.size());
        for (const ArcProfile &profile : curve.profiles) {
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
        curveProfiles.resize(std::min(
            profilesPerCurve, static_cast<int>(curveProfiles.size())));
        for (const RankedProfile &profile : std::as_const(curveProfiles)) {
            ranked.push_back(profile);
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const RankedProfile &left, const RankedProfile &right) {
        if (std::abs(left.score - right.score) > kEpsilon) {
            return left.score < right.score;
        }
        return left.curve->primitive->shapeId
            < right.curve->primitive->shapeId;
    });
    ranked.resize(std::min(
        kMaximumCurveProfileShortlist,
        static_cast<int>(ranked.size())));
    QVector<QPair<const CurvePrimitive *, const ArcProfile *>> result;
    result.reserve(ranked.size());
    for (const RankedProfile &profile : std::as_const(ranked)) {
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
    shortlist.reserve(kMaximumExactCurveCandidates);
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
        if (shortlist.size() >= kMaximumExactCurveCandidates) {
            break;
        }
    }
    for (const int index : std::as_const(order)) {
        if (exhaustiveLegacyEvaluation) {
            break;
        }
        if (shortlist.size() >= kMaximumExactCurveCandidates) {
            break;
        }
        if (!shortlist.contains(index)) {
            shortlist.push_back(index);
        }
    }

    std::optional<EvaluatedCurveTransform> best;
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
        EvaluatedCurveTransform evaluated{
            *placement, candidate.coreMiddle, candidate.hasCoreMiddle};
        if (!best || betterCurvePlacement(
                         evaluated.curve, best->curve)) {
            best = std::move(evaluated);
        }
    }
    return best;
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

struct CoreLayout {
    QVector<QPointF> points;
    QVector<int> spanAtStart;
    QVector<int> inwardSpanAtStart;
};

enum class CoreFitKind {
    None,
    Span,
    InwardSpan,
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
                     maximumEvaluations * kMaximumExactCurveCandidates);
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
                     maximumEvaluations * kMaximumExactCurveCandidates);
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
    const QPainterPath target = request.targetPath.isEmpty()
        ? contour.path : request.targetPath;
    const QPainterPath legalEnvelope = request.legalEnvelope.isEmpty()
        ? target : request.legalEnvelope.united(target);
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
        for (const PenContourLoop &loop : contour.loops) {
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
                    result.placements.push_back(span.curve.placement);
                    coverage = coverage.united(span.curve.path);
                    corePoints.push_back(segments[span.last].end);
                    segmentIndex = span.last + 1;
                    continue;
                }
                const int inwardSpanIndex = inwardSpanAtStart[segmentIndex];
                if (inwardSpanIndex >= 0) {
                    const InwardCurveSpanPlacement &span =
                        inwardSpans[inwardSpanIndex];
                    result.placements.push_back(span.curve.curve.placement);
                    coverage = coverage.united(span.curve.curve.path);
                    corePoints.push_back(span.curve.coreMiddle);
                    corePoints.push_back(segments[span.last].end);
                    segmentIndex = span.last + 1;
                    continue;
                }
                const PenBoundarySegment &segment = segments[segmentIndex];
                if (inwardCurves[segmentIndex]) {
                    result.placements.push_back(inwardCurves[segmentIndex]->curve.placement);
                    coverage = coverage.united(inwardCurves[segmentIndex]->curve.path);
                    corePoints.push_back(inwardCurves[segmentIndex]->coreMiddle);
                } else if (segment.curved
                           && !chordInsideTarget(segment.start,
                                                 segment.end,
                                                 target)) {
                    const auto point = interiorCorePoint(segment, target);
                    corePoints.push_back(point ? *point : segment.control);
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
        meshRequest.mergeSquares = false;
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
        for (const PolygonMeshPlacement &placement : corePlacements) {
            const PenPrimitive *primitive = primitiveForId(primitives, placement.shapeId);
            if (primitive == nullptr) {
                result.placements.clear();
                result.error = QStringLiteral("Pen core selected unavailable Primitive %1")
                    .arg(placement.shapeId);
                return result;
            }
            result.placements.push_back({
                placement.shapeId,
                placement.transform,
                primitive->area * std::abs(placement.transform.determinant()),
                placement.shapeId == kCircleShapeId,
            });
            coverage = coverage.united(
                placement.transform.map(primitive->silhouette));
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
    QVector<bool> activeInwardCurves(segments.size(), false);
    for (int i = 0; i < inwardCurves.size(); ++i) {
        activeInwardCurves[i] = inwardCurves[i].has_value();
    }
    const auto coreLayout = [&](const QVector<bool> &enabledSpans,
                                 const QVector<bool> &enabledInwardSpans,
                                 const QVector<bool> &enabledInwardCurves) {
        CoreLayout layout;
        layout.spanAtStart.fill(-1, segments.size());
        layout.inwardSpanAtStart.fill(-1, segments.size());
        QVector<bool> activeSpanCovered(segments.size(), false);
        int activeSpanCount = 0;
        int activeInwardSpanCount = 0;
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
            + activeInwardCount;
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
        activeSpans, activeInwardSpans, activeInwardCurves);
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
                activeSpans, activeInwardSpans, activeInwardCurves);
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
    QPainterPath coverage;
    coverage.setFillRule(Qt::WindingFill);
    for (int i = 0; i < segments.size();) {
        const int spanIndex = layout.spanAtStart[i];
        if (spanIndex >= 0) {
            const CurveSpanPlacement &span = curveSpans[spanIndex];
            PenPlacement placement = span.curve.placement;
            for (int segmentIndex = span.first;
                 segmentIndex <= span.last;
                 ++segmentIndex) {
                placement.exposedContourArc +=
                    sampledArcLength(segments[segmentIndex]);
            }
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
            for (int segmentIndex = span.first;
                 segmentIndex <= span.last; ++segmentIndex) {
                placement.exposedContourArc +=
                    sampledArcLength(segments[segmentIndex]);
            }
            result.placements.push_back(std::move(placement));
            coverage = coverage.united(span.curve.curve.path);
            i = span.last + 1;
            continue;
        }
        if (activeInwardCurves[i]) {
            PenPlacement placement = inwardCurves[i]->curve.placement;
            placement.exposedContourArc = sampledArcLength(segments[i]);
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
    for (const PolygonMeshPlacement &placement : corePlacements) {
        const PenPrimitive *primitive = primitiveForId(primitives, placement.shapeId);
        if (primitive == nullptr) {
            result.error = QStringLiteral("Pen core selected unavailable Primitive %1").arg(placement.shapeId);
            return result;
        }
        PenPlacement penPlacement;
        penPlacement.shapeId = placement.shapeId;
        penPlacement.transform = placement.transform;
        penPlacement.area = primitive->area * std::abs(placement.transform.determinant());
        penPlacement.coreEllipse = placement.shapeId == kCircleShapeId;
        result.placements.push_back(penPlacement);
        coverage = coverage.united(placement.transform.map(primitive->silhouette));
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
