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
        require(!gui::loadBehaviorSettings().autoSavePenPaths);
        require(!gui::loadBehaviorSettings().autoSaveLiningPaths);
        settings.setValue(QStringLiteral("ui/behavior/autoSavePaths"), true);
        settings.sync();
        require(gui::loadBehaviorSettings().autoSavePenPaths && gui::loadBehaviorSettings().autoSaveLiningPaths);
        settings.remove(QStringLiteral("ui/behavior/autoSavePaths"));
        settings.sync();
        require(!gui::loadBehaviorSettings().separateSelectAndMarqueeTools);
        require(!gui::loadBehaviorSettings().selectShapesByBoundingBox);
        require(gui::loadBehaviorSettings().showSelectedShapeOutlines);
        for (int flags = 0; flags < 8; ++flags) {
            auto behavior = gui::loadBehaviorSettings();
            behavior.separateSelectAndMarqueeTools = (flags & 1) != 0;
            behavior.selectShapesByBoundingBox = (flags & 2) != 0;
            behavior.showSelectedShapeOutlines = (flags & 4) != 0;
            gui::saveBehaviorSettings(behavior);
            const auto restored = gui::loadBehaviorSettings();
            require(restored.separateSelectAndMarqueeTools == behavior.separateSelectAndMarqueeTools);
            require(restored.selectShapesByBoundingBox == behavior.selectShapesByBoundingBox);
            require(restored.showSelectedShapeOutlines == behavior.showSelectedShapeOutlines);
        }
        for (bool enabled : {true, false}) {
            auto behavior = gui::loadBehaviorSettings();
            behavior.autoSavePenPaths = enabled;
            behavior.autoSaveLiningPaths = !enabled;
            gui::saveBehaviorSettings(behavior);
            require(gui::loadBehaviorSettings().autoSavePenPaths == enabled);
            require(gui::loadBehaviorSettings().autoSaveLiningPaths != enabled);
        }
        settings.setValue(QStringLiteral("ui/behavior/contourFillMode"), QStringLiteral("catalog"));
        settings.sync();
        require(gui::loadBehaviorSettings().contourFillMode == gui::ContourFillMode::CompactFit);
        settings.setValue(QStringLiteral("ui/behavior/contourFillMode"), QStringLiteral("thin"));
        settings.sync();
        require(gui::loadBehaviorSettings().contourFillMode == gui::ContourFillMode::CompactFit);
        settings.setValue(QStringLiteral("ui/behavior/contourFillMode"), QStringLiteral("differential"));
        settings.sync();
        require(gui::loadBehaviorSettings().contourFillMode == gui::ContourFillMode::Analytic);
        settings.remove(QStringLiteral("ui/behavior/contourFillMode"));
        settings.setValue(QStringLiteral("ui/behavior/differentialContourFill"), true);
        settings.setValue(QStringLiteral("ui/behavior/differentiablePenFill"), true);
        settings.setValue(QStringLiteral("ui/behavior/liningFillMode"), 2);
        settings.setValue(QStringLiteral("ui/behavior/bucketRegionMode"), 1);
        settings.sync();
        require(gui::loadBehaviorSettings().contourFillMode == gui::ContourFillMode::Analytic);
        for (auto mode : {gui::ContourFillMode::Analytic, gui::ContourFillMode::CompactFit}) {
            auto behavior = gui::loadBehaviorSettings();
            behavior.contourFillMode = mode;
            gui::saveBehaviorSettings(behavior);
            require(gui::loadBehaviorSettings().contourFillMode == mode);
        }
        settings.sync();
        require(settings.value(QStringLiteral("ui/behavior/contourFillMode")).toString() == QStringLiteral("compact"));
        require(!settings.contains(QStringLiteral("ui/behavior/differentialContourFill")));
        require(!settings.contains(QStringLiteral("ui/behavior/differentiablePenFill")));
        require(!settings.contains(QStringLiteral("ui/behavior/liningFillMode")));
        require(!settings.contains(QStringLiteral("ui/behavior/bucketRegionMode")));
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "Removed-mode migration and contour fill round trips passed\n";
    return 0;
}
