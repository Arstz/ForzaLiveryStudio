#include "compact_fit_whole_shapes.h"
#include "compact_fit_catalog_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>

namespace gui::compact {
namespace {

constexpr int kSquareShapeId = 101;
constexpr int kCircleShapeId = 102;
constexpr int kCurveSamples = 32;
constexpr int kMaximumSamples = 512;
constexpr int kWindowStarts = 24;
constexpr int kConicSamples = 9;
constexpr int kConicParameters = 5;
constexpr int kEllipseModels = 8;
constexpr int kEllipsePhases = 4;
constexpr int kMaximumLines = 16;
constexpr int kMaximumDirections = 4;
constexpr int kMaximumOffsets = 6;
constexpr int kCandidatesPerShape = 24;
constexpr double kPivotTolerance = 1e-11;
constexpr double kArcTolerance = 0.002;
constexpr double kModelTolerance = 0.005;
constexpr double kMinimumLineFraction = 0.01;
constexpr double kParallelTolerance = 0.01;
constexpr double kMinimumDirectionCross = 0.2;
constexpr double kClearanceUlps = 4;

double cross(QPointF left, QPointF right) {
    return left.x() * right.y() - left.y() * right.x();
}

double squaredLength(QPointF point) {
    return QPointF::dotProduct(point, point);
}

catalog::Polygons sampledContours(const PenFillRequest &request, const catalog::Region &region) {
    catalog::Polygons result;
    const auto append = [&](const QVector<PenPoint> &points) {
        QPolygonF polygon;
        for (const auto &segment : penSegments(points)) {
            const int count = segment.curved ? kCurveSamples : 1;
            for (int index = 0; index < count; ++index)
                polygon.push_back(segment.point(double(index) / count));
        }
        if (polygon.size() >= 3)
            result.push_back(std::move(polygon));
    };
    if (request.loops.isEmpty())
        append(request.points);
    else
        for (const auto &loop : request.loops)
            append(loop.points);
    if (result.isEmpty())
        result = region.required;

    return result;
}

QTransform conditioningFrame(const catalog::Region &region) {
    const auto center = region.bounds.center();
    const double scale = std::max(region.bounds.width(), region.bounds.height());
    double area = 0, firstX = 0, firstY = 0, secondX = 0, secondY = 0, mixed = 0;
    for (const auto &polygon : region.required) {
        for (int index = 0; index < polygon.size(); ++index) {
            const auto a = (polygon[index] - center) / scale;
            const auto b = (polygon[(index + 1) % polygon.size()] - center) / scale;
            const double weight = cross(a, b);
            area += weight / 2;
            firstX += (a.x() + b.x()) * weight / 6;
            firstY += (a.y() + b.y()) * weight / 6;
            secondX += (a.x()*a.x() + a.x()*b.x() + b.x()*b.x()) * weight / 12;
            secondY += (a.y()*a.y() + a.y()*b.y() + b.y()*b.y()) * weight / 12;
            mixed += (2*a.x()*a.y() + a.x()*b.y() + b.x()*a.y() + 2*b.x()*b.y()) * weight / 24;
        }
    }
    const double x = firstX / area, y = firstY / area;
    const double xx = secondX / area - x*x, yy = secondY / area - y*y;
    const double xy = mixed / area - x*y;
    const double angle = std::atan2(2*xy, xx-yy) / 2;
    const double difference = std::hypot(xx-yy, 2*xy);
    const double major = std::max(kPivotTolerance, (xx+yy+difference)/2);
    const double minor = std::max(kPivotTolerance, (xx+yy-difference)/2);
    const double cosine = std::cos(angle), sine = std::sin(angle);
    const double horizontal = 1 / (scale * std::sqrt(major));
    const double vertical = 1 / (scale * std::sqrt(minor));

    return QTransform(cosine*horizontal, -sine*vertical, sine*horizontal, cosine*vertical,
        -(cosine*center.x()+sine*center.y())*horizontal,
        -(-sine*center.x()+cosine*center.y())*vertical);
}

bool solveConic(std::array<std::array<double, kConicParameters + 1>, kConicParameters> system,
    std::array<double, kConicParameters> *solution) {
    for (int column = 0; column < kConicParameters; ++column) {
        int pivot = column;
        for (int row = column + 1; row < kConicParameters; ++row)
            if (std::abs(system[row][column]) > std::abs(system[pivot][column]))
                pivot = row;
        if (std::abs(system[pivot][column]) < kPivotTolerance)
            return false;
        std::swap(system[pivot], system[column]);
        const double divisor = system[column][column];
        for (int index = column; index <= kConicParameters; ++index)
            system[column][index] /= divisor;
        for (int row = 0; row < kConicParameters; ++row) {
            if (row == column)
                continue;
            const double factor = system[row][column];
            for (int index = column; index <= kConicParameters; ++index)
                system[row][index] -= factor * system[column][index];
        }
    }
    for (int index = 0; index < kConicParameters; ++index)
        (*solution)[index] = system[index][kConicParameters];

    return true;
}

struct Ellipse {
    QPointF center;
    QTransform unitCircle;
    QTransform inverse;
    int support = 0;
    double error = 0;
};

std::optional<Ellipse> fitEllipse(const QPolygonF &points) {
    QPointF origin;
    for (const auto &point : points)
        origin += point / points.size();
    double scale = 0;
    for (const auto &point : points)
        scale = std::max(scale, std::sqrt(squaredLength(point-origin)));
    if (scale <= kPivotTolerance)
        return {};
    std::array<std::array<double, kConicParameters + 1>, kConicParameters> system{};
    for (const auto &point : points) {
        const auto local = (point-origin) / scale;
        const double x = local.x(), y = local.y();
        const std::array<double, kConicParameters> values{x*x, x*y, y*y, x, y};
        for (int row = 0; row < kConicParameters; ++row) {
            for (int column = 0; column < kConicParameters; ++column)
                system[row][column] += values[row] * values[column];
            system[row][kConicParameters] += values[row];
        }
    }
    std::array<double, kConicParameters> fitted{};
    if (!solveConic(system, &fitted))
        return {};
    const auto [a, b, c, d, e] = fitted;
    const double determinant = a*c - b*b/4;
    if (a <= 0 || c <= 0 || determinant <= kPivotTolerance)
        return {};
    const QPointF center((b*e/2-c*d)/(2*determinant), (b*d/2-a*e)/(2*determinant));
    const double radius = 1 + a*center.x()*center.x() + b*center.x()*center.y() + c*center.y()*center.y();
    const double difference = std::hypot(a-c, b);
    const double first = (a+c+difference)/2, second = (a+c-difference)/2;
    if (radius <= 0 || second <= kPivotTolerance)
        return {};
    const double angle = std::atan2(b, a-c)/2;
    const double horizontal = scale * std::sqrt(radius/first);
    const double vertical = scale * std::sqrt(radius/second);
    const auto position = origin + center * scale;
    const QTransform transform(std::cos(angle)*horizontal, std::sin(angle)*horizontal,
        -std::sin(angle)*vertical, std::cos(angle)*vertical, position.x(), position.y());
    bool invertible = false;
    const auto inverse = transform.inverted(&invertible);
    if (!invertible || !std::isfinite(horizontal) || !std::isfinite(vertical))
        return {};

    return Ellipse{position, transform, inverse};
}

QVector<Ellipse> ellipseModels(const catalog::Polygons &contours, const std::function<bool()> &cancelled) {
    QVector<Ellipse> result;
    for (const auto &polygon : contours) {
        if (polygon.size() < kConicSamples)
            continue;
        for (int start = 0; start < polygon.size(); start += std::max(1, int(polygon.size())/kWindowStarts)) {
            if (cancelled && cancelled())
                return result;
            for (double fraction : {0.125, 0.25, 0.5, 0.75}) {
                QPolygonF samples;
                const int length = std::max(kConicSamples-1, int(polygon.size()*fraction));
                for (int index = 0; index < kConicSamples; ++index)
                    samples.push_back(polygon[(start + index*length/(kConicSamples-1)) % polygon.size()]);
                auto fitted = fitEllipse(samples);
                if (!fitted)
                    continue;
                for (const auto &point : polygon) {
                    const double distance = std::abs(std::sqrt(squaredLength(fitted->inverse.map(point)))-1);
                    fitted->support += distance < kArcTolerance;
                    fitted->error += std::min(distance*distance, kArcTolerance*kArcTolerance);
                }
                if (fitted->support < kConicSamples)
                    continue;
                const auto duplicate = std::find_if(result.begin(), result.end(), [&](const auto &previous) {
                    if (QLineF(previous.center, fitted->center).length() > kModelTolerance)
                        return false;
                    for (QPointF point : {QPointF(1,0), QPointF(0,1)})
                        if (std::abs(std::sqrt(squaredLength(previous.inverse.map(fitted->unitCircle.map(point))))-1) > kModelTolerance)
                            return false;
                    return true;
                });
                if (duplicate == result.end())
                    result.push_back(*fitted);
                else if (duplicate->support < fitted->support
                    || (duplicate->support == fitted->support && duplicate->error > fitted->error))
                    *duplicate = *fitted;
            }
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.support == right.support ? left.error < right.error : left.support > right.support;
    });
    result.resize(std::min(kEllipseModels, int(result.size())));

    return result;
}

struct Line { QPointF first, second, normal; double length = 0; };
struct Direction { QPointF normal; QVector<double> offsets; };

QVector<double> uniqueOffsets(QVector<double> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end(), [](double left, double right) {
        return std::abs(left-right) < kPivotTolerance * 100;
    }), values.end());
    if (values.size() > kMaximumOffsets) {
        QVector<double> retained;
        for (int index = 0; index < kMaximumOffsets; ++index)
            retained.push_back(values[index*(values.size()-1)/(kMaximumOffsets-1)]);
        values = retained;
    }

    return values;
}

QVector<QLineF> straightSegments(const PenFillRequest &request, const catalog::Polygons &contours,
    const QTransform &conditioned) {
    QVector<QLineF> result;
    const auto append = [&](const QVector<PenPoint> &points) {
        for (const auto &segment : penSegments(points))
            if (!segment.curved)
                result.push_back({conditioned.map(segment.start), conditioned.map(segment.end)});
    };
    if (!request.loops.isEmpty()) {
        for (const auto &loop : request.loops)
            append(loop.points);
    } else if (!request.points.isEmpty()) {
        append(request.points);
    } else {
        QRectF bounds;
        for (const auto &polygon : contours)
            bounds = bounds.united(polygon.boundingRect());
        const double minimum = std::max(bounds.width(), bounds.height()) * kMinimumLineFraction;
        for (const auto &polygon : contours)
            for (int index = 0; index < polygon.size(); ++index) {
                const QLineF line(polygon[index], polygon[(index + 1) % polygon.size()]);
                if (line.length() >= minimum)
                    result.push_back(line);
            }
    }

    return result;
}

QVector<QTransform> parallelograms(const QVector<QLineF> &segments, const PenPrimitive &square) {
    QVector<Line> lines;
    for (const auto &segment : segments) {
        const auto first = segment.p1(), second = segment.p2();
        const double length = segment.length();
        if (length <= kPivotTolerance)
            continue;
        const auto delta = (second-first)/length;
        QPointF normal(-delta.y(), delta.x());
        if (normal.x() < 0 || (normal.x() == 0 && normal.y() < 0))
            normal = -normal;
        lines.push_back({first, second, normal, length});
    }
    std::stable_sort(lines.begin(), lines.end(), [](const auto &left, const auto &right) { return left.length > right.length; });
    lines.resize(std::min(kMaximumLines, int(lines.size())));
    QVector<Direction> directions;
    for (const auto &line : lines) {
        auto direction = std::find_if(directions.begin(), directions.end(), [&](const auto &value) {
            return std::abs(cross(line.normal, value.normal)) < kParallelTolerance;
        });
        if (direction == directions.end()) {
            if (directions.size() == kMaximumDirections)
                continue;
            directions.push_back({line.normal, {QPointF::dotProduct(line.normal, line.first)}});
        } else {
            direction->offsets.push_back(QPointF::dotProduct(direction->normal, (line.first + line.second) / 2));
        }
    }
    QVector<QTransform> result;
    for (int first = 0; first < directions.size(); ++first) {
        for (int second = first+1; second < directions.size(); ++second) {
            const auto a = directions[first].normal, b = directions[second].normal;
            const double determinant = cross(a,b);
            if (std::abs(determinant) < kMinimumDirectionCross)
                continue;
            auto horizontal = uniqueOffsets(directions[first].offsets);
            auto vertical = uniqueOffsets(directions[second].offsets);
            const auto extend = [&](QVector<double> *values, QPointF normal, QPointF other) {
                if (values->size() >= 2)
                    return;
                for (const auto &line : lines)
                    if (std::abs(cross(line.normal, other)) < kParallelTolerance) {
                        values->push_back(QPointF::dotProduct(normal, line.first));
                        values->push_back(QPointF::dotProduct(normal, line.second));
                    }
                *values = uniqueOffsets(*values);
            };
            extend(&horizontal, a, b);
            extend(&vertical, b, a);
            const auto intersection = [&](double x, double y) {
                return QPointF((x*b.y()-a.y()*y)/determinant, (a.x()*y-x*b.x())/determinant);
            };
            for (int left = 0; left < horizontal.size(); ++left)
                for (int right = left+1; right < horizontal.size(); ++right)
                    for (int top = 0; top < vertical.size(); ++top)
                        for (int bottom = top+1; bottom < vertical.size(); ++bottom)
                            result.push_back(catalog::affineFromAnchors(
                                {square.bounds.topLeft(), square.bounds.topRight(), square.bounds.bottomLeft()},
                                {intersection(horizontal[left],vertical[top]), intersection(horizontal[right],vertical[top]),
                                 intersection(horizontal[left],vertical[bottom])}));
        }
    }

    return result;
}

} // namespace

QVector<ReusableCandidate> wholeShapeCandidates(const PenFillRequest &request,
    const catalog::Region &region, const QVector<catalog::Primitive> &primitives,
    const FillOptions &options, const std::function<bool()> &cancelled) {
    const auto conditioned = conditioningFrame(region);
    const auto restore = conditioned.inverted();
    auto contours = sampledContours(request, region);
    for (auto &polygon : contours) {
        polygon = conditioned.map(polygon);
        if (polygon.size() > kMaximumSamples) {
            QPolygonF retained;
            for (int index = 0; index < kMaximumSamples; ++index)
                retained.push_back(polygon[index*polygon.size()/kMaximumSamples]);
            polygon = retained;
        }
    }
    const auto coverage = catalog::leewayAdjustedRegion(region, options.leeway, options.boundaryAllowance);
    const auto envelopeBounds = coverage.permittedPath.boundingRect();
    struct ScoredCandidate { ReusableCandidate candidate; double score; bool compensated; };
    QVector<ScoredCandidate> candidates;
    const auto append = [&](const catalog::Primitive &primitive, const QTransform &transform, bool compensated = false) {
        if (cancelled && cancelled())
            return;
        auto restored = transform * restore;
        if (primitive.shape.shapeId == kSquareShapeId) {
            const auto bounds = restored.mapRect(primitive.shape.bounds);
            const auto center = bounds.center();
            const double coordinate = std::max({std::abs(bounds.left()), std::abs(bounds.right()),
                std::abs(bounds.top()), std::abs(bounds.bottom()), options.observationScale});
            const double clearance = kClearanceUlps * std::max(catalog::kVerificationClearance,
                coordinate * std::numeric_limits<float>::epsilon());
            QTransform expansion;
            expansion.translate(center.x(), center.y());
            expansion.scale(1 + clearance / bounds.width(), 1 + clearance / bounds.height());
            expansion.translate(-center.x(), -center.y());
            restored *= expansion;
        }
        const auto emitted = catalog::emittedTransform(restored);
        const auto polygons = catalog::mapped(primitive.shape, emitted);
        const auto bounds = catalog::painterPath(polygons).boundingRect();
        if (!envelopeBounds.adjusted(-catalog::kVerificationClearance, -catalog::kVerificationClearance,
            catalog::kVerificationClearance, catalog::kVerificationClearance).contains(bounds)
            || !catalog::subtract(polygons, coverage.permitted).isEmpty())
            return;
        const double score = catalog::area(catalog::intersect(polygons, coverage.required))
            - catalog::area(catalog::subtract(polygons, coverage.spillFree));
        if (score > 0)
            candidates.push_back({{{primitive.shape.shapeId, emitted}, polygons, bounds}, score, compensated});
    };
    const auto appendEllipse = [&](const catalog::Primitive &primitive, const QTransform &transform) {
        const double compensation = 1 / std::cos(std::numbers::pi / primitive.shape.contours.front().size()) - 1;
        const auto center = primitive.shape.bounds.center();
        for (double fraction : {0.0, 0.5, 1.0}) {
            QTransform scale;
            scale.translate(center.x(), center.y());
            scale.scale(1 + fraction * compensation, 1 + fraction * compensation);
            scale.translate(-center.x(), -center.y());
            append(primitive, scale * transform, fraction > 0);
        }
    };
    for (const auto &primitive : primitives) {
        if (!catalog::usesTask(primitive, catalog::ShapeTask::WholeRegion)
            && !catalog::usesTask(primitive, catalog::ShapeTask::Interior))
            continue;
        if (primitive.shape.shapeId == kSquareShapeId) {
            for (const auto &transform : parallelograms(straightSegments(request, contours, conditioned), primitive.shape))
                append(primitive, transform);
        } else if (primitive.shape.shapeId == kCircleShapeId) {
            const auto center = primitive.shape.bounds.center();
            const double radius = primitive.shape.bounds.width()/2;
            const auto vertex = primitive.shape.contours.front().front()-center;
            const double phase = std::atan2(vertex.y(),vertex.x());
            const double interval = 2*std::numbers::pi / primitive.shape.contours.front().size();
            for (const auto &ellipse : ellipseModels(contours, cancelled)) {
                for (int index = 0; index < kEllipsePhases; ++index) {
                    QTransform transform;
                    transform.rotateRadians(-phase + index*interval/kEllipsePhases);
                    transform.scale(1/radius, 1/radius);
                    transform.translate(-center.x(), -center.y());
                    appendEllipse(primitive, transform * ellipse.unitCircle);
                }
                for (const auto &polygon : contours) {
                    const auto point = *std::min_element(polygon.begin(), polygon.end(), [&](const auto &left, const auto &right) {
                        return std::abs(squaredLength(ellipse.inverse.map(left))-1)
                            < std::abs(squaredLength(ellipse.inverse.map(right))-1);
                    });
                    const auto relative = ellipse.inverse.map(point);
                    QTransform transform;
                    transform.translate(-center.x(), -center.y());
                    transform.scale(1/radius,1/radius);
                    transform.rotateRadians(std::atan2(relative.y(), relative.x())-phase);
                    appendEllipse(primitive, transform * ellipse.unitCircle);
                }
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) { return left.score > right.score; });
    QVector<ReusableCandidate> result;
    QHash<int,int> counts;
    for (auto &[candidate, score, compensated] : candidates) {
        const int key = 2 * candidate.placement.shapeId + int(compensated);
        if (counts[key] >= kCandidatesPerShape)
            continue;
        ++counts[key];
        result.push_back(std::move(candidate));
    }

    return result;
}

} // namespace gui::compact
