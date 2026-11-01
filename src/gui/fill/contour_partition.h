#pragma once

#include "mesh_data.h"

class QJsonObject;

namespace gui {

bool connectMeshRegions(fls::scene::MeshData &mesh, QString *error, QJsonObject *diagnostics);

} // namespace gui
