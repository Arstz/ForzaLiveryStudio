#include "game_paths.h"

#include <QDir>

namespace fh6 {

QString gameMediaDir(const QString &gameFolder) {
    if (gameFolder.isEmpty()) {
        return QString();
    }
    const QDir root(gameFolder);
    if (root.exists(QStringLiteral("Cars"))) {
        return root.absolutePath();
    }
    if (root.exists(QStringLiteral("media"))) {
        return root.filePath(QStringLiteral("media"));
    }

    return root.absolutePath();
}

QString gameCarsDir(const QString &gameFolder) {
    const QString media = gameMediaDir(gameFolder);

    return media.isEmpty() ? QString() : QDir(media).filePath(QStringLiteral("Cars"));
}

QString gamePaintMaterialsArchive(const QString &gameFolder) {
    const QString cars = gameCarsDir(gameFolder);

    return cars.isEmpty() ? QString() : QDir(cars).filePath(QStringLiteral("_library/Materials.zip"));
}

QString gamePaintTexturesArchive(const QString &gameFolder) {
    const QString cars = gameCarsDir(gameFolder);

    return cars.isEmpty() ? QString() : QDir(cars).filePath(QStringLiteral("_library/Textures.zip"));
}

QString gamePaintMaterialsPrefix() {
    return QStringLiteral("_fmnext/usercustomizable/");
}

} // namespace fh6
