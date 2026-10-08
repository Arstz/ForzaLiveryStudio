#include "compact_fit_catalog_internal.h"

#include "layer.h"
#include "matrix_math.h"
#include "polygon_mesh.h"

#include <clipper2/clipper.engine.h>

#include <algorithm>
#include <bit>
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
constexpr int kLocalOperandLimit = 32;
constexpr int64_t kLocalOperandMinimumExtent = 4000000;
constexpr int64_t kLocalOperandPadding = 16;
constexpr double kSweptSquareMinimumRadius = 1e-5;
constexpr double kSweptSquareMaximumCoordinate = 1000000.0;

enum class GeometryOperation { Union, Difference, Intersection, Expansion, Mapping, Count };

struct GeometryTiming {
    qint64 nanoseconds = 0;
    int calls = 0;
};

thread_local std::array<GeometryTiming, static_cast<int>(GeometryOperation::Count)> geometryTimings;

class GeometryTimer {
public:
    explicit GeometryTimer(GeometryOperation operation) : operation_(operation) {
        static const bool enabled = qEnvironmentVariableIsSet("FLS_PROFILE_GEOMETRY");
        if (enabled)
            timer_.start();
    }
    ~GeometryTimer() {
        if (timer_.isValid()) {
            auto &timing = geometryTimings[static_cast<int>(operation_)];
            timing.nanoseconds += timer_.nsecsElapsed();
            ++timing.calls;
        }
    }
private:
    QElapsedTimer timer_;
    GeometryOperation operation_;
};

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

int strictConvexOrientation(const Clipper2Lib::Path64 &path) {
    int orientation = 0;
    int firstDirection = 0, previousDirection = 0, directionChanges = 0;

    if (path.size() < 3)
        return 0;
    for (int index = 0; index < path.size(); ++index) {
        const auto &start = path[index];
        const int next = index + 1 == path.size() ? 0 : index + 1;
        const int after = next + 1 == path.size() ? 0 : next + 1;
        const auto &end = path[next];
        const int turn = Clipper2Lib::CrossProductSign(start, end, path[after]);
        const int direction = end.x > start.x ? 1 : end.x < start.x ? -1 : 0;

        if (turn == 0 || (orientation != 0 && turn != orientation))
            return 0;
        orientation = turn;
        if (direction != 0) {
            if (firstDirection == 0)
                firstDirection = direction;
            else if (direction != previousDirection)
                ++directionChanges;
            previousDirection = direction;
        }
    }
    directionChanges += previousDirection != firstDirection;

    return directionChanges == 2 ? orientation : 0;
}

class PolygonContainment {
public:
    explicit PolygonContainment(const Clipper2Lib::Paths64 &paths)
        : bounds_(Clipper2Lib::GetBounds(paths)), bands_(kContainmentBands),
          step_(std::max<int64_t>(1, (bounds_.bottom - bounds_.top) / kContainmentBands + 1)) {
        for (const auto &path : paths) {
            loopBounds_.push_back(Clipper2Lib::GetBounds(path));
            for (int index = 0; index < path.size(); ++index) {
                const Edge edge{path[index], path[(index + 1) % path.size()]};
                const int position = edges_.size();
                const int first = band(std::min(edge.start.y, edge.end.y));
                const int last = band(std::max(edge.start.y, edge.end.y));

                edges_.push_back(edge);
                for (int row = first; row <= last; ++row)
                    bands_[row].push_back(position);
            }
        }
    }

    bool contains(const Clipper2Lib::Paths64 &subject, const Clipper2Lib::Paths64 &clip) const {
        const auto bounds = Clipper2Lib::GetBounds(subject);

        if (edges_.isEmpty() || subject.empty() || !bounds_.Contains(bounds))
            return false;
        for (const auto &path : subject) {
            if (path.empty() || !contains(path.front()))
                return false;
            for (int index = 0; index < path.size(); ++index) {
                const Edge edge{path[index], path[(index + 1) % path.size()]};
                const auto left = std::min(edge.start.x, edge.end.x);
                const auto right = std::max(edge.start.x, edge.end.x);
                const auto top = std::min(edge.start.y, edge.end.y);
                const auto bottom = std::max(edge.start.y, edge.end.y);
                const int first = band(top), last = band(bottom);

                for (int row = first; row <= last; ++row) {
                    for (int position : bands_[row]) {
                        const auto &other = edges_[position];
                        if (std::max(other.start.x, other.end.x) < left
                            || std::min(other.start.x, other.end.x) > right
                            || std::max(other.start.y, other.end.y) < top
                            || std::min(other.start.y, other.end.y) > bottom)
                            continue;
                        const int firstSide = Clipper2Lib::CrossProductSign(edge.start, edge.end, other.start);
                        const int lastSide = Clipper2Lib::CrossProductSign(edge.start, edge.end, other.end);
                        const int startSide = Clipper2Lib::CrossProductSign(other.start, other.end, edge.start);
                        const int endSide = Clipper2Lib::CrossProductSign(other.start, other.end, edge.end);
                        if (firstSide * lastSide <= 0 && startSide * endSide <= 0)
                            return false;
                    }
                }
            }
        }
        for (int index = 0; index < clip.size(); ++index) {
            const auto &path = clip[index];
            if (path.empty() || !bounds.Contains(loopBounds_[index]))
                continue;
            for (const auto &part : subject) {
                if (containsBoundary(path.front(), part))
                    return false;
            }
        }

        return true;
    }

private:
    struct Edge {
        Clipper2Lib::Point64 start, end;
    };
    Clipper2Lib::Rect64 bounds_;
    QVector<Clipper2Lib::Rect64> loopBounds_;
    QVector<Edge> edges_;
    QVector<QVector<int>> bands_;
    int64_t step_;

    static bool containsBoundary(const Clipper2Lib::Point64 &point, const Clipper2Lib::Path64 &path) {
        int winding = 0;

        for (int index = 0; index < path.size(); ++index) {
            const auto &start = path[index];
            const auto &end = path[(index + 1) % path.size()];
            const int side = Clipper2Lib::CrossProductSign(start, end, point);
            if (side == 0 && point.x >= std::min(start.x, end.x) && point.x <= std::max(start.x, end.x)
                && point.y >= std::min(start.y, end.y) && point.y <= std::max(start.y, end.y))
                return true;
            if (start.y <= point.y && end.y > point.y && side > 0)
                ++winding;
            else if (end.y <= point.y && start.y > point.y && side < 0)
                --winding;
        }

        return winding != 0;
    }

    int band(int64_t y) const {

        return int(std::clamp<int64_t>((y - bounds_.top) / step_, 0, kContainmentBands - 1));
    }

    bool contains(const Clipper2Lib::Point64 &point) const {
        int winding = 0;

        for (int position : bands_[band(point.y)]) {
            const auto &edge = edges_[position];
            const int side = Clipper2Lib::CrossProductSign(edge.start, edge.end, point);
            if (side == 0 && point.x >= std::min(edge.start.x, edge.end.x)
                && point.x <= std::max(edge.start.x, edge.end.x)
                && point.y >= std::min(edge.start.y, edge.end.y)
                && point.y <= std::max(edge.start.y, edge.end.y))
                return false;
            if (edge.start.y <= point.y && edge.end.y > point.y && side > 0)
                ++winding;
            else if (edge.end.y <= point.y && edge.start.y > point.y && side < 0)
                --winding;
        }

        return winding != 0;
    }
};

struct LocalOperand {
    Clipper2Lib::ReuseableDataContainer64 data;
    Clipper2Lib::Rect64 bounds;
};

struct PreparedOperand {
    Polygons polygons;
    Clipper2Lib::Paths64 paths;
    Clipper2Lib::ReuseableDataContainer64 data;
    Clipper2Lib::Rect64 bounds;
    mutable QVector<std::shared_ptr<const LocalOperand>> local;
    mutable std::unique_ptr<PolygonContainment> containment;
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
    result->paths = integerPaths(polygons);
    result->bounds = Clipper2Lib::GetBounds(result->paths);
    result->type = type;
    result->data.AddPaths(result->paths, type, false);
    if (cache.size() >= kPreparedOperandLimit)
        cache.pop_back();
    cache.push_front(result);

    return result;
}

int64_t floorMultiple(int64_t value, int64_t step) {

    return (value / step - (value < 0 && value % step != 0)) * step;
}

Clipper2Lib::Paths64 localizedPaths(const Clipper2Lib::Paths64 &paths,
                                   const Clipper2Lib::Rect64 &bounds) {
    Clipper2Lib::Paths64 result;

    for (const auto &path : paths) {
        std::vector<unsigned char> preserved(path.size(), 0);
        Clipper2Lib::Path64 local;
        std::vector<unsigned char> retained;
        local.reserve(path.size());
        retained.reserve(path.size());
        for (int index = 0; index < path.size(); ++index) {
            const int next = index + 1 == path.size() ? 0 : index + 1;
            const auto &start = path[index];
            const auto &end = path[next];
            if (std::max(start.x, end.x) >= bounds.left && std::min(start.x, end.x) <= bounds.right
                && std::max(start.y, end.y) >= bounds.top && std::min(start.y, end.y) <= bounds.bottom) {
                preserved[index] = 1;
                preserved[next] = 1;
            }
        }
        for (int index = 0; index < path.size(); ++index) {
            const auto point = preserved[index] ? path[index] : Clipper2Lib::Point64{
                std::clamp(path[index].x, bounds.left, bounds.right),
                std::clamp(path[index].y, bounds.top, bounds.bottom)};
            if (!local.empty() && local.back() == point) {
                retained.back() |= preserved[index];
                continue;
            }
            while (local.size() >= 2 && !retained.back()) {
                const auto &before = local[local.size() - 2];
                const auto &previous = local.back();
                if (Clipper2Lib::CrossProductSign(before, previous, point) != 0
                    || previous.x < std::min(before.x, point.x) || previous.x > std::max(before.x, point.x)
                    || previous.y < std::min(before.y, point.y) || previous.y > std::max(before.y, point.y))
                    break;
                local.pop_back();
                retained.pop_back();
            }
            local.push_back(point);
            retained.push_back(preserved[index]);
        }
        if (local.size() > 1 && local.front() == local.back())
            local.pop_back();
        if (local.size() >= 3)
            result.push_back(std::move(local));
    }

    return result;
}

std::shared_ptr<const LocalOperand> localOperand(const PreparedOperand &operand,
                                                const Clipper2Lib::Paths64 &subject) {
    if (subject.empty())
        return {};
    const auto subjectBounds = Clipper2Lib::GetBounds(subject);
    const int64_t extent = std::max(subjectBounds.Width(), subjectBounds.Height());
    const int64_t step = static_cast<int64_t>(std::bit_ceil(static_cast<uint64_t>(
        std::max(kLocalOperandMinimumExtent, extent / 2))));
    const Clipper2Lib::Rect64 bounds{
        floorMultiple(subjectBounds.left - kLocalOperandPadding, step),
        floorMultiple(subjectBounds.top - kLocalOperandPadding, step),
        -floorMultiple(-subjectBounds.right - kLocalOperandPadding, step),
        -floorMultiple(-subjectBounds.bottom - kLocalOperandPadding, step)};

    if (bounds.Width() >= operand.bounds.Width() && bounds.Height() >= operand.bounds.Height())
        return {};
    for (int index = 0; index < operand.local.size(); ++index) {
        const auto &entry = operand.local[index];
        if (entry->bounds.left == bounds.left && entry->bounds.right == bounds.right
            && entry->bounds.top == bounds.top && entry->bounds.bottom == bounds.bottom) {
            auto reused = operand.local.takeAt(index);
            operand.local.push_front(reused);

            return reused;
        }
    }
    auto result = std::make_shared<LocalOperand>();
    result->bounds = bounds;
    result->data.AddPaths(localizedPaths(operand.paths, bounds), operand.type, false);
    if (operand.local.size() >= kLocalOperandLimit)
        operand.local.pop_back();
    operand.local.push_front(result);

    return result;
}

QPolygonF sweptSquare(const QPointF &start, const QPointF &end, double radius) {
    const auto &bottom = start.y() <= end.y() ? start : end;
    const auto &right = start.x() >= end.x() ? start : end;
    const auto &top = start.y() >= end.y() ? start : end;
    const auto &left = start.x() <= end.x() ? start : end;
    const double extent = std::abs(radius);
    const std::array<QPointF, 8> corners{
        bottom + QPointF(-extent, -extent), bottom + QPointF(extent, -extent),
        right + QPointF(extent, -extent), right + QPointF(extent, extent),
        top + QPointF(extent, extent), top + QPointF(-extent, extent),
        left + QPointF(-extent, extent), left + QPointF(-extent, -extent)};
    QPolygonF result;

    if (extent < kSweptSquareMinimumRadius || std::max({std::abs(start.x()), std::abs(start.y()),
            std::abs(end.x()), std::abs(end.y())}) > kSweptSquareMaximumCoordinate) {
        QPolygonF points;
        for (const auto &offset : {QPointF(-extent, -extent), QPointF(extent, -extent),
                 QPointF(extent, extent), QPointF(-extent, extent)}) {
            points.push_back(start + offset);
            points.push_back(end + offset);
        }

        return convexHull(points);
    }
    result.reserve(corners.size());
    for (const auto &corner : corners)
        if (result.isEmpty() || result.back() != corner)
            result.push_back(corner);
    if (result.size() > 1 && result.front() == result.back())
        result.removeLast();

    return result;
}

Polygons polygonsFromIntegerPaths(const Clipper2Lib::Paths64 &output) {
    Polygons result;

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

Polygons booleanOperation(const Polygons &subject, const Polygons &clip,
                          Clipper2Lib::ClipType operation) {
    const GeometryTimer timer(operation == Clipper2Lib::ClipType::Union ? GeometryOperation::Union
        : operation == Clipper2Lib::ClipType::Difference ? GeometryOperation::Difference : GeometryOperation::Intersection);
    const bool normalizeOnly = clip.isEmpty() && operation != Clipper2Lib::ClipType::Intersection;
    const auto preparedSubject = normalizeOnly && subject.size() == 1
        ? std::shared_ptr<const PreparedOperand>() : preparedOperand(subject, Clipper2Lib::PathType::Subject);
    const auto preparedClip = preparedOperand(clip, Clipper2Lib::PathType::Clip);
    const auto subjectPaths = preparedSubject ? Clipper2Lib::Paths64() : integerPaths(subject);
    const auto &paths = preparedSubject ? preparedSubject->paths : subjectPaths;
    Clipper2Lib::Clipper64 engine;
    Clipper2Lib::Paths64 output;
    Polygons result;

    if (normalizeOnly && paths.size() == 1) {
        const int orientation = strictConvexOrientation(paths.front());
        if (orientation != 0) {
            output = paths;
            if (orientation < 0)
                std::reverse(output.front().begin(), output.front().end());

            return polygonsFromIntegerPaths(output);
        }
    }
    if ((operation == Clipper2Lib::ClipType::Difference
        || operation == Clipper2Lib::ClipType::Intersection) && preparedClip) {
        const auto bounds = Clipper2Lib::GetBounds(paths);
        if (paths.empty() || bounds.right < preparedClip->bounds.left
            || bounds.left > preparedClip->bounds.right || bounds.bottom < preparedClip->bounds.top
            || bounds.top > preparedClip->bounds.bottom) {
            return operation == Clipper2Lib::ClipType::Intersection ? result
                : booleanOperation(subject, {}, Clipper2Lib::ClipType::Union);
        }
        if (!preparedClip->containment)
            preparedClip->containment = std::make_unique<PolygonContainment>(preparedClip->paths);
        if (preparedClip->containment->contains(paths, preparedClip->paths)) {
            return operation == Clipper2Lib::ClipType::Difference ? result
                : booleanOperation(subject, {}, Clipper2Lib::ClipType::Union);
        }
    }
    engine.PreserveCollinear(false);
    if (preparedSubject)
        engine.AddReuseableData(preparedSubject->data);
    else
        engine.AddSubject(paths);
    if (preparedClip) {
        const auto local = localOperand(*preparedClip, paths);
        engine.AddReuseableData(local ? local->data : preparedClip->data);
    } else {
        engine.AddClip(integerPaths(clip));
    }
    if (!engine.Execute(operation, Clipper2Lib::FillRule::NonZero, output)) {
        throw std::runtime_error("Catalog cover polygon operation failed");
    }
    return polygonsFromIntegerPaths(output);
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

QJsonObject geometryPerformance() {
    const std::array<QString, static_cast<int>(GeometryOperation::Count)> names{
        QStringLiteral("union"), QStringLiteral("difference"), QStringLiteral("intersection"),
        QStringLiteral("expansion"), QStringLiteral("mapping")};
    QJsonObject result;

    for (int index = 0; index < names.size(); ++index)
        result.insert(names[index], QJsonObject{{QStringLiteral("milliseconds"), geometryTimings[index].nanoseconds / 1e6},
            {QStringLiteral("calls"), geometryTimings[index].calls}});

    return result;
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
    const GeometryTimer timer(GeometryOperation::Mapping);
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
    const GeometryTimer timer(GeometryOperation::Expansion);
    Polygons parts = polygons;

    for (const QPolygonF &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            parts.push_back(sweptSquare(polygon[index],
                polygon[index + 1 == polygon.size() ? 0 : index + 1], radius));
        }
    }

    return unite(parts);
}

Polygons interiorSupport(const Polygons &polygons, double allowance) {
    Polygons boundary;

    for (const auto &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            boundary.push_back(sweptSquare(polygon[index],
                polygon[index + 1 == polygon.size() ? 0 : index + 1], allowance));
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
