#pragma once

#include "game_export.h"
#include "wgs_save_reader.h"

#include <QMap>
#include <QSize>

class QImage;

namespace fls {

struct WgsExportTarget {
    QString indexPath;
    QByteArray creatorTag;
    QByteArray assetGuid;
    QByteArray fingerprint;
    QSize liveryThumbnailSize;
    QSize smallLiveryThumbnailSize;
    NativeExportVersions versions;
    ExportGameVersion game = ExportGameVersion::Horizon5;
    WgsAssetKind kind = WgsAssetKind::Group;
    int carId = 0;

    ExportGameVersion encodingGame() const;
    QSize thumbnailSize() const;
};

struct WgsAppendResult {
    QString containerName;
    QString backupPath;
};

bool supportsWgsAppend(ExportGameVersion game);
WgsExportTarget inspectWgsAppendTarget(const QString &indexPath, ExportGameVersion game,
                                      WgsAssetKind kind, int carId = 0);
WgsAppendResult appendWgsContainer(const WgsExportTarget &target, const QMap<QString, QByteArray> &blobs);
WgsAppendResult exportWgsAsset(const WgsExportTarget &target, Project project, const QImage &thumbnail,
                              const SpriteSizeFn &spriteSize, const ExportLayerChoices &choices);

} // namespace fls
