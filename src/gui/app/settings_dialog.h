#pragma once

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
class QTableWidget;

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
                   const BehaviorSettings &behaviorSettings,
                   const QVector<ShortcutSettingsItem> &shortcuts,
                   QWidget *parent = nullptr);

    UiTheme selectedTheme() const;
    CanvasColorSettings selectedCanvasSettings() const;
    BehaviorSettings selectedBehaviorSettings() const;
    QVector<ShortcutSettingsItem> shortcutItems() const;
    bool shortcutsAreValid();
    void setThemeChangedCallback(std::function<void(UiTheme)> callback);

private:
    void resetShortcutRow(int row);
    void resetAllShortcuts();
    void chooseCanvasColor(UiTheme theme);
    void chooseGuidelineColor();
    void updateCanvasColorControls();
    void accept() override;

    UiTheme initialTheme_;
    std::function<void(UiTheme)> themeChangedCallback_;
    CanvasColorSettings canvasSettings_;
    BehaviorSettings behaviorSettings_;
    QVector<ShortcutSettingsItem> shortcuts_;
    QComboBox *themeCombo_ = nullptr;
    QComboBox *darkCanvasMode_ = nullptr;
    QPushButton *darkCanvasColorButton_ = nullptr;
    QComboBox *lightCanvasMode_ = nullptr;
    QPushButton *lightCanvasColorButton_ = nullptr;
    QPushButton *guidelineColorButton_ = nullptr;
    QCheckBox *visibilityBordersCheck_ = nullptr;
    QCheckBox *positionLimitBorderCheck_ = nullptr;
    QCheckBox *displayAnchorsDuringTransformDrag_ = nullptr;
    QCheckBox *generatePreviewsWithTransformations_ = nullptr;
    QComboBox *visibilityBorderResolution_ = nullptr;
    QDoubleSpinBox *nudgeStep_ = nullptr;
    QDoubleSpinBox *nudgeShiftStep_ = nullptr;
    QSpinBox *liveryTextureScale_ = nullptr;
    QSpinBox *autosaveIntervalMinutes_ = nullptr;
    QLineEdit *carModelsFolder_ = nullptr;
    QCheckBox *discardModelOnLiveryOpen_ = nullptr;
    QCheckBox *loadCarTextures_ = nullptr;
    QCheckBox *valueEditingWheelCheck_ = nullptr;
    QTableWidget *shortcutTable_ = nullptr;
    QLabel *validationLabel_ = nullptr;
};

} // namespace gui
