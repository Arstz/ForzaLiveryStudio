#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

class QApplication;

namespace gui {

enum class UiTheme {
    Dark,
    Light,
};

enum class CanvasColorMode {
    ThemeDefault,
    Custom,
};

struct CanvasColorSettings {
    CanvasColorMode darkMode = CanvasColorMode::ThemeDefault;
    CanvasColorMode lightMode = CanvasColorMode::ThemeDefault;
    QColor darkCustom;
    QColor lightCustom;
};

struct TransformModeSettings {
    bool relativeMode = false; // false = Absolute (world-axis box), true = Relative (box follows shape)
};

struct BehaviorSettings {
    bool insertShapeWithLastSelectedColor = true;
    bool insertShapeWithLastSelectedScale = false;
    bool showPropertyDebug = false;
    bool moveToolAutoSelect = false;
    bool selectionFlashEnabled = true;
};

QString themeSettingsValue(UiTheme theme);
UiTheme themeFromSettingsValue(const QString &value);
UiTheme loadUiTheme();
void saveUiTheme(UiTheme theme);
QColor defaultCanvasColor(UiTheme theme);
CanvasColorSettings loadCanvasColorSettings();
void saveCanvasColorSettings(const CanvasColorSettings &settings);
TransformModeSettings loadTransformModeSettings();
void saveTransformModeSettings(const TransformModeSettings &settings);
BehaviorSettings loadBehaviorSettings();
void saveBehaviorSettings(const BehaviorSettings &settings);
QColor canvasColorForTheme(UiTheme theme, const CanvasColorSettings &settings);
QPalette paletteForTheme(UiTheme theme);
QColor iconColorForTheme(UiTheme theme);
void applyUiTheme(QApplication &app, UiTheme theme);
UiTheme currentUiTheme();
bool isDarkTheme(UiTheme theme);

} // namespace gui
