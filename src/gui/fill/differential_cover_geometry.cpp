#include "differential_cover_internal.h"

#include <clipper2/clipper.engine.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace gui::cover {

QStringList differentialShapeAssetPaths() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(appDir).filePath(QStringLiteral("assets/differential_shapes.json")),
        QDir(cwd).filePath(QStringLiteral("assets/differential_shapes.json")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/differential_shapes.json")),
    };
}

QVector<int> loadDifferentialShapeIdsFromFile(const QString &path,
                                              QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not open differential shape catalog: %1")
                         .arg(path);
        }
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid differential shape catalog: %1")
                         .arg(parseError.errorString());
        }
        return {};
    }
    if (!document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "differential shape catalog root must be an object");
        }
        return {};
    }

    const QJsonValue shapeIdsValue =
        document.object().value(QStringLiteral("shape_ids"));
    if (!shapeIdsValue.isArray() || shapeIdsValue.toArray().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "differential shape catalog must contain a non-empty shape_ids array");
        }
        return {};
    }

    const QJsonArray shapeIdsArray = shapeIdsValue.toArray();
    QVector<int> result;
    QSet<int> seenShapeIds;
    result.reserve(shapeIdsArray.size());
    seenShapeIds.reserve(shapeIdsArray.size());
    for (qsizetype index = 0; index < shapeIdsArray.size(); ++index) {
        const QJsonValue value = shapeIdsArray.at(index);
        const int shapeId = value.toInt(-1);
        const double numericShapeId = value.toDouble(-1.0);
        if (!value.isDouble()
            || numericShapeId != static_cast<double>(shapeId)
            || shapeId <= 0
            || shapeId > kMaximumShapeId) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "differential shape catalog entry %1 is not a valid shape id")
                             .arg(index);
            }
            return {};
        }
        if (seenShapeIds.contains(shapeId)) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "differential shape catalog contains duplicate shape id %1")
                             .arg(shapeId);
            }
            return {};
        }
        seenShapeIds.insert(shapeId);
        result.push_back(shapeId);
    }
    if (error != nullptr) {
        error->clear();
    }

    return result;
}

QVector<int> loadDifferentialShapeIds(QString *error) {
    for (const QString &path : differentialShapeAssetPaths()) {
        if (QFile::exists(path)) {
            return loadDifferentialShapeIdsFromFile(path, error);
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("differential_shapes.json was not found");
    }

    return {};
}
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

void orientPolygonSet(Polygons *polygons) {
    double signedSetArea = 0.0;
    for (const QPolygonF &polygon : *polygons) {
        signedSetArea += signedArea(polygon);
    }
    if (signedSetArea < 0.0) {
        for (QPolygonF &polygon : *polygons) {
            std::reverse(polygon.begin(), polygon.end());
        }
    }
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

QVector<QRectF> individualPolygonBounds(const Polygons &polygons) {
    QVector<QRectF> result;
    result.reserve(polygons.size());
    for (const QPolygonF &polygon : polygons) {
        result.push_back(polygon.boundingRect());
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

void clipHalfPlane(const QVector<JetPoint> &subject,
                   const JetPoint &windowStart,
                   const JetPoint &windowEnd,
                   QVector<JetPoint> *result) {
    result->clear();
    if (subject.isEmpty()) {
        return;
    }
    result->reserve(subject.size() + 2);

    JetPoint start = subject.back();
    Jet startSide = sideValue(start, windowStart, windowEnd);
    bool startInside = startSide.value >= -kGeometryEpsilon;
    for (const JetPoint &end : subject) {
        const Jet endSide = sideValue(end, windowStart, windowEnd);
        const bool endInside = endSide.value >= -kGeometryEpsilon;
        if (endInside != startInside) {
            result->push_back(intersectionPoint(start, end, startSide, endSide));
        }
        if (endInside) {
            result->push_back(end);
        }
        start = end;
        startSide = endSide;
        startInside = endInside;
    }
}

Jet clippedPolygonArea(const QPolygonF &subject,
                       std::array<JetPoint, 3> window) {
    QVector<JetPoint> clipped;
    clipped.reserve(subject.size() + 3);
    for (const QPointF &point : subject) {
        clipped.push_back(constantPoint(point));
    }
    QVector<JetPoint> scratch;
    scratch.reserve(subject.size() + 6);

    const Jet orientation = cross(window[1] - window[0],
                                  window[2] - window[0]);
    if (orientation.value < 0.0) {
        std::swap(window[1], window[2]);
    }
    for (int edge = 0; edge < 3 && !clipped.isEmpty(); ++edge) {
        clipHalfPlane(clipped, window[edge], window[(edge + 1) % 3], &scratch);
        clipped.swap(scratch);
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

Jet clippedConvexPolygonArea(const QPolygonF &subject,
                             const QVector<JetPoint> &window) {
    QVector<JetPoint> clipped;
    clipped.reserve(subject.size() + window.size());
    for (const QPointF &point : subject) {
        clipped.push_back(constantPoint(point));
    }
    QVector<JetPoint> scratch;
    scratch.reserve(subject.size() + window.size() * 2);

    Jet windowArea;
    for (int i = 0; i < window.size(); ++i) {
        windowArea = windowArea
            + cross(window[i], window[(i + 1) % window.size()]);
    }
    const bool reverse = windowArea.value < 0.0;
    for (int edge = 0; edge < window.size() && !clipped.isEmpty(); ++edge) {
        const int start = reverse ? (window.size() - edge) % window.size() : edge;
        const int end = reverse
            ? (window.size() - edge - 1 + window.size()) % window.size()
            : (edge + 1) % window.size();
        clipHalfPlane(clipped, window[start], window[end], &scratch);
        clipped.swap(scratch);
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

QRectF jetWindowBounds(const QVector<JetPoint> &window) {
    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const JetPoint &point : window) {
        minimumX = std::min(minimumX, point.x.value);
        minimumY = std::min(minimumY, point.y.value);
        maximumX = std::max(maximumX, point.x.value);
        maximumY = std::max(maximumY, point.y.value);
    }

    return QRectF(QPointF(minimumX, minimumY),
                  QPointF(maximumX, maximumY));
}

IntersectionJets intersectionAreas(const ShapeMesh &shape,
                                   const Affine &transform,
                                   const Polygons &coveredSubject,
                                   const Polygons &legalSubject,
                                   const EvaluationBounds &subjectBounds) {
    IntersectionJets result;
    if (shape.convex) {
        QVector<JetPoint> window;
        window.reserve(shape.boundary.size());
        for (const Vec2 &point : shape.boundary) {
            window.push_back(transformedVertex(point, transform));
        }
        const QRectF windowBounds = jetWindowBounds(window);
        for (int i = 0; i < coveredSubject.size(); ++i) {
            if (subjectBounds.covered[i].intersects(windowBounds)) {
                result.covered = result.covered
                    + clippedConvexPolygonArea(coveredSubject[i], window);
            }
        }
        for (int i = 0; i < legalSubject.size(); ++i) {
            if (subjectBounds.legal[i].intersects(windowBounds)) {
                result.legal = result.legal
                    + clippedConvexPolygonArea(legalSubject[i], window);
            }
        }

        return result;
    }
    for (const std::array<int, 3> &triangle : shape.triangles) {
        std::array<JetPoint, 3> window = {
            transformedVertex(shape.vertices[triangle[0]], transform),
            transformedVertex(shape.vertices[triangle[1]], transform),
            transformedVertex(shape.vertices[triangle[2]], transform),
        };
        const double minimumX = std::min({
            window[0].x.value, window[1].x.value, window[2].x.value,
        });
        const double minimumY = std::min({
            window[0].y.value, window[1].y.value, window[2].y.value,
        });
        const double maximumX = std::max({
            window[0].x.value, window[1].x.value, window[2].x.value,
        });
        const double maximumY = std::max({
            window[0].y.value, window[1].y.value, window[2].y.value,
        });
        const QRectF windowBounds(
            QPointF(minimumX, minimumY), QPointF(maximumX, maximumY));
        for (int i = 0; i < coveredSubject.size(); ++i) {
            if (!subjectBounds.covered[i].intersects(windowBounds)) {
                continue;
            }
            result.covered =
                result.covered + clippedPolygonArea(coveredSubject[i], window);
        }
        for (int i = 0; i < legalSubject.size(); ++i) {
            if (!subjectBounds.legal[i].intersects(windowBounds)) {
                continue;
            }
            result.legal =
                result.legal + clippedPolygonArea(legalSubject[i], window);
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

AreaGradient areaGradient(const ShapeMesh &shape,
                          const Affine &transform,
                          const Polygons &coveredSubject,
                          const Polygons &legalSubject,
                          const EvaluationBounds &subjectBounds) {
    const IntersectionJets intersections =
        intersectionAreas(shape, transform, coveredSubject, legalSubject,
                          subjectBounds);
    const Jet spill =
        transformedShapeArea(shape, transform) - intersections.legal;
    AreaGradient result;
    result.covered = intersections.covered.value;
    result.spill = std::max(0.0, spill.value);
    result.coveredGradient = intersections.covered.gradient;
    result.spillGradient = spill.gradient;

    return result;
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
    if (signedArea(result) < 0.0) {
        std::reverse(result.begin(), result.end());
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

Polygons intersectionPolygons(const Polygons &subjects,
                              const Polygons &clips) {
    return booleanOperation(
        subjects, clips, Clipper2Lib::ClipType::Intersection);
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

Vec2 normalizedVector(const Vec2 &vector) {
    const double length = std::hypot(vector.x, vector.y);
    if (length <= kGeometryEpsilon) {
        return {};
    }

    return {vector.x / length, vector.y / length};
}

double vectorAngle(const Vec2 &left, const Vec2 &right) {
    const double cosine = std::clamp(
        left.x * right.x + left.y * right.y,
        -1.0, 1.0);

    return std::acos(cosine);
}

QVector<ShapeFeature> shapeFeatures(
    const QVector<Vec2> &boundary) {
    QVector<ShapeFeature> result;
    if (boundary.size() < 3) {
        return result;
    }

    QVector<double> cumulativeLength(
        boundary.size() + 1, 0.0);
    for (int index = 0; index < boundary.size(); ++index) {
        const Vec2 &left = boundary[index];
        const Vec2 &right =
            boundary[(index + 1) % boundary.size()];
        cumulativeLength[index + 1] =
            cumulativeLength[index]
            + std::hypot(
                right.x - left.x,
                right.y - left.y);
    }
    const double perimeter = cumulativeLength.back();
    if (perimeter <= kGeometryEpsilon) {
        return result;
    }

    QSet<int> retainedIndices;
    for (int index = 0; index < boundary.size(); ++index) {
        const Vec2 incoming = normalizedVector({
            boundary[index].x
                - boundary[
                    (index + boundary.size() - 1)
                    % boundary.size()].x,
            boundary[index].y
                - boundary[
                    (index + boundary.size() - 1)
                    % boundary.size()].y,
        });
        const Vec2 outgoing = normalizedVector({
            boundary[(index + 1) % boundary.size()].x
                - boundary[index].x,
            boundary[(index + 1) % boundary.size()].y
                - boundary[index].y,
        });
        if (vectorAngle(incoming, outgoing)
            >= kShapeCornerSalience) {
            retainedIndices.insert(index);
        }
    }
    for (int sample = 0;
         sample < kCatalogSmoothFeatureCount; ++sample) {
        const double targetLength =
            perimeter
            * static_cast<double>(sample)
            / static_cast<double>(
                kCatalogSmoothFeatureCount);
        const auto found = std::upper_bound(
            cumulativeLength.cbegin(),
            cumulativeLength.cend(),
            targetLength);
        const int index = std::clamp(
            static_cast<int>(
                found - cumulativeLength.cbegin())
                - 1,
            0,
            static_cast<int>(
                boundary.size()) - 1);
        retainedIndices.insert(index);
    }

    QVector<int> orderedIndices =
        retainedIndices.values();
    std::sort(
        orderedIndices.begin(),
        orderedIndices.end());
    result.reserve(orderedIndices.size());
    for (const int index : orderedIndices) {
        const int previous =
            (index + boundary.size() - 1)
            % boundary.size();
        const int next =
            (index + 1) % boundary.size();
        const Vec2 incoming = normalizedVector({
            boundary[index].x - boundary[previous].x,
            boundary[index].y - boundary[previous].y,
        });
        const Vec2 outgoing = normalizedVector({
            boundary[next].x - boundary[index].x,
            boundary[next].y - boundary[index].y,
        });
        result.push_back({
            boundary[index],
            incoming,
            outgoing,
            cumulativeLength[index] / perimeter,
            index,
            vectorAngle(incoming, outgoing)
                    >= kShapeCornerSalience
                ? ContourFeatureKind::Corner
                : ContourFeatureKind::SmoothJunction,
        });
    }

    return result;
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

    QVector<QVector<Vec2>> boundaryLoops;
    QVector<bool> visited(result.vertices.size(), false);
    for (int start = 0; start < result.vertices.size(); ++start) {
        if (visited[start]) {
            continue;
        }
        QVector<Vec2> loop;
        int previous = -1;
        int current = start;
        do {
            if (visited[current]) {
                result.error = QStringLiteral(
                    "shape %1 boundary revisits a vertex").arg(shapeId);
                return result;
            }
            visited[current] = true;
            loop.push_back(result.vertices[current]);
            const QVector<int> &neighbors = adjacency[current];
            const int next = neighbors[0] == previous
                ? neighbors[1] : neighbors[0];
            previous = current;
            current = next;
        } while (current != start);
        boundaryLoops.push_back(std::move(loop));
    }
    for (int loopIndex = 0; loopIndex < boundaryLoops.size(); ++loopIndex) {
        const Vec2 probe = boundaryLoops[loopIndex].front();
        int containmentDepth = 0;
        for (int otherIndex = 0; otherIndex < boundaryLoops.size();
             ++otherIndex) {
            if (otherIndex == loopIndex) {
                continue;
            }
            QPolygonF polygon;
            polygon.reserve(boundaryLoops[otherIndex].size());
            for (const Vec2 &point : boundaryLoops[otherIndex]) {
                polygon.push_back(QPointF(point.x, point.y));
            }
            containmentDepth += polygon.containsPoint(
                QPointF(probe.x, probe.y), Qt::OddEvenFill);
        }
        const bool shouldBePositive = containmentDepth % 2 == 0;
        if ((signedArea(boundaryLoops[loopIndex]) > 0.0)
            != shouldBePositive) {
            std::reverse(boundaryLoops[loopIndex].begin(),
                         boundaryLoops[loopIndex].end());
        }
    }
    std::sort(boundaryLoops.begin(), boundaryLoops.end(),
              [](const QVector<Vec2> &left, const QVector<Vec2> &right) {
        return std::abs(signedArea(left)) > std::abs(signedArea(right));
    });
    result.boundary = boundaryLoops.front();
    for (int loopIndex = 1; loopIndex < boundaryLoops.size(); ++loopIndex) {
        const QVector<Vec2> &loop = boundaryLoops[loopIndex];
        int boundaryBridge = 0;
        int loopBridge = 0;
        double bridgeDistance = std::numeric_limits<double>::max();
        for (int boundaryIndex = 0;
             boundaryIndex < result.boundary.size(); ++boundaryIndex) {
            for (int candidateIndex = 0;
                 candidateIndex < loop.size(); ++candidateIndex) {
                const double dx = result.boundary[boundaryIndex].x
                    - loop[candidateIndex].x;
                const double dy = result.boundary[boundaryIndex].y
                    - loop[candidateIndex].y;
                const double distance = dx * dx + dy * dy;
                if (distance < bridgeDistance) {
                    bridgeDistance = distance;
                    boundaryBridge = boundaryIndex;
                    loopBridge = candidateIndex;
                }
            }
        }
        QVector<Vec2> stitched;
        stitched.reserve(result.boundary.size() + loop.size() + 2);
        for (int index = 0; index <= boundaryBridge; ++index) {
            stitched.push_back(result.boundary[index]);
        }
        stitched.push_back(loop[loopBridge]);
        for (int offset = 1; offset < loop.size(); ++offset) {
            stitched.push_back(loop[(loopBridge + offset) % loop.size()]);
        }
        stitched.push_back(loop[loopBridge]);
        stitched.push_back(result.boundary[boundaryBridge]);
        for (int index = boundaryBridge + 1;
             index < result.boundary.size(); ++index) {
            stitched.push_back(result.boundary[index]);
        }
        result.boundary = std::move(stitched);
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
    double boundaryArea = 0.0;
    for (const QVector<Vec2> &loop : std::as_const(boundaryLoops)) {
        boundaryArea += signedArea(loop);
    }
    boundaryArea = std::abs(boundaryArea);
    if (std::abs(triangleArea - boundaryArea)
        > std::max(1.0, boundaryArea) * 1e-8) {
        result.error = QStringLiteral("shape %1 triangles overlap or leave gaps")
                           .arg(shapeId);
        return result;
    }
    result.area = boundaryArea;
    result.convex = boundaryLoops.size() == 1
        && boundaryIsConvex(result.boundary);
    result.features =
        shapeFeatures(boundaryLoops.front());
    if (result.features.isEmpty()) {
        result.error =
            QStringLiteral("shape %1 has no salient boundary features")
                .arg(shapeId);
    }

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
    orientPolygonSet(&result);

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
QVector<ShapeMesh> buildShapeCatalog(const ShapeGeometryStore &geometry,
                                     QString *error) {
    const QVector<int> shapeIds = loadDifferentialShapeIds(error);
    if (shapeIds.isEmpty()) {
        return {};
    }

    QVector<ShapeMesh> result;
    result.reserve(shapeIds.size());
    for (const int shapeId : shapeIds) {
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

namespace {

struct PathFlatteningTransforms {
    QTransform forward;
    QTransform inverse;
};

PathFlatteningTransforms pathFlatteningTransforms(
    double flatnessTolerance) {
    constexpr double kPainterCurveThreshold = 0.25;
    constexpr double kMaximumFlatteningScale = 4096.0;
    PathFlatteningTransforms result;
    if (std::isfinite(flatnessTolerance)
        && flatnessTolerance > 0.0) {
        const double scale = std::clamp(
            kPainterCurveThreshold / flatnessTolerance,
            1.0, kMaximumFlatteningScale);
        result.forward.scale(scale, scale);
        result.inverse.scale(1.0 / scale, 1.0 / scale);
    }

    return result;
}

Polygons normalizedPathPolygons(
    const QList<QPolygonF> &pathPolygons,
    const QTransform &inverseTransform) {
    Polygons result;
    result.reserve(pathPolygons.size());
    for (QPolygonF polygon : pathPolygons) {
        polygon = inverseTransform.map(polygon);
        polygon = normalizedPolygon(std::move(polygon));
        if (!polygon.isEmpty()) {
            result.push_back(std::move(polygon));
        }
    }
    orientPolygonSet(&result);

    return result;
}

} // namespace

Polygons polygonsFromPainterPath(
    const QPainterPath &path,
    double flatnessTolerance) {
    const PathFlatteningTransforms transforms =
        pathFlatteningTransforms(flatnessTolerance);

    return normalizedPathPolygons(
        path.toSubpathPolygons(transforms.forward),
        transforms.inverse);
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
    const EvaluationBounds subjectBounds{
        individualPolygonBounds(coveredSubject),
        individualPolygonBounds(legalSubject),
    };

    return areaGradient(shape, transform, coveredSubject, legalSubject,
                        subjectBounds);
}
QTransform toQTransform(const Affine &transform) {
    return QTransform(transform.a, transform.b,
                      transform.c, transform.d,
                      transform.e, transform.f);
}

QPointF normalizedPoint(const QPointF &vector) {
    const double length =
        std::hypot(vector.x(), vector.y());
    if (length <= kGeometryEpsilon) {
        return {};
    }

    return vector / length;
}

double contourSpanLength(const ContourSpan &span) {
    constexpr int kCurveSamples = 8;
    if (!span.curved) {
        return QLineF(span.start, span.end).length();
    }

    double result = 0.0;
    QPointF previous = span.start;
    for (int sample = 1;
         sample <= kCurveSamples; ++sample) {
        const double parameter =
            static_cast<double>(sample)
            / static_cast<double>(kCurveSamples);
        const double inverse = 1.0 - parameter;
        const QPointF point =
            span.start * (inverse * inverse)
            + span.control
                * (2.0 * inverse * parameter)
            + span.end * (parameter * parameter);
        result += QLineF(previous, point).length();
        previous = point;
    }

    return result;
}

QVector<ContourFeature> extractContourFeatures(
    const QVector<ContourSpan> &spans,
    double boundaryTolerance) {
    QVector<ContourFeature> result;
    if (spans.size() < 2
        || !std::isfinite(boundaryTolerance)
        || boundaryTolerance <= 0.0) {
        return result;
    }

    QVector<double> lengths;
    lengths.reserve(spans.size());
    double perimeter = 0.0;
    for (const ContourSpan &span : spans) {
        const double length =
            contourSpanLength(span);
        lengths.push_back(length);
        perimeter += length;
    }
    if (perimeter <= kGeometryEpsilon) {
        return result;
    }

    result.reserve(spans.size());
    double totalWeight = 0.0;
    for (int index = 0;
         index < spans.size(); ++index) {
        const ContourSpan &previous =
            spans[
                (index + spans.size() - 1)
                % spans.size()];
        const ContourSpan &next = spans[index];
        const QPointF incoming =
            normalizedPoint(
                previous.end
                - (previous.curved
                       ? previous.control
                       : previous.start));
        const QPointF outgoing =
            normalizedPoint(
                (next.curved
                     ? next.control
                     : next.end)
                - next.start);
        const double angle = std::acos(
            std::clamp(
                QPointF::dotProduct(
                    incoming, outgoing),
                -1.0, 1.0));
        const double localScale =
            (lengths[
                 (index + lengths.size() - 1)
                 % lengths.size()]
             + lengths[index])
            * 0.5;
        const double maximumRadius =
            std::max(
                boundaryTolerance,
                std::min(
                    boundaryTolerance * 4.0,
                    localScale * 0.25));
        const double captureRadius =
            std::clamp(
                localScale * 0.1,
                boundaryTolerance,
                maximumRadius);
        const double weight =
            localScale / perimeter;
        result.push_back({
            next.start,
            incoming,
            outgoing,
            captureRadius,
            weight,
            index,
            angle >= kCornerAngleTolerance
                ? ContourFeatureKind::Corner
                : ContourFeatureKind::SmoothJunction,
        });
        totalWeight += weight;
    }
    if (totalWeight > kGeometryEpsilon) {
        for (ContourFeature &feature : result) {
            feature.weight /= totalWeight;
        }
    }

    return result;
}

Polygons expandedCoverEnvelope(
    const Polygons &target,
    double distance,
    double flatnessTolerance) {
    if (!std::isfinite(distance)
        || distance <= 0.0) {
        return normalizedInputPolygons(target);
    }
    const QPainterPath targetPath =
        painterPathFromPolygons(target);
    QPainterPathStroker stroker;
    stroker.setWidth(distance * 2.0);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);
    const QPainterPath expanded =
        targetPath.united(
            stroker.createStroke(
                targetPath));

    return unionPolygons(
        polygonsFromPainterPath(
            expanded,
            flatnessTolerance));
}

Polygons expandedCoverEnvelope(
    const QPainterPath &target,
    double distance,
    double flatnessTolerance) {
    if (!std::isfinite(distance)
        || distance <= 0.0) {
        return polygonsFromPainterPath(
            target, flatnessTolerance);
    }
    QPainterPath expanded = target;
    expanded.setFillRule(Qt::WindingFill);
    QPainterPathStroker stroker;
    stroker.setWidth(distance * 2.0);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);
    expanded.addPath(
        stroker.createStroke(target));
    const PathFlatteningTransforms transforms =
        pathFlatteningTransforms(flatnessTolerance);

    return unionPolygons(
        normalizedPathPolygons(
            expanded.toFillPolygons(
                transforms.forward),
            transforms.inverse));
}

} // namespace gui::cover
