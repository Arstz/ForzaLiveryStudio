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

quint64 edgeKey(int a, int b)
{
    const quint32 low = static_cast<quint32>(std::min(a, b));
    const quint32 high = static_cast<quint32>(std::max(a, b));
    return (static_cast<quint64>(low) << 32) | high;
}

struct SquareCandidate {
    int firstTriangle = -1;
    int secondTriangle = -1;
    QPolygonF quad;
};

QPolygonF orderedQuad(const MeshTriangle &first,
                      const MeshTriangle &second,
                      const QPolygonF &polygon)
{
    QVector<int> indices = {first.a, first.b, first.c, second.a, second.b, second.c};
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.size() != 4) {
        return {};
    }
    QPointF center;
    for (int index : indices) {
        center += polygon[index];
    }
    center /= 4.0;
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return std::atan2(polygon[a].y() - center.y(), polygon[a].x() - center.x())
            < std::atan2(polygon[b].y() - center.y(), polygon[b].x() - center.x());
    });
    QPolygonF result;
    for (int index : indices) {
        result.push_back(polygon[index]);
    }
    return result;
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

QVector<SquareCandidate> squareCandidates(const QVector<MeshTriangle> &triangles,
                                          const QPolygonF &polygon,
                                          double epsilon)
{
    QHash<quint64, QVector<int>> trianglesByEdge;
    for (int i = 0; i < triangles.size(); ++i) {
        const MeshTriangle &triangle = triangles[i];
        for (const auto &edge : {qMakePair(triangle.a, triangle.b),
                                 qMakePair(triangle.b, triangle.c),
                                 qMakePair(triangle.c, triangle.a)}) {
            trianglesByEdge[edgeKey(edge.first, edge.second)].push_back(i);
        }
    }

    QVector<SquareCandidate> result;
    for (auto it = trianglesByEdge.constBegin(); it != trianglesByEdge.constEnd(); ++it) {
        if (it.value().size() != 2) {
            continue;
        }
        const int first = it.value()[0];
        const int second = it.value()[1];
        const QPolygonF quad = orderedQuad(triangles[first], triangles[second], polygon);
        if (isParallelogram(quad, epsilon)) {
            result.push_back({first, second, quad});
        }
    }
    std::sort(result.begin(), result.end(), [](const SquareCandidate &a, const SquareCandidate &b) {
        return qMakePair(a.firstTriangle, a.secondTriangle)
            < qMakePair(b.firstTriangle, b.secondTriangle);
    });
    return result;
}

struct MatchNode {
    QVector<QPair<int, int>> edges;
    int freeScore = 0;
    int parentMatchedScore = 0;
    int chosenChild = -1;
    int chosenCandidate = -1;
};

void scoreMatching(int node,
                   int parent,
                   QVector<MatchNode> *nodes)
{
    MatchNode &current = (*nodes)[node];
    int base = 0;
    for (const auto &edge : current.edges) {
        if (edge.first == parent) {
            continue;
        }
        scoreMatching(edge.first, node, nodes);
        base += (*nodes)[edge.first].freeScore;
    }
    current.parentMatchedScore = base;
    current.freeScore = base;
    for (const auto &edge : current.edges) {
        if (edge.first == parent) {
            continue;
        }
        const int candidateScore = base - (*nodes)[edge.first].freeScore
            + (*nodes)[edge.first].parentMatchedScore + 1;
        if (candidateScore > current.freeScore
            || (candidateScore == current.freeScore
                && (current.chosenChild < 0 || edge.first < current.chosenChild))) {
            current.freeScore = candidateScore;
            current.chosenChild = edge.first;
            current.chosenCandidate = edge.second;
        }
    }
}

void collectMatching(int node,
                     int parent,
                     bool matchedToParent,
                     const QVector<MatchNode> &nodes,
                     QSet<int> *selectedCandidates)
{
    const MatchNode &current = nodes[node];
    const int matchedChild = matchedToParent ? -1 : current.chosenChild;
    if (matchedChild >= 0) {
        selectedCandidates->insert(current.chosenCandidate);
    }
    for (const auto &edge : current.edges) {
        if (edge.first != parent) {
            collectMatching(edge.first,
                            node,
                            edge.first == matchedChild,
                            nodes,
                            selectedCandidates);
        }
    }
}

QSet<int> maximumSquareMatching(int triangleCount,
                                const QVector<SquareCandidate> &candidates)
{
    QVector<MatchNode> nodes(triangleCount);
    for (int i = 0; i < candidates.size(); ++i) {
        const SquareCandidate &candidate = candidates[i];
        nodes[candidate.firstTriangle].edges.push_back({candidate.secondTriangle, i});
        nodes[candidate.secondTriangle].edges.push_back({candidate.firstTriangle, i});
    }
    for (MatchNode &node : nodes) {
        std::sort(node.edges.begin(), node.edges.end());
    }

    QSet<int> selected;
    QVector<bool> visited(triangleCount, false);
    for (int root = 0; root < triangleCount; ++root) {
        if (visited[root] || nodes[root].edges.isEmpty()) {
            continue;
        }
        QVector<int> stack = {root};
        visited[root] = true;
        while (!stack.isEmpty()) {
            const int node = stack.takeLast();
            for (const auto &edge : nodes[node].edges) {
                if (!visited[edge.first]) {
                    visited[edge.first] = true;
                    stack.push_back(edge.first);
                }
            }
        }
        scoreMatching(root, -1, &nodes);
        collectMatching(root, -1, false, nodes, &selected);
    }
    return selected;
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
        result.error = QStringLiteral("A polygon needs at least three distinct vertices");
        return result;
    }

    const double scale = polygonScale(result.polygon);
    const double coordinateEpsilon = polygonCoordinateEpsilon(result.polygon);
    result.crossings = polygonCrossings(result.polygon);
    if (!result.crossings.isEmpty()) {
        result.error = QStringLiteral("The polygon crosses itself");
        return result;
    }
    if (std::abs(signedArea(result.polygon)) <= coordinateEpsilon * scale) {
        result.error = QStringLiteral("The polygon has no fillable area");
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
        result.error = contour.error.isEmpty() ? QStringLiteral("Invalid polygon") : contour.error;
        return result;
    }
    result.contour = contour.path;
    const double scale = polygonScale(contour.polygon);
    const double epsilon = kEpsilon * scale * scale;
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

    QVector<SquareCandidate> candidates;
    QSet<int> selectedSquares;
    if (request.mergeSquares) {
        candidates = squareCandidates(triangles, contour.polygon, epsilon);
        selectedSquares = maximumSquareMatching(triangles.size(), candidates);
    }
    QVector<int> selectedSquareIndices(selectedSquares.begin(), selectedSquares.end());
    std::sort(selectedSquareIndices.begin(), selectedSquareIndices.end());
    QVector<bool> consumed(triangles.size(), false);
    QPainterPath mesh;
    mesh.setFillRule(Qt::WindingFill);
    for (int candidateIndex : selectedSquareIndices) {
        const SquareCandidate &candidate = candidates[candidateIndex];
        bool ok = false;
        const QTransform transform = affineFromTriangles(request.sources.square[0],
                                                         request.sources.square[1],
                                                         request.sources.square[3],
                                                         candidate.quad[0],
                                                         candidate.quad[1],
                                                         candidate.quad[3],
                                                         &ok);
        if (!ok || QLineF(transform.map(request.sources.square[2]), candidate.quad[2]).length()
                       > scale * 1e-7) {
            result.error = QStringLiteral("Could not map a Square into the lasso mesh");
            result.placements.clear();
            return result;
        }
        result.placements.push_back({101, transform});
        consumed[candidate.firstTriangle] = true;
        consumed[candidate.secondTriangle] = true;
        mesh = mesh.united(transform.map(polygonPath(request.sources.square)));
    }
    for (int i = 0; i < triangles.size(); ++i) {
        if (cancelled && cancelled()) {
            result.placements.clear();
            result.cancelled = true;
            return result;
        }
        if (consumed[i]) {
            continue;
        }
        const MeshTriangle &triangle = triangles[i];
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

    const double allowedArea = std::max(1e-8, pathArea(contour.path) * 1e-8);
    if (pathArea(mesh.subtracted(contour.path)) > allowedArea
        || pathArea(contour.path.subtracted(mesh)) > allowedArea) {
        result.error = QStringLiteral("The generated lasso mesh did not match its contour");
        result.placements.clear();
    }
    return result;
}

} // namespace gui
