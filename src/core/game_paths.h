#pragma once

#include <QString>
#include <QStringList>
#include <QSet>

namespace fls {

QStringList carModelGames(const QString &preferredGame = {});
QString gameMediaDir(const QString &gameFolder);
QStringList gameCarsDirs(const QString &gameFolder);
QString gameCarsDir(const QString &gameFolder);
QString findCarModelPath(const QString &carsFolder, const QString &modelCode);
QSet<QString> availableCarModelCodes(const QString &gameFolder);
QString gamePaintMaterialsArchive(const QString &gameFolder);
QString gamePaintTexturesArchive(const QString &gameFolder);

// Entry prefix of the customizable paint materials inside the materials archive.
QString gamePaintMaterialsPrefix();

} // namespace fls
