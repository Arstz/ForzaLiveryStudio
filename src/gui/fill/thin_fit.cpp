#include "thin_fit.h"
#include "compact_fit_catalog_internal.h"
#include "compact_fit_quality.h"
#include "compact_fit_reduction.h"
#include "lining_fill.h"
#include "profile_fit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <numeric>

namespace gui::thin {
namespace {

constexpr double kMinimumScale = 0.02;
constexpr double kMaximumScale = 1.0;
constexpr double kScaleWidthFraction = 0.2;
constexpr double kMinimumAllowance = 0.001;
constexpr double kToleranceWidthFraction = 0.01;
constexpr double kStrokeMinimumFlatteningScale = 512.0;
constexpr double kStrokeMaximumFlatteningScale = 4096.0;
constexpr double kStrokeWidthFlatteningScale = 8.0;
constexpr double kStrokeCurveThreshold = 0.0001;
constexpr int kExpansionRounds = 3;
constexpr double kGrowthStep = 0.075;
constexpr double kJoinExtensionWidthFraction = 0.12;
constexpr int kNativeReplacementRounds = 2;
constexpr int kNativeReplacementTrials = 512;
constexpr int kNativeMergeTrials = 32;
constexpr int kNativeMergeAnchors = 24;
constexpr double kMinimumNativeMergeAreaRatio = 0.001;
constexpr double kNativeMissingPenalty = 4.0;
constexpr int kReplacementProbes = 16;
constexpr int kSpanProbeCount = 32;
constexpr double kSpanProbeWidthFraction = 0.35;
constexpr double kSpanProbeCoverage = 0.9;
constexpr int kMaximumGapRectangles = 32;
constexpr int kMaximumContinuityRectangles = 64;
constexpr int kMaximumGapSplits = 6;
constexpr double kMinimumGapAreaRatio = 0.0005;
constexpr int kMergeTrials = 1000;

constexpr int kBoundaryLeafSize = 8;
constexpr int kEnvelopeCircleSamples = 16;
constexpr double kMaximumThicknessRatio = 2.0;
constexpr double kRayEpsilon = catalog::kVerificationClearance;
constexpr double kRaySideCosine = 0.5;
constexpr double kRaySideSine = 0.8660254037844386;
constexpr double kMinimumVisibleWidthFraction = 0.3;
constexpr double kMinimumContributionAreaRatio = 0.0001;
constexpr double kRedundantOverlapFraction = 0.03;
constexpr double kProtectedCoreWidthFraction = 0.06;
constexpr int kPlacementEnvelopeProbes = 32;

bool probesInside(const PenPrimitive &shape, const QTransform &transform,
                  const catalog::PointContainment &envelope) {
    for (const auto &polygon : shape.contours) {
        const int stride = std::max(1, int(polygon.size()) / kPlacementEnvelopeProbes);
        const int subdivisions = std::max(1, kPlacementEnvelopeProbes / int(polygon.size()));
        for (int index = 0; index < polygon.size(); index += stride)
            for (int sample = 0; sample < subdivisions; ++sample) {
                const auto point = polygon[index] + (polygon[(index + 1) % polygon.size()] - polygon[index])
                    * (double(sample) / subdivisions);
                if (!envelope.contains(transform.map(point)))
                    return false;
            }
    }

    return true;
}

struct Topology {
    int components = 0;
    int holes = 0;
};

Topology topologyOf(const catalog::Polygons &polygons) {
    Topology result;

    for (const auto &polygon : polygons)
        if (catalog::signedArea(polygon) > 0.0)
            ++result.components;
        else
            ++result.holes;

    return result;
}

class BoundarySegments {
public:
    explicit BoundarySegments(const catalog::Polygons &polygons) {
        for (const auto &polygon : polygons)
            for (int index = 0; index < polygon.size(); ++index) {
                const auto start = polygon[index];
                const auto end = polygon[(index + 1) % polygon.size()];
                if (QLineF(start, end).length() > kRayEpsilon)
                    segments_.push_back({start, end, QRectF(start, end).normalized()});
            }
        if (!segments_.isEmpty())
            build(0, segments_.size());
    }

    double width(const QPointF &point, const QPointF &inward, double maximum) const {
        const auto side = QPointF(-inward.y(), inward.x());
        double result = maximum;

        for (const auto &direction : {inward, inward * kRaySideCosine + side * kRaySideSine,
                inward * kRaySideCosine - side * kRaySideSine})
            if (!nodes_.isEmpty())
                crossing(0, point, direction, &result);

        return result;
    }

private:
    struct Segment {
        QPointF start;
        QPointF end;
        QRectF bounds;
    };
    struct Node {
        QRectF bounds;
        int first = 0;
        int count = 0;
        int left = -1;
        int right = -1;
    };

    int build(int first, int count) {
        QRectF bounds = segments_[first].bounds;
        const int index = nodes_.size();

        for (int offset = 1; offset < count; ++offset) {
            const auto &next = segments_[first + offset].bounds;
            bounds = QRectF(QPointF(std::min(bounds.left(), next.left()), std::min(bounds.top(), next.top())),
                QPointF(std::max(bounds.right(), next.right()), std::max(bounds.bottom(), next.bottom())));
        }
        nodes_.push_back({bounds, first, count});
        if (count > kBoundaryLeafSize) {
            const int middle = first + count / 2;
            const bool horizontal = bounds.width() >= bounds.height();
            std::nth_element(segments_.begin() + first, segments_.begin() + middle, segments_.begin() + first + count,
                [horizontal](const auto &left, const auto &right) {
                    return horizontal ? left.bounds.center().x() < right.bounds.center().x()
                        : left.bounds.center().y() < right.bounds.center().y();
                });
            const int left = build(first, middle - first);
            const int right = build(middle, first + count - middle);
            nodes_[index].left = left;
            nodes_[index].right = right;
        }

        return index;
    }

    static double cross(const QPointF &first, const QPointF &second) {
        return first.x() * second.y() - first.y() * second.x();
    }

    static double rayEntry(const QRectF &bounds, const QPointF &origin, const QPointF &direction, double maximum) {
        double near = 0.0;
        double far = maximum;

        for (bool horizontal : {true, false}) {
            const double position = horizontal ? origin.x() : origin.y();
            const double delta = horizontal ? direction.x() : direction.y();
            const double lower = horizontal ? bounds.left() : bounds.top();
            const double upper = horizontal ? bounds.right() : bounds.bottom();
            if (std::abs(delta) < kRayEpsilon) {
                if (position < lower || position > upper)
                    return std::numeric_limits<double>::infinity();
                continue;
            }
            double start = (lower - position) / delta;
            double end = (upper - position) / delta;
            if (start > end)
                std::swap(start, end);
            near = std::max(near, start);
            far = std::min(far, end);
            if (near > far)
                return std::numeric_limits<double>::infinity();
        }

        return near;
    }

    void crossing(int index, const QPointF &point, const QPointF &direction, double *distance) const {
        const auto &node = nodes_[index];

        if (rayEntry(node.bounds, point, direction, *distance) > *distance)
            return;
        if (node.left >= 0) {
            const double left = rayEntry(nodes_[node.left].bounds, point, direction, *distance);
            const double right = rayEntry(nodes_[node.right].bounds, point, direction, *distance);
            crossing(left <= right ? node.left : node.right, point, direction, distance);
            crossing(left <= right ? node.right : node.left, point, direction, distance);
            return;
        }
        for (int offset = 0; offset < node.count; ++offset) {
            const auto &segment = segments_[node.first + offset];
            const auto edge = segment.end - segment.start;
            const auto delta = segment.start - point;
            const double determinant = cross(direction, edge);
            if (std::abs(determinant) < kRayEpsilon * QLineF({}, edge).length())
                continue;
            const double along = cross(delta, edge) / determinant;
            const double across = cross(delta, direction) / determinant;
            if (along > kRayEpsilon && along < *distance && across >= 0.0 && across <= 1.0)
                *distance = along;
        }
    }

    QVector<Segment> segments_;
    QVector<Node> nodes_;
};

double meanWidth(const catalog::Polygons &polygons);

catalog::Polygons thicknessEnvelope(const catalog::Polygons &polygons, double allowance,
                                    double maximumThicknessRatio, double knownWidth,
                                    const std::function<bool()> &cancelled) {
    catalog::Polygons parts = polygons;
    const BoundarySegments boundary(polygons);
    const auto bounds = catalog::painterPath(polygons).boundingRect();
    const double maximum = std::hypot(bounds.width(), bounds.height());
    const double fraction = (maximumThicknessRatio - 1.0) * 0.5;
    const double minimumWidth = meanWidth(polygons) * kMinimumVisibleWidthFraction;

    for (const auto &polygon : polygons) {
        QVector<double> radii(polygon.size());
        for (int index = 0; index < polygon.size(); ++index) {
            if (cancelled && cancelled())
                return {};
            const auto edge = polygon[(index + 1) % polygon.size()] - polygon[index];
            const double length = QLineF({}, edge).length();
            const auto inward = length > kRayEpsilon ? QPointF(-edge.y(), edge.x()) / length : QPointF();
            const double width = knownWidth > 0.0 ? knownWidth
                : boundary.width(polygon[index] + edge * 0.5, inward, maximum);
            radii[index] = std::min(allowance, std::max(kRayEpsilon, std::max(minimumWidth, width) * fraction));
        }
        auto vertexRadii = radii;
        for (int index = 0; index < polygon.size(); ++index)
            vertexRadii[index] = std::min(radii[index], radii[(index + polygon.size() - 1) % polygon.size()]);
        for (int index = 0; index < polygon.size(); ++index) {
            const int next = (index + 1) % polygon.size();
            const auto edge = polygon[next] - polygon[index];
            const double length = QLineF({}, edge).length();
            if (length <= kRayEpsilon)
                continue;
            const auto normal = QPointF(-edge.y(), edge.x()) / length;
            parts.push_back({polygon[index] + normal * vertexRadii[index], polygon[index] - normal * vertexRadii[index],
                polygon[next] - normal * vertexRadii[next], polygon[next] + normal * vertexRadii[next]});
            QPolygonF circle;
            for (int sample = 0; sample < kEnvelopeCircleSamples; ++sample) {
                const double angle = sample * 2.0 * std::acos(-1.0) / kEnvelopeCircleSamples;
                circle.push_back(polygon[index] + QPointF(std::cos(angle), std::sin(angle)) * vertexRadii[index]);
            }
            parts.push_back(circle);
        }
    }

    return catalog::unite(parts);
}

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

catalog::Polygons coverageOf(const QVector<PenPlacement> &placements,
                             const QVector<catalog::Primitive> &primitives) {
    catalog::Polygons result;

    for (const auto &placement : placements) {
        const auto found = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &primitive) {
            return primitive.shape.shapeId == placement.shapeId;
        });
        if (found == primitives.cend())
            throw std::runtime_error("Thin fit placement geometry is unavailable");
        result += catalog::mapped(found->shape, placement.transform);
    }

    return catalog::unite(result);
}

double meanWidth(const catalog::Polygons &polygons) {
    double perimeter = 0.0;

    for (const auto &polygon : polygons)
        for (int index = 0; index < polygon.size(); ++index)
            perimeter += QLineF(polygon[index], polygon[(index + 1) % polygon.size()]).length();

    return perimeter > 0.0 ? 2.0 * catalog::area(polygons) / perimeter : 0.0;
}

catalog::Polygons normalizedStroke(const QPainterPath &stroke, double width) {
    catalog::Polygons polygons;
    const double scale = std::clamp(kStrokeWidthFlatteningScale / width,
        kStrokeMinimumFlatteningScale, kStrokeMaximumFlatteningScale);
    const auto inverse = QTransform::fromScale(1.0 / scale, 1.0 / scale);

    for (const auto &polygon : stroke.toSubpathPolygons(QTransform::fromScale(scale, scale)))
        polygons.push_back(inverse.map(polygon));

    return catalog::expanded(catalog::unite(polygons), 1.0 / scale);
}

catalog::Region polygonRegion(const catalog::Polygons &polygons) {
    catalog::Region result;
    result.required = catalog::unite(polygons);
    result.visible = result.required;
    result.spillFree = result.required;
    result.requiredPath = catalog::painterPath(result.required);
    result.spillFreePath = result.requiredPath;
    result.bounds = result.requiredPath.boundingRect();
    result.area = catalog::area(result.required);
    result.originalArea = result.area;
    result.tolerance = std::max(kMinimumAllowance, meanWidth(result.required) * kToleranceWidthFraction);
    result.permitted = catalog::expanded(result.required, result.tolerance);
    result.permittedPath = catalog::painterPath(result.permitted);
    result.spillFreeContainment = std::make_shared<catalog::PointContainment>(result.spillFree);

    return result;
}

std::optional<PenPlacement> enclosingRectangle(const catalog::Polygons &polygons,
                                              const PenPrimitive &square, const catalog::Region &region,
                                              const catalog::PointContainment &envelope) {
    QPolygonF points;
    for (const auto &polygon : polygons)
        points += polygon;
    const auto hull = catalog::convexHull(points);
    QVector<std::pair<double, QTransform>> proposals;
    const std::array<QPointF, 3> source{square.bounds.topLeft(), square.bounds.topRight(), square.bounds.bottomLeft()};

    for (int edge = 0; edge < hull.size(); ++edge) {
        const auto along = hull[(edge + 1) % hull.size()] - hull[edge];
        const double length = QLineF({}, along).length();
        if (length <= 0.0)
            continue;
        const auto horizontal = along / length;
        const auto vertical = QPointF(-horizontal.y(), horizontal.x());
        double left = std::numeric_limits<double>::infinity();
        double right = -left;
        double top = left;
        double bottom = -left;
        for (const auto &point : hull) {
            const double x = QPointF::dotProduct(point, horizontal);
            const double y = QPointF::dotProduct(point, vertical);
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
        const double clearance = catalog::kVerificationClearance * 2.0;
        const auto origin = horizontal * (left - clearance) + vertical * (top - clearance);
        const std::array<QPointF, 3> target{origin,
            origin + horizontal * (right - left + clearance * 2.0),
            origin + vertical * (bottom - top + clearance * 2.0)};
        proposals.push_back({(right - left) * (bottom - top), catalog::affineFromAnchors(source, target)});
    }
    std::sort(proposals.begin(), proposals.end(), [](const auto &first, const auto &second) {
        return first.first < second.first;
    });
    for (int index = 0; index < std::min(3, int(proposals.size())); ++index) {
        PenPlacement result;
        result.shapeId = square.shapeId;
        result.transform = catalog::emittedTransform(proposals[index].second);
        if (!probesInside(square, result.transform, envelope))
            continue;
        const auto coverage = catalog::mapped(square, result.transform);
        if (catalog::subtract(polygons, coverage).isEmpty()
            && catalog::subtract(catalog::expanded(coverage, catalog::kVerificationClearance), region.permitted).isEmpty())
            return result;
    }

    return {};
}

int reducePlacements(QVector<PenPlacement> *placements, const catalog::Region &region,
                      const QVector<catalog::Primitive> &primitives, double maximumMissing,
                      const std::function<bool()> &cancelled) {
    QVector<catalog::Polygons> polygons;
    QVector<QRectF> bounds;
    QVector<double> areas;
    QVector<bool> retained(placements->size(), true);
    QVector<int> order(placements->size());
    const auto core = catalog::interiorSupport(region.visible, meanWidth(region.visible) * kProtectedCoreWidthFraction);
    const int originalCount = placements->size();
    double missing = catalog::area(catalog::subtract(region.visible, coverageOf(*placements, primitives)));

    if (missing > maximumMissing)
        return 0;
    for (const auto &placement : *placements) {
        const auto geometry = coverageOf({placement}, primitives);
        polygons.push_back(geometry);
        bounds.push_back(catalog::painterPath(geometry).boundingRect());
        areas.push_back(catalog::area(geometry));
    }
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int first, int second) {
        return areas[first] < areas[second];
    });
    for (int index : order) {
        if (stopped(cancelled))
            break;
        catalog::Polygons others;
        for (int other = 0; other < polygons.size(); ++other)
            if (other != index && retained[other] && bounds[index].intersects(bounds[other]))
                others += polygons[other];
        others = catalog::unite(others);
        const auto privateCoverage = catalog::intersect(catalog::subtract(polygons[index], others), region.visible);
        const double privateArea = catalog::area(privateCoverage);
        if (missing + privateArea > maximumMissing)
            continue;
        if (!catalog::intersect(privateCoverage, core).isEmpty()) {
            const double usefulArea = catalog::area(catalog::intersect(polygons[index], region.visible));
            const double minimumContribution = std::max(region.area * kMinimumContributionAreaRatio,
                usefulArea * kRedundantOverlapFraction);
            if (privateArea > minimumContribution)
                continue;
        }
        const auto before = topologyOf(catalog::unite(others + polygons[index]));
        const auto after = topologyOf(others);
        if (after.components > before.components || after.holes != before.holes)
            continue;
        retained[index] = false;
        missing += privateArea;
    }
    QVector<PenPlacement> result;
    for (int index = 0; index < placements->size(); ++index)
        if (retained[index])
            result.push_back((*placements)[index]);
    *placements = std::move(result);

    return originalCount - placements->size();
}

void expandPlacements(QVector<PenPlacement> *placements, const catalog::Region &region,
                      const QVector<catalog::Primitive> &primitives,
                      const catalog::Polygons &preferredCoverage,
                      const std::function<bool()> &cancelled) {
    QVector<catalog::Polygons> polygons;
    QVector<QRectF> bounds;
    const auto core = catalog::interiorSupport(region.visible, meanWidth(region.visible) * kProtectedCoreWidthFraction);
    const catalog::PointContainment envelope(region.permitted);
    const double width = meanWidth(region.visible);
    auto missing = catalog::subtract(preferredCoverage, coverageOf(*placements, primitives));
    double missingArea = catalog::area(missing);

    for (const auto &placement : *placements) {
        polygons.push_back(coverageOf({placement}, primitives));
        bounds.push_back(catalog::painterPath(polygons.back()).boundingRect());
    }
    for (int round = 0; round < kExpansionRounds && !missing.isEmpty() && !stopped(cancelled); ++round) {
        bool changed = false;
        for (int index = 0; index < placements->size() && !stopped(cancelled); ++index) {
            const auto &placement = (*placements)[index];
            const auto shape = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &primitive) {
                return primitive.shape.shapeId == placement.shapeId;
            });
            const auto center = placement.transform.mapRect(shape->shape.bounds).center();
            const auto horizontal = placement.transform.map(shape->shape.bounds.topRight())
                - placement.transform.map(shape->shape.bounds.topLeft());
            const auto vertical = placement.transform.map(shape->shape.bounds.bottomLeft())
                - placement.transform.map(shape->shape.bounds.topLeft());
            const auto axis = QLineF({}, horizontal).length() >= QLineF({}, vertical).length() ? horizontal : vertical;
            PenPlacement bestPlacement;
            catalog::Polygons bestPolygons;
            catalog::Polygons bestExposed;
            const auto neighborhood = bounds[index].adjusted(-width, -width, width, width);
            const auto localMissing = catalog::intersect(missing, {QPolygonF(neighborhood)});
            double bestArea = missingArea;

            const bool hasNeighbor = std::any_of(bounds.cbegin(), bounds.cend(), [&](const auto &other) {
                const auto contact = other.adjusted(-width, -width, width, width);
                return &other != &bounds[index]
                    && (contact.contains(center - axis * 0.5) || contact.contains(center + axis * 0.5));
            });
            if (localMissing.isEmpty())
                continue;
            for (bool extendJoins : {false, true}) {
                if (extendJoins && !hasNeighbor)
                    continue;
                QTransform growth;
                const double angle = std::atan2(axis.y(), axis.x()) * 180.0 / std::acos(-1.0);
                growth.translate(center.x(), center.y());
                growth.rotate(angle);
                growth.scale(1.0 + (extendJoins ? width * kJoinExtensionWidthFraction : 0.0)
                    / std::max(kRayEpsilon, QLineF({}, axis).length()), extendJoins ? 1.0 : 1.0 + kGrowthStep);
                growth.rotate(-angle);
                growth.translate(-center.x(), -center.y());
                auto proposed = placement;
                proposed.transform = catalog::emittedTransform(placement.transform * growth);
                if (!probesInside(shape->shape, proposed.transform, envelope))
                    continue;
                const auto addition = catalog::mapped(shape->shape, proposed.transform);
                const auto gain = catalog::intersect(addition, localMissing);
                if (gain.isEmpty() || !catalog::subtract(addition, region.permitted).isEmpty())
                    continue;
                const auto lost = catalog::intersect(catalog::subtract(polygons[index], addition), preferredCoverage);
                catalog::Polygons exposed;
                if (!lost.isEmpty()) {
                    catalog::Polygons others;
                    const auto lostBounds = catalog::painterPath(lost).boundingRect();
                    for (int other = 0; other < polygons.size(); ++other)
                        if (other != index && lostBounds.intersects(bounds[other]))
                            others += polygons[other];
                    exposed = catalog::subtract(lost, catalog::unite(others));
                }
                if (!catalog::intersect(exposed, placements->size() == 1 ? region.visible : core).isEmpty())
                    continue;
                const double nextArea = missingArea - catalog::area(gain) + catalog::area(exposed);
                if (nextArea >= bestArea)
                    continue;
                bestPlacement = proposed;
                bestPolygons = addition;
                bestExposed = exposed;
                bestArea = nextArea;
                break;
            }
            if (bestArea < missingArea) {
                (*placements)[index] = bestPlacement;
                polygons[index] = bestPolygons;
                bounds[index] = catalog::painterPath(bestPolygons).boundingRect();
                missing = catalog::unite(catalog::subtract(missing, bestPolygons) + bestExposed);
                missingArea = catalog::area(missing);
                changed = true;
            }
        }
        if (!changed)
            break;
    }
}

class HullAnchors {
public:
    explicit HullAnchors(const QPolygonF &polygon) : polygon_(polygon) {
        QPointF weighted;
        double sum = 0.0;

        for (int index = 0; index < polygon.size(); ++index) {
            const auto &first = polygon[index];
            const auto &second = polygon[(index + 1) % polygon.size()];
            const double cross = first.x() * second.y() - first.y() * second.x();
            weighted += (first + second) * cross;
            sum += cross;
        }
        const auto center = sum != 0.0 ? weighted / (sum * 3.0) : QPointF();
        areas_.push_back(0.0);
        for (int index = 0; index < polygon.size(); ++index) {
            const auto first = polygon[index] - center;
            const auto second = polygon[(index + 1) % polygon.size()] - center;
            areas_.push_back(areas_.back() + std::abs(first.x() * second.y() - first.y() * second.x()));
        }
    }

    std::array<QPointF, 3> at(int offset, int direction) const {
        std::array<QPointF, 3> result;
        const double total = areas_.back();

        for (int anchor = 0; anchor < 3; ++anchor) {
            const double area = std::fmod(areas_[offset] + total * (1.0 + direction * anchor / 3.0), total);
            const int index = std::clamp(int(std::upper_bound(areas_.cbegin(), areas_.cend(), area)
                - areas_.cbegin()) - 1, 0, int(polygon_.size()) - 1);
            const double fraction = (area - areas_[index]) / (areas_[index + 1] - areas_[index]);
            result[anchor] = polygon_[index] * (1.0 - fraction)
                + polygon_[(index + 1) % polygon_.size()] * fraction;
        }

        return result;
    }

private:
    QPolygonF polygon_;
    QVector<double> areas_;
};

std::optional<PenPlacement> fitWholeRegion(const catalog::Region &region,
                                         const QVector<catalog::Primitive> &primitives,
                                         double maximumMissing, const std::function<bool()> &cancelled,
                                         catalog::ShapeTask task = catalog::ShapeTask::WholeRegion,
                                         int anchorLimit = std::numeric_limits<int>::max()) {
    std::optional<PenPlacement> best;
    QPolygonF targetPoints;
    for (const auto &polygon : region.required)
        targetPoints += polygon;
    const auto targetHull = catalog::convexHull(targetPoints);
    const HullAnchors targetAnchors(targetHull);
    const catalog::PointContainment envelope(region.permitted);
    const auto bounds = region.bounds;
    double bestCost = std::numeric_limits<double>::infinity();

    for (const auto &primitive : primitives) {
        if (!catalog::usesTask(primitive, task))
            continue;
        const std::array<QPointF, 3> source{primitive.shape.bounds.topLeft(),
            primitive.shape.bounds.topRight(), primitive.shape.bounds.bottomLeft()};
        QVector<QTransform> proposals;
        for (bool transpose : {false, true}) {
            for (bool flipHorizontal : {false, true}) {
                for (bool flipVertical : {false, true}) {
                    if (stopped(cancelled))
                        return {};
                    const auto origin = QPointF(flipHorizontal ? bounds.right() : bounds.left(),
                        flipVertical ? bounds.bottom() : bounds.top());
                    const auto horizontal = QPointF(flipHorizontal ? -bounds.width() : bounds.width(), 0.0);
                    const auto vertical = QPointF(0.0, flipVertical ? -bounds.height() : bounds.height());
                    const std::array<QPointF, 3> target{origin,
                        origin + (transpose ? vertical : horizontal), origin + (transpose ? horizontal : vertical)};
                    proposals.push_back(catalog::affineFromAnchors(source, target));
                }
            }
        }
        QPolygonF sourcePoints;
        for (const auto &polygon : primitive.shape.contours)
            sourcePoints += polygon;
        const auto sourceHull = catalog::convexHull(sourcePoints);
        if (sourceHull.size() >= 3 && targetHull.size() >= 3) {
            const HullAnchors sourceAnchors(sourceHull);
            const auto hullSource = sourceAnchors.at(0, 1);
            for (int offset = 0; offset < targetHull.size();
                    offset += std::max(1, int(targetHull.size()) / anchorLimit))
                for (int direction : {-1, 1}) {
                    proposals.push_back(catalog::affineFromAnchors(hullSource, targetAnchors.at(offset, direction)));
                }
        }
        if (sourceHull.size() == 3 && targetHull.size() == 3) {
            for (int offset = 0; offset < 3; ++offset)
                for (int direction : {-1, 1})
                    proposals.push_back(catalog::affineFromAnchors({sourceHull[0], sourceHull[1], sourceHull[2]},
                        {targetHull[offset], targetHull[(offset + direction + 3) % 3],
                            targetHull[(offset + 2 * direction + 6) % 3]}));
        }
        for (const auto &transform : proposals) {
            if (stopped(cancelled))
                return {};
            PenPlacement placement{primitive.shape.shapeId, catalog::emittedTransform(transform)};
            if (primitive.shape.area * std::abs(placement.transform.determinant()) < region.area - maximumMissing)
                continue;
            bool contained = true;
            for (const auto &polygon : primitive.shape.contours)
                for (int index = 0; index < polygon.size(); index += std::max(1, int(polygon.size()) / kSpanProbeCount))
                    if (!envelope.contains(placement.transform.map(polygon[index]))) {
                        contained = false;
                        break;
                    }
            if (!contained)
                continue;
            const auto coverage = catalog::mapped(primitive.shape, placement.transform);
            const double missing = catalog::area(catalog::subtract(region.visible, coverage));
            if (!catalog::subtract(coverage, region.permitted).isEmpty()
                || missing > maximumMissing)
                continue;
            const double spill = catalog::area(catalog::subtract(coverage, region.visible));
            const double cost = missing * kNativeMissingPenalty + spill;
            if (cost < bestCost) {
                best = placement;
                bestCost = cost;
            }
            if (cost < catalog::kVerificationClearance * catalog::kVerificationClearance)
                return best;
        }
    }

    return best;
}

QVector<PenPlacement> fitSeparateRegions(const catalog::Region &region,
                                       const QVector<catalog::Primitive> &primitives,
                                       double maximumMissing, const std::function<bool()> &cancelled) {
    QVector<PenPlacement> result;
    const auto topology = topologyOf(region.required);

    if (!region.leeway.isEmpty() || topology.components < 2)
        return result;
    for (const auto &polygon : region.required) {
        if (catalog::signedArea(polygon) <= 0.0)
            continue;
        auto local = region;
        local.required = catalog::intersect(region.required, {polygon});
        local.visible = local.required;
        local.area = catalog::area(local.required);
        local.bounds = catalog::painterPath(local.required).boundingRect();
        const auto placement = fitWholeRegion(local, primitives, maximumMissing * local.area / region.area, cancelled);
        if (!placement)
            return {};
        result.push_back(*placement);
    }

    return result;
}

std::optional<PenPlacement> fitWholeSpan(const QVector<PenPoint> &points, double width,
                                       const QVector<catalog::Primitive> &primitives,
                                       const catalog::Region &region, double maximumMissing, double allowance,
                                       const std::function<bool()> &cancelled, bool useGpu) {
    QVector<QPointF> probes;
    std::optional<PenPlacement> best;
    const auto path = buildLiningPath(points).centerline;
    const auto origin = path.pointAtPercent(0.5);
    const auto chord = points.back().position - points.front().position;
    const catalog::PointContainment envelope(region.permitted);
    QVector<QTransform> adjustments;
    const double angle = std::atan2(chord.y(), chord.x()) * 180.0 / std::acos(-1.0);
    const double length = path.length();
    double bestSpill = std::numeric_limits<double>::infinity();

    for (int index = 0; index < kSpanProbeCount; ++index) {
        const double fraction = path.percentAtLength(length * (index + 0.5) / kSpanProbeCount);
        const auto center = path.pointAtPercent(fraction);
        const double tangent = path.angleAtPercent(fraction) * std::acos(-1.0) / 180.0;
        const QPointF normal(std::sin(tangent), std::cos(tangent));
        probes += QVector<QPointF>{center, center + normal * (width * kSpanProbeWidthFraction),
            center - normal * (width * kSpanProbeWidthFraction)};
    }
    for (double along : {1.0, 1.01, 1.03, 1.06}) {
        for (double across : {0.9, 0.95, 1.0, 1.05, 1.1}) {
            for (double offset : {-0.5, -0.25, 0.0, 0.25, 0.5}) {
                QTransform adjustment;
                adjustment.translate(origin.x(), origin.y());
                adjustment.rotate(angle);
                adjustment.translate(0.0, allowance * offset);
                adjustment.scale(along, across);
                adjustment.rotate(-angle);
                adjustment.translate(-origin.x(), -origin.y());
                adjustments.push_back(adjustment);
            }
        }
    }
    for (const auto &candidate : profile::spanCandidates(path, width, primitives,
            allowance + width * 0.5, useGpu, cancelled)) {
        const auto shape = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &primitive) {
            return primitive.shape.shapeId == candidate.shapeId;
        });
        for (const auto &adjustment : adjustments) {
            if (stopped(cancelled))
                return {};
            auto placement = candidate;
            placement.transform = catalog::emittedTransform(candidate.transform * adjustment);
            bool contained = true;
            for (const auto &polygon : shape->shape.contours) {
                const int stride = std::max(1, int(polygon.size()) / kSpanProbeCount);
                for (int index = 0; index < polygon.size(); index += stride) {
                    if (!envelope.contains(placement.transform.map(polygon[index]))) {
                        contained = false;
                        break;
                    }
                }
                if (!contained)
                    break;
            }
            if (!contained)
                continue;
            bool invertible = false;
            const auto inverse = placement.transform.inverted(&invertible);
            if (!invertible)
                continue;
            int missed = 0;
            for (const auto &point : probes) {
                missed += !shape->shape.silhouette.contains(inverse.map(point));
                if (missed > probes.size() * (1.0 - kSpanProbeCoverage))
                    break;
            }
            if (missed > probes.size() * (1.0 - kSpanProbeCoverage))
                continue;
            const auto coverage = catalog::mapped(shape->shape, placement.transform);
            if (!catalog::subtract(coverage, region.permitted).isEmpty()
                || catalog::area(catalog::subtract(region.visible, coverage)) > maximumMissing)
                continue;
            const double spill = catalog::area(catalog::subtract(coverage, region.visible));
            if (spill < bestSpill) {
                best = placement;
                bestSpill = spill;
            }
        }
    }

    return best;
}

void repairContinuity(const catalog::Polygons &gap, const PenPrimitive &square,
                      const catalog::Region &region, const catalog::PointContainment &envelope,
                      QVector<PenPlacement> *placements,
                      catalog::Polygons *missing, int shapeBudget, int *added, int depth,
                      const std::function<bool()> &cancelled) {
    const auto required = catalog::intersect(gap, *missing);
    const auto bounds = catalog::painterPath(required).boundingRect();

    if (required.isEmpty() || placements->size() >= shapeBudget
        || *added >= kMaximumContinuityRectangles || stopped(cancelled))
        return;
    const auto rectangle = enclosingRectangle(required, square, region, envelope);
    if (rectangle) {
        placements->push_back(*rectangle);
        *missing = catalog::subtract(*missing, catalog::mapped(square, rectangle->transform));
        ++*added;
        return;
    }
    if (depth >= kMaximumGapSplits)
        return;
    auto first = bounds;
    auto second = bounds;
    if (bounds.width() >= bounds.height()) {
        first.setRight(bounds.center().x());
        second.setLeft(bounds.center().x());
    } else {
        first.setBottom(bounds.center().y());
        second.setTop(bounds.center().y());
    }
    for (const auto &part : {first, second})
        repairContinuity(catalog::intersect(required, {QPolygonF(part)}), square, region, envelope,
            placements, missing, shapeBudget, added, depth + 1, cancelled);
}

void repairCoverage(QVector<PenPlacement> *placements, const catalog::Region &region,
                     const QVector<catalog::Primitive> &primitives, int shapeBudget,
                     double maximumMissing, const std::function<bool()> &cancelled) {
    const auto square = std::find_if(primitives.cbegin(), primitives.cend(), [](const auto &primitive) {
        return primitive.shape.shapeId == 101 && catalog::usesTask(primitive, catalog::ShapeTask::GapPatches);
    });
    auto coverage = coverageOf(*placements, primitives);
    auto missing = catalog::subtract(region.visible, coverage);
    QVector<std::pair<double, PenPlacement>> proposals;
    const catalog::PointContainment envelope(region.permitted);
    const double minimumGain = region.area * kMinimumGapAreaRatio;
    int continuityRectangles = 0;

    if (square == primitives.cend())
        return;
    for (auto polygon : coverage) {
        if (stopped(cancelled) || placements->size() >= shapeBudget)
            return;
        if (catalog::signedArea(polygon) >= 0.0)
            continue;
        std::reverse(polygon.begin(), polygon.end());
        const auto hole = catalog::subtract({polygon}, region.permitted).isEmpty()
            ? catalog::Polygons{polygon} : catalog::intersect({polygon}, missing);
        if (hole.isEmpty())
            continue;
        auto repairTarget = catalog::unite(missing + hole);
        repairContinuity(hole, square->shape, region, envelope, placements, &repairTarget,
            shapeBudget, &continuityRectangles, 0, cancelled);
        missing = catalog::intersect(repairTarget, region.visible);
    }
    for (const auto &polygon : catalog::Polygons(missing)) {
        if (catalog::signedArea(polygon) <= 0.0)
            continue;
        const auto contact = catalog::expanded({polygon}, catalog::kVerificationClearance * 4.0);
        int components = 0;
        for (const auto &component : coverage)
            if (catalog::signedArea(component) > 0.0 && !catalog::intersect(contact, {component}).isEmpty())
                ++components;
        if (components >= 2)
            repairContinuity({polygon}, square->shape, region, envelope, placements, &missing,
                shapeBudget, &continuityRectangles, 0, cancelled);
    }
    if (catalog::area(missing) <= maximumMissing)
        return;
    for (const auto &polygon : missing) {
        if (stopped(cancelled))
            return;
        if (catalog::signedArea(polygon) < minimumGain)
            continue;
        const auto rectangle = enclosingRectangle({polygon}, square->shape, region, envelope);
        if (rectangle)
            proposals.push_back({catalog::area(catalog::intersect(
                catalog::mapped(square->shape, rectangle->transform), missing)), *rectangle});
    }
    std::sort(proposals.begin(), proposals.end(), [](const auto &first, const auto &second) {
        return first.first > second.first;
    });
    int added = 0;
    for (const auto &proposal : proposals) {
        if (stopped(cancelled) || catalog::area(missing) <= maximumMissing
            || placements->size() >= shapeBudget || added >= kMaximumGapRectangles)
            break;
        const auto addition = catalog::mapped(square->shape, proposal.second.transform);
        if (catalog::area(catalog::intersect(addition, missing)) < minimumGain)
            continue;
        placements->push_back(proposal.second);
        missing = catalog::subtract(missing, addition);
        ++added;
    }
}

void mergePlacements(QVector<PenPlacement> *placements, const catalog::Region &region,
                     const QVector<catalog::Primitive> &primitives,
                     const std::function<bool()> &cancelled) {
    const auto square = std::find_if(primitives.cbegin(), primitives.cend(), [](const auto &primitive) {
        return primitive.shape.shapeId == 101 && catalog::usesTask(primitive, catalog::ShapeTask::ExactReplacements);
    });
    struct Pair {
        int first;
        int second;
        double distance;
    };
    QVector<catalog::Polygons> polygons;
    QVector<QRectF> bounds;
    QVector<int> owner(placements->size());
    QVector<Pair> pairs;
    const catalog::PointContainment envelope(region.permitted);
    int trials = 0;
    int nativeTrials = 0;

    if (square == primitives.cend() && catalog::shapeIdsForTask(primitives,
            catalog::ShapeTask::GroupReplacements).isEmpty())
        return;
    std::iota(owner.begin(), owner.end(), 0);
    for (const auto &placement : *placements) {
        const auto required = catalog::intersect(coverageOf({placement}, primitives), region.visible);
        polygons.push_back(required);
        bounds.push_back(catalog::painterPath(required).boundingRect());
    }
    for (int index = 0; index < placements->size(); ++index)
        for (int other = index + 1; other < placements->size(); ++other)
            if (bounds[index].adjusted(-region.tolerance, -region.tolerance, region.tolerance, region.tolerance).intersects(bounds[other]))
                pairs.push_back({index, other, QLineF(bounds[index].center(), bounds[other].center()).length()});
    std::sort(pairs.begin(), pairs.end(), [](const auto &first, const auto &second) {
        return first.distance < second.distance;
    });
    const auto root = [&](int index) {
        while (owner[index] != index)
            index = owner[index];

        return index;
    };
    for (const auto &pair : pairs) {
        if (trials >= kMergeTrials || stopped(cancelled))
            break;
        const int first = root(pair.first);
        const int second = root(pair.second);
        if (first == second)
            continue;
        ++trials;
        const auto required = catalog::unite(polygons[first] + polygons[second]);
        auto replacement = square == primitives.cend() ? std::optional<PenPlacement>{}
            : enclosingRectangle(required, square->shape, region, envelope);
        if (!replacement && nativeTrials < kNativeMergeTrials
            && catalog::area(required) >= region.area * kMinimumNativeMergeAreaRatio) {
            ++nativeTrials;
            auto local = region;
            local.required = required;
            local.visible = required;
            local.area = catalog::area(required);
            local.bounds = catalog::painterPath(required).boundingRect();
            replacement = fitWholeRegion(local, primitives, 0.0, cancelled,
                catalog::ShapeTask::GroupReplacements, kNativeMergeAnchors);
        }
        if (!replacement)
            continue;
        (*placements)[first] = *replacement;
        polygons[first] = required;
        owner[second] = first;
    }
    QVector<PenPlacement> result;
    for (int index = 0; index < placements->size(); ++index)
        if (owner[index] == index)
            result.push_back((*placements)[index]);
    *placements = std::move(result);
}

int replaceNativeGroups(QVector<PenPlacement> *placements, const catalog::Region &region,
                        const QVector<catalog::Primitive> &primitives,
                        const QVector<compact::ReusableCandidate> &candidates,
                        const std::function<bool()> &cancelled) {
    QVector<int> order(candidates.size());
    QVector<catalog::Polygons> pieces;
    QVector<catalog::Polygons> exclusive;
    QVector<QRectF> bounds;
    QVector<QRectF> exclusiveBounds;
    QVector<bool> retained;
    compact::CoverageOwnership ownership;
    int removed = 0;
    int trials = 0;

    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int first, int second) {
        return candidates[first].placement.area > candidates[second].placement.area;
    });
    for (int round = 0; round < kNativeReplacementRounds && !stopped(cancelled); ++round) {
        pieces.clear();
        bounds.clear();
        exclusive.clear();
        exclusiveBounds.clear();
        retained = QVector<bool>(placements->size(), true);
        for (const auto &placement : *placements) {
            pieces.push_back(coverageOf({placement}, primitives));
            bounds.push_back(catalog::painterPath(pieces.back()).boundingRect());
        }
        ownership.synchronize(pieces);
        for (int index = 0; index < pieces.size(); ++index) {
            exclusive.push_back(catalog::intersect(ownership.exclusive({index}), region.visible));
            exclusiveBounds.push_back(catalog::painterPath(exclusive.back()).boundingRect());
        }
        QVector<PenPlacement> additions;
        for (int candidateIndex : order) {
            if (trials >= kNativeReplacementTrials || stopped(cancelled))
                break;
            const auto &candidate = candidates[candidateIndex];
            const auto primitive = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &entry) {
                return entry.shape.shapeId == candidate.placement.shapeId
                    && catalog::usesTask(entry, catalog::ShapeTask::GroupReplacements);
            });
            if (primitive == primitives.cend())
                continue;
            const auto inverse = candidate.placement.transform.inverted();
            QVector<int> members;
            for (int index = 0; index < pieces.size(); ++index) {
                if (!retained[index] || exclusive[index].isEmpty()
                    || !candidate.bounds.adjusted(-kRayEpsilon, -kRayEpsilon, kRayEpsilon, kRayEpsilon)
                        .contains(exclusiveBounds[index]))
                    continue;
                bool contained = true;
                for (const auto &polygon : exclusive[index]) {
                    const int stride = std::max(1, int(polygon.size()) / kReplacementProbes);
                    for (int vertex = 0; vertex < polygon.size(); vertex += stride)
                        if (!primitive->shape.silhouette.contains(inverse.map(polygon[vertex]))) {
                            contained = false;
                            break;
                        }
                    if (!contained)
                        break;
                }
                if (contained)
                    members.push_back(index);
            }
            if (members.size() < 2)
                continue;
            ++trials;
            catalog::Polygons original;
            catalog::Polygons others;
            auto neighborhood = candidate.bounds;
            for (int member : members) {
                original += pieces[member];
                neighborhood = neighborhood.united(bounds[member]);
            }
            for (int index = 0; index < pieces.size(); ++index)
                if (retained[index] && !members.contains(index) && neighborhood.intersects(bounds[index]))
                    others += pieces[index];
            for (const auto &placement : additions) {
                const auto addition = coverageOf({placement}, primitives);
                if (neighborhood.intersects(catalog::painterPath(addition).boundingRect()))
                    others += addition;
            }
            others = catalog::unite(others);
            const auto required = catalog::intersect(catalog::subtract(original, others), region.visible);
            const auto proposed = catalog::mapped(primitive->shape,
                catalog::emittedTransform(candidate.placement.transform));
            if (!catalog::subtract(required, proposed).isEmpty()
                || !catalog::subtract(proposed, region.permitted).isEmpty())
                continue;
            const auto before = topologyOf(catalog::unite(original + others));
            const auto after = topologyOf(catalog::unite(proposed + others));
            if (after.components > before.components || after.holes != before.holes)
                continue;
            additions.push_back(candidate.placement);
            for (int member : members)
                retained[member] = false;
            removed += members.size() - 1;
        }
        if (additions.isEmpty())
            break;
        QVector<PenPlacement> result;
        for (int index = 0; index < placements->size(); ++index)
            if (retained[index])
                result.push_back((*placements)[index]);
        *placements = result + additions;
    }

    return removed;
}

} // namespace

QVector<catalog::Primitive> buildCatalog(const ShapeGeometryStore &geometry, QString *error) {
    const auto deployed = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/lining_shapes.json"));
    const auto source = QDir::current().filePath(QStringLiteral("assets/lining_shapes.json"));

    return catalog::buildCatalog(geometry, QFileInfo::exists(deployed) ? deployed : source, error);
}

static catalog::FillResult fillTarget(const PenFillRequest &request, const catalog::Polygons &polygons,
                                     const QVector<catalog::Primitive> &primitives,
                                     const FillOptions &options, const std::function<bool()> &cancelled,
                                     const QVector<PenPoint> &spine = {}, double liningWidth = 0.0) {
    catalog::FillResult result;
    QVector<compact::ReusableCandidate> candidates;
    QElapsedTimer timer;
    QElapsedTimer refinementTimer;
    QJsonObject timings;
    QJsonObject refinements;
    const auto recordRefinement = [&](const QString &stage) {
        refinements.insert(stage, refinementTimer.nsecsElapsed() / 1e6);
        refinementTimer.restart();
    };
    timer.start();
    result.fill.shapeLimit = options.shapeBudget;

    try {
        if (options.shapeBudget < 1 || options.profileTrialBudget < 1
            || !std::isfinite(options.boundaryAllowance) || options.boundaryAllowance <= 0.0
            || !std::isfinite(options.minimumCoverage) || options.minimumCoverage <= 0.0 || options.minimumCoverage > 1.0
            || !std::isfinite(options.maximumThicknessRatio) || options.maximumThicknessRatio <= 1.0
            || options.maximumThicknessRatio > kMaximumThicknessRatio
            || !std::isfinite(options.preferredThicknessRatio) || options.preferredThicknessRatio < 1.0
            || options.preferredThicknessRatio > kMaximumThicknessRatio)
            throw std::runtime_error("Lining requires a positive margin, work budgets and a valid coverage target");
        if (stopped(cancelled)) {
            result.fill.cancelled = true;
            return result;
        }
        auto authored = polygons.isEmpty() ? catalog::buildRegion(request, cancelled) : polygonRegion(polygons);
        if (authored.required.isEmpty() || authored.area <= 0.0)
            throw std::runtime_error("Lining requires a nonempty fillable region");
        authored.flexibleBoundary = true;
        const double width = meanWidth(authored.visible);
        const double scale = std::clamp(width * kScaleWidthFraction, kMinimumScale, kMaximumScale);
        const double allowance = options.boundaryAllowance;
        auto region = catalog::leewayAdjustedRegion(authored, options.leeway, allowance);
        const auto envelope = thicknessEnvelope(authored.visible, allowance, options.maximumThicknessRatio, liningWidth, cancelled);
        const double preferredThicknessRatio = std::min(options.preferredThicknessRatio, options.maximumThicknessRatio);
        const auto preferredCoverage = catalog::subtract(thicknessEnvelope(authored.visible, allowance,
            preferredThicknessRatio, liningWidth, cancelled), region.leeway);
        region.permitted = catalog::unite(envelope + region.leeway);
        region.permittedPath = catalog::painterPath(region.permitted);
        const double maximumMissing = region.area * (1.0 - options.minimumCoverage);
        result.diagnostics.insert(QStringLiteral("meanWidth"), width);
        result.diagnostics.insert(QStringLiteral("boundaryAllowance"), allowance);
        result.diagnostics.insert(QStringLiteral("minimumCoverage"), options.minimumCoverage);
        result.diagnostics.insert(QStringLiteral("maximumThicknessRatio"), options.maximumThicknessRatio);
        result.diagnostics.insert(QStringLiteral("preferredThicknessRatio"), preferredThicknessRatio);
        result.diagnostics.insert(QStringLiteral("permittedAreaRatio"), catalog::area(envelope) / authored.area);
        timings.insert(QStringLiteral("setup"), timer.nsecsElapsed() / 1e6);
        timer.restart();
        const auto square = std::find_if(primitives.cbegin(), primitives.cend(), [](const auto &primitive) {
            return primitive.shape.shapeId == 101 && catalog::usesTask(primitive, catalog::ShapeTask::StraightEdges);
        });
        const catalog::PointContainment envelopeContainment(region.permitted);
        const auto rectangle = square == primitives.cend() ? std::optional<PenPlacement>{}
            : enclosingRectangle(region.required, square->shape, region, envelopeContainment);
        auto wholeSpan = rectangle;
        if (!wholeSpan)
            wholeSpan = fitWholeRegion(region, primitives, maximumMissing, cancelled);
        if (!wholeSpan && !spine.isEmpty())
            wholeSpan = fitWholeSpan(spine, liningWidth, primitives, region, maximumMissing, allowance, cancelled, options.useGpu);
        const auto separate = wholeSpan ? QVector<PenPlacement>{}
            : fitSeparateRegions(region, primitives, maximumMissing, cancelled);
        if (!separate.isEmpty() && separate.size() <= options.shapeBudget) {
            result.fill.placements = separate;
            result.diagnostics.insert(QStringLiteral("wholeComponents"), separate.size());
        } else if (wholeSpan) {
            result.fill.placements = {*wholeSpan};
            result.diagnostics.insert(QStringLiteral("wholeSpan"), true);
        } else {
            compact::FillOptions seedOptions;
            seedOptions.shapeBudget = options.shapeBudget;
            seedOptions.profileTrialBudget = options.profileTrialBudget;
            seedOptions.thinRegion = true;
            seedOptions.seedOnly = true;
            seedOptions.useGpu = options.useGpu;
            seedOptions.boundaryAllowance = std::min(allowance, width * (options.maximumThicknessRatio - 1.0) * 0.5);
            seedOptions.inwardAllowance = std::max(kMinimumAllowance, width * kToleranceWidthFraction);
            seedOptions.observationScale = scale;
            seedOptions.leeway = options.leeway;
            auto seed = profile::seedRegion(region, primitives, seedOptions, cancelled, &candidates);
            result.diagnostics.insert(QStringLiteral("profileSeed"), seed.diagnostics);
            if (!seed.fill.error.isEmpty())
                throw std::runtime_error(seed.fill.error.toStdString());
            result.fill.placements = std::move(seed.fill.placements);
        }
        timings.insert(QStringLiteral("fitting"), timer.nsecsElapsed() / 1e6);
        timer.restart();
        if (options.workProgress)
            options.workProgress(result.fill.placements.size(), 0, 0);
        refinementTimer.start();
        expandPlacements(&result.fill.placements, region, primitives, preferredCoverage, cancelled);
        recordRefinement(QStringLiteral("growth"));
        repairCoverage(&result.fill.placements, region, primitives, options.shapeBudget, maximumMissing, cancelled);
        recordRefinement(QStringLiteral("initialGapRepair"));
        int removed = reducePlacements(&result.fill.placements, region, primitives, maximumMissing, cancelled);
        recordRefinement(QStringLiteral("initialReduction"));
        const int nativeReduction = replaceNativeGroups(&result.fill.placements, region, primitives, candidates, cancelled);
        result.diagnostics.insert(QStringLiteral("nativeGroupShapeReduction"), nativeReduction);
        recordRefinement(QStringLiteral("nativeReplacement"));
        mergePlacements(&result.fill.placements, region, primitives, cancelled);
        recordRefinement(QStringLiteral("merging"));
        repairCoverage(&result.fill.placements, region, primitives, options.shapeBudget, maximumMissing, cancelled);
        recordRefinement(QStringLiteral("finalGapRepair"));
        removed += reducePlacements(&result.fill.placements, region, primitives, maximumMissing, cancelled);
        recordRefinement(QStringLiteral("finalReduction"));
        result.diagnostics.insert(QStringLiteral("removedLowContributionPlacements"), removed);
        const auto coverage = coverageOf(result.fill.placements, primitives);
        const auto visibleCoverage = region.leeway.isEmpty() ? coverage : catalog::subtract(coverage, region.leeway);
        const auto missing = catalog::subtract(region.visible, visibleCoverage);
        const auto outside = catalog::subtract(coverage, region.permitted);
        const compact::BoundaryModel boundary(region.visible, scale);
        const auto boundaryQuality = boundary.measure(visibleCoverage);
        const auto targetQuality = boundary.measure(region.visible);
        catalog::Polygons authoredHoles;
        for (auto polygon : region.visible)
            if (catalog::signedArea(polygon) < 0.0) {
                std::reverse(polygon.begin(), polygon.end());
                authoredHoles.push_back(polygon);
            }
        double newInteriorHoleArea = 0.0;
        for (auto polygon : visibleCoverage)
            if (catalog::signedArea(polygon) < 0.0) {
                std::reverse(polygon.begin(), polygon.end());
                if (catalog::intersect({polygon}, authoredHoles).isEmpty())
                    newInteriorHoleArea += catalog::area(catalog::intersect({polygon}, region.visible));
            }
        result.fill.targetArea = catalog::area(region.visible);
        result.fill.coveredArea = result.fill.targetArea - catalog::area(missing);
        result.fill.outsideArea = catalog::area(catalog::subtract(visibleCoverage, region.visible));
        result.fill.unfilled = catalog::painterPath(missing);
        const double coverageRatio = result.fill.targetArea > 0.0 ? result.fill.coveredArea / result.fill.targetArea : 0.0;
        if (coverageRatio < options.minimumCoverage || !outside.isEmpty() || result.fill.placements.isEmpty()
            || result.fill.placements.size() > options.shapeBudget
            || newInteriorHoleArea > catalog::kVerificationClearance * catalog::kVerificationClearance
            || boundaryQuality.components > targetQuality.components || boundaryQuality.holes < targetQuality.holes)
            result.fill.error = QStringLiteral("Lining could not preserve coverage and continuity within the local thickness limit");
        result.diagnostics.insert(QStringLiteral("coverageRatio"), coverageRatio);
        result.diagnostics.insert(QStringLiteral("generatedAreaRatio"), catalog::area(visibleCoverage) / result.fill.targetArea);
        result.diagnostics.insert(QStringLiteral("preferredCoverageRatio"), 1.0
            - catalog::area(catalog::subtract(preferredCoverage, visibleCoverage)) / catalog::area(preferredCoverage));
        result.diagnostics.insert(QStringLiteral("missingInteriorArea"), catalog::area(missing));
        result.diagnostics.insert(QStringLiteral("newInteriorHoleArea"), newInteriorHoleArea);
        result.diagnostics.insert(QStringLiteral("outsideEnvelope"), catalog::area(outside));
        result.diagnostics.insert(QStringLiteral("boundaryQuality"), boundary.diagnostics(boundaryQuality));
        result.diagnostics.insert(QStringLiteral("targetBoundary"), boundary.diagnostics(targetQuality));
        recordRefinement(QStringLiteral("verification"));
        result.diagnostics.insert(QStringLiteral("refinementMilliseconds"), refinements);
        timings.insert(QStringLiteral("refinementAndVerification"), timer.nsecsElapsed() / 1e6);
    } catch (const std::exception &failure) {
        result.fill.error = QString::fromUtf8(failure.what());
    }
    if (stopped(cancelled)) {
        result.fill = {};
        result.fill.cancelled = true;
    }
    result.diagnostics.insert(QStringLiteral("strategy"), QStringLiteral("long span lining with bounded spill"));
    result.diagnostics.insert(QStringLiteral("stageMilliseconds"), timings);
    if (!primitives.isEmpty() && primitives.front().configuration)
        result.diagnostics.insert(QStringLiteral("shapeConfiguration"), *primitives.front().configuration);

    return result;
}

catalog::FillResult fillRegion(const PenFillRequest &request, const QVector<catalog::Primitive> &primitives,
                               const FillOptions &options, const std::function<bool()> &cancelled) {

    return fillTarget(request, {}, primitives, options, cancelled);
}

catalog::FillResult fillPolygons(const QVector<QPolygonF> &polygons, const QVector<catalog::Primitive> &primitives,
                                const FillOptions &options, const std::function<bool()> &cancelled) {
    if (polygons.isEmpty()) {
        catalog::FillResult result;
        result.fill.error = QStringLiteral("Thin fit requires a nonempty region");

        return result;
    }

    return fillTarget({}, polygons, primitives, options, cancelled);
}

catalog::FillResult fillLiningPath(const QVector<PenPoint> &points, double width,
                                   const QVector<catalog::Primitive> &primitives,
                                   const FillOptions &options, const std::function<bool()> &cancelled) {
    catalog::FillResult result;
    const auto path = buildLiningPath(points);
    QPainterPathStroker stroker;

    if (!path.valid() || !std::isfinite(width) || width <= 0.0) {
        result.fill.error = path.valid() ? QStringLiteral("Lining width must be positive") : path.error;
        return result;
    }
    stroker.setWidth(width);
    stroker.setCurveThreshold(kStrokeCurveThreshold);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    const auto stroke = stroker.createStroke(path.centerline);
    result = fillTarget({}, normalizedStroke(stroke, width), primitives, options, cancelled, points, width);
    result.diagnostics.insert(QStringLiteral("liningWidth"), width);

    return result;
}

} // namespace gui::thin
