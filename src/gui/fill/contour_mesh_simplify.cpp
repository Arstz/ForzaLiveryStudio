#include "contour_mesh_simplify.h"

#include "cubic_contour.h"
#include "region_fill.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace gui {
namespace {

using fls::scene::MeshData;
using fls::scene::MeshPoint;

constexpr double kReferenceLength = 1024.0;
constexpr double kCurveSamplingFraction = 0.25;
constexpr double kPolylineToleranceFraction = 0.5;
constexpr int kMaximumCurveDepth = 24;
constexpr int kBudgetTrials = 4;
constexpr double kMaximumBucketToleranceFactor = 2.0;
constexpr double kBucketStrokeFraction = 0.3;
constexpr int kMinimumLoopAnchors = 3;
constexpr int kMaximumFitRefinements = 4;

struct SharedEdge {
    QSet<int> owners;
    PenBoundarySegment curve;
    int first = -1;
    int last = -1;
    int uses = 0;
    bool compatible = true;
};

struct SharedChain {
    QVector<int> vertices;
    QVector<PenBoundarySegment> curves;
    QVector<PenPoint> fitted;
    QSet<int> owners;
    int level = 0;
    int refinements = 0;
    double toleranceLimit = std::numeric_limits<double>::infinity();
};

quint64 edgeKey(int first, int last) {
    if (first > last) std::swap(first, last);

    return (quint64(quint32(first)) << 32) | quint32(last);
}

double length(const QPointF &point) {

    return std::hypot(point.x(), point.y());
}

QPair<double, double> loopMeasure(const MeshData &mesh, const QVector<MeshPoint> &points) {
    double area = 0.0;
    double perimeter = 0.0;

    for (int point = 0; point < points.size(); ++point) {
        const auto &first = points[point];
        const auto &last = points[(point + 1) % points.size()];
        const PenBoundarySegment curve{mesh.vertices[first.vertex], mesh.vertices[first.vertex] + first.outgoing,
            mesh.vertices[last.vertex], true, mesh.vertices[last.vertex] + last.incoming};
        area += curve.signedArea();
        perimeter += curve.controlLength();
    }

    return {area, perimeter};
}

bool preservesWinding(const MeshData &original, const MeshData &candidate, QSet<int> &unsafe) {
    for (int contour = 0; contour < original.contours.size(); ++contour) {
        const auto &before = original.contours[contour];
        const auto &after = candidate.contours[contour];
        const auto check = [&](const QVector<MeshPoint> &source, const QVector<MeshPoint> &result) {
            return result.size() >= 3 && loopMeasure(original, source).first * loopMeasure(candidate, result).first > 0.0;
        };
        if (!check(before.points, after.points)) unsafe.insert(contour);
        for (int hole = 0; hole < before.cutouts.size(); ++hole) {
            if (!check(before.cutouts[hole], after.cutouts[hole])) unsafe.insert(contour);
        }
    }

    return unsafe.isEmpty();
}

void sampleCurve(const PenBoundarySegment &curve, double tolerance, QPolygonF &points, int depth = 0) {
    if (curve.flatness() > tolerance && depth < kMaximumCurveDepth) {
        const auto halves = curve.split();
        sampleCurve(halves.first, tolerance, points, depth + 1);
        sampleCurve(halves.second, tolerance, points, depth + 1);
    } else points.push_back(curve.end);
}

double distanceSquared(const QPointF &point, const QPolygonF &path) {
    double minimum = std::numeric_limits<double>::infinity();

    for (int index = 1; index < path.size(); ++index) {
        const QPointF direction = path[index] - path[index - 1];
        const double squared = QPointF::dotProduct(direction, direction);
        const double parameter = squared > 0.0 ? std::clamp(QPointF::dotProduct(point - path[index - 1], direction) / squared, 0.0, 1.0) : 0.0;
        const QPointF delta = point - path[index - 1] - direction * parameter;
        minimum = std::min(minimum, QPointF::dotProduct(delta, delta));
    }

    return minimum;
}

bool withinCorridor(const QPolygonF &source, const QVector<PenPoint> &fitted, double tolerance) {
    if (fitted.size() < 2) return false;
    QPolygonF sampled{fitted.front().position};
    const double limit = tolerance * (1.0 - 2.0 * kCurveSamplingFraction);

    for (const auto &curve : penSegments(fitted, false)) sampleCurve(curve, tolerance * kCurveSamplingFraction, sampled);
    for (const auto &point : source) if (distanceSquared(point, sampled) > limit * limit) return false;
    for (const auto &point : sampled) if (distanceSquared(point, source) > limit * limit) return false;

    return true;
}

void fitChain(SharedChain &chain, const MeshData &mesh, double tolerance, double scale) {
    QPolygonF source;
    double perimeter = 0.0;

    chain.fitted.clear();
    if (chain.level >= 2 || chain.vertices.size() < 3) return;
    tolerance = std::ldexp(tolerance, -chain.refinements);
    source.push_back(mesh.vertices[chain.vertices.front()]);
    for (const auto &curve : chain.curves) sampleCurve(curve, tolerance * kCurveSamplingFraction, source);
    for (int point = 1; point < source.size(); ++point) perimeter += length(source[point] - source[point - 1]);
    if (perimeter <= 0.0) return;
    const QPointF origin = source.front();
    for (auto &point : source) point = (point - origin) * scale;
    const double allowance = tolerance * scale;
    if (chain.level == 0) {
        CubicFitOptions options;
        options.tolerance = allowance * kPolylineToleranceFraction;
        options.adaptToRasterNoise = false;
        options.relaxCornerTangents = true;
        options.cornerScale = std::max(options.cornerScale, 4.0 * options.tolerance);
        chain.fitted = fitCubicPolyline(source, options);
        if (!withinCorridor(source, chain.fitted, allowance)) chain.level = 1;
    }
    if (chain.level == 1) {
        const auto reduced = simplifyOpenPolyline(source, allowance * kPolylineToleranceFraction);
        chain.fitted.clear();
        for (const auto &point : reduced) chain.fitted.push_back({point, PenPointKind::Hard, {}, {}, true});
        for (int index = 1; index < chain.fitted.size(); ++index) {
            const QPointF handle = (chain.fitted[index].position - chain.fitted[index - 1].position) / 3.0;
            chain.fitted[index - 1].outgoing = handle;
            chain.fitted[index].incoming = -handle;
        }
    }
    if (chain.fitted.size() >= chain.vertices.size()) {
        chain.fitted.clear();
        return;
    }
    for (auto &point : chain.fitted) {
        point.position = origin + point.position / scale;
        point.incoming /= scale;
        point.outgoing /= scale;
    }
    if (!chain.fitted.isEmpty()) {
        chain.fitted.front().position = mesh.vertices[chain.vertices.front()];
        chain.fitted.back().position = mesh.vertices[chain.vertices.back()];
    }
}

void retainLoopAnchors(const MeshData &mesh, const QVector<MeshPoint> &points, QSet<int> &fixed) {
    QVector<int> retained;

    for (const auto &point : points) if (fixed.contains(point.vertex)) retained.push_back(point.vertex);
    if (retained.isEmpty() && !points.isEmpty()) {
        retained.push_back(points.front().vertex);
        fixed.insert(points.front().vertex);
    }
    while (retained.size() < kMinimumLoopAnchors) {
        int selected = -1;
        double maximum = -1.0;
        for (const auto &point : points) {
            if (fixed.contains(point.vertex)) continue;
            const QPointF delta = mesh.vertices[point.vertex] - mesh.vertices[retained.front()];
            const QPointF chord = mesh.vertices[retained.back()] - mesh.vertices[retained.front()];
            const double distance = retained.size() == 1 ? QPointF::dotProduct(delta, delta)
                : std::abs(delta.x() * chord.y() - delta.y() * chord.x());
            if (distance > maximum) {
                maximum = distance;
                selected = point.vertex;
            }
        }
        if (selected < 0) break;
        fixed.insert(selected);
        retained.push_back(selected);
    }
}

QVector<SharedChain> sharedChains(const MeshData &mesh, const QSet<int> &anchors, double tolerance) {
    std::map<quint64, SharedEdge> edges;
    QHash<int, QVector<int>> adjacency;
    QSet<int> fixed = anchors;
    QSet<quint64> visited;
    QVector<SharedChain> chains;
    const double roundoff = std::pow(std::numeric_limits<double>::epsilon(), 2.0 / 3.0);
    const auto collect = [&](const QVector<MeshPoint> &points, int owner) {
        for (int index = 0; index < points.size(); ++index) {
            const auto &first = points[index];
            const auto &last = points[(index + 1) % points.size()];
            PenBoundarySegment curve{mesh.vertices[first.vertex], mesh.vertices[first.vertex] + first.outgoing,
                mesh.vertices[last.vertex], true, mesh.vertices[last.vertex] + last.incoming};
            if (first.vertex > last.vertex) {
                std::swap(curve.start, curve.end);
                std::swap(curve.control, curve.control2);
            }
            const double precision = roundoff * std::max(curve.controlLength(), tolerance);
            auto &edge = edges[edgeKey(first.vertex, last.vertex)];
            edge.first = std::min(first.vertex, last.vertex);
            edge.last = std::max(first.vertex, last.vertex);
            edge.owners.insert(owner);
            if (edge.uses == 0) edge.curve = curve;
            else edge.compatible &= length(edge.curve.control - curve.control) <= precision
                && length(edge.curve.control2 - curve.control2) <= precision;
            ++edge.uses;
            edge.compatible &= first.explicitHandles && last.explicitHandles;
        }
    };

    for (int owner = 0; owner < mesh.contours.size(); ++owner) {
        collect(mesh.contours[owner].points, owner);
        for (const auto &hole : mesh.contours[owner].cutouts) collect(hole, owner);
    }
    for (const auto &[key, edge] : edges) {
        if (edge.uses <= 2 && edge.owners.size() == edge.uses && edge.compatible) {
            adjacency[edge.first].push_back(edge.last);
            adjacency[edge.last].push_back(edge.first);
        } else {
            fixed.insert(edge.first);
            fixed.insert(edge.last);
        }
    }
    for (auto vertex = adjacency.cbegin(); vertex != adjacency.cend(); ++vertex) {
        if (vertex.value().size() != 2 || edges.at(edgeKey(vertex.key(), vertex.value()[0])).owners
            != edges.at(edgeKey(vertex.key(), vertex.value()[1])).owners) fixed.insert(vertex.key());
    }
    for (const auto &contour : mesh.contours) {
        retainLoopAnchors(mesh, contour.points, fixed);
        for (const auto &hole : contour.cutouts) retainLoopAnchors(mesh, hole, fixed);
    }
    const auto walk = [&](int first, int last) {
        SharedChain chain;
        chain.vertices = {first, last};
        chain.owners = edges.at(edgeKey(first, last)).owners;
        visited.insert(edgeKey(first, last));
        while (!fixed.contains(chain.vertices.back()) && chain.vertices.back() != first) {
            const int current = chain.vertices.back();
            int next = -1;
            for (const int neighbor : adjacency.value(current)) if (neighbor != chain.vertices[chain.vertices.size() - 2]) next = neighbor;
            if (next < 0 || visited.contains(edgeKey(current, next))) break;
            visited.insert(edgeKey(current, next));
            chain.vertices.push_back(next);
        }

        for (int edge = 1; edge < chain.vertices.size(); ++edge) {
            auto curve = edges.at(edgeKey(chain.vertices[edge - 1], chain.vertices[edge])).curve;
            if (chain.vertices[edge - 1] > chain.vertices[edge]) {
                std::swap(curve.start, curve.end);
                std::swap(curve.control, curve.control2);
            }
            chain.curves.push_back(curve);
        }

        return chain;
    };
    for (const auto &[key, edge] : edges) {
        if (!adjacency.value(edge.first).contains(edge.last) || visited.contains(key)) continue;
        if (fixed.contains(edge.first)) chains.push_back(walk(edge.first, edge.last));
        else if (fixed.contains(edge.last)) chains.push_back(walk(edge.last, edge.first));
    }
    for (const auto &[key, edge] : edges) {
        if (!adjacency.value(edge.first).contains(edge.last) || visited.contains(key)) continue;
        auto cycle = walk(edge.first, edge.last);
        if (cycle.vertices.front() != cycle.vertices.back() || cycle.vertices.size() < 4) continue;
        const int count = static_cast<int>(cycle.vertices.size()) - 1;
        for (int part = 0; part < 3; ++part) {
            SharedChain chain;
            chain.owners = cycle.owners;
            chain.vertices = cycle.vertices.mid(count * part / 3, count * (part + 1) / 3 - count * part / 3 + 1);
            chain.curves = cycle.curves.mid(count * part / 3, count * (part + 1) / 3 - count * part / 3);
            chains.push_back(std::move(chain));
        }
    }

    return chains;
}

MeshData rebuildMesh(const MeshData &mesh, const QVector<SharedChain> &chains) {
    MeshData result = mesh;
    QHash<quint64, int> replacements;
    QVector<QVector<int>> vertices(chains.size());

    for (int index = 0; index < chains.size(); ++index) {
        const auto &chain = chains[index];
        if (chain.fitted.isEmpty()) continue;
        for (int point = 0; point < chain.fitted.size(); ++point) {
            if (point == 0) vertices[index].push_back(chain.vertices.front());
            else if (point + 1 == chain.fitted.size()) vertices[index].push_back(chain.vertices.back());
            else {
                vertices[index].push_back(static_cast<int>(result.vertices.size()));
                result.vertices.push_back(chain.fitted[point].position);
            }
        }
        for (int edge = 1; edge < chain.vertices.size(); ++edge) replacements.insert(edgeKey(chain.vertices[edge - 1], chain.vertices[edge]), index);
    }
    const auto rebuild = [&](QVector<MeshPoint> &points) {
        QVector<MeshPoint> reduced;
        QVector<QPointF> incoming;
        for (int point = 0; point < points.size(); ++point) {
            const auto &first = points[point];
            const auto &last = points[(point + 1) % points.size()];
            const auto replacement = replacements.constFind(edgeKey(first.vertex, last.vertex));
            if (replacement == replacements.cend()) {
                reduced.push_back(first);
                incoming.push_back(last.incoming);
                continue;
            }
            const int index = replacement.value();
            const auto &chain = chains[index];
            const bool forward = first.vertex == chain.vertices.front() && last.vertex == chain.vertices[1];
            const bool backward = first.vertex == chain.vertices.back() && last.vertex == chain.vertices[chain.vertices.size() - 2];
            if (!forward && !backward) continue;
            for (int segment = 0; segment + 1 < chain.fitted.size(); ++segment) {
                const int start = forward ? segment : static_cast<int>(chain.fitted.size()) - 1 - segment;
                const int end = forward ? start + 1 : start - 1;
                auto fitted = first;
                fitted.vertex = vertices[index][start];
                fitted.kind = segment == 0 ? first.kind : chain.fitted[start].kind;
                fitted.explicitHandles = true;
                fitted.outgoing = forward ? chain.fitted[start].outgoing : chain.fitted[start].incoming;
                reduced.push_back(fitted);
                incoming.push_back(forward ? chain.fitted[end].incoming : chain.fitted[end].outgoing);
            }
        }
        for (int point = 0; point < reduced.size(); ++point) reduced[point].incoming = incoming[(point + reduced.size() - 1) % reduced.size()];
        points = std::move(reduced);
    };
    for (auto &contour : result.contours) {
        rebuild(contour.points);
        for (auto &hole : contour.cutouts) rebuild(hole);
    }
    QHash<int, int> compact;
    QVector<QPointF> positions;
    const auto remap = [&](QVector<MeshPoint> &points) {
        for (auto &point : points) {
            if (!compact.contains(point.vertex)) {
                compact.insert(point.vertex, static_cast<int>(positions.size()));
                positions.push_back(result.vertices[point.vertex]);
            }
            point.vertex = compact.value(point.vertex);
        }
    };
    for (auto &contour : result.contours) {
        remap(contour.points);
        for (auto &hole : contour.cutouts) remap(hole);
    }
    result.vertices = std::move(positions);

    return result;
}

} // namespace

void simplifyMeshBoundaries(MeshData &mesh, double tolerance, const QSet<int> &anchors,
                            const MeshSimplificationValidator &validate, QJsonObject *diagnostics, int softVertexLimit) {
    QElapsedTimer timer;
    const QRectF bounds = QPolygonF(mesh.vertices).boundingRect();
    const MeshData original = mesh;
    const double extent = std::max(bounds.width(), bounds.height());
    const double scale = extent > 0.0 ? kReferenceLength / extent : 1.0;
    const bool automatic = tolerance <= 0.0;
    const double bucketTolerance = CubicFitOptions{}.tolerance;
    const double initialTolerance = automatic ? bucketTolerance / (scale * kPolylineToleranceFraction) : tolerance;
    auto sourceChains = sharedChains(original, anchors, initialTolerance);
    QVector<double> limits;
    QSet<int> removable;
    QJsonArray checks;
    int attempts = 0;
    int trials = 0;
    int accepted = 0;
    int cubic = 0;
    int polygonal = 0;
    int refined = 0;
    qint64 validationTime = 0;
    double selectedTolerance = initialTolerance;

    timer.start();
    for (const auto &contour : original.contours) {
        const auto outer = loopMeasure(original, contour.points);
        double area = std::abs(outer.first);
        double perimeter = outer.second;
        for (const auto &hole : contour.cutouts) {
            const auto interior = loopMeasure(original, hole);
            area -= std::abs(interior.first);
            perimeter += interior.second;
        }
        const double width = perimeter > 0.0 ? 2.0 * std::max(0.0, area) / perimeter : 0.0;
        limits.push_back(std::max(bucketTolerance, width * scale * kBucketStrokeFraction) / (scale * kPolylineToleranceFraction));
    }
    for (auto &chain : sourceChains) {
        if (automatic) for (const int owner : chain.owners) chain.toleranceLimit = std::min(chain.toleranceLimit, limits[owner]);
        for (int point = 1; point + 1 < chain.vertices.size(); ++point) removable.insert(chain.vertices[point]);
    }
    for (int trial = 0; trial < (automatic ? kBudgetTrials : 1); ++trial) {
        auto chains = sourceChains;
        const double factor = automatic ? 1.0 + (kMaximumBucketToleranceFactor - 1.0) * trial / (kBudgetTrials - 1) : 1.0;
        const double allowance = initialTolerance * factor;
        ++trials;
        for (auto &chain : chains) fitChain(chain, original, std::min(allowance, chain.toleranceLimit), scale);
        while (std::any_of(chains.cbegin(), chains.cend(), [](const auto &chain) { return !chain.fitted.isEmpty(); })) {
            auto candidate = rebuildMesh(original, chains);
            QSet<int> unsafe;
            const qint64 started = timer.elapsed();
            ++attempts;
            const bool structural = candidate.valid();
            const bool winding = preservesWinding(original, candidate, unsafe);
            const bool valid = structural && winding && (!validate || validate(candidate, unsafe));
            QJsonArray unsafeIndices;
            for (const int contour : unsafe) unsafeIndices.append(contour);
            checks.append(QJsonObject{{QStringLiteral("trial"), trial}, {QStringLiteral("vertices"), candidate.vertices.size()},
                {QStringLiteral("structural"), structural}, {QStringLiteral("winding"), winding},
                {QStringLiteral("valid"), valid}, {QStringLiteral("unsafeContours"), unsafeIndices}});
            validationTime += timer.elapsed() - started;
            if (valid) {
                if (candidate.vertices.size() < mesh.vertices.size()) {
                    accepted = cubic = polygonal = refined = 0;
                    for (const auto &chain : chains) if (!chain.fitted.isEmpty()) {
                        ++accepted;
                        if (chain.refinements > 0) ++refined;
                        if (chain.level == 0) ++cubic;
                        else ++polygonal;
                    }
                    mesh = std::move(candidate);
                    selectedTolerance = allowance;
                }
                break;
            }
            bool revised = false;
            for (auto &chain : chains) {
                if (chain.fitted.isEmpty() || (!unsafe.isEmpty() && !chain.owners.intersects(unsafe))) continue;
                if (chain.refinements < kMaximumFitRefinements) ++chain.refinements;
                else ++chain.level;
                fitChain(chain, original, std::min(allowance, chain.toleranceLimit), scale);
                revised = true;
            }
            if (!revised) break;
        }
        if (mesh.vertices.size() <= softVertexLimit) break;
    }
    if (diagnostics != nullptr) {
        *diagnostics = {{QStringLiteral("verticesBefore"), original.vertices.size()}, {QStringLiteral("verticesAfter"), mesh.vertices.size()},
                        {QStringLiteral("checks"), checks},
                        {QStringLiteral("chains"), sourceChains.size()}, {QStringLiteral("acceptedChains"), accepted},
                        {QStringLiteral("cubicChains"), cubic}, {QStringLiteral("polylineChains"), polygonal},
                        {QStringLiteral("refinedChains"), refined},
                        {QStringLiteral("validationMs"), validationTime}, {QStringLiteral("budgetTrials"), trials},
                        {QStringLiteral("fixedVertices"), original.vertices.size() - removable.size()},
                        {QStringLiteral("softVertexLimit"), softVertexLimit}, {QStringLiteral("metSoftLimit"), mesh.vertices.size() <= softVertexLimit},
                        {QStringLiteral("fitTolerancePixels"), selectedTolerance * scale * kPolylineToleranceFraction},
                        {QStringLiteral("attempts"), attempts}, {QStringLiteral("tolerance"), selectedTolerance}, {QStringLiteral("elapsedMs"), timer.elapsed()}};
    }
}

} // namespace gui
