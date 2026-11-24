#pragma once

#include "game_export.h"
#include "wgs_save_writer.h"

#include <QString>

#include <functional>
#include <optional>

class QWidget;

namespace gui {

using ExportGameVersion = fls::ExportGameVersion;

enum class ExportContainerFormat {
    PgsFolder,
    Xbox360PlainFile,
    Xbox360SignedFile,
    WgsFh5ToMotorsport,
    WgsAppend,
};

struct ExportAssetSelection {
    QString directory;
    std::optional<fls::WgsExportTarget> wgsTarget;
    fls::ExportLayerChoices layerChoices;
    ExportGameVersion gameVersion = ExportGameVersion::Horizon6;
    ExportContainerFormat containerFormat = ExportContainerFormat::PgsFolder;
};

bool supportsExportContainer(ExportGameVersion game, ExportContainerFormat format);
void rememberExportSelection(const ExportAssetSelection &selection);

std::optional<ExportAssetSelection> showExportAssetDialog(
    QWidget *parent, const QString &startDirectory, const QString &projectName, bool livery,
    int carId = 0, const std::function<fls::ExportLayerWarnings(ExportGameVersion)> &layerWarnings = {});

} // namespace gui
