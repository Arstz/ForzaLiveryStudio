#pragma once

#include "wgs_save_reader.h"

#include <QString>

#include <optional>

class QWidget;

namespace gui {

struct ImportAssetSelection {
    QString path;
    QString directory;
    std::optional<fls::WgsAsset> wgsAsset;
    bool motorsport = false;
};

ImportAssetSelection showImportAssetDialog(QWidget *parent, const QString &startDirectory);

} // namespace gui
