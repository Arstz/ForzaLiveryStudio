#include "main_window.h"
#include "theme_manager.h"
#include "image_io.h"
#include "car_model_renderer.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>

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
        } else if (gui::supportedImageSuffixes().contains(info.suffix().toLower())) {
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

    if (QCoreApplication::arguments().contains(QStringLiteral("--shadertest"))) {
        QOffscreenSurface surface;
        surface.create();
        QOpenGLContext context;
        QString result = QStringLiteral("no GL context");
        if (context.create() && context.makeCurrent(&surface)) {
            const QString log = gui::CarModelRenderer::shaderSelfTest();
            result = log.isEmpty() ? QStringLiteral("SHADER OK") : QStringLiteral("SHADER FAIL\n") + log;
        }
        QFile out(QStringLiteral("shader_selftest.txt"));
        if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            out.write(result.toUtf8());
        }
        return 0;
    }

    gui::MainWindow window;
    openStartupFiles(window, QCoreApplication::arguments().mid(1));
    window.show();
    return QApplication::exec();
}
