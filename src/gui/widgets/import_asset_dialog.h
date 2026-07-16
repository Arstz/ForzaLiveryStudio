#pragma once

#include <QString>

class QWidget;

namespace gui {

struct ImportAssetSelection {
    QString path;
    QString directory;
    bool motorsport = false;
};

ImportAssetSelection showImportAssetDialog(QWidget *parent, const QString &startDirectory);

} // namespace gui
