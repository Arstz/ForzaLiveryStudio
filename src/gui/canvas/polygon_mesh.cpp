#include "polygon_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr double kEpsilon = 1e-9;

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

double polygonScale(const QPolygonF &polygon)
{
    const QRectF bounds = polygon.boundingRect();
    return std::max({1.0, bounds.width(), bounds.height()});
}

double distanceSquaredToSegment(const QPointF &point, const QPointF &a, const QPointF &b)
{
    const QPointF edge = b - a;
    const double length2 = QPointF::dotProduct(edge, edge);
    if (length2 <= kEpsilon) {
        return QPointF::dotProduct(point - a, point - a);
    }
    const double t = std::clamp(QPointF::dotProduct(point - a, edge) / length2, 0.0, 1.0);
    const QPointF delta = point - (a + edge * t);
    return QPointF::dotProduct(delta, delta);
}

double polygonCoordinateEpsilon(const QPolygonF &polygon)
{
    double magnitude = polygonScale(polygon);
    for (const QPointF &point : polygon) {
        magnitude = std::max({magnitude, std::abs(point.x()), std::abs(point.y())});
    }
    return std::max(1e-12,
                    magnitude * std::numeric_limits<double>::epsilon() * 32.0);
}

int orientation(const QPointF &a,
                const QPointF &b,
                const QPointF &c,
                double coordinateEpsilon)
{
    const QPointF edge = b - a;
    const QPointF relative = c - a;
    const double value = cross(edge, relative);
    const double roundoff = (std::abs(edge.x() * relative.y())
                             + std::abs(edge.y() * relative.x()))
        * std::numeric_limits<double>::epsilon() * 8.0;
    const double geometric = std::hypot(edge.x(), edge.y()) * coordinateEpsilon;
    if (std::abs(value) <= std::max(roundoff, geometric)) {
        return 0;
    }
    return value > 0.0 ? 1 : -1;
}

bool pointOnSegment(const QPointF &point,
                    const QPointF &a,
                    const QPointF &b,
                    double coordinateEpsilon)
{
    return orientation(a, b, point, coordinateEpsilon) == 0
        && point.x() >= std::min(a.x(), b.x()) - coordinateEpsilon
        && point.x() <= std::max(a.x(), b.x()) + coordinateEpsilon
        && point.y() >= std::min(a.y(), b.y()) - coordinateEpsilon
        && point.y() <= std::max(a.y(), b.y()) + coordinateEpsilon;
}

bool segmentIntersection(const QPointF &a,
                         const QPointF &b,
                         const QPointF &c,
                         const QPointF &d,
                         double coordinateEpsilon,
                         QPointF *intersection)
{
    const int o1 = orientation(a, b, c, coordinateEpsilon);
    const int o2 = orientation(a, b, d, coordinateEpsilon);
    const int o3 = orientation(c, d, a, coordinateEpsilon);
    const int o4 = orientation(c, d, b, coordinateEpsilon);
    if (o1 != o2 && o1 != 0 && o2 != 0 && o3 != o4 && o3 != 0 && o4 != 0) {
        const QPointF r = b - a;
        const QPointF s = d - c;
        const double denominator = cross(r, s);
        if (intersection != nullptr) {
            *intersection = std::abs(denominator) > 0.0
                ? a + r * (cross(c - a, s) / denominator)
                : (a + b + c + d) * 0.25;
        }
        return true;
    }

    const auto endpointIntersection = [&](int value,
                                          const QPointF &point,
                                          const QPointF &segmentStart,
                                          const QPointF &segmentEnd) {
        if (value != 0
            || !pointOnSegment(point, segmentStart, segmentEnd, coordinateEpsilon)) {
            return false;
        }
        if (intersection != nullptr) {
            *intersection = point;
        }
        return true;
    };
    if (endpointIntersection(o1, c, a, b)
        || endpointIntersection(o2, d, a, b)
        || endpointIntersection(o3, a, c, d)
        || endpointIntersection(o4, b, c, d)) {
        return true;
    }
    return false;
}

QVector<QPointF> polygonCrossings(const QPolygonF &polygon)
{
    struct Edge {
        QPointF a;
        QPointF b;
        QRectF bounds;
        int index = -1;
    };

    QVector<QPointF> result;
    const int count = polygon.size();
    const double coordinateEpsilon = polygonCoordinateEpsilon(polygon);
    QVector<Edge> edges;
    edges.reserve(count);
    for (int i = 0; i < count; ++i) {
        const QPointF a = polygon[i];
        const QPointF b = polygon[(i + 1) % count];
        edges.push_back({a, b, QRectF(a, b).normalized(), i});
    }
    std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) {
        if (a.bounds.left() != b.bounds.left()) {
            return a.bounds.left() < b.bounds.left();
        }
        return a.index < b.index;
    });

    for (int i = 0; i < edges.size(); ++i) {
        const Edge &first = edges[i];
        for (int j = i + 1; j < edges.size(); ++j) {
            const Edge &second = edges[j];
            if (second.bounds.left() > first.bounds.right() + coordinateEpsilon) {
                break;
            }
            if (second.bounds.top() > first.bounds.bottom() + coordinateEpsilon
                || first.bounds.top() > second.bounds.bottom() + coordinateEpsilon) {
                continue;
            }
            const int firstNext = (first.index + 1) % count;
            const int secondNext = (second.index + 1) % count;
            if (firstNext == second.index || secondNext == first.index) {
                continue;
            }
            QPointF point;
            if (segmentIntersection(first.a,
                                    first.b,
                                    second.a,
                                    second.b,
                                    coordinateEpsilon,
                                    &point)) {
                const bool duplicate = std::any_of(result.begin(), result.end(), [&](const QPointF &existing) {
                    return QLineF(existing, point).length() <= coordinateEpsilon * 2.0;
                });
                if (duplicate) {
                    continue;
                }
                result.push_back(point);
            }
        }
    }
    return result;
}

QPolygonF convexHull(QVector<QPointF> points)
{
    std::sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    points.erase(std::unique(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) {
                     return QLineF(a, b).length() <= kEpsilon;
                 }),
                 points.end());
    if (points.size() <= 2) {
        return QPolygonF(points);
    }

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

QPolygonF geometryHull(const ShapeGeometry *geometry)
{
    if (geometry == nullptr) {
        return {};
    }
    QVector<QPointF> points;
    points.reserve(geometry->triangles.size() * 3);
    for (const ShapeTriangle &triangle : geometry->triangles) {
        if (triangle.alpha0 > 0.0) {
            points.push_back(triangle.p0);
        }
        if (triangle.alpha1 > 0.0) {
            points.push_back(triangle.p1);
        }
        if (triangle.alpha2 > 0.0) {
            points.push_back(triangle.p2);
        }
    }
    return convexHull(std::move(points));
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

bool pointInTriangle(const QPointF &point,
                     const QPointF &a,
                     const QPointF &b,
                     const QPointF &c,
                     double epsilon)
{
    return cross(b - a, point - a) >= -epsilon
        && cross(c - b, point - b) >= -epsilon
        && cross(a - c, point - c) >= -epsilon;
}

struct MeshTriangle {
    int a = -1;
    int b = -1;
    int c = -1;
};

QVector<MeshTriangle> triangulate(const QPolygonF &polygon,
                                  double epsilon,
                                  const std::function<bool()> &cancelled,
                                  QString *error)
{
    QVector<int> remaining;
    remaining.reserve(polygon.size());
    for (int i = 0; i < polygon.size(); ++i) {
        remaining.push_back(i);
    }
    if (signedArea(polygon) < 0.0) {
        std::reverse(remaining.begin(), remaining.end());
    }

    QVector<MeshTriangle> triangles;
    triangles.reserve(polygon.size() - 2);
    while (remaining.size() > 3) {
        if (cancelled && cancelled()) {
            return {};
        }
        bool clipped = false;
        for (int i = 0; i < remaining.size(); ++i) {
            if (cancelled && cancelled()) {
                return {};
            }
            const int previous = remaining[(i + remaining.size() - 1) % remaining.size()];
            const int current = remaining[i];
            const int next = remaining[(i + 1) % remaining.size()];
            if (cross(polygon[current] - polygon[previous],
                      polygon[next] - polygon[current]) <= epsilon) {
                continue;
            }
            bool containsVertex = false;
            for (int candidate : remaining) {
                if (candidate == previous || candidate == current || candidate == next) {
                    continue;
                }
                if (pointInTriangle(polygon[candidate],
                                    polygon[previous],
                                    polygon[current],
                                    polygon[next],
                                    epsilon)) {
                    containsVertex = true;
                    break;
                }
            }
            if (containsVertex) {
                continue;
            }
            triangles.push_back({previous, current, next});
            remaining.removeAt(i);
            clipped = true;
            break;
        }
        if (!clipped) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not triangulate the lasso polygon");
            }
            return {};
        }
    }
    triangles.push_back({remaining[0], remaining[1], remaining[2]});
    return triangles;
}

QPainterPath polygonPath(const QPolygonF &polygon)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addPolygon(polygon);
    path.closeSubpath();
    return path;
}

double pathArea(const QPainterPath &path)
{
    double result = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        result += signedArea(polygon);
    }
    return std::abs(result);
}

QPointF polygonCentroid(const QPolygonF &polygon)
{
    QPointF weighted;
    double twiceArea = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &first = polygon[i];
        const QPointF &second = polygon[(i + 1) % polygon.size()];
        const double weight = cross(first, second);
        weighted += (first + second) * weight;
        twiceArea += weight;
    }
    return std::abs(twiceArea) > kEpsilon
        ? weighted / (3.0 * twiceArea)
        : polygon.boundingRect().center();
}

struct MeshShape {
    int shapeId = 103;
    QPolygonF polygon;
    QPainterPath path;
    bool anchorTriangle = false;
};

struct RectangleCandidate {
    QPolygonF polygon;
    QPainterPath path;
    QBitArray triangles;
    double area = 0.0;
    double centerPenalty = 0.0;
};

QPolygonF tightRectangle(const QPolygonF &points,
                         const QPointF &axis,
                         double epsilon)
{
    const double length = std::hypot(axis.x(), axis.y());
    if (length <= epsilon) {
        return {};
    }
    const QPointF horizontal = axis / length;
    const QPointF vertical(-horizontal.y(), horizontal.x());
    double left = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double top = std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();
    for (const QPointF &point : points) {
        const double x = QPointF::dotProduct(point, horizontal);
        const double y = QPointF::dotProduct(point, vertical);
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
    if (right - left <= epsilon || bottom - top <= epsilon) {
        return {};
    }
    const auto pointAt = [&](double x, double y) {
        return horizontal * x + vertical * y;
    };
    return {pointAt(left, top),
            pointAt(right, top),
            pointAt(right, bottom),
            pointAt(left, bottom)};
}

QByteArray rectangleKey(const QPolygonF &rectangle, double quantum)
{
    QVector<QPair<qint64, qint64>> points;
    points.reserve(rectangle.size());
    for (const QPointF &point : rectangle) {
        points.push_back({
            qRound64(point.x() / quantum),
            qRound64(point.y() / quantum)});
    }
    std::sort(points.begin(), points.end());
    QByteArray key;
    for (const auto &point : points) {
        key += QByteArray::number(point.first);
        key += ',';
        key += QByteArray::number(point.second);
        key += ';';
    }
    return key;
}

QByteArray edgeKey(const QPointF &first,
                   const QPointF &second,
                   double quantum)
{
    QPair<qint64, qint64> a = {
        qRound64(first.x() / quantum),
        qRound64(first.y() / quantum)};
    QPair<qint64, qint64> b = {
        qRound64(second.x() / quantum),
        qRound64(second.y() / quantum)};
    if (b.first < a.first || (b.first == a.first && b.second < a.second)) {
        std::swap(a, b);
    }
    return QByteArray::number(a.first) + ',' + QByteArray::number(a.second)
        + ';' + QByteArray::number(b.first) + ',' + QByteArray::number(b.second);
}

QPolygonF adjacentTriangleParallelogram(const MeshShape &first,
                                        const MeshShape &second,
                                        double scale,
                                        double coordinateTolerance)
{
    const double linearTolerance = std::max(coordinateTolerance * 4.0,
                                            scale * 1e-9);
    const double linearToleranceSquared = linearTolerance * linearTolerance;
    QVector<QPointF> unique;
    unique.reserve(4);
    const auto appendUnique = [&](const QPointF &point) {
        const bool duplicate = std::any_of(
            unique.begin(), unique.end(), [&](const QPointF &existing) {
                const QPointF delta = point - existing;
                return QPointF::dotProduct(delta, delta)
                    <= linearToleranceSquared;
            });
        if (!duplicate) {
            unique.push_back(point);
        }
    };
    for (const QPointF &point : first.polygon) {
        appendUnique(point);
    }
    for (const QPointF &point : second.polygon) {
        appendUnique(point);
    }
    if (unique.size() != 4) {
        return {};
    }

    const QPolygonF hull = convexHull(std::move(unique));
    if (hull.size() != 4) {
        return {};
    }
    const double hullArea = std::abs(signedArea(hull));
    const double triangleArea = std::abs(signedArea(first.polygon))
        + std::abs(signedArea(second.polygon));
    const double areaTolerance = std::max(hullArea * 1e-9,
                                          scale * linearTolerance * 4.0);
    if (hullArea <= areaTolerance
        || std::abs(hullArea - triangleArea) > areaTolerance) {
        return {};
    }

    const QPointF oppositeError = hull[2] - (hull[1] + hull[3] - hull[0]);
    if (QPointF::dotProduct(oppositeError, oppositeError)
        > linearToleranceSquared) {
        return {};
    }
    return hull;
}

int bitCount(const QBitArray &bits)
{
    int result = 0;
    for (qsizetype i = 0; i < bits.size(); ++i) {
        result += bits.testBit(i) ? 1 : 0;
    }
    return result;
}

bool polygonInsideConvex(const QPolygonF &polygon,
                         const QPolygonF &container,
                         double epsilon)
{
    const double direction = signedArea(container) < 0.0 ? -1.0 : 1.0;
    for (const QPointF &point : polygon) {
        for (int edge = 0; edge < container.size(); ++edge) {
            if (direction
                    * cross(container[(edge + 1) % container.size()] - container[edge],
                            point - container[edge])
                < -epsilon) {
                return false;
            }
        }
    }
    return true;
}

bool isConvex(const QPolygonF &polygon, double epsilon)
{
    int direction = 0;
    for (int i = 0; i < polygon.size(); ++i) {
        const double turn = cross(
            polygon[(i + 1) % polygon.size()] - polygon[i],
            polygon[(i + 2) % polygon.size()] - polygon[(i + 1) % polygon.size()]);
        if (std::abs(turn) <= epsilon) {
            continue;
        }
        const int current = turn > 0.0 ? 1 : -1;
        if (direction != 0 && current != direction) {
            return false;
        }
        direction = current;
    }
    return direction != 0;
}

bool pointInsideOrOnContour(const QPointF &point,
                            const QPolygonF &polygon,
                            const QPainterPath &contour,
                            double epsilon)
{
    if (contour.contains(point)) {
        return true;
    }
    for (int edge = 0; edge < polygon.size(); ++edge) {
        if (pointOnSegment(point,
                           polygon[edge],
                           polygon[(edge + 1) % polygon.size()],
                           epsilon)) {
            return true;
        }
    }
    return false;
}

bool polygonInsideContour(const QPolygonF &candidate,
                          const QPolygonF &polygon,
                          const QPainterPath &contour,
                          double epsilon)
{
    QPointF center;
    for (const QPointF &point : candidate) {
        if (!pointInsideOrOnContour(point, polygon, contour, epsilon)) {
            return false;
        }
        center += point;
    }
    if (!pointInsideOrOnContour(center / candidate.size(), polygon, contour, epsilon)) {
        return false;
    }

    for (int candidateEdge = 0; candidateEdge < candidate.size(); ++candidateEdge) {
        const QPointF start = candidate[candidateEdge];
        const QPointF end = candidate[(candidateEdge + 1) % candidate.size()];
        const QPointF direction = end - start;
        const double lengthSquared = QPointF::dotProduct(direction, direction);
        if (lengthSquared <= epsilon * epsilon) {
            return false;
        }
        QVector<double> positions = {0.0, 1.0};
        const auto appendPosition = [&](const QPointF &point) {
            const double position = std::clamp(
                QPointF::dotProduct(point - start, direction) / lengthSquared,
                0.0,
                1.0);
            positions.push_back(position);
        };
        for (int edge = 0; edge < polygon.size(); ++edge) {
            const QPointF &first = polygon[edge];
            const QPointF &second = polygon[(edge + 1) % polygon.size()];
            QPointF intersection;
            if (segmentIntersection(start,
                                    end,
                                    first,
                                    second,
                                    epsilon,
                                    &intersection)) {
                appendPosition(intersection);
            }
            if (pointOnSegment(first, start, end, epsilon)) {
                appendPosition(first);
            }
            if (pointOnSegment(second, start, end, epsilon)) {
                appendPosition(second);
            }
        }
        std::sort(positions.begin(), positions.end());
        const double parameterTolerance = epsilon / std::sqrt(lengthSquared);
        positions.erase(std::unique(positions.begin(),
                                    positions.end(),
                                    [&](double first, double second) {
                                        return std::abs(first - second)
                                            <= parameterTolerance;
                                    }),
                        positions.end());
        for (int interval = 0; interval + 1 < positions.size(); ++interval) {
            if (positions[interval + 1] - positions[interval]
                <= parameterTolerance) {
                continue;
            }
            const double middle = (positions[interval] + positions[interval + 1]) * 0.5;
            if (!pointInsideOrOnContour(start + direction * middle,
                                        polygon,
                                        contour,
                                        epsilon)) {
                return false;
            }
        }
    }
    return true;
}

QVector<MeshShape> triangleShapes(const QPolygonF &polygon,
                                  const QVector<MeshTriangle> &triangles,
                                  bool anchorTriangles = false)
{
    QVector<MeshShape> result;
    result.reserve(triangles.size());
    for (const MeshTriangle &triangle : triangles) {
        const QPolygonF target({polygon[triangle.a],
                                polygon[triangle.b],
                                polygon[triangle.c]});
        result.push_back({103, target, polygonPath(target), anchorTriangles});
    }
    return result;
}

QVector<RectangleCandidate> rectangleCandidates(const QVector<MeshShape> &triangles,
                                                const QPolygonF &contourPolygon,
                                                const QPainterPath &contour,
                                                const std::function<bool()> &cancelled)
{
    QVector<RectangleCandidate> result;
    const int count = triangles.size();
    if (count < 2) {
        return result;
    }

    constexpr qint64 maximumSeedPairs = 4096;
    const qint64 totalPairs = static_cast<qint64>(count) * (count - 1) / 2;
    const qint64 stride = std::max<qint64>(
        1, (totalPairs + maximumSeedPairs - 1) / maximumSeedPairs);
    const double scale = polygonScale(contourPolygon);
    const double quantum = std::max(1e-9, scale * 1e-7);
    const double boundsTolerance = std::max(1e-9, scale * 1e-8);
    const double rectangleTolerance = std::max(1e-12, scale * scale * 1e-10);
    const bool convexContour = isConvex(contourPolygon, rectangleTolerance);
    const double coordinateTolerance = polygonCoordinateEpsilon(contourPolygon);
    const double edgeQuantum = std::max(coordinateTolerance * 8.0,
                                        scale * 1e-12);
    const QPointF selectionCenter = contourPolygon.boundingRect().center();
    const double scaleSquared = scale * scale;
    QSet<QByteArray> seen;

    const auto appendCandidate = [&](const QPolygonF &rectangle) {
        if (rectangle.size() != 4) {
            return;
        }
        const QByteArray key = rectangleKey(rectangle, quantum);
        if (seen.contains(key)) {
            return;
        }
        seen.insert(key);

        const QPainterPath rectanglePath = polygonPath(rectangle);
        const bool contained = convexContour
            ? polygonInsideConvex(rectangle,
                                  contourPolygon,
                                  rectangleTolerance)
            : polygonInsideContour(rectangle,
                                   contourPolygon,
                                   contour,
                                   coordinateTolerance);
        if (!contained) {
            return;
        }
        QBitArray covered(count, false);
        const QRectF rectangleBounds = rectangle.boundingRect().adjusted(
            -boundsTolerance,
            -boundsTolerance,
            boundsTolerance,
            boundsTolerance);
        for (int triangle = 0; triangle < count; ++triangle) {
            if (!rectangleBounds.contains(triangles[triangle].polygon.boundingRect())) {
                continue;
            }
            if (polygonInsideConvex(triangles[triangle].polygon,
                                    rectangle,
                                    rectangleTolerance)) {
                covered.setBit(triangle, true);
            }
        }
        if (bitCount(covered) < 2) {
            return;
        }
        const QPointF center = rectangle.boundingRect().center();
        const QPointF delta = center - selectionCenter;
        result.push_back({
            rectangle,
            rectanglePath,
            std::move(covered),
            pathArea(rectanglePath),
            QPointF::dotProduct(delta, delta) / scaleSquared});
    };

    // Shared edges are exhaustive; unlike the broader bounded search below,
    // an exact two-triangle Rectangle must never be lost to pair sampling.
    QMap<QByteArray, QVector<int>> edgeOwners;
    for (int triangle = 0; triangle < count; ++triangle) {
        if (cancelled && cancelled()) {
            return {};
        }
        const QPolygonF &polygon = triangles[triangle].polygon;
        for (int edge = 0; edge < polygon.size(); ++edge) {
            edgeOwners[edgeKey(polygon[edge],
                               polygon[(edge + 1) % polygon.size()],
                               edgeQuantum)]
                .push_back(triangle);
        }
    }
    for (const QVector<int> &owners : edgeOwners) {
        for (int first = 0; first + 1 < owners.size(); ++first) {
            for (int second = first + 1; second < owners.size(); ++second) {
                if (cancelled && cancelled()) {
                    return {};
                }
                appendCandidate(adjacentTriangleParallelogram(
                    triangles[owners[first]],
                    triangles[owners[second]],
                    scale,
                    coordinateTolerance));
            }
        }
    }

    qint64 ordinal = 0;
    for (int first = 0; first + 1 < count; ++first) {
        for (int second = first + 1; second < count; ++second, ++ordinal) {
            if (cancelled && cancelled()) {
                return {};
            }
            if (ordinal % stride != 0) {
                continue;
            }

            QVector<QPointF> points;
            points.reserve(6);
            for (const QPointF &point : triangles[first].polygon) {
                points.push_back(point);
            }
            for (const QPointF &point : triangles[second].polygon) {
                points.push_back(point);
            }
            const QPolygonF hull = convexHull(std::move(points));
            for (int edge = 0; edge < hull.size(); ++edge) {
                appendCandidate(tightRectangle(
                    hull,
                    hull[(edge + 1) % hull.size()] - hull[edge],
                    quantum));
            }
        }
    }
    return result;
}

QVector<MeshShape> mergeRectangles(const QVector<MeshShape> &triangles,
                                   const QPolygonF &contourPolygon,
                                   const QPainterPath &contour,
                                   const std::function<bool()> &cancelled)
{
    const QVector<RectangleCandidate> candidates = rectangleCandidates(
        triangles,
        contourPolygon,
        contour,
        cancelled);
    if (cancelled && cancelled()) {
        return {};
    }

    QBitArray remaining(triangles.size(), true);
    QVector<MeshShape> rectangles;
    while (true) {
        if (cancelled && cancelled()) {
            return {};
        }
        int best = -1;
        int bestCovered = 1;
        double bestArea = 0.0;
        double bestCenterPenalty = std::numeric_limits<double>::infinity();
        for (int candidate = 0; candidate < candidates.size(); ++candidate) {
            int covered = 0;
            const QBitArray &candidateTriangles = candidates[candidate].triangles;
            for (qsizetype triangle = 0; triangle < remaining.size(); ++triangle) {
                covered += remaining.testBit(triangle)
                    && candidateTriangles.testBit(triangle) ? 1 : 0;
            }
            if (covered > bestCovered
                || (covered == bestCovered
                    && (candidates[candidate].area > bestArea
                        || (candidates[candidate].area == bestArea
                            && candidates[candidate].centerPenalty < bestCenterPenalty)))) {
                best = candidate;
                bestCovered = covered;
                bestArea = candidates[candidate].area;
                bestCenterPenalty = candidates[candidate].centerPenalty;
            }
        }
        if (best < 0) {
            break;
        }
        rectangles.push_back({
            101,
            candidates[best].polygon,
            candidates[best].path,
            false});
        for (qsizetype triangle = 0; triangle < remaining.size(); ++triangle) {
            if (candidates[best].triangles.testBit(triangle)) {
                remaining.setBit(triangle, false);
            }
        }
    }

    QVector<MeshShape> result = std::move(rectangles);
    for (int triangle = 0; triangle < triangles.size(); ++triangle) {
        if (remaining.testBit(triangle)) {
            result.push_back(triangles[triangle]);
        }
    }
    return result;
}

QVector<QPointF> anchorCandidates(const QPolygonF &polygon,
                                  const QPainterPath &contour)
{
    QVector<QPointF> result;
    const double scale = polygonScale(polygon);
    const double duplicateSquared = std::pow(scale * 1e-6, 2.0);
    const auto append = [&](const QPointF &point) {
        if (!contour.contains(point)) {
            return;
        }
        const bool duplicate = std::any_of(
            result.begin(), result.end(), [&](const QPointF &existing) {
                const QPointF delta = point - existing;
                return QPointF::dotProduct(delta, delta) <= duplicateSquared;
            });
        if (!duplicate) {
            result.push_back(point);
        }
    };

    append(polygonCentroid(polygon));
    append(polygon.boundingRect().center());

    QPointF bestGridPoint;
    double bestClearance = 0.0;
    const QRectF bounds = polygon.boundingRect();
    constexpr int gridDivisions = 8;
    for (int y = 1; y < gridDivisions; ++y) {
        for (int x = 1; x < gridDivisions; ++x) {
            const QPointF point(
                bounds.left() + bounds.width() * x / gridDivisions,
                bounds.top() + bounds.height() * y / gridDivisions);
            if (!contour.contains(point)) {
                continue;
            }
            double clearance = std::numeric_limits<double>::infinity();
            for (int edge = 0; edge < polygon.size(); ++edge) {
                clearance = std::min(
                    clearance,
                    distanceSquaredToSegment(
                        point,
                        polygon[edge],
                        polygon[(edge + 1) % polygon.size()]));
            }
            if (clearance > bestClearance) {
                bestClearance = clearance;
                bestGridPoint = point;
            }
        }
    }
    if (bestClearance > std::pow(scale * 1e-5, 2.0)) {
        append(bestGridPoint);
    }
    return result;
}

QVector<MeshShape> splitTriangleAtAnchor(const MeshShape &shape,
                                                const QPointF &anchor,
                                                double epsilon)
{
    if (shape.shapeId != 103
        || shape.anchorTriangle
        || shape.polygon.size() != 3
        || !pointInTriangle(anchor,
                            shape.polygon[0],
                            shape.polygon[1],
                            shape.polygon[2],
                            epsilon)) {
        return {};
    }
    const double duplicateTolerance = std::sqrt(std::max(epsilon, kEpsilon));
    for (const QPointF &vertex : shape.polygon) {
        if (QLineF(anchor, vertex).length() <= duplicateTolerance) {
            return {};
        }
    }

    QVector<MeshShape> result;
    for (int edge = 0; edge < shape.polygon.size(); ++edge) {
        QPolygonF triangle({
            shape.polygon[edge],
            shape.polygon[(edge + 1) % shape.polygon.size()],
            anchor});
        if (std::abs(signedArea(triangle)) <= epsilon) {
            continue;
        }
        if (signedArea(triangle) < 0.0) {
            std::reverse(triangle.begin(), triangle.end());
        }
        result.push_back({103, triangle, polygonPath(triangle), true});
    }
    return result.size() >= 2 ? result : QVector<MeshShape>();
}

QVector<MeshShape> anchorFan(const QPolygonF &polygon,
                             const QPointF &anchor,
                             const QPainterPath &contour,
                             double epsilon)
{
    QVector<MeshShape> result;
    result.reserve(polygon.size());
    for (int edge = 0; edge < polygon.size(); ++edge) {
        QPolygonF triangle({polygon[edge],
                            polygon[(edge + 1) % polygon.size()],
                            anchor});
        if (std::abs(signedArea(triangle)) <= epsilon) {
            return {};
        }
        if (signedArea(triangle) < 0.0) {
            std::reverse(triangle.begin(), triangle.end());
        }
        if (!polygonInsideContour(triangle,
                                  polygon,
                                  contour,
                                  polygonCoordinateEpsilon(polygon))) {
            return {};
        }
        result.push_back({103, triangle, polygonPath(triangle), true});
    }
    return result;
}

int shapeCount(const QVector<MeshShape> &mesh, int shapeId)
{
    return static_cast<int>(std::count_if(
        mesh.begin(), mesh.end(), [&](const MeshShape &shape) {
            return shape.shapeId == shapeId;
        }));
}

int anchorTriangleCount(const QVector<MeshShape> &mesh)
{
    return static_cast<int>(std::count_if(
        mesh.begin(), mesh.end(), [](const MeshShape &shape) {
            return shape.anchorTriangle;
        }));
}

bool preferMesh(const QVector<MeshShape> &candidate,
                const QVector<MeshShape> &current)
{
    if (candidate.size() != current.size()) {
        return candidate.size() < current.size();
    }
    const int candidateRectangles = shapeCount(candidate, 101);
    const int currentRectangles = shapeCount(current, 101);
    if (candidateRectangles != currentRectangles) {
        return candidateRectangles > currentRectangles;
    }
    return anchorTriangleCount(candidate) > anchorTriangleCount(current);
}

QVector<MeshShape> lassoMesh(const QPolygonF &polygon,
                             const QPainterPath &contour,
                             double epsilon,
                             const std::function<bool()> &cancelled,
                             QString *error)
{
    const QVector<MeshTriangle> triangulation = triangulate(
        polygon, epsilon, cancelled, error);
    if (triangulation.isEmpty() || (cancelled && cancelled())) {
        return {};
    }

    const QVector<MeshShape> baselineTriangles = triangleShapes(
        polygon, triangulation);
    QVector<MeshShape> result = mergeRectangles(
        baselineTriangles,
        polygon,
        contour,
        cancelled);
    if (result.isEmpty() || (cancelled && cancelled())) {
        return {};
    }

    const QVector<QPointF> anchors = anchorCandidates(polygon, contour);
    const int initialTriangleCount = baselineTriangles.size();
    for (const QPointF &anchor : anchors) {
        QVector<MeshShape> fan = anchorFan(polygon, anchor, contour, epsilon);
        if (fan.isEmpty()) {
            continue;
        }
        QVector<MeshShape> candidate = mergeRectangles(
            fan,
            polygon,
            contour,
            cancelled);
        if (candidate.isEmpty()
            || candidate.size() > initialTriangleCount
            || (cancelled && cancelled())) {
            continue;
        }
        if (preferMesh(candidate, result)) {
            result = std::move(candidate);
        }
    }

    int remainingBudget = baselineTriangles.size() - result.size();
    if (remainingBudget <= 0) {
        return result;
    }
    for (const QPointF &anchor : anchors) {
        if (cancelled && cancelled()) {
            return {};
        }
        for (int shape = 0; shape < result.size(); ++shape) {
            QVector<MeshShape> split = splitTriangleAtAnchor(
                result[shape], anchor, epsilon);
            const int cost = split.size() - 1;
            if (split.isEmpty() || cost > remainingBudget) {
                continue;
            }
            result.removeAt(shape);
            for (MeshShape &triangle : split) {
                result.push_back(std::move(triangle));
            }
            remainingBudget -= cost;
            break;
        }
        if (remainingBudget <= 0) {
            break;
        }
    }
    return result;
}

} // namespace

PolygonContour buildPolygonContour(const QVector<QPointF> &points, double tolerance)
{
    PolygonContour result;
    tolerance = std::max(tolerance, kEpsilon);
    for (const QPointF &point : points) {
        if (result.polygon.isEmpty()
            || QLineF(result.polygon.back(), point).length() > tolerance) {
            result.polygon.push_back(point);
        }
    }
    if (result.polygon.size() > 1
        && QLineF(result.polygon.front(), result.polygon.back()).length() <= tolerance) {
        result.polygon.removeLast();
    }

    bool changed = true;
    while (changed && result.polygon.size() >= 3) {
        changed = false;
        for (int i = 0; i < result.polygon.size(); ++i) {
            const QPointF &previous = result.polygon[(i + result.polygon.size() - 1) % result.polygon.size()];
            const QPointF &current = result.polygon[i];
            const QPointF &next = result.polygon[(i + 1) % result.polygon.size()];
            if (distanceSquaredToSegment(current, previous, next) <= tolerance * tolerance) {
                result.polygon.removeAt(i);
                changed = true;
                break;
            }
        }
    }
    if (result.polygon.size() < 3) {
        result.error = QStringLiteral("A polygonal lasso needs at least three distinct vertices");
        return result;
    }

    const double scale = polygonScale(result.polygon);
    const double coordinateEpsilon = polygonCoordinateEpsilon(result.polygon);
    result.crossings = polygonCrossings(result.polygon);
    if (!result.crossings.isEmpty()) {
        result.error = QStringLiteral("The polygonal lasso crosses itself");
        return result;
    }
    if (std::abs(signedArea(result.polygon)) <= coordinateEpsilon * scale) {
        result.error = QStringLiteral("The polygonal lasso has no fillable area");
        return result;
    }
    result.path = polygonPath(result.polygon);
    return result;
}

PolygonMeshSources buildPolygonMeshSources(const ShapeGeometryStore &geometry)
{
    return {geometryHull(geometry.shape(101)),
            geometryHull(geometry.shape(103))};
}

PolygonMeshResult meshPolygon(const PolygonMeshRequest &request,
                              const std::function<bool()> &cancelled)
{
    PolygonMeshResult result;
    if (cancelled && cancelled()) {
        result.cancelled = true;
        return result;
    }
    if (request.sources.triangle.size() != 3
        || (request.mergeSquares && request.sources.square.size() != 4)) {
        result.error = request.mergeSquares
            ? QStringLiteral("Rectangle or Triangle geometry is unavailable")
            : QStringLiteral("Triangle geometry is unavailable");
        return result;
    }

    const PolygonContour contour = buildPolygonContour(request.points);
    if (!contour.valid()) {
        result.error = contour.error.isEmpty()
            ? QStringLiteral("Invalid polygonal lasso")
            : contour.error;
        return result;
    }
    result.contour = contour.path;
    const double scale = polygonScale(contour.polygon);
    const double epsilon = kEpsilon * scale * scale;
    const double allowedArea = std::max(1e-8, pathArea(contour.path) * 1e-8);

    QString triangulationError;
    QVector<MeshShape> shapes;
    if (request.mergeSquares) {
        shapes = lassoMesh(contour.polygon,
                           contour.path,
                           epsilon,
                           cancelled,
                           &triangulationError);
    } else {
        const QVector<MeshTriangle> triangles = triangulate(
            contour.polygon,
            epsilon,
            cancelled,
            &triangulationError);
        if (!triangles.isEmpty()) {
            shapes = triangleShapes(contour.polygon, triangles);
        }
    }
    if (cancelled && cancelled()) {
        result.cancelled = true;
        return result;
    }
    if (shapes.isEmpty()) {
        result.error = triangulationError.isEmpty()
            ? QStringLiteral("Could not mesh the lasso polygon")
            : triangulationError;
        return result;
    }

    QPainterPath mesh;
    mesh.setFillRule(Qt::WindingFill);
    for (const MeshShape &shape : shapes) {
        if (cancelled && cancelled()) {
            result.placements.clear();
            result.cancelled = true;
            return result;
        }

        bool ok = false;
        QTransform transform;
        if (shape.shapeId == 101 && shape.polygon.size() == 4) {
            transform = affineFromTriangles(request.sources.square[0],
                                            request.sources.square[1],
                                            request.sources.square[3],
                                            shape.polygon[0],
                                            shape.polygon[1],
                                            shape.polygon[3],
                                            &ok);
            if (!ok
                || QLineF(transform.map(request.sources.square[2]),
                          shape.polygon[2]).length() > scale * 1e-7) {
                result.error = QStringLiteral("Could not map a Rectangle into the lasso mesh");
                result.placements.clear();
                return result;
            }
        } else if (shape.shapeId == 103 && shape.polygon.size() == 3) {
            transform = affineFromTriangles(request.sources.triangle[0],
                                            request.sources.triangle[1],
                                            request.sources.triangle[2],
                                            shape.polygon[0],
                                            shape.polygon[1],
                                            shape.polygon[2],
                                            &ok);
            if (!ok) {
                result.error = QStringLiteral("Could not map a Triangle into the lasso mesh");
                result.placements.clear();
                return result;
            }
        } else {
            result.error = QStringLiteral("The lasso mesh produced an unsupported shape");
            result.placements.clear();
            return result;
        }
        result.placements.push_back({shape.shapeId, transform});
        if (!polygonInsideContour(shape.polygon,
                                  contour.polygon,
                                  contour.path,
                                  polygonCoordinateEpsilon(contour.polygon))) {
            result.error = QStringLiteral("The lasso mesh crossed its contour");
            result.placements.clear();
            return result;
        }
        mesh.addPath(shape.path);
    }

    if (pathArea(contour.path.subtracted(mesh)) > allowedArea) {
        result.error = QStringLiteral("The generated lasso mesh did not match its contour");
        result.placements.clear();
    }
    return result;
}

} // namespace gui
