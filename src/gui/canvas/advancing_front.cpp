#include "advancing_front.h"

#include "region_fill.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>

namespace gui {
namespace {

constexpr double kGeometryEpsilon = 1e-8;
constexpr double kOriginalBoundaryDistance = 0.75;
constexpr double kMinimumNewArea = 0.25;
constexpr double kStraightSpanTurn = 0.18;
constexpr int kSquareShapeId = 101;
constexpr int kCircleShapeId = 102;
constexpr int kTriangleShapeId = 103;
constexpr int kHalfCircleShapeId = 109;
constexpr int kConcaveArcShapeId = 129;
constexpr int kQuarterCircleShapeId = 130;
constexpr int kSsimWindow = 7;
constexpr int kSsimRadius = kSsimWindow / 2;
constexpr std::array<double, 3> kPrimitiveArcFractions = {0.125, 0.25, 0.5};

struct PathComponent {
    QPainterPath path;
    QPolygonF polygon;
    QRectF bounds;
    double area = 0.0;
    int stableIndex = 0;
};

struct Candidate {
    PenPlacement placement;
    QPainterPath path;
    QPolygonF sourceArc;
    QPolygonF targetSpan;
    QString stableKey;
    double contourParity = 0.0;
    double newArea = 0.0;
    double normalizedArea = 0.0;
    double redundantArea = 0.0;
    double leakageDssim = 0.0;
    double score = 0.0;
    int stateIndex = -1;
    int componentCountAfter = 0;
    int baselineShapesReplaced = 0;
};

struct SpanState {
    int seedEdge = 0;
    int direction = 1;
    int nextPointCount = 2;
    int testedSpanCount = 0;
    int regressionCount = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    double maximumArea = 0.0;
    bool active = true;
};

struct CandidateJob {
    QPolygonF span;
    const PenPrimitive *primitive = nullptr;
    int stateIndex = -1;
    int pointCount = 0;
};

struct AcceptedStep {
    QVector<Candidate> alternatives;
    Candidate selected;
    int nextAlternative = 1;
};

struct CoverageMetrics {
    double targetCoverage = 0.0;
    double leakageFraction = std::numeric_limits<double>::infinity();
    bool valid = false;
};

double cross(const QPointF &left, const QPointF &right) {
    return left.x() * right.y() - left.y() * right.x();
}

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        result += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    }

    return result * 0.5;
}

double polygonArea(const QPolygonF &polygon) {
    return std::abs(signedArea(polygon));
}

double pathArea(const QPainterPath &path) {
    double result = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        result += polygonArea(polygon);
    }

    return result;
}

QPolygonF normalizedPolygon(QPolygonF polygon) {
    while (polygon.size() > 1
           && QLineF(polygon.front(), polygon.back()).length() <= kGeometryEpsilon) {
        polygon.removeLast();
    }

    return polygon;
}

QPainterPath polygonPath(const QPolygonF &polygon) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    if (polygon.size() >= 3) {
        result.addPolygon(polygon);
        result.closeSubpath();
    }

    return result;
}

std::optional<QPointF> polygonInteriorPoint(const QPolygonF &polygon) {
    const QPainterPath path = polygonPath(polygon);
    const QRectF bounds = polygon.boundingRect();
    const QPointF center = bounds.center();
    if (path.contains(center)) {
        return center;
    }
    QPointF mean;
    for (const QPointF &point : polygon) {
        mean += point;
    }
    mean /= std::max(1, static_cast<int>(polygon.size()));
    if (path.contains(mean)) {
        return mean;
    }
    const double step = std::max(
        0.001, std::min(bounds.width(), bounds.height()) * 0.001);
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF start = polygon[i];
        const QPointF end = polygon[(i + 1) % polygon.size()];
        const QPointF edge = end - start;
        const double length = std::hypot(edge.x(), edge.y());
        if (length <= kGeometryEpsilon) {
            continue;
        }
        const QPointF middle = (start + end) * 0.5;
        const QPointF normal(-edge.y() / length, edge.x() / length);
        for (const double direction : {-1.0, 1.0}) {
            const QPointF candidate = middle + normal * (step * direction);
            if (path.contains(candidate)) {
                return candidate;
            }
        }
    }

    return std::nullopt;
}

QVector<PathComponent> pathComponents(const QPainterPath &path) {
    struct Contour {
        QPolygonF polygon;
        QPainterPath path;
        QPointF interiorPoint;
        QRectF bounds;
        double area = 0.0;
        int stableIndex = 0;
        bool filled = false;
    };

    QVector<Contour> contours;
    const QList<QPolygonF> polygons = path.simplified().toSubpathPolygons();
    contours.reserve(polygons.size());
    for (int i = 0; i < polygons.size(); ++i) {
        QPolygonF polygon = normalizedPolygon(polygons[i]);
        const double area = polygonArea(polygon);
        const std::optional<QPointF> interiorPoint = polygonInteriorPoint(polygon);
        if (polygon.size() < 3 || area <= kGeometryEpsilon || !interiorPoint) {
            continue;
        }
        Contour contour;
        contour.path = polygonPath(polygon);
        contour.polygon = std::move(polygon);
        contour.interiorPoint = *interiorPoint;
        contour.bounds = contour.polygon.boundingRect();
        contour.area = area;
        contour.stableIndex = i;
        contour.filled = path.contains(contour.interiorPoint);
        contours.push_back(std::move(contour));
    }

    QVector<PathComponent> result;
    result.reserve(contours.size());
    QVector<int> componentContours;
    for (int contourIndex = 0; contourIndex < contours.size(); ++contourIndex) {
        const Contour &contour = contours[contourIndex];
        if (!contour.filled) {
            continue;
        }
        PathComponent component;
        component.path.setFillRule(Qt::OddEvenFill);
        component.path.addPolygon(contour.polygon);
        component.path.closeSubpath();
        component.polygon = contour.polygon;
        component.bounds = contour.bounds;
        component.area = contour.area;
        component.stableIndex = contour.stableIndex;
        componentContours.push_back(contourIndex);
        result.push_back(std::move(component));
    }
    for (const Contour &hole : contours) {
        if (hole.filled) {
            continue;
        }
        int owner = -1;
        double ownerArea = std::numeric_limits<double>::infinity();
        for (int componentIndex = 0;
             componentIndex < result.size(); ++componentIndex) {
            const Contour &outer = contours[componentContours[componentIndex]];
            if (outer.area >= ownerArea
                || !outer.bounds.contains(hole.interiorPoint)
                || !outer.path.contains(hole.interiorPoint)) {
                continue;
            }
            owner = componentIndex;
            ownerArea = outer.area;
        }
        if (owner < 0) {
            continue;
        }
        result[owner].path.addPolygon(hole.polygon);
        result[owner].path.closeSubpath();
        result[owner].area = std::max(
            0.0, result[owner].area - hole.area);
    }
    std::stable_sort(result.begin(), result.end(), [](const PathComponent &left,
                                                       const PathComponent &right) {
        if (std::abs(left.area - right.area) > kGeometryEpsilon) {
            return left.area > right.area;
        }
        if (std::abs(left.bounds.top() - right.bounds.top()) > kGeometryEpsilon) {
            return left.bounds.top() < right.bounds.top();
        }
        if (std::abs(left.bounds.left() - right.bounds.left()) > kGeometryEpsilon) {
            return left.bounds.left() < right.bounds.left();
        }
        return left.stableIndex < right.stableIndex;
    });

    return result;
}

double pointToSegmentDistance(const QPointF &point,
                              const QPointF &start,
                              const QPointF &end) {
    const QPointF edge = end - start;
    const double lengthSquared = QPointF::dotProduct(edge, edge);
    if (lengthSquared <= kGeometryEpsilon) {
        return QLineF(point, start).length();
    }
    const double at = std::clamp(QPointF::dotProduct(point - start, edge)
                                     / lengthSquared,
                                 0.0,
                                 1.0);

    return QLineF(point, start + edge * at).length();
}

double pointToPolylineDistance(const QPointF &point,
                               const QPolygonF &polyline,
                               bool closed) {
    if (polyline.size() < 2) {
        return std::numeric_limits<double>::infinity();
    }
    const int segmentCount = closed ? polyline.size() : polyline.size() - 1;
    double result = std::numeric_limits<double>::infinity();
    for (int i = 0; i < segmentCount; ++i) {
        result = std::min(result,
                          pointToSegmentDistance(point,
                                                 polyline[i],
                                                 polyline[(i + 1) % polyline.size()]));
    }

    return result;
}

double pointToPathBoundaryDistance(const QPointF &point, const QPainterPath &path) {
    double result = std::numeric_limits<double>::infinity();
    for (QPolygonF polygon : path.toSubpathPolygons()) {
        polygon = normalizedPolygon(std::move(polygon));
        result = std::min(result, pointToPolylineDistance(point, polygon, true));
    }

    return result;
}

double polylineLength(const QPolygonF &polyline, bool closed) {
    double result = 0.0;
    const int pointCount = static_cast<int>(polyline.size());
    const int segmentCount = closed ? pointCount : std::max(0, pointCount - 1);
    for (int i = 0; i < segmentCount; ++i) {
        result += QLineF(polyline[i], polyline[(i + 1) % polyline.size()]).length();
    }

    return result;
}

QPolygonF resamplePolyline(const QPolygonF &polyline, int sampleCount) {
    QPolygonF result;
    if (polyline.size() < 2 || sampleCount < 2) {
        return result;
    }
    QVector<double> distances(polyline.size(), 0.0);
    for (int i = 1; i < polyline.size(); ++i) {
        distances[i] = distances[i - 1]
            + QLineF(polyline[i - 1], polyline[i]).length();
    }
    const double total = distances.back();
    if (total <= kGeometryEpsilon) {
        return result;
    }
    result.reserve(sampleCount);
    int segment = 1;
    for (int i = 0; i < sampleCount; ++i) {
        const double wanted = total * i / std::max(1, sampleCount - 1);
        while (segment + 1 < distances.size() && distances[segment] < wanted) {
            ++segment;
        }
        const double segmentLength = distances[segment] - distances[segment - 1];
        const double at = segmentLength <= kGeometryEpsilon
            ? 0.0 : (wanted - distances[segment - 1]) / segmentLength;
        result.push_back(polyline[segment - 1] * (1.0 - at) + polyline[segment] * at);
    }

    return result;
}

QPointF normalizedVector(const QPointF &value) {
    const double length = std::hypot(value.x(), value.y());
    return length <= kGeometryEpsilon ? QPointF{} : value / length;
}

double contourParity(const QPolygonF &front,
                     const QPolygonF &primitiveArc,
                     const AdvancingFrontOptions &options) {
    const double longest = std::max(polylineLength(front, false),
                                    polylineLength(primitiveArc, false));
    const int sampleCount = std::clamp(
        static_cast<int>(std::ceil(longest / std::max(options.sampleSpacing,
                                                       kGeometryEpsilon))) + 1,
        options.minimumBoundarySamples,
        options.maximumBoundarySamples);
    const QPolygonF frontSamples = resamplePolyline(front, sampleCount);
    const QPolygonF primitiveSamples = resamplePolyline(primitiveArc, sampleCount);
    if (frontSamples.size() < 2 || primitiveSamples.size() < 2) {
        return 0.0;
    }
    int frontMatches = 0;
    int primitiveMatches = 0;
    for (const QPointF &point : frontSamples) {
        if (pointToPolylineDistance(point, primitiveSamples, false)
            <= options.contourMatchDistance) {
            ++frontMatches;
        }
    }
    for (const QPointF &point : primitiveSamples) {
        if (pointToPolylineDistance(point, frontSamples, false)
            <= options.contourMatchDistance) {
            ++primitiveMatches;
        }
    }
    const double recall = static_cast<double>(frontMatches) / frontSamples.size();
    const double precision = static_cast<double>(primitiveMatches)
        / primitiveSamples.size();
    if (recall + precision <= kGeometryEpsilon) {
        return 0.0;
    }
    const QPointF frontStart = normalizedVector(frontSamples[1] - frontSamples[0]);
    const QPointF frontEnd = normalizedVector(frontSamples.back()
                                               - frontSamples[frontSamples.size() - 2]);
    const QPointF primitiveStart = normalizedVector(primitiveSamples[1]
                                                     - primitiveSamples[0]);
    const QPointF primitiveEnd = normalizedVector(primitiveSamples.back()
                                                   - primitiveSamples[primitiveSamples.size() - 2]);
    const double tangentAlignment = std::clamp(
        (QPointF::dotProduct(frontStart, primitiveStart)
         + QPointF::dotProduct(frontEnd, primitiveEnd) + 2.0) * 0.25,
        0.0,
        1.0);
    const double harmonic = 2.0 * recall * precision / (recall + precision);

    return std::clamp(harmonic * tangentAlignment, 0.0, 1.0);
}

QImage renderMask(const QPainterPath &path, const QRect &sourceRect, int supersample) {
    if (sourceRect.isEmpty() || supersample < 1) {
        return {};
    }
    const qint64 width = static_cast<qint64>(sourceRect.width()) * supersample;
    const qint64 height = static_cast<qint64>(sourceRect.height()) * supersample;
    if (width < 1 || height < 1 || width > std::numeric_limits<int>::max()
        || height > std::numeric_limits<int>::max()) {
        return {};
    }
    QImage high(static_cast<int>(width), static_cast<int>(height),
                QImage::Format_Grayscale8);
    if (high.isNull()) {
        return {};
    }
    high.fill(0);
    QPainter painter(&high);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.scale(supersample, supersample);
    painter.translate(-sourceRect.left(), -sourceRect.top());
    painter.fillPath(path, Qt::white);
    painter.end();

    return high.scaled(sourceRect.size(), Qt::IgnoreAspectRatio,
                       Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_Grayscale8);
}

CoverageMetrics coverageMetrics(const QPainterPath &target,
                                const QPainterPath &coverage,
                                const QSize &imageSize,
                                int supersample) {
    CoverageMetrics result;
    const QRect imageRect(QPoint{}, imageSize);
    const QRect sourceRect = target.boundingRect().united(coverage.boundingRect())
                                 .toAlignedRect().intersected(imageRect);
    const QImage targetMask = renderMask(target, sourceRect, supersample);
    const QImage coverageMask = renderMask(coverage, sourceRect, supersample);
    if (targetMask.isNull() || coverageMask.isNull()) {
        return result;
    }
    double targetMass = 0.0;
    double coveredMass = 0.0;
    double leakedMass = 0.0;
    for (int y = 0; y < targetMask.height(); ++y) {
        const uchar *targetRow = targetMask.constScanLine(y);
        const uchar *coverageRow = coverageMask.constScanLine(y);
        for (int x = 0; x < targetMask.width(); ++x) {
            const double targetValue = targetRow[x];
            const double coverageValue = coverageRow[x];
            targetMass += targetValue;
            coveredMass += std::min(targetValue, coverageValue);
            leakedMass += std::max(0.0, coverageValue - targetValue);
        }
    }
    if (targetMass <= kGeometryEpsilon) {
        return result;
    }
    result.targetCoverage = coveredMass / targetMass;
    result.leakageFraction = leakedMass / targetMass;
    result.valid = true;

    return result;
}

double imageDssim(const QImage &left, const QImage &right) {
    if (left.isNull() || right.isNull() || left.size() != right.size()) {
        return std::numeric_limits<double>::infinity();
    }
    constexpr double sampleCount = kSsimWindow * kSsimWindow;
    constexpr double covarianceDivisor = sampleCount - 1.0;
    constexpr double c1 = 6.5025;
    constexpr double c2 = 58.5225;
    if (left.width() < kSsimWindow || left.height() < kSsimWindow) {
        double squaredError = 0.0;
        for (int y = 0; y < left.height(); ++y) {
            const uchar *leftRow = left.constScanLine(y);
            const uchar *rightRow = right.constScanLine(y);
            for (int x = 0; x < left.width(); ++x) {
                const double difference = leftRow[x] - rightRow[x];
                squaredError += difference * difference;
            }
        }
        const double count = std::max(1, left.width() * left.height());
        return std::clamp(squaredError / (count * 255.0 * 255.0), 0.0, 1.0);
    }

    using MomentRows = std::array<QVector<double>, 5>;
    std::array<MomentRows, kSsimWindow> ring;
    MomentRows vertical;
    MomentRows horizontal;
    for (int moment = 0; moment < 5; ++moment) {
        vertical[moment].fill(0.0, left.width());
        horizontal[moment].fill(0.0, left.width());
        for (int row = 0; row < kSsimWindow; ++row) {
            ring[row][moment].fill(0.0, left.width());
        }
    }

    double similarity = 0.0;
    qint64 windows = 0;
    for (int y = 0; y < left.height(); ++y) {
        const uchar *leftRow = left.constScanLine(y);
        const uchar *rightRow = right.constScanLine(y);
        std::array<double, 5> rolling{};
        for (int x = 0; x < kSsimWindow; ++x) {
            const double leftValue = leftRow[x];
            const double rightValue = rightRow[x];
            rolling[0] += leftValue;
            rolling[1] += rightValue;
            rolling[2] += leftValue * leftValue;
            rolling[3] += rightValue * rightValue;
            rolling[4] += leftValue * rightValue;
        }
        for (int center = kSsimRadius;
             center < left.width() - kSsimRadius;
             ++center) {
            for (int moment = 0; moment < 5; ++moment) {
                horizontal[moment][center] = rolling[moment];
            }
            const int remove = center - kSsimRadius;
            const int add = center + kSsimRadius + 1;
            if (add < left.width()) {
                const double removeLeft = leftRow[remove];
                const double removeRight = rightRow[remove];
                const double addLeft = leftRow[add];
                const double addRight = rightRow[add];
                rolling[0] += addLeft - removeLeft;
                rolling[1] += addRight - removeRight;
                rolling[2] += addLeft * addLeft - removeLeft * removeLeft;
                rolling[3] += addRight * addRight - removeRight * removeRight;
                rolling[4] += addLeft * addRight - removeLeft * removeRight;
            }
        }

        const int slot = y % kSsimWindow;
        for (int center = kSsimRadius;
             center < left.width() - kSsimRadius;
             ++center) {
            for (int moment = 0; moment < 5; ++moment) {
                if (y >= kSsimWindow) {
                    vertical[moment][center] -= ring[slot][moment][center];
                }
                ring[slot][moment][center] = horizontal[moment][center];
                vertical[moment][center] += horizontal[moment][center];
            }
        }
        if (y < kSsimWindow - 1) {
            continue;
        }
        for (int center = kSsimRadius;
             center < left.width() - kSsimRadius;
             ++center) {
            const double sumLeft = vertical[0][center];
            const double sumRight = vertical[1][center];
            const double meanLeft = sumLeft / sampleCount;
            const double meanRight = sumRight / sampleCount;
            const double varianceLeft = std::max(
                0.0, (vertical[2][center] - sumLeft * sumLeft / sampleCount)
                         / covarianceDivisor);
            const double varianceRight = std::max(
                0.0, (vertical[3][center] - sumRight * sumRight / sampleCount)
                         / covarianceDivisor);
            const double covariance =
                (vertical[4][center] - sumLeft * sumRight / sampleCount)
                / covarianceDivisor;
            similarity += ((2.0 * meanLeft * meanRight + c1)
                           * (2.0 * covariance + c2))
                / ((meanLeft * meanLeft + meanRight * meanRight + c1)
                   * (varianceLeft + varianceRight + c2));
            ++windows;
        }
    }
    if (windows <= 0) {
        return std::numeric_limits<double>::infinity();
    }

    return (1.0 - std::clamp(similarity / windows, -1.0, 1.0)) * 0.5;
}

double maskDssim(const QPainterPath &left,
                 const QPainterPath &right,
                 const QSize &imageSize,
                 const AdvancingFrontOptions &options,
                 const QRectF &affectedBounds = {}) {
    if (!imageSize.isValid() || imageSize.isEmpty()) {
        return std::numeric_limits<double>::infinity();
    }
    const QRect imageRect(QPoint(0, 0), imageSize);
    const QRectF affected = affectedBounds.isEmpty()
        ? left.boundingRect().united(right.boundingRect()) : affectedBounds;
    QRect sourceRect = affected
        .adjusted(-options.dssimPadding,
                  -options.dssimPadding,
                  options.dssimPadding,
                  options.dssimPadding)
        .toAlignedRect()
        .intersected(imageRect);
    if (sourceRect.isEmpty()) {
        return 0.0;
    }
    if (sourceRect.width() < kSsimWindow || sourceRect.height() < kSsimWindow) {
        sourceRect = sourceRect.adjusted(-kSsimWindow, -kSsimWindow,
                                         kSsimWindow, kSsimWindow)
            .intersected(imageRect);
    }

    return imageDssim(renderMask(left, sourceRect, options.dssimSupersample),
                      renderMask(right, sourceRect, options.dssimSupersample));
}

QPolygonF openRdp(const QPolygonF &points, double epsilon) {
    if (points.size() <= 2 || epsilon <= 0.0) {
        return points;
    }
    QVector<bool> keep(points.size(), false);
    keep.front() = true;
    keep.back() = true;
    QVector<QPair<int, int>> stack{{0, points.size() - 1}};
    while (!stack.isEmpty()) {
        const auto [first, last] = stack.takeLast();
        int furthest = -1;
        double maximumDistance = epsilon;
        for (int i = first + 1; i < last; ++i) {
            const double distance = pointToSegmentDistance(points[i],
                                                           points[first],
                                                           points[last]);
            if (distance > maximumDistance) {
                maximumDistance = distance;
                furthest = i;
            }
        }
        if (furthest >= 0) {
            keep[furthest] = true;
            stack.push_back({first, furthest});
            stack.push_back({furthest, last});
        }
    }
    QPolygonF result;
    result.reserve(points.size());
    for (int i = 0; i < points.size(); ++i) {
        if (keep[i]) {
            result.push_back(points[i]);
        }
    }

    return result;
}

double guidePointSignificance(const QPolygonF &polygon, int index) {
    const int previous = (index + polygon.size() - 1) % polygon.size();
    const int next = (index + 1) % polygon.size();
    const QPointF incoming = normalizedVector(polygon[index] - polygon[previous]);
    const QPointF outgoing = normalizedVector(polygon[next] - polygon[index]);
    const double turn = std::acos(std::clamp(
        QPointF::dotProduct(incoming, outgoing), -1.0, 1.0));
    const double support = QLineF(polygon[previous], polygon[index]).length()
        + QLineF(polygon[index], polygon[next]).length();

    return turn * std::max(1.0, support);
}

QPolygonF limitGuidePoints(const QPolygonF &polygon, int maximumPoints) {
    if (maximumPoints < 3 || polygon.size() <= maximumPoints) {
        return polygon;
    }
    QSet<int> selected;
    const int distributedCount = std::max(3, maximumPoints / 2);
    for (int i = 0; i < distributedCount; ++i) {
        selected.insert(i * polygon.size() / distributedCount);
    }
    QVector<int> ranked(polygon.size());
    std::iota(ranked.begin(), ranked.end(), 0);
    std::stable_sort(ranked.begin(), ranked.end(), [&](int left, int right) {
        const double leftSignificance = guidePointSignificance(polygon, left);
        const double rightSignificance = guidePointSignificance(polygon, right);
        if (std::abs(leftSignificance - rightSignificance) > kGeometryEpsilon) {
            return leftSignificance > rightSignificance;
        }
        return left < right;
    });
    for (const int index : ranked) {
        if (selected.size() >= maximumPoints) {
            break;
        }
        selected.insert(index);
    }
    QVector<int> ordered(selected.begin(), selected.end());
    std::sort(ordered.begin(), ordered.end());
    QPolygonF result;
    result.reserve(ordered.size());
    for (const int index : ordered) {
        result.push_back(polygon[index]);
    }

    return result;
}

int closestPointIndex(const QPolygonF &polygon, const QPointF &point) {
    int result = -1;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int i = 0; i < polygon.size(); ++i) {
        const double distance = QLineF(polygon[i], point).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            result = i;
        }
    }

    return result;
}

QPolygonF denseSpanPoints(const QPolygonF &dense,
                          const QPolygonF &guide,
                          int seedEdge,
                          int direction,
                          int pointCount) {
    if (dense.size() < 2 || guide.size() < 2 || pointCount < 2) {
        return {};
    }
    const int guideStart = direction > 0
        ? seedEdge : (seedEdge + 1) % guide.size();
    const int guideEnd = (guideStart + direction * (pointCount - 1)
                          + guide.size() * pointCount) % guide.size();
    const int denseStart = closestPointIndex(dense, guide[guideStart]);
    const int denseEnd = closestPointIndex(dense, guide[guideEnd]);
    if (denseStart < 0 || denseEnd < 0) {
        return {};
    }
    QPolygonF result;
    result.push_back(dense[denseStart]);
    int index = denseStart;
    while (index != denseEnd && result.size() <= dense.size()) {
        index = (index + direction + dense.size()) % dense.size();
        result.push_back(dense[index]);
    }

    return result;
}

enum class SpanFamily {
    Straight,
    ConsistentCurve,
    MixedCurve,
};

SpanFamily spanFamily(const QPolygonF &span) {
    double positiveTurn = 0.0;
    double negativeTurn = 0.0;
    for (int i = 1; i + 1 < span.size(); ++i) {
        const QPointF incoming = normalizedVector(span[i] - span[i - 1]);
        const QPointF outgoing = normalizedVector(span[i + 1] - span[i]);
        const double signedTurn = std::atan2(cross(incoming, outgoing),
                                             QPointF::dotProduct(incoming, outgoing));
        if (signedTurn >= 0.0) {
            positiveTurn += signedTurn;
        } else {
            negativeTurn -= signedTurn;
        }
    }
    const double totalTurn = positiveTurn + negativeTurn;
    if (totalTurn < kStraightSpanTurn) {
        return SpanFamily::Straight;
    }
    if (std::min(positiveTurn, negativeTurn) > totalTurn * 0.2) {
        return SpanFamily::MixedCurve;
    }

    return SpanFamily::ConsistentCurve;
}

bool primitiveMatchesSpan(const PenPrimitive &primitive, SpanFamily family) {
    if (family == SpanFamily::Straight) {
        return primitive.shapeId == kSquareShapeId
            || primitive.shapeId == kTriangleShapeId;
    }
    if (family == SpanFamily::ConsistentCurve) {
        return primitive.shapeId != kSquareShapeId
            && primitive.shapeId != kTriangleShapeId
            && primitive.shapeId != kConcaveArcShapeId;
    }

    return primitive.shapeId != kCircleShapeId
        && primitive.shapeId != kHalfCircleShapeId
        && primitive.shapeId != kQuarterCircleShapeId;
}

QPolygonF exposedGuideContour(const PathComponent &component,
                              const QPainterPath &target,
                              const QSize &imageSize,
                              const AdvancingFrontOptions &options) {
    QPolygonF base = simplifyClosedPolygonCyclic(component.polygon,
                                                  options.candidateGuideEpsilon);
    if (base.size() < 3) {
        base = component.polygon;
    }
    base = limitGuidePoints(base, options.candidateGuidePointCap);
    QVector<bool> original(base.size(), false);
    int originalCount = 0;
    for (int i = 0; i < base.size(); ++i) {
        original[i] = pointToPathBoundaryDistance(base[i], target)
            <= kOriginalBoundaryDistance;
        originalCount += original[i] ? 1 : 0;
    }
    const auto acceptable = [&](const QPolygonF &candidate) {
        return candidate.size() >= 3
            && buildPolygonContour(candidate).valid()
            && maskDssim(component.path, polygonPath(candidate), imageSize, options,
                         component.bounds)
                   <= options.maximumOmissionDssim + kGeometryEpsilon;
    };
    if (originalCount == 0) {
        QPolygonF best = base;
        for (const double epsilon : options.exposedSimplificationEpsilons) {
            const QPolygonF candidate = simplifyClosedPolygonCyclic(base, epsilon);
            if (candidate.size() < best.size() && acceptable(candidate)) {
                best = candidate;
            }
        }
        return best;
    }
    if (originalCount == base.size()) {
        return base;
    }

    int seam = 0;
    while (seam < base.size() && !original[seam]) {
        ++seam;
    }
    QPolygonF ordered;
    QVector<bool> orderedOriginal;
    ordered.reserve(base.size());
    orderedOriginal.reserve(base.size());
    for (int i = 0; i < base.size(); ++i) {
        const int index = (seam + i) % base.size();
        ordered.push_back(base[index]);
        orderedOriginal.push_back(original[index]);
    }
    ordered.push_back(ordered.front());
    orderedOriginal.push_back(true);

    QPolygonF result;
    for (int i = 0; i + 1 < ordered.size();) {
        result.push_back(ordered[i]);
        if (!orderedOriginal[i]) {
            ++i;
            continue;
        }
        int nextOriginal = i + 1;
        while (nextOriginal < ordered.size() && !orderedOriginal[nextOriginal]) {
            ++nextOriginal;
        }
        if (nextOriginal >= ordered.size() || nextOriginal == i + 1) {
            ++i;
            continue;
        }
        QPolygonF run;
        for (int index = i; index <= nextOriginal; ++index) {
            run.push_back(ordered[index]);
        }
        QPolygonF bestRun = run;
        for (const double epsilon : options.exposedSimplificationEpsilons) {
            const QPolygonF trial = openRdp(run, epsilon);
            QPolygonF candidate = result;
            for (int index = 1; index < trial.size(); ++index) {
                candidate.push_back(trial[index]);
            }
            for (int index = nextOriginal + 1; index + 1 < ordered.size(); ++index) {
                candidate.push_back(ordered[index]);
            }
            if (trial.size() < bestRun.size() && acceptable(candidate)) {
                bestRun = trial;
            }
        }
        for (int index = 1; index + 1 < bestRun.size(); ++index) {
            result.push_back(bestRun[index]);
        }
        i = nextOriginal;
    }
    while (result.size() > 1
           && QLineF(result.front(), result.back()).length() <= kGeometryEpsilon) {
        result.removeLast();
    }

    return result.size() >= 3 ? result : base;
}

bool solveThreeByThree(std::array<std::array<double, 4>, 3> matrix,
                       std::array<double, 3> *solution) {
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row) {
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(matrix[pivot][column]) <= kGeometryEpsilon) {
            return false;
        }
        std::swap(matrix[pivot], matrix[column]);
        const double divisor = matrix[column][column];
        for (int entry = column; entry < 4; ++entry) {
            matrix[column][entry] /= divisor;
        }
        for (int row = 0; row < 3; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = matrix[row][column];
            for (int entry = column; entry < 4; ++entry) {
                matrix[row][entry] -= factor * matrix[column][entry];
            }
        }
    }
    for (int row = 0; row < 3; ++row) {
        (*solution)[row] = matrix[row][3];
    }

    return true;
}

std::optional<QTransform> leastSquaresAffine(const QPolygonF &source,
                                             const QPolygonF &target) {
    const int sampleCount = std::clamp(
        std::max(static_cast<int>(source.size()), static_cast<int>(target.size())),
        8,
        128);
    const QPolygonF sourceSamples = resamplePolyline(source, sampleCount);
    const QPolygonF targetSamples = resamplePolyline(target, sampleCount);
    if (sourceSamples.size() != sampleCount || targetSamples.size() != sampleCount) {
        return std::nullopt;
    }
    double xx = 0.0;
    double xy = 0.0;
    double yy = 0.0;
    double x = 0.0;
    double y = 0.0;
    double targetXTimesX = 0.0;
    double targetXTimesY = 0.0;
    double targetX = 0.0;
    double targetYTimesX = 0.0;
    double targetYTimesY = 0.0;
    double targetY = 0.0;
    for (int i = 0; i < sampleCount; ++i) {
        const QPointF &sourcePoint = sourceSamples[i];
        const QPointF &targetPoint = targetSamples[i];
        xx += sourcePoint.x() * sourcePoint.x();
        xy += sourcePoint.x() * sourcePoint.y();
        yy += sourcePoint.y() * sourcePoint.y();
        x += sourcePoint.x();
        y += sourcePoint.y();
        targetXTimesX += targetPoint.x() * sourcePoint.x();
        targetXTimesY += targetPoint.x() * sourcePoint.y();
        targetX += targetPoint.x();
        targetYTimesX += targetPoint.y() * sourcePoint.x();
        targetYTimesY += targetPoint.y() * sourcePoint.y();
        targetY += targetPoint.y();
    }
    const std::array<std::array<double, 3>, 3> normal = {{{xx, xy, x},
                                                          {xy, yy, y},
                                                          {x, y, static_cast<double>(sampleCount)}}};
    std::array<std::array<double, 4>, 3> horizontal{};
    std::array<std::array<double, 4>, 3> vertical{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            horizontal[row][column] = normal[row][column];
            vertical[row][column] = normal[row][column];
        }
    }
    horizontal[0][3] = targetXTimesX;
    horizontal[1][3] = targetXTimesY;
    horizontal[2][3] = targetX;
    vertical[0][3] = targetYTimesX;
    vertical[1][3] = targetYTimesY;
    vertical[2][3] = targetY;
    std::array<double, 3> horizontalSolution{};
    std::array<double, 3> verticalSolution{};
    if (!solveThreeByThree(horizontal, &horizontalSolution)
        || !solveThreeByThree(vertical, &verticalSolution)) {
        return std::nullopt;
    }
    QTransform transform(horizontalSolution[0], verticalSolution[0],
                         horizontalSolution[1], verticalSolution[1],
                         horizontalSolution[2], verticalSolution[2]);
    if (!transform.isAffine() || !std::isfinite(transform.determinant())) {
        return std::nullopt;
    }

    return transform;
}

QTransform affineFromTriangles(const QPointF &a,
                               const QPointF &b,
                               const QPointF &c,
                               const QPointF &p,
                               const QPointF &q,
                               const QPointF &r,
                               bool *ok) {
    const QPointF sourceFirst = b - a;
    const QPointF sourceSecond = c - a;
    const QPointF targetFirst = q - p;
    const QPointF targetSecond = r - p;
    const double determinant = cross(sourceFirst, sourceSecond);
    if (std::abs(determinant) <= kGeometryEpsilon) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }
    const double m11 = (targetFirst.x() * sourceSecond.y()
                        - targetSecond.x() * sourceFirst.y()) / determinant;
    const double m21 = (-targetFirst.x() * sourceSecond.x()
                        + targetSecond.x() * sourceFirst.x()) / determinant;
    const double m12 = (targetFirst.y() * sourceSecond.y()
                        - targetSecond.y() * sourceFirst.y()) / determinant;
    const double m22 = (-targetFirst.y() * sourceSecond.x()
                        + targetSecond.y() * sourceFirst.x()) / determinant;
    const double dx = p.x() - m11 * a.x() - m21 * a.y();
    const double dy = p.y() - m12 * a.x() - m22 * a.y();
    if (ok != nullptr) {
        *ok = std::isfinite(m11) && std::isfinite(m12)
            && std::isfinite(m21) && std::isfinite(m22)
            && std::isfinite(dx) && std::isfinite(dy);
    }

    return QTransform(m11, m12, m21, m22, dx, dy);
}

double transformCondition(const QTransform &transform) {
    const double a = transform.m11();
    const double b = transform.m21();
    const double c = transform.m12();
    const double d = transform.m22();
    const double trace = a * a + b * b + c * c + d * d;
    const double determinant = a * d - b * c;
    const double discriminant = std::sqrt(std::max(0.0,
                                                   trace * trace
                                                       - 4.0 * determinant * determinant));
    const double largest = std::max(0.0, (trace + discriminant) * 0.5);
    const double smallest = std::max(0.0, (trace - discriminant) * 0.5);
    if (smallest <= kGeometryEpsilon) {
        return std::numeric_limits<double>::infinity();
    }

    return std::sqrt(largest / smallest);
}

QPolygonF largestPrimitiveContour(const PenPrimitive &primitive) {
    QPolygonF result;
    double bestArea = 0.0;
    for (QPolygonF contour : primitive.contours) {
        contour = normalizedPolygon(std::move(contour));
        const double area = polygonArea(contour);
        if (contour.size() >= 3 && area > bestArea) {
            bestArea = area;
            result = std::move(contour);
        }
    }

    return result;
}

QPointF primitiveInteriorPoint(const PenPrimitive &primitive) {
    const QPointF center = primitive.bounds.center();
    if (primitive.silhouette.contains(center)) {
        return center;
    }
    constexpr int grid = 9;
    for (int y = 1; y < grid; ++y) {
        for (int x = 1; x < grid; ++x) {
            const QPointF point(primitive.bounds.left()
                                    + primitive.bounds.width() * x / grid,
                                primitive.bounds.top()
                                    + primitive.bounds.height() * y / grid);
            if (primitive.silhouette.contains(point)) {
                return point;
            }
        }
    }

    return center;
}

QPolygonF primitiveArc(const QPolygonF &contour,
                       int start,
                       int steps,
                       int direction) {
    QPolygonF result;
    result.reserve(steps + 1);
    for (int i = 0; i <= steps; ++i) {
        const int index = (start + direction * i % contour.size()
                           + contour.size()) % contour.size();
        result.push_back(contour[index]);
    }

    return result;
}

QPointF targetInwardDirection(const QPolygonF &span,
                              const QPainterPath &component) {
    const QPointF chord = span.back() - span.front();
    QPointF normal = normalizedVector(QPointF(-chord.y(), chord.x()));
    if (normal.isNull()) {
        return {};
    }
    const QPointF middle = span[span.size() / 2];
    const double probeDistance = std::max(0.25,
                                          std::min(2.0,
                                                   QLineF(span.front(), span.back()).length()
                                                       * 0.02));
    const bool positiveInside = component.contains(middle + normal * probeDistance);
    const bool negativeInside = component.contains(middle - normal * probeDistance);
    if (!positiveInside && negativeInside) {
        normal = -normal;
    } else if (!positiveInside && !negativeInside) {
        const QPolygonF spanPolygon = span;
        if (signedArea(spanPolygon) < 0.0) {
            normal = -normal;
        }
    }

    return normal;
}

double inwardDepth(const QPointF &start,
                   const QPointF &direction,
                   const QPainterPath &component) {
    const QRectF bounds = component.boundingRect();
    const double maximum = std::hypot(bounds.width(), bounds.height());
    if (maximum <= kGeometryEpsilon || direction.isNull()) {
        return 0.0;
    }
    const double step = std::max(0.5, maximum / 128.0);
    double inside = 0.0;
    double outside = step;
    while (outside <= maximum && component.contains(start + direction * outside)) {
        inside = outside;
        outside += step;
    }
    for (int iteration = 0; iteration < 10; ++iteration) {
        const double middle = (inside + outside) * 0.5;
        if (component.contains(start + direction * middle)) {
            inside = middle;
        } else {
            outside = middle;
        }
    }

    return inside;
}

std::optional<Candidate> cheapCandidate(const CandidateJob &job,
                                        const QPainterPath &target,
                                        const QPainterPath &component,
                                        const QPainterPath &residual,
                                        const QPainterPath &coverage,
                                        double componentArea,
                                        const AdvancingFrontOptions &options) {
    const PenPrimitive &primitive = *job.primitive;
    const QPolygonF contour = largestPrimitiveContour(primitive);
    if (contour.size() < 3 || job.span.size() < 2) {
        return std::nullopt;
    }
    const QPointF sourceCore = primitiveInteriorPoint(primitive);
    const QPointF targetMiddle = job.span[job.span.size() / 2];
    const QPointF inward = targetInwardDirection(job.span, component);
    const double maximumDepth = inwardDepth(targetMiddle + inward * 0.25,
                                            inward,
                                            component);
    if (maximumDepth <= kGeometryEpsilon) {
        return std::nullopt;
    }
    std::optional<Candidate> best;
    const int orientationCount = std::min(
        options.primitiveBoundaryOrientations,
        static_cast<int>(contour.size()));
    for (const double fraction : kPrimitiveArcFractions) {
        const int arcSteps = std::clamp(static_cast<int>(std::lround(
                                            contour.size() * fraction)),
                                        2,
                                        std::max(2, static_cast<int>(contour.size()) - 1));
        for (int orientation = 0; orientation < orientationCount; ++orientation) {
            const int sourceStart = orientation * contour.size()
                / orientationCount;
            for (const int sourceDirection : {-1, 1}) {
                const QPolygonF sourceArc = primitiveArc(contour,
                                                         sourceStart,
                                                         arcSteps,
                                                         sourceDirection);
                const QPointF sourceChord = sourceArc.back() - sourceArc.front();
                const double sourceCoreSide = cross(sourceChord,
                                                    sourceCore - sourceArc.front());
                if (std::abs(sourceCoreSide) <= kGeometryEpsilon) {
                    continue;
                }
                double maximumSourceSide = std::abs(sourceCoreSide);
                for (const QPointF &point : contour) {
                    const double side = cross(sourceChord, point - sourceArc.front());
                    if (side * sourceCoreSide > 0.0) {
                        maximumSourceSide = std::max(maximumSourceSide, std::abs(side));
                    }
                }
                const double coreFraction = std::clamp(
                    std::abs(sourceCoreSide) / maximumSourceSide, 0.1, 1.0);
                const QPointF targetCore = targetMiddle
                    + inward * (maximumDepth * coreFraction);
                bool ok = false;
                const QTransform transform = affineFromTriangles(
                    sourceArc.front(), sourceArc.back(), sourceCore,
                    job.span.front(), job.span.back(), targetCore, &ok);
                if (!ok || std::abs(transform.determinant()) <= kGeometryEpsilon
                    || transformCondition(transform) > options.maximumScaleCondition) {
                    continue;
                }
                Candidate candidate;
                candidate.placement.shapeId = primitive.shapeId;
                candidate.placement.transform = transform;
                candidate.placement.area = primitive.area
                    * std::abs(transform.determinant());
                candidate.path = transform.map(primitive.silhouette).simplified();
                candidate.sourceArc = transform.map(sourceArc);
                candidate.targetSpan = job.span;
                const double candidateArea = pathArea(candidate.path);
                if (candidateArea <= kGeometryEpsilon) {
                    continue;
                }
                candidate.newArea = pathArea(candidate.path.intersected(residual));
                if (candidate.newArea < kMinimumNewArea) {
                    continue;
                }
                candidate.redundantArea = pathArea(candidate.path.intersected(coverage));
                candidate.contourParity = contourParity(candidate.targetSpan,
                                                         candidate.sourceArc,
                                                         options);
                candidate.normalizedArea = candidate.newArea
                    / std::max(componentArea, kGeometryEpsilon);
                candidate.score = options.contourWeight * candidate.contourParity
                    + options.areaWeight * candidate.normalizedArea
                    - options.redundancyPenaltyWeight
                        * candidate.redundantArea / candidateArea;
                candidate.stateIndex = job.stateIndex;
                candidate.stableKey = QStringLiteral("%1:%2:%3:%4")
                    .arg(job.stateIndex, 5, 10, QLatin1Char('0'))
                    .arg(job.pointCount, 3, 10, QLatin1Char('0'))
                    .arg(primitive.shapeId, 5, 10, QLatin1Char('0'))
                    .arg(orientation * 2 + (sourceDirection > 0 ? 1 : 0),
                         3, 10, QLatin1Char('0'));
                if (!best || candidate.score > best->score + kGeometryEpsilon
                    || (std::abs(candidate.score - best->score) <= kGeometryEpsilon
                        && candidate.stableKey < best->stableKey)) {
                    best = std::move(candidate);
                }
            }
        }
    }

    return best;
}

void parallelFor(QThreadPool *pool,
                 int workerCount,
                 int count,
                 const std::function<void(int)> &function) {
    if (count <= 0) {
        return;
    }
    std::atomic<int> next{0};
    const int workers = std::min(workerCount, count);
    for (int worker = 0; worker < workers; ++worker) {
        pool->start([&next, count, &function]() {
            while (true) {
                const int index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= count) {
                    return;
                }
                function(index);
            }
        });
    }
    pool->waitForDone();
}

QVector<int> rankedSeedEdges(const QPolygonF &front, int maximumSeeds) {
    QVector<int> result(front.size());
    std::iota(result.begin(), result.end(), 0);
    const auto significance = [&front](int edge) {
        const int previous = (edge + front.size() - 1) % front.size();
        const int next = (edge + 1) % front.size();
        const QPointF incoming = normalizedVector(front[edge] - front[previous]);
        const QPointF outgoing = normalizedVector(front[next] - front[edge]);
        const double curvature = std::acos(std::clamp(
            QPointF::dotProduct(incoming, outgoing), -1.0, 1.0));
        const double length = QLineF(front[edge], front[next]).length();
        return length * (1.0 + curvature);
    };
    std::stable_sort(result.begin(), result.end(), [&](int left, int right) {
        const double leftSignificance = significance(left);
        const double rightSignificance = significance(right);
        if (std::abs(leftSignificance - rightSignificance) > kGeometryEpsilon) {
            return leftSignificance > rightSignificance;
        }
        return left < right;
    });
    result.resize(std::min(maximumSeeds, static_cast<int>(result.size())));

    return result;
}

bool betterCandidate(const Candidate &left, const Candidate &right) {
    if (left.baselineShapesReplaced != right.baselineShapesReplaced) {
        return left.baselineShapesReplaced > right.baselineShapesReplaced;
    }
    if (std::abs(left.score - right.score) > kGeometryEpsilon) {
        return left.score > right.score;
    }
    if (std::abs(left.newArea - right.newArea) > kGeometryEpsilon) {
        return left.newArea > right.newArea;
    }
    if (std::abs(left.contourParity - right.contourParity) > kGeometryEpsilon) {
        return left.contourParity > right.contourParity;
    }
    if (left.placement.shapeId != right.placement.shapeId) {
        return left.placement.shapeId < right.placement.shapeId;
    }

    return left.stableKey < right.stableKey;
}

QVector<Candidate> generateCandidates(const QPolygonF &front,
                                      const QPainterPath &target,
                                      const PathComponent &component,
                                      const QPainterPath &residual,
                                      const QPainterPath &coverage,
                                      const QVector<PenPrimitive> &primitives,
                                      const AdvancingFrontOptions &options,
                                      int cheapBudget,
                                      int *usedBudget,
                                      AdvancingFrontStats *stats,
                                      QThreadPool *pool,
                                      const std::function<bool()> &cancelled) {
    QVector<Candidate> candidates;
    if (front.size() < 3 || *usedBudget >= cheapBudget) {
        return candidates;
    }
    QVector<SpanState> states;
    const QVector<int> seeds = rankedSeedEdges(front, options.seedEdgeCount);
    states.reserve(seeds.size() * 2);
    for (const int seed : seeds) {
        states.push_back({seed, 1});
        states.push_back({seed, -1});
    }
    const int maximumSpan = std::min(
        options.maximumSpanPoints,
        std::max(2, static_cast<int>(front.size()) / 2));
    while (std::any_of(states.cbegin(), states.cend(), [](const SpanState &state) {
               return state.active;
           })) {
        if (cancelled && cancelled()) {
            return {};
        }
        QVector<CandidateJob> jobs;
        QVector<QPair<int, QPair<int, int>>> jobRanges;
        jobRanges.resize(states.size(), {-1, {-1, -1}});
        for (int stateIndex = 0; stateIndex < states.size(); ++stateIndex) {
            SpanState &state = states[stateIndex];
            if (!state.active || state.nextPointCount > maximumSpan) {
                state.active = false;
                continue;
            }
            const int firstJob = jobs.size();
            const QPolygonF span = denseSpanPoints(
                component.polygon, front, state.seedEdge,
                state.direction, state.nextPointCount);
            const SpanFamily family = spanFamily(span);
            for (const PenPrimitive &primitive : primitives) {
                if (!primitiveMatchesSpan(primitive, family)) {
                    continue;
                }
                jobs.push_back({span, &primitive, stateIndex, state.nextPointCount});
            }
            jobRanges[stateIndex] = {firstJob, {firstJob, jobs.size()}};
        }
        const int remainingBudget = cheapBudget - *usedBudget;
        if (jobs.size() > remainingBudget) {
            jobs.resize(std::max(0, remainingBudget));
            stats->cheapBudgetExhausted = true;
        }
        if (jobs.isEmpty()) {
            break;
        }
        QVector<std::optional<Candidate>> results(jobs.size());
        const auto evaluate = [&](int index) {
            results[index] = cheapCandidate(jobs[index], target, component.path,
                                            residual, coverage, component.area,
                                            options);
        };
        parallelFor(pool, options.workerCount, jobs.size(), evaluate);
        *usedBudget += jobs.size();
        stats->cheapCandidates += jobs.size();
        stats->rejectedCandidateCounts[QStringLiteral("cheap_no_fit")]
            += static_cast<int>(std::count_if(
                results.cbegin(), results.cend(),
                [](const std::optional<Candidate> &candidate) {
                    return !candidate.has_value();
                }));
        for (int stateIndex = 0; stateIndex < states.size(); ++stateIndex) {
            SpanState &state = states[stateIndex];
            const int first = jobRanges[stateIndex].first;
            const int requestedLast = jobRanges[stateIndex].second.second;
            if (!state.active || first < 0 || first >= results.size()) {
                continue;
            }
            const int last = std::min(requestedLast,
                                      static_cast<int>(results.size()));
            double spanBest = -std::numeric_limits<double>::infinity();
            for (int index = first; index < last; ++index) {
                if (!results[index]) {
                    continue;
                }
                state.maximumArea = std::max(state.maximumArea,
                                             results[index]->newArea);
                spanBest = std::max(spanBest, results[index]->score);
                candidates.push_back(std::move(*results[index]));
            }
            ++state.testedSpanCount;
            if (spanBest > state.bestScore) {
                state.bestScore = spanBest;
                state.regressionCount = 0;
            } else if (state.testedSpanCount >= options.minimumSpanTests
                       && spanBest <= state.bestScore - options.regressionScoreDrop) {
                ++state.regressionCount;
            } else {
                state.regressionCount = 0;
            }
            ++state.nextPointCount;
            if (state.regressionCount >= options.regressionSpanCount
                || state.nextPointCount > maximumSpan) {
                state.active = false;
            }
        }
        if (stats->cheapBudgetExhausted) {
            break;
        }
    }
    for (Candidate &candidate : candidates) {
        const double maximumArea = candidate.stateIndex >= 0
                && candidate.stateIndex < states.size()
            ? states[candidate.stateIndex].maximumArea : candidate.newArea;
        candidate.normalizedArea = candidate.newArea
            / std::max(maximumArea, kGeometryEpsilon);
        candidate.score = options.contourWeight * candidate.contourParity
            + options.areaWeight * candidate.normalizedArea
            - options.redundancyPenaltyWeight
                * candidate.redundantArea
                    / std::max(candidate.placement.area, kGeometryEpsilon);
    }
    std::stable_sort(candidates.begin(), candidates.end(), betterCandidate);

    return candidates;
}

int baselineShapesReplaced(const QPainterPath &candidate,
                           const QPainterPath &residual,
                           const QVector<QPainterPath> &baselinePaths,
                           double requiredCoverage) {
    int result = 0;
    const QRectF candidateBounds = candidate.boundingRect();
    for (const QPainterPath &baselinePath : baselinePaths) {
        if (!candidateBounds.intersects(baselinePath.boundingRect())) {
            continue;
        }
        const double baselineArea = pathArea(baselinePath);
        if (baselineArea <= kGeometryEpsilon) {
            continue;
        }
        const QPainterPath remaining = baselinePath.intersected(residual);
        const double remainingArea = pathArea(remaining);
        if (remainingArea < baselineArea * 0.25) {
            continue;
        }
        const double coveredArea = pathArea(candidate.intersected(remaining));
        if (coveredArea + kGeometryEpsilon >= remainingArea * requiredCoverage) {
            ++result;
        }
    }

    return result;
}

Candidate reevaluateCandidate(Candidate candidate,
                              const QPainterPath &target,
                              const QPainterPath &residual,
                              const QPainterPath &coverage,
                              const QVector<QPainterPath> &baselinePaths,
                              const QSize &imageSize,
                              const AdvancingFrontOptions &options,
                              bool refineTransform) {
    if (refineTransform) {
        if (const auto refined = leastSquaresAffine(candidate.sourceArc,
                                                     candidate.targetSpan)) {
            const QTransform adjustment = *refined;
            const QTransform refinedPlacement = candidate.placement.transform
                * adjustment;
            if (std::abs(refinedPlacement.determinant()) > kGeometryEpsilon
                && transformCondition(refinedPlacement)
                    <= options.maximumScaleCondition) {
                candidate.placement.transform = refinedPlacement;
                candidate.placement.area *= std::abs(adjustment.determinant());
                candidate.path = adjustment.map(candidate.path).simplified();
                candidate.sourceArc = adjustment.map(candidate.sourceArc);
                candidate.contourParity = contourParity(candidate.targetSpan,
                                                         candidate.sourceArc,
                                                         options);
            }
        }
    }
    candidate.newArea = pathArea(candidate.path.intersected(residual));
    candidate.redundantArea = pathArea(candidate.path.intersected(coverage));
    candidate.baselineShapesReplaced = baselineShapesReplaced(
        candidate.path, residual, baselinePaths,
        options.baselineReplacementCoverage);
    const QPainterPath nextCoverage = coverage.united(candidate.path);
    const QPainterPath clippedCoverage = nextCoverage.intersected(target);
    candidate.leakageDssim = maskDssim(nextCoverage, clippedCoverage,
                                       imageSize, options,
                                       candidate.path.boundingRect());
    const QVector<PathComponent> beforeComponents = pathComponents(residual);
    const QPainterPath residualAfter = target.subtracted(nextCoverage);
    const QVector<PathComponent> afterComponents = pathComponents(residualAfter);
    candidate.componentCountAfter = afterComponents.size();
    const int addedComponents = std::max(
        0,
        static_cast<int>(afterComponents.size() - beforeComponents.size()));
    double tinyComponentPenalty = 0.0;
    for (const PathComponent &component : afterComponents) {
        const double fraction = component.area
            / std::max(pathArea(target), kGeometryEpsilon);
        if (fraction < 0.001) {
            tinyComponentPenalty += 1.0 - fraction / 0.001;
        }
    }
    const double leakagePenalty = options.maximumLeakageDssim <= kGeometryEpsilon
        ? 0.0 : 0.1 * candidate.leakageDssim / options.maximumLeakageDssim;
    const double fragmentationPenalty = options.fragmentationPenaltyWeight
        * (addedComponents + tinyComponentPenalty);
    candidate.score = options.contourWeight * candidate.contourParity
        + options.areaWeight * candidate.normalizedArea
        - options.redundancyPenaltyWeight
            * candidate.redundantArea
                / std::max(candidate.placement.area, kGeometryEpsilon)
        - leakagePenalty - fragmentationPenalty;

    return candidate;
}

QVector<Candidate> refineCandidates(QVector<Candidate> candidates,
                                    const QPainterPath &target,
                                    const QPainterPath &residual,
                                    const QPainterPath &coverage,
                                    const QVector<QPainterPath> &baselinePaths,
                                    const QSize &imageSize,
                                    const AdvancingFrontOptions &options,
                                    int minimumShapesReplaced,
                                    int dssimBudget,
                                    int *usedBudget,
                                    AdvancingFrontStats *stats,
                                    QThreadPool *pool,
                                    const std::function<bool()> &cancelled) {
    const int candidateCount = static_cast<int>(candidates.size());
    const int finalistCount = std::min({options.finalistCount,
                                       candidateCount,
                                       dssimBudget - *usedBudget});
    if (finalistCount <= 0) {
        stats->dssimBudgetExhausted = *usedBudget >= dssimBudget;
        return {};
    }
    candidates.resize(finalistCount);
    QVector<Candidate> refined(finalistCount);
    const auto evaluate = [&](int index) {
        if (cancelled && cancelled()) {
            return;
        }
        refined[index] = reevaluateCandidate(candidates[index], target, residual,
                                             coverage, baselinePaths, imageSize,
                                             options, true);
    };
    parallelFor(pool, options.workerCount, finalistCount, evaluate);
    *usedBudget += finalistCount;
    stats->refinedCandidates += finalistCount;
    stats->prunedCandidates += std::max(0, candidateCount - finalistCount);
    stats->rejectedCandidateCounts[QStringLiteral("refinement_pruned")]
        += std::max(0, candidateCount - finalistCount);
    for (const Candidate &candidate : refined) {
        if (candidate.path.isEmpty()) {
            ++stats->rejectedCandidateCounts[QStringLiteral("refined_empty_path")];
        } else if (candidate.newArea < kMinimumNewArea) {
            ++stats->rejectedCandidateCounts[
                QStringLiteral("refined_insufficient_new_area")];
        } else if (!std::isfinite(candidate.leakageDssim)) {
            ++stats->rejectedCandidateCounts[
                QStringLiteral("refined_nonfinite_leakage")];
        } else if (candidate.leakageDssim
                   > options.maximumLeakageDssim + kGeometryEpsilon) {
            ++stats->rejectedCandidateCounts[
                QStringLiteral("refined_leakage_limit")];
        } else if (candidate.baselineShapesReplaced < minimumShapesReplaced) {
            ++stats->rejectedCandidateCounts[
                QStringLiteral("refined_shape_gain")];
        }
    }
    refined.erase(std::remove_if(refined.begin(), refined.end(), [&](const Candidate &candidate) {
                      return candidate.path.isEmpty()
                          || candidate.newArea < kMinimumNewArea
                          || !std::isfinite(candidate.leakageDssim)
                          || candidate.leakageDssim
                              > options.maximumLeakageDssim + kGeometryEpsilon
                          || candidate.baselineShapesReplaced
                              < minimumShapesReplaced;
                  }),
                  refined.end());
    std::stable_sort(refined.begin(), refined.end(), betterCandidate);

    return refined;
}

QVector<Candidate> completeFitCandidates(const PathComponent &component,
                                         const QPainterPath &target,
                                         const QPainterPath &coverage,
                                         const QVector<QPainterPath> &baselinePaths,
                                         const QVector<PenPrimitive> &primitives,
                                         const QSize &imageSize,
                                         const AdvancingFrontOptions &options,
                                         int dssimBudget,
                                         int *usedBudget,
                                         AdvancingFrontStats *stats) {
    QVector<Candidate> result;
    if (*usedBudget >= dssimBudget) {
        stats->dssimBudgetExhausted = true;
        return result;
    }
    const QPolygonF points = component.polygon;
    QPointF mean;
    for (const QPointF &point : points) {
        mean += point;
    }
    mean /= std::max(1, static_cast<int>(points.size()));
    double covarianceX = 0.0;
    double covarianceY = 0.0;
    double covarianceXY = 0.0;
    for (const QPointF &point : points) {
        const QPointF centered = point - mean;
        covarianceX += centered.x() * centered.x();
        covarianceY += centered.y() * centered.y();
        covarianceXY += centered.x() * centered.y();
    }
    const double principalAngle = 0.5 * std::atan2(2.0 * covarianceXY,
                                                   covarianceX - covarianceY);
    const std::array<double, 3> angles = {0.0,
                                          principalAngle,
                                          principalAngle + std::numbers::pi * 0.5};
    for (const PenPrimitive &primitive : primitives) {
        if (primitive.bounds.width() <= kGeometryEpsilon
            || primitive.bounds.height() <= kGeometryEpsilon) {
            continue;
        }
        for (const double angle : angles) {
            if (*usedBudget >= dssimBudget) {
                stats->dssimBudgetExhausted = true;
                break;
            }
            QTransform unrotate;
            unrotate.translate(mean.x(), mean.y());
            unrotate.rotateRadians(-angle);
            unrotate.translate(-mean.x(), -mean.y());
            const QRectF alignedBounds = unrotate.map(points).boundingRect();
            QTransform transform;
            transform.translate(mean.x(), mean.y());
            transform.rotateRadians(angle);
            transform.scale(alignedBounds.width() / primitive.bounds.width(),
                            alignedBounds.height() / primitive.bounds.height());
            transform.translate(-primitive.bounds.center().x(),
                                -primitive.bounds.center().y());
            if (transformCondition(transform) > options.maximumScaleCondition) {
                continue;
            }
            Candidate candidate;
            candidate.placement.shapeId = primitive.shapeId;
            candidate.placement.transform = transform;
            candidate.placement.area = primitive.area
                * std::abs(transform.determinant());
            candidate.path = transform.map(primitive.silhouette).simplified();
            candidate.newArea = pathArea(candidate.path.intersected(component.path));
            candidate.normalizedArea = candidate.newArea
                / std::max(component.area, kGeometryEpsilon);
            const QPainterPath nextCoverage = coverage.united(candidate.path);
            candidate.leakageDssim = maskDssim(nextCoverage,
                                               nextCoverage.intersected(target),
                                               imageSize,
                                               options,
                                               candidate.path.boundingRect());
            const double componentDssim = maskDssim(candidate.path,
                                                    component.path,
                                                    imageSize,
                                                    options,
                                                    candidate.path.boundingRect()
                                                        .united(component.bounds));
            ++*usedBudget;
            ++stats->refinedCandidates;
            if (candidate.normalizedArea + kGeometryEpsilon
                < options.completeFitCoverage) {
                ++stats->rejectedCandidateCounts[
                    QStringLiteral("complete_coverage_limit")];
                continue;
            }
            if (candidate.leakageDssim
                > options.maximumLeakageDssim + kGeometryEpsilon) {
                ++stats->rejectedCandidateCounts[
                    QStringLiteral("complete_leakage_limit")];
                continue;
            }
            if (componentDssim > options.completeFitDssim + kGeometryEpsilon) {
                ++stats->rejectedCandidateCounts[
                    QStringLiteral("complete_dssim_limit")];
                continue;
            }
            candidate.baselineShapesReplaced = baselineShapesReplaced(
                candidate.path, component.path, baselinePaths,
                options.baselineReplacementCoverage);
            QPolygonF candidateBoundary;
            double candidateBoundaryArea = 0.0;
            for (QPolygonF polygon : candidate.path.toSubpathPolygons()) {
                polygon = normalizedPolygon(std::move(polygon));
                const double area = polygonArea(polygon);
                if (area > candidateBoundaryArea) {
                    candidateBoundaryArea = area;
                    candidateBoundary = std::move(polygon);
                }
            }
            QPolygonF openComponent = component.polygon;
            openComponent.push_back(openComponent.front());
            if (!candidateBoundary.isEmpty()) {
                candidateBoundary.push_back(candidateBoundary.front());
            }
            candidate.contourParity = contourParity(openComponent,
                                                     candidateBoundary,
                                                     options);
            candidate.score = options.contourWeight * candidate.contourParity
                + options.areaWeight * candidate.normalizedArea
                - 0.1 * candidate.leakageDssim
                    / std::max(options.maximumLeakageDssim, kGeometryEpsilon);
            candidate.stableKey = QStringLiteral("complete:%1:%2")
                .arg(primitive.shapeId, 5, 10, QLatin1Char('0'))
                .arg(angle, 0, 'f', 8);
            result.push_back(std::move(candidate));
        }
    }
    std::stable_sort(result.begin(), result.end(), betterCandidate);

    return result;
}

QPainterPath coverageForSteps(const QVector<AcceptedStep> &steps) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    for (const AcceptedStep &step : steps) {
        result = result.united(step.selected.path);
    }

    return result;
}

bool tryBacktrack(QVector<AcceptedStep> *steps,
                  QPainterPath *coverage,
                  const QPainterPath &target,
                  const QVector<QPainterPath> &baselinePaths,
                  const QSize &imageSize,
                  const AdvancingFrontOptions &options,
                  AdvancingFrontStats *stats) {
    for (int depth = 0;
         depth < options.rollbackDepth && !steps->isEmpty();
         ++depth) {
        AcceptedStep step = steps->takeLast();
        ++stats->backtracks;
        *coverage = coverageForSteps(*steps);
        const QPainterPath residual = target.subtracted(*coverage);
        while (step.nextAlternative < step.alternatives.size()) {
            Candidate alternative = reevaluateCandidate(
                step.alternatives[step.nextAlternative++], target, residual,
                *coverage, baselinePaths, imageSize, options, false);
            if (alternative.newArea < kMinimumNewArea
                || alternative.leakageDssim
                    > options.maximumLeakageDssim + kGeometryEpsilon) {
                continue;
            }
            step.selected = std::move(alternative);
            steps->push_back(std::move(step));
            *coverage = coverage->united(steps->back().selected.path);
            return true;
        }
    }

    return false;
}

QPainterPath discardSmallResiduals(const QPainterPath &target,
                                   const QPainterPath &residual,
                                   const QSize &imageSize,
                                   const AdvancingFrontOptions &options,
                                   int dssimBudget,
                                   int *usedBudget,
                                   AdvancingFrontStats *stats,
                                   QPainterPath *discarded) {
    QVector<PathComponent> components = pathComponents(residual);
    std::reverse(components.begin(), components.end());
    QPainterPath kept = residual;
    for (const PathComponent &component : components) {
        if (*usedBudget >= dssimBudget) {
            stats->dssimBudgetExhausted = true;
            break;
        }
        const QPainterPath candidateDiscarded = discarded->united(component.path);
        const QPainterPath candidateVisible = target.subtracted(candidateDiscarded);
        const double omission = maskDssim(target, candidateVisible,
                                          imageSize, options,
                                          candidateDiscarded.boundingRect());
        ++*usedBudget;
        ++stats->refinedCandidates;
        if (omission > options.maximumOmissionDssim + kGeometryEpsilon) {
            continue;
        }
        *discarded = candidateDiscarded;
        kept = kept.subtracted(component.path);
        stats->discardedArea += component.area;
        stats->omissionDssim = omission;
    }

    return kept;
}

const PenPrimitive *primitiveForId(const QVector<PenPrimitive> &primitives,
                                   int shapeId) {
    const auto found = std::find_if(primitives.cbegin(), primitives.cend(),
                                    [shapeId](const PenPrimitive &primitive) {
        return primitive.shapeId == shapeId;
    });

    return found == primitives.cend() ? nullptr : &*found;
}

bool appendFallback(const QPainterPath &residual,
                    const QVector<PenPrimitive> &primitives,
                    const PolygonMeshSources &meshSources,
                    const AdvancingFrontOptions &options,
                    QVector<PenPlacement> *placements,
                    QPainterPath *coverage,
                    AdvancingFrontStats *stats,
                    const std::function<bool()> &cancelled,
                    QString *error) {
    const QVector<PathComponent> components = pathComponents(residual);
    if (components.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Residual decomposition produced no components");
        }
        return false;
    }
    QVector<PenPlacement> fallbackPlacements;
    QPainterPath fallbackCoverage;
    fallbackCoverage.setFillRule(Qt::WindingFill);
    QHash<int, int> fallbackPrimitiveCounts;
    for (const PathComponent &component : components) {
        if (cancelled && cancelled()) {
            return false;
        }
        const PenFillResult fallback = fillRegionOutlineMesh(
            component.path, meshSources, options.fallbackContourEpsilon, cancelled);
        if (fallback.cancelled) {
            return false;
        }
        if (!fallback.error.isEmpty() || fallback.placements.isEmpty()) {
            if (error != nullptr) {
                *error = fallback.error.isEmpty()
                    ? QStringLiteral("Residual mesh produced no placements")
                    : fallback.error;
            }
            return false;
        }
        for (const PenPlacement &placement : fallback.placements) {
            const PenPrimitive *primitive = primitiveForId(primitives,
                                                           placement.shapeId);
            if (primitive == nullptr) {
                if (error != nullptr) {
                    *error = QStringLiteral("Residual mesh used an unavailable primitive");
                }
                return false;
            }
            fallbackPlacements.push_back(placement);
            fallbackCoverage = fallbackCoverage.united(
                placement.transform.map(primitive->silhouette));
            ++fallbackPrimitiveCounts[placement.shapeId];
            if (options.maximumOutputShapes >= 0
                && placements->size() + fallbackPlacements.size()
                    > options.maximumOutputShapes) {
                stats->countLimitReached = true;
                if (error != nullptr) {
                    *error = QStringLiteral(
                        "Residual fallback cannot beat the Safe shape count");
                }
                return false;
            }
        }
    }
    *placements += fallbackPlacements;
    *coverage = coverage->united(fallbackCoverage);
    stats->fallbackShapes += fallbackPlacements.size();
    for (auto it = fallbackPrimitiveCounts.cbegin();
         it != fallbackPrimitiveCounts.cend(); ++it) {
        stats->acceptedPrimitiveCounts[it.key()] += it.value();
    }

    return true;
}

} // namespace

AdvancingFrontOptions defaultAdvancingFrontOptions() {
    AdvancingFrontOptions result;
    result.workerCount = std::max(1, QThread::idealThreadCount() - 1);

    return result;
}

QString advancingFrontOptionsLog(const AdvancingFrontOptions &options) {
    QStringList values;
    QStringList epsilonValues;
    for (const double epsilon : options.exposedSimplificationEpsilons) {
        epsilonValues.push_back(QString::number(epsilon, 'g', 8));
    }
    values << QStringLiteral("original_contour_epsilon=%1")
                  .arg(options.originalContourEpsilon, 0, 'g', 8)
           << QStringLiteral("exposed_epsilons=%1").arg(epsilonValues.join(','))
           << QStringLiteral("fallback_contour_epsilon=%1")
                  .arg(options.fallbackContourEpsilon, 0, 'g', 8)
           << QStringLiteral("candidate_guide_epsilon=%1")
                  .arg(options.candidateGuideEpsilon, 0, 'g', 8)
           << QStringLiteral("contour_match_distance=%1")
                  .arg(options.contourMatchDistance, 0, 'g', 8)
           << QStringLiteral("sample_spacing=%1").arg(options.sampleSpacing, 0, 'g', 8)
           << QStringLiteral("contour_weight=%1").arg(options.contourWeight, 0, 'g', 8)
           << QStringLiteral("area_weight=%1").arg(options.areaWeight, 0, 'g', 8)
           << QStringLiteral("regression_score_drop=%1")
                  .arg(options.regressionScoreDrop, 0, 'g', 8)
           << QStringLiteral("maximum_leakage_dssim=%1")
                  .arg(options.maximumLeakageDssim, 0, 'g', 8)
           << QStringLiteral("maximum_omission_dssim=%1")
                  .arg(options.maximumOmissionDssim, 0, 'g', 8)
           << QStringLiteral("maximum_final_dssim=%1")
                  .arg(options.maximumFinalDssim, 0, 'g', 8)
           << QStringLiteral("complete_fit_dssim=%1")
                  .arg(options.completeFitDssim, 0, 'g', 8)
           << QStringLiteral("complete_fit_coverage=%1")
                  .arg(options.completeFitCoverage, 0, 'g', 8)
           << QStringLiteral("maximum_scale_condition=%1")
                  .arg(options.maximumScaleCondition, 0, 'g', 8)
           << QStringLiteral("redundancy_penalty_weight=%1")
                  .arg(options.redundancyPenaltyWeight, 0, 'g', 8)
           << QStringLiteral("fragmentation_penalty_weight=%1")
                  .arg(options.fragmentationPenaltyWeight, 0, 'g', 8)
           << QStringLiteral("minimum_target_coverage=%1")
                  .arg(options.minimumTargetCoverage, 0, 'g', 8)
           << QStringLiteral("maximum_leakage_fraction=%1")
                  .arg(options.maximumLeakageFraction, 0, 'g', 8)
           << QStringLiteral("baseline_replacement_coverage=%1")
                  .arg(options.baselineReplacementCoverage, 0, 'g', 8)
           << QStringLiteral("dssim_supersample=%1").arg(options.dssimSupersample)
           << QStringLiteral("dssim_padding=%1").arg(options.dssimPadding)
           << QStringLiteral("seed_edge_count=%1").arg(options.seedEdgeCount)
           << QStringLiteral("finalist_count=%1").arg(options.finalistCount)
           << QStringLiteral("candidate_guide_point_cap=%1")
                  .arg(options.candidateGuidePointCap)
           << QStringLiteral("preflight_candidate_budget=%1")
                  .arg(options.preflightCandidateBudget)
           << QStringLiteral("minimum_baseline_shapes_replaced=%1")
                  .arg(options.minimumBaselineShapesReplaced)
           << QStringLiteral("primitive_boundary_orientations=%1")
                  .arg(options.primitiveBoundaryOrientations)
           << QStringLiteral("minimum_span_tests=%1").arg(options.minimumSpanTests)
           << QStringLiteral("regression_span_count=%1")
                  .arg(options.regressionSpanCount)
           << QStringLiteral("maximum_span_points=%1").arg(options.maximumSpanPoints)
           << QStringLiteral("boundary_samples=%1..%2")
                  .arg(options.minimumBoundarySamples)
                  .arg(options.maximumBoundarySamples)
           << QStringLiteral("rollback_depth=%1").arg(options.rollbackDepth)
           << QStringLiteral("alternatives_per_step=%1")
                  .arg(options.alternativesPerStep)
           << QStringLiteral("cheap_budget=%1+%2*points cap %3")
                  .arg(options.cheapBudgetBase)
                  .arg(options.cheapBudgetPerPoint)
                  .arg(options.cheapBudgetCap)
           << QStringLiteral("dssim_budget=%1+%2*points cap %3")
                  .arg(options.dssimBudgetBase)
                  .arg(options.dssimBudgetPerPoint)
                  .arg(options.dssimBudgetCap)
           << QStringLiteral("coverage_rebuild_interval=%1")
                  .arg(options.coverageRebuildInterval)
           << QStringLiteral("worker_count=%1").arg(options.workerCount)
           << QStringLiteral("maximum_output_shapes=%1")
                  .arg(options.maximumOutputShapes)
           << QStringLiteral("baseline_placements=%1")
                  .arg(options.baselinePlacements.size())
           << QStringLiteral("verbose_rejected_candidates=%1")
                  .arg(options.verboseRejectedCandidates ? QStringLiteral("yes")
                                                         : QStringLiteral("no"));

    return values.join(QLatin1Char('\n'));
}

AdvancingFrontResult fillRegionAdvancingFront(
    const QPainterPath &target,
    const QVector<PenPrimitive> &primitives,
    const PolygonMeshSources &meshSources,
    const QSize &imageSize,
    const AdvancingFrontOptions &requestedOptions,
    const std::function<void(double, int)> &progress,
    const std::function<bool()> &cancelled) {
    AdvancingFrontResult result;
    AdvancingFrontOptions options = requestedOptions;
    if (options.workerCount <= 0) {
        options.workerCount = std::max(1, QThread::idealThreadCount() - 1);
    }
    QElapsedTimer totalClock;
    totalClock.start();
    result.stats.initialArea = pathArea(target);
    if (target.isEmpty() || result.stats.initialArea <= kGeometryEpsilon) {
        result.error = QStringLiteral("Advancing Front target is empty");
        return result;
    }
    if (primitives.isEmpty() || !meshSources.valid()) {
        result.error = QStringLiteral("Advancing Front primitive geometry is unavailable");
        return result;
    }
    if (options.maximumOutputShapes == 0) {
        result.stats.countLimitReached = true;
        result.error = QStringLiteral("Advancing Front cannot beat a one-shape Safe result");
        result.stats.totalElapsedMs = totalClock.elapsed();
        return result;
    }
    QVector<QPainterPath> baselinePaths;
    baselinePaths.reserve(options.baselinePlacements.size());
    for (const PenPlacement &placement : options.baselinePlacements) {
        const PenPrimitive *primitive = primitiveForId(primitives,
                                                       placement.shapeId);
        if (primitive != nullptr) {
            baselinePaths.push_back(
                placement.transform.map(primitive->silhouette).simplified());
        }
    }
    QVector<PathComponent> initialComponents = pathComponents(target);
    for (const PathComponent &component : initialComponents) {
        result.stats.originalPointCount += component.polygon.size();
        result.stats.guidePointCount += limitGuidePoints(
            simplifyClosedPolygonCyclic(component.polygon,
                                        options.candidateGuideEpsilon),
            options.candidateGuidePointCap).size();
    }
    const int pointCount = std::max(1, result.stats.guidePointCount);
    const int cheapBudget = std::min(options.cheapBudgetCap,
                                     options.cheapBudgetBase
                                         + options.cheapBudgetPerPoint * pointCount);
    const int dssimBudget = std::min(options.dssimBudgetCap,
                                     options.dssimBudgetBase
                                         + options.dssimBudgetPerPoint * pointCount);
    int usedCheapBudget = 0;
    int usedDssimBudget = 0;
    QPainterPath coverage;
    coverage.setFillRule(Qt::WindingFill);
    QPainterPath discarded;
    discarded.setFillRule(Qt::WindingFill);
    QVector<AcceptedStep> steps;
    QThreadPool candidatePool;
    candidatePool.setMaxThreadCount(options.workerCount);
    bool forceFallback = false;
    bool preflight = !baselinePaths.isEmpty();
    int acceptedSinceRebuild = 0;

    while (true) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.error = QStringLiteral("Advancing Front cancelled");
            break;
        }
        QPainterPath residual = target.subtracted(coverage).subtracted(discarded);
        residual = discardSmallResiduals(target, residual, imageSize, options,
                                         dssimBudget, &usedDssimBudget,
                                         &result.stats, &discarded);
        QVector<PathComponent> components = pathComponents(residual);
        result.stats.residualArea = pathArea(residual);
        if (progress) {
            progress(result.stats.residualArea
                         / std::max(result.stats.initialArea, kGeometryEpsilon),
                     result.stats.cheapCandidates);
        }
        if (components.isEmpty() || result.stats.residualArea <= kGeometryEpsilon) {
            break;
        }
        if (options.maximumOutputShapes >= 0
            && steps.size() >= options.maximumOutputShapes) {
            result.stats.countLimitReached = true;
            result.error = QStringLiteral(
                "Advancing Front reached the Safe shape-count limit");
            break;
        }
        if (forceFallback || usedCheapBudget >= cheapBudget
            || usedDssimBudget >= dssimBudget) {
            result.stats.cheapBudgetExhausted = usedCheapBudget >= cheapBudget;
            result.stats.dssimBudgetExhausted = usedDssimBudget >= dssimBudget;
            break;
        }

        QVector<Candidate> completeCandidates;
        for (const PathComponent &component : components) {
            QVector<Candidate> componentCandidates = completeFitCandidates(
                component, target, coverage, baselinePaths, primitives,
                imageSize, options, dssimBudget, &usedDssimBudget,
                &result.stats);
            if (preflight) {
                const int candidateCount = componentCandidates.size();
                componentCandidates.erase(
                    std::remove_if(componentCandidates.begin(),
                                   componentCandidates.end(),
                                   [&](const Candidate &candidate) {
                    return candidate.baselineShapesReplaced
                        < options.minimumBaselineShapesReplaced;
                }), componentCandidates.end());
                result.stats.rejectedCandidateCounts[
                    QStringLiteral("complete_shape_gain")]
                    += candidateCount - componentCandidates.size();
            }
            completeCandidates += componentCandidates;
            if (usedDssimBudget >= dssimBudget) {
                break;
            }
        }
        if (!completeCandidates.isEmpty()) {
            std::stable_sort(completeCandidates.begin(), completeCandidates.end(),
                             betterCandidate);
            const int alternatives = std::min(
                options.alternativesPerStep,
                static_cast<int>(completeCandidates.size()));
            completeCandidates.resize(alternatives);
            AcceptedStep step;
            step.alternatives = completeCandidates;
            step.selected = step.alternatives.front();
            coverage = coverage.united(step.selected.path);
            steps.push_back(std::move(step));
            ++result.stats.acceptedCandidates;
            preflight = false;
            forceFallback = completeCandidates.front().normalizedArea
                < 1.0 - kGeometryEpsilon;
            continue;
        }

        const PathComponent component = components.front();
        const QPolygonF front = exposedGuideContour(component, target,
                                                    imageSize, options);
        result.stats.guidePointCount = std::max(
            result.stats.guidePointCount,
            static_cast<int>(front.size()));
        QElapsedTimer candidateClock;
        candidateClock.start();
        const int candidateBudget = preflight
            ? std::min(cheapBudget,
                       usedCheapBudget + options.preflightCandidateBudget)
            : cheapBudget;
        const int candidatesBefore = result.stats.cheapCandidates;
        QVector<Candidate> candidates = generateCandidates(
            front, target, component, residual, coverage, primitives, options,
            candidateBudget, &usedCheapBudget, &result.stats, &candidatePool,
            cancelled);
        if (preflight) {
            result.stats.preflightCandidates +=
                result.stats.cheapCandidates - candidatesBefore;
            if (candidateBudget < cheapBudget) {
                result.stats.cheapBudgetExhausted = false;
            }
        }
        result.stats.candidateElapsedMs += candidateClock.elapsed();
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.error = QStringLiteral("Advancing Front cancelled");
            break;
        }
        QElapsedTimer dssimClock;
        dssimClock.start();
        const int componentCountBefore = components.size();
        candidates = refineCandidates(std::move(candidates), target, residual,
                                      coverage, baselinePaths, imageSize, options,
                                      baselinePaths.isEmpty() ? 0
                                          : (preflight
                                              ? options.minimumBaselineShapesReplaced
                                              : 1),
                                      dssimBudget, &usedDssimBudget,
                                      &result.stats, &candidatePool, cancelled);
        result.stats.dssimElapsedMs += dssimClock.elapsed();
        if (candidates.isEmpty()) {
            if (preflight) {
                result.stats.preflightRejected = true;
            }
            if (tryBacktrack(&steps, &coverage, target, baselinePaths, imageSize,
                             options, &result.stats)) {
                continue;
            }
            break;
        }
        const int alternativeCount = std::min(
            options.alternativesPerStep,
            static_cast<int>(candidates.size()));
        candidates.resize(alternativeCount);
        AcceptedStep step;
        step.alternatives = candidates;
        step.selected = step.alternatives.front();
        if (step.selected.componentCountAfter > componentCountBefore) {
            result.stats.componentSplits += step.selected.componentCountAfter
                - componentCountBefore;
        }
        coverage = coverage.united(step.selected.path);
        steps.push_back(std::move(step));
        ++result.stats.acceptedCandidates;
        preflight = false;
        ++acceptedSinceRebuild;
        if (acceptedSinceRebuild >= options.coverageRebuildInterval) {
            coverage = coverageForSteps(steps);
            acceptedSinceRebuild = 0;
        }
    }

    for (const AcceptedStep &step : steps) {
        result.placements.push_back(step.selected.placement);
        ++result.stats.acceptedPrimitiveCounts[step.selected.placement.shapeId];
    }
    result.stats.acceptedCandidates = steps.size();
    result.coverage = coverageForSteps(steps);
    result.stats.baselineShapesReplaced = baselineShapesReplaced(
        result.coverage, target, baselinePaths,
        options.baselineReplacementCoverage);
    result.residual = target.subtracted(result.coverage).subtracted(discarded);
    if (!result.cancelled && result.error.isEmpty() && steps.isEmpty()
        && !baselinePaths.isEmpty()) {
        result.stats.preflightRejected = true;
        result.error = QStringLiteral(
            "Preflight found no candidate that consolidates Safe shapes");
    }
    if (!result.cancelled && result.error.isEmpty() && !result.residual.isEmpty()
        && pathArea(result.residual) > kGeometryEpsilon) {
        QElapsedTimer fallbackClock;
        fallbackClock.start();
        QString fallbackError;
        const bool fallbackSucceeded = appendFallback(
            result.residual, primitives, meshSources, options,
            &result.placements, &result.coverage, &result.stats,
            cancelled, &fallbackError);
        if (!fallbackSucceeded) {
            if (cancelled && cancelled()) {
                result.cancelled = true;
                result.error = QStringLiteral("Advancing Front cancelled");
            } else {
                result.error = fallbackError.isEmpty()
                    ? QStringLiteral("Residual fallback failed") : fallbackError;
                result.stats.diagnostics.push_back(
                    QStringLiteral("fallback_failure=%1").arg(result.error));
            }
        }
        result.stats.fallbackElapsedMs = fallbackClock.elapsed();
    }
    result.residual = target.subtracted(result.coverage).subtracted(discarded);
    result.stats.residualArea = pathArea(result.residual);
    const QPainterPath clippedCoverage = result.coverage.intersected(target);
    result.stats.leakageDssim = maskDssim(result.coverage, clippedCoverage,
                                         imageSize, options);
    result.stats.finalDssim = maskDssim(target, result.coverage,
                                       imageSize, options);
    const CoverageMetrics metrics = coverageMetrics(
        target, result.coverage, imageSize, options.dssimSupersample);
    result.stats.targetCoverage = metrics.targetCoverage;
    result.stats.leakageFraction = metrics.leakageFraction;
    if (!result.cancelled && result.error.isEmpty()) {
        if (!metrics.valid) {
            result.error = QStringLiteral("Final coverage comparison failed");
        } else if (metrics.targetCoverage + kGeometryEpsilon
                   < options.minimumTargetCoverage) {
            result.error = QStringLiteral(
                "Final target coverage %1 is below %2")
                .arg(metrics.targetCoverage, 0, 'g', 8)
                .arg(options.minimumTargetCoverage, 0, 'g', 8);
        } else if (metrics.leakageFraction
                   > options.maximumLeakageFraction + kGeometryEpsilon) {
            result.error = QStringLiteral(
                "Final leakage fraction %1 exceeds %2")
                .arg(metrics.leakageFraction, 0, 'g', 8)
                .arg(options.maximumLeakageFraction, 0, 'g', 8);
        } else if (options.maximumOutputShapes >= 0
                   && result.placements.size() > options.maximumOutputShapes) {
            result.stats.countLimitReached = true;
            result.error = QStringLiteral(
                "Advancing Front did not beat the Safe shape count");
        }
    }
    result.structurallyComplete = result.error.isEmpty() && metrics.valid;
    result.stats.finalGuardFailed = result.stats.finalDssim
        > options.maximumFinalDssim + kGeometryEpsilon;
    if (result.stats.finalGuardFailed) {
        result.stats.diagnostics.push_back(QStringLiteral(
            "WARNING=final DSSIM guard failed (%1 > %2); filled shapes retained")
                .arg(result.stats.finalDssim, 0, 'g', 8)
                .arg(options.maximumFinalDssim, 0, 'g', 8));
    }
    if (!result.cancelled && result.error.isEmpty()
        && result.placements.isEmpty()) {
        result.error = QStringLiteral("Advancing Front produced no placements");
    }
    result.stats.totalElapsedMs = totalClock.elapsed();
    result.stats.diagnostics.push_back(
        QStringLiteral("budgets cheap=%1/%2 dssim=%3/%4")
            .arg(usedCheapBudget)
            .arg(cheapBudget)
            .arg(usedDssimBudget)
            .arg(dssimBudget));

    return result;
}

} // namespace gui
