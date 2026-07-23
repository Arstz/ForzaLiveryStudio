#include "main_window.h"
#include "theme_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>

namespace {

bool isProjectPath(const QString &path) {
    const QString suffix = QFileInfo(path).suffix();
    return suffix.compare(QStringLiteral("3so"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
}

void openStartupFiles(gui::MainWindow &window, const QStringList &paths) {
    QString projectPath;
    QStringList imagePaths;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isFile()) {
            qWarning().noquote() << QStringLiteral("Startup file does not exist: %1").arg(path);
            continue;
        }
        const QString absolutePath = info.absoluteFilePath();
        if (projectPath.isEmpty() && isProjectPath(absolutePath)) {
            projectPath = absolutePath;
        } else if (!QImageReader::imageFormat(absolutePath).isEmpty()) {
            imagePaths.push_back(absolutePath);
        } else {
            qWarning().noquote() << QStringLiteral("Unsupported startup file: %1").arg(absolutePath);
        }
    }

    QString error;
    if (!projectPath.isEmpty()) {
        if (!window.loadProjectJson(projectPath, &error)) {
            qWarning().noquote() << QStringLiteral("Could not open startup project %1: %2")
                                        .arg(projectPath, error);
            window.newProject();
        }
    } else {
        window.newProject();
    }

    for (const QString &imagePath : imagePaths) {
        error.clear();
        if (!window.importGuideLayer(imagePath, &error)) {
            qWarning().noquote() << QStringLiteral("Could not import startup image %1: %2")
                                        .arg(imagePath, error);
        }
    }
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForzaTools"));
    QCoreApplication::setApplicationName(QStringLiteral("ForzaLiveryStudio"));
    gui::applyUiTheme(app, gui::loadUiTheme());

    gui::MainWindow window;
    openStartupFiles(window, QCoreApplication::arguments().mid(1));
    window.show();
    return QApplication::exec();
}
