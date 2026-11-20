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
    WgsContainer,
    Xbox360PlainFile,
    Xbox360SignedFile,
};

struct ExportAssetSelection {
    QString directory;
    std::optional<fls::WgsExportTarget> wgsTarget;
    ExportGameVersion gameVersion = ExportGameVersion::Horizon6;
    ExportContainerFormat containerFormat = ExportContainerFormat::PgsFolder;
};

std::optional<ExportAssetSelection> showExportAssetDialog(
    QWidget *parent, const QString &startDirectory, const QString &projectName, bool livery,
    int carId = 0, quint64 maskedLayerCount = 0,
    const std::function<quint64(ExportGameVersion)> &incompatibleLayerCount = {});

} // namespace gui
