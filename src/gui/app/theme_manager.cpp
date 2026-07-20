#include "theme_manager.h"

#include <QApplication>
#include <QSettings>
#include <QStyleFactory>

#include <algorithm>

namespace gui {
namespace {

UiTheme currentTheme = UiTheme::Dark;

} // namespace

QString themeSettingsValue(UiTheme theme)
{
    return theme == UiTheme::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

UiTheme themeFromSettingsValue(const QString &value)
{
    return value.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0 ? UiTheme::Light : UiTheme::Dark;
}

UiTheme loadUiTheme()
{
    return themeFromSettingsValue(QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString());
}

void saveUiTheme(UiTheme theme)
{
    QSettings().setValue(QStringLiteral("ui/theme"), themeSettingsValue(theme));
}

QColor defaultCanvasColor(UiTheme theme)
{
    return theme == UiTheme::Light ? QColor(244, 245, 247) : QColor(24, 25, 28);
}

CanvasColorSettings loadCanvasColorSettings()
{
    QSettings settings;
    CanvasColorSettings result;
    result.darkMode = settings.value(QStringLiteral("ui/canvas/darkMode"), QStringLiteral("default")).toString() == QStringLiteral("custom")
        ? CanvasColorMode::Custom
        : CanvasColorMode::ThemeDefault;
    result.lightMode = settings.value(QStringLiteral("ui/canvas/lightMode"), QStringLiteral("default")).toString() == QStringLiteral("custom")
        ? CanvasColorMode::Custom
        : CanvasColorMode::ThemeDefault;

    const QColor legacy(settings.value(QStringLiteral("ui/canvasColor")).toString());
    const QColor dark(settings.value(QStringLiteral("ui/canvas/darkCustom"),
                                     legacy.isValid() ? legacy.name(QColor::HexRgb)
                                                      : defaultCanvasColor(UiTheme::Dark).name(QColor::HexRgb)).toString());
    const QColor light(settings.value(QStringLiteral("ui/canvas/lightCustom"),
                                      defaultCanvasColor(UiTheme::Light).name(QColor::HexRgb)).toString());
    result.darkCustom = dark.isValid() ? dark : defaultCanvasColor(UiTheme::Dark);
    result.lightCustom = light.isValid() ? light : defaultCanvasColor(UiTheme::Light);
    if (legacy.isValid() && !settings.contains(QStringLiteral("ui/canvas/darkMode"))) {
        result.darkMode = CanvasColorMode::Custom;
    }
    return result;
}

void saveCanvasColorSettings(const CanvasColorSettings &settings)
{
    QSettings qsettings;
    qsettings.remove(QStringLiteral("ui/canvasColor"));
    qsettings.setValue(QStringLiteral("ui/canvas/darkMode"), settings.darkMode == CanvasColorMode::Custom ? QStringLiteral("custom") : QStringLiteral("default"));
    qsettings.setValue(QStringLiteral("ui/canvas/lightMode"), settings.lightMode == CanvasColorMode::Custom ? QStringLiteral("custom") : QStringLiteral("default"));
    qsettings.setValue(QStringLiteral("ui/canvas/darkCustom"),
                       (settings.darkCustom.isValid() ? settings.darkCustom : defaultCanvasColor(UiTheme::Dark)).name(QColor::HexRgb));
    qsettings.setValue(QStringLiteral("ui/canvas/lightCustom"),
                       (settings.lightCustom.isValid() ? settings.lightCustom : defaultCanvasColor(UiTheme::Light)).name(QColor::HexRgb));
}

TransformModeSettings loadTransformModeSettings()
{
    TransformModeSettings result;
    result.relativeMode = QSettings().value(QStringLiteral("ui/transform/relativeModeOption"), false).toBool();
    return result;
}

void saveTransformModeSettings(const TransformModeSettings &settings)
{
    QSettings qsettings;
    qsettings.remove(QStringLiteral("ui/transform/relativeMode"));
    qsettings.setValue(QStringLiteral("ui/transform/relativeModeOption"), settings.relativeMode);
}

BehaviorSettings loadBehaviorSettings()
{
    QSettings settings;
    BehaviorSettings result;
    result.insertShapeWithLastSelectedColor = settings.value(QStringLiteral("ui/behavior/insertShapeWithLastSelectedColor"), true).toBool();
    result.insertShapeWithLastSelectedScale = settings.value(QStringLiteral("ui/behavior/insertShapeWithLastSelectedScale"), false).toBool();
    result.showPropertyDebug = settings.value(QStringLiteral("ui/behavior/showPropertyDebug"), false).toBool();
    result.moveToolAutoSelect = settings.value(QStringLiteral("ui/behavior/moveToolAutoSelect"), false).toBool();
    result.selectionFlashEnabled = settings.value(QStringLiteral("ui/behavior/selectionFlashEnabled"), true).toBool();
    result.displayAnchorsDuringTransformDrag = settings.value(QStringLiteral("ui/behavior/displayAnchorsDuringTransformDrag"), true).toBool();
    result.generatePreviewsWithTransformations = settings.value(QStringLiteral("ui/behavior/generatePreviewsWithTransformations"), false).toBool();
    result.guideLayersVisible = settings.value(QStringLiteral("ui/behavior/guideLayersVisible"), true).toBool();
    result.guideLayersOnTop = settings.value(QStringLiteral("ui/behavior/guideLayersOnTop"), true).toBool();
    result.guidelinesVisible = settings.value(QStringLiteral("ui/behavior/guidelinesVisible"), true).toBool();
    result.guidelinesLocked = settings.value(QStringLiteral("ui/behavior/guidelinesLocked"), false).toBool();
    result.guidelineColor = QColor(settings.value(QStringLiteral("ui/behavior/guidelineColor"),
                                                 QStringLiteral("#00aaff")).toString());
    if (!result.guidelineColor.isValid()) {
        result.guidelineColor = QColor(0, 170, 255);
    }
    result.visibilityBordersEnabled = settings.value(QStringLiteral("ui/behavior/visibilityBordersEnabled"), true).toBool();
    result.positionLimitBorderEnabled = settings.value(QStringLiteral("ui/behavior/positionLimitBorderEnabled"), false).toBool();
    result.valueEditingWheelEnabled = settings.value(QStringLiteral("ui/behavior/valueEditingWheelEnabled"), true).toBool();
    const QSize resolution = settings.value(QStringLiteral("ui/behavior/visibilityBorderResolution"), QSize(1920, 1080)).toSize();
    result.visibilityBorderResolution = resolution.isValid() ? resolution : QSize(1920, 1080);
    result.nudgeStep = settings.value(QStringLiteral("ui/behavior/nudgeStep"), 0.1).toDouble();
    result.nudgeShiftStep = settings.value(QStringLiteral("ui/behavior/nudgeShiftStep"), 1.0).toDouble();
    result.liveryTextureScale = settings.value(QStringLiteral("ui/behavior/liveryTextureScale"), 4).toInt();
    result.autosaveIntervalMinutes = settings.value(QStringLiteral("ui/behavior/autosaveIntervalMinutes"), 5).toInt();
    result.carModelsFolder = settings.value(QStringLiteral("ui/behavior/carModelsFolder")).toString();
    result.discardModelOnLiveryOpen = settings.value(QStringLiteral("ui/behavior/discardModelOnLiveryOpen"), true).toBool();
    result.loadCarTextures = settings.value(QStringLiteral("ui/behavior/loadCarTextures"), false).toBool();
    if (result.nudgeStep <= 0.0) {
        result.nudgeStep = 0.1;
    }
    if (result.nudgeShiftStep <= 0.0) {
        result.nudgeShiftStep = 1.0;
    }
    result.liveryTextureScale = std::clamp(result.liveryTextureScale, 1, 8);
    result.autosaveIntervalMinutes = std::clamp(result.autosaveIntervalMinutes, 0, 1440);
    return result;
}

void saveBehaviorSettings(const BehaviorSettings &settings)
{
    QSettings qsettings;
    qsettings.setValue(QStringLiteral("ui/behavior/insertShapeWithLastSelectedColor"), settings.insertShapeWithLastSelectedColor);
    qsettings.setValue(QStringLiteral("ui/behavior/insertShapeWithLastSelectedScale"), settings.insertShapeWithLastSelectedScale);
    qsettings.setValue(QStringLiteral("ui/behavior/showPropertyDebug"), settings.showPropertyDebug);
    qsettings.setValue(QStringLiteral("ui/behavior/moveToolAutoSelect"), settings.moveToolAutoSelect);
    qsettings.setValue(QStringLiteral("ui/behavior/selectionFlashEnabled"), settings.selectionFlashEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/displayAnchorsDuringTransformDrag"), settings.displayAnchorsDuringTransformDrag);
    qsettings.setValue(QStringLiteral("ui/behavior/generatePreviewsWithTransformations"), settings.generatePreviewsWithTransformations);
    qsettings.setValue(QStringLiteral("ui/behavior/guideLayersVisible"), settings.guideLayersVisible);
    qsettings.setValue(QStringLiteral("ui/behavior/guideLayersOnTop"), settings.guideLayersOnTop);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelinesVisible"), settings.guidelinesVisible);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelinesLocked"), settings.guidelinesLocked);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelineColor"),
                       (settings.guidelineColor.isValid() ? settings.guidelineColor : QColor(0, 170, 255)).name(QColor::HexArgb));
    qsettings.setValue(QStringLiteral("ui/behavior/visibilityBordersEnabled"), settings.visibilityBordersEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/positionLimitBorderEnabled"), settings.positionLimitBorderEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/valueEditingWheelEnabled"), settings.valueEditingWheelEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/visibilityBorderResolution"), settings.visibilityBorderResolution);
    qsettings.setValue(QStringLiteral("ui/behavior/nudgeStep"), settings.nudgeStep);
    qsettings.setValue(QStringLiteral("ui/behavior/nudgeShiftStep"), settings.nudgeShiftStep);
    qsettings.setValue(QStringLiteral("ui/behavior/liveryTextureScale"), std::clamp(settings.liveryTextureScale, 1, 8));
    qsettings.setValue(QStringLiteral("ui/behavior/autosaveIntervalMinutes"), std::clamp(settings.autosaveIntervalMinutes, 0, 1440));
    qsettings.setValue(QStringLiteral("ui/behavior/carModelsFolder"), settings.carModelsFolder);
    qsettings.setValue(QStringLiteral("ui/behavior/discardModelOnLiveryOpen"), settings.discardModelOnLiveryOpen);
    qsettings.setValue(QStringLiteral("ui/behavior/loadCarTextures"), settings.loadCarTextures);
}

QColor canvasColorForTheme(UiTheme theme, const CanvasColorSettings &settings)
{
    if (theme == UiTheme::Light) {
        return settings.lightMode == CanvasColorMode::Custom && settings.lightCustom.isValid()
            ? settings.lightCustom
            : defaultCanvasColor(UiTheme::Light);
    }
    return settings.darkMode == CanvasColorMode::Custom && settings.darkCustom.isValid()
        ? settings.darkCustom
        : defaultCanvasColor(UiTheme::Dark);
}

bool isDarkTheme(UiTheme theme)
{
    return theme == UiTheme::Dark;
}

QColor iconColorForTheme(UiTheme theme)
{
    return isDarkTheme(theme) ? QColor(238, 241, 245) : QColor(32, 34, 37);
}

QPalette paletteForTheme(UiTheme theme)
{
    QPalette palette;
    if (theme == UiTheme::Light) {
        palette.setColor(QPalette::Window, QColor(244, 245, 247));
        palette.setColor(QPalette::WindowText, QColor(28, 30, 33));
        palette.setColor(QPalette::Base, QColor(255, 255, 255));
        palette.setColor(QPalette::AlternateBase, QColor(235, 237, 240));
        palette.setColor(QPalette::ToolTipBase, QColor(255, 255, 255));
        palette.setColor(QPalette::ToolTipText, QColor(28, 30, 33));
        palette.setColor(QPalette::Text, QColor(28, 30, 33));
        palette.setColor(QPalette::Button, QColor(238, 240, 243));
        palette.setColor(QPalette::ButtonText, QColor(28, 30, 33));
        palette.setColor(QPalette::BrightText, QColor(255, 255, 255));
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        palette.setColor(QPalette::Link, QColor(0, 102, 204));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(128, 132, 138));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(128, 132, 138));
        return palette;
    }

    palette.setColor(QPalette::Window, QColor(32, 33, 36));
    palette.setColor(QPalette::WindowText, QColor(238, 241, 245));
    palette.setColor(QPalette::Base, QColor(24, 25, 28));
    palette.setColor(QPalette::AlternateBase, QColor(38, 40, 44));
    palette.setColor(QPalette::ToolTipBase, QColor(46, 48, 54));
    palette.setColor(QPalette::ToolTipText, QColor(238, 241, 245));
    palette.setColor(QPalette::Text, QColor(238, 241, 245));
    palette.setColor(QPalette::Button, QColor(43, 45, 50));
    palette.setColor(QPalette::ButtonText, QColor(238, 241, 245));
    palette.setColor(QPalette::BrightText, QColor(255, 255, 255));
    palette.setColor(QPalette::Highlight, QColor(72, 126, 176));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::Link, QColor(126, 180, 255));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(128, 132, 138));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(128, 132, 138));
    return palette;
}

void applyUiTheme(QApplication &app, UiTheme theme)
{
    currentTheme = theme;
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"))) {
        app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    app.setPalette(paletteForTheme(theme));
}

UiTheme currentUiTheme()
{
    return currentTheme;
}

} // namespace gui
