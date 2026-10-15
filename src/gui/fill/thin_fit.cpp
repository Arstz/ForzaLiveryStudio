#include "thin_fit.h"
#include "compact_fit_catalog_internal.h"
#include "compact_fit_quality.h"
#include "compact_fit_reduction.h"
#include "lining_fill.h"
#include "profile_fit.h"

#include <QDataStream>

#include <algorithm>
#include <array>
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
constexpr int kExpansionRounds = 3;
constexpr double kGrowthWindowWidthPadding = 3.0;
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
constexpr double kProtectedCoreWidthFraction = 0.06;
constexpr int kPlacementEnvelopeProbes = 32;
constexpr double kPoseMissingPenalty = 24.0;
constexpr double kPoseOutsidePenalty = 96.0;
constexpr double kPoseExtraInkPenalty = 0.1;
constexpr double kPoseAlongTranslationWidthFraction = 0.5;
constexpr double kPoseAcrossTranslationWidthFraction = 0.25;
constexpr double kPoseAcrossScaleStep = 0.16;
constexpr double kPoseShearWidthFraction = 0.5;
constexpr double kPoseTurnWidthFraction = 0.5;
constexpr double kNeighborhoodWidthPadding = 2.0;
constexpr double kNeighborhoodContactWidthFraction = 0.25;
constexpr double kNeighborhoodAreaPenalty = 0.1;
constexpr double kNeighborhoodInitialMissingRatio = 0.35;
constexpr int kNeighborhoodStarts = 3;
constexpr int kPoseParameters = 6;
constexpr std::array<double, 5> kPoseSteps{1.0, 0.5, 0.25, 0.125, 0.0625};
constexpr std::array<int, 4> kNeighborhoodSizes{8, 5, 3, 2};
constexpr int kGapNeighborLimit = 8;
constexpr std::array<double, 5> kGapGrowthSteps{0.015, 0.03, 0.06, 0.12, 0.24};
constexpr std::array<double, 3> kMicroGapGrowthSteps{0.00001, 0.0001, 0.001};
constexpr double kMicroGapAreaFraction = 0.000001;
constexpr double kPlacementClearanceFactor = 2.0;
constexpr double kWholeRegionMinimumBoundingCoverage = 0.5;
constexpr std::array<double, 3> kWholeRegionOutwardScales{1.05, 1.1, 1.2};
constexpr double kTraceSupportWidthFraction = 0.02;
constexpr double kTraceCapExtensionWidthFraction = 0.85;
constexpr double kTraceMinimumBodyWidthRatio = 0.4;
constexpr double kTraceWidthErrorWeight = 0.5;
constexpr double kTraceVariableWidthMinimumRatio = 0.65;
constexpr double kTraceWidthVariationTolerance = 0.05;
constexpr int kTraceTaperProbes = 3;
constexpr double kTracePreferredWidthRatio = 0.9;
constexpr double kTraceCurveErrorWidthFraction = 0.08;
constexpr double kTraceMarginSpineFraction = 0.5;
constexpr double kTraceMaximumSpineOffsetWidthFraction = 0.25;
constexpr double kTraceTangentLimit = 0.08715574274765817;
constexpr double kTraceHardPointTangentLimit = 0.3420201433256687;
constexpr double kTraceJoinExtensionWidthFraction = 0.12;
constexpr double kTraceProfileOffsetWidthFraction = 0.05;
constexpr int kTraceSamples = 65;
constexpr int kTraceWidthSamples = 17;
constexpr int kTraceMaximumDepth = 16;
constexpr int kTraceMergeRounds = 2;
constexpr std::array<int, 4> kTraceMergeSizes{8, 5, 3, 2};
constexpr std::array<double, 3> kTraceThicknessScales{0.9, 1.0, 1.1};
constexpr int kTraceCandidatePosesPerShape = 1;
constexpr int kTraceAlternatives = 48;
constexpr int kTraceContourTrials = 8192;
constexpr int kTraceContourPasses = 2;
constexpr int kTraceContourSplitTrials = 8;
constexpr int kTraceRepairPolishTrials = 512;
constexpr int kTraceRepairPolishMilliseconds = 750;
constexpr double kTraceSmoothTurnLimit = 0.7853981633974483;
constexpr double kTraceContourStep = 0.04;
constexpr std::array<double, 3> kTraceJoinOverlapSteps{0.15, 0.3, 0.6};
constexpr std::array<double, 3> kTraceJoinWidthTransfers{-0.1, 0.0, 0.1};
constexpr double kTraceBoundaryWeight = 8.0;
constexpr double kTracePeakTurnWeight = 2.0;
constexpr double kTraceContourPeakWeight = 16.0;
constexpr double kTraceEndpointRadiusWidthFraction = 0.75;
constexpr double kTraceStraightErrorWidthFraction = 0.04;
constexpr double kTraceMinimumJoinWidthRatio = 0.4;
constexpr int kTraceSquareShapeId = 101;
constexpr int kTraceRectangleShapeId = 2205;
constexpr double kTraceMergeEnergySlack = 0.01;
constexpr double kTraceMergeTurnSlack = 0.035;
constexpr std::array<double, 4> kTraceContourSteps{2.0, 1.0, 0.5, 0.25};

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

catalog::Polygons enclosedSpaces(const catalog::Polygons &polygons) {
    catalog::Polygons result;

    for (auto polygon : polygons)
        if (catalog::signedArea(polygon) < 0.0) {
            std::reverse(polygon.begin(), polygon.end());
            result.push_back(std::move(polygon));
        }

    return catalog::unite(result);
}

catalog::Polygons interiorHoles(const catalog::Polygons &coverage, const catalog::Region &region) {
    catalog::Polygons authoredHoles;
    catalog::Polygons result;
    const auto visible = region.leeway.isEmpty() ? coverage : catalog::subtract(coverage, region.leeway);

    for (auto polygon : region.visible)
        if (catalog::signedArea(polygon) < 0.0) {
            std::reverse(polygon.begin(), polygon.end());
            authoredHoles.push_back(polygon);
        }
    for (auto polygon : visible)
        if (catalog::signedArea(polygon) < 0.0) {
            std::reverse(polygon.begin(), polygon.end());
            if (catalog::intersect({polygon}, authoredHoles).isEmpty())
                result += catalog::intersect({polygon}, region.visible);
        }

    return catalog::unite(result);
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
    result.permitted = result.required;
    result.permittedPath = result.requiredPath;
    result.spillFreeContainment = std::make_shared<catalog::PointContainment>(result.spillFree);

    return result;
}

catalog::Region visibleRegion(const catalog::Region &authored, const catalog::Polygons &leeway) {
    auto result = authored;
    result.leeway = catalog::unite(leeway);
    if (result.leeway.isEmpty())
        return result;
    result.visible = catalog::subtract(authored.required, result.leeway);
    result.required = result.visible;
    result.requiredPath = catalog::painterPath(result.required);
    result.bounds = result.requiredPath.boundingRect();
    result.area = catalog::area(result.required);
    result.spillFree = catalog::unite(authored.required + result.leeway);
    result.spillFreePath = catalog::painterPath(result.spillFree);
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
        const double coordinateExtent = std::max({std::abs(left), std::abs(right), std::abs(top), std::abs(bottom)});
        const double clearance = std::max(catalog::kVerificationClearance,
            coordinateExtent * std::numeric_limits<float>::epsilon()) * kPlacementClearanceFactor;
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
    auto totalCoverage = coverageOf(*placements, primitives);
    auto allowedHoles = interiorHoles(totalCoverage, region);
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
        if (catalog::area(catalog::intersect(privateCoverage, core)) > kRayEpsilon * kRayEpsilon)
            continue;
        const auto before = topologyOf(catalog::unite(others + polygons[index]));
        const auto after = topologyOf(others);
        if (after.components > before.components || after.holes != before.holes)
            continue;
        const auto nextCoverage = catalog::subtract(totalCoverage, catalog::subtract(polygons[index], others));
        const auto nextHoles = interiorHoles(nextCoverage, region);
        if (catalog::area(catalog::subtract(nextHoles, allowedHoles)) > kRayEpsilon * kRayEpsilon)
            continue;
        retained[index] = false;
        totalCoverage = nextCoverage;
        allowedHoles = nextHoles;
        missing += privateArea;
    }
    QVector<PenPlacement> result;
    for (int index = 0; index < placements->size(); ++index)
        if (retained[index])
            result.push_back((*placements)[index]);
    *placements = std::move(result);

    return originalCount - placements->size();
}

struct GrowthWindow {
    QRectF bounds;
    catalog::Polygons preferred;
    catalog::Polygons permitted;
    catalog::Polygons protectedCoverage;
};

GrowthWindow growthWindow(const QRectF &bounds, double width, const catalog::Region &region,
                          const catalog::Polygons &preferred, const catalog::Polygons &protectedCoverage) {
    GrowthWindow result;
    const double padding = width * kGrowthWindowWidthPadding;

    result.bounds = bounds.adjusted(-padding, -padding, padding, padding);
    const catalog::Polygons frame{QPolygonF(result.bounds)};
    result.preferred = catalog::intersect(preferred, frame);
    result.permitted = catalog::intersect(region.permitted, frame);
    result.protectedCoverage = catalog::intersect(protectedCoverage, frame);

    return result;
}

void expandPlacements(QVector<PenPlacement> *placements, const catalog::Region &region,
                      const QVector<catalog::Primitive> &primitives,
                      const catalog::Polygons &preferredCoverage, double preferredThicknessRatio,
                      const std::function<bool()> &cancelled) {
    QVector<catalog::Polygons> polygons;
    QVector<QRectF> bounds;
    QVector<GrowthWindow> windows(placements->size());
    const catalog::PointContainment envelope(region.permitted);
    const double width = meanWidth(region.visible);
    const double growthStep = std::max(kGrowthStep, (preferredThicknessRatio - 1.0) * 0.5);

    for (const auto &placement : *placements) {
        polygons.push_back(coverageOf({placement}, primitives));
        bounds.push_back(catalog::painterPath(polygons.back()).boundingRect());
    }
    for (int round = 0; round < kExpansionRounds && !stopped(cancelled); ++round) {
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
            catalog::Polygons localCoverage;
            const auto neighborhood = bounds[index].adjusted(-width, -width, width, width);
            auto &window = windows[index];
            double bestGain = 0.0;

            if (!window.bounds.contains(neighborhood))
                window = growthWindow(bounds[index], width, region, preferredCoverage, region.visible);
            for (int other = 0; other < polygons.size(); ++other)
                if (window.bounds.intersects(bounds[other]))
                    localCoverage += polygons[other];
            const auto localMissing = catalog::intersect(
                catalog::subtract(window.preferred, catalog::unite(localCoverage)), {QPolygonF(neighborhood)});

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
                    / std::max(kRayEpsilon, QLineF({}, axis).length()), extendJoins ? 1.0 : 1.0 + growthStep);
                growth.rotate(-angle);
                growth.translate(-center.x(), -center.y());
                auto proposed = placement;
                proposed.transform = catalog::emittedTransform(placement.transform * growth);
                if (!probesInside(shape->shape, proposed.transform, envelope))
                    continue;
                const auto addition = catalog::mapped(shape->shape, proposed.transform);
                const auto gain = catalog::intersect(addition, localMissing);
                const auto additionBounds = catalog::painterPath(addition).boundingRect();
                const auto &permitted = window.bounds.contains(additionBounds) ? window.permitted : region.permitted;
                if (gain.isEmpty() || !catalog::subtract(addition, permitted).isEmpty())
                    continue;
                const auto lost = catalog::intersect(catalog::subtract(polygons[index], addition), window.preferred);
                catalog::Polygons exposed;
                if (!lost.isEmpty()) {
                    catalog::Polygons others;
                    const auto lostBounds = catalog::painterPath(lost).boundingRect();
                    for (int other = 0; other < polygons.size(); ++other)
                        if (other != index && lostBounds.intersects(bounds[other]))
                            others += polygons[other];
                    exposed = catalog::subtract(lost, catalog::unite(others));
                }
                if (!catalog::intersect(exposed, window.protectedCoverage).isEmpty())
                    continue;
                const double netGain = catalog::area(gain) - catalog::area(exposed);
                if (netGain <= bestGain)
                    continue;
                bestPlacement = proposed;
                bestPolygons = addition;
                bestGain = netGain;
                break;
            }
            if (bestGain > 0.0) {
                (*placements)[index] = bestPlacement;
                polygons[index] = bestPolygons;
                bounds[index] = catalog::painterPath(bestPolygons).boundingRect();
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
                    const auto transform = catalog::affineFromAnchors(source, target);
                    proposals.push_back(transform);
                    if (region.area >= bounds.width() * bounds.height() * kWholeRegionMinimumBoundingCoverage)
                        for (int dimension = 0; dimension < 2; ++dimension)
                            for (double scale : kWholeRegionOutwardScales) {
                                QTransform growth;
                                growth.translate(origin.x(), origin.y());
                                growth.scale(dimension == 0 ? scale : 1.0, dimension == 1 ? scale : 1.0);
                                growth.translate(-origin.x(), -origin.y());
                                proposals.push_back(transform * growth);
                            }
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

bool repairWithNeighbor(QVector<PenPlacement> *placements, const catalog::Polygons &gap,
                        const catalog::Region &region, const QVector<catalog::Primitive> &primitives,
                        const std::function<bool()> &cancelled) {
    QVector<catalog::Polygons> pieces;
    QVector<QRectF> bounds;
    QVector<std::pair<double, int>> order;
    PenPlacement best;
    const auto target = catalog::intersect(catalog::expanded(gap, catalog::kVerificationClearance), region.permitted);
    const auto targetBounds = catalog::painterPath(target).boundingRect();
    const catalog::PointContainment envelope(region.permitted);
    auto support = region.visible;
    const bool microGap = catalog::area(gap) <= region.area * kMicroGapAreaFraction;
    int bestIndex = -1;
    double bestGain = 0.0;
    double bestExtraArea = std::numeric_limits<double>::infinity();

    if (microGap)
        support = catalog::interiorSupport(region.visible, meanWidth(region.visible) * kProtectedCoreWidthFraction);
    for (int index = 0; index < placements->size(); ++index) {
        pieces.push_back(coverageOf({(*placements)[index]}, primitives));
        bounds.push_back(catalog::painterPath(pieces.back()).boundingRect());
        const auto closest = QPointF(std::clamp(targetBounds.center().x(), bounds.back().left(), bounds.back().right()),
            std::clamp(targetBounds.center().y(), bounds.back().top(), bounds.back().bottom()));
        order.push_back({QLineF(closest, targetBounds.center()).length(), index});
    }
    std::stable_sort(order.begin(), order.end());
    for (int neighbor = 0; neighbor < std::min(kGapNeighborLimit, int(order.size())) && !stopped(cancelled); ++neighbor) {
        const int index = order[neighbor].second;
        const auto &placement = (*placements)[index];
        const auto shape = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &primitive) {
            return primitive.shape.shapeId == placement.shapeId;
        });
        const auto center = placement.transform.map(shape->shape.bounds.center());
        const auto horizontal = placement.transform.map(shape->shape.bounds.topRight())
            - placement.transform.map(shape->shape.bounds.topLeft());
        const auto vertical = placement.transform.map(shape->shape.bounds.bottomLeft())
            - placement.transform.map(shape->shape.bounds.topLeft());
        const auto axis = QLineF({}, horizontal).length() >= QLineF({}, vertical).length() ? horizontal : vertical;
        const double angle = std::atan2(axis.y(), axis.x()) * 180.0 / std::acos(-1.0);
        const double padding = std::hypot(bounds[index].width(), bounds[index].height())
            * kGapGrowthSteps.back() + kRayEpsilon;
        const auto frame = bounds[index].adjusted(-padding, -padding, padding, padding);
        const auto permitted = catalog::intersect(region.permitted, {QPolygonF(frame)});
        catalog::Polygons others;
        for (int other = 0; other < pieces.size(); ++other)
            if (other != index && bounds[index].intersects(bounds[other]))
                others += pieces[other];
        others = catalog::unite(others);
        const auto protectedCoverage = catalog::subtract(catalog::intersect(pieces[index], support), others);
        const auto allowedSpaces = microGap ? enclosedSpaces(catalog::unite(others + pieces[index]))
            : catalog::Polygons{};
        QVector<double> growthSteps;
        if (catalog::area(gap) <= shape->shape.area * std::abs(placement.transform.determinant()) * kMicroGapAreaFraction)
            for (double step : kMicroGapGrowthSteps)
                growthSteps.push_back(step);
        for (double step : kGapGrowthSteps)
            growthSteps.push_back(step);
        for (double step : growthSteps)
            for (int dimension = 0; dimension < 3 && !stopped(cancelled); ++dimension) {
                QTransform growth;
                growth.translate(center.x(), center.y());
                growth.rotate(angle);
                growth.scale(dimension == 1 ? 1.0 : 1.0 + step, dimension == 0 ? 1.0 : 1.0 + step);
                growth.rotate(-angle);
                growth.translate(-center.x(), -center.y());
                auto proposed = placement;
                proposed.transform = catalog::emittedTransform(placement.transform * growth);
                if (!probesInside(shape->shape, proposed.transform, envelope))
                    continue;
                const auto addition = catalog::mapped(shape->shape, proposed.transform);
                const auto &localPermitted = frame.contains(catalog::painterPath(addition).boundingRect())
                    ? permitted : region.permitted;
                if (!catalog::subtract(addition, localPermitted).isEmpty()
                    || !catalog::subtract(protectedCoverage, addition).isEmpty())
                    continue;
                if (microGap && catalog::area(catalog::subtract(
                        enclosedSpaces(catalog::unite(others + addition)), allowedSpaces)) > kRayEpsilon * kRayEpsilon)
                    continue;
                const double gain = catalog::area(catalog::intersect(addition, target));
                const double extraArea = catalog::area(addition) - catalog::area(pieces[index]);
                if (gain < bestGain || (gain == bestGain && extraArea >= bestExtraArea))
                    continue;
                best = proposed;
                bestIndex = index;
                bestGain = gain;
                bestExtraArea = extraArea;
            }
    }
    if (bestIndex < 0 || bestGain <= 0.0)
        return false;
    (*placements)[bestIndex] = best;

    return true;
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
    for (const auto &polygon : interiorHoles(coverage, region)) {
        if (stopped(cancelled) || placements->size() >= shapeBudget)
            return;
        if (catalog::signedArea(polygon) <= 0.0)
            continue;
        const catalog::Polygons hole{polygon};
        if (repairWithNeighbor(placements, hole, region, primitives, cancelled)) {
            coverage = coverageOf(*placements, primitives);
            missing = catalog::subtract(region.visible, coverage);
        }
        const auto residual = catalog::subtract(hole, coverage);
        auto repairTarget = catalog::unite(missing + residual);
        repairContinuity(residual, square->shape, region, envelope, placements, &repairTarget,
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

struct PoseEvaluation {
    catalog::Polygons coverage;
    double cost = std::numeric_limits<double>::infinity();
};

PoseEvaluation evaluatePose(const PenPrimitive &shape, const QTransform &transform,
                            const catalog::Region &target, const catalog::Polygons &preferred) {
    PoseEvaluation result;
    const auto emitted = catalog::emittedTransform(transform);

    if (!std::isfinite(emitted.determinant()) || std::abs(emitted.determinant()) < catalog::kMinimumDeterminant)
        return result;
    result.coverage = catalog::mapped(shape, emitted);
    result.cost = catalog::area(catalog::subtract(target.required, result.coverage)) * kPoseMissingPenalty
        + catalog::area(catalog::subtract(result.coverage, target.permitted)) * kPoseOutsidePenalty
        + catalog::area(catalog::subtract(result.coverage, preferred)) * kPoseExtraInkPenalty;

    return result;
}

PenPlacement polishNativePose(PenPlacement initial, const PenPrimitive &shape,
                              const catalog::Region &target, const catalog::Polygons &preferred,
                              double width, int budget, int *evaluations,
                              const std::function<bool()> &stop) {
    auto best = initial;
    auto evaluation = evaluatePose(shape, best.transform, target, preferred);
    ++*evaluations;

    for (double step : kPoseSteps) {
        for (int parameter = 0; parameter < kPoseParameters && *evaluations < budget && !stop(); ++parameter) {
            for (double direction : {-1.0, 1.0}) {
                if (*evaluations >= budget || stop())
                    break;
                const auto center = best.transform.map(shape.bounds.center());
                const auto horizontal = best.transform.map(shape.bounds.topRight())
                    - best.transform.map(shape.bounds.topLeft());
                const auto vertical = best.transform.map(shape.bounds.bottomLeft())
                    - best.transform.map(shape.bounds.topLeft());
                const auto axis = QLineF({}, horizontal).length() >= QLineF({}, vertical).length() ? horizontal : vertical;
                QTransform adjustment;
                const double length = std::max(width, QLineF({}, axis).length());
                const double angle = std::atan2(axis.y(), axis.x()) * 180.0 / std::acos(-1.0);
                const double delta = direction * step;
                adjustment.translate(center.x(), center.y());
                adjustment.rotate(angle);
                switch (parameter) {
                case 0: adjustment.translate(width * delta * kPoseAlongTranslationWidthFraction, 0.0); break;
                case 1: adjustment.translate(0.0, width * delta * kPoseAcrossTranslationWidthFraction); break;
                case 2: adjustment.scale(1.0 + width / length * delta, 1.0); break;
                case 3: adjustment.scale(1.0, 1.0 + delta * kPoseAcrossScaleStep); break;
                case 4: adjustment.shear(0.0, width / length * delta * kPoseShearWidthFraction); break;
                case 5: adjustment.rotate(std::atan2(width * delta * kPoseTurnWidthFraction, length) * 180.0 / std::acos(-1.0)); break;
                }
                adjustment.rotate(-angle);
                adjustment.translate(-center.x(), -center.y());
                auto proposal = best;
                proposal.transform = catalog::emittedTransform(best.transform * adjustment);
                const auto next = evaluatePose(shape, proposal.transform, target, preferred);
                ++*evaluations;
                if (next.cost < evaluation.cost) {
                    best = proposal;
                    evaluation = next;
                }
            }
        }
    }

    return best;
}

int refitNativeNeighborhoods(QVector<PenPlacement> *placements, const catalog::Region &region,
                             const QVector<catalog::Primitive> &primitives,
                             const catalog::Polygons &preferred, const FillOptions &options,
                             const std::function<bool()> &cancelled, QJsonObject *diagnostics) {
    QVector<catalog::Polygons> pieces;
    QVector<QRectF> bounds;
    QVector<double> areas;
    QVector<int> order(placements->size());
    QVector<bool> retained(placements->size(), true);
    QElapsedTimer timer;
    const auto core = catalog::interiorSupport(region.visible, meanWidth(region.visible) * kProtectedCoreWidthFraction);
    int evaluations = 0;
    int groups = 0;
    int reduction = 0;
    const double width = meanWidth(region.visible);
    const auto stop = [&] {
        return stopped(cancelled) || timer.elapsed() >= options.qualityTimeBudgetMilliseconds
            || evaluations >= options.qualityEvaluationBudget;
    };

    timer.start();
    for (const auto &placement : *placements) {
        pieces.push_back(coverageOf({placement}, primitives));
        bounds.push_back(catalog::painterPath(pieces.back()).boundingRect());
        areas.push_back(catalog::area(pieces.back()));
    }
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int first, int second) {
        return areas[first] > areas[second];
    });
    for (int pivot : order) {
        if (stop())
            break;
        if (!retained[pivot])
            continue;
        QVector<std::pair<double, int>> neighbors;
        const auto contact = catalog::expanded(pieces[pivot], width * kNeighborhoodContactWidthFraction);
        for (int index = 0; index < pieces.size(); ++index)
            if (index != pivot && retained[index]
                && bounds[pivot].adjusted(-width, -width, width, width).intersects(bounds[index])) {
                const double overlap = catalog::area(catalog::intersect(contact, pieces[index]));
                if (overlap > 0.0)
                    neighbors.push_back({1.0 - overlap / std::max(areas[index], kRayEpsilon)
                        + areas[index] / std::max(areas[pivot], kRayEpsilon) * kNeighborhoodAreaPenalty, index});
            }
        std::sort(neighbors.begin(), neighbors.end());
        for (int count : kNeighborhoodSizes) {
            if (stop() || neighbors.size() < count - 1)
                continue;
            QVector<int> members{pivot};
            catalog::Polygons original;
            catalog::Polygons others;
            QPolygonF points;
            auto frame = bounds[pivot];
            for (int neighbor = 0; neighbor < count - 1; ++neighbor)
                members.push_back(neighbors[neighbor].second);
            for (int member : members) {
                original += pieces[member];
                frame = frame.united(bounds[member]);
                for (const auto &polygon : pieces[member])
                    points += polygon;
            }
            frame = frame.adjusted(-width * kNeighborhoodWidthPadding, -width * kNeighborhoodWidthPadding,
                width * kNeighborhoodWidthPadding, width * kNeighborhoodWidthPadding);
            for (int index = 0; index < pieces.size(); ++index)
                if (retained[index] && !members.contains(index) && frame.intersects(bounds[index]))
                    others += pieces[index];
            others = catalog::unite(others);
            original = catalog::unite(original);
            const auto hull = catalog::convexHull(points);
            auto local = region;
            local.required = catalog::subtract(catalog::intersect(region.visible, {hull}), others);
            local.visible = local.required;
            local.area = catalog::area(local.required);
            local.bounds = catalog::painterPath(local.required).boundingRect();
            local.permitted = catalog::intersect(region.permitted, {QPolygonF(frame)});
            if (local.required.isEmpty())
                continue;
            const auto preferredLocal = catalog::unite(catalog::intersect(preferred, {QPolygonF(frame)}) + others);
            const auto protectedLocal = catalog::intersect(local.required, core);
            const auto originalMissing = catalog::subtract(local.required, original);
            const auto originalCoreMissing = catalog::subtract(protectedLocal, original);
            const auto before = topologyOf(catalog::unite(original + others));
            QVector<std::pair<double, PenPlacement>> starts;
            const auto whole = fitWholeRegion(local, primitives, local.area * kNeighborhoodInitialMissingRatio, stop,
                catalog::ShapeTask::GroupReplacements, kNativeMergeAnchors);
            if (whole)
                starts.push_back({0.0, *whole});
            for (int member : members) {
                const auto shape = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &entry) {
                    return entry.shape.shapeId == (*placements)[member].shapeId
                        && catalog::usesTask(entry, catalog::ShapeTask::GroupReplacements);
                });
                if (shape != primitives.cend())
                    starts.push_back({evaluatePose(shape->shape, (*placements)[member].transform, local, preferredLocal).cost,
                        (*placements)[member]});
            }
            std::stable_sort(starts.begin(), starts.end(), [](const auto &first, const auto &second) {
                return first.first < second.first;
            });
            ++groups;
            bool accepted = false;
            for (int start = 0; start < std::min(kNeighborhoodStarts, int(starts.size())) && !stop(); ++start) {
                const auto shape = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &entry) {
                    return entry.shape.shapeId == starts[start].second.shapeId;
                });
                const auto proposal = polishNativePose(starts[start].second, shape->shape, local, preferredLocal,
                    width, options.qualityEvaluationBudget, &evaluations, stop);
                const auto coverage = catalog::mapped(shape->shape, proposal.transform);
                const auto nextMissing = catalog::subtract(local.required, coverage);
                const auto nextCoreMissing = catalog::subtract(protectedLocal, coverage);
                if (catalog::area(nextMissing) > catalog::area(originalMissing) + kRayEpsilon * kRayEpsilon
                    || !catalog::subtract(nextCoreMissing, originalCoreMissing).isEmpty()
                    || !catalog::subtract(coverage, region.permitted).isEmpty())
                    continue;
                const auto after = topologyOf(catalog::unite(coverage + others));
                if (after.components > before.components || after.holes != before.holes)
                    continue;
                (*placements)[pivot] = proposal;
                pieces[pivot] = coverage;
                bounds[pivot] = catalog::painterPath(coverage).boundingRect();
                areas[pivot] = catalog::area(coverage);
                for (int member : members)
                    if (member != pivot)
                        retained[member] = false;
                reduction += count - 1;
                accepted = true;
                break;
            }
            if (options.workProgress)
                options.workProgress(placements->size() - reduction, evaluations, options.qualityEvaluationBudget);
            if (accepted)
                break;
        }
    }
    QVector<PenPlacement> result;
    for (int index = 0; index < placements->size(); ++index)
        if (retained[index])
            result.push_back((*placements)[index]);
    *placements = std::move(result);
    *diagnostics = {{QStringLiteral("groups"), groups}, {QStringLiteral("evaluations"), evaluations},
        {QStringLiteral("shapeReduction"), reduction}, {QStringLiteral("milliseconds"), timer.elapsed()}};

    return reduction;
}

QPainterPath tracePath(const QVector<LiningStrokeSegment> &segments) {
    QPainterPath result(segments.front().centerline.start);

    for (const auto &segment : segments)
        segment.centerline.appendTo(result);

    return result;
}

double traceWidth(const QVector<LiningStrokeSegment> &segments) {
    double result = std::numeric_limits<double>::infinity();

    for (const auto &segment : segments)
        result = std::min({result, segment.startWidth, segment.endWidth});

    return result;
}

catalog::Polygons traceStroke(const QVector<LiningStrokeSegment> &segments, double widthRatio) {

    return normalizedStroke(buildLiningRibbon(segments, widthRatio), traceWidth(segments));
}

QPointF traceDirection(const QPainterPath &path, double percent) {
    const double angle = path.angleAtPercent(percent) * std::acos(-1.0) / 180.0;

    return {std::cos(angle), -std::sin(angle)};
}

double traceDistance(const QPointF &point, const QPolygonF &profile) {
    double result = std::numeric_limits<double>::infinity();

    for (int index = 1; index < profile.size(); ++index) {
        const auto delta = profile[index] - profile[index - 1];
        const double squaredLength = QPointF::dotProduct(delta, delta);
        const double fraction = squaredLength > 0.0
            ? std::clamp(QPointF::dotProduct(point - profile[index - 1], delta) / squaredLength, 0.0, 1.0) : 0.0;
        result = std::min(result, QLineF(point, profile[index - 1] + delta * fraction).length());
    }

    return result;
}

double traceTangentError(const QPointF &first, const QPointF &second) {
    const double product = QLineF({}, first).length() * QLineF({}, second).length();

    if (product <= kRayEpsilon || QPointF::dotProduct(first, second) <= 0.0)
        return 1.0;

    return std::abs(first.x() * second.y() - first.y() * second.x()) / product;
}

struct TraceAlternative {
    QPolygonF profile;
    PenPlacement placement;
    int capVariant = 0;
    double curveError = 0.0;
    double tangentError = 0.0;
    double matchCost = 0.0;
    double contourCost = 0.0;
};

struct TraceSpan {
    QVector<LiningStrokeSegment> segments;
    QPolygonF profile;
    PenPlacement placement;
    QVector<TraceAlternative> alternatives;
    int group = 0;
    double curveError = 0.0;
    double tangentError = 0.0;
};

struct TraceProposal {
    PenPlacement placement;
    QPolygonF profile;
    int pivot = 0;
};

double traceProfileAllowanceWidthFraction(const FillOptions &options) {
    return kTraceCurveErrorWidthFraction + std::min(kTraceMaximumSpineOffsetWidthFraction,
        options.liningOutsideMarginWidthFraction * kTraceMarginSpineFraction);
}

QVector<compact::BoundaryExclusion> traceEndpointExclusions(const QVector<LiningStrokeSegment> &segments) {
    if (segments.isEmpty())
        return {};

    return {{segments.front().centerline.start, segments.front().startWidth * kTraceEndpointRadiusWidthFraction},
        {segments.back().centerline.end, segments.back().endWidth * kTraceEndpointRadiusWidthFraction}};
}

double traceContourCost(const compact::BoundaryModel &boundary, const compact::BoundaryMetrics &metrics) {
    return boundary.energy(metrics) / std::max(boundary.perimeter(), kRayEpsilon);
}

double traceContourEnergy(const compact::BoundaryModel &boundary, const compact::BoundaryMetrics &metrics, double width) {
    return boundary.energy(metrics) + width * kTraceContourPeakWeight * metrics.maximumExcessTurn * metrics.maximumExcessTurn;
}

bool traceStraight(const QVector<LiningStrokeSegment> &segments) {
    const auto path = tracePath(segments);
    const QPolygonF chord{path.pointAtPercent(0.0), path.pointAtPercent(1.0)};
    const double tolerance = traceWidth(segments) * kTraceStraightErrorWidthFraction;

    for (int sample = 1; sample + 1 < kTraceSamples; ++sample)
        if (traceDistance(path.pointAtPercent(double(sample) / (kTraceSamples - 1)), chord) > tolerance)
            return false;

    return true;
}

struct TraceWidthProbe {
    QPointF point;
    QPointF normal;
    double width = 0.0;
};

QVector<TraceWidthProbe> traceWidthProbes(const QVector<LiningStrokeSegment> &segments) {
    QVector<TraceWidthProbe> result;
    QVector<QPainterPath> paths;
    QVector<double> lengths;
    double totalLength = 0.0;
    int segmentIndex = 0;

    for (const auto &segment : segments) {
        paths.push_back(tracePath({segment}));
        totalLength += paths.back().length();
        lengths.push_back(totalLength);
    }
    for (int sample = 0; sample < kTraceWidthSamples; ++sample) {
        const double along = totalLength * (sample + 1.0) / (kTraceWidthSamples + 1.0);
        while (segmentIndex + 1 < segments.size() && along > lengths[segmentIndex])
            ++segmentIndex;
        const double localLength = along - (segmentIndex > 0 ? lengths[segmentIndex - 1] : 0.0);
        const double fraction = paths[segmentIndex].percentAtLength(localLength);
        const auto tangent = traceDirection(paths[segmentIndex], fraction);
        const auto &segment = segments[segmentIndex];
        result.push_back({paths[segmentIndex].pointAtPercent(fraction), QPointF(-tangent.y(), tangent.x()),
            segment.startWidth * (1.0 - fraction) + segment.endWidth * fraction});
    }

    return result;
}

double traceSectionWidth(const catalog::Polygons &polygons, const TraceWidthProbe &probe, double allowance) {
    QVector<double> crossings;
    const auto tangent = QPointF(-probe.normal.y(), probe.normal.x());
    double bestDistance = std::numeric_limits<double>::infinity();
    double result = 0.0;

    for (const auto &polygon : polygons)
        for (int index = 0; index < polygon.size(); ++index) {
            const auto first = polygon[index] - probe.point;
            const auto last = polygon[(index + 1) % polygon.size()] - probe.point;
            const double start = QPointF::dotProduct(first, tangent);
            const double end = QPointF::dotProduct(last, tangent);
            if ((start <= 0.0 && end > 0.0) || (end <= 0.0 && start > 0.0)) {
                const double fraction = -start / (end - start);
                crossings.push_back(QPointF::dotProduct(first + (last - first) * fraction, probe.normal));
            }
        }
    std::sort(crossings.begin(), crossings.end());
    for (int index = 0; index + 1 < crossings.size(); index += 2) {
        const double distance = std::max({crossings[index], -crossings[index + 1], 0.0});
        if (distance <= allowance && distance < bestDistance) {
            result = crossings[index + 1] - crossings[index];
            bestDistance = distance;
        }
    }

    return result;
}

std::optional<double> traceWidthError(const catalog::Polygons &coverage,
                                      const QVector<TraceWidthProbe> &probes, double allowance, bool nativeTaper = false) {
    const auto extremes = std::minmax_element(probes.cbegin(), probes.cend(), [](const auto &left, const auto &right) {
        return left.width < right.width;
    });
    const bool variable = extremes.second->width > extremes.first->width * (1.0 + kTraceWidthVariationTolerance);
    double error = 0.0;
    double meanRatio = 0.0;

    for (int index = 0; index < probes.size(); ++index) {
        const double ratio = traceSectionWidth(coverage, probes[index], allowance) / probes[index].width;
        if (variable && !nativeTaper && index >= kTraceTaperProbes && index + kTraceTaperProbes < probes.size()
            && ratio < kTraceVariableWidthMinimumRatio)
            return {};
        meanRatio += ratio;
        error += std::abs(ratio - 1.0);
    }
    if (meanRatio / probes.size() < kTraceMinimumBodyWidthRatio)
        return {};

    return error / probes.size();
}

QVector<QPointF> traceCapAnchors(const QVector<PenPoint> &points) {
    QVector<QPointF> result{points.front().position, points.back().position};

    for (const auto &point : points)
        if (point.kind == PenPointKind::Hard)
            result.push_back(point.position);

    return result;
}

QVector<compact::BoundaryExclusion> traceHardPointExclusions(const QVector<LiningStrokeSegment> &segments,
                                                            const QVector<QPointF> &capAnchors) {
    QVector<compact::BoundaryExclusion> result;

    for (const auto &segment : segments)
        for (const auto &endpoint : {std::pair{segment.centerline.start, segment.startWidth},
                std::pair{segment.centerline.end, segment.endWidth}})
            if (std::any_of(capAnchors.cbegin(), capAnchors.cend(), [&](const auto &anchor) {
                    return QLineF(anchor, endpoint.first).length() <= kRayEpsilon;
                }) && std::none_of(result.cbegin(), result.cend(), [&](const auto &exclusion) {
                    return QLineF(exclusion.point, endpoint.first).length() <= kRayEpsilon;
                }))
                result.push_back({endpoint.first, endpoint.second * kTraceEndpointRadiusWidthFraction});

    return result;
}

QVector<TraceWidthProbe> traceJoinProbes(const QVector<LiningStrokeSegment> &segments,
                                        const QVector<QPointF> &capAnchors) {
    QVector<TraceWidthProbe> result;
    const auto path = tracePath(segments);

    for (bool end : {false, true}) {
        const auto position = path.pointAtPercent(end ? 1.0 : 0.0);
        const auto tangent = traceDirection(path, end ? 1.0 : 0.0);
        if (std::none_of(capAnchors.cbegin(), capAnchors.cend(), [&](const auto &anchor) {
                return QLineF(anchor, position).length() <= kRayEpsilon;
            }))
            result.push_back({position, QPointF(-tangent.y(), tangent.x()),
                end ? segments.back().endWidth : segments.front().startWidth});
    }

    return result;
}

double traceTangentLimit(const QPointF &point, const QVector<QPointF> &capAnchors) {
    return std::any_of(capAnchors.cbegin(), capAnchors.cend(), [&](const auto &anchor) {
        return QLineF(anchor, point).length() <= kRayEpsilon;
    }) ? kTraceHardPointTangentLimit : kTraceTangentLimit;
}

bool traceJoinsWide(const catalog::Polygons &coverage, const QVector<TraceWidthProbe> &probes, double allowance,
                     const catalog::Polygons &leeway = {}) {
    const auto observed = leeway.isEmpty() ? coverage : catalog::unite(coverage + leeway);

    return std::all_of(probes.cbegin(), probes.cend(), [&](const auto &probe) {
        return traceSectionWidth(observed, probe, allowance) >= probe.width * kTraceMinimumJoinWidthRatio;
    });
}

std::pair<QVector<LiningStrokeSegment>, QVector<LiningStrokeSegment>> splitTraceSegments(
    const QVector<LiningStrokeSegment> &segments) {
    QVector<LiningStrokeSegment> first;
    QVector<LiningStrokeSegment> second;
    double remaining = tracePath(segments).length() * 0.5;

    for (const auto &segment : segments) {
        const QPainterPath path = tracePath({segment});
        const double length = path.length();
        if (remaining >= length) {
            first.push_back(segment);
            remaining -= length;
        } else if (remaining > kRayEpsilon) {
            const double fraction = path.percentAtLength(remaining);
            const auto parts = segment.centerline.split(fraction);
            const double middleWidth = segment.startWidth * (1.0 - fraction) + segment.endWidth * fraction;
            first.push_back({parts.first, segment.startWidth, middleWidth});
            second.push_back({parts.second, middleWidth, segment.endWidth});
            remaining = 0.0;
        } else {
            second.push_back(segment);
        }
    }

    return {first, second};
}

class TraceFitter {
public:
    TraceFitter(const QVector<catalog::Primitive> &primitives, const FillOptions &options,
                const catalog::Region &region, const QVector<QPointF> &capAnchors, const std::function<bool()> &cancelled)
        : primitives_(primitives), options_(options), region_(region), envelope_(region.permitted),
          capAnchors_(capAnchors), cancelled_(cancelled), remainingTrials_(options.profileTrialBudget) {
        for (const auto &primitive : primitives)
            if (primitive.stroke)
                strokePrimitives_.push_back(primitive);
            else if (primitive.shape.shapeId == kTraceSquareShapeId || primitive.shape.shapeId == kTraceRectangleShapeId)
                fallbackPrimitives_.push_back(primitive);
        timer_.start();
    }

    std::optional<TraceSpan> fit(const QVector<LiningStrokeSegment> &segments, int group, bool allowFallback = false,
                                 bool precise = false) {
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        stream << group << allowFallback << precise;
        for (const auto &segment : segments)
            stream << segment.centerline.start << segment.centerline.control << segment.centerline.control2
                << segment.centerline.end << segment.centerline.curved << segment.startWidth << segment.endWidth;
        const auto cached = fits_.constFind(key);

        if (cached != fits_.cend())
            return *cached;
        if (stopped(cancelled_) || remainingTrials_ <= 0 || timer_.elapsed() >= options_.qualityTimeBudgetMilliseconds)
            return {};
        auto result = fitCatalog(segments, group, strokePrimitives_, precise);
        if (!result && (allowFallback || strokePrimitives_.isEmpty()) && traceStraight(segments))
            result = fitCatalog(segments, group, fallbackPrimitives_, precise);
        fits_.insert(key, result);

        return result;
    }

    std::optional<TraceSpan> fitCatalog(const QVector<LiningStrokeSegment> &segments, int group,
                                       const QVector<catalog::Primitive> &sources, bool precise) {
        if (sources.isEmpty())
            return {};
        const auto path = tracePath(segments);
        const double width = traceWidth(segments);
        const auto required = catalog::subtract(traceStroke(segments, kTraceSupportWidthFraction), region_.leeway);
        const auto preferred = catalog::subtract(traceStroke(segments, kTracePreferredWidthRatio), region_.leeway);
        const auto start = path.pointAtPercent(0.0);
        const auto end = path.pointAtPercent(1.0);
        const auto chord = end - start;
        const auto normal = traceDirection(path, 0.5);
        const auto across = QPointF(-normal.y(), normal.x());
        const auto widthProbes = traceWidthProbes(segments);
        const auto joinProbes = traceJoinProbes(segments, capAnchors_);
        const double startTangentLimit = traceTangentLimit(start, capAnchors_);
        const double endTangentLimit = traceTangentLimit(end, capAnchors_);
        QPolygonF targetSamples;
        QJsonObject matching;
        QVector<TraceAlternative> poses;
        std::array<int, 5> rejected{};
        const double length = path.length();
        const double chordLength = QLineF(start, end).length();
        const double errorLimit = width * (precise ? kTraceCurveErrorWidthFraction : traceProfileAllowanceWidthFraction(options_));
        const double traceAllowance = width * kTraceCurveErrorWidthFraction;
        for (int sample = 0; sample < kTraceSamples; ++sample)
            targetSamples.push_back(path.pointAtPercent(path.percentAtLength(length * sample / (kTraceSamples - 1))));
        if (required.isEmpty())
            return TraceSpan{segments, {}, {}, {}, group};
        const auto consider = [&](const PenPlacement &placement, const QPolygonF &profile, int capVariant) {
            if (stopped(cancelled_) || profile.size() < 2)
                return;
            const double startError = traceTangentError(profile[1] - profile.front(), traceDirection(path, 0.0));
            const double endError = traceTangentError(profile.back() - profile[profile.size() - 2], traceDirection(path, 1.0));
            const double tangentError = std::max(startError, endError);
            if (startError > startTangentLimit || endError > endTangentLimit) {
                ++rejected[0];
                return;
            }
            double curveError = 0.0;
            for (const auto &point : targetSamples) {
                curveError = std::max(curveError, traceDistance(point, profile));
                if (curveError > errorLimit) {
                    ++rejected[1];
                    return;
                }
            }
            const double qualityCost = std::max(startError / startTangentLimit, endError / endTangentLimit) + curveError / errorLimit;
            const auto primitive = std::find_if(primitives_.cbegin(), primitives_.cend(), [&](const auto &entry) {
                return entry.shape.shapeId == placement.shapeId;
            });
            if (primitive == primitives_.cend())
                return;
            const auto coverage = catalog::mapped(primitive->shape, placement.transform);
            const auto topology = topologyOf(coverage);
            if (topology.components != 1 || topology.holes != 0)
                return;
            if (!traceJoinsWide(coverage, joinProbes, traceAllowance, region_.leeway)) {
                ++rejected[2];
                return;
            }
            const auto widthError = traceWidthError(coverage, widthProbes, traceAllowance,
                sources.front().stroke && joinProbes.isEmpty());
            if (!widthError) {
                ++rejected[3];
                return;
            }
            const double cost = qualityCost + *widthError * kTraceWidthErrorWeight;
            int count = 0;
            int worst = -1;
            for (int index = 0; index < poses.size(); ++index)
                if (poses[index].placement.shapeId == placement.shapeId && poses[index].capVariant == capVariant) {
                    ++count;
                    if (worst < 0 || poses[index].matchCost > poses[worst].matchCost)
                        worst = index;
                }
            if (count >= kTraceCandidatePosesPerShape && cost >= poses[worst].matchCost)
                return;
            if (!probesInside(primitive->shape, placement.transform, envelope_)
                || !catalog::subtract(coverage, region_.permitted).isEmpty()
                || !catalog::subtract(required, catalog::expanded(coverage, traceAllowance)).isEmpty()) {
                ++rejected[4];
                return;
            }
            const TraceAlternative pose{profile, placement, capVariant, curveError, tangentError, cost};
            if (count >= kTraceCandidatePosesPerShape)
                poses[worst] = pose;
            else
                poses.push_back(pose);
        };
        const auto stopProfiles = [&] {
            return stopped(cancelled_) || timer_.elapsed() >= options_.qualityTimeBudgetMilliseconds;
        };
        for (const auto &candidate : profile::traceCandidates(path, sources, width * kTracePreferredWidthRatio, errorLimit,
                options_.useGpu, &remainingTrials_, stopProfiles, &matching)) {
            for (double extension : {0.0, kTraceJoinExtensionWidthFraction, kTraceCapExtensionWidthFraction}) {
                for (double thickness : kTraceThicknessScales) {
                    QTransform growth;
                    const auto center = path.pointAtPercent(0.5);
                    const double angle = std::atan2(chord.y(), chord.x()) * 180.0 / std::acos(-1.0);
                    growth.translate(center.x(), center.y());
                    growth.rotate(angle);
                    const double clearance = extension > 0.0
                        ? kPlacementClearanceFactor * 2.0 / kStrokeMinimumFlatteningScale : 0.0;
                    growth.scale(1.0 + (width * extension + clearance) / std::max(chordLength, kRayEpsilon), thickness);
                    growth.rotate(-angle);
                    growth.translate(-center.x(), -center.y());
                    for (double offset : {0.0, -kTraceProfileOffsetWidthFraction, kTraceProfileOffsetWidthFraction}) {
                        QTransform shift = QTransform::fromTranslate(across.x() * width * offset, across.y() * width * offset);
                        const auto adjustment = growth * shift;
                        auto placement = candidate.placement;
                        placement.transform = catalog::emittedTransform(placement.transform * adjustment);
                        consider(placement, adjustment.map(candidate.profile), extension == 0.0 ? 0
                            : extension == kTraceJoinExtensionWidthFraction ? 1 : 2);
                    }
                }
            }
        }
        if (capAnchors_.contains(segments.front().centerline.start) && capAnchors_.contains(segments.back().centerline.end)) {
            matching.insert(QStringLiteral("group"), group);
            matching.insert(QStringLiteral("length"), length);
            matching.insert(QStringLiteral("native"), sources.front().stroke);
            matching.insert(QStringLiteral("precise"), precise);
            matching.insert(QStringLiteral("tangentRejected"), rejected[0]);
            matching.insert(QStringLiteral("curveRejected"), rejected[1]);
            matching.insert(QStringLiteral("joinRejected"), rejected[2]);
            matching.insert(QStringLiteral("widthRejected"), rejected[3]);
            matching.insert(QStringLiteral("envelopeRejected"), rejected[4]);
            matching.insert(QStringLiteral("accepted"), poses.size());
            wholeSpanAttempts_.push_back(matching);
        }
        const compact::BoundaryModel boundary(preferred,
            std::clamp(width * kScaleWidthFraction, kMinimumScale, kMaximumScale), traceEndpointExclusions(segments));
        for (auto &pose : poses) {
            const auto coverage = catalog::subtract(coverageOf({pose.placement}, primitives_), region_.leeway);
            const auto metrics = boundary.measure(coverage, coverage);
            pose.contourCost = traceContourCost(boundary, metrics)
                + kTracePeakTurnWeight * metrics.maximumExcessTurn * metrics.maximumExcessTurn;
        }
        std::stable_sort(poses.begin(), poses.end(), [](const auto &left, const auto &right) {
            return left.matchCost + left.contourCost * kTraceBoundaryWeight
                < right.matchCost + right.contourCost * kTraceBoundaryWeight;
        });
        if (poses.size() > kTraceAlternatives)
            poses.resize(kTraceAlternatives);
        ++spansFitted_;
        if (options_.workProgress)
            options_.workProgress(accepted_, options_.profileTrialBudget - remainingTrials_, options_.profileTrialBudget);

        if (poses.isEmpty())
            return {};
        const auto &best = poses.front();

        return TraceSpan{segments, best.profile, best.placement, poses, group, best.curveError, best.tangentError};
    }

    bool splitAndFit(const QVector<LiningStrokeSegment> &segments, int group, int depth, QVector<TraceSpan> *result,
                      bool precise = false) {
        if (stopped(cancelled_) || result->size() >= options_.shapeBudget
            || remainingTrials_ <= 0 || timer_.elapsed() >= options_.qualityTimeBudgetMilliseconds)
            return false;
        if (const auto fitted = fit(segments, group, traceStraight(segments), precise)) {
            result->push_back(*fitted);
            accepted_ = result->size();
            return true;
        }
        if (depth >= kTraceMaximumDepth)
            return false;
        const auto parts = splitTraceSegments(segments);

        return !parts.first.isEmpty() && !parts.second.isEmpty()
            && splitAndFit(parts.first, group, depth + 1, result, precise)
            && splitAndFit(parts.second, group, depth + 1, result, precise);
    }

    QJsonObject diagnostics() const {

        return {{QStringLiteral("profileTrials"), options_.profileTrialBudget - remainingTrials_},
            {QStringLiteral("wholeSpanAttempts"), wholeSpanAttempts_},
            {QStringLiteral("spansFitted"), spansFitted_},
            {QStringLiteral("profileTimeLimitReached"), timer_.elapsed() >= options_.qualityTimeBudgetMilliseconds}};
    }

private:
    const QVector<catalog::Primitive> &primitives_;
    QVector<catalog::Primitive> strokePrimitives_;
    QVector<catalog::Primitive> fallbackPrimitives_;
    const FillOptions &options_;
    const catalog::Region &region_;
    catalog::PointContainment envelope_;
    const QVector<QPointF> &capAnchors_;
    const std::function<bool()> &cancelled_;
    QElapsedTimer timer_;
    QHash<QByteArray, std::optional<TraceSpan>> fits_;
    QJsonArray wholeSpanAttempts_;
    int remainingTrials_;
    int spansFitted_ = 0;
    int accepted_ = 0;
};

catalog::Polygons traceCoverage(const QVector<TraceSpan> &spans, const QVector<catalog::Primitive> &primitives) {
    QVector<PenPlacement> placements;

    for (const auto &span : spans)
        if (span.placement.shapeId != 0)
            placements.push_back(span.placement);

    return coverageOf(placements, primitives);
}

QJsonObject refineTraceContours(QVector<TraceSpan> *spans, const QVector<catalog::Primitive> &primitives,
                                const catalog::Region &region, const catalog::Polygons &ribbon,
                                const QVector<LiningStrokeSegment> &stroke, const QVector<QPointF> &capAnchors,
                                const FillOptions &options, const std::function<bool()> &cancelled);

QJsonObject repairTraceContours(QVector<TraceSpan> *spans, TraceFitter *fitter,
                                const QVector<catalog::Primitive> &primitives, const catalog::Region &region,
                                const compact::BoundaryModel &boundary, const catalog::Polygons &ribbon,
                                const QVector<LiningStrokeSegment> &stroke, const QVector<QPointF> &capAnchors,
                                double width, const FillOptions &options,
                                const std::function<bool()> &cancelled) {
    QVector<QPointF> attempted;
    QJsonArray attempts;
    auto coverage = traceCoverage(*spans, primitives);
    auto metrics = boundary.measure(catalog::subtract(coverage, region.leeway));
    const auto initialMetrics = metrics;
    int trials = 0;
    int accepted = 0;

    while (trials < kTraceContourSplitTrials && spans->size() < options.shapeBudget && !stopped(cancelled)
        && metrics.maximumExcessTurn > kTraceSmoothTurnLimit) {
        QVector<compact::BoundaryDefect> defects;
        const auto visible = catalog::subtract(coverage, region.leeway);
        boundary.measure(visible, boundary.observationSupport(visible), QRectF(), &defects);
        std::stable_sort(defects.begin(), defects.end(), [](const auto &left, const auto &right) {
            return left.energy > right.energy;
        });
        int pivot = -1;
        for (const auto &defect : defects) {
            double nearest = std::numeric_limits<double>::infinity();
            for (int index = 0; index < spans->size(); ++index) {
                const auto &span = (*spans)[index];
                const auto center = tracePath(span.segments).pointAtPercent(0.5);
                if (span.placement.shapeId == 0 || std::any_of(attempted.cbegin(), attempted.cend(), [&](const auto &point) {
                        return QLineF(point, center).length() <= kRayEpsilon;
                    }))
                    continue;
                for (auto polygon : coverageOf({span.placement}, primitives)) {
                    polygon.push_back(polygon.front());
                    const double distance = traceDistance(defect.point, polygon);
                    if (distance < nearest) {
                        nearest = distance;
                        pivot = index;
                    }
                }
            }
            if (pivot >= 0)
                break;
        }
        if (pivot < 0)
            break;
        const auto original = (*spans)[pivot];
        const auto parts = splitTraceSegments(original.segments);
        QVector<TraceSpan> replacements;
        attempted.push_back(tracePath(original.segments).pointAtPercent(0.5));
        ++trials;
        if (parts.first.isEmpty() || parts.second.isEmpty()
            || !fitter->splitAndFit(parts.first, original.group, 0, &replacements, true)
            || !fitter->splitAndFit(parts.second, original.group, 0, &replacements, true)
            || replacements.size() + spans->size() - 1 > options.shapeBudget) {
            attempts.push_back(QJsonObject{{QStringLiteral("shapeId"), original.placement.shapeId},
                {QStringLiteral("fitFailed"), true}});
            continue;
        }
        if (capAnchors.contains(original.segments.front().centerline.start)
            && capAnchors.contains(original.segments.back().centerline.end)
            && original.placement.shapeId != kTraceSquareShapeId && original.placement.shapeId != kTraceRectangleShapeId
            && std::any_of(replacements.cbegin(), replacements.cend(), [](const auto &span) {
                return span.placement.shapeId == kTraceSquareShapeId || span.placement.shapeId == kTraceRectangleShapeId;
            }))
            continue;
        auto proposed = *spans;
        proposed.removeAt(pivot);
        for (int index = 0; index < replacements.size(); ++index)
            proposed.insert(pivot + index, replacements[index]);
        auto polishing = options;
        polishing.qualityEvaluationBudget = kTraceRepairPolishTrials;
        polishing.qualityTimeBudgetMilliseconds = std::min(options.qualityTimeBudgetMilliseconds, kTraceRepairPolishMilliseconds);
        refineTraceContours(&proposed, primitives, region, ribbon, stroke, capAnchors, polishing, cancelled);
        const auto nextCoverage = traceCoverage(proposed, primitives);
        const auto nextMetrics = boundary.measure(catalog::subtract(nextCoverage, region.leeway));
        attempts.push_back(QJsonObject{{QStringLiteral("shapeId"), original.placement.shapeId},
            {QStringLiteral("replacements"), replacements.size()},
            {QStringLiteral("components"), nextMetrics.components},
            {QStringLiteral("holes"), nextMetrics.holes},
            {QStringLiteral("energy"), traceContourEnergy(boundary, nextMetrics, width)},
            {QStringLiteral("previousEnergy"), traceContourEnergy(boundary, metrics, width)},
            {QStringLiteral("maximumExcessTurn"), nextMetrics.maximumExcessTurn},
            {QStringLiteral("missingTrace"), catalog::area(catalog::subtract(region.visible,
                catalog::expanded(nextCoverage, width * kTraceCurveErrorWidthFraction)))}});
        if (nextMetrics.components > metrics.components || nextMetrics.holes != metrics.holes
            || topologyOf(catalog::unite(nextCoverage + region.leeway)).components
                > topologyOf(catalog::unite(coverage + region.leeway)).components
            || traceContourEnergy(boundary, nextMetrics, width) >= traceContourEnergy(boundary, metrics, width)
            || nextMetrics.maximumExcessTurn > metrics.maximumExcessTurn + kTraceMergeTurnSlack
            || !catalog::subtract(region.visible, catalog::expanded(nextCoverage, width * kTraceCurveErrorWidthFraction)).isEmpty()
            || !catalog::subtract(nextCoverage, region.permitted).isEmpty() || !interiorHoles(nextCoverage, region).isEmpty())
            continue;
        *spans = proposed;
        coverage = nextCoverage;
        metrics = nextMetrics;
        ++accepted;
    }

    return {{QStringLiteral("trials"), trials}, {QStringLiteral("accepted"), accepted}, {QStringLiteral("attempts"), attempts},
        {QStringLiteral("before"), boundary.diagnostics(initialMetrics)},
        {QStringLiteral("after"), boundary.diagnostics(metrics)}};
}

QJsonObject refineTraceContours(QVector<TraceSpan> *spans, const QVector<catalog::Primitive> &primitives,
                                const catalog::Region &region, const catalog::Polygons &ribbon,
                                const QVector<LiningStrokeSegment> &stroke, const QVector<QPointF> &capAnchors,
                                const FillOptions &options,
                                const std::function<bool()> &cancelled) {
    QVector<catalog::Polygons> pieces;
    QVector<QPolygonF> targets;
    QVector<QPointF> startTangents;
    QVector<QPointF> endTangents;
    QVector<QVector<TraceWidthProbe>> widthProbes;
    QVector<bool> nativeTaper;
    QVector<TraceWidthProbe> joinProbes;
    QElapsedTimer timer;
    const double minimumWidth = traceWidth(stroke);
    const compact::BoundaryModel boundary(catalog::subtract(ribbon, region.leeway),
        std::clamp(minimumWidth * kScaleWidthFraction, kMinimumScale, kMaximumScale),
        traceHardPointExclusions(stroke, capAnchors));
    const int budget = options.qualityEvaluationBudget > 0
        ? std::min(kTraceContourTrials, options.qualityEvaluationBudget) : kTraceContourTrials;
    int trials = 0;
    int accepted = 0;
    int replacements = 0;
    timer.start();

    for (const auto &span : *spans) {
        pieces.push_back(span.placement.shapeId == 0 ? catalog::Polygons{}
            : coverageOf({span.placement}, primitives));
        const auto path = tracePath(span.segments);
        const double length = path.length();
        QPolygonF samples;
        for (int sample = 0; sample < kTraceSamples; ++sample)
            samples.push_back(path.pointAtPercent(path.percentAtLength(length * sample / (kTraceSamples - 1))));
        targets.push_back(samples);
        startTangents.push_back(traceDirection(path, 0.0));
        endTangents.push_back(traceDirection(path, 1.0));
        widthProbes.push_back(traceWidthProbes(span.segments));
        const auto spanJoins = traceJoinProbes(span.segments, capAnchors);
        joinProbes += spanJoins;
        nativeTaper.push_back(spanJoins.isEmpty()
            && span.placement.shapeId != kTraceSquareShapeId && span.placement.shapeId != kTraceRectangleShapeId);
    }
    const auto assembled = [&] {
        catalog::Polygons result;
        for (const auto &piece : pieces)
            result += piece;

        return catalog::unite(result);
    };
    const auto coverage = assembled();
    auto metrics = boundary.measure(catalog::subtract(coverage, region.leeway));
    auto exactTopology = topologyOf(catalog::unite(coverage + region.leeway));
    const auto initialTopology = exactTopology;
    const auto initialMetrics = metrics;
    const double traceAllowance = minimumWidth * kTraceCurveErrorWidthFraction;
    double missing = catalog::area(catalog::subtract(region.visible, catalog::expanded(coverage, traceAllowance)));
    double continuityCost = traceContourEnergy(boundary, metrics, minimumWidth);
    const auto finished = [&] {
        return stopped(cancelled) || trials >= budget || timer.elapsed() >= options.qualityTimeBudgetMilliseconds;
    };
    const auto diagnostics = [&] {
        return QJsonObject{{QStringLiteral("trials"), trials}, {QStringLiteral("accepted"), accepted},
            {QStringLiteral("shapeReplacements"), replacements},
            {QStringLiteral("exactComponentsBefore"), initialTopology.components},
            {QStringLiteral("exactComponentsAfter"), exactTopology.components},
            {QStringLiteral("before"), boundary.diagnostics(initialMetrics)},
            {QStringLiteral("after"), boundary.diagnostics(metrics)}};
    };
    const auto consider = [&](const QVector<TraceProposal> &proposals) {
        QVector<TraceAlternative> checked;
        QVector<catalog::Polygons> additions;
        QVector<catalog::Polygons> originals;

        ++trials;
        for (const auto &proposal : proposals) {
            const auto &profile = proposal.profile;
            const int pivot = proposal.pivot;
            const auto &span = (*spans)[pivot];
            const double localWidth = traceWidth(span.segments);
            double curveError = 0.0;
            if (profile.size() < 2)
                return;
            const double startError = traceTangentError(profile[1] - profile.front(), startTangents[pivot]);
            const double endError = traceTangentError(profile.back() - profile[profile.size() - 2], endTangents[pivot]);
            if (startError > traceTangentLimit(span.segments.front().centerline.start, capAnchors)
                || endError > traceTangentLimit(span.segments.back().centerline.end, capAnchors))
                return;
            for (const auto &point : targets[pivot])
                curveError = std::max(curveError, traceDistance(point, profile));
            if (curveError > localWidth * traceProfileAllowanceWidthFraction(options))
                return;
            const auto proposed = coverageOf({proposal.placement}, primitives);
            if (!catalog::subtract(proposed, region.permitted).isEmpty()
                || !traceWidthError(proposed, widthProbes[pivot], localWidth * kTraceCurveErrorWidthFraction, nativeTaper[pivot]))
                return;
            checked.push_back({profile, proposal.placement, 0, curveError, std::max(startError, endError)});
            additions.push_back(proposed);
            originals.push_back(pieces[pivot]);
        }
        for (int index = 0; index < proposals.size(); ++index)
            pieces[proposals[index].pivot] = additions[index];
        const auto nextCoverage = assembled();
        for (int index = 0; index < proposals.size(); ++index)
            pieces[proposals[index].pivot] = originals[index];
        const double nextMissing = catalog::area(catalog::subtract(region.visible,
            catalog::expanded(nextCoverage, traceAllowance)));
        if (nextMissing > missing || !interiorHoles(nextCoverage, region).isEmpty()
            || !traceJoinsWide(nextCoverage, joinProbes, traceAllowance, region.leeway))
            return;
        const auto nextMetrics = boundary.measure(catalog::subtract(nextCoverage, region.leeway));
        const auto nextTopology = topologyOf(catalog::unite(nextCoverage + region.leeway));
        const double nextCost = traceContourEnergy(boundary, nextMetrics, minimumWidth);
        if (nextMetrics.components > metrics.components || nextMetrics.holes < metrics.holes
            || nextTopology.components > exactTopology.components
            || nextMetrics.maximumExcessTurn > std::max(initialMetrics.maximumExcessTurn, kTraceSmoothTurnLimit)
            || (nextMissing == missing && nextMetrics.components == metrics.components
                && nextTopology.components == exactTopology.components && nextCost >= continuityCost))
            return;
        for (int index = 0; index < proposals.size(); ++index) {
            const int pivot = proposals[index].pivot;
            const auto &candidate = checked[index];
            auto &span = (*spans)[pivot];
            replacements += span.placement.shapeId != candidate.placement.shapeId;
            span.placement = candidate.placement;
            span.profile = candidate.profile;
            span.curveError = candidate.curveError;
            span.tangentError = candidate.tangentError;
            pieces[pivot] = additions[index];
        }
        metrics = nextMetrics;
        exactTopology = nextTopology;
        missing = nextMissing;
        continuityCost = nextCost;
        ++accepted;
    };
    const auto overlapProposal = [&](int pivot, bool end, double extension, double widthTransfer) {
        const auto &span = (*spans)[pivot];
        const auto path = tracePath(span.segments);
        const auto anchor = path.pointAtPercent(end ? 0.0 : 1.0);
        const auto chord = path.pointAtPercent(1.0) - path.pointAtPercent(0.0);
        const double length = QLineF({}, chord).length();
        const double angle = std::atan2(chord.y(), chord.x()) * 180.0 / std::acos(-1.0);
        QTransform adjustment;
        adjustment.translate(anchor.x(), anchor.y());
        adjustment.rotate(angle);
        adjustment.scale(1.0 + extension / std::max(length, kRayEpsilon), 1.0 + widthTransfer);
        adjustment.rotate(-angle);
        adjustment.translate(-anchor.x(), -anchor.y());
        auto placement = span.placement;
        placement.transform = catalog::emittedTransform(placement.transform * adjustment);

        return TraceProposal{placement, adjustment.map(span.profile), pivot};
    };
    for (int pass = 0; pass < kTraceContourPasses; ++pass) {
        for (int pivot = 0; pivot < spans->size(); ++pivot) {
            const auto alternatives = (*spans)[pivot].alternatives;
            for (const auto &alternative : alternatives) {
                if (finished())
                    return diagnostics();
                consider({{alternative.placement, alternative.profile, pivot}});
            }
        }
        for (double step : kTraceContourSteps)
            for (int pivot = 0; pivot < spans->size(); ++pivot) {
                auto &span = (*spans)[pivot];
                if (span.placement.shapeId == 0)
                    continue;
                const auto path = tracePath(span.segments);
                const auto center = path.pointAtPercent(0.5);
                const auto direction = traceDirection(path, 0.5);
                QTransform localAxes;
                localAxes.rotate(-std::atan2(direction.y(), direction.x()) * 180.0 / std::acos(-1.0));
                localAxes.translate(-center.x(), -center.y());
                const auto localBounds = localAxes.map(path).boundingRect();
                const double angle = std::atan2(direction.y(), direction.x()) * 180.0 / std::acos(-1.0);
                const double localWidth = traceWidth(span.segments);
                const double alongExtent = std::max(localWidth, localBounds.width());
                const double acrossExtent = std::max(localWidth, localBounds.height());
                const double displacement = localWidth * kTraceContourStep * step;
                for (int parameter = 0; parameter < kPoseParameters; ++parameter)
                    for (double sign : {-1.0, 1.0}) {
                        if (finished())
                            return diagnostics();
                        QTransform adjustment;
                        adjustment.translate(center.x(), center.y());
                        adjustment.rotate(angle);
                        if (parameter < 2)
                            adjustment.translate(parameter == 0 ? sign * displacement : 0.0,
                                parameter == 1 ? sign * displacement : 0.0);
                        else if (parameter < 4)
                            adjustment.scale(parameter == 2 ? 1.0 + sign * displacement / alongExtent : 1.0,
                                parameter == 3 ? 1.0 + sign * displacement / acrossExtent : 1.0);
                        else if (parameter == 4)
                            adjustment.shear(sign * displacement / acrossExtent, 0.0);
                        else
                            adjustment.rotate(sign * std::atan2(displacement, std::max(alongExtent, acrossExtent))
                                * 180.0 / std::acos(-1.0));
                        adjustment.rotate(-angle);
                        adjustment.translate(-center.x(), -center.y());
                        auto placement = span.placement;
                        placement.transform = catalog::emittedTransform(placement.transform * adjustment);
                        consider({{placement, adjustment.map(span.profile), pivot}});
                    }
            }
        for (int pivot = 0; pivot + 1 < spans->size(); ++pivot) {
            if ((*spans)[pivot].placement.shapeId == 0 || (*spans)[pivot + 1].placement.shapeId == 0)
                continue;
            const double joinWidth = std::min((*spans)[pivot].segments.back().endWidth,
                (*spans)[pivot + 1].segments.front().startWidth);
            for (double overlap : kTraceJoinOverlapSteps)
                for (double balance : kTraceJoinWidthTransfers) {
                    if (finished())
                        return diagnostics();
                    consider({overlapProposal(pivot, true, joinWidth * overlap, balance),
                        overlapProposal(pivot + 1, false, joinWidth * overlap, -balance)});
                }
        }
    }

    return diagnostics();
}

catalog::FillResult fillOpenTrace(const LiningPath &path, const QVector<PenPoint> &points, double width,
                                 const QVector<catalog::Primitive> &primitives, const FillOptions &options,
                                 const std::function<bool()> &cancelled) {
    catalog::FillResult result;
    QVector<TraceSpan> spans;
    QVector<LiningStrokeSegment> pending;
    QVector<TraceWidthProbe> joinProbes;
    const auto stroke = buildLiningStroke(points, width);
    const auto capAnchors = traceCapAnchors(points);
    const double maximumWidthRatio = 1.0 + 2.0 * options.liningOutsideMarginWidthFraction;
    QElapsedTimer timer;
    QElapsedTimer stageTimer;
    QJsonObject timings;
    int group = 0;
    int merged = 0;
    double maximumCurveError = 0.0;
    double maximumCurveErrorWidthFraction = 0.0;
    double maximumWidth = width;
    double maximumTangentError = 0.0;
    timer.start();
    stageTimer.start();
    result.fill.shapeLimit = options.shapeBudget;

    try {
        if (stopped(cancelled)) {
            result.fill.cancelled = true;
            return result;
        }
        if (options.shapeBudget < 1 || options.profileTrialBudget < 1
            || options.qualityTimeBudgetMilliseconds < 1 || primitives.isEmpty()
            || !std::isfinite(options.liningOutsideMarginWidthFraction) || options.liningOutsideMarginWidthFraction < 0.0)
            throw std::runtime_error("Lining requires native shapes and positive shape and profile budgets");
        for (const auto &point : points) {
            if (!std::isfinite(point.strokeWidth) || point.strokeWidth < 0.0)
                throw std::runtime_error("Lining anchor widths must be finite and nonnegative");
            maximumWidth = std::max(maximumWidth, liningPointWidth(point, width));
        }
        timings.insert(QStringLiteral("setup"), stageTimer.restart());
        if (options.phaseProgress)
            options.phaseProgress(QStringLiteral("traceSpans"));
        const auto ribbon = traceStroke(stroke, 1.0);
        auto region = visibleRegion(polygonRegion(traceStroke(stroke, kTraceSupportWidthFraction)), options.leeway);
        region.permitted = catalog::unite(traceStroke(stroke, maximumWidthRatio) + region.leeway);
        region.permittedPath = catalog::painterPath(region.permitted);
        const compact::BoundaryModel boundary(catalog::subtract(ribbon, region.leeway),
            std::clamp(traceWidth(stroke) * kScaleWidthFraction, kMinimumScale, kMaximumScale),
            traceHardPointExclusions(stroke, capAnchors));
        TraceFitter fitter(primitives, options, region, capAnchors, cancelled);
        for (int index = 0; index < path.segments.size(); ++index) {
            pending.push_back(stroke[index]);
            if (index + 1 < path.segments.size()) {
                if (points[index + 1].kind != PenPointKind::Hard)
                    continue;
            }
            const int spanStart = spans.size();
            if (!fitter.splitAndFit(pending, group, 0, &spans)) {
                spans.resize(spanStart);
                if (spanStart >= options.shapeBudget)
                    throw std::runtime_error("Lining exceeds its shape budget");
                const auto fallback = fitter.fit(pending, group, true);
                if (!fallback)
                    throw std::runtime_error("Lining could not construct a continuous curve within its accuracy and shape limits");
                spans.push_back(*fallback);
            }
            ++group;
            pending.clear();
        }
        timings.insert(QStringLiteral("fitting"), stageTimer.restart());
        if (options.phaseProgress)
            options.phaseProgress(QStringLiteral("traceMerging"));
        for (int round = 0; round < kTraceMergeRounds && !stopped(cancelled); ++round)
            for (int size : kTraceMergeSizes)
                for (int index = 0; index + size <= spans.size() && !stopped(cancelled);) {
                    if (spans[index].group == spans[index + size - 1].group) {
                        QVector<LiningStrokeSegment> combined;
                        for (int offset = 0; offset < size; ++offset)
                            combined += spans[index + offset].segments;
                        if (const auto replacement = fitter.fit(combined, spans[index].group, traceStraight(combined))) {
                            QVector<PenPlacement> beforePlacements;
                            QVector<PenPlacement> afterPlacements;
                            for (int piece = 0; piece < spans.size(); ++piece) {
                                if (spans[piece].placement.shapeId != 0) {
                                    beforePlacements.push_back(spans[piece].placement);
                                    if (piece < index || piece >= index + size)
                                        afterPlacements.push_back(spans[piece].placement);
                                }
                            }
                            if (replacement->placement.shapeId != 0)
                                afterPlacements.push_back(replacement->placement);
                            const auto before = boundary.measure(catalog::subtract(
                                coverageOf(beforePlacements, primitives), region.leeway));
                            const auto after = boundary.measure(catalog::subtract(
                                coverageOf(afterPlacements, primitives), region.leeway));
                            if (after.components > before.components || after.holes != before.holes
                                || traceContourEnergy(boundary, after, traceWidth(stroke))
                                    > traceContourEnergy(boundary, before, traceWidth(stroke)) + traceWidth(stroke) * kTraceMergeEnergySlack
                                || after.maximumExcessTurn > before.maximumExcessTurn + kTraceMergeTurnSlack) {
                                ++index;
                                continue;
                            }
                            spans[index] = *replacement;
                            spans.remove(index + 1, size - 1);
                            merged += size - 1;
                            continue;
                        }
                    }
                    ++index;
                }
        timings.insert(QStringLiteral("merging"), stageTimer.restart());
        if (options.phaseProgress)
            options.phaseProgress(QStringLiteral("traceContourRefinement"));
        auto contourRefinement = refineTraceContours(&spans, primitives, region, ribbon, stroke, capAnchors, options, cancelled);
        timings.insert(QStringLiteral("initialContourRefinement"), stageTimer.restart());
        if (options.phaseProgress)
            options.phaseProgress(QStringLiteral("traceContourRepair"));
        const auto contourRepair = repairTraceContours(&spans, &fitter, primitives, region, boundary,
            ribbon, stroke, capAnchors, traceWidth(stroke), options, cancelled);
        timings.insert(QStringLiteral("contourRepair"), stageTimer.restart());
        if (contourRepair.value(QStringLiteral("accepted")).toInt() > 0) {
            if (options.phaseProgress)
                options.phaseProgress(QStringLiteral("traceContourRefinement"));
            const auto polish = refineTraceContours(&spans, primitives, region, ribbon, stroke, capAnchors, options, cancelled);
            for (const auto &key : {QStringLiteral("trials"), QStringLiteral("accepted"), QStringLiteral("shapeReplacements")})
                contourRefinement.insert(key, contourRefinement.value(key).toInt() + polish.value(key).toInt());
            contourRefinement.insert(QStringLiteral("afterInitialRefinement"), contourRefinement.value(QStringLiteral("after")));
            contourRefinement.insert(QStringLiteral("after"), polish.value(QStringLiteral("after")));
        }
        timings.insert(QStringLiteral("contourRefinement"), stageTimer.restart());
        for (const auto &span : spans) {
            joinProbes += traceJoinProbes(span.segments, capAnchors);
            if (span.placement.shapeId != 0)
                result.fill.placements.push_back(span.placement);
            maximumCurveError = std::max(maximumCurveError, span.curveError);
            maximumCurveErrorWidthFraction = std::max(maximumCurveErrorWidthFraction,
                span.curveError / traceWidth(span.segments));
            maximumTangentError = std::max(maximumTangentError, span.tangentError);
        }
        const auto originalPlacements = result.fill.placements;
        int removed = reducePlacements(&result.fill.placements, region, primitives, 0.0, cancelled);
        if (removed > 0) {
            const auto before = boundary.measure(catalog::subtract(coverageOf(originalPlacements, primitives), region.leeway));
            const auto after = boundary.measure(catalog::subtract(coverageOf(result.fill.placements, primitives), region.leeway));
            if (traceContourEnergy(boundary, after, traceWidth(stroke)) > traceContourEnergy(boundary, before, traceWidth(stroke))
                || after.maximumExcessTurn > before.maximumExcessTurn
                || !traceJoinsWide(coverageOf(result.fill.placements, primitives), joinProbes,
                    traceWidth(stroke) * kTraceCurveErrorWidthFraction, region.leeway)) {
                result.fill.placements = originalPlacements;
                removed = 0;
            }
        }
        if (options.phaseProgress)
            options.phaseProgress(QStringLiteral("traceVerification"));
        const auto coverage = coverageOf(result.fill.placements, primitives);
        const auto missingTrace = catalog::subtract(region.visible,
            catalog::expanded(coverage, traceWidth(stroke) * kTraceCurveErrorWidthFraction));
        const auto visibleRibbon = catalog::subtract(ribbon, region.leeway);
        const auto missingRibbon = catalog::subtract(visibleRibbon, coverage);
        const auto outside = catalog::subtract(coverage, region.permitted);
        const auto observed = catalog::unite(coverage + region.leeway);
        const auto expected = catalog::unite(region.required + region.leeway);
        const auto observedTopology = topologyOf(observed);
        const auto expectedTopology = topologyOf(expected);
        if (!missingTrace.isEmpty() || !outside.isEmpty() || observedTopology.components > expectedTopology.components
            || result.fill.placements.size() > options.shapeBudget || !interiorHoles(coverage, region).isEmpty()
            || !traceJoinsWide(coverage, joinProbes, traceWidth(stroke) * kTraceCurveErrorWidthFraction, region.leeway))
            throw std::runtime_error("Lining could not preserve its continuous trace within the fitting corridor");
        result.fill.targetArea = catalog::area(visibleRibbon);
        result.fill.coveredArea = result.fill.targetArea - catalog::area(missingRibbon);
        result.fill.outsideArea = catalog::area(catalog::subtract(coverage, ribbon));
        result.fill.unfilled = catalog::painterPath(missingRibbon);
        result.diagnostics = fitter.diagnostics();
        result.diagnostics.insert(QStringLiteral("contourRepair"), contourRepair);
        result.diagnostics.insert(QStringLiteral("contourRefinement"), contourRefinement);
        result.diagnostics.insert(QStringLiteral("coverageRatio"), result.fill.targetArea > 0.0
            ? result.fill.coveredArea / result.fill.targetArea : 1.0);
        result.diagnostics.insert(QStringLiteral("traceCoverageRatio"), 1.0);
        result.diagnostics.insert(QStringLiteral("outsideEnvelope"), catalog::area(outside));
        result.diagnostics.insert(QStringLiteral("maximumCurveError"), maximumCurveError);
        result.diagnostics.insert(QStringLiteral("maximumCurveErrorWidthFraction"), maximumCurveErrorWidthFraction);
        result.diagnostics.insert(QStringLiteral("maximumTangentError"), maximumTangentError);
        result.diagnostics.insert(QStringLiteral("mergedSpans"), merged);
        result.diagnostics.insert(QStringLiteral("removedRedundantTracePlacements"), removed);
        result.diagnostics.insert(QStringLiteral("hardSpans"), group);
        double fallbackLength = 0.0;
        for (const auto &span : spans) {
            const auto primitive = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &entry) {
                return entry.shape.shapeId == span.placement.shapeId;
            });
            if (primitive != primitives.cend() && !primitive->stroke)
                fallbackLength += tracePath(span.segments).length();
        }
        result.diagnostics.insert(QStringLiteral("straightFallbackLengthFraction"), fallbackLength / path.centerline.length());
        QJsonObject shapeCounts;
        int strokeCount = 0;
        for (const auto &placement : result.fill.placements) {
            const auto id = QString::number(placement.shapeId);
            shapeCounts.insert(id, shapeCounts.value(id).toInt() + 1);
            const auto primitive = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &entry) {
                return entry.shape.shapeId == placement.shapeId;
            });
            strokeCount += primitive != primitives.cend() && primitive->stroke;
        }
        result.diagnostics.insert(QStringLiteral("shapeCounts"), shapeCounts);
        result.diagnostics.insert(QStringLiteral("strokePlacements"), strokeCount);
        result.diagnostics.insert(QStringLiteral("fallbackPlacements"), result.fill.placements.size() - strokeCount);
        if (options.workProgress)
            options.workProgress(result.fill.placements.size(), options.profileTrialBudget, options.profileTrialBudget);
    } catch (const std::exception &failure) {
        result.fill.error = QString::fromUtf8(failure.what());
        result.fill.placements.clear();
    }
    if (stopped(cancelled)) {
        result.fill = {};
        result.fill.cancelled = true;
    }
    result.diagnostics.insert(QStringLiteral("strategy"), QStringLiteral("defined centerline matching with contour continuity optimization"));
    result.diagnostics.insert(QStringLiteral("traceSupportWidth"), traceWidth(stroke) * kTraceSupportWidthFraction);
    result.diagnostics.insert(QStringLiteral("minimumBodyWidthRatio"), kTraceMinimumBodyWidthRatio);
    result.diagnostics.insert(QStringLiteral("minimumJoinWidthRatio"), kTraceMinimumJoinWidthRatio);
    result.diagnostics.insert(QStringLiteral("straightFallbackDeviationWidthFraction"), kTraceStraightErrorWidthFraction);
    result.diagnostics.insert(QStringLiteral("maximumWidthRatio"), maximumWidthRatio);
    result.diagnostics.insert(QStringLiteral("outsideMarginWidthFraction"), options.liningOutsideMarginWidthFraction);
    QJsonArray anchorWidths;
    for (const auto &point : points)
        anchorWidths.push_back(liningPointWidth(point, width));
    result.diagnostics.insert(QStringLiteral("anchorWidths"), anchorWidths);
    result.diagnostics.insert(QStringLiteral("curveErrorLimit"), maximumWidth * traceProfileAllowanceWidthFraction(options));
    result.diagnostics.insert(QStringLiteral("curveErrorWidthFractionLimit"), traceProfileAllowanceWidthFraction(options));
    result.diagnostics.insert(QStringLiteral("traceAllowanceWidthFraction"), kTraceCurveErrorWidthFraction);
    result.diagnostics.insert(QStringLiteral("tangentErrorLimit"), kTraceHardPointTangentLimit);
    result.diagnostics.insert(QStringLiteral("smoothJoinTangentErrorLimit"), kTraceTangentLimit);
    result.diagnostics.insert(QStringLiteral("elapsedMilliseconds"), timer.elapsed());
    timings.insert(QStringLiteral("verification"), stageTimer.elapsed());
    result.diagnostics.insert(QStringLiteral("stageMilliseconds"), timings);
    if (!primitives.isEmpty() && primitives.front().configuration)
        result.diagnostics.insert(QStringLiteral("shapeConfiguration"), *primitives.front().configuration);

    return result;
}

} // namespace

FillOptions qualityOptions() {
    FillOptions result;
    result.profileTrialBudget = kQualityProfileTrialBudget;
    result.qualityEvaluationBudget = kQualityEvaluationBudget;
    result.qualityTimeBudgetMilliseconds = kQualityTimeBudgetMilliseconds;
    result.maximumThicknessRatio = kQualityMaximumThicknessRatio;
    result.preferredThicknessRatio = kQualityPreferredThicknessRatio;

    return result;
}

QVector<catalog::Primitive> buildCatalog(const ShapeGeometryStore &geometry, QString *error) {
    const auto deployed = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/lining_shapes.json"));
    const auto source = QDir::current().filePath(QStringLiteral("assets/lining_shapes.json"));

    auto result = catalog::buildCatalog(geometry, QFileInfo::exists(deployed) ? deployed : source, error);
    for (const auto &primitive : result)
        if (primitive.centerline.isEmpty()) {
            if (error)
                *error = QStringLiteral("Lining shape %1 requires a centerline definition").arg(primitive.shape.shapeId);
            return {};
        }

    return result;
}

static catalog::FillResult fillTarget(const PenFillRequest &request, const catalog::Polygons &polygons,
                                     const QVector<catalog::Primitive> &primitives,
                                     const FillOptions &options, const std::function<bool()> &cancelled) {
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
    const auto phase = [&](const QString &name) {
        if (options.phaseProgress)
            options.phaseProgress(name);
    };
    timer.start();
    result.fill.shapeLimit = options.shapeBudget;

    try {
        if (options.shapeBudget < 1 || options.profileTrialBudget < 1
            || options.qualityEvaluationBudget < 0 || options.qualityTimeBudgetMilliseconds < 1
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
        phase(QStringLiteral("region"));
        auto authored = polygons.isEmpty() ? catalog::buildRegion(request, cancelled) : polygonRegion(polygons);
        if (authored.required.isEmpty() || authored.area <= 0.0)
            throw std::runtime_error("Lining requires a nonempty fillable region");
        authored.flexibleBoundary = true;
        const double width = meanWidth(authored.visible);
        const double scale = std::clamp(width * kScaleWidthFraction, kMinimumScale, kMaximumScale);
        const double allowance = options.boundaryAllowance;
        auto region = visibleRegion(authored, options.leeway);
        phase(QStringLiteral("envelope"));
        const auto envelope = thicknessEnvelope(authored.visible, allowance, options.maximumThicknessRatio, 0.0, cancelled);
        const double preferredThicknessRatio = std::min(options.preferredThicknessRatio, options.maximumThicknessRatio);
        phase(QStringLiteral("preferredEnvelope"));
        const auto preferredCoverage = catalog::subtract(thicknessEnvelope(authored.visible, allowance,
            preferredThicknessRatio, 0.0, cancelled), region.leeway);
        region.permitted = catalog::unite(envelope + region.leeway);
        region.permittedPath = catalog::painterPath(region.permitted);
        QJsonObject holeStages;
        const auto recordHoles = [&](const QString &stage) {
            if (options.qualityEvaluationBudget > 0)
                holeStages.insert(stage, catalog::area(interiorHoles(coverageOf(result.fill.placements, primitives), region)));
        };
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
        phase(QStringLiteral("wholeSpan"));
        const auto rectangle = square == primitives.cend() ? std::optional<PenPlacement>{}
            : enclosingRectangle(region.required, square->shape, region, envelopeContainment);
        auto wholeSpan = rectangle;
        if (!wholeSpan)
            wholeSpan = fitWholeRegion(region, primitives, maximumMissing, cancelled);
        const auto separate = wholeSpan ? QVector<PenPlacement>{}
            : fitSeparateRegions(region, primitives, maximumMissing, cancelled);
        if (!separate.isEmpty() && separate.size() <= options.shapeBudget) {
            result.fill.placements = separate;
            result.diagnostics.insert(QStringLiteral("wholeComponents"), separate.size());
        } else if (wholeSpan) {
            result.fill.placements = {*wholeSpan};
            result.diagnostics.insert(QStringLiteral("wholeSpan"), true);
        } else {
            phase(QStringLiteral("profileSeed"));
            compact::FillOptions seedOptions;
            seedOptions.shapeBudget = options.shapeBudget;
            seedOptions.profileTrialBudget = options.profileTrialBudget;
            seedOptions.thinRegion = true;
            seedOptions.thinRegionQuality = options.qualityEvaluationBudget > 0;
            if (seedOptions.thinRegionQuality)
                seedOptions.thinPreferredCoverage = preferredCoverage;
            seedOptions.seedOnly = true;
            seedOptions.useGpu = options.useGpu;
            seedOptions.workProgress = options.workProgress;
            seedOptions.boundaryAllowance = std::min(allowance, width * (options.maximumThicknessRatio - 1.0) * 0.5);
            seedOptions.inwardAllowance = std::max(kMinimumAllowance, width * kToleranceWidthFraction);
            seedOptions.observationScale = scale;
            seedOptions.leeway = options.leeway;
            auto seed = profile::seedRegion(region, primitives, seedOptions, cancelled, &candidates, options.phaseProgress);
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
        phase(QStringLiteral("growth"));
        recordHoles(QStringLiteral("seed"));
        expandPlacements(&result.fill.placements, region, primitives, preferredCoverage, preferredThicknessRatio, cancelled);
        recordHoles(QStringLiteral("growth"));
        recordRefinement(QStringLiteral("growth"));
        phase(QStringLiteral("gapRepair"));
        repairCoverage(&result.fill.placements, region, primitives, options.shapeBudget, maximumMissing, cancelled);
        recordHoles(QStringLiteral("initialGapRepair"));
        recordRefinement(QStringLiteral("initialGapRepair"));
        phase(QStringLiteral("reduction"));
        int removed = reducePlacements(&result.fill.placements, region, primitives, maximumMissing, cancelled);
        recordHoles(QStringLiteral("initialReduction"));
        recordRefinement(QStringLiteral("initialReduction"));
        phase(QStringLiteral("nativeReplacement"));
        const int nativeReduction = replaceNativeGroups(&result.fill.placements, region, primitives, candidates, cancelled);
        result.diagnostics.insert(QStringLiteral("nativeGroupShapeReduction"), nativeReduction);
        recordRefinement(QStringLiteral("nativeReplacement"));
        phase(QStringLiteral("merging"));
        mergePlacements(&result.fill.placements, region, primitives, cancelled);
        recordHoles(QStringLiteral("merging"));
        recordRefinement(QStringLiteral("merging"));
        const bool nativeDirect = result.diagnostics.value(QStringLiteral("profileSeed")).toObject()
            .value(QStringLiteral("nativeArcComplete")).toBool();
        result.diagnostics.insert(QStringLiteral("nativeDirectRefinement"), nativeDirect);
        if (options.qualityEvaluationBudget > 0 && result.fill.placements.size() > 1 && !nativeDirect) {
            phase(QStringLiteral("qualityRefinement"));
            QJsonObject quality;
            refitNativeNeighborhoods(&result.fill.placements, region, primitives, preferredCoverage,
                options, cancelled, &quality);
            result.diagnostics.insert(QStringLiteral("qualityRefinement"), quality);
            recordRefinement(QStringLiteral("qualityRefinement"));
            recordHoles(QStringLiteral("qualityRefinement"));
        }
        phase(QStringLiteral("finalRepair"));
        repairCoverage(&result.fill.placements, region, primitives, options.shapeBudget, maximumMissing, cancelled);
        recordHoles(QStringLiteral("finalGapRepair"));
        recordRefinement(QStringLiteral("finalGapRepair"));
        removed += reducePlacements(&result.fill.placements, region, primitives, maximumMissing, cancelled);
        recordHoles(QStringLiteral("finalReduction"));
        result.diagnostics.insert(QStringLiteral("interiorHoleStages"), holeStages);
        recordRefinement(QStringLiteral("finalReduction"));
        phase(QStringLiteral("verification"));
        result.diagnostics.insert(QStringLiteral("removedLowContributionPlacements"), removed);
        const auto coverage = coverageOf(result.fill.placements, primitives);
        const auto visibleCoverage = region.leeway.isEmpty() ? coverage : catalog::subtract(coverage, region.leeway);
        const auto missing = catalog::subtract(region.visible, visibleCoverage);
        const auto outside = catalog::subtract(coverage, region.permitted);
        const compact::BoundaryModel boundary(region.visible, scale);
        const auto boundaryQuality = boundary.measure(visibleCoverage);
        const auto targetQuality = boundary.measure(region.visible);
        const double newInteriorHoleArea = catalog::area(interiorHoles(coverage, region));
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
        result.diagnostics.insert(QStringLiteral("missingCoreArea"), catalog::area(catalog::intersect(missing,
            catalog::interiorSupport(region.visible, width * kProtectedCoreWidthFraction))));
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

    if (!path.valid() || !std::isfinite(width) || width <= 0.0) {
        result.fill.error = path.valid() ? QStringLiteral("Lining width must be positive") : path.error;
        return result;
    }
    result = fillOpenTrace(path, points, width, primitives, options, cancelled);
    result.diagnostics.insert(QStringLiteral("liningWidth"), width);

    return result;
}

} // namespace gui::thin
