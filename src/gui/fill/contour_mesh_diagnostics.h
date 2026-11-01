#pragma once

#include "mesh_data.h"

#include <QJsonObject>

namespace gui {

QString writeMeshReconstructionDiagnostics(const fls::scene::MeshData &input, const fls::scene::MeshData *output,
                                           QJsonObject report, const QVector<QString> &sourceEntries,
                                           const QString &directory = {});

} // namespace gui
