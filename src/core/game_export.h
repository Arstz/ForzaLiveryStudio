#pragma once

#include "core_types.h"
#include "nested_payload.h"
#include "xbox360_crypto.h"

namespace fls {

enum class ExportGameVersion {
    Horizon6,
    Horizon5,
    Motorsport2023,
    Horizon2,
    Horizon3,
    Motorsport5,
    Motorsport6,
    Motorsport7,
    Horizon4,
    Motorsport3,
    Motorsport4,
    Horizon1,
    Motorsport2,
};

QString exportGameCode(ExportGameVersion game);
bool isXbox360ExportGame(ExportGameVersion game);
QString xbox360ExportExtension(ExportGameVersion game, Xbox360FileKind kind);
QString exportThumbnailName(ExportGameVersion game, bool livery);
bool exportGameSupportsMasks(ExportGameVersion game);
quint64 exportMaskedLayerCount(const Project &project);
quint64 exportIncompatibleLayerCount(const Project &project, ExportGameVersion game);
void discardIncompatibleExportLayers(Project &project, ExportGameVersion game);
QByteArray encodeGameLiveryPayload(const Project &project, ExportGameVersion game);
void exportGameLivery(const Project &project, const QString &outputFolder,
                      ExportGameVersion game);
void exportGameGroup(const Project &project, const QString &outputFolder,
                     ExportGameVersion game, const SpriteSizeFn &spriteSize = {});
void exportXbox360Asset(const Project &project, const QString &path, ExportGameVersion game,
                        const Xbox360Credentials *credentials = nullptr);

} // namespace fls
