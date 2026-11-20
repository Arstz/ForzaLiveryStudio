#pragma once

#include "vinyl_decoder.h"

#include <optional>

namespace fls::detail {

enum class VinylSceneDialect { Horizon, Motorsport2023, Xbox360 };

Project newVinylImportProject(const QString &folderOrFile, const QByteArray &payload = {});
QString importedVinylLayerName(quint16 shapeId, std::optional<quint32> decalId);
void appendVinylGroupToScene(Project &project, const VinylGroup &root, int sourceOffset,
                             VinylSceneDialect dialect = VinylSceneDialect::Horizon);
void appendLiverySectionsToScene(Project &project, const QVector<LiverySection> &sections,
                                 VinylSceneDialect dialect = VinylSceneDialect::Horizon);

} // namespace fls::detail
