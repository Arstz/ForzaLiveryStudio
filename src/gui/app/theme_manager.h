#pragma once

#include "contour_data.h"

#include <QColor>
#include <QFont>
#include <QBrush>
#include <QPalette>
#include <QMap>
#include <QMargins>
#include <QSize>
#include <QString>
#include <QVector>

class QApplication;
class QIcon;
class QToolBar;
class QWidget;

namespace gui {

using UiTheme = QString;
inline const UiTheme kDefaultUiTheme = QStringLiteral("mahogany-dark");

inline constexpr int kUiCornerRadius = 5;
inline constexpr int kUiHighlightMargin = 1;
inline constexpr int kUiControlHeight = 27;
inline constexpr int kUiControlBorderWidth = 1;
inline constexpr int kUiToolbarButtonPadding = 3;
inline constexpr int kUiToolbarIconSpacing = 4;
inline constexpr int kUiCompactControlExtent = 22;
inline constexpr int kUiCollapsedDockExtent = kUiControlHeight + 2 * kUiHighlightMargin;
inline constexpr int kUiHeaderHeight = kUiCollapsedDockExtent + kUiControlBorderWidth;
inline constexpr int kUiSpacing = 6;
inline constexpr int kUiMargin = 10;
inline constexpr int kUiSplitterWidth = 4;
inline constexpr int kUiActionGlyphExtent = 10;
inline constexpr int kUiActionIconExtent = 12;
inline constexpr double kUiActionGlyphStrokeWidth = 1.5;

struct UiColors {
    QColor main;
    QColor secondary;
    QColor contrast;
    QColor accent;
    QColor focus;
    QColor border;
    QColor text;
    QColor mutedText;
    QColor disabledText;
    QColor hover;
    QColor selection;
    QColor warning;
    QColor error;
    QColor guide;
    QColor selectedText;
    QColor icon;
    QColor canvas;
    QColor preview;
    QColor checkerboard;
    QColor carPreview;
    QColor rulerBackground;
    QColor rulerDivider;
    QColor rulerText;
    QColor resizeHandle;
    QColor editorBackground;
    QColor buttonBackground;
    QColor toolbarBackground;
    QColor statusBackground;
    QColor tooltipBackground;
    QColor tooltipText;
    QColor browserBackground;
    QColor browserPreview;
    QColor separator;
    QColor panelBackground;
    QColor panelHeaderBackground;
    QColor navigationBackground;
    QColor popupBackground;
};

struct UiColorRole {
    QString id;
    QString label;
    QColor UiColors::*member;
    QColor UiColors::*fallback = nullptr;
};

struct UiThemeDefinition {
    UiTheme id;
    QString name;
    QMap<QString, QColor> colors;
    bool dark = true;
};

using ThemeColorOverrides = QMap<UiTheme, QMap<QString, QColor>>;

const QVector<UiThemeDefinition> &uiThemes();
const QVector<UiColorRole> &uiColorRoles();
ThemeColorOverrides themeColorOverrides();
void setThemeColorOverrides(const ThemeColorOverrides &overrides);
void saveThemeColorOverrides();
UiColors uiColors(UiTheme theme);
QFont uiTextFont();
QFont uiSecondaryTextFont();
int uiTextPixelSize(const QFont &font);
QMargins uiTextInsets(const QFont &font);
void applyUiToolbarMetrics(QToolBar &toolbar);
QIcon uiAddIcon();
QString colorSwatchStyle(const QColor &color);
void setUiState(QWidget *widget, const QString &state);

enum class CanvasColorMode {
    ThemeDefault,
    Custom,
};

struct CanvasColorPreference {
    QColor custom;
    CanvasColorMode mode = CanvasColorMode::ThemeDefault;
};

using CanvasColorSettings = QMap<UiTheme, CanvasColorPreference>;

struct TransformModeSettings {
    bool relativeMode = false;
};

enum class ContourFillMode {
    Analytic,
    CompactFit,
};

inline constexpr int kDefaultBucketTolerance = 16;
inline constexpr double kDefaultLiningOutsideMargin = 0.5;
inline constexpr bool kDefaultAutomaticSnapping = true;
inline constexpr bool kDefaultPenColorAwareSnapping = false;
inline constexpr bool kDefaultAutoAdjustNewPenHandles = true;
inline constexpr bool kDefaultHoverShapeOutline = true;

struct BehaviorSettings {
    bool insertShapeWithLastSelectedColor = true;
    bool insertShapeWithLastSelectedScale = false;
    ContourFillMode contourFillMode = ContourFillMode::Analytic;
    bool autoSavePenPaths = false;
    bool penAlwaysOnTop = true;
    bool penColorAwareSnapping = kDefaultPenColorAwareSnapping;
    bool autoAdjustNewPenHandles = kDefaultAutoAdjustNewPenHandles;
    bool autoSaveLiningPaths = false;
    int bucketTolerance = kDefaultBucketTolerance;
    double liningWidth = fls::scene::kDefaultLiningWidth;
    double liningOutsideMargin = kDefaultLiningOutsideMargin;
    bool showPropertyDebug = false;
    bool moveToolAutoSelect = false;
    bool automaticSnapping = kDefaultAutomaticSnapping;
    bool pipetteAutoReturn = true;
    bool selectionFlashEnabled = true;
    bool displayAnchorsDuringTransformDrag = true;
    bool generatePreviewsWithTransformations = false;
    bool guideLayersVisible = true;
    bool guideLayersOnTop = true;
    bool guidelinesVisible = true;
    bool guidelinesLocked = false;
    QColor guidelineColor = QColor(0, 170, 255);
    bool visibilityBordersEnabled = true;
    bool valueEditingWheelEnabled = true;
    QSize visibilityBorderResolution = QSize(1920, 1080);
    double nudgeStep = 0.1;
    double nudgeShiftStep = 1.0;
    int liveryTextureScale = 4;
    int autosaveIntervalMinutes = 5;
    QMap<QString, QString> gameFolders;
    bool discardModelOnLiveryOpen = true;
    bool loadCarTextures = false;
    bool pasteInPlace = false;
    bool verticalToolbar = false;
    bool separateSelectAndMarqueeTools = false;
    bool selectShapesByBoundingBox = false;
    bool showSelectedShapeOutlines = true;
    bool hoverShapeOutline = kDefaultHoverShapeOutline;
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
