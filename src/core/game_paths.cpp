#include "game_paths.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace fls {
namespace {

const QStringList kCarModelExtensions = {QStringLiteral("carbin"), QStringLiteral("zip"), QStringLiteral("modelbin")};

QStringList assetPathComponents(QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));

    return QDir::cleanPath(path).toLower().split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

} // namespace

CarAssetIndex::CarAssetIndex(const QString &root)
    : root_(QDir(root).absolutePath()) {
    const QDir directory(root_);
    QDirIterator files(root_, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        const QFileInfo info = files.fileInfo();
        pathsByName_[info.fileName().toLower()].push_back(directory.relativeFilePath(info.filePath()));
    }
}

QString CarAssetIndex::resolve(const QString &reference, const QString &originDirectory) const {
    if (reference.isEmpty()) return {};
    const QStringList requested = assetPathComponents(reference);
    if (requested.isEmpty()) return {};

    const auto candidates = pathsByName_.constFind(requested.back());
    if (candidates == pathsByName_.cend()) return {};
    const QDir root(root_);
    const QString origin = originDirectory.isEmpty()
        ? QStringLiteral(".") : root.relativeFilePath(originDirectory);
    QStringList logicalPath = requested;
    const qsizetype cars = logicalPath.indexOf(QStringLiteral("cars"));
    if (logicalPath.front() == QStringLiteral("game:") && cars >= 0 && cars + 2 < logicalPath.size()) {
        logicalPath = logicalPath.sliced(cars + 2);
    }
    const QStringList anchored = assetPathComponents(origin + QLatin1Char('/') + logicalPath.join(QLatin1Char('/')));
    const bool relativeOnly = requested.contains(QStringLiteral(".."));
    QString best;
    qsizetype bestScore = 0;
    bool ambiguous = false;
    for (const QString &candidate : *candidates) {
        const QStringList components = assetPathComponents(candidate);
        qsizetype suffix = 0;
        while (suffix < std::min(components.size(), requested.size())
               && components[components.size() - suffix - 1] == requested[requested.size() - suffix - 1]) {
            ++suffix;
        }
        const bool exact = components == anchored;
        if (relativeOnly && !exact) continue;
        const qsizetype score = exact ? requested.size() + 1 : suffix;
        if (score > bestScore) {
            best = candidate;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore) {
            ambiguous = true;
        }
    }

    return best.isEmpty() || ambiguous ? QString() : root.filePath(best);
}

QString CarAssetIndex::scene(const QString &modelCode) const {
    const QString namedScene = resolve(modelCode + QStringLiteral(".carbin"));
    if (!namedScene.isEmpty()) return namedScene;

    QString onlyScene;
    for (auto it = pathsByName_.cbegin(); it != pathsByName_.cend(); ++it) {
        if (!it.key().endsWith(QStringLiteral(".carbin"))) continue;
        if (!onlyScene.isEmpty() || it.value().size() != 1) return {};
        onlyScene = it.value().front();
    }
    if (!onlyScene.isEmpty()) return QDir(root_).filePath(onlyScene);

    const QString namedModel = resolve(modelCode + QStringLiteral(".modelbin"));
    if (!namedModel.isEmpty()) return namedModel;
    for (auto it = pathsByName_.cbegin(); it != pathsByName_.cend(); ++it) {
        if (!it.key().endsWith(QStringLiteral(".modelbin"))) continue;
        if (!onlyScene.isEmpty() || it.value().size() != 1) return {};
        onlyScene = it.value().front();
    }

    return onlyScene.isEmpty() ? QString() : QDir(root_).filePath(onlyScene);
}

QStringList carModelGames(const QString &preferredGame) {
    static const QStringList games = {
        QStringLiteral("FH6"), QStringLiteral("FM2023"), QStringLiteral("FH5"),
        QStringLiteral("FH4"), QStringLiteral("FM7"), QStringLiteral("FH3"),
        QStringLiteral("FM6"), QStringLiteral("FH2"), QStringLiteral("FM5"),
    };
    QStringList ordered = games;
    const QString preferred = preferredGame.trimmed().toUpper();

    if (ordered.removeOne(preferred)) ordered.prepend(preferred);

    return ordered;
}

QString gameMediaDir(const QString &gameFolder) {
    if (gameFolder.isEmpty()) {
        return QString();
    }
    const QDir root(gameFolder);
    if (root.exists(QStringLiteral("Cars")) || root.exists(QStringLiteral("pcfamily/Cars"))) {
        return root.absolutePath();
    }
    if (root.exists(QStringLiteral("media"))) {
        return root.filePath(QStringLiteral("media"));
    }

    return root.absolutePath();
}

QStringList gameCarsDirs(const QString &gameFolder) {
    const QDir media(gameMediaDir(gameFolder));
    QStringList directories;

    if (gameFolder.isEmpty()) return directories;
    for (const QString &relative : {QStringLiteral("Cars"), QStringLiteral("pcfamily/Cars")}) {
        const QString path = media.filePath(relative);
        if (QFileInfo(path).isDir()) directories.push_back(path);
    }

    return directories;
}

QString gameCarsDir(const QString &gameFolder) {
    const QStringList directories = gameCarsDirs(gameFolder);
    const QString media = gameMediaDir(gameFolder);

    if (!directories.isEmpty()) return directories.first();

    return media.isEmpty() ? QString() : QDir(media).filePath(QStringLiteral("Cars"));
}

QString findCarModelPath(const QString &carsFolder, const QString &modelCode) {
    const QDir directory(carsFolder);
    const auto modelInDirectory = [](const QString &path) -> QString {
        const QDir subdirectory(path);
        for (const QString &pattern : {QStringLiteral("*.carbin"), QStringLiteral("*.zip"), QStringLiteral("*.modelbin")}) {
            const QStringList matches = subdirectory.entryList({pattern}, QDir::Files);
            if (!matches.isEmpty()) return subdirectory.filePath(matches.first());
        }

        return {};
    };

    if (modelCode.isEmpty() || carsFolder.isEmpty() || !directory.exists()) return {};
    for (const QString &extension : kCarModelExtensions) {
        const QString candidate = directory.filePath(QStringLiteral("%1.%2").arg(modelCode, extension));
        if (QFileInfo(candidate).isFile()) return candidate;
    }

    QDirIterator entries(carsFolder, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        entries.next();
        const QFileInfo info = entries.fileInfo();
        if (info.isDir() && info.fileName().compare(modelCode, Qt::CaseInsensitive) == 0) {
            const QString path = modelInDirectory(info.filePath());
            if (!path.isEmpty()) return path;
        } else if (info.isFile() && info.completeBaseName().compare(modelCode, Qt::CaseInsensitive) == 0
                   && kCarModelExtensions.contains(info.suffix().toLower())) {
            return info.filePath();
        }
    }

    return {};
}

QSet<QString> availableCarModelCodes(const QString &gameFolder) {
    QSet<QString> models;

    for (const QString &directory : gameCarsDirs(gameFolder)) {
        const QString root = QDir(directory).absolutePath();
        QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const QFileInfo info = files.fileInfo();
            if (!kCarModelExtensions.contains(info.suffix().toLower())) continue;
            models.insert(info.completeBaseName().toLower());
            if (info.absolutePath() != root) models.insert(info.dir().dirName().toLower());
        }
    }

    return models;
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

} // namespace fls
