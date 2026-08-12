#pragma once

#include <QString>
#include <QVector>

namespace fls {

enum class WgsAssetKind {
    Group,
    Livery,
};

struct WgsAsset {
    QString containerName;
    QString payloadPath;
    QString headerPath;
    QString thumbnailPath;
    WgsAssetKind kind = WgsAssetKind::Group;
};

struct WgsSave {
    QString packageId;
    QVector<WgsAsset> assets;
    int containerCount = 0;
};

bool isWgsSaveDirectory(const QString &path);
WgsSave readWgsSave(const QString &path);

} // namespace fls
