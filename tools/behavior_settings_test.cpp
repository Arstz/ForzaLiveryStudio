#include "theme_manager.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <stdexcept>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForzaLiveryStudioTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ContourFillSettings"));
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path());
    try {
        QSettings settings;
        const auto require = [](bool condition) { if (!condition) { throw std::runtime_error("Contour fill settings mismatch"); } };
        require(gui::loadBehaviorSettings().contourFillMode == gui::ContourFillMode::Analytic);
        settings.setValue(QStringLiteral("ui/behavior/contourFillMode"), QStringLiteral("catalog"));
        settings.sync();
        require(gui::loadBehaviorSettings().contourFillMode == gui::ContourFillMode::CompactFit);
        for (auto mode : {gui::ContourFillMode::Analytic, gui::ContourFillMode::Differential, gui::ContourFillMode::CompactFit}) {
            auto behavior = gui::loadBehaviorSettings();
            behavior.contourFillMode = mode;
            gui::saveBehaviorSettings(behavior);
            require(gui::loadBehaviorSettings().contourFillMode == mode);
        }
        settings.sync();
        require(settings.value(QStringLiteral("ui/behavior/contourFillMode")).toString() == QStringLiteral("compact"));
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "Removed-mode migration and contour fill round trips passed\n";
    return 0;
}
