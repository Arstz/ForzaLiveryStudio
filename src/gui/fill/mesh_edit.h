#pragma once

#include "mesh_data.h"
#include "pen_fill.h"
#include <QSet>

namespace gui {

struct MeshEditEdge {
    PenBoundarySegment curve;
    int first = -1;
    int last = -1;
};

QVector<MeshEditEdge> meshEditEdges(const fls::scene::MeshData &mesh);
void materializeMeshHandles(fls::scene::MeshData &mesh);
bool translateMeshVertices(fls::scene::MeshData &mesh, const QSet<int> &vertices, const QPointF &delta);
bool setMeshEdgeHandle(fls::scene::MeshData &mesh, int first, int last, int vertex, const QPointF &control);
int splitMeshEdge(fls::scene::MeshData &mesh, const MeshEditEdge &edge, double parameter);
bool removeMeshVertices(fls::scene::MeshData &mesh, const QSet<int> &vertices, QString *error = nullptr);

} // namespace gui
