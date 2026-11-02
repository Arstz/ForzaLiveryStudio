#include "mesh_fill.h"

#include "compact_fit_catalog_internal.h"
#include "compact_fit_coordinates.h"
#include "mesh_edit.h"
#include "profile_fit.h"

#include <QElapsedTimer>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace gui {
namespace {

using catalog::Polygons;
using EdgeKey = std::pair<int, int>;

constexpr double kBoundaryPixelFraction = 0.25;
constexpr double kFloatRoundingFactor = 8.0;
constexpr int kMaximumSubdivisionDepth = 24;
constexpr int kSquareShapeId = 101;

struct FillFace {
    PenFillRequest request;
    Polygons target;
    double score = 0.0;
    int source = -1;
};

struct FillEdge {
    QPolygonF points;
    QVector<int> owners;
    double cost = 0.0;
};

bool stopped(const std::function<bool()> &stopRequested) {

    return stopRequested && stopRequested();
}

EdgeKey edgeKey(int first, int last) {

    return std::minmax(first, last);
}

void sampleCurve(const PenBoundarySegment &curve, double tolerance, QPolygonF &points, int depth = 0) {
    if (curve.flatness() > tolerance && depth < kMaximumSubdivisionDepth) {
        const auto halves = curve.split();
        sampleCurve(halves.first, tolerance, points, depth + 1);
        sampleCurve(halves.second, tolerance, points, depth + 1);
    } else points.push_back(curve.end);
}

QPolygonF sampledLoop(const QVector<fls::scene::MeshPoint> &points, std::map<EdgeKey, FillEdge> &edges, int owner, bool outer) {
    QPolygonF polygon;

    for (int index = 0; index < points.size(); ++index) {
        const int first = points[index].vertex;
        const int last = points[(index + 1) % points.size()].vertex;
        auto &edge = edges.at(edgeKey(first, last));
        if (!edge.owners.contains(owner)) edge.owners.push_back(owner);
        for (int sample = 0; sample + 1 < edge.points.size(); ++sample) {
            const int position = first < last ? sample : static_cast<int>(edge.points.size()) - 1 - sample;
            const auto &point = edge.points[position];
            if (polygon.isEmpty() || polygon.back() != point) polygon.push_back(point);
        }
    }
    if ((catalog::signedArea(polygon) > 0.0) != outer) std::reverse(polygon.begin(), polygon.end());

    return polygon;
}

catalog::Region fillRegion(const FillFace &face, const Polygons &leeway, const Polygons &protectedHoles) {
    auto region = catalog::leewayAdjustedRegion(catalog::buildRegion(face.request, {}), leeway,
        compact::kDefaultBoundaryAllowance);

    region.permitted = catalog::subtract(region.permitted, protectedHoles);
    region.permittedPath = catalog::painterPath(region.permitted);

    return region;
}

Polygons assignedLeeway(int face, const QVector<FillFace> &faces, const QVector<QVector<int>> &neighbors,
                        const QVector<bool> &filled, const Polygons &above) {
    QVector<int> pending{face};
    QSet<int> reached{face};
    Polygons authorized;

    for (int position = 0; position < pending.size(); ++position) {
        for (const int neighbor : neighbors[pending[position]]) {
            if (!filled[neighbor] || reached.contains(neighbor)) continue;
            reached.insert(neighbor);
            pending.push_back(neighbor);
        }
    }
    for (int index = 0; index < faces.size(); ++index) if (index != face && reached.contains(index)) authorized += faces[index].target;

    return catalog::intersect(catalog::unite(authorized), above);
}

const PenPrimitive *primitiveFor(int id, const QVector<catalog::Primitive> &primitives) {
    const auto found = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &primitive) { return primitive.shape.shapeId == id; });

    return found != primitives.cend() ? &found->shape : nullptr;
}

std::optional<PenPlacement> squareCover(const catalog::Region &region, const QVector<catalog::Primitive> &primitives) {
    const auto *square = primitiveFor(kSquareShapeId, primitives);
    QPolygonF points;

    if (square == nullptr || square->contours.size() != 1) return {};
    const auto source = catalog::convexHull(square->contours.front());
    for (const auto &polygon : region.spillFree) points += polygon;
    const auto target = catalog::convexHull(points);
    if (source.size() != 4 || target.size() != 4) return {};
    const auto transform = catalog::emittedTransform(catalog::affineFromAnchors(
        {source[0], source[1], source[3]}, {target[0], target[1], target[3]}));
    const auto support = catalog::mapped(*square, transform);
    if (support.isEmpty() || !catalog::subtract(support, region.permitted).isEmpty()
        || !catalog::subtract(region.required, catalog::expanded(support, catalog::kVerificationClearance)).isEmpty()) return {};

    return PenPlacement{kSquareShapeId, transform, catalog::area(support)};
}

} // namespace

MeshFillResult fillMesh(const MeshFillRequest &request, const std::function<bool()> &stopRequested,
                        const std::function<void(const MeshFillProgress &)> &progress) {
    MeshFillResult result;
    QVector<FillFace> faces;
    std::map<EdgeKey, FillEdge> edges;
    QJsonArray order;
    QJsonArray details;
    int failedFaces = 0;
    QElapsedTimer timer;
    const QRectF bounds = QPolygonF(request.mesh.vertices).boundingRect();
    const double extent = std::max(bounds.width(), bounds.height());
    const double observationScale = compact::kDefaultObservationScale;
    const double flatness = observationScale * kBoundaryPixelFraction;
    const double coordinate = std::max({std::abs(bounds.left()), std::abs(bounds.right()), std::abs(bounds.top()), std::abs(bounds.bottom()), extent});
    const double clearance = std::max(catalog::kVerificationClearance, coordinate * std::numeric_limits<float>::epsilon() * kFloatRoundingFactor);
    const auto report = [&] {
        if (progress) progress({result.completedFaces, result.totalFaces, result.shapeCount});
    };

    timer.start();
    try {
        if (!request.mesh.valid() || request.primitives.isEmpty() || request.shapeBudget < 1) throw std::runtime_error("Mesh fill needs a valid mesh, shape catalog and shape budget");
        for (const auto &edge : meshEditEdges(request.mesh)) {
            FillEdge sampled;
            sampled.points.push_back(edge.curve.start);
            sampleCurve(edge.curve, flatness, sampled.points);
            for (auto &point : sampled.points) point = QPointF(std::round(point.x() * catalog::kCoordinateScale), std::round(point.y() * catalog::kCoordinateScale)) / catalog::kCoordinateScale;
            sampled.cost = 1.0 + std::sqrt(edge.curve.flatness() / flatness);
            edges.emplace(edgeKey(edge.first, edge.last), std::move(sampled));
        }
        Polygons combined;
        for (int index = 0; index < request.mesh.contours.size() && !stopped(stopRequested); ++index) {
            const auto &source = request.mesh.contours[index];
            if (!source.visible) continue;
            const auto data = request.mesh.contourData(index);
            if (data.pathKind != fls::scene::ContourPathKind::Pen || !data.closed || !data.cutoutClosed || data.fillMask
                || source.opacity < 1.0 || fls::scene::contourFillColor(data).alpha() != 255) throw std::runtime_error("Mesh fill requires closed opaque Pen faces without masks");
            FillFace face;
            face.source = index;
            face.request.loops.push_back({data.points, PenLoopKind::Outer});
            for (const auto &hole : data.cutouts) face.request.loops.push_back({hole, PenLoopKind::Cutout});
            if (!buildPenContour(face.request.loops, flatness).valid()) throw std::runtime_error("Mesh fill requires valid face boundaries and holes");
            face.target.push_back(sampledLoop(source.points, edges, faces.size(), true));
            for (const auto &hole : source.cutouts) face.target.push_back(sampledLoop(hole, edges, faces.size(), false));
            face.target = catalog::unite(face.target);
            if (face.target.isEmpty() || catalog::area(catalog::intersect(face.target, combined)) > clearance * clearance) throw std::runtime_error("Mesh faces must have disjoint material before filling");
            combined = catalog::unite(combined + face.target);
            faces.push_back(std::move(face));
        }
        result.totalFaces = faces.size();
        Polygons protectedHoles;
        for (const auto &face : faces) for (auto polygon : face.target) {
            if (catalog::signedArea(polygon) >= 0.0) continue;
            std::reverse(polygon.begin(), polygon.end());
            protectedHoles.push_back(std::move(polygon));
        }
        protectedHoles = catalog::subtract(catalog::unite(protectedHoles), combined);
        QVector<QVector<int>> neighbors(faces.size());
        QVector<bool> filled(faces.size(), false);
        for (const auto &[key, edge] : edges) {
            for (const int owner : edge.owners) faces[owner].score += edge.cost;
            if (edge.owners.size() == 2) {
                neighbors[edge.owners[0]].push_back(edge.owners[1]);
                neighbors[edge.owners[1]].push_back(edge.owners[0]);
            }
        }
        Polygons above;
        report();
        while (result.completedFaces < faces.size() && !stopped(stopRequested)) {
            int selected = -1;
            for (int index = 0; index < faces.size(); ++index) {
                if (!filled[index] && (selected < 0 || faces[index].score < faces[selected].score)) selected = index;
            }
            const auto &face = faces[selected];
            const auto leeway = assignedLeeway(selected, faces, neighbors, filled, above);
            const auto region = fillRegion(face, leeway, protectedHoles);
            const auto &source = request.mesh.contours[face.source];
            result.layers.push_back({source.name, fls::scene::contourFillColor(source.style), {}, face.source});
            auto &layer = result.layers.back();
            Polygons coverage;
            QJsonObject fitting;
            compact::FillOptions options;
            options.shapeBudget = request.shapeBudget - result.shapeCount;
            options.profileTrialBudget = request.profileTrialBudget;
            options.observationScale = observationScale;
            options.leeway = leeway;
            options.permitted = region.permitted;
            options.useGpu = false;
            options.timeBudgetMilliseconds = compact::kNoTimeBudget;
            options.retainFailedFill = true;
            const int previousCount = result.shapeCount;
            bool verifiedCheckpoint = false;
            const auto accept = [&](PenPlacement placement) {
                if (verifiedCheckpoint || result.shapeCount >= request.shapeBudget) return;
                const auto *primitive = primitiveFor(placement.shapeId, request.primitives);
                if (primitive == nullptr) return;
                placement.transform = catalog::emittedTransform(placement.transform);
                const auto support = catalog::mapped(*primitive, placement.transform);
                if (support.isEmpty() || !catalog::subtract(support, region.permitted).isEmpty()) return;
                if (std::any_of(layer.placements.cbegin(), layer.placements.cend(), [&](const auto &existing) {
                    return existing.shapeId == placement.shapeId && existing.transform == placement.transform;
                })) return;
                layer.placements.push_back(placement);
                coverage = catalog::unite(coverage + support);
                ++result.shapeCount;
                report();
            };
            const auto checkpoint = [&](const QVector<PenPlacement> &placements, bool verified) {
                if (placements.isEmpty() || (verifiedCheckpoint && (!verified || placements.size() > layer.placements.size()))) return;
                QVector<PenPlacement> accepted;
                Polygons painted;
                for (auto placement : placements) {
                    const auto *primitive = primitiveFor(placement.shapeId, request.primitives);
                    if (primitive == nullptr || accepted.size() >= options.shapeBudget) return;
                    placement.transform = catalog::emittedTransform(placement.transform);
                    const auto support = catalog::mapped(*primitive, placement.transform);
                    if (support.isEmpty() || !catalog::subtract(support, region.permitted).isEmpty()) return;
                    accepted.push_back(placement);
                    painted += support;
                }
                layer.placements = std::move(accepted);
                coverage = catalog::unite(painted);
                verifiedCheckpoint = verified;
                result.shapeCount = previousCount + layer.placements.size();
                report();
            };
            order.append(face.source);
            if (const auto square = squareCover(region, request.primitives)) {
                accept(*square);
                fitting.insert(QStringLiteral("strategy"), QStringLiteral("single_square"));
            } else {
                options.placementAccepted = accept;
                options.placementCheckpoint = checkpoint;
                const auto fitted = profile::fillRegion(face.request, request.primitives, options, stopRequested);
                fitting = fitted.diagnostics;
                fitting.insert(QStringLiteral("error"), fitted.fill.error);
                if (!fitted.fill.placements.isEmpty()) checkpoint(fitted.fill.placements, fitted.fill.error.isEmpty());
            }
            const auto visible = catalog::subtract(coverage, catalog::expanded(above, catalog::kVerificationClearance));
            const double spill = catalog::area(catalog::subtract(visible, catalog::expanded(face.target, clearance * 2.0)));
            const double missing = catalog::area(catalog::subtract(face.target, catalog::expanded(coverage, clearance * 2.0)));
            catalog::FillResult verification;
            verification.fill.placements = layer.placements;
            layer.complete = !layer.placements.isEmpty()
                && compact::verifyEmittedFill(face.request, request.primitives, options, &verification);
            fitting.insert(QStringLiteral("meshVerification"), verification.diagnostics);
            details.append(QJsonObject{{QStringLiteral("face"), face.source}, {QStringLiteral("greedyCost"), face.score},
                {QStringLiteral("leewayArea"), catalog::area(leeway)}, {QStringLiteral("shapes"), layer.placements.size()},
                {QStringLiteral("fitting"), fitting},
                {QStringLiteral("complete"), layer.complete}, {QStringLiteral("missingArea"), missing}, {QStringLiteral("visibleSpillArea"), spill}});
            if (stopped(stopRequested) && !layer.complete) break;
            if (!layer.complete) {
                if (result.shapeCount >= request.shapeBudget) throw std::runtime_error("Mesh fill reached its shape budget");
                if (layer.placements.isEmpty()) throw std::runtime_error(fitting.value(QStringLiteral("error")).toString(
                    QStringLiteral("Mesh fill did not produce native shapes for a face")).toStdString());
                ++failedFaces;
            }
            filled[selected] = true;
            above = catalog::unite(above + coverage);
            ++result.completedFaces;
            report();
            if (stopped(stopRequested)) break;
            for (const auto &[key, edge] : edges) if (edge.owners.contains(selected)) {
                for (const int owner : edge.owners) if (!filled[owner]) faces[owner].score = std::max(0.0, faces[owner].score - edge.cost);
            }
        }
        if (failedFaces > 0) result.error = QStringLiteral("Compact Fill quality checks failed for %1 faces; available shapes were retained").arg(failedFaces);
    } catch (const std::exception &error) {
        result.error = QString::fromUtf8(error.what());
    }
    result.stopped = stopped(stopRequested);
    result.diagnostics = {{QStringLiteral("version"), 2}, {QStringLiteral("strategy"), QStringLiteral("greedy_foreground_first")},
        {QStringLiteral("foregroundOrder"), order}, {QStringLiteral("faces"), details}, {QStringLiteral("shapes"), result.shapeCount},
        {QStringLiteral("completedFaces"), result.completedFaces}, {QStringLiteral("totalFaces"), result.totalFaces},
        {QStringLiteral("failedFaces"), failedFaces},
        {QStringLiteral("stopped"), result.stopped}, {QStringLiteral("error"), result.error}, {QStringLiteral("elapsedMs"), timer.elapsed()}};
    report();

    return result;
}

} // namespace gui
