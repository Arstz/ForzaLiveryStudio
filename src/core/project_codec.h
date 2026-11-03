#pragma once

#include "core_types.h"
#include "nested_payload.h"

#include <QJsonObject>
#include <QString>

namespace fls {

inline constexpr int ProjectJsonVersion = 5;
inline constexpr char ProjectJsonFormat[] = "fls_editor_project";
inline constexpr char LegacyProjectJsonFormat[] = "fh6_editor_project";

Project projectFromJson(const QJsonObject &object);
QJsonObject projectToJson(const Project &project);

QByteArray encodeProjectDocument(const Project &project);
Project decodeProjectDocument(const QByteArray &fileBytes);
QByteArray creatorTagFromSavePath(const QString &path);
Project importCGroupFlat(const QString &folderOrFile);
Project importCGroupNested(const QString &folderOrFile);
Project importCGroupNestedData(const QByteArray &wrapped, const QByteArray &header,
                               const QString &name);
Project importCLivery(const QString &folderOrFile);
Project importCLiveryData(const QByteArray &wrapped, const QByteArray &header,
                          const QString &name);
void exportNestedProjectFolder(const Project &project, const QString &outputFolder,
                               const QString &name = {}, const SpriteSizeFn &spriteSize = {});

QByteArray encodeCLiveryPayload(const Project &project);
void exportCLivery(const Project &project, const QString &outputFolder);

} // namespace fls
