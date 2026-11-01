#pragma once

#include "mesh_data.h"

#include <QJsonObject>
#include <QSet>
#include <functional>

namespace gui {

using MeshSimplificationValidator = std::function<bool(const fls::scene::MeshData &, QSet<int> &)>;
inline constexpr int kMeshSoftVertexLimit = 250;

void simplifyMeshBoundaries(fls::scene::MeshData &mesh, double tolerance, const QSet<int> &anchors,
                            const MeshSimplificationValidator &validate, QJsonObject *diagnostics = nullptr, int softVertexLimit = kMeshSoftVertexLimit);

} // namespace gui
