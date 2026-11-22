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

struct ExportLayerWarnings {
    quint64 masks = 0;
    quint64 logos = 0;
    quint64 shapes = 0;
};

struct ExportLayerChoices {
    bool keepMasks = false;
    bool keepLogos = true;
    bool keepShapes = false;
};

QString exportGameCode(ExportGameVersion game);
bool isXbox360ExportGame(ExportGameVersion game);
QString xbox360ExportExtension(ExportGameVersion game, Xbox360FileKind kind);
QString exportThumbnailName(ExportGameVersion game, bool livery);
bool exportGameSupportsMasks(ExportGameVersion game);
quint64 exportMaskedLayerCount(const Project &project);
quint64 exportIncompatibleLayerCount(const Project &project, ExportGameVersion game);
ExportLayerWarnings exportLayerWarnings(const Project &project, ExportGameVersion game);
bool prepareExportLayers(Project &project, ExportGameVersion game, const ExportLayerChoices &choices);
QByteArray encodeGameLiveryPayload(const Project &project, ExportGameVersion game,
                                  const ExportLayerChoices &choices = {});
void exportGameLivery(const Project &project, const QString &outputFolder,
                      ExportGameVersion game, const ExportLayerChoices &choices = {});
void exportGameGroup(const Project &project, const QString &outputFolder,
                     ExportGameVersion game, const SpriteSizeFn &spriteSize = {},
                     const ExportLayerChoices &choices = {});
void exportXbox360Asset(const Project &project, const QString &path, ExportGameVersion game,
                        const Xbox360Credentials *credentials = nullptr,
                        const ExportLayerChoices &choices = {});

} // namespace fls
