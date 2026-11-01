#include "mesh_edit.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <map>

namespace gui {
namespace {

using fls::scene::MeshData;
using fls::scene::MeshPoint;

constexpr int kMinimumClosedPoints = 3;

template <typename Mesh, typename Function>
void forEachMeshLoop(Mesh &mesh, const Function &function) {
    for (auto &contour : mesh.contours) {
        function(contour.points, contour.style.pathKind == fls::scene::ContourPathKind::Pen && contour.style.closed);
        for (int hole = 0; hole < contour.cutouts.size(); ++hole) {
            function(contour.cutouts[hole], contour.style.cutoutClosed || hole + 1 < contour.cutouts.size());
        }
    }
}

bool matchesEdge(int first, int last, int a, int b) {

    return (first == a && last == b) || (first == b && last == a);
}

PenBoundarySegment explicitCurve(PenBoundarySegment curve) {
    if (!curve.curved) {
        curve.control = curve.start + (curve.end - curve.start) / 3.0;
        curve.control2 = curve.end - (curve.end - curve.start) / 3.0;
        curve.curved = true;
    }

    return curve;
}

void storeEdgeCurve(MeshData &mesh, const MeshEditEdge &edge, int curvedVertex = -1) {
    const auto curve = explicitCurve(edge.curve);

    forEachMeshLoop(mesh, [&](auto &points, bool closed) {
        for (auto &point : points) if (point.vertex == curvedVertex) point.kind = PenPointKind::Soft;
        for (int index = 0; index < points.size() - (closed ? 0 : 1); ++index) {
            auto &first = points[index];
            auto &last = points[(index + 1) % points.size()];
            if (!matchesEdge(edge.first, edge.last, first.vertex, last.vertex)) continue;
            const bool forward = first.vertex == edge.first;
            first.outgoing = forward ? curve.control - curve.start : curve.control2 - curve.end;
            last.incoming = forward ? curve.control2 - curve.end : curve.control - curve.start;
            first.explicitHandles = last.explicitHandles = true;
        }
    });
}

void compactVertices(MeshData &mesh) {
    QHash<int, int> remap;
    QVector<QPointF> vertices;

    forEachMeshLoop(mesh, [&](auto &points, bool) {
        for (auto &point : points) {
            if (!remap.contains(point.vertex)) {
                remap.insert(point.vertex, static_cast<int>(vertices.size()));
                vertices.push_back(mesh.vertices[point.vertex]);
            }
            point.vertex = remap.value(point.vertex);
        }
    });
    mesh.vertices = std::move(vertices);
}

} // namespace

QVector<MeshEditEdge> meshEditEdges(const MeshData &mesh) {
    std::map<std::pair<int, int>, MeshEditEdge> edges;
    QVector<MeshEditEdge> result;

    forEachMeshLoop(mesh, [&](const auto &points, bool closed) {
        QVector<PenPoint> resolved;
        for (const auto &point : points) {
            resolved.push_back({mesh.vertices[point.vertex], point.kind, point.incoming, point.outgoing, point.explicitHandles});
        }
        const auto curves = penSegments(resolved, closed);
        for (int index = 0; index < curves.size(); ++index) {
            const int first = points[index].vertex;
            const int last = points[(index + 1) % points.size()].vertex;
            auto curve = curves[index];
            if (first > last) {
                std::swap(curve.start, curve.end);
                std::swap(curve.control, curve.control2);
            }
            const auto key = std::minmax(first, last);
            edges.try_emplace(key, MeshEditEdge{curve, key.first, key.second});
        }
    });
    for (const auto &[key, edge] : edges) result.push_back(edge);

    return result;
}

void materializeMeshHandles(MeshData &mesh) {
    forEachMeshLoop(mesh, [&](auto &points, bool closed) {
        QVector<PenPoint> resolved;
        for (const auto &point : points) {
            resolved.push_back({mesh.vertices[point.vertex], point.kind, point.incoming, point.outgoing, point.explicitHandles});
        }
        const auto curves = penSegments(resolved, closed);
        if (curves.isEmpty()) return;
        for (int index = 0; index < points.size(); ++index) {
            if (closed || index > 0) points[index].incoming = explicitCurve(curves[(index + curves.size() - 1) % curves.size()]).control2 - resolved[index].position;
            if (index < curves.size()) points[index].outgoing = explicitCurve(curves[index]).control - resolved[index].position;
            points[index].explicitHandles = true;
        }
    });
}

bool translateMeshVertices(MeshData &mesh, const QSet<int> &vertices, const QPointF &delta) {
    const auto edges = meshEditEdges(mesh);
    QHash<QPair<int, int>, PenBoundarySegment> curves;

    if (!std::isfinite(delta.x()) || !std::isfinite(delta.y()) || !std::all_of(vertices.cbegin(), vertices.cend(), [&](int vertex) {
        return vertex >= 0 && vertex < mesh.vertices.size();
    })) return false;
    if (delta.isNull()) return true;
    materializeMeshHandles(mesh);
    for (const int vertex : vertices) mesh.vertices[vertex] += delta;
    for (const auto &edge : edges) {
        auto curve = edge.curve;
        curve.control += mesh.vertices[edge.first] - curve.start;
        curve.control2 += mesh.vertices[edge.last] - curve.end;
        curve.start = mesh.vertices[edge.first];
        curve.end = mesh.vertices[edge.last];
        auto controls = explicitCurve(curve);
        controls.curved = curve.curved;
        curves.insert({edge.first, edge.last}, controls);
    }
    forEachMeshLoop(mesh, [&](auto &points, bool closed) {
        for (int index = 0; index < points.size() - (closed ? 0 : 1); ++index) {
            auto &first = points[index];
            auto &last = points[(index + 1) % points.size()];
            const auto curve = curves.value(std::minmax(first.vertex, last.vertex));
            const bool forward = first.vertex < last.vertex;
            first.outgoing = forward ? curve.control - curve.start : curve.control2 - curve.end;
            last.incoming = forward ? curve.control2 - curve.end : curve.control - curve.start;
            if (curve.curved && first.kind == PenPointKind::Hard && last.kind == PenPointKind::Hard) first.kind = PenPointKind::Soft;
        }
    });

    return mesh.valid();
}

bool setMeshEdgeHandle(MeshData &mesh, int first, int last, int vertex, const QPointF &control) {
    auto edges = meshEditEdges(mesh);
    const auto found = std::find_if(edges.begin(), edges.end(), [&](const auto &edge) {
        return matchesEdge(first, last, edge.first, edge.last);
    });

    if (found == edges.end() || (vertex != first && vertex != last) || !std::isfinite(control.x()) || !std::isfinite(control.y())) return false;
    materializeMeshHandles(mesh);
    if (vertex == found->first) found->curve.control = control;
    else found->curve.control2 = control;
    found->curve.curved = true;
    storeEdgeCurve(mesh, *found, vertex);

    return true;
}

int splitMeshEdge(MeshData &mesh, const MeshEditEdge &edge, double parameter) {
    const auto halves = explicitCurve(edge.curve).split(parameter);
    const int vertex = static_cast<int>(mesh.vertices.size());
    bool inserted = false;

    if (!std::isfinite(parameter) || parameter <= 0.0 || parameter >= 1.0) return -1;
    forEachMeshLoop(mesh, [&](auto &points, bool closed) {
        for (int index = static_cast<int>(points.size()) - (closed ? 1 : 2); index >= 0; --index) {
            const int next = (index + 1) % points.size();
            const bool forward = points[index].vertex == edge.first;
            if (!matchesEdge(edge.first, edge.last, points[index].vertex, points[next].vertex)) continue;
            auto middle = points[index];
            middle.vertex = vertex;
            middle.kind = PenPointKind::Soft;
            middle.explicitHandles = true;
            middle.incoming = (forward ? halves.first.control2 : halves.second.control) - halves.first.end;
            middle.outgoing = (forward ? halves.second.control : halves.first.control2) - halves.first.end;
            points[index].outgoing = forward ? halves.first.control - halves.first.start : halves.second.control2 - halves.second.end;
            points[next].incoming = forward ? halves.second.control2 - halves.second.end : halves.first.control - halves.first.start;
            points[index].explicitHandles = points[next].explicitHandles = true;
            points.insert(index + 1, middle);
            inserted = true;
        }
    });
    if (inserted) mesh.vertices.push_back(halves.first.end);

    return inserted ? vertex : -1;
}

bool removeMeshVertices(MeshData &mesh, const QSet<int> &vertices, QString *error) {
    MeshData candidate = mesh;
    QHash<int, QSet<int>> neighbors;
    QSet<QPair<int, int>> originalEdges;
    bool removable = !vertices.isEmpty() && std::all_of(vertices.cbegin(), vertices.cend(), [&](int vertex) {
        return vertex >= 0 && vertex < mesh.vertices.size();
    });

    materializeMeshHandles(candidate);
    for (const auto &edge : meshEditEdges(mesh)) originalEdges.insert({edge.first, edge.last});
    forEachMeshLoop(candidate, [&](auto &points, bool closed) {
        for (int index = 0; index < points.size(); ++index) {
            const int vertex = points[index].vertex;
            if (!vertices.contains(vertex)) continue;
            if (closed || index > 0) neighbors[vertex].insert(points[(index + points.size() - 1) % points.size()].vertex);
            if (closed || index + 1 < points.size()) neighbors[vertex].insert(points[(index + 1) % points.size()].vertex);
        }
        const int removed = std::count_if(points.cbegin(), points.cend(), [&](const auto &point) { return vertices.contains(point.vertex); });
        if (removed == 0) return;
        removable &= points.size() - removed >= (closed ? kMinimumClosedPoints : 2);
        points.erase(std::remove_if(points.begin(), points.end(), [&](const auto &point) { return vertices.contains(point.vertex); }), points.end());
    });
    for (const auto &adjacent : neighbors) removable &= adjacent.size() <= 2;
    if (!removable || !candidate.valid()) {
        if (error != nullptr) *error = QStringLiteral("Keep shared junctions and enough anchors to define each region");

        return false;
    }
    for (const auto &edge : meshEditEdges(candidate)) {
        if (!originalEdges.contains({edge.first, edge.last})) storeEdgeCurve(candidate, edge, edge.curve.curved ? edge.first : -1);
    }
    compactVertices(candidate);
    mesh = std::move(candidate);

    return true;
}

} // namespace gui
