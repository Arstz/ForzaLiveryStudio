#pragma once

#include "layer.h"

#include <QTransform>

class QJsonObject;

namespace gui {

void appendMeshGeometry(fls::scene::MeshData &destination, const fls::scene::Layer &node,
                        const QTransform &transform);
bool connectMeshContours(fls::scene::MeshData &mesh, QString *error = nullptr, QJsonObject *diagnostics = nullptr);

} // namespace gui
