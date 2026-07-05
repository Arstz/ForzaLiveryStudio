#include "theme_manager.h"

#include <QApplication>
#include <QSettings>
#include <QStyleFactory>

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
