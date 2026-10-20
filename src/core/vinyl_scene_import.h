#pragma once

#include "vinyl_decoder.h"

namespace fls::detail {

enum class VinylSceneDialect { Horizon, Motorsport2023 };

Project newVinylImportProject(const QString &folderOrFile, const QByteArray &payload = {});
void appendVinylGroupToScene(Project &project, const VinylGroup &root, int sourceOffset,
                             VinylSceneDialect dialect = VinylSceneDialect::Horizon);
void appendLiverySectionsToScene(Project &project, const QVector<LiverySection> &sections,
                                 VinylSceneDialect dialect = VinylSceneDialect::Horizon);

} // namespace fls::detail
