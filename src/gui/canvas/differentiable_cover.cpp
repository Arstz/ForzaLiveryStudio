#include "differentiable_cover.h"

#include <clipper2/clipper.engine.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <utility>

namespace gui::cover {
namespace {

constexpr std::array<int, 9> kShapeIds = {
    101, 102, 103, 109, 127, 129, 130, 139, 2123,
};
constexpr double kGeometryEpsilon = 1e-10;
constexpr double kClipperScale = 1000000.0;
constexpr int kGradientCount = 6;
constexpr int kRouterCandidateCount = 3;
constexpr int kMaximumDistanceDimension = 1024;
constexpr double kAdamBeta1 = 0.9;
constexpr double kAdamBeta2 = 0.999;
constexpr double kAdamEpsilon = 1e-8;
constexpr double kGradientNormLimit = 1000000.0;
constexpr double kGradientStopNorm = 1e-8;
constexpr double kRestartTranslationFraction = 0.15;
constexpr double kRestartAngleRange = 0.2;
constexpr double kRestartScaleRange = 0.12;
constexpr double kInitialRadiusFraction = 0.8;
constexpr double kMinimumAffineScale = 1e-6;
constexpr double kLegalShrinkFactor = 0.9;
constexpr int kLegalShrinkSteps = 64;

struct Jet {
    std::array<double, kGradientCount> gradient{};
    double value = 0.0;
};

struct JetPoint {
    Jet x;
    Jet y;
};

struct DistanceSeed {
    QPointF point;
    double angle = 0.0;
    double radius = 0.0;
};

struct Candidate {
    Affine transform;
    int shapeId = 0;
    double covered = 0.0;
    double spill = 0.0;
    bool valid = false;
};

struct QueueNode {
    int index = 0;
    double distance = 0.0;

    bool operator>(const QueueNode &other) const {
        return distance > other.distance;
    }
};

Jet constantJet(double value) {
    Jet result;
    result.value = value;

    return result;
}

Jet parameterJet(double value, int parameter) {
    Jet result = constantJet(value);
    result.gradient[parameter] = 1.0;

    return result;
}

Jet operator+(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value + right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = left.gradient[i] + right.gradient[i];
    }

    return result;
}

Jet operator-(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value - right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = left.gradient[i] - right.gradient[i];
    }

    return result;
}

Jet operator-(const Jet &value) {
    Jet result;
    result.value = -value.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = -value.gradient[i];
    }

    return result;
}

Jet operator*(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value * right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = left.gradient[i] * right.value
            + left.value * right.gradient[i];
    }

    return result;
}

Jet operator*(const Jet &left, double right) {
    return left * constantJet(right);
}

Jet operator*(double left, const Jet &right) {
    return constantJet(left) * right;
}

Jet operator/(const Jet &left, const Jet &right) {
    Jet result;
    const double denominator = right.value * right.value;

    result.value = left.value / right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = (left.gradient[i] * right.value
                              - left.value * right.gradient[i])
            / denominator;
    }

    return result;
}

Jet absoluteJet(const Jet &value) {
    return value.value < 0.0 ? -value : value;
}

JetPoint operator+(const JetPoint &left, const JetPoint &right) {
    return {left.x + right.x, left.y + right.y};
}

JetPoint operator-(const JetPoint &left, const JetPoint &right) {
    return {left.x - right.x, left.y - right.y};
}

JetPoint operator*(const JetPoint &point, const Jet &scale) {
    return {point.x * scale, point.y * scale};
}

Jet cross(const JetPoint &left, const JetPoint &right) {
    return left.x * right.y - left.y * right.x;
}

double cross(const Vec2 &left, const Vec2 &right) {
    return left.x * right.y - left.y * right.x;
}

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &left = polygon[i];
        const QPointF &right = polygon[(i + 1) % polygon.size()];
        result += left.x() * right.y() - left.y() * right.x();
    }

    return result * 0.5;
}

double signedArea(const QVector<Vec2> &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const Vec2 &left = polygon[i];
        const Vec2 &right = polygon[(i + 1) % polygon.size()];
        result += cross(left, right);
    }

    return result * 0.5;
}

double polygonSetArea(const Polygons &polygons) {
    double result = 0.0;
    for (const QPolygonF &polygon : polygons) {
        result += signedArea(polygon);
    }

    return std::abs(result);
}

QPolygonF normalizedPolygon(QPolygonF polygon) {
    while (polygon.size() > 1
           && QLineF(polygon.front(), polygon.back()).length() <= kGeometryEpsilon) {
        polygon.removeLast();
    }
    if (polygon.size() < 3 || std::abs(signedArea(polygon)) <= kGeometryEpsilon) {
        return {};
    }

    return polygon;
}

QRectF polygonBounds(const Polygons &polygons) {
    QRectF result;
    bool haveBounds = false;
    for (const QPolygonF &polygon : polygons) {
        if (polygon.isEmpty()) {
            continue;
        }
        result = haveBounds ? result.united(polygon.boundingRect())
                            : polygon.boundingRect();
        haveBounds = true;
    }

    return result;
}

JetPoint transformedVertex(const Vec2 &vertex, const Affine &transform) {
    const Jet a = parameterJet(transform.a, 0);
    const Jet b = parameterJet(transform.b, 1);
    const Jet c = parameterJet(transform.c, 2);
    const Jet d = parameterJet(transform.d, 3);
    const Jet e = parameterJet(transform.e, 4);
    const Jet f = parameterJet(transform.f, 5);

    return {
        a * vertex.x + c * vertex.y + e,
        b * vertex.x + d * vertex.y + f,
    };
}

JetPoint constantPoint(const QPointF &point) {
    return {constantJet(point.x()), constantJet(point.y())};
}

Jet sideValue(const JetPoint &point,
              const JetPoint &windowStart,
              const JetPoint &windowEnd) {
    return cross(windowEnd - windowStart, point - windowStart);
}

JetPoint intersectionPoint(const JetPoint &start,
                           const JetPoint &end,
                           const Jet &startSide,
                           const Jet &endSide) {
    const Jet denominator = startSide - endSide;
    if (std::abs(denominator.value) <= kGeometryEpsilon) {
        return end;
    }
    const Jet fraction = startSide / denominator;

    return start + (end - start) * fraction;
}

QVector<JetPoint> clipHalfPlane(const QVector<JetPoint> &subject,
                                const JetPoint &windowStart,
                                const JetPoint &windowEnd) {
    QVector<JetPoint> result;
    if (subject.isEmpty()) {
        return result;
    }

    JetPoint start = subject.back();
    Jet startSide = sideValue(start, windowStart, windowEnd);
    bool startInside = startSide.value >= -kGeometryEpsilon;
    for (const JetPoint &end : subject) {
        const Jet endSide = sideValue(end, windowStart, windowEnd);
        const bool endInside = endSide.value >= -kGeometryEpsilon;
        if (endInside != startInside) {
            result.push_back(intersectionPoint(start, end, startSide, endSide));
        }
        if (endInside) {
            result.push_back(end);
        }
        start = end;
        startSide = endSide;
        startInside = endInside;
    }

    return result;
}

Jet clippedPolygonArea(const QPolygonF &subject,
                       std::array<JetPoint, 3> window) {
    QVector<JetPoint> clipped;
    clipped.reserve(subject.size() + 3);
    for (const QPointF &point : subject) {
        clipped.push_back(constantPoint(point));
    }

    const Jet orientation = cross(window[1] - window[0],
                                  window[2] - window[0]);
    if (orientation.value < 0.0) {
        std::swap(window[1], window[2]);
    }
    for (int edge = 0; edge < 3 && !clipped.isEmpty(); ++edge) {
        clipped = clipHalfPlane(clipped, window[edge], window[(edge + 1) % 3]);
    }
    if (clipped.size() < 3) {
        return {};
    }

    Jet doubledArea;
    for (int i = 0; i < clipped.size(); ++i) {
        doubledArea = doubledArea
            + cross(clipped[i], clipped[(i + 1) % clipped.size()]);
    }

    return doubledArea * 0.5;
}

Jet intersectionArea(const ShapeMesh &shape,
                     const Affine &transform,
                     const Polygons &subject) {
    Jet result;
    for (const std::array<int, 3> &triangle : shape.triangles) {
        std::array<JetPoint, 3> window = {
            transformedVertex(shape.vertices[triangle[0]], transform),
            transformedVertex(shape.vertices[triangle[1]], transform),
            transformedVertex(shape.vertices[triangle[2]], transform),
        };
        for (const QPolygonF &polygon : subject) {
            result = result + clippedPolygonArea(polygon, window);
        }
    }

    return result;
}

Jet transformedShapeArea(const ShapeMesh &shape, const Affine &transform) {
    const Jet a = parameterJet(transform.a, 0);
    const Jet b = parameterJet(transform.b, 1);
    const Jet c = parameterJet(transform.c, 2);
    const Jet d = parameterJet(transform.d, 3);

    return absoluteJet(a * d - b * c) * shape.area;
}

bool finiteGradient(const AreaGradient &evaluation) {
    if (!std::isfinite(evaluation.covered) || !std::isfinite(evaluation.spill)) {
        return false;
    }
    for (int i = 0; i < kGradientCount; ++i) {
        if (!std::isfinite(evaluation.coveredGradient[i])
            || !std::isfinite(evaluation.spillGradient[i])) {
            return false;
        }
    }

    return true;
}

Vec2 transformedPoint(const Vec2 &point, const Affine &transform) {
    return {
        transform.a * point.x + transform.c * point.y + transform.e,
        transform.b * point.x + transform.d * point.y + transform.f,
    };
}

QPolygonF transformedBoundary(const ShapeMesh &shape, const Affine &transform) {
    QPolygonF result;
    result.reserve(shape.boundary.size());
    for (const Vec2 &point : shape.boundary) {
        const Vec2 mapped = transformedPoint(point, transform);
        result.push_back(QPointF(mapped.x, mapped.y));
    }

    return result;
}

Clipper2Lib::Paths64 toClipperPaths(const Polygons &polygons) {
    Clipper2Lib::Paths64 result;
    result.reserve(static_cast<size_t>(polygons.size()));
    for (const QPolygonF &polygon : polygons) {
        Clipper2Lib::Path64 path;
        path.reserve(static_cast<size_t>(polygon.size()));
        for (const QPointF &point : polygon) {
            path.emplace_back(std::llround(point.x() * kClipperScale),
                              std::llround(point.y() * kClipperScale));
        }
        if (path.size() >= 3) {
            result.push_back(std::move(path));
        }
    }

    return result;
}

Polygons fromClipperPaths(const Clipper2Lib::Paths64 &paths) {
    Polygons result;
    result.reserve(static_cast<qsizetype>(paths.size()));
    for (const Clipper2Lib::Path64 &path : paths) {
        QPolygonF polygon;
        polygon.reserve(static_cast<qsizetype>(path.size()));
        for (const Clipper2Lib::Point64 &point : path) {
            polygon.push_back(QPointF(static_cast<double>(point.x) / kClipperScale,
                                      static_cast<double>(point.y) / kClipperScale));
        }
        polygon = normalizedPolygon(std::move(polygon));
        if (!polygon.isEmpty()) {
            result.push_back(std::move(polygon));
        }
    }

    return result;
}

Polygons booleanOperation(const Polygons &subjects,
                          const Polygons &clips,
                          Clipper2Lib::ClipType operation) {
    Clipper2Lib::Clipper64 clipper;
    Clipper2Lib::Paths64 result;
    clipper.AddSubject(toClipperPaths(subjects));
    if (!clips.isEmpty()) {
        clipper.AddClip(toClipperPaths(clips));
    }
    if (!clipper.Execute(operation, Clipper2Lib::FillRule::NonZero, result)) {
        return {};
    }

    return fromClipperPaths(result);
}

Polygons differencePolygons(const Polygons &subjects, const Polygons &clips) {
    return booleanOperation(subjects, clips, Clipper2Lib::ClipType::Difference);
}

Polygons unionPolygons(const Polygons &subjects) {
    return booleanOperation(subjects, {}, Clipper2Lib::ClipType::Union);
}

bool samePoint(const QPointF &left, const QPointF &right) {
    return std::abs(left.x() - right.x()) <= kGeometryEpsilon
        && std::abs(left.y() - right.y()) <= kGeometryEpsilon;
}

int vertexIndex(QVector<Vec2> *vertices, const QPointF &point) {
    for (int i = 0; i < vertices->size(); ++i) {
        if (samePoint(QPointF((*vertices)[i].x, (*vertices)[i].y), point)) {
            return i;
        }
    }
    vertices->push_back({point.x(), point.y()});

    return vertices->size() - 1;
}

bool boundaryIsConvex(const QVector<Vec2> &boundary) {
    double sign = 0.0;
    for (int i = 0; i < boundary.size(); ++i) {
        const Vec2 left{
            boundary[(i + 1) % boundary.size()].x - boundary[i].x,
            boundary[(i + 1) % boundary.size()].y - boundary[i].y,
        };
        const Vec2 right{
            boundary[(i + 2) % boundary.size()].x
                - boundary[(i + 1) % boundary.size()].x,
            boundary[(i + 2) % boundary.size()].y
                - boundary[(i + 1) % boundary.size()].y,
        };
        const double turn = cross(left, right);
        if (std::abs(turn) <= kGeometryEpsilon) {
            continue;
        }
        if (sign == 0.0) {
            sign = turn;
        } else if (turn * sign < 0.0) {
            return false;
        }
    }

    return true;
}

ShapeMesh buildShapeMesh(int shapeId, const ShapeGeometry &geometry) {
    ShapeMesh result;
    result.id = shapeId;
    result.triangles.reserve(geometry.triangles.size());
    std::map<std::pair<int, int>, int> edgeCounts;
    for (const ShapeTriangle &triangle : geometry.triangles) {
        if (std::abs(triangle.alpha0 - 1.0) > kGeometryEpsilon
            || std::abs(triangle.alpha1 - 1.0) > kGeometryEpsilon
            || std::abs(triangle.alpha2 - 1.0) > kGeometryEpsilon) {
            result.error = QStringLiteral("shape %1 has non-opaque vertices").arg(shapeId);
            return result;
        }
        const std::array<int, 3> indices = {
            vertexIndex(&result.vertices, triangle.p0),
            vertexIndex(&result.vertices, triangle.p1),
            vertexIndex(&result.vertices, triangle.p2),
        };
        result.triangles.push_back(indices);
        for (int edge = 0; edge < 3; ++edge) {
            const int left = indices[edge];
            const int right = indices[(edge + 1) % 3];
            ++edgeCounts[std::minmax(left, right)];
        }
    }
    if (result.triangles.size() != result.vertices.size() - 2) {
        result.error = QStringLiteral("shape %1 triangulation count is invalid").arg(shapeId);
        return result;
    }

    QVector<QVector<int>> adjacency(result.vertices.size());
    int boundaryEdgeCount = 0;
    for (const auto &[edge, count] : edgeCounts) {
        if (count > 2) {
            result.error = QStringLiteral("shape %1 has a non-manifold edge").arg(shapeId);
            return result;
        }
        if (count == 1) {
            adjacency[edge.first].push_back(edge.second);
            adjacency[edge.second].push_back(edge.first);
            ++boundaryEdgeCount;
        }
    }
    if (boundaryEdgeCount != result.vertices.size()) {
        result.error = QStringLiteral("shape %1 boundary count is invalid").arg(shapeId);
        return result;
    }
    for (const QVector<int> &neighbors : adjacency) {
        if (neighbors.size() != 2) {
            result.error = QStringLiteral("shape %1 boundary is not a loop").arg(shapeId);
            return result;
        }
    }

    int previous = -1;
    int current = 0;
    for (int i = 0; i < result.vertices.size(); ++i) {
        result.boundary.push_back(result.vertices[current]);
        const QVector<int> &neighbors = adjacency[current];
        const int next = neighbors[0] == previous ? neighbors[1] : neighbors[0];
        previous = current;
        current = next;
    }
    if (current != 0) {
        result.error = QStringLiteral("shape %1 boundary did not close").arg(shapeId);
        return result;
    }
    if (signedArea(result.boundary) < 0.0) {
        std::reverse(result.boundary.begin(), result.boundary.end());
    }

    double triangleArea = 0.0;
    for (const std::array<int, 3> &triangle : result.triangles) {
        const Vec2 &a = result.vertices[triangle[0]];
        const Vec2 &b = result.vertices[triangle[1]];
        const Vec2 &c = result.vertices[triangle[2]];
        triangleArea += std::abs(cross(
            Vec2{b.x - a.x, b.y - a.y},
            Vec2{c.x - a.x, c.y - a.y}))
            * 0.5;
    }
    const double boundaryArea = std::abs(signedArea(result.boundary));
    if (std::abs(triangleArea - boundaryArea)
        > std::max(1.0, boundaryArea) * 1e-8) {
        result.error = QStringLiteral("shape %1 triangles overlap or leave gaps")
                           .arg(shapeId);
        return result;
    }
    result.area = boundaryArea;
    result.convex = boundaryIsConvex(result.boundary);

    return result;
}

Polygons normalizedInputPolygons(const Polygons &polygons) {
    Polygons result;
    result.reserve(polygons.size());
    for (QPolygonF polygon : polygons) {
        polygon = normalizedPolygon(std::move(polygon));
        if (!polygon.isEmpty()) {
            result.push_back(std::move(polygon));
        }
    }
    if (result.size() == 1 && signedArea(result.front()) < 0.0) {
        std::reverse(result.front().begin(), result.front().end());
    }

    return result;
}

DistanceSeed distanceSeed(const Polygons &polygons) {
    const QRectF bounds = polygonBounds(polygons);
    DistanceSeed result;
    result.point = bounds.center();
    if (!bounds.isValid() || bounds.isEmpty()) {
        return result;
    }

    const double maximumExtent = std::max(bounds.width(), bounds.height());
    const double step = std::max(1.0, maximumExtent / kMaximumDistanceDimension);
    const int width = std::max(3, static_cast<int>(std::ceil(bounds.width() / step)) + 3);
    const int height = std::max(3, static_cast<int>(std::ceil(bounds.height() / step)) + 3);
    QImage mask(width, height, QImage::Format_Grayscale8);
    mask.fill(0);
    QPainterPath maskPath;
    maskPath.setFillRule(Qt::WindingFill);
    for (const QPolygonF &polygon : polygons) {
        QPolygonF mapped;
        mapped.reserve(polygon.size());
        for (const QPointF &point : polygon) {
            mapped.push_back(QPointF((point.x() - bounds.left()) / step + 1.0,
                                      (point.y() - bounds.top()) / step + 1.0));
        }
        maskPath.addPolygon(mapped);
    }
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillPath(maskPath, Qt::white);
    }

    const double diagonal = std::sqrt(2.0);
    const double infinite = std::numeric_limits<double>::infinity();
    QVector<double> distance(width * height, infinite);
    const auto isInside = [&mask](int x, int y) {
        return mask.constScanLine(y)[x] != 0;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!isInside(x, y)) {
                distance[y * width + x] = 0.0;
            }
        }
    }
    const auto relax = [&distance, width, height](int x, int y,
                                                   int otherX, int otherY,
                                                   double cost) {
        if (otherX < 0 || otherX >= width || otherY < 0 || otherY >= height) {
            return;
        }
        const int index = y * width + x;
        distance[index] = std::min(distance[index],
                                   distance[otherY * width + otherX] + cost);
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!isInside(x, y)) {
                continue;
            }
            relax(x, y, x - 1, y, 1.0);
            relax(x, y, x, y - 1, 1.0);
            relax(x, y, x - 1, y - 1, diagonal);
            relax(x, y, x + 1, y - 1, diagonal);
        }
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            if (!isInside(x, y)) {
                continue;
            }
            relax(x, y, x + 1, y, 1.0);
            relax(x, y, x, y + 1, 1.0);
            relax(x, y, x + 1, y + 1, diagonal);
            relax(x, y, x - 1, y + 1, diagonal);
        }
    }

    int bestIndex = -1;
    double bestDistance = 0.0;
    for (int i = 0; i < distance.size(); ++i) {
        if (std::isfinite(distance[i]) && distance[i] > bestDistance) {
            bestDistance = distance[i];
            bestIndex = i;
        }
    }
    if (bestIndex < 0) {
        return result;
    }
    const int seedX = bestIndex % width;
    const int seedY = bestIndex / width;
    result.point = QPointF(bounds.left() + (seedX - 0.5) * step,
                           bounds.top() + (seedY - 0.5) * step);
    result.radius = bestDistance * step;

    QVector<QPointF> localPoints;
    const double neighborhood = std::max(step, result.radius * 2.5);
    for (const QPolygonF &polygon : polygons) {
        for (const QPointF &point : polygon) {
            if (QLineF(point, result.point).length() <= neighborhood) {
                localPoints.push_back(point);
            }
        }
    }
    if (localPoints.size() < 3) {
        for (const QPolygonF &polygon : polygons) {
            localPoints += polygon;
        }
    }
    QPointF mean;
    for (const QPointF &point : localPoints) {
        mean += point;
    }
    if (!localPoints.isEmpty()) {
        mean /= localPoints.size();
    }
    double covarianceXX = 0.0;
    double covarianceXY = 0.0;
    double covarianceYY = 0.0;
    for (const QPointF &point : localPoints) {
        const QPointF centered = point - mean;
        covarianceXX += centered.x() * centered.x();
        covarianceXY += centered.x() * centered.y();
        covarianceYY += centered.y() * centered.y();
    }
    result.angle = 0.5 * std::atan2(2.0 * covarianceXY,
                                    covarianceXX - covarianceYY);

    return result;
}

QRectF shapeBounds(const ShapeMesh &shape) {
    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const Vec2 &point : shape.boundary) {
        minimumX = std::min(minimumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumX = std::max(maximumX, point.x);
        maximumY = std::max(maximumY, point.y);
    }

    return QRectF(QPointF(minimumX, minimumY),
                  QPointF(maximumX, maximumY));
}

Affine initialTransform(const ShapeMesh &shape,
                        const DistanceSeed &seed,
                        double angleOffset,
                        double scaleFactor,
                        const QPointF &translationOffset) {
    const QRectF bounds = shapeBounds(shape);
    const QPointF localCenter = bounds.center();
    double sourceRadius = 0.0;
    for (const Vec2 &point : shape.boundary) {
        sourceRadius = std::max(sourceRadius,
                                QLineF(QPointF(point.x, point.y), localCenter).length());
    }
    const double scale = std::max(
        kMinimumAffineScale,
        seed.radius * kInitialRadiusFraction
            / std::max(sourceRadius, kGeometryEpsilon) * scaleFactor);
    const double angle = seed.angle + angleOffset;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    Affine result;
    result.a = cosine * scale;
    result.b = sine * scale;
    result.c = -sine * scale;
    result.d = cosine * scale;
    result.e = seed.point.x() + translationOffset.x()
        - result.a * localCenter.x() - result.c * localCenter.y();
    result.f = seed.point.y() + translationOffset.y()
        - result.b * localCenter.x() - result.d * localCenter.y();

    return result;
}

std::array<double, kGradientCount> affineValues(const Affine &transform) {
    return {
        transform.a, transform.b, transform.c,
        transform.d, transform.e, transform.f,
    };
}

Affine affineFromValues(const std::array<double, kGradientCount> &values) {
    return {
        values[0], values[1], values[2],
        values[3], values[4], values[5],
    };
}

bool lexicographicTransformLess(const Affine &left, const Affine &right) {
    return affineValues(left) < affineValues(right);
}

bool betterCandidate(const Candidate &left, const Candidate &right) {
    if (!left.valid) {
        return false;
    }
    if (!right.valid) {
        return true;
    }
    if (std::abs(left.covered - right.covered) > kGeometryEpsilon) {
        return left.covered > right.covered;
    }
    if (left.shapeId != right.shapeId) {
        return left.shapeId < right.shapeId;
    }

    return lexicographicTransformLess(left.transform, right.transform);
}

Candidate legalCandidate(const ShapeMesh &shape,
                         Affine transform,
                         const Polygons &residual,
                         const Polygons &mayCover,
                         const FillOptions &options) {
    Candidate result;
    result.shapeId = shape.id;
    for (int step = 0; step <= kLegalShrinkSteps; ++step) {
        const AreaGradient evaluation =
            evaluateAreaGradient(shape, transform, residual, mayCover);
        if (finiteGradient(evaluation)
            && evaluation.spill <= options.epsSpill
            && evaluation.covered >= options.epsGain) {
            result.transform = transform;
            result.covered = evaluation.covered;
            result.spill = std::max(0.0, evaluation.spill);
            result.valid = true;

            return result;
        }
        transform.a *= kLegalShrinkFactor;
        transform.b *= kLegalShrinkFactor;
        transform.c *= kLegalShrinkFactor;
        transform.d *= kLegalShrinkFactor;
    }

    return result;
}

Candidate optimizeCandidate(const ShapeMesh &shape,
                            const Polygons &residual,
                            const Polygons &mayCover,
                            const FillOptions &options,
                            const DistanceSeed &seed,
                            std::mt19937_64 *random,
                            int restart,
                            const std::function<bool()> &cancelled) {
    std::uniform_real_distribution<double> unitDistribution(-1.0, 1.0);
    const double angleOffset = restart == 0
        ? 0.0 : unitDistribution(*random) * kRestartAngleRange;
    const double scaleFactor = restart == 0
        ? 1.0 : 1.0 + unitDistribution(*random) * kRestartScaleRange;
    const QPointF translationOffset = restart == 0
        ? QPointF()
        : QPointF(unitDistribution(*random), unitDistribution(*random))
            * (seed.radius * kRestartTranslationFraction);
    std::array<double, kGradientCount> values = affineValues(
        initialTransform(shape, seed, angleOffset, scaleFactor, translationOffset));
    std::array<double, kGradientCount> firstMoment{};
    std::array<double, kGradientCount> secondMoment{};
    Candidate best = legalCandidate(shape, affineFromValues(values),
                                    residual, mayCover, options);
    double beta1Power = 1.0;
    double beta2Power = 1.0;
    for (int iteration = 1; iteration <= options.adamIterations; ++iteration) {
        if (cancelled && cancelled()) {
            return {};
        }
        const Affine transform = affineFromValues(values);
        const AreaGradient evaluation =
            evaluateAreaGradient(shape, transform, residual, mayCover);
        if (!finiteGradient(evaluation)) {
            break;
        }

        std::array<double, kGradientCount> scoreGradient{};
        double gradientNormSquared = 0.0;
        for (int parameter = 0; parameter < kGradientCount; ++parameter) {
            scoreGradient[parameter] = evaluation.coveredGradient[parameter]
                - options.spillWeight * evaluation.spillGradient[parameter];
            gradientNormSquared += scoreGradient[parameter]
                * scoreGradient[parameter];
        }
        const double gradientNorm = std::sqrt(gradientNormSquared);
        if (gradientNorm <= kGradientStopNorm) {
            break;
        }
        if (gradientNorm > kGradientNormLimit) {
            const double factor = kGradientNormLimit / gradientNorm;
            for (double &gradient : scoreGradient) {
                gradient *= factor;
            }
        }

        beta1Power *= kAdamBeta1;
        beta2Power *= kAdamBeta2;
        for (int parameter = 0; parameter < kGradientCount; ++parameter) {
            firstMoment[parameter] = kAdamBeta1 * firstMoment[parameter]
                + (1.0 - kAdamBeta1) * scoreGradient[parameter];
            secondMoment[parameter] = kAdamBeta2 * secondMoment[parameter]
                + (1.0 - kAdamBeta2) * scoreGradient[parameter]
                    * scoreGradient[parameter];
            const double correctedFirst = firstMoment[parameter]
                / (1.0 - beta1Power);
            const double correctedSecond = secondMoment[parameter]
                / (1.0 - beta2Power);
            values[parameter] += options.adamLearningRate * correctedFirst
                / (std::sqrt(correctedSecond) + kAdamEpsilon);
        }
        const Candidate candidate = legalCandidate(
            shape, affineFromValues(values), residual, mayCover, options);
        if (betterCandidate(candidate, best)) {
            best = candidate;
        }
    }

    return best;
}

double descriptorAspect(const QRectF &bounds) {
    const double minimum = std::max(
        kGeometryEpsilon, std::min(bounds.width(), bounds.height()));
    const double maximum = std::max(bounds.width(), bounds.height());

    return maximum / minimum;
}

QVector<const ShapeMesh *> routedShapes(const Polygons &residual,
                                        const QVector<ShapeMesh> &catalog,
                                        bool useRouter) {
    QVector<const ShapeMesh *> result;
    for (const ShapeMesh &shape : catalog) {
        if (shape.valid()) {
            result.push_back(&shape);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const ShapeMesh *left, const ShapeMesh *right) {
                  return left->id < right->id;
              });
    if (!useRouter || result.size() <= kRouterCandidateCount) {
        return result;
    }

    const QRectF residualBounds = polygonBounds(residual);
    const double residualAspect = descriptorAspect(residualBounds);
    const double residualFill = polygonSetArea(residual)
        / std::max(kGeometryEpsilon,
                   residualBounds.width() * residualBounds.height());
    std::stable_sort(result.begin(), result.end(),
                     [residualAspect, residualFill](
                         const ShapeMesh *left, const ShapeMesh *right) {
                         const QRectF leftBounds = shapeBounds(*left);
                         const QRectF rightBounds = shapeBounds(*right);
                         const double leftFill = left->area
                             / std::max(kGeometryEpsilon,
                                        leftBounds.width() * leftBounds.height());
                         const double rightFill = right->area
                             / std::max(kGeometryEpsilon,
                                        rightBounds.width() * rightBounds.height());
                         const double leftDistance =
                             std::abs(std::log(descriptorAspect(leftBounds))
                                      - std::log(residualAspect))
                             + 2.0 * std::abs(leftFill - residualFill);
                         const double rightDistance =
                             std::abs(std::log(descriptorAspect(rightBounds))
                                      - std::log(residualAspect))
                             + 2.0 * std::abs(rightFill - residualFill);
                         if (std::abs(leftDistance - rightDistance)
                             > kGeometryEpsilon) {
                             return leftDistance < rightDistance;
                         }
                         return left->id < right->id;
                     });
    result.resize(kRouterCandidateCount);

    return result;
}

std::uint64_t derivedSeed(const Polygons &polygons) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const QPolygonF &polygon : polygons) {
        for (const QPointF &point : polygon) {
            const qint64 x = std::llround(point.x() * kClipperScale);
            const qint64 y = std::llround(point.y() * kClipperScale);
            result ^= static_cast<std::uint64_t>(x);
            result *= 1099511628211ULL;
            result ^= static_cast<std::uint64_t>(y);
            result *= 1099511628211ULL;
        }
    }

    return result;
}

bool validOptions(const FillOptions &options) {
    return options.budget > 0 && options.adamIterations > 0
        && options.restarts >= 0
        && std::isfinite(options.spillWeight) && options.spillWeight > 0.0
        && std::isfinite(options.epsArea) && options.epsArea >= 0.0
        && std::isfinite(options.epsGain) && options.epsGain > 0.0
        && std::isfinite(options.epsSpill) && options.epsSpill >= 0.0
        && std::isfinite(options.adamLearningRate)
        && options.adamLearningRate > 0.0;
}

} // namespace

QVector<ShapeMesh> buildShapeCatalog(const ShapeGeometryStore &geometry,
                                     QString *error) {
    QVector<ShapeMesh> result;
    result.reserve(kShapeIds.size());
    for (const int shapeId : kShapeIds) {
        const ShapeGeometry *source = geometry.shape(shapeId);
        if (source == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("shape %1 is unavailable").arg(shapeId);
            }
            return {};
        }
        ShapeMesh shape = buildShapeMesh(shapeId, *source);
        if (!shape.valid()) {
            if (error != nullptr) {
                *error = shape.error.isEmpty()
                    ? QStringLiteral("shape %1 is invalid").arg(shapeId)
                    : shape.error;
            }
            return {};
        }
        result.push_back(std::move(shape));
    }
    if (error != nullptr) {
        error->clear();
    }

    return result;
}

Polygons polygonsFromPainterPath(const QPainterPath &path) {
    Polygons result;
    const QList<QPolygonF> subpaths = path.toSubpathPolygons();
    result.reserve(subpaths.size());
    for (QPolygonF polygon : subpaths) {
        polygon = normalizedPolygon(std::move(polygon));
        if (!polygon.isEmpty()) {
            result.push_back(std::move(polygon));
        }
    }
    if (result.size() == 1 && signedArea(result.front()) < 0.0) {
        std::reverse(result.front().begin(), result.front().end());
    }

    return result;
}

QPainterPath painterPathFromPolygons(const Polygons &polygons) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    for (const QPolygonF &polygon : polygons) {
        result.addPolygon(polygon);
        result.closeSubpath();
    }

    return result;
}

AreaGradient evaluateAreaGradient(const ShapeMesh &shape,
                                  const Affine &transform,
                                  const Polygons &coveredSubject,
                                  const Polygons &legalSubject) {
    const Jet covered = intersectionArea(shape, transform, coveredSubject);
    const Jet legal = intersectionArea(shape, transform, legalSubject);
    const Jet spill = transformedShapeArea(shape, transform) - legal;
    AreaGradient result;
    result.covered = covered.value;
    result.spill = std::max(0.0, spill.value);
    result.coveredGradient = covered.gradient;
    result.spillGradient = spill.gradient;

    return result;
}

FillResult analyticCoverFill(
    const FillInput &input,
    const QVector<ShapeMesh> &catalog,
    const FillOptions &options,
    const std::function<bool()> &cancelled) {
    FillResult result;
    const Polygons mustCover = normalizedInputPolygons(input.mustCover);
    const Polygons mayCover = normalizedInputPolygons(input.mayCover);
    if (mustCover.isEmpty() || mayCover.isEmpty()) {
        result.error = QStringLiteral("Differentiable cover input is empty");
        return result;
    }
    if (!validOptions(options)) {
        result.error = QStringLiteral("Differentiable cover options are invalid");
        return result;
    }
    if (catalog.isEmpty()
        || std::any_of(catalog.cbegin(), catalog.cend(),
                       [](const ShapeMesh &shape) { return !shape.valid(); })) {
        result.error = QStringLiteral("Differentiable cover catalog is invalid");
        return result;
    }

    result.residual = mustCover;
    const double targetArea = polygonSetArea(mustCover);
    const std::uint64_t seedValue = options.seed == 0
        ? derivedSeed(mustCover) : options.seed;
    std::mt19937_64 random(seedValue);
    Polygons footprints;
    while (polygonSetArea(result.residual) > options.epsArea
           && result.placements.size() < options.budget) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.residualArea = polygonSetArea(result.residual);
            result.coveredArea = targetArea - result.residualArea;
            return result;
        }

        const DistanceSeed seed = distanceSeed(result.residual);
        const QVector<const ShapeMesh *> candidates =
            routedShapes(result.residual, catalog, options.useRouter);
        Candidate best;
        for (const ShapeMesh *shape : candidates) {
            for (int restart = 0; restart <= options.restarts; ++restart) {
                const Candidate candidate = optimizeCandidate(
                    *shape, result.residual, mayCover, options,
                    seed, &random, restart, cancelled);
                if (cancelled && cancelled()) {
                    result.cancelled = true;
                    result.residualArea = polygonSetArea(result.residual);
                    result.coveredArea = targetArea - result.residualArea;
                    return result;
                }
                if (betterCandidate(candidate, best)) {
                    best = candidate;
                }
            }
        }
        if (!best.valid) {
            result.stalled = true;
            break;
        }

        const Polygons footprint{transformedBoundary(
            *std::find_if(catalog.cbegin(), catalog.cend(),
                          [&best](const ShapeMesh &shape) {
                              return shape.id == best.shapeId;
                          }),
            best.transform)};
        Polygons nextResidual = differencePolygons(result.residual, footprint);
        const double previousArea = polygonSetArea(result.residual);
        const double nextArea = polygonSetArea(nextResidual);
        const double exactGain = previousArea - nextArea;
        if (!std::isfinite(exactGain) || exactGain < options.epsGain) {
            result.stalled = true;
            break;
        }

        result.placements.push_back({
            best.transform,
            best.shapeId,
            exactGain,
        });
        footprints += footprint;
        result.residual = std::move(nextResidual);
    }

    result.residualArea = polygonSetArea(result.residual);
    result.coveredArea = std::max(0.0, targetArea - result.residualArea);
    result.budgetHit = result.placements.size() >= options.budget
        && result.residualArea > options.epsArea;
    if (!footprints.isEmpty()) {
        const Polygons coverage = unionPolygons(footprints);
        result.outsideArea = polygonSetArea(differencePolygons(coverage, mayCover));
    }

    return result;
}

QTransform toQTransform(const Affine &transform) {
    return QTransform(transform.a, transform.b,
                      transform.c, transform.d,
                      transform.e, transform.f);
}

} // namespace gui::cover
