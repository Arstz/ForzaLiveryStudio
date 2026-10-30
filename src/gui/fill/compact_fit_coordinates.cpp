#include "compact_fit_coordinates.h"
#include "compact_fit_catalog_internal.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <numbers>

namespace gui::compact {
namespace {

constexpr double kCoordinateClearanceUlps = 4.0;
constexpr int kCoordinateRepairEvaluations = 2000;
constexpr int kCoordinateRepairMilliseconds = 1500;
constexpr double kCoverageToleranceFraction = 0.25;

const PenPrimitive &placementPrimitive(const PenPlacement &placement, const QVector<catalog::Primitive> &primitives) {
    const auto found = std::find_if(primitives.begin(), primitives.end(), [&](const auto &primitive) {
        return primitive.shape.shapeId == placement.shapeId;
    });
    if (found == primitives.end())
        throw std::runtime_error("Coordinate conversion encountered an unknown shape");

    return found->shape;
}

QVector<PenPlacement> restorePlacements(QVector<PenPlacement> *placements,
    const QVector<catalog::Primitive> &primitives, const QTransform &toWorld, const QTransform &toFit,
    double unit, double clearance = 0.0) {
    QVector<PenPlacement> result;
    result.reserve(placements->size());
    for (auto &placement : *placements) {
        auto transform = placement.transform;
        if (clearance > 0.0) {
            const auto bounds = transform.mapRect(placementPrimitive(placement, primitives).bounds);
            const auto center = bounds.center();
            QTransform expansion;
            const double extent = std::max(bounds.width(), bounds.height());

            expansion.translate(center.x(), center.y());
            expansion.scale(1.0 + 2.0 * clearance / extent, 1.0 + 2.0 * clearance / extent);
            expansion.translate(-center.x(), -center.y());
            transform *= expansion;
        }
        auto worldPlacement = placement;
        worldPlacement.transform = catalog::emittedTransform(transform * toWorld);
        worldPlacement.area *= unit * unit;
        result.push_back(worldPlacement);
        placement.transform = worldPlacement.transform * toFit;
    }

    return result;
}

double conversionClearance(const QVector<PenPlacement> &placements,
    const QVector<catalog::Primitive> &primitives, double unit) {
    double maximumCoordinate = unit;
    for (const auto &placement : placements) {
        const auto bounds = placement.transform.mapRect(placementPrimitive(placement, primitives).bounds);
        maximumCoordinate = std::max({maximumCoordinate, std::abs(bounds.left()), std::abs(bounds.right()),
            std::abs(bounds.top()), std::abs(bounds.bottom())});
    }

    return kCoordinateClearanceUlps * std::max(catalog::kVerificationClearance,
        maximumCoordinate * std::numeric_limits<float>::epsilon() / unit);
}

catalog::Polygons mappedPolygons(const catalog::Polygons &polygons, const QTransform &transform) {
    catalog::Polygons result;
    result.reserve(polygons.size());
    for (const auto &polygon : polygons)
        result.push_back(transform.map(polygon));

    return result;
}

PenFillRequest mappedRequest(PenFillRequest request, const QTransform &transform, double unit) {
    for (auto &point : request.points)
        transformPenPoint(point, transform);
    for (auto &loop : request.loops)
        for (auto &point : loop.points)
            transformPenPoint(point, transform);
    request.boundaryTolerance /= unit;

    return request;
}

FillOptions mappedOptions(FillOptions options, const QTransform &transform, double unit) {
    for (auto &placement : options.initialPlacements) {
        placement.transform *= transform;
        placement.area /= unit * unit;
    }
    if (options.replacementCandidates) {
        auto candidates = std::make_shared<QVector<ReusableCandidate>>(*options.replacementCandidates);
        for (auto &candidate : *candidates) {
            candidate.placement.transform *= transform;
            candidate.placement.area /= unit * unit;
            candidate.polygons = mappedPolygons(candidate.polygons, transform);
            candidate.bounds = transform.mapRect(candidate.bounds);
        }
        options.replacementCandidates = std::move(candidates);
    }
    options.leeway = mappedPolygons(options.leeway, transform);
    options.thinPreferredCoverage = mappedPolygons(options.thinPreferredCoverage, transform);
    options.boundaryAllowance /= unit;
    options.inwardAllowance /= unit;
    options.observationScale = kDefaultObservationScale;

    return options;
}

PenFillRequest canonicalRequest(PenFillRequest request) {
    const auto rotate = [](QVector<PenPoint> &points) {
        if (points.isEmpty())
            return;
        const auto first = std::min_element(points.begin(), points.end(), [](const auto &left, const auto &right) {
            return left.position.x() == right.position.x() ? left.position.y() < right.position.y()
                : left.position.x() < right.position.x();
        });
        std::rotate(points.begin(), first, points.end());
    };
    rotate(request.points);
    for (auto &loop : request.loops)
        rotate(loop.points);

    return request;
}

QPointF geometryOrigin(const PenFillRequest &request, double unit) {
    QRectF bounds;
    if (request.loops.isEmpty()) {
        bounds = penPath(request.points).boundingRect();
    } else {
        for (const auto &loop : request.loops)
            if (loop.kind == PenLoopKind::Outer)
                bounds = bounds.united(penPath(loop.points).boundingRect());
    }
    auto center = bounds.center();
    constexpr double kOriginSnap = 1e-9;
    if (std::abs(center.x()) <= unit * kOriginSnap)
        center.setX(0);
    if (std::abs(center.y()) <= unit * kOriginSnap)
        center.setY(0);

    return center;
}

int measurementDimension(const QString &key) {
    static const QSet<QString> lengths{
        QStringLiteral("boundaryAllowance"), QStringLiteral("selectedBoundaryAllowance"),
        QStringLiteral("requestedBoundaryAllowance"), QStringLiteral("inwardAllowance"),
        QStringLiteral("fittingInwardAllowance"), QStringLiteral("observationScale"),
        QStringLiteral("cornerDistanceLimit"), QStringLiteral("maximumBoundaryDistance"),
        QStringLiteral("coordinateClearance"),
        QStringLiteral("maximumCornerDistance"), QStringLiteral("cellSize"), QStringLiteral("searchScale"),
        QStringLiteral("span"), QStringLiteral("energy"), QStringLiteral("tangentEnergy"),
        QStringLiteral("turnEnergy"), QStringLiteral("boundaryEnergy"), QStringLiteral("boundaryEnergyLimit")};
    static const QSet<QString> areas{
        QStringLiteral("area"), QStringLiteral("missing"), QStringLiteral("spill"), QStringLiteral("deepMissing"),
        QStringLiteral("missingArea"), QStringLiteral("spillArea"), QStringLiteral("deepMissingArea"),
        QStringLiteral("leewayArea"), QStringLiteral("requiredTargetArea"), QStringLiteral("visibleTargetArea"),
        QStringLiteral("missingBeyondInward"), QStringLiteral("repairResidualArea"),
        QStringLiteral("observedHoleDefectArea"), QStringLiteral("outsideEnvelope"),
        QStringLiteral("areaError"), QStringLiteral("areaErrorLimit"), QStringLiteral("missingInteriorArea"),
        QStringLiteral("initialMissingArea"), QStringLiteral("initialDeepMissingArea"),
        QStringLiteral("targetArea"), QStringLiteral("coveredArea"), QStringLiteral("outsideArea"),
        QStringLiteral("residualArea"), QStringLiteral("cornerEnergy")};

    return lengths.contains(key) ? 1 : areas.contains(key) ? 2 : 0;
}

QJsonValue restoredMeasurements(const QJsonValue &value, double unit) {
    if (value.isArray()) {
        QJsonArray result;
        for (const auto &entry : value.toArray())
            result.push_back(restoredMeasurements(entry, unit));

        return result;
    }
    if (!value.isObject())
        return value;
    auto result = value.toObject();
    for (auto iterator = result.begin(); iterator != result.end(); ++iterator) {
        if (iterator.key() == QStringLiteral("shapeConfiguration"))
            continue;
        const int dimension = measurementDimension(iterator.key());
        if (iterator.value().isDouble() && dimension > 0)
            iterator.value() = iterator.value().toDouble() * (dimension == 1 ? unit : unit * unit);
        else
            iterator.value() = restoredMeasurements(iterator.value(), unit);
    }

    return result;
}

} // namespace

double scaleCoverageToRegion(PenFillRequest &request, FillOptions &options) {
    const auto loops = request.loops.isEmpty() ? QVector<PenLoop>{{request.points, PenLoopKind::Outer}} : request.loops;
    double area = 0;
    for (const auto &loop : loops) {
        const auto origin = loop.points.isEmpty() ? QPointF{} : loop.points.front().position;
        double integral = 0;
        for (auto segment : penSegments(loop.points)) {
            segment.start -= origin;
            segment.control -= origin;
            segment.control2 -= origin;
            segment.end -= origin;
            integral += segment.signedArea();
        }
        area += std::abs(integral) * (loop.kind == PenLoopKind::Outer ? 1 : -1);
    }
    const double scale = std::sqrt(4 * area / std::numbers::pi)
        / (kCoverageReferenceDiameter * options.observationScale);
    if (!std::isfinite(scale) || scale <= 0)
        throw std::runtime_error("Coverage scaling requires a positive finite region area and observation scale");
    options.boundaryAllowance *= scale;
    options.inwardAllowance *= scale;
    request.boundaryTolerance = std::min(request.boundaryTolerance, options.inwardAllowance * kCoverageToleranceFraction);

    return scale;
}

catalog::FillResult fitInObservationUnits(const PenFillRequest &request,
    const QVector<catalog::Primitive> &primitives, const FillOptions &options,
    const std::function<bool()> &cancelled, const std::function<void(int, double, double)> &progress,
    const CoordinateSolver &solver) {
    const auto canonical = canonicalRequest(request);
    const auto &points = canonical.loops.isEmpty() ? canonical.points : canonical.loops.front().points;
    const double unit = options.observationScale;
    const QPointF origin = geometryOrigin(canonical, unit);
    const double areaUnit = unit * unit;

    if (points.isEmpty() || !std::isfinite(unit) || unit <= 0.0
        || !std::isfinite(origin.x()) || !std::isfinite(origin.y())
        || (unit == kDefaultObservationScale && origin.isNull()))
        return solver(canonical, options, progress);
    const QTransform toWorld(unit, 0, 0, unit, origin.x(), origin.y());
    const QTransform toFit = toWorld.inverted();
    const auto workingRequest = mappedRequest(canonical, toFit, unit);
    const auto workingOptions = mappedOptions(options, toFit, unit);
    const std::function<void(int, double, double)> workingProgress = progress
        ? std::function<void(int, double, double)>([&](int count, double missing, double spill) {
            progress(count, missing * areaUnit, spill * areaUnit);
        }) : std::function<void(int, double, double)>();
    auto result = solver(workingRequest, workingOptions, workingProgress);
    const auto fittedPlacements = result.fill.placements;
    const bool searchVerified = result.fill.error.isEmpty()
        && result.diagnostics.value(QStringLiteral("approximationVerified")).toBool();
    auto worldPlacements = restorePlacements(&result.fill.placements, primitives, toWorld, toFit, unit);
    result.fill.placements = worldPlacements;
    result.fill.unfilled = toWorld.map(result.fill.unfilled);
    result.fill.targetArea *= areaUnit;
    result.fill.coveredArea *= areaUnit;
    result.fill.outsideArea *= areaUnit;
    result.diagnostics = restoredMeasurements(result.diagnostics, unit).toObject();
    result.diagnostics.insert(QStringLiteral("coordinateFrame"), QJsonObject{
        {QStringLiteral("origin"), QJsonArray{origin.x(), origin.y()}},
        {QStringLiteral("worldUnitsPerObservationUnit"), unit}});
    if (searchVerified && !result.fill.cancelled && !(cancelled && cancelled())) {
        try {
            bool verified = verifyEmittedFill(request, primitives, options, &result);
            if (!verified) {
                const double clearance = conversionClearance(worldPlacements, primitives, unit);
                for (double factor : {1.0, kCoordinateClearanceUlps}) {
                    auto placements = fittedPlacements;
                    result.fill.placements = restorePlacements(&placements, primitives, toWorld, toFit,
                        unit, clearance * factor);
                    verified = verifyEmittedFill(request, primitives, options, &result);
                    result.diagnostics.insert(QStringLiteral("coordinateClearance"), clearance * factor * unit);
                    if (verified)
                        break;
                }
            }
            const int remainingWork = std::max(0, options.evaluationBudget
                - result.diagnostics.value(QStringLiteral("evaluations")).toInt());
            if (!verified && remainingWork > 0 && !(cancelled && cancelled())) {
                auto repair = options;
                repair.initialPlacements = worldPlacements;
                repair.shapeBudget = std::min(options.shapeBudget, static_cast<int>(worldPlacements.size()));
                repair.evaluationBudget = std::min(remainingWork, kCoordinateRepairEvaluations);
                repair.timeBudgetMilliseconds = options.timeBudgetMilliseconds > 0
                    ? std::min(options.timeBudgetMilliseconds, kCoordinateRepairMilliseconds) : kCoordinateRepairMilliseconds;
                repair.adaptiveBudget = false;
                repair.retainFailedFill = true;
                repair.workProgress = {};
                auto corrected = solver(request, repair, {});
                result.diagnostics.insert(QStringLiteral("evaluations"),
                    result.diagnostics.value(QStringLiteral("evaluations")).toInt()
                        + corrected.diagnostics.value(QStringLiteral("evaluations")).toInt());
                result.diagnostics.insert(QStringLiteral("coordinateRepair"), corrected.diagnostics);
                if (!corrected.fill.cancelled && corrected.fill.error.isEmpty()
                    && verifyEmittedFill(request, primitives, options, &corrected)) {
                    result.fill = std::move(corrected.fill);
                    verified = true;
                }
            }
            if (!verified) {
                result.fill.error = QStringLiteral("Compact Fit output failed verification in original coordinates");
                result.diagnostics.insert(QStringLiteral("coordinateVerificationFailed"), true);
                if (!options.retainFailedFill)
                    result.fill.placements.clear();
            } else {
                verifyEmittedFill(request, primitives, options, &result);
            }
        } catch (const std::exception &error) {
            result.fill.error = QString::fromUtf8(error.what());
            result.diagnostics.insert(QStringLiteral("coordinateVerificationFailed"), true);
            result.diagnostics.insert(QStringLiteral("approximationVerified"), false);
            if (!options.retainFailedFill)
                result.fill.placements.clear();
        }
    }
    result.diagnostics.insert(QStringLiteral("retainedAfterError"),
        !result.fill.error.isEmpty() && !result.fill.placements.isEmpty());
    if (result.fill.cancelled || (cancelled && cancelled())) {
        result.fill.cancelled = true;
        result.fill.placements.clear();
    }

    return result;
}

} // namespace gui::compact
