#include "polygon_mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr double kEpsilon = 1e-9;
constexpr int kInfinitePartitionCost = std::numeric_limits<int>::max() / 4;
constexpr int kOverlapOptimizationBudgetMs = 150;

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

quint64 edgeKey(int a, int b)
{
    const quint32 low = static_cast<quint32>(std::min(a, b));
    const quint32 high = static_cast<quint32>(std::max(a, b));
    return (static_cast<quint64>(low) << 32) | high;
}

bool isParallelogram(const QPolygonF &quad, double epsilon)
{
    if (quad.size() != 4) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        if (cross(quad[(i + 1) % 4] - quad[i],
                  quad[(i + 2) % 4] - quad[(i + 1) % 4]) <= epsilon) {
            return false;
        }
    }
    return QLineF(quad[0] + quad[2], quad[1] + quad[3]).length()
        <= polygonScale(quad) * 1e-7;
}

QPainterPath polygonPath(const QPolygonF &polygon);

struct QuantizedPoint {
    qint64 x = 0;
    qint64 y = 0;

    bool operator==(const QuantizedPoint &other) const
    {
        return x == other.x && y == other.y;
    }
};

size_t qHash(const QuantizedPoint &point, size_t seed = 0)
{
    return qHashMulti(seed, point.x, point.y);
}

enum class PartitionCellKind {
    None,
    Triangle,
    Square,
};

struct PartitionChoice {
    int cost = kInfinitePartitionCost;
    int squareCount = -1;
    double balancePenalty = std::numeric_limits<double>::infinity();
    double centerPenalty = std::numeric_limits<double>::infinity();
    PartitionCellKind kind = PartitionCellKind::None;
    int first = -1;
    int second = -1;
};

struct PartitionCell {
    PartitionCellKind kind = PartitionCellKind::None;
    int a = -1;
    int b = -1;
    int c = -1;
    int d = -1;
};

struct QuadPartitionCandidate {
    int first = -1;
    int second = -1;
};

struct MinimumPartition {
    QPolygonF polygon;
    QVector<PartitionCell> cells;
    bool cancelled = false;
    QString error;
};

bool boundaryEdge(int a, int b, int count)
{
    return b == a + 1 || (a == 0 && b == count - 1);
}

bool diagonalInsidePolygon(int a,
                           int b,
                           const QPolygonF &polygon,
                           const QPainterPath &path,
                           double coordinateEpsilon)
{
    const int count = polygon.size();
    if (boundaryEdge(a, b, count)) {
        return true;
    }
    for (int edge = 0; edge < count; ++edge) {
        const int next = (edge + 1) % count;
        if (edge == a || next == a || edge == b || next == b) {
            continue;
        }
        if (segmentIntersection(polygon[a],
                                polygon[b],
                                polygon[edge],
                                polygon[next],
                                coordinateEpsilon,
                                nullptr)) {
            return false;
        }
    }
    return path.contains((polygon[a] + polygon[b]) * 0.5);
}

bool triangleCellValid(int a,
                       int b,
                       int c,
                       const QPolygonF &polygon,
                       const QPainterPath &path,
                       const QVector<QVector<bool>> &validEdges,
                       double epsilon)
{
    if (!validEdges[a][b] || !validEdges[b][c] || !validEdges[a][c]
        || cross(polygon[b] - polygon[a], polygon[c] - polygon[b]) <= epsilon) {
        return false;
    }
    return path.contains((polygon[a] + polygon[b] + polygon[c]) / 3.0);
}

bool quadCellValid(int a,
                   int b,
                   int c,
                   int d,
                   const QPolygonF &polygon,
                   const QPainterPath &path,
                   const QVector<QVector<bool>> &validEdges,
                   double epsilon)
{
    if (!validEdges[a][b] || !validEdges[b][c]
        || !validEdges[c][d] || !validEdges[a][d]) {
        return false;
    }
    const QPolygonF quad({polygon[a], polygon[b], polygon[c], polygon[d]});
    return isParallelogram(quad, epsilon)
        && path.contains((polygon[a] + polygon[b] + polygon[c] + polygon[d]) / 4.0);
}

bool betterPartitionChoice(const PartitionChoice &candidate,
                           const PartitionChoice &current)
{
    if (candidate.cost != current.cost) {
        return candidate.cost < current.cost;
    }
    if (candidate.squareCount != current.squareCount) {
        return candidate.squareCount > current.squareCount;
    }
    if (candidate.centerPenalty != current.centerPenalty) {
        return candidate.centerPenalty < current.centerPenalty;
    }
    if (candidate.balancePenalty != current.balancePenalty) {
        return candidate.balancePenalty < current.balancePenalty;
    }
    const int candidateRank = candidate.kind == PartitionCellKind::Square ? 0 : 1;
    const int currentRank = current.kind == PartitionCellKind::Square ? 0 : 1;
    if (candidateRank != currentRank) {
        return candidateRank < currentRank;
    }
    return qMakePair(candidate.first, candidate.second)
        < qMakePair(current.first, current.second);
}

MinimumPartition minimumShapePartition(const QPolygonF &input,
                                       double epsilon,
                                       const std::function<bool()> &cancelled)
{
    MinimumPartition result;
    result.polygon = input;
    if (signedArea(result.polygon) < 0.0) {
        std::reverse(result.polygon.begin(), result.polygon.end());
    }
    const int count = result.polygon.size();
    const QPainterPath path = polygonPath(result.polygon);
    const double coordinateEpsilon = polygonCoordinateEpsilon(result.polygon);
    const QPointF selectionCenter = result.polygon.boundingRect().center();
    const double scaleSquared = std::pow(polygonScale(result.polygon), 2.0);

    QVector<QVector<bool>> validEdges(count, QVector<bool>(count, false));
    for (int a = 0; a < count; ++a) {
        for (int b = a + 1; b < count; ++b) {
            if (cancelled && cancelled()) {
                result.cancelled = true;
                return result;
            }
            validEdges[a][b] = validEdges[b][a] = diagonalInsidePolygon(a,
                                                                        b,
                                                                        result.polygon,
                                                                        path,
                                                                        coordinateEpsilon);
        }
    }

    const double vertexTolerance = std::max(1e-9, polygonScale(result.polygon) * 1e-7);
    const QPointF bucketOrigin = result.polygon.front();
    QHash<QuantizedPoint, QVector<int>> verticesByBucket;
    const auto bucketFor = [&](const QPointF &point) {
        return QuantizedPoint{
            static_cast<qint64>(std::floor((point.x() - bucketOrigin.x()) / vertexTolerance)),
            static_cast<qint64>(std::floor((point.y() - bucketOrigin.y()) / vertexTolerance))};
    };
    for (int index = 0; index < count; ++index) {
        verticesByBucket[bucketFor(result.polygon[index])].push_back(index);
    }

    QHash<quint64, QVector<QuadPartitionCandidate>> quadsByOuterEdge;
    for (int a = 0; a + 3 < count; ++a) {
        for (int b = a + 1; b + 2 < count; ++b) {
            for (int c = b + 1; c + 1 < count; ++c) {
                if (cancelled && cancelled()) {
                    result.cancelled = true;
                    return result;
                }
                const QPointF required = result.polygon[a] + result.polygon[c] - result.polygon[b];
                const QuantizedPoint bucket = bucketFor(required);
                for (qint64 dx = -1; dx <= 1; ++dx) {
                    for (qint64 dy = -1; dy <= 1; ++dy) {
                        const auto matches = verticesByBucket.constFind({bucket.x + dx,
                                                                         bucket.y + dy});
                        if (matches == verticesByBucket.constEnd()) {
                            continue;
                        }
                        for (int d : matches.value()) {
                            if (d <= c || QLineF(required, result.polygon[d]).length() > vertexTolerance) {
                                continue;
                            }
                            if (quadCellValid(a,
                                              b,
                                              c,
                                              d,
                                              result.polygon,
                                              path,
                                              validEdges,
                                              epsilon)) {
                                quadsByOuterEdge[edgeKey(a, d)].push_back({b, c});
                            }
                        }
                    }
                }
            }
        }
    }
    for (auto it = quadsByOuterEdge.begin(); it != quadsByOuterEdge.end(); ++it) {
        std::sort(it.value().begin(), it.value().end(), [](const auto &left, const auto &right) {
            return qMakePair(left.first, left.second) < qMakePair(right.first, right.second);
        });
        it.value().erase(std::unique(it.value().begin(),
                                     it.value().end(),
                                     [](const auto &left, const auto &right) {
                                         return left.first == right.first && left.second == right.second;
                                     }),
                         it.value().end());
    }

    QVector<QVector<PartitionChoice>> choices(count, QVector<PartitionChoice>(count));
    for (int index = 0; index + 1 < count; ++index) {
        choices[index][index + 1].cost = 0;
        choices[index][index + 1].squareCount = 0;
        choices[index][index + 1].balancePenalty = 0.0;
        choices[index][index + 1].centerPenalty = 0.0;
    }
    for (int span = 2; span < count; ++span) {
        for (int a = 0; a + span < count; ++a) {
            if (cancelled && cancelled()) {
                result.cancelled = true;
                return result;
            }
            const int d = a + span;
            if (!validEdges[a][d]) {
                continue;
            }
            PartitionChoice best;
            for (int b = a + 1; b < d; ++b) {
                if (choices[a][b].cost >= kInfinitePartitionCost
                    || choices[b][d].cost >= kInfinitePartitionCost) {
                    continue;
                }
                if (!triangleCellValid(a,
                                       b,
                                       d,
                                       result.polygon,
                                       path,
                                       validEdges,
                                       epsilon)) {
                    continue;
                }
                PartitionChoice candidate;
                candidate.cost = choices[a][b].cost + choices[b][d].cost + 1;
                candidate.squareCount = choices[a][b].squareCount
                    + choices[b][d].squareCount;
                const double leftSpan = b - a;
                const double rightSpan = d - b;
                candidate.balancePenalty = choices[a][b].balancePenalty
                    + choices[b][d].balancePenalty
                    + std::abs(leftSpan - rightSpan) / (d - a);
                const QPointF triangleCenter = (result.polygon[a]
                                                + result.polygon[b]
                                                + result.polygon[d]) / 3.0;
                const QPointF triangleDelta = triangleCenter - selectionCenter;
                candidate.centerPenalty = choices[a][b].centerPenalty
                    + choices[b][d].centerPenalty
                    + QPointF::dotProduct(triangleDelta, triangleDelta) / scaleSquared;
                candidate.kind = PartitionCellKind::Triangle;
                candidate.first = b;
                if (betterPartitionChoice(candidate, best)) {
                    best = candidate;
                }
            }
            const QVector<QuadPartitionCandidate> quads = quadsByOuterEdge.value(edgeKey(a, d));
            for (const QuadPartitionCandidate &quad : quads) {
                if (choices[a][quad.first].cost >= kInfinitePartitionCost
                    || choices[quad.first][quad.second].cost >= kInfinitePartitionCost
                    || choices[quad.second][d].cost >= kInfinitePartitionCost) {
                    continue;
                }
                PartitionChoice candidate;
                candidate.cost = choices[a][quad.first].cost
                    + choices[quad.first][quad.second].cost
                    + choices[quad.second][d].cost + 1;
                candidate.squareCount = choices[a][quad.first].squareCount
                    + choices[quad.first][quad.second].squareCount
                    + choices[quad.second][d].squareCount + 1;
                const double firstSpan = quad.first - a;
                const double secondSpan = quad.second - quad.first;
                const double thirdSpan = d - quad.second;
                const double averageSpan = (d - a) / 3.0;
                const double spanScale = static_cast<double>((d - a) * (d - a));
                candidate.balancePenalty = choices[a][quad.first].balancePenalty
                    + choices[quad.first][quad.second].balancePenalty
                    + choices[quad.second][d].balancePenalty
                    + (std::pow(firstSpan - averageSpan, 2.0)
                       + std::pow(secondSpan - averageSpan, 2.0)
                       + std::pow(thirdSpan - averageSpan, 2.0)) / spanScale;
                const QPointF squareCenter = (result.polygon[a]
                                              + result.polygon[quad.first]
                                              + result.polygon[quad.second]
                                              + result.polygon[d]) / 4.0;
                const QPointF squareDelta = squareCenter - selectionCenter;
                candidate.centerPenalty = choices[a][quad.first].centerPenalty
                    + choices[quad.first][quad.second].centerPenalty
                    + choices[quad.second][d].centerPenalty
                    + QPointF::dotProduct(squareDelta, squareDelta) / scaleSquared;
                candidate.kind = PartitionCellKind::Square;
                candidate.first = quad.first;
                candidate.second = quad.second;
                if (betterPartitionChoice(candidate, best)) {
                    best = candidate;
                }
            }
            choices[a][d] = best;
        }
    }

    if (choices[0][count - 1].kind == PartitionCellKind::None) {
        result.error = QStringLiteral("Could not find an exact Triangle/Square partition");
        return result;
    }
    QVector<QPair<int, int>> intervals = {{0, count - 1}};
    while (!intervals.isEmpty()) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.cells.clear();
            return result;
        }
        const auto interval = intervals.takeLast();
        const int a = interval.first;
        const int d = interval.second;
        if (d == a + 1) {
            continue;
        }
        const PartitionChoice &choice = choices[a][d];
        if (choice.kind == PartitionCellKind::Triangle) {
            result.cells.push_back({choice.kind, a, choice.first, d, -1});
            intervals.push_back({choice.first, d});
            intervals.push_back({a, choice.first});
        } else if (choice.kind == PartitionCellKind::Square) {
            result.cells.push_back({choice.kind, a, choice.first, choice.second, d});
            intervals.push_back({choice.second, d});
            intervals.push_back({choice.first, choice.second});
            intervals.push_back({a, choice.first});
        } else {
            result.error = QStringLiteral("Could not reconstruct the Triangle/Square partition");
            result.cells.clear();
            return result;
        }
    }
    return result;
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

struct CoverCandidate {
    int shapeId = 0;
    QVector<int> vertices;
    QPolygonF polygon;
    QPainterPath path;
    QBitArray atoms;
    double area = 0.0;
    double centerPenalty = 0.0;
};

bool usesInteriorAnchor(const CoverCandidate &candidate)
{
    return std::any_of(candidate.vertices.begin(),
                       candidate.vertices.end(),
                       [](int vertex) { return vertex < 0; });
}

struct CoverageAtom {
    QPainterPath path;
    QBitArray signature;
    double area = 0.0;
};

struct CoverObjective {
    int count = std::numeric_limits<int>::max();
    int maximumVertexUse = std::numeric_limits<int>::max();
    double centerPenalty = std::numeric_limits<double>::infinity();
    double overlapPenalty = std::numeric_limits<double>::infinity();
    int squareCount = -1;
    int interiorAnchorCount = -1;
    QVector<int> candidates;
};

struct MinimumCover {
    QPolygonF polygon;
    QVector<CoverCandidate> candidates;
    QVector<int> selected;
    bool cancelled = false;
    QString error;
};

QPointF polygonVertexCenter(const QPolygonF &polygon)
{
    QPointF result;
    for (const QPointF &point : polygon) {
        result += point;
    }
    return polygon.isEmpty() ? result : result / polygon.size();
}

MinimumCover coverFromPartition(const MinimumPartition &partition)
{
    MinimumCover result;
    result.polygon = partition.polygon;
    const QPointF selectionCenter = result.polygon.boundingRect().center();
    const double scaleSquared = std::pow(polygonScale(result.polygon), 2.0);
    result.candidates.reserve(partition.cells.size());
    result.selected.reserve(partition.cells.size());
    for (const PartitionCell &cell : partition.cells) {
        CoverCandidate candidate;
        candidate.shapeId = cell.kind == PartitionCellKind::Square ? 101 : 103;
        candidate.vertices = cell.kind == PartitionCellKind::Square
            ? QVector<int>({cell.a, cell.b, cell.c, cell.d})
            : QVector<int>({cell.a, cell.b, cell.c});
        for (int vertex : candidate.vertices) {
            candidate.polygon.push_back(result.polygon[vertex]);
        }
        candidate.path = polygonPath(candidate.polygon);
        candidate.area = std::abs(signedArea(candidate.polygon));
        const QPointF delta = polygonVertexCenter(candidate.polygon) - selectionCenter;
        candidate.centerPenalty = QPointF::dotProduct(delta, delta) / scaleSquared;
        result.selected.push_back(result.candidates.size());
        result.candidates.push_back(std::move(candidate));
    }
    return result;
}

QPointF pathCentroid(const QPainterPath &path)
{
    QPointF weightedCenter;
    double totalArea = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        if (polygon.size() < 3) {
            continue;
        }
        double twiceArea = 0.0;
        QPointF numerator;
        for (int i = 0; i < polygon.size(); ++i) {
            const QPointF &a = polygon[i];
            const QPointF &b = polygon[(i + 1) % polygon.size()];
            const double edgeCross = cross(a, b);
            twiceArea += edgeCross;
            numerator += (a + b) * edgeCross;
        }
        if (std::abs(twiceArea) <= kEpsilon) {
            continue;
        }
        const QPointF center = numerator / (3.0 * twiceArea);
        const double area = std::abs(twiceArea) * 0.5;
        weightedCenter += center * area;
        totalArea += area;
    }
    return totalArea > 0.0 ? weightedCenter / totalArea : path.boundingRect().center();
}

QVector<QPointF> interiorCenterPoints(const MinimumPartition &partition,
                                      const QPainterPath &contour)
{
    const QPolygonF &polygon = partition.polygon;
    const double scale = polygonScale(polygon);
    const double duplicateDistanceSquared = std::pow(scale * 1e-6, 2.0);
    QVector<QPointF> samples;
    const auto appendSample = [&](const QPointF &point) {
        if (!contour.contains(point)) {
            return;
        }
        const bool duplicate = std::any_of(samples.begin(), samples.end(), [&](const QPointF &existing) {
            return QPointF::dotProduct(point - existing, point - existing)
                <= duplicateDistanceSquared;
        });
        if (!duplicate) {
            samples.push_back(point);
        }
    };
    appendSample(pathCentroid(contour));
    for (const PartitionCell &cell : partition.cells) {
        QPointF center = polygon[cell.a] + polygon[cell.b] + polygon[cell.c];
        int divisor = 3;
        if (cell.kind == PartitionCellKind::Square) {
            center += polygon[cell.d];
            divisor = 4;
        }
        appendSample(center / divisor);
    }
    const QRectF bounds = polygon.boundingRect();
    constexpr int gridDivisions = 8;
    for (int y = 1; y < gridDivisions; ++y) {
        for (int x = 1; x < gridDivisions; ++x) {
            appendSample(QPointF(bounds.left() + bounds.width() * x / gridDivisions,
                                 bounds.top() + bounds.height() * y / gridDivisions));
        }
    }

    const auto clearanceSquared = [&](const QPointF &point) {
        double result = std::numeric_limits<double>::infinity();
        for (int edge = 0; edge < polygon.size(); ++edge) {
            result = std::min(result,
                              distanceSquaredToSegment(point,
                                                       polygon[edge],
                                                       polygon[(edge + 1) % polygon.size()]));
        }
        return result;
    };
    QVector<QPointF> centers;
    constexpr int maximumCenters = 3;
    const double separationSquared = std::pow(scale * 0.2, 2.0);
    while (centers.size() < maximumCenters) {
        int best = -1;
        double bestScore = 0.0;
        for (int sample = 0; sample < samples.size(); ++sample) {
            const QPointF &point = samples[sample];
            const double clearance = clearanceSquared(point);
            double separation = std::numeric_limits<double>::infinity();
            for (const QPointF &center : centers) {
                separation = std::min(separation,
                                      QPointF::dotProduct(point - center, point - center));
            }
            if (!centers.isEmpty() && separation < separationSquared) {
                continue;
            }
            const double score = centers.isEmpty()
                ? clearance
                : std::min(clearance, separation * 0.25);
            if (score > bestScore) {
                best = sample;
                bestScore = score;
            }
        }
        if (best < 0 || bestScore <= std::pow(scale * 1e-5, 2.0)) {
            break;
        }
        centers.push_back(samples.takeAt(best));
    }
    return centers;
}

bool candidateInsideContour(const QPainterPath &candidate,
                            const QPainterPath &contour,
                            double containmentTolerance)
{
    return pathArea(candidate.subtracted(contour)) <= containmentTolerance;
}

MinimumCover centerBiasedCover(MinimumCover baseline,
                               const QVector<QPointF> &centerPoints,
                               const QPainterPath &contour,
                               double containmentTolerance)
{
    QVector<int> vertexUse(baseline.polygon.size(), 0);
    for (const CoverCandidate &candidate : baseline.candidates) {
        for (int vertex : candidate.vertices) {
            if (vertex >= 0) {
                ++vertexUse[vertex];
            }
        }
    }
    const QPointF selectionCenter = baseline.polygon.boundingRect().center();
    const double scaleSquared = std::pow(polygonScale(baseline.polygon), 2.0);
    for (int index = 0; index < baseline.candidates.size(); ++index) {
        const CoverCandidate &original = baseline.candidates[index];
        if (original.shapeId != 103 || original.vertices.size() != 3) {
            continue;
        }
        CoverCandidate best;
        bool haveBest = false;
        int bestMaximumUse = *std::max_element(vertexUse.begin(), vertexUse.end());
        double bestCenterPenalty = original.centerPenalty;
        for (int centerIndex = 0; centerIndex < centerPoints.size(); ++centerIndex) {
            for (int omitted = 0; omitted < 3; ++omitted) {
                const int first = original.vertices[(omitted + 1) % 3];
                const int second = original.vertices[(omitted + 2) % 3];
                const int low = std::min(first, second);
                const int high = std::max(first, second);
                const QPolygonF polygon({baseline.polygon[low],
                                         baseline.polygon[high],
                                         centerPoints[centerIndex]});
                if (std::abs(signedArea(polygon)) <= kEpsilon) {
                    continue;
                }
                const QPainterPath path = polygonPath(polygon);
                if (!candidateInsideContour(path, contour, containmentTolerance)
                    || pathArea(original.path.subtracted(path)) > containmentTolerance) {
                    continue;
                }
                QVector<int> nextUse = vertexUse;
                for (int vertex : original.vertices) {
                    --nextUse[vertex];
                }
                ++nextUse[low];
                ++nextUse[high];
                const int maximumUse = *std::max_element(nextUse.begin(), nextUse.end());
                const QPointF delta = polygonVertexCenter(polygon) - selectionCenter;
                const double centerPenalty = QPointF::dotProduct(delta, delta) / scaleSquared;
                if (centerPenalty > bestCenterPenalty
                    || (centerPenalty == bestCenterPenalty
                        && maximumUse >= bestMaximumUse)) {
                    continue;
                }
                best.shapeId = 103;
                best.vertices = {low, high, -centerIndex - 1};
                best.polygon = polygon;
                best.path = path;
                best.area = std::abs(signedArea(polygon));
                best.centerPenalty = centerPenalty;
                bestMaximumUse = maximumUse;
                bestCenterPenalty = centerPenalty;
                haveBest = true;
            }
        }
        if (!haveBest) {
            continue;
        }
        for (int vertex : original.vertices) {
            --vertexUse[vertex];
        }
        for (int vertex : best.vertices) {
            if (vertex >= 0) {
                ++vertexUse[vertex];
            }
        }
        baseline.candidates[index] = std::move(best);
    }
    return baseline;
}

bool candidateLess(const CoverCandidate &left, const CoverCandidate &right)
{
    if (left.shapeId != right.shapeId) {
        return left.shapeId < right.shapeId;
    }
    return std::lexicographical_compare(left.vertices.begin(),
                                        left.vertices.end(),
                                        right.vertices.begin(),
                                        right.vertices.end());
}

QVector<CoverCandidate> coverCandidates(const QPolygonF &polygon,
                                        const QPainterPath &contour,
                                        const QVector<QPointF> &centerPoints,
                                        double epsilon,
                                        double containmentTolerance,
                                        const std::function<bool()> &cancelled,
                                        bool *wasCancelled)
{
    // Keep generated-center candidates sparse: triangles use a contour edge,
    // while Squares are found by a hashed parallelogram lookup. Considering
    // every center/vertex tuple multiplies path atoms and destroys the
    // interactive time bound.
    QVector<CoverCandidate> result;
    const int count = polygon.size();
    const QPointF selectionCenter = polygon.boundingRect().center();
    const double scaleSquared = std::pow(polygonScale(polygon), 2.0);
    const auto appendCandidate = [&](int shapeId,
                                     const QVector<int> &vertices,
                                     const QPolygonF &candidatePolygon) {
        const double area = std::abs(signedArea(candidatePolygon));
        if (area <= epsilon) {
            return;
        }
        const QPainterPath candidatePath = polygonPath(candidatePolygon);
        if (!candidateInsideContour(candidatePath, contour, containmentTolerance)) {
            return;
        }
        const QPointF center = polygonVertexCenter(candidatePolygon);
        const QPointF centerDelta = center - selectionCenter;
        CoverCandidate candidate;
        candidate.shapeId = shapeId;
        candidate.vertices = vertices;
        candidate.polygon = candidatePolygon;
        candidate.path = candidatePath;
        candidate.area = area;
        candidate.centerPenalty = QPointF::dotProduct(centerDelta, centerDelta) / scaleSquared;
        result.push_back(std::move(candidate));
    };

    for (int a = 0; a + 2 < count; ++a) {
        for (int b = a + 1; b + 1 < count; ++b) {
            for (int c = b + 1; c < count; ++c) {
                if (cancelled && cancelled()) {
                    *wasCancelled = true;
                    return {};
                }
                appendCandidate(103,
                                {a, b, c},
                                QPolygonF({polygon[a], polygon[b], polygon[c]}));
            }
        }
    }

    const double vertexTolerance = std::max(1e-9, polygonScale(polygon) * 1e-7);
    const QPointF bucketOrigin = polygon.front();
    const auto bucketFor = [&](const QPointF &point) {
        return QuantizedPoint{
            static_cast<qint64>(std::floor((point.x() - bucketOrigin.x()) / vertexTolerance)),
            static_cast<qint64>(std::floor((point.y() - bucketOrigin.y()) / vertexTolerance))};
    };
    QHash<QuantizedPoint, QVector<int>> verticesByBucket;
    for (int index = 0; index < count; ++index) {
        verticesByBucket[bucketFor(polygon[index])].push_back(index);
    }
    for (int a = 0; a + 3 < count; ++a) {
        for (int b = a + 1; b + 2 < count; ++b) {
            for (int c = b + 1; c + 1 < count; ++c) {
                if (cancelled && cancelled()) {
                    *wasCancelled = true;
                    return {};
                }
                const QPointF required = polygon[a] + polygon[c] - polygon[b];
                const QuantizedPoint bucket = bucketFor(required);
                for (qint64 dx = -1; dx <= 1; ++dx) {
                    for (qint64 dy = -1; dy <= 1; ++dy) {
                        const auto matches = verticesByBucket.constFind({bucket.x + dx,
                                                                         bucket.y + dy});
                        if (matches == verticesByBucket.constEnd()) {
                            continue;
                        }
                        for (int d : matches.value()) {
                            if (d <= c || QLineF(required, polygon[d]).length() > vertexTolerance) {
                                continue;
                            }
                            const QPolygonF quad({polygon[a], polygon[b], polygon[c], polygon[d]});
                            if (isParallelogram(quad, epsilon)) {
                                appendCandidate(101, {a, b, c, d}, quad);
                            }
                        }
                    }
                }
            }
        }
    }
    for (int centerIndex = 0; centerIndex < centerPoints.size(); ++centerIndex) {
        const QPointF &center = centerPoints[centerIndex];
        for (int a = 0; a + 1 < count; ++a) {
            for (int b = a + 1; b < count; ++b) {
                if (cancelled && cancelled()) {
                    *wasCancelled = true;
                    return {};
                }
                const QPointF required = polygon[a] + polygon[b] - center;
                const QuantizedPoint bucket = bucketFor(required);
                for (qint64 dx = -1; dx <= 1; ++dx) {
                    for (qint64 dy = -1; dy <= 1; ++dy) {
                        const auto matches = verticesByBucket.constFind({bucket.x + dx,
                                                                         bucket.y + dy});
                        if (matches == verticesByBucket.constEnd()) {
                            continue;
                        }
                        for (int opposite : matches.value()) {
                            if (opposite == a || opposite == b
                                || QLineF(required, polygon[opposite]).length()
                                    > vertexTolerance) {
                                continue;
                            }
                            appendCandidate(101,
                                            {-centerIndex - 1, a, opposite, b},
                                            QPolygonF({center,
                                                       polygon[a],
                                                       polygon[opposite],
                                                       polygon[b]}));
                        }
                    }
                }
            }
        }
    }
    for (int centerIndex = 0; centerIndex < centerPoints.size(); ++centerIndex) {
        for (int edge = 0; edge < count; ++edge) {
            if (cancelled && cancelled()) {
                *wasCancelled = true;
                return {};
            }
            const int next = (edge + 1) % count;
            appendCandidate(103,
                            {edge, next, -centerIndex - 1},
                            QPolygonF({polygon[edge],
                                       polygon[next],
                                       centerPoints[centerIndex]}));
        }
    }
    std::sort(result.begin(), result.end(), candidateLess);
    result.erase(std::unique(result.begin(),
                             result.end(),
                             [](const CoverCandidate &left, const CoverCandidate &right) {
                                 return left.shapeId == right.shapeId
                                     && left.vertices == right.vertices;
                             }),
                 result.end());
    return result;
}

int findCandidate(const QVector<CoverCandidate> &candidates,
                  int shapeId,
                  const QVector<int> &vertices)
{
    for (int i = 0; i < candidates.size(); ++i) {
        if (candidates[i].shapeId == shapeId && candidates[i].vertices == vertices) {
            return i;
        }
    }
    return -1;
}

QByteArray bitArrayKey(const QBitArray &bits)
{
    QByteArray result((bits.size() + 7) / 8, '\0');
    for (qsizetype bit = 0; bit < bits.size(); ++bit) {
        if (bits.testBit(bit)) {
            result[bit / 8] = static_cast<char>(result[bit / 8] | (1 << (bit % 8)));
        }
    }
    return result;
}

bool buildCoverageAtoms(QVector<CoverCandidate> *candidates,
                        const QPainterPath &contour,
                        double contourArea,
                        const std::function<bool()> &cancelled,
                        QVector<double> *atomAreas,
                        bool *wasCancelled)
{
    // Splitting the contour by every candidate boundary turns continuous path
    // coverage into a finite set-cover problem without sampling the interior.
    const double atomTolerance = std::max(1e-12, contourArea * 1e-12);
    QVector<CoverageAtom> atoms;
    atoms.push_back({contour, QBitArray(candidates->size(), false), contourArea});
    for (int candidateIndex = 0; candidateIndex < candidates->size(); ++candidateIndex) {
        if (cancelled && cancelled()) {
            *wasCancelled = true;
            return false;
        }
        QVector<CoverageAtom> next;
        next.reserve(atoms.size() * 2);
        const QPainterPath &candidatePath = (*candidates)[candidateIndex].path;
        for (CoverageAtom &atom : atoms) {
            if (cancelled && cancelled()) {
                *wasCancelled = true;
                return false;
            }
            const QPainterPath intersection = atom.path.intersected(candidatePath);
            const double intersectionArea = pathArea(intersection);
            if (intersectionArea <= atomTolerance) {
                next.push_back(std::move(atom));
                continue;
            }
            const QPainterPath remainder = atom.path.subtracted(candidatePath);
            const double remainderArea = pathArea(remainder);
            if (remainderArea <= atomTolerance
                || intersectionArea >= atom.area - atomTolerance) {
                atom.signature.setBit(candidateIndex, true);
                next.push_back(std::move(atom));
                continue;
            }
            QBitArray intersectionSignature = atom.signature;
            intersectionSignature.setBit(candidateIndex, true);
            next.push_back({intersection, std::move(intersectionSignature), intersectionArea});
            next.push_back({remainder, std::move(atom.signature), remainderArea});
        }
        atoms = std::move(next);
    }

    QVector<QBitArray> signatures;
    QHash<QByteArray, int> atomBySignature;
    for (const CoverageAtom &atom : atoms) {
        if (atom.area <= atomTolerance) {
            continue;
        }
        const QByteArray key = bitArrayKey(atom.signature);
        const auto existing = atomBySignature.constFind(key);
        if (existing == atomBySignature.constEnd()) {
            atomBySignature.insert(key, atomAreas->size());
            atomAreas->push_back(atom.area);
            signatures.push_back(atom.signature);
        } else {
            (*atomAreas)[existing.value()] += atom.area;
        }
    }
    for (CoverCandidate &candidate : *candidates) {
        candidate.atoms = QBitArray(atomAreas->size(), false);
    }
    for (int atom = 0; atom < signatures.size(); ++atom) {
        bool covered = false;
        for (int candidate = 0; candidate < candidates->size(); ++candidate) {
            if (signatures[atom].testBit(candidate)) {
                (*candidates)[candidate].atoms.setBit(atom, true);
                covered = true;
            }
        }
        if (!covered) {
            return false;
        }
    }
    return !atomAreas->isEmpty();
}

int compareDouble(double left, double right)
{
    const double tolerance = 1e-12 * std::max({1.0, std::abs(left), std::abs(right)});
    if (left < right - tolerance) {
        return -1;
    }
    if (left > right + tolerance) {
        return 1;
    }
    return 0;
}

bool objectiveBetter(const CoverObjective &candidate, const CoverObjective &current)
{
    if (candidate.count != current.count) {
        return candidate.count < current.count;
    }
    if (candidate.squareCount != current.squareCount) {
        return candidate.squareCount > current.squareCount;
    }
    if (candidate.interiorAnchorCount != current.interiorAnchorCount) {
        return candidate.interiorAnchorCount > current.interiorAnchorCount;
    }
    if (const int comparison = compareDouble(candidate.centerPenalty, current.centerPenalty)) {
        return comparison < 0;
    }
    if (candidate.maximumVertexUse != current.maximumVertexUse) {
        return candidate.maximumVertexUse < current.maximumVertexUse;
    }
    if (const int comparison = compareDouble(candidate.overlapPenalty, current.overlapPenalty)) {
        return comparison < 0;
    }
    return std::lexicographical_compare(candidate.candidates.begin(),
                                        candidate.candidates.end(),
                                        current.candidates.begin(),
                                        current.candidates.end());
}

class ExactCoverSearch {
public:
    ExactCoverSearch(const QPolygonF &polygon,
                     QVector<CoverCandidate> *candidates,
                     const QVector<double> &atomAreas,
                     const QPainterPath &contour,
                     double allowedArea,
                     const std::function<bool()> &cancelled)
        : polygon_(polygon)
        , candidates_(*candidates)
        , atomAreas_(atomAreas)
        , contour_(contour)
        , contourArea_(pathArea(contour))
        , scaleSquared_(std::pow(polygonScale(polygon), 2.0))
        , allowedArea_(allowedArea)
        , cancelled_(cancelled)
        , selected_(candidates_.size(), false)
        , excluded_(candidates_.size(), false)
        , vertexUse_(polygon.size(), 0)
    {
        candidatesByAtom_.resize(atomAreas_.size());
        for (int candidate = 0; candidate < candidates_.size(); ++candidate) {
            for (qsizetype atom = 0; atom < candidates_[candidate].atoms.size(); ++atom) {
                if (candidates_[candidate].atoms.testBit(atom)) {
                    candidatesByAtom_[atom].push_back(candidate);
                }
            }
        }
    }

    void setInitialSolution(const QVector<int> &initial)
    {
        CoverObjective objective;
        if (evaluate(initial, &objective)) {
            best_ = std::move(objective);
        }
    }

    void run()
    {
        QBitArray covered(atomAreas_.size(), false);
        search(covered, 0, 0.0, 0, 0.0, 0.0, 0, 0);
    }

    bool wasCancelled() const { return wasCancelled_; }
    const CoverObjective &best() const { return best_; }

private:
    struct BranchCandidate {
        int candidate = -1;
        int gainedAtoms = 0;
        double gainedArea = 0.0;
        int resultingMaximumUse = 0;
    };

    double overlapPenalty(int first, int second)
    {
        const quint64 key = edgeKey(first, second);
        const auto cached = overlapPenaltyCache_.constFind(key);
        if (cached != overlapPenaltyCache_.constEnd()) {
            return cached.value();
        }
        const QPainterPath overlap = candidates_[first].path.intersected(candidates_[second].path);
        const double area = pathArea(overlap);
        double penalty = 0.0;
        if (area > allowedArea_ * 1e-4) {
            const QPointF delta = pathCentroid(overlap) - polygon_.boundingRect().center();
            penalty = QPointF::dotProduct(delta, delta) / scaleSquared_;
        }
        overlapPenaltyCache_.insert(key, penalty);
        return penalty;
    }

    bool evaluate(const QVector<int> &selection, CoverObjective *objective)
    {
        QPainterPath mesh;
        mesh.setFillRule(Qt::WindingFill);
        QVector<int> vertexUse(polygon_.size(), 0);
        double centerPenalty = 0.0;
        double pairPenalty = 0.0;
        int maximumUse = 0;
        int squares = 0;
        int interiorAnchors = 0;
        for (int i = 0; i < selection.size(); ++i) {
            const CoverCandidate &candidate = candidates_[selection[i]];
            mesh = mesh.united(candidate.path);
            centerPenalty += candidate.centerPenalty;
            squares += candidate.shapeId == 101 ? 1 : 0;
            interiorAnchors += usesInteriorAnchor(candidate) ? 1 : 0;
            for (int vertex : candidate.vertices) {
                if (vertex >= 0) {
                    maximumUse = std::max(maximumUse, ++vertexUse[vertex]);
                }
            }
            for (int j = 0; j < i; ++j) {
                pairPenalty += overlapPenalty(selection[j], selection[i]);
            }
        }
        if (pathArea(mesh.subtracted(contour_)) > allowedArea_
            || pathArea(contour_.subtracted(mesh)) > allowedArea_) {
            return false;
        }
        objective->count = selection.size();
        objective->maximumVertexUse = maximumUse;
        objective->centerPenalty = centerPenalty;
        objective->overlapPenalty = pairPenalty;
        objective->squareCount = squares;
        objective->interiorAnchorCount = interiorAnchors;
        objective->candidates = selection;
        std::sort(objective->candidates.begin(), objective->candidates.end());
        return true;
    }

    int lowerBound(const QBitArray &covered,
                   int uncoveredAtoms,
                   double uncoveredArea) const
    {
        int maximumAtoms = 0;
        double maximumArea = 0.0;
        for (int candidate = 0; candidate < candidates_.size(); ++candidate) {
            if (selected_.testBit(candidate) || excluded_.testBit(candidate)) {
                continue;
            }
            int gainedAtoms = 0;
            double gainedArea = 0.0;
            const QBitArray &candidateAtoms = candidates_[candidate].atoms;
            for (qsizetype atom = 0; atom < candidateAtoms.size(); ++atom) {
                if (candidateAtoms.testBit(atom) && !covered.testBit(atom)) {
                    ++gainedAtoms;
                    gainedArea += atomAreas_[atom];
                }
            }
            maximumAtoms = std::max(maximumAtoms, gainedAtoms);
            maximumArea = std::max(maximumArea, gainedArea);
        }
        if (maximumAtoms == 0 || maximumArea <= 0.0) {
            return kInfinitePartitionCost;
        }
        const int atomBound = (uncoveredAtoms + maximumAtoms - 1) / maximumAtoms;
        const int areaBound = static_cast<int>(std::ceil(
            std::max(0.0, uncoveredArea - allowedArea_ * 1e-4) / maximumArea));
        return std::max(atomBound, areaBound);
    }

    bool partialCannotBeat(int lowerBound,
                           int maximumVertexUse,
                           double centerPenalty,
                           double pairPenalty,
                           int squareCount,
                           int interiorAnchorCount) const
    {
        const int possibleCount = currentSelection_.size() + lowerBound;
        if (possibleCount > best_.count) {
            return true;
        }
        if (possibleCount < best_.count) {
            return false;
        }
        const int remainingSlots = best_.count - currentSelection_.size();
        const int possibleSquareCount = squareCount + remainingSlots;
        if (possibleSquareCount != best_.squareCount) {
            return possibleSquareCount < best_.squareCount;
        }
        const int possibleInteriorAnchorCount = interiorAnchorCount + remainingSlots;
        if (possibleInteriorAnchorCount != best_.interiorAnchorCount) {
            return possibleInteriorAnchorCount < best_.interiorAnchorCount;
        }
        if (const int comparison = compareDouble(centerPenalty, best_.centerPenalty)) {
            return comparison > 0;
        }
        if (maximumVertexUse != best_.maximumVertexUse) {
            return maximumVertexUse > best_.maximumVertexUse;
        }
        if (const int comparison = compareDouble(pairPenalty, best_.overlapPenalty)) {
            return comparison > 0;
        }
        return false;
    }

    void search(const QBitArray &covered,
                int coveredAtoms,
                double coveredArea,
                int maximumVertexUse,
                double centerPenalty,
                double pairPenalty,
                int squareCount,
                int interiorAnchorCount)
    {
        if (cancelled_ && cancelled_()) {
            wasCancelled_ = true;
            return;
        }
        if (coveredAtoms == atomAreas_.size()) {
            CoverObjective objective;
            if (evaluate(currentSelection_, &objective) && objectiveBetter(objective, best_)) {
                best_ = std::move(objective);
            }
            return;
        }
        if (currentSelection_.size() >= best_.count) {
            return;
        }

        const int uncoveredAtoms = atomAreas_.size() - coveredAtoms;
        const double uncoveredArea = std::max(0.0, contourArea_ - coveredArea);
        const int bound = lowerBound(covered, uncoveredAtoms, uncoveredArea);
        if (bound >= kInfinitePartitionCost
            || partialCannotBeat(bound,
                                 maximumVertexUse,
                                 centerPenalty,
                                 pairPenalty,
                                 squareCount,
                                 interiorAnchorCount)) {
            return;
        }

        int branchAtom = -1;
        int fewestCandidates = std::numeric_limits<int>::max();
        for (int atom = 0; atom < candidatesByAtom_.size(); ++atom) {
            if (covered.testBit(atom)) {
                continue;
            }
            int available = 0;
            for (int candidate : candidatesByAtom_[atom]) {
                available += !selected_.testBit(candidate) && !excluded_.testBit(candidate) ? 1 : 0;
            }
            if (available == 0) {
                return;
            }
            if (available < fewestCandidates
                || (available == fewestCandidates
                    && (branchAtom < 0 || atomAreas_[atom] > atomAreas_[branchAtom]))) {
                branchAtom = atom;
                fewestCandidates = available;
            }
        }

        QVector<BranchCandidate> options;
        for (int candidate : candidatesByAtom_[branchAtom]) {
            if (selected_.testBit(candidate) || excluded_.testBit(candidate)) {
                continue;
            }
            BranchCandidate option;
            option.candidate = candidate;
            option.resultingMaximumUse = maximumVertexUse;
            for (int vertex : candidates_[candidate].vertices) {
                if (vertex >= 0) {
                    option.resultingMaximumUse = std::max(option.resultingMaximumUse,
                                                           vertexUse_[vertex] + 1);
                }
            }
            for (qsizetype atom = 0; atom < candidates_[candidate].atoms.size(); ++atom) {
                if (candidates_[candidate].atoms.testBit(atom) && !covered.testBit(atom)) {
                    ++option.gainedAtoms;
                    option.gainedArea += atomAreas_[atom];
                }
            }
            options.push_back(option);
        }
        std::sort(options.begin(), options.end(), [&](const BranchCandidate &left,
                                                       const BranchCandidate &right) {
            if (left.gainedArea != right.gainedArea) {
                return left.gainedArea > right.gainedArea;
            }
            if (left.gainedAtoms != right.gainedAtoms) {
                return left.gainedAtoms > right.gainedAtoms;
            }
            const CoverCandidate &leftCandidate = candidates_[left.candidate];
            const CoverCandidate &rightCandidate = candidates_[right.candidate];
            if ((leftCandidate.shapeId == 101) != (rightCandidate.shapeId == 101)) {
                return leftCandidate.shapeId == 101;
            }
            if (usesInteriorAnchor(leftCandidate) != usesInteriorAnchor(rightCandidate)) {
                return usesInteriorAnchor(leftCandidate);
            }
            if (leftCandidate.centerPenalty != rightCandidate.centerPenalty) {
                return leftCandidate.centerPenalty < rightCandidate.centerPenalty;
            }
            if (left.resultingMaximumUse != right.resultingMaximumUse) {
                return left.resultingMaximumUse < right.resultingMaximumUse;
            }
            if (leftCandidate.shapeId != rightCandidate.shapeId) {
                return leftCandidate.shapeId < rightCandidate.shapeId;
            }
            return left.candidate < right.candidate;
        });

        QVector<int> excludedHere;
        for (const BranchCandidate &option : options) {
            if (wasCancelled_) {
                break;
            }
            const int candidate = option.candidate;
            selected_.setBit(candidate, true);
            currentSelection_.push_back(candidate);
            int nextMaximumUse = maximumVertexUse;
            for (int vertex : candidates_[candidate].vertices) {
                if (vertex >= 0) {
                    nextMaximumUse = std::max(nextMaximumUse, ++vertexUse_[vertex]);
                }
            }
            double nextPairPenalty = pairPenalty;
            for (int selected : currentSelection_) {
                if (selected != candidate) {
                    nextPairPenalty += overlapPenalty(selected, candidate);
                }
            }
            QBitArray nextCovered = covered | candidates_[candidate].atoms;
            search(nextCovered,
                   coveredAtoms + option.gainedAtoms,
                   coveredArea + option.gainedArea,
                   nextMaximumUse,
                   centerPenalty + candidates_[candidate].centerPenalty,
                   nextPairPenalty,
                   squareCount + (candidates_[candidate].shapeId == 101 ? 1 : 0),
                   interiorAnchorCount + (usesInteriorAnchor(candidates_[candidate]) ? 1 : 0));
            for (int vertex : candidates_[candidate].vertices) {
                if (vertex >= 0) {
                    --vertexUse_[vertex];
                }
            }
            currentSelection_.removeLast();
            selected_.setBit(candidate, false);
            // Later sibling branches forbid earlier choices for this atom, so
            // each candidate subset is explored once without losing covers.
            excluded_.setBit(candidate, true);
            excludedHere.push_back(candidate);
        }
        for (int candidate : excludedHere) {
            excluded_.setBit(candidate, false);
        }
    }

    QPolygonF polygon_;
    QVector<CoverCandidate> &candidates_;
    const QVector<double> &atomAreas_;
    QPainterPath contour_;
    double contourArea_ = 0.0;
    double scaleSquared_ = 1.0;
    double allowedArea_ = 0.0;
    std::function<bool()> cancelled_;
    QVector<QVector<int>> candidatesByAtom_;
    QBitArray selected_;
    QBitArray excluded_;
    QVector<int> vertexUse_;
    QVector<int> currentSelection_;
    QHash<quint64, double> overlapPenaltyCache_;
    CoverObjective best_;
    bool wasCancelled_ = false;
};

MinimumCover minimumShapeCover(const QPolygonF &input,
                               double epsilon,
                               const std::function<bool()> &cancelled)
{
    const MinimumPartition partition = minimumShapePartition(input, epsilon, cancelled);
    if (partition.cancelled || (cancelled && cancelled())) {
        MinimumCover result;
        result.cancelled = true;
        return result;
    }
    if (!partition.error.isEmpty()) {
        MinimumCover result;
        result.error = partition.error;
        return result;
    }
    MinimumCover baseline = coverFromPartition(partition);
    const QPainterPath contour = polygonPath(baseline.polygon);
    const double contourArea = pathArea(contour);
    const double containmentTolerance = std::max(1e-10, contourArea * 1e-10);
    const double allowedArea = std::max(1e-8, contourArea * 1e-8);
    const QVector<QPointF> centerPoints = interiorCenterPoints(partition, contour);
    baseline = centerBiasedCover(std::move(baseline),
                                 centerPoints,
                                 contour,
                                 containmentTolerance);
    const int baselineCount = baseline.selected.size();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kOverlapOptimizationBudgetMs);
    bool userCancelled = false;
    const auto stopOptimization = [&]() {
        if (cancelled && cancelled()) {
            userCancelled = true;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return true;
        }
        return false;
    };
    const auto cancelledResult = []() {
        MinimumCover result;
        result.cancelled = true;
        return result;
    };

    bool wasCancelled = false;
    QVector<CoverCandidate> candidates = coverCandidates(baseline.polygon,
                                                         contour,
                                                         centerPoints,
                                                         epsilon,
                                                         containmentTolerance,
                                                         stopOptimization,
                                                         &wasCancelled);
    if (userCancelled) {
        return cancelledResult();
    }
    if (wasCancelled) {
        return baseline;
    }
    if (candidates.isEmpty()) {
        return baseline;
    }
    for (const CoverCandidate &baselineCandidate : baseline.candidates) {
        if (findCandidate(candidates,
                          baselineCandidate.shapeId,
                          baselineCandidate.vertices) < 0) {
            candidates.push_back(baselineCandidate);
        }
    }
    std::sort(candidates.begin(), candidates.end(), candidateLess);
    candidates.erase(std::unique(candidates.begin(),
                                 candidates.end(),
                                 [](const CoverCandidate &left,
                                    const CoverCandidate &right) {
                                     return left.shapeId == right.shapeId
                                         && left.vertices == right.vertices;
                                 }),
                     candidates.end());

    QVector<int> initial;
    initial.reserve(baseline.candidates.size());
    for (const CoverCandidate &baselineCandidate : baseline.candidates) {
        const int candidate = findCandidate(candidates,
                                            baselineCandidate.shapeId,
                                            baselineCandidate.vertices);
        if (candidate < 0) {
            return baseline;
        }
        initial.push_back(candidate);
    }

    QVector<double> atomAreas;
    if (!buildCoverageAtoms(&candidates,
                            contour,
                            contourArea,
                            stopOptimization,
                            &atomAreas,
                            &wasCancelled)) {
        if (userCancelled) {
            return cancelledResult();
        }
        return baseline;
    }
    ExactCoverSearch search(baseline.polygon,
                            &candidates,
                            atomAreas,
                            contour,
                            allowedArea,
                            stopOptimization);
    search.setInitialSolution(initial);
    search.run();
    if (userCancelled) {
        return cancelledResult();
    }
    if (search.best().candidates.isEmpty()
        || search.best().count > baselineCount) {
        return baseline;
    }
    MinimumCover result;
    result.polygon = baseline.polygon;
    result.candidates = std::move(candidates);
    result.selected = search.best().candidates;
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
            ? QStringLiteral("Square or Triangle geometry is unavailable")
            : QStringLiteral("Triangle geometry is unavailable");
        return result;
    }
    const PolygonContour contour = buildPolygonContour(request.points);
    if (!contour.valid()) {
        result.error = contour.error.isEmpty() ? QStringLiteral("Invalid polygonal lasso") : contour.error;
        return result;
    }
    result.contour = contour.path;
    const double scale = polygonScale(contour.polygon);
    const double epsilon = kEpsilon * scale * scale;
    QPainterPath mesh;
    mesh.setFillRule(Qt::WindingFill);
    if (request.mergeSquares) {
        const MinimumCover cover = minimumShapeCover(contour.polygon, epsilon, cancelled);
        if (cover.cancelled || (cancelled && cancelled())) {
            result.cancelled = true;
            return result;
        }
        if (!cover.error.isEmpty()) {
            result.error = cover.error;
            return result;
        }
        for (int candidateIndex : cover.selected) {
            if (cancelled && cancelled()) {
                result.placements.clear();
                result.cancelled = true;
                return result;
            }
            const CoverCandidate &candidate = cover.candidates[candidateIndex];
            bool ok = false;
            if (candidate.shapeId == 101) {
                const QTransform transform = affineFromTriangles(request.sources.square[0],
                                                                 request.sources.square[1],
                                                                 request.sources.square[3],
                                                                 candidate.polygon[0],
                                                                 candidate.polygon[1],
                                                                 candidate.polygon[3],
                                                                 &ok);
                if (!ok
                    || QLineF(transform.map(request.sources.square[2]),
                              candidate.polygon[2]).length() > scale * 1e-7) {
                    result.error = QStringLiteral("Could not map a Square into the lasso mesh");
                    result.placements.clear();
                    return result;
                }
                result.placements.push_back({101, transform});
                mesh = mesh.united(transform.map(polygonPath(request.sources.square)));
            } else if (candidate.shapeId == 103) {
                const QTransform transform = affineFromTriangles(request.sources.triangle[0],
                                                                 request.sources.triangle[1],
                                                                 request.sources.triangle[2],
                                                                 candidate.polygon[0],
                                                                 candidate.polygon[1],
                                                                 candidate.polygon[2],
                                                                 &ok);
                if (!ok) {
                    result.error = QStringLiteral("Could not map a Triangle into the lasso mesh");
                    result.placements.clear();
                    return result;
                }
                result.placements.push_back({103, transform});
                mesh = mesh.united(transform.map(polygonPath(request.sources.triangle)));
            }
        }
    } else {
        QString triangulationError;
        const QVector<MeshTriangle> triangles = triangulate(contour.polygon,
                                                            epsilon,
                                                            cancelled,
                                                            &triangulationError);
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        if (triangles.isEmpty()) {
            result.error = triangulationError;
            return result;
        }
        for (const MeshTriangle &triangle : triangles) {
            if (cancelled && cancelled()) {
                result.placements.clear();
                result.cancelled = true;
                return result;
            }
            bool ok = false;
            const QTransform transform = affineFromTriangles(request.sources.triangle[0],
                                                             request.sources.triangle[1],
                                                             request.sources.triangle[2],
                                                             contour.polygon[triangle.a],
                                                             contour.polygon[triangle.b],
                                                             contour.polygon[triangle.c],
                                                             &ok);
            if (!ok) {
                result.error = QStringLiteral("Could not map a Triangle into the lasso mesh");
                result.placements.clear();
                return result;
            }
            result.placements.push_back({103, transform});
            mesh = mesh.united(transform.map(polygonPath(request.sources.triangle)));
        }
    }

    const double allowedArea = std::max(1e-8, pathArea(contour.path) * 1e-8);
    if (pathArea(mesh.subtracted(contour.path)) > allowedArea
        || pathArea(contour.path.subtracted(mesh)) > allowedArea) {
        result.error = QStringLiteral("The generated lasso mesh did not match its contour");
        result.placements.clear();
    }
    return result;
}

} // namespace gui
