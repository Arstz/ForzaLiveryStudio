#include "theme_manager.h"

#include "differential_cover.h"

#include <QApplication>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QStyleFactory>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

UiTheme currentTheme = UiTheme::Dark;

QColor validColor(const QColor &color, const QColor &fallback) {
    return color.isValid() ? color : fallback;
}

PreviewBackgroundMode previewBackgroundMode(const QString &value) {
    if (value == QStringLiteral("checkerboard")) {
        return PreviewBackgroundMode::Checkerboard;
    }
    if (value == QStringLiteral("custom")) {
        return PreviewBackgroundMode::Custom;
    }
    return PreviewBackgroundMode::ThemeDefault;
}

QString previewBackgroundModeValue(PreviewBackgroundMode mode) {
    switch (mode) {
    case PreviewBackgroundMode::Checkerboard:
        return QStringLiteral("checkerboard");
    case PreviewBackgroundMode::Custom:
        return QStringLiteral("custom");
    case PreviewBackgroundMode::ThemeDefault:
        return QStringLiteral("default");
    }
    return QStringLiteral("default");
}

double boundedSetting(QSettings &settings,
                      const QString &key,
                      double fallback,
                      double minimum,
                      double maximum) {
    bool valid = false;
    const double value = settings.value(key, fallback).toDouble(&valid);
    if (!valid || !std::isfinite(value)) {
        return fallback;
    }

    return std::clamp(value, minimum, maximum);
}

int boundedSetting(QSettings &settings,
                   const QString &key,
                   int fallback,
                   int minimum,
                   int maximum) {
    bool valid = false;
    const int value = settings.value(key, fallback).toInt(&valid);
    if (!valid) {
        return fallback;
    }

    return std::clamp(value, minimum, maximum);
}

} // namespace

QString themeSettingsValue(UiTheme theme) {
    return theme == UiTheme::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

UiTheme themeFromSettingsValue(const QString &value) {
    return value.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0 ? UiTheme::Light : UiTheme::Dark;
}

UiTheme loadUiTheme() {
    return themeFromSettingsValue(QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString());
}

void saveUiTheme(UiTheme theme) {
    QSettings().setValue(QStringLiteral("ui/theme"), themeSettingsValue(theme));
}

QColor defaultCanvasColor(UiTheme theme) {
    return theme == UiTheme::Light ? QColor(244, 245, 247) : QColor(24, 25, 28);
}

CanvasColorSettings loadCanvasColorSettings() {
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
    result.darkCustom = validColor(dark, defaultCanvasColor(UiTheme::Dark));
    result.lightCustom = validColor(light, defaultCanvasColor(UiTheme::Light));
    if (legacy.isValid() && !settings.contains(QStringLiteral("ui/canvas/darkMode"))) {
        result.darkMode = CanvasColorMode::Custom;
    }
    return result;
}

void saveCanvasColorSettings(const CanvasColorSettings &settings) {
    QSettings qsettings;
    qsettings.remove(QStringLiteral("ui/canvasColor"));
    qsettings.setValue(QStringLiteral("ui/canvas/darkMode"), settings.darkMode == CanvasColorMode::Custom ? QStringLiteral("custom") : QStringLiteral("default"));
    qsettings.setValue(QStringLiteral("ui/canvas/lightMode"), settings.lightMode == CanvasColorMode::Custom ? QStringLiteral("custom") : QStringLiteral("default"));
    qsettings.setValue(QStringLiteral("ui/canvas/darkCustom"),
                       validColor(settings.darkCustom, defaultCanvasColor(UiTheme::Dark)).name(QColor::HexRgb));
    qsettings.setValue(QStringLiteral("ui/canvas/lightCustom"),
                       validColor(settings.lightCustom, defaultCanvasColor(UiTheme::Light)).name(QColor::HexRgb));
}

TransformModeSettings loadTransformModeSettings() {
    TransformModeSettings result;
    result.relativeMode = QSettings().value(QStringLiteral("ui/transform/relativeModeOption"), false).toBool();
    return result;
}

void saveTransformModeSettings(const TransformModeSettings &settings) {
    QSettings qsettings;
    qsettings.remove(QStringLiteral("ui/transform/relativeMode"));
    qsettings.setValue(QStringLiteral("ui/transform/relativeModeOption"), settings.relativeMode);
}

BehaviorSettings loadBehaviorSettings() {
    QSettings settings;
    const BehaviorSettings defaults;
    BehaviorSettings result = defaults;
    result.insertShapeWithLastSelectedColor = settings.value(QStringLiteral("ui/behavior/insertShapeWithLastSelectedColor"), result.insertShapeWithLastSelectedColor).toBool();
    result.insertShapeWithLastSelectedScale = settings.value(QStringLiteral("ui/behavior/insertShapeWithLastSelectedScale"), result.insertShapeWithLastSelectedScale).toBool();
    const QVariant legacyDifferentialFill = settings.value(
        QStringLiteral("ui/behavior/differentiablePenFill"),
        result.differentialContourFill);
    result.differentialContourFill = settings.value(
        QStringLiteral("ui/behavior/differentialContourFill"),
        legacyDifferentialFill).toBool();
    result.showPropertyDebug = settings.value(QStringLiteral("ui/behavior/showPropertyDebug"), result.showPropertyDebug).toBool();
    result.moveToolAutoSelect = settings.value(QStringLiteral("ui/behavior/moveToolAutoSelect"), result.moveToolAutoSelect).toBool();
    result.allowMoveOutsideBoundingBox = settings.value(
        QStringLiteral("ui/behavior/allowMoveOutsideBoundingBox"), result.allowMoveOutsideBoundingBox).toBool();
    result.pipetteAutoReturn = settings.value(
        QStringLiteral("ui/behavior/pipetteAutoReturn"), result.pipetteAutoReturn).toBool();
    result.selectionFlashEnabled = settings.value(QStringLiteral("ui/behavior/selectionFlashEnabled"), result.selectionFlashEnabled).toBool();
    result.displayAnchorsDuringTransformDrag = settings.value(QStringLiteral("ui/behavior/displayAnchorsDuringTransformDrag"), result.displayAnchorsDuringTransformDrag).toBool();
    result.generatePreviewsWithTransformations = settings.value(QStringLiteral("ui/behavior/generatePreviewsWithTransformations"), result.generatePreviewsWithTransformations).toBool();
    result.guideLayersVisible = settings.value(QStringLiteral("ui/behavior/guideLayersVisible"), result.guideLayersVisible).toBool();
    result.guideLayersOnTop = settings.value(QStringLiteral("ui/behavior/guideLayersOnTop"), result.guideLayersOnTop).toBool();
    result.guidelinesVisible = settings.value(QStringLiteral("ui/behavior/guidelinesVisible"), result.guidelinesVisible).toBool();
    result.guidelinesLocked = settings.value(QStringLiteral("ui/behavior/guidelinesLocked"), result.guidelinesLocked).toBool();
    result.guidelineColor = QColor(settings.value(QStringLiteral("ui/behavior/guidelineColor"),
                                                 result.guidelineColor.name(QColor::HexArgb)).toString());
    result.visibilityBordersEnabled = settings.value(QStringLiteral("ui/behavior/visibilityBordersEnabled"), result.visibilityBordersEnabled).toBool();
    result.valueEditingWheelEnabled = settings.value(QStringLiteral("ui/behavior/valueEditingWheelEnabled"), result.valueEditingWheelEnabled).toBool();
    const QSize resolution = settings.value(QStringLiteral("ui/behavior/visibilityBorderResolution"), result.visibilityBorderResolution).toSize();
    result.visibilityBorderResolution = resolution.isValid() ? resolution : defaults.visibilityBorderResolution;
    result.nudgeStep = settings.value(QStringLiteral("ui/behavior/nudgeStep"), result.nudgeStep).toDouble();
    result.nudgeShiftStep = settings.value(QStringLiteral("ui/behavior/nudgeShiftStep"), result.nudgeShiftStep).toDouble();
    result.liveryTextureScale = settings.value(QStringLiteral("ui/behavior/liveryTextureScale"), result.liveryTextureScale).toInt();
    result.autosaveIntervalMinutes = settings.value(QStringLiteral("ui/behavior/autosaveIntervalMinutes"), result.autosaveIntervalMinutes).toInt();
    if (settings.contains(QStringLiteral("ui/behavior/autosaveEnabled"))
        && !settings.value(QStringLiteral("ui/behavior/autosaveEnabled")).toBool()) {
        result.autosaveIntervalMinutes = 0;
    }
    result.gameFolder = settings.value(QStringLiteral("ui/behavior/gameFolder")).toString();
    result.discardModelOnLiveryOpen = settings.value(QStringLiteral("ui/behavior/discardModelOnLiveryOpen"), result.discardModelOnLiveryOpen).toBool();
    result.loadCarTextures = settings.value(QStringLiteral("ui/behavior/loadCarTextures"), result.loadCarTextures).toBool();
    result.verticalToolbar = settings.value(QStringLiteral("ui/behavior/verticalToolbar"), result.verticalToolbar).toBool();
    result.separateOpacityAndSkewTools = settings.value(
        QStringLiteral("ui/behavior/separateOpacityAndSkewTools"),
        result.separateOpacityAndSkewTools).toBool();
    result.guidelineColor = validColor(result.guidelineColor, defaults.guidelineColor);
    if (result.nudgeStep <= 0.0) {
        result.nudgeStep = defaults.nudgeStep;
    }
    if (result.nudgeShiftStep <= 0.0) {
        result.nudgeShiftStep = defaults.nudgeShiftStep;
    }
    result.liveryTextureScale = std::clamp(result.liveryTextureScale, 1, 8);
    result.autosaveIntervalMinutes = std::clamp(result.autosaveIntervalMinutes, 0, 1440);
    return result;
}

void saveBehaviorSettings(const BehaviorSettings &settings) {
    QSettings qsettings;
    const BehaviorSettings defaults;
    qsettings.setValue(QStringLiteral("ui/behavior/insertShapeWithLastSelectedColor"), settings.insertShapeWithLastSelectedColor);
    qsettings.setValue(QStringLiteral("ui/behavior/insertShapeWithLastSelectedScale"), settings.insertShapeWithLastSelectedScale);
    qsettings.setValue(QStringLiteral("ui/behavior/differentialContourFill"),
                       settings.differentialContourFill);
    qsettings.remove(QStringLiteral("ui/behavior/differentiablePenFill"));
    qsettings.setValue(QStringLiteral("ui/behavior/showPropertyDebug"), settings.showPropertyDebug);
    qsettings.setValue(QStringLiteral("ui/behavior/moveToolAutoSelect"), settings.moveToolAutoSelect);
    qsettings.setValue(QStringLiteral("ui/behavior/allowMoveOutsideBoundingBox"), settings.allowMoveOutsideBoundingBox);
    qsettings.setValue(QStringLiteral("ui/behavior/pipetteAutoReturn"), settings.pipetteAutoReturn);
    qsettings.setValue(QStringLiteral("ui/behavior/selectionFlashEnabled"), settings.selectionFlashEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/displayAnchorsDuringTransformDrag"), settings.displayAnchorsDuringTransformDrag);
    qsettings.setValue(QStringLiteral("ui/behavior/generatePreviewsWithTransformations"), settings.generatePreviewsWithTransformations);
    qsettings.setValue(QStringLiteral("ui/behavior/guideLayersVisible"), settings.guideLayersVisible);
    qsettings.setValue(QStringLiteral("ui/behavior/guideLayersOnTop"), settings.guideLayersOnTop);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelinesVisible"), settings.guidelinesVisible);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelinesLocked"), settings.guidelinesLocked);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelineColor"),
                       validColor(settings.guidelineColor, defaults.guidelineColor).name(QColor::HexArgb));
    qsettings.setValue(QStringLiteral("ui/behavior/visibilityBordersEnabled"), settings.visibilityBordersEnabled);
    qsettings.remove(QStringLiteral("ui/behavior/positionLimitBorderEnabled"));
    qsettings.setValue(QStringLiteral("ui/behavior/valueEditingWheelEnabled"), settings.valueEditingWheelEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/visibilityBorderResolution"), settings.visibilityBorderResolution);
    qsettings.setValue(QStringLiteral("ui/behavior/nudgeStep"), settings.nudgeStep);
    qsettings.setValue(QStringLiteral("ui/behavior/nudgeShiftStep"), settings.nudgeShiftStep);
    qsettings.setValue(QStringLiteral("ui/behavior/liveryTextureScale"), std::clamp(settings.liveryTextureScale, 1, 8));
    qsettings.setValue(QStringLiteral("ui/behavior/autosaveIntervalMinutes"), std::clamp(settings.autosaveIntervalMinutes, 0, 1440));
    qsettings.remove(QStringLiteral("ui/behavior/autosaveEnabled"));
    qsettings.setValue(QStringLiteral("ui/behavior/gameFolder"), settings.gameFolder);
    qsettings.setValue(QStringLiteral("ui/behavior/discardModelOnLiveryOpen"), settings.discardModelOnLiveryOpen);
    qsettings.setValue(QStringLiteral("ui/behavior/loadCarTextures"), settings.loadCarTextures);
    qsettings.setValue(QStringLiteral("ui/behavior/verticalToolbar"), settings.verticalToolbar);
    qsettings.setValue(QStringLiteral("ui/behavior/separateOpacityAndSkewTools"),
                       settings.separateOpacityAndSkewTools);
}

cover::FillOptions loadDifferentialFillOptions() {
    QSettings settings;
    cover::FillOptions result;
    result.budget = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/budget"), result.budget,
        cover::kMinimumBudget, cover::kMaximumBudget);
    result.adamIterations = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/adamIterations"),
        result.adamIterations, cover::kMinimumAdamIterations,
        cover::kMaximumAdamIterations);
    result.restarts = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/restarts"), result.restarts,
        cover::kMinimumRestarts, cover::kMaximumRestarts);
    result.spillWeight = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/spillWeight"),
        result.spillWeight, cover::kMinimumSpillWeight, cover::kMaximumSpillWeight);
    result.epsArea = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/epsArea"), result.epsArea,
        cover::kMinimumEpsArea, cover::kMaximumEpsArea);
    result.epsGain = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/epsGain"), result.epsGain,
        cover::kMinimumEpsGain, cover::kMaximumEpsGain);
    result.epsSpill = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/epsSpill"), result.epsSpill,
        cover::kMinimumEpsSpill, cover::kMaximumEpsSpill);
    result.adamLearningRate = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/adamLearningRate"),
        result.adamLearningRate, cover::kMinimumAdamLearningRate,
        cover::kMaximumAdamLearningRate);
    result.inactivityTimeoutSeconds = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/inactivityTimeoutSeconds"),
        result.inactivityTimeoutSeconds, cover::kMinimumInactivityTimeoutSeconds,
        cover::kMaximumInactivityTimeoutSeconds);
    result.boundaryTolerance = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/boundaryTolerance"),
        result.boundaryTolerance, cover::kMinimumBoundaryTolerance,
        cover::kMaximumBoundaryTolerance);
    result.outwardMargin = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/outwardMargin"),
        result.outwardMargin, cover::kMinimumOutwardMargin,
        cover::kMaximumOutwardMargin);
    result.areaWindowRatio = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/areaWindowRatio"),
        result.areaWindowRatio, cover::kMinimumAreaWindowRatio,
        cover::kMaximumAreaWindowRatio);
    result.targetCoverageRatio = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/targetCoverageRatio"),
        result.targetCoverageRatio, cover::kMinimumTargetCoverageRatio,
        cover::kMaximumTargetCoverageRatio);
    result.tverskyAlpha = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/tverskyAlpha"),
        result.tverskyAlpha, cover::kMinimumTverskyAlpha,
        cover::kMaximumTverskyAlpha);
    result.tverskyBeta = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/tverskyBeta"),
        result.tverskyBeta, cover::kMinimumTverskyBeta,
        cover::kMaximumTverskyBeta);
    result.featureWeight = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/featureWeight"),
        result.featureWeight, cover::kMinimumFeatureWeight,
        cover::kMaximumFeatureWeight);
    result.featureRestarts = boundedSetting(
        settings, QStringLiteral("ui/differentialFill/featureRestarts"),
        result.featureRestarts, cover::kMinimumFeatureRestarts,
        cover::kMaximumFeatureRestarts);
    result.seed = static_cast<std::uint64_t>(boundedSetting(
        settings, QStringLiteral("ui/differentialFill/seed"),
        cover::kMinimumSeed, cover::kMinimumSeed, cover::kMaximumSeed));
    result.useRouter = settings.value(
        QStringLiteral("ui/differentialFill/useRouter"), result.useRouter).toBool();
    result.useGpu = settings.value(
        QStringLiteral("ui/differentialFill/useGpu"), result.useGpu).toBool();
    result.useWeightedContour = settings.value(
        QStringLiteral("ui/differentialFill/useWeightedContour"), true).toBool();

    return result;
}

void saveDifferentialFillOptions(const cover::FillOptions &options) {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/differentialFill/budget"),
                      std::clamp(options.budget, cover::kMinimumBudget,
                                 cover::kMaximumBudget));
    settings.setValue(QStringLiteral("ui/differentialFill/adamIterations"),
                      std::clamp(options.adamIterations,
                                 cover::kMinimumAdamIterations,
                                 cover::kMaximumAdamIterations));
    settings.setValue(QStringLiteral("ui/differentialFill/restarts"),
                      std::clamp(options.restarts, cover::kMinimumRestarts,
                                 cover::kMaximumRestarts));
    settings.setValue(QStringLiteral("ui/differentialFill/spillWeight"),
                      std::clamp(options.spillWeight, cover::kMinimumSpillWeight,
                                 cover::kMaximumSpillWeight));
    settings.setValue(QStringLiteral("ui/differentialFill/epsArea"),
                      std::clamp(options.epsArea, cover::kMinimumEpsArea,
                                 cover::kMaximumEpsArea));
    settings.setValue(QStringLiteral("ui/differentialFill/epsGain"),
                      std::clamp(options.epsGain, cover::kMinimumEpsGain,
                                 cover::kMaximumEpsGain));
    settings.setValue(QStringLiteral("ui/differentialFill/epsSpill"),
                      std::clamp(options.epsSpill, cover::kMinimumEpsSpill,
                                 cover::kMaximumEpsSpill));
    settings.setValue(QStringLiteral("ui/differentialFill/adamLearningRate"),
                      std::clamp(options.adamLearningRate,
                                 cover::kMinimumAdamLearningRate,
                                 cover::kMaximumAdamLearningRate));
    settings.setValue(QStringLiteral("ui/differentialFill/inactivityTimeoutSeconds"),
                      std::clamp(options.inactivityTimeoutSeconds,
                                 cover::kMinimumInactivityTimeoutSeconds,
                                 cover::kMaximumInactivityTimeoutSeconds));
    settings.setValue(QStringLiteral("ui/differentialFill/boundaryTolerance"),
                      std::clamp(options.boundaryTolerance,
                                 cover::kMinimumBoundaryTolerance,
                                 cover::kMaximumBoundaryTolerance));
    settings.setValue(QStringLiteral("ui/differentialFill/outwardMargin"),
                      std::clamp(options.outwardMargin,
                                 cover::kMinimumOutwardMargin,
                                 cover::kMaximumOutwardMargin));
    settings.setValue(QStringLiteral("ui/differentialFill/areaWindowRatio"),
                      std::clamp(options.areaWindowRatio,
                                 cover::kMinimumAreaWindowRatio,
                                 cover::kMaximumAreaWindowRatio));
    settings.setValue(QStringLiteral("ui/differentialFill/targetCoverageRatio"),
                      std::clamp(options.targetCoverageRatio,
                                 cover::kMinimumTargetCoverageRatio,
                                 cover::kMaximumTargetCoverageRatio));
    settings.setValue(QStringLiteral("ui/differentialFill/tverskyAlpha"),
                      std::clamp(options.tverskyAlpha,
                                 cover::kMinimumTverskyAlpha,
                                 cover::kMaximumTverskyAlpha));
    settings.setValue(QStringLiteral("ui/differentialFill/tverskyBeta"),
                      std::clamp(options.tverskyBeta,
                                 cover::kMinimumTverskyBeta,
                                 cover::kMaximumTverskyBeta));
    settings.setValue(QStringLiteral("ui/differentialFill/featureWeight"),
                      std::clamp(options.featureWeight,
                                 cover::kMinimumFeatureWeight,
                                 cover::kMaximumFeatureWeight));
    settings.setValue(QStringLiteral("ui/differentialFill/featureRestarts"),
                      std::clamp(options.featureRestarts,
                                 cover::kMinimumFeatureRestarts,
                                 cover::kMaximumFeatureRestarts));
    const std::uint64_t boundedSeed = std::min<std::uint64_t>(
        options.seed, static_cast<std::uint64_t>(cover::kMaximumSeed));
    settings.setValue(QStringLiteral("ui/differentialFill/seed"),
                      static_cast<int>(boundedSeed));
    settings.setValue(QStringLiteral("ui/differentialFill/useRouter"), options.useRouter);
    settings.setValue(QStringLiteral("ui/differentialFill/useGpu"), options.useGpu);
    settings.setValue(QStringLiteral("ui/differentialFill/useWeightedContour"),
                      options.useWeightedContour);
}

PreviewBackgroundSettings loadPreviewBackgroundSettings() {
    QSettings settings;
    PreviewBackgroundSettings result;
    result.buffer.mode = previewBackgroundMode(
        settings.value(QStringLiteral("ui/previews/bufferMode"), QStringLiteral("default")).toString());
    result.layers.mode = previewBackgroundMode(
        settings.value(QStringLiteral("ui/previews/layersMode"), QStringLiteral("default")).toString());
    result.buffer.custom = QColor(settings.value(
        QStringLiteral("ui/previews/bufferCustom"), defaultCanvasColor(UiTheme::Dark).name()).toString());
    result.layers.custom = QColor(settings.value(
        QStringLiteral("ui/previews/layersCustom"), defaultCanvasColor(UiTheme::Dark).name()).toString());
    result.buffer.custom = validColor(result.buffer.custom, defaultCanvasColor(UiTheme::Dark));
    result.layers.custom = validColor(result.layers.custom, defaultCanvasColor(UiTheme::Dark));

    return result;
}

void savePreviewBackgroundSettings(const PreviewBackgroundSettings &settings) {
    QSettings qsettings;
    qsettings.setValue(QStringLiteral("ui/previews/bufferMode"),
                       previewBackgroundModeValue(settings.buffer.mode));
    qsettings.setValue(QStringLiteral("ui/previews/layersMode"),
                       previewBackgroundModeValue(settings.layers.mode));
    qsettings.setValue(QStringLiteral("ui/previews/bufferCustom"),
                       validColor(settings.buffer.custom, defaultCanvasColor(UiTheme::Dark)).name(QColor::HexRgb));
    qsettings.setValue(QStringLiteral("ui/previews/layersCustom"),
                       validColor(settings.layers.custom, defaultCanvasColor(UiTheme::Dark)).name(QColor::HexRgb));
}

QColor defaultPreviewBackgroundColor(UiTheme theme) {
    return paletteForTheme(theme).color(QPalette::Base);
}

QColor previewBackgroundColor(UiTheme theme, const PreviewBackground &background) {
    if (background.mode == PreviewBackgroundMode::Custom && background.custom.isValid()) {
        return background.custom;
    }

    return defaultPreviewBackgroundColor(theme);
}

QBrush previewBackgroundBrush(UiTheme theme, const PreviewBackground &background) {
    if (background.mode != PreviewBackgroundMode::Checkerboard) {
        return QBrush(previewBackgroundColor(theme, background));
    }
    constexpr int kCheckerExtent = 8;
    constexpr int kCheckerTileExtent = kCheckerExtent * 2;
    const QColor base = defaultPreviewBackgroundColor(theme);
    const QColor alternate = isDarkTheme(theme) ? base.lighter(145) : base.darker(112);
    QImage tile(kCheckerTileExtent, kCheckerTileExtent, QImage::Format_RGB32);
    tile.fill(base);
    QPainter painter(&tile);
    painter.fillRect(0, 0, kCheckerExtent, kCheckerExtent, alternate);
    painter.fillRect(kCheckerExtent, kCheckerExtent, kCheckerExtent, kCheckerExtent, alternate);

    return QBrush(QPixmap::fromImage(tile));
}

QColor canvasColorForTheme(UiTheme theme, const CanvasColorSettings &settings) {
    const CanvasColorMode mode = theme == UiTheme::Light ? settings.lightMode : settings.darkMode;
    const QColor custom = theme == UiTheme::Light ? settings.lightCustom : settings.darkCustom;

    return mode == CanvasColorMode::Custom && custom.isValid() ? custom : defaultCanvasColor(theme);
}

bool isDarkTheme(UiTheme theme) {
    return theme == UiTheme::Dark;
}

QColor iconColorForTheme(UiTheme theme) {
    return isDarkTheme(theme) ? QColor(238, 241, 245) : QColor(32, 34, 37);
}

QPalette paletteForTheme(UiTheme theme) {
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
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::Link, QColor(0, 102, 204));
    } else {
        palette.setColor(QPalette::Window, QColor(32, 33, 36));
        palette.setColor(QPalette::WindowText, QColor(238, 241, 245));
        palette.setColor(QPalette::Base, QColor(24, 25, 28));
        palette.setColor(QPalette::AlternateBase, QColor(38, 40, 44));
        palette.setColor(QPalette::ToolTipBase, QColor(46, 48, 54));
        palette.setColor(QPalette::ToolTipText, QColor(238, 241, 245));
        palette.setColor(QPalette::Text, QColor(238, 241, 245));
        palette.setColor(QPalette::Button, QColor(43, 45, 50));
        palette.setColor(QPalette::ButtonText, QColor(238, 241, 245));
        palette.setColor(QPalette::Highlight, QColor(72, 126, 176));
        palette.setColor(QPalette::Link, QColor(126, 180, 255));
    }
    palette.setColor(QPalette::BrightText, QColor(255, 255, 255));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(128, 132, 138));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(128, 132, 138));

    return palette;
}

void applyUiTheme(QApplication &app, UiTheme theme) {
    currentTheme = theme;
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"))) {
        app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    app.setPalette(paletteForTheme(theme));
}

UiTheme currentUiTheme() {
    return currentTheme;
}

} // namespace gui
