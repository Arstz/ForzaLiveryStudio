#pragma once

#include "core_types.h"
#include "nested_payload.h"

#include <QJsonObject>
#include <QString>

namespace fh6 {

inline constexpr int ProjectJsonVersion = 2;
inline constexpr char ProjectJsonFormat[] = "fh6_editor_project";

Project projectFromJson(const QJsonObject &object);
QJsonObject projectToJson(const Project &project);

QByteArray encodeProjectDocument(const Project &project);
Project decodeProjectDocument(const QByteArray &fileBytes);
Project importCGroupFlat(const QString &folderOrFile);
Project importCGroupNested(const QString &folderOrFile);
Project importCLivery(const QString &folderOrFile);
void exportNestedProjectFolder(const Project &project, const QString &outputFolder,
                               const QString &name = {}, const SpriteSizeFn &spriteSize = {});

Project importFM2023Asset(const QString &folderOrFile);

QByteArray encodeCLiveryPayload(const Project &project);
void exportCLivery(const Project &project, const QString &outputFolder);

} // namespace fh6
