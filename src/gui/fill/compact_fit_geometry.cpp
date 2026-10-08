#include "compact_fit_catalog_internal.h"

#include "layer.h"
#include "matrix_math.h"
#include "polygon_mesh.h"

#include <clipper2/clipper.engine.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace gui::catalog {
namespace {

constexpr int kMaximumSubdivisionDepth = 24;
constexpr int kMaximumBoundarySegments = 12000;
constexpr int kMeshInflationAttempts = 16;
constexpr int kMaximumCompletionDepth = 12;
constexpr int kMaximumCompletionWork = 12000;
constexpr double kTransformZeroThreshold = 1e-12;
constexpr int kPreparedOperandLimit = 16;
constexpr int kPreparedOperandMinimumPoints = 64;
constexpr int kContainmentBands = 256;

double cross(const QPointF &left, const QPointF &right) {
    return left.x() * right.y() - left.y() * right.x();
}

Polygons polygonsFromPath(const QPainterPath &path) {
    Polygons result;
    for (QPolygonF polygon : path.toSubpathPolygons()) {
        if (polygon.size() >= 3) {
            result.push_back(std::move(polygon));
        }
    }

    return unite(result);
}

Clipper2Lib::Paths64 integerPaths(const Polygons &polygons) {
    Clipper2Lib::Paths64 result;
    result.reserve(polygons.size());
    for (const QPolygonF &polygon : polygons) {
        Clipper2Lib::Path64 path;
        path.reserve(polygon.size());
        for (const QPointF &point : polygon) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())
                || std::abs(point.x()) > kMaximumCoordinate
                || std::abs(point.y()) > kMaximumCoordinate) {
                throw std::runtime_error("Catalog cover coordinates exceed the geometry range");
            }
            path.emplace_back(std::llround(point.x() * kCoordinateScale),
                              std::llround(point.y() * kCoordinateScale));
        }
        if (path.size() >= 3) {
            result.push_back(std::move(path));
        }
    }

    return result;
}

struct PreparedOperand {
    Polygons polygons;
    Clipper2Lib::ReuseableDataContainer64 data;
    Clipper2Lib::PathType type;
};

std::shared_ptr<const PreparedOperand> preparedOperand(const Polygons &polygons,
                                                      Clipper2Lib::PathType type) {
    thread_local QVector<std::shared_ptr<const PreparedOperand>> cache;
    int points = 0;

    for (int index = 0; index < cache.size(); ++index) {
        const auto &entry = cache[index];
        if (entry->type == type && entry->polygons.constData() == polygons.constData()
            && entry->polygons.size() == polygons.size()) {
            auto result = cache.takeAt(index);
            cache.push_front(result);

            return result;
        }
    }
    for (const auto &polygon : polygons)
        points += polygon.size();
    if (points < kPreparedOperandMinimumPoints)
        return {};
    auto result = std::make_shared<PreparedOperand>();
    result->polygons = polygons;
    result->type = type;
    result->data.AddPaths(integerPaths(polygons), type, false);
    if (cache.size() >= kPreparedOperandLimit)
        cache.pop_back();
    cache.push_front(result);

    return result;
}

Polygons booleanOperation(const Polygons &subject, const Polygons &clip,
                          Clipper2Lib::ClipType operation) {
    const auto preparedSubject = preparedOperand(subject, Clipper2Lib::PathType::Subject);
    const auto preparedClip = preparedOperand(clip, Clipper2Lib::PathType::Clip);
    Clipper2Lib::Clipper64 engine;
    Clipper2Lib::Paths64 output;
    Polygons result;
    engine.PreserveCollinear(false);
    if (preparedSubject)
        engine.AddReuseableData(preparedSubject->data);
    else
        engine.AddSubject(integerPaths(subject));
    if (preparedClip)
        engine.AddReuseableData(preparedClip->data);
    else
        engine.AddClip(integerPaths(clip));
    if (!engine.Execute(operation, Clipper2Lib::FillRule::NonZero, output)) {
        throw std::runtime_error("Catalog cover polygon operation failed");
    }
    for (const Clipper2Lib::Path64 &path : output) {
        QPolygonF polygon;
        polygon.reserve(path.size());
        for (const auto &point : path) {
            polygon.push_back(QPointF(point.x / kCoordinateScale,
                                     point.y / kCoordinateScale));
        }
        if (polygon.size() >= 3) {
            const auto first = std::min_element(
                polygon.begin(), polygon.end(), [](const QPointF &left, const QPointF &right) {
                    return left.x() == right.x() ? left.y() < right.y() : left.x() < right.x();
                });
            std::rotate(polygon.begin(), first, polygon.end());
            result.push_back(std::move(polygon));
        }
    }
    std::sort(result.begin(), result.end(), [](const QPolygonF &left, const QPolygonF &right) {
        const double leftArea = signedArea(left);
        const double rightArea = signedArea(right);
        if (leftArea != rightArea) {
            return leftArea > rightArea;
        }
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
            [](const QPointF &a, const QPointF &b) {
                return a.x() == b.x() ? a.y() < b.y() : a.x() < b.x();
            });
    });

    return result;
}

void appendSegment(const PenBoundarySegment &segment, double tolerance,
                   QPolygonF *chords, Polygons *hulls, int depth,
                   const std::function<bool()> &cancelled) {
    if (cancelled && cancelled()) {
        return;
    }
    if (chords->size() >= kMaximumBoundarySegments || depth > kMaximumSubdivisionDepth) {
        throw std::runtime_error("Catalog cover boundary refinement exceeds its work limit");
    }
    if (!segment.curved || segment.flatness() <= tolerance) {
        chords->push_back(segment.start);
        if (segment.curved) {
            const QPolygonF hull =
                convexHull({segment.start, segment.control, segment.control2, segment.end});
            if (hull.size() >= 3)
                hulls->push_back(hull);
        }
        return;
    }
    const auto [left, right] = segment.split();
    appendSegment(left, tolerance, chords, hulls, depth + 1, cancelled);
    appendSegment(right, tolerance, chords, hulls, depth + 1, cancelled);
}

double contourArea(const QVector<PenBoundarySegment> &segments) {
    double area = 0;
    for (const auto &segment : segments)
        area += segment.signedArea();
    return std::abs(area);
}

const PenPrimitive *findShape(const QVector<Primitive> &primitives, int shapeId) {
    const auto found = std::find_if(primitives.begin(), primitives.end(),
        [shapeId](const Primitive &primitive) { return primitive.shape.shapeId == shapeId; });

    return found == primitives.end() ? nullptr : &found->shape;
}

QVector<Polygons> connectedRegions(const Polygons &polygons) {
    QVector<Polygons> result;
    for (const QPolygonF &polygon : polygons) {
        if (signedArea(polygon) > 0.0) {
            result.push_back({polygon});
        }
    }
    for (const QPolygonF &polygon : polygons) {
        if (signedArea(polygon) >= 0.0) {
            continue;
        }
        int owner = -1;
        double ownerArea = std::numeric_limits<double>::infinity();
        for (int index = 0; index < result.size(); ++index) {
            const QPolygonF &outer = result[index].front();
            const double outerArea = signedArea(outer);
            if (outer.containsPoint(polygon.front(), Qt::OddEvenFill) && outerArea < ownerArea) {
                owner = index;
                ownerArea = outerArea;
            }
        }
        if (owner < 0) {
            throw std::runtime_error("Catalog cover could not assign an interior boundary");
        }
        result[owner].push_back(polygon);
    }

    return result;
}

bool acceptCompletion(const PenPrimitive &shape, const QTransform &transform,
                      const Polygons &required, const Region &region, Candidate *candidate) {
    candidate->placement.shapeId = shape.shapeId;
    candidate->placement.transform = emittedTransform(transform);
    candidate->polygons = mapped(shape, candidate->placement.transform);
    if (candidate->polygons.isEmpty() || !subtract(required, candidate->polygons).isEmpty()
        || !subtract(expanded(candidate->polygons, kVerificationClearance), region.permitted).isEmpty()) {
        return false;
    }
    candidate->path = painterPath(candidate->polygons);
    candidate->bounds = candidate->path.boundingRect();
    candidate->gain = area(intersect(candidate->polygons, region.required));
    candidate->spill = area(subtract(candidate->polygons, region.required));

    return true;
}

int longestEdge(const QPolygonF &target) {
    int result = 0;
    double longest = 0.0;
    for (int index = 0; index < target.size(); ++index) {
        const QPointF edge = target[(index + 1) % target.size()] - target[index];
        const double length = QPointF::dotProduct(edge, edge);
        if (length > longest) {
            result = index;
            longest = length;
        }
    }

    return result;
}

bool completeWithTriangle(const QPolygonF &target, const Polygons &required,
                          const PenPrimitive &triangle, const Region &region,
                          const std::function<bool()> &cancelled, Candidate *candidate) {
    const QPolygonF &contour = triangle.contours.front();
    const std::array<QPointF, 3> source = {contour[0], contour[1], contour[2]};
    const QPointF center = (target[0] + target[1] + target[2]) / 3.0;
    const int edge = longestEdge(target);
    const double length = QLineF(target[edge], target[(edge + 1) % target.size()]).length();
    const double clearanceScale = 3.0 * length / (2.0 * std::abs(signedArea(target)));
    std::array<int, 3> order = {0, 1, 2};

    if (length <= 0.0 || !std::isfinite(clearanceScale)) {
        return false;
    }
    do {
        double clearance = 0.0;
        for (int attempt = 0; attempt < kMeshInflationAttempts; ++attempt) {
            if (cancelled && cancelled()) {
                return false;
            }
            const double inflation = clearance * clearanceScale;
            if (inflation * length > region.tolerance * 2.0) {
                break;
            }
            std::array<QPointF, 3> anchors;
            for (int index = 0; index < anchors.size(); ++index) {
                anchors[index] = center + (target[order[index]] - center) * (1.0 + inflation);
            }
            if (acceptCompletion(triangle, affineFromAnchors(source, anchors), required,
                                 region, candidate)) {
                return true;
            }
            clearance = clearance == 0.0 ? kVerificationClearance * std::sqrt(2.0) : clearance * 2.0;
        }
    } while (std::next_permutation(order.begin(), order.end()));

    return false;
}

bool completeWithRectangle(const QPolygonF &target, const Polygons &required,
                           const PenPrimitive *square, const Region &region,
                           const std::function<bool()> &cancelled, Candidate *candidate) {
    if (square == nullptr) {
        return false;
    }
    const int edge = longestEdge(target);
    const QPointF origin = target[edge];
    const QPointF along = target[(edge + 1) % target.size()] - origin;
    const double length = std::hypot(along.x(), along.y());
    if (length <= 0.0) {
        return false;
    }
    const QPointF horizontal = along / length;
    const QPointF vertical(-horizontal.y(), horizontal.x());
    const std::array<QPointF, 3> source = {
        square->bounds.topLeft(), square->bounds.topRight(), square->bounds.bottomLeft(),
    };
    double minimumX = 0.0;
    double maximumX = 0.0;
    double minimumY = 0.0;
    double maximumY = 0.0;
    for (const QPointF &point : target) {
        const QPointF relative = point - origin;
        const double x = QPointF::dotProduct(relative, horizontal);
        const double y = QPointF::dotProduct(relative, vertical);
        minimumX = std::min(minimumX, x);
        maximumX = std::max(maximumX, x);
        minimumY = std::min(minimumY, y);
        maximumY = std::max(maximumY, y);
    }
    double clearance = kVerificationClearance * std::sqrt(2.0);
    for (int attempt = 0; attempt < kMeshInflationAttempts && clearance < region.tolerance;
         ++attempt, clearance *= 2.0) {
        if (cancelled && cancelled()) {
            return false;
        }
        const QPointF corner = origin + horizontal * (minimumX - clearance)
            + vertical * (minimumY - clearance);
        const std::array<QPointF, 3> anchors = {
            corner,
            corner + horizontal * (maximumX - minimumX + 2.0 * clearance),
            corner + vertical * (maximumY - minimumY + 2.0 * clearance),
        };
        if (acceptCompletion(*square, affineFromAnchors(source, anchors), required,
                             region, candidate)) {
            return true;
        }
    }

    return false;
}

bool completeTriangle(const QPolygonF &target, const PenPrimitive &triangle,
                      const PenPrimitive *square, const Region &region,
                      const std::function<bool()> &cancelled, int depth,
                      int *work, QVector<Candidate> *result) {
    if (cancelled && cancelled()) {
        return false;
    }
    if (++*work > kMaximumCompletionWork) {
        throw std::runtime_error("Catalog cover mesh completion exceeds its work limit");
    }
    Candidate candidate;
    const Polygons required = expanded({target}, kVerificationClearance);
    if (completeWithTriangle(target, required, triangle, region, cancelled, &candidate)
        || completeWithRectangle(target, required, square, region, cancelled, &candidate)) {
        result->push_back(std::move(candidate));
        return true;
    }
    if (cancelled && cancelled()) {
        return false;
    }
    if (depth >= kMaximumCompletionDepth) {
        throw std::runtime_error(QStringLiteral("Catalog cover could not reconstruct a mesh piece within tolerance (area %1, center %2, %3, depth %4)")
            .arg(std::abs(signedArea(target)), 0, 'g', 12)
            .arg(target.boundingRect().center().x()).arg(target.boundingRect().center().y())
            .arg(depth).toStdString());
    }
    const int edge = longestEdge(target);
    const QPointF start = target[edge];
    const QPointF end = target[(edge + 1) % target.size()];
    const QPointF opposite = target[(edge + 2) % target.size()];
    const QPointF midpoint = (start + end) * 0.5;

    return completeTriangle({start, midpoint, opposite}, triangle, square, region,
                            cancelled, depth + 1, work, result)
        && completeTriangle({midpoint, end, opposite}, triangle, square, region,
                            cancelled, depth + 1, work, result);
}

} // namespace

PointContainment::PointContainment(const Polygons &polygons)
    : path_(painterPath(polygons)), bounds_(path_.boundingRect()), bands_(kContainmentBands),
      step_(std::max(bounds_.height() / kContainmentBands, kVerificationClearance)) {
    for (const auto &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            const Edge edge{polygon[index], polygon[(index + 1) % polygon.size()],
                QLineF(polygon[index], polygon[(index + 1) % polygon.size()]).length()};
            const int first = std::clamp(static_cast<int>(std::floor(
                (std::min(edge.start.y(), edge.end.y()) - bounds_.top() - kVerificationClearance)
                    / step_)), 0, kContainmentBands - 1);
            const int last = std::clamp(static_cast<int>(std::floor(
                (std::max(edge.start.y(), edge.end.y()) - bounds_.top() + kVerificationClearance)
                    / step_)), 0, kContainmentBands - 1);
            const int position = edges_.size();
            edges_.push_back(edge);
            for (int band = first; band <= last; ++band)
                bands_[band].push_back(position);
        }
    }
}

bool PointContainment::contains(const QPointF &point) const {
    if (!bounds_.contains(point))
        return false;
    const int band = std::clamp(static_cast<int>(std::floor(
        (point.y() - bounds_.top()) / step_)), 0, kContainmentBands - 1);
    int winding = 0;

    for (int index : bands_[band]) {
        const auto &edge = edges_[index];
        const auto delta = edge.end - edge.start;
        const auto offset = point - edge.start;
        const double side = cross(delta, offset);
        if (std::abs(side) <= kVerificationClearance * edge.length
            && point.x() >= std::min(edge.start.x(), edge.end.x()) - kVerificationClearance
            && point.x() <= std::max(edge.start.x(), edge.end.x()) + kVerificationClearance
            && point.y() >= std::min(edge.start.y(), edge.end.y()) - kVerificationClearance
            && point.y() <= std::max(edge.start.y(), edge.end.y()) + kVerificationClearance) {
            return path_.contains(point);
        }
        if (edge.start.y() <= point.y() && edge.end.y() > point.y() && side > 0.0)
            ++winding;
        else if (edge.end.y() <= point.y() && edge.start.y() > point.y() && side < 0.0)
            --winding;
    }

    return winding != 0;
}

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    if (polygon.isEmpty()) {
        return result;
    }
    const QPointF origin = polygon.front();
    for (int index = 1; index + 1 < polygon.size(); ++index) {
        result += cross(polygon[index] - origin, polygon[index + 1] - origin);
    }

    return result * 0.5;
}

double area(const Polygons &polygons) {
    double result = 0.0;
    for (const QPolygonF &polygon : polygons) {
        result += signedArea(polygon);
    }

    return std::abs(result);
}

Polygons unite(const Polygons &polygons) {
    return booleanOperation(polygons, {}, Clipper2Lib::ClipType::Union);
}

Polygons subtract(const Polygons &subject, const Polygons &clip) {
    return booleanOperation(subject, clip, Clipper2Lib::ClipType::Difference);
}

Polygons intersect(const Polygons &subject, const Polygons &clip) {
    return booleanOperation(subject, clip, Clipper2Lib::ClipType::Intersection);
}

QPainterPath painterPath(const Polygons &polygons) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    for (const QPolygonF &polygon : polygons) {
        result.addPolygon(polygon);
        result.closeSubpath();
    }

    return result;
}

QPolygonF convexHull(QPolygonF points) {
    std::sort(points.begin(), points.end(), [](const QPointF &left, const QPointF &right) {
        return left.x() == right.x() ? left.y() < right.y() : left.x() < right.x();
    });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) {
        return points;
    }
    QPolygonF hull;
    for (const QPointF &point : points) {
        while (hull.size() >= 2
               && cross(hull.back() - hull[hull.size() - 2], point - hull.back()) <= 0.0) {
            hull.removeLast();
        }
        hull.push_back(point);
    }
    const int lowerCount = hull.size();
    for (int index = points.size() - 2; index >= 0; --index) {
        while (hull.size() > lowerCount
               && cross(hull.back() - hull[hull.size() - 2], points[index] - hull.back()) <= 0.0) {
            hull.removeLast();
        }
        hull.push_back(points[index]);
    }
    hull.removeLast();

    return hull;
}

Polygons mapped(const PenPrimitive &shape, const QTransform &transform) {
    Polygons result;
    for (const QPolygonF &polygon : shape.contours) {
        QPolygonF transformed = transform.map(polygon);
        if (transform.determinant() < 0.0) {
            std::reverse(transformed.begin(), transformed.end());
        }
        result.push_back(std::move(transformed));
    }

    return unite(result);
}

Polygons expanded(const Polygons &polygons, double radius) {
    Polygons parts = polygons;
    const std::array<QPointF, 4> offsets = {
        QPointF(-radius, -radius), QPointF(radius, -radius),
        QPointF(radius, radius), QPointF(-radius, radius),
    };
    for (const QPolygonF &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            QPolygonF points;
            for (const QPointF &offset : offsets) {
                points.push_back(polygon[index] + offset);
                points.push_back(polygon[(index + 1) % polygon.size()] + offset);
            }
            parts.push_back(convexHull(points));
        }
    }

    return unite(parts);
}

Polygons interiorSupport(const Polygons &polygons, double allowance) {
    Polygons boundary;
    const QPointF delta(allowance, allowance);

    for (const auto &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            QPolygonF corners;
            for (const auto &point : {polygon[index], polygon[(index + 1) % polygon.size()]}) {
                corners += QPolygonF({point - delta, point + QPointF(delta.x(), -delta.y()),
                    point + delta, point + QPointF(-delta.x(), delta.y())});
            }
            boundary.push_back(convexHull(corners));
        }
    }

    return subtract(polygons, unite(boundary));
}

QTransform emittedTransform(const QTransform &transform) {
    const fls::Matrix3 matrix = fls::affine(transform.m11(), transform.m21(), transform.dx(),
                                           transform.m12(), transform.m22(), transform.dy());
    fls::scene::Transform2D fields = fls::decomposeTransform2D(matrix);
    for (double *field : {&fields.x, &fields.y, &fields.scaleX, &fields.scaleY,
                          &fields.skew, &fields.rotation}) {
        *field = std::abs(*field) < kTransformZeroThreshold ? 0.0 : static_cast<float>(*field);
    }
    const fls::Matrix3 emitted = fields.matrix();

    return QTransform(emitted.m[0][0], emitted.m[1][0], emitted.m[0][1],
                      emitted.m[1][1], emitted.m[0][2], emitted.m[1][2]);
}

QTransform affineFromAnchors(const std::array<QPointF, 3> &source,
                             const std::array<QPointF, 3> &target) {
    const QTransform from(source[1].x() - source[0].x(), source[1].y() - source[0].y(),
                          source[2].x() - source[0].x(), source[2].y() - source[0].y(),
                          source[0].x(), source[0].y());
    const QTransform to(target[1].x() - target[0].x(), target[1].y() - target[0].y(),
                        target[2].x() - target[0].x(), target[2].y() - target[0].y(),
                        target[0].x(), target[0].y());
    bool invertible = false;
    const QTransform inverse = from.inverted(&invertible);

    return invertible ? inverse * to : QTransform(0, 0, 0, 0, 0, 0);
}

Region buildRegion(const PenFillRequest &request, const std::function<bool()> &cancelled) {
    Region result;
    result.tolerance = request.boundaryTolerance;
    Polygons chords;
    Polygons hulls;
    const QVector<PenLoop> loops = request.loops.isEmpty()
        ? QVector<PenLoop>{{request.points, PenLoopKind::Outer}} : request.loops;
    const PenContour contour = buildPenContour(loops, request.boundaryTolerance * kGeometryFraction);
    if (!contour.valid()) {
        throw std::runtime_error(contour.error.toStdString());
    }
    for (const PenContourLoop &loop : contour.loops) {
        QPolygonF polygon;
        for (const PenBoundarySegment &segment : loop.segments) {
            appendSegment(segment, request.boundaryTolerance * kGeometryFraction,
                          &polygon, &hulls, 0, cancelled);
        }
        const bool positive = loop.kind == PenLoopKind::Outer;
        if ((signedArea(polygon) > 0.0) != positive) {
            std::reverse(polygon.begin(), polygon.end());
        }
        chords.push_back(polygon);
        result.originalArea += contourArea(loop.segments) * (positive ? 1.0 : -1.0);
    }
    result.required = unite(chords);
    result.required += hulls;
    result.required = unite(result.required);
    result.permitted = expanded(result.required,
        request.boundaryTolerance * kEnvelopeFraction / std::sqrt(2.0));
    result.visible = result.required;
    result.spillFree = result.required;
    result.requiredPath = painterPath(result.required);
    result.permittedPath = painterPath(result.permitted);
    result.spillFreePath = result.requiredPath;
    result.spillFreeContainment = std::make_shared<PointContainment>(result.spillFree);
    result.bounds = result.requiredPath.boundingRect();
    result.area = area(result.required);
    if (result.area <= 0.0 || !std::isfinite(result.area)) {
        throw std::runtime_error("Catalog cover target has no fillable area");
    }

    return result;
}

Region leewayAdjustedRegion(const Region &region, const Polygons &inputLeeway,
                            double outwardAllowance) {
    const Polygons leeway = unite(inputLeeway);
    if (leeway.isEmpty()) {
        Region result = region;
        result.permitted = expanded(region.required, outwardAllowance);
        result.permittedPath = painterPath(result.permitted);

        return result;
    }
    QPainterPathStroker stroker;
    stroker.setWidth(region.tolerance * 2.0);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);
    const QPainterPath leewayPath = painterPath(leeway);
    const Polygons leewayCore = polygonsFromPath(
        leewayPath.subtracted(stroker.createStroke(leewayPath)));
    Region result = region;
    result.required = subtract(region.required, leewayCore);
    result.visible = subtract(region.required, leeway);
    result.spillFree = unite(region.required + leeway);
    result.leeway = leeway;
    result.permitted = unite(expanded(region.required, outwardAllowance) + leeway);
    result.requiredPath = painterPath(result.required);
    result.permittedPath = painterPath(result.permitted);
    result.spillFreePath = painterPath(result.spillFree);
    result.spillFreeContainment = std::make_shared<PointContainment>(result.spillFree);
    result.bounds = result.requiredPath.boundingRect();
    result.area = area(result.required);

    return result;
}

QVector<Candidate> completeCover(const Region &region, const QVector<Primitive> &primitives,
                                const std::function<bool()> &cancelled) {
    QVector<Candidate> result;
    const PenPrimitive *triangle = findShape(primitives, 103);
    const PenPrimitive *square = findShape(primitives, 101);
    int work = 0;
    if (triangle == nullptr || triangle->contours.size() != 1 || triangle->contours.front().size() != 3) {
        throw std::runtime_error("Catalog cover Triangle geometry is unavailable");
    }
    for (const Polygons &component : connectedRegions(region.required)) {
        PolygonMeshRequest request;
        request.mergeSquares = false;
        request.sources.triangle = triangle->contours.front();
        for (const QPolygonF &polygon : component) {
            request.contours.push_back(polygon);
        }
        const PolygonMeshResult mesh = meshPolygon(request, cancelled);
        if (mesh.cancelled) {
            return {};
        }
        if (!mesh.error.isEmpty()) {
            throw std::runtime_error(mesh.error.toStdString());
        }
        for (const PolygonMeshPlacement &placement : mesh.placements) {
            const Polygons target = mapped(*triangle, placement.transform);
            if (!target.isEmpty() && !completeTriangle(target.front(), *triangle, square, region,
                                                      cancelled, 0, &work, &result)) {
                return {};
            }
        }
    }

    return result;
}

} // namespace gui::catalog
