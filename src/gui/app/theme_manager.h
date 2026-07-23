#pragma once

#include <QColor>
#include <QBrush>
#include <QPalette>
#include <QSize>
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
    bool relativeMode = false;
};

struct BehaviorSettings {
    bool insertShapeWithLastSelectedColor = true;
    bool insertShapeWithLastSelectedScale = false;
    bool showPropertyDebug = false;
    bool moveToolAutoSelect = false;
    bool allowMoveOutsideBoundingBox = true;
    bool selectionFlashEnabled = true;
    bool displayAnchorsDuringTransformDrag = true;
    bool generatePreviewsWithTransformations = false;
    bool guideLayersVisible = true;
    bool guideLayersOnTop = true;
    bool guidelinesVisible = true;
    bool guidelinesLocked = false;
    QColor guidelineColor = QColor(0, 170, 255);
    bool visibilityBordersEnabled = true;
    bool positionLimitBorderEnabled = false;
    bool valueEditingWheelEnabled = true;
    QSize visibilityBorderResolution = QSize(1920, 1080);
    double nudgeStep = 0.1;
    double nudgeShiftStep = 1.0;
    int liveryTextureScale = 4;
    int autosaveIntervalMinutes = 5;
    QString carModelsFolder;
    bool discardModelOnLiveryOpen = true;
    bool loadCarTextures = false;
    bool verticalToolbar = false;
    bool separateOpacityAndSkewTools = false;
};

enum class PreviewBackgroundMode {
    ThemeDefault,
    Checkerboard,
    Custom,
};

struct PreviewBackground {
    PreviewBackgroundMode mode = PreviewBackgroundMode::ThemeDefault;
    QColor custom;
};

struct PreviewBackgroundSettings {
    PreviewBackground buffer;
    PreviewBackground layers;
};

QString themeSettingsValue(UiTheme theme);
UiTheme themeFromSettingsValue(const QString &value);
UiTheme loadUiTheme();
void saveUiTheme(UiTheme theme);
QColor defaultCanvasColor(UiTheme theme);
CanvasColorSettings loadCanvasColorSettings();
void saveCanvasColorSettings(const CanvasColorSettings &settings);
PreviewBackgroundSettings loadPreviewBackgroundSettings();
void savePreviewBackgroundSettings(const PreviewBackgroundSettings &settings);
QColor defaultPreviewBackgroundColor(UiTheme theme);
QColor previewBackgroundColor(UiTheme theme, const PreviewBackground &background);
QBrush previewBackgroundBrush(UiTheme theme, const PreviewBackground &background);
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
