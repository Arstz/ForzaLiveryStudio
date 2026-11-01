#include "mesh_data.h"

#include <QSet>
#include <QHash>

#include <algorithm>
#include <cmath>

namespace fls::scene {
namespace {

QVector<ContourPoint> resolvePoints(const QVector<MeshPoint> &points, const QVector<QPointF> &vertices) {
    QVector<ContourPoint> result;

    for (const auto &point : points) {
        result.push_back({vertices.value(point.vertex), point.kind, point.incoming, point.outgoing,
                          point.explicitHandles, point.strokeWidth, point.endCap});
    }

    return result;
}

QVector<MeshPoint> storePoints(const QVector<ContourPoint> &points, QVector<QPointF> &vertices) {
    QVector<MeshPoint> result;

    for (const auto &point : points) {
        result.push_back({point.incoming, point.outgoing, point.kind, static_cast<int>(vertices.size()),
                          point.explicitHandles, point.strokeWidth, point.endCap});
        vertices.push_back(point.position);
    }

    return result;
}

QSet<int> contourVertices(const MeshContour &contour) {
    QSet<int> result;
    const auto collect = [&](const QVector<MeshPoint> &points) {
        for (const auto &point : points) result.insert(point.vertex);
    };

    collect(contour.points);
    for (const auto &cutout : contour.cutouts) collect(cutout);

    return result;
}

bool finitePoint(const QPointF &point) {
    return std::isfinite(point.x()) && std::isfinite(point.y());
}

} // namespace

ContourData MeshData::contourData(int index) const {
    const auto &contour = contours[index];
    ContourData result = contour.style;

    result.points = resolvePoints(contour.points, vertices);
    result.cutouts.clear();
    for (const auto &cutout : contour.cutouts) result.cutouts.push_back(resolvePoints(cutout, vertices));

    return result;
}

void MeshData::appendContour(const ContourData &data, const QString &name, bool visible, double opacity) {
    MeshContour contour;

    contour.name = name;
    contour.style = data;
    contour.style.points.clear();
    contour.style.cutouts.clear();
    contour.points = storePoints(data.points, vertices);
    for (const auto &cutout : data.cutouts) contour.cutouts.push_back(storePoints(cutout, vertices));
    contour.visible = visible;
    contour.opacity = opacity;
    contours.push_back(std::move(contour));
}

bool MeshData::replaceContour(int index, const ContourData &data, QString *error) {
    MeshData replacement = *this;
    QHash<int, int> uses;
    QHash<int, QPointF> moved;
    const auto count = [&](const QVector<MeshPoint> &points) {
        for (const auto &point : points) ++uses[point.vertex];
    };

    if (index < 0 || index >= contours.size()) return false;
    for (int other = 0; other < contours.size(); ++other) {
        if (other == index) continue;
        count(contours[other].points);
        for (const auto &cutout : contours[other].cutouts) count(cutout);
    }
    const auto update = [&](const QVector<MeshPoint> &oldPoints, const QVector<ContourPoint> &points,
                            QVector<MeshPoint> &destination) {
        QVector<int> matched;
        QSet<int> retained;
        int previous = -1;
        for (int pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            int match = oldPoints.size() == points.size() ? pointIndex : -1;
            if (match < 0) {
                for (int oldIndex = previous + 1; oldIndex < oldPoints.size(); ++oldIndex) {
                    if (vertices[oldPoints[oldIndex].vertex] == points[pointIndex].position) {
                        match = oldIndex;
                        previous = oldIndex;
                        break;
                    }
                }
            }
            matched.push_back(match);
            if (match >= 0) retained.insert(match);
        }
        for (int oldIndex = 0; oldIndex < oldPoints.size(); ++oldIndex) {
            if (!retained.contains(oldIndex) && uses.value(oldPoints[oldIndex].vertex) > 0) return false;
        }
        destination.clear();
        for (int pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            const auto &point = points[pointIndex];
            const int match = matched[pointIndex];
            int vertex = match >= 0 ? oldPoints[match].vertex : static_cast<int>(replacement.vertices.size());
            if (match < 0) replacement.vertices.push_back(point.position);
            else if (point.position != vertices[vertex]) {
                if (moved.contains(vertex) && moved.value(vertex) != point.position) return false;
                moved.insert(vertex, point.position);
                replacement.vertices[vertex] = point.position;
            }
            destination.push_back({point.incoming, point.outgoing, point.kind, vertex,
                                   point.explicitHandles, point.strokeWidth, point.endCap});
        }

        return true;
    };
    auto &contour = replacement.contours[index];
    bool accepted = update(contours[index].points, data.points, contour.points);
    contour.cutouts.resize(std::max(contours[index].cutouts.size(), data.cutouts.size()));
    for (int cutout = 0; cutout < contour.cutouts.size(); ++cutout) {
        accepted = update(contours[index].cutouts.value(cutout), data.cutouts.value(cutout), contour.cutouts[cutout]) && accepted;
    }
    contour.cutouts.resize(data.cutouts.size());
    contour.style = data;
    contour.style.points.clear();
    contour.style.cutouts.clear();
    if (!accepted || !replacement.valid()) {
        if (error != nullptr) *error = QStringLiteral("Mesh edits must retain shared connection anchors");

        return false;
    }
    *this = std::move(replacement);

    return true;
}

bool MeshData::connected() const {
    QVector<QSet<int>> sets;
    QSet<int> reached;
    QSet<int> visited;

    if (contours.isEmpty()) return false;
    for (const auto &contour : contours) sets.push_back(contourVertices(contour));
    reached = sets.front();
    visited.insert(0);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int index = 0; index < sets.size(); ++index) {
            if (!visited.contains(index) && sets[index].intersects(reached)) {
                visited.insert(index);
                reached.unite(sets[index]);
                changed = true;
            }
        }
    }

    return visited.size() == contours.size();
}

bool MeshData::valid() const {
    const auto validPoints = [&](const QVector<MeshPoint> &points) {
        return std::all_of(points.cbegin(), points.cend(), [&](const MeshPoint &point) {
            return point.vertex >= 0 && point.vertex < vertices.size()
                && finitePoint(point.incoming) && finitePoint(point.outgoing)
                && std::isfinite(point.strokeWidth) && point.strokeWidth >= 0.0
                && std::isfinite(point.endCap);
        });
    };

    if (contours.size() < 2 || !std::all_of(vertices.cbegin(), vertices.cend(), finitePoint)) return false;
    for (const auto &contour : contours) {
        if (contour.points.size() < 2 || !validPoints(contour.points)
            || !std::all_of(contour.cutouts.cbegin(), contour.cutouts.cend(), validPoints)
            || !std::isfinite(contour.opacity) || contour.opacity < 0.0 || contour.opacity > 1.0
            || !std::isfinite(contour.style.strokeWidth) || contour.style.strokeWidth <= 0.0) {
            return false;
        }
    }

    return connected();
}

} // namespace fls::scene
