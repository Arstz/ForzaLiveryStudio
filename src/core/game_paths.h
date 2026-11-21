#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QSet>

class QDir;
class QFileInfo;

namespace fls {

class CarAssetIndex {
public:
    explicit CarAssetIndex(const QString &root);

    QString resolve(const QString &reference, const QString &originDirectory = {}) const;
    QString scene(const QString &modelCode) const;

private:
    QString root_;
    QHash<QString, QStringList> pathsByName_;
    bool obfuscatedNames_;
};

bool hasFh3ObfuscatedNames(const QString &directory);
QString decodeFh3AssetName(const QString &name);
QString gameAssetFileName(const QFileInfo &file);
QString gameAssetFilePath(const QDir &directory, const QString &relativePath);

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
