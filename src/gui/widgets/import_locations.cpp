#include "import_locations.h"

#include <QtCore>
#include <QtWidgets>

namespace gui {
namespace {

QString importDirectoryKey(const QString &actionKey) {
    return actionKey.isEmpty()
        ? QStringLiteral("import/defaultDirectory")
        : QStringLiteral("import/%1Directory").arg(actionKey);
}

QString configuredDirectory(const QSettings &settings, const QString &key) {
    const QString path = settings.value(key).toString();
    return !path.isEmpty() && QFileInfo(path).isDir() ? path : QString();
}

QString folderForPath(const QString &path) {
    const QFileInfo info(path);
    return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

}

QString mostRecentContainersRoot() {
    const QDir pgsDir(QStringLiteral("C:/XboxGames/GameSave/pgs"));
    if (!pgsDir.exists()) {
        return {};
    }

    QFileInfo best;
    const QFileInfoList profiles = pgsDir.entryInfoList(QStringList{QStringLiteral("u_*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &profile : profiles) {
        const QFileInfo candidate(QDir(profile.absoluteFilePath()).filePath(QStringLiteral("current/ContainersRoot")));
        if (!candidate.isDir()) {
            continue;
        }
        if (!best.exists() || candidate.lastModified() > best.lastModified()) {
            best = candidate;
        }
    }
    return best.exists() ? best.absoluteFilePath() : QString();
}

QString importDialogStartDirectory(QWidget *parent, const QString &actionKey) {
    QSettings settings;
    const QString key = importDirectoryKey(actionKey);
    const QString configured = configuredDirectory(settings, key);
    if (!configured.isEmpty()) {
        return configured;
    }
    const QString legacy = configuredDirectory(settings, importDirectoryKey({}));
    if (!legacy.isEmpty()) {
        return legacy;
    }
    const QString detected = mostRecentContainersRoot();
    if (!detected.isEmpty()) {
        return detected;
    }

    const QString selected = QFileDialog::getExistingDirectory(
        parent,
        QStringLiteral("Select Game Save ContainersRoot Folder"),
        QStringLiteral("C:/XboxGames/GameSave/pgs"));
    if (!selected.isEmpty()) {
        settings.setValue(key, selected);
    }
    return selected;
}

QString importDialogStartDirectory(QWidget *parent) {
    return importDialogStartDirectory(parent, {});
}

QString importBrowserStartDirectory(const QString &actionKey, const QStringList &fallbackActionKeys) {
    QSettings settings;
    const QString current = configuredDirectory(settings, importDirectoryKey(actionKey));
    if (!current.isEmpty()) {
        return current;
    }
    for (const QString &fallbackKey : fallbackActionKeys) {
        const QString fallback = configuredDirectory(settings, importDirectoryKey(fallbackKey));
        if (!fallback.isEmpty()) {
            return fallback;
        }
    }

    const QString legacy = configuredDirectory(settings, importDirectoryKey({}));
    if (!legacy.isEmpty()) {
        return legacy;
    }
    const QString detected = mostRecentContainersRoot();
    return detected.isEmpty() ? QDir::homePath() : detected;
}

void rememberImportDirectory(const QString &path) {
    rememberImportDirectory(path, {});
}

void rememberImportDirectory(const QString &path, const QString &actionKey) {
    const QString folder = folderForPath(path);
    if (!folder.isEmpty()) {
        QSettings().setValue(importDirectoryKey(actionKey), folder);
    }
}

} // namespace gui
