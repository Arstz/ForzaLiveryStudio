#pragma once

#include "core_types.h"
#include "nested_payload.h"

#include <QJsonObject>
#include <QString>

namespace fh6 {

// v2 introduced the unified scene tree ("root"). v1 documents (flat layers/groups
// + root_child_ids) are still read via the legacy path in projectFromJson.
inline constexpr int ProjectJsonVersion = 2;
inline constexpr char ProjectJsonFormat[] = "fh6_editor_project";

Project projectFromJson(const QJsonObject &object);
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
void exportNestedProjectFolder(const Project &project, const QString &outputFolder,
                               const QString &name = {}, const SpriteSizeFn &spriteSize = {});

// Import an FM2023 (Forza Motorsport 2023+) asset. Detects livery vs raw group
// variants and routes accordingly. Returns an editable project.
Project importFM2023Asset(const QString &folderOrFile);

// Livery export. encodeCLiveryPayload rebuilds the decompressed C_livery container
// from the payload captured on import (project.liverySource), replacing the gyvl
// artwork with buildLiveryGyvl(project), recomputing the 11 section decal counts,
// and writing the current target car id into the vlrc root header. exportCLivery
// writes a game-ready folder (C_livery + car-id-patched header + bigThumb.webp).
// Requires an imported livery; from-scratch container synthesis remains separate.
QByteArray encodeCLiveryPayload(const Project &project);
void exportCLivery(const Project &project, const QString &outputFolder);

} // namespace fh6
