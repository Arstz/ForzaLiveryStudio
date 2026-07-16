#include "import_locations.h"

#include <QtCore>
#include <QtWidgets>

namespace gui {

QString mostRecentContainersRoot()
{
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

QString importDialogStartDirectory(QWidget *parent, const QString &actionKey)
{
    QSettings settings;
    const QString key = actionKey.isEmpty()
        ? QStringLiteral("import/defaultDirectory")
        : QStringLiteral("import/%1Directory").arg(actionKey);
    const QString configured = settings.value(key).toString();
    if (!configured.isEmpty() && QFileInfo(configured).isDir()) {
        return configured;
    }
    const QString legacy = settings.value(QStringLiteral("import/defaultDirectory")).toString();
    if (!legacy.isEmpty() && QFileInfo(legacy).isDir()) {
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

QString importDialogStartDirectory(QWidget *parent)
{
    QSettings settings;
    const QString configured = settings.value(QStringLiteral("import/defaultDirectory")).toString();
    if (!configured.isEmpty() && QFileInfo(configured).isDir()) {
        return configured;
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
        settings.setValue(QStringLiteral("import/defaultDirectory"), selected);
    }
    return selected;
}

QString importBrowserStartDirectory(const QString &actionKey, const QStringList &fallbackActionKeys)
{
    QSettings settings;
    const auto configuredDirectory = [&](const QString &key) {
        const QString path = settings.value(QStringLiteral("import/%1Directory").arg(key)).toString();
        return !path.isEmpty() && QFileInfo(path).isDir() ? path : QString();
    };

    const QString current = configuredDirectory(actionKey);
    if (!current.isEmpty()) {
        return current;
    }
    for (const QString &fallbackKey : fallbackActionKeys) {
        const QString fallback = configuredDirectory(fallbackKey);
        if (!fallback.isEmpty()) {
            return fallback;
        }
    }

    const QString legacy = settings.value(QStringLiteral("import/defaultDirectory")).toString();
    if (!legacy.isEmpty() && QFileInfo(legacy).isDir()) {
        return legacy;
    }
    const QString detected = mostRecentContainersRoot();
    return detected.isEmpty() ? QDir::homePath() : detected;
}

void rememberImportDirectory(const QString &path)
{
    const QFileInfo info(path);
    const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (!folder.isEmpty()) {
        QSettings().setValue(QStringLiteral("import/defaultDirectory"), folder);
    }
}

void rememberImportDirectory(const QString &path, const QString &actionKey)
{
    const QFileInfo info(path);
    const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (!folder.isEmpty()) {
        const QString key = actionKey.isEmpty()
            ? QStringLiteral("import/defaultDirectory")
            : QStringLiteral("import/%1Directory").arg(actionKey);
        QSettings().setValue(key, folder);
    }
}

} // namespace gui
