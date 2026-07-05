#pragma once

#include "core_types.h"
#include "nested_payload.h"

#include <QJsonObject>
#include <QString>

namespace fh6 {

inline constexpr int ProjectJsonVersion = 1;
inline constexpr char ProjectJsonFormat[] = "fh6_editor_project";

ShapeLayer shapeLayerFromJson(const QJsonObject &object);
LayerGroup layerGroupFromJson(const QJsonObject &object);
Project projectFromJson(const QJsonObject &object);
QJsonObject shapeLayerToJson(const ShapeLayer &layer);
QJsonObject layerGroupToJson(const LayerGroup &group);
QJsonObject projectToJson(const Project &project);

// Editor project container (`.3so`): the project JSON wrapped in a gzip stream.
// encodeProjectDocument serializes the project and gzip-compresses it.
// decodeProjectDocument accepts either a gzip stream (`.3so`) or bare JSON (legacy
// `.json` projects), sniffing the gzip magic, and returns the parsed project.
QByteArray encodeProjectDocument(const Project &project);
Project decodeProjectDocument(const QByteArray &fileBytes);
Project importCGroupFlat(const QString &folderOrFile);
Project importCGroupNested(const QString &folderOrFile);
// Import a C_livery (read-only): builds 11 top-level panel-section groups.
Project importCLivery(const QString &folderOrFile);
void exportFlatProjectFolder(const Project &project, const QString &outputFolder, const QString &name);
void exportNestedProjectFolder(const Project &project, const QString &outputFolder,
                               const QString &name = {}, const SpriteSizeFn &spriteSize = {});

} // namespace fh6
