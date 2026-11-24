#pragma once

#include "game_export.h"
#include "wgs_save_reader.h"

#include <QMap>

namespace fls {

struct WgsExportTarget {
    QString indexPath;
    QByteArray creatorTag;
    QByteArray assetGuid;
    QByteArray fingerprint;
    ExportGameVersion game = ExportGameVersion::Horizon5;
    WgsAssetKind kind = WgsAssetKind::Group;
    int carId = 0;
};

struct WgsAppendResult {
    QString containerName;
    QString backupPath;
};

WgsExportTarget inspectWgsAppendTarget(const QString &indexPath, ExportGameVersion game,
                                      WgsAssetKind kind, int carId = 0);
WgsAppendResult appendWgsContainer(const WgsExportTarget &target, const QMap<QString, QByteArray> &blobs);

} // namespace fls
