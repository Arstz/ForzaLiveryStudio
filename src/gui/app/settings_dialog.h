#pragma once

#include "differential_cover.h"
#include "system_integration.h"
#include "theme_manager.h"

#include <QDialog>
#include <QKeySequence>
#include <QVector>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace gui {

struct ShortcutSettingsItem {
    QString id;
    QString label;
    QKeySequence defaultSequence;
    QKeySequence currentSequence;
};

class SettingsDialog final : public QDialog {
public:
    SettingsDialog(UiTheme theme,
                   const CanvasColorSettings &canvasSettings,
                   const PreviewBackgroundSettings &previewBackgroundSettings,
                   const BehaviorSettings &behaviorSettings,
                   const cover::FillOptions &differentialFillOptions,
                   const QVector<ShortcutSettingsItem> &shortcuts,
                   QWidget *parent = nullptr);

    UiTheme selectedTheme() const;
    CanvasColorSettings selectedCanvasSettings() const;
    PreviewBackgroundSettings selectedPreviewBackgroundSettings() const;
    BehaviorSettings selectedBehaviorSettings() const;
    cover::FillOptions selectedDifferentialFillOptions() const;
    SystemIconSet selectedSystemIconSet() const;
    bool projectFileAssociationEnabled() const;
    QVector<ShortcutSettingsItem> shortcutItems() const;
    bool shortcutsAreValid();
    void setThemeChangedCallback(std::function<void(UiTheme)> callback);

private:
    void resetShortcutRow(int row);
    void resetAllShortcuts();
    void chooseCanvasColor(UiTheme theme);
    void choosePreviewBackgroundColor(bool buffer);
    void chooseGuidelineColor();
    void updateCanvasColorControls();
    void updatePreviewBackgroundControls();
    void accept() override;

    std::function<void(UiTheme)> themeChangedCallback_;
    CanvasColorSettings canvasSettings_;
    PreviewBackgroundSettings previewBackgroundSettings_;
    BehaviorSettings behaviorSettings_;
    cover::FillOptions differentialFillOptions_;
    QVector<ShortcutSettingsItem> shortcuts_;
    QComboBox *themeCombo_ = nullptr;
    QComboBox *darkCanvasMode_ = nullptr;
    QPushButton *darkCanvasColorButton_ = nullptr;
    QComboBox *lightCanvasMode_ = nullptr;
    QPushButton *lightCanvasColorButton_ = nullptr;
    QComboBox *bufferBackgroundMode_ = nullptr;
    QPushButton *bufferBackgroundColorButton_ = nullptr;
    QComboBox *layersBackgroundMode_ = nullptr;
    QPushButton *layersBackgroundColorButton_ = nullptr;
    QPushButton *guidelineColorButton_ = nullptr;
    QCheckBox *visibilityBordersCheck_ = nullptr;
    QCheckBox *displayAnchorsDuringTransformDrag_ = nullptr;
    QCheckBox *generatePreviewsWithTransformations_ = nullptr;
    QComboBox *visibilityBorderResolution_ = nullptr;
    QDoubleSpinBox *nudgeStep_ = nullptr;
    QDoubleSpinBox *nudgeShiftStep_ = nullptr;
    QSpinBox *liveryTextureScale_ = nullptr;
    QSpinBox *autosaveIntervalMinutes_ = nullptr;
    QLineEdit *gameFolder_ = nullptr;
    QCheckBox *discardModelOnLiveryOpen_ = nullptr;
    QCheckBox *loadCarTextures_ = nullptr;
    QSpinBox *differentialBudget_ = nullptr;
    QSpinBox *differentialAdamIterations_ = nullptr;
    QSpinBox *differentialRestarts_ = nullptr;
    QDoubleSpinBox *differentialSpillWeight_ = nullptr;
    QDoubleSpinBox *differentialEpsArea_ = nullptr;
    QDoubleSpinBox *differentialEpsGain_ = nullptr;
    QDoubleSpinBox *differentialEpsSpill_ = nullptr;
    QDoubleSpinBox *differentialAdamLearningRate_ = nullptr;
    QDoubleSpinBox *differentialInactivityTimeout_ = nullptr;
    QDoubleSpinBox *differentialBoundaryTolerance_ = nullptr;
    QDoubleSpinBox *differentialOutwardMargin_ = nullptr;
    QDoubleSpinBox *differentialAreaWindowRatio_ = nullptr;
    QDoubleSpinBox *differentialTargetCoverageRatio_ = nullptr;
    QDoubleSpinBox *differentialTverskyAlpha_ = nullptr;
    QDoubleSpinBox *differentialTverskyBeta_ = nullptr;
    QDoubleSpinBox *differentialFeatureWeight_ = nullptr;
    QSpinBox *differentialFeatureRestarts_ = nullptr;
    QSpinBox *differentialSeed_ = nullptr;
    QCheckBox *differentialUseRouter_ = nullptr;
    QCheckBox *differentialUseGpu_ = nullptr;
    QCheckBox *differentialUseWeightedContour_ = nullptr;
    QComboBox *toolbarViewCombo_ = nullptr;
    QCheckBox *separateOpacityAndSkewToolsCheck_ = nullptr;
    QCheckBox *valueEditingWheelCheck_ = nullptr;
    QComboBox *systemIconSetCombo_ = nullptr;
    QCheckBox *projectFileAssociationCheck_ = nullptr;
    QLabel *projectFileAssociationStatus_ = nullptr;
    QVector<QKeySequenceEdit *> shortcutEdits_;
    QLabel *validationLabel_ = nullptr;
};

} // namespace gui
