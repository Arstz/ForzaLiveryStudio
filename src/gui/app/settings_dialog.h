#pragma once

#include "system_integration.h"
#include "theme_manager.h"

#include <QDialog>
#include <QKeySequence>
#include <QMap>
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
                   const QVector<ShortcutSettingsItem> &shortcuts,
                   QWidget *parent = nullptr);

    UiTheme selectedTheme() const;
    CanvasColorSettings selectedCanvasSettings() const;
    ThemeColorOverrides selectedThemeColorOverrides() const;
    PreviewBackgroundSettings selectedPreviewBackgroundSettings() const;
    BehaviorSettings selectedBehaviorSettings() const;
    SystemIconSet selectedSystemIconSet() const;
    bool projectFileAssociationEnabled() const;
    QVector<ShortcutSettingsItem> shortcutItems() const;
    bool shortcutsAreValid();
    void setThemeChangedCallback(std::function<void(UiTheme)> callback);

private:
    void resetShortcutRow(int row);
    void resetAllShortcuts();
    void chooseCanvasColor();
    void chooseThemeColor();
    void updateThemeColorControls();
    void previewThemeColors();
    void choosePreviewBackgroundColor(bool buffer);
    void chooseGuidelineColor();
    void updateCanvasColorControls();
    void updatePreviewBackgroundControls();
    bool updateShortcutConflicts(bool showDialog);
    void accept() override;

    std::function<void(UiTheme)> themeChangedCallback_;
    CanvasColorSettings canvasSettings_;
    ThemeColorOverrides themeColorOverrides_;
    PreviewBackgroundSettings previewBackgroundSettings_;
    BehaviorSettings behaviorSettings_;
    QVector<ShortcutSettingsItem> shortcuts_;
    QComboBox *themeCombo_ = nullptr;
    QComboBox *canvasMode_ = nullptr;
    QPushButton *canvasColorButton_ = nullptr;
    QComboBox *themeColorRole_ = nullptr;
    QPushButton *themeColorButton_ = nullptr;
    QPushButton *resetThemeColorButton_ = nullptr;
    QPushButton *resetThemeColorsButton_ = nullptr;
    QComboBox *bufferBackgroundMode_ = nullptr;
    QPushButton *bufferBackgroundColorButton_ = nullptr;
    QComboBox *layersBackgroundMode_ = nullptr;
    QPushButton *layersBackgroundColorButton_ = nullptr;
    QPushButton *guidelineColorButton_ = nullptr;
    QCheckBox *visibilityBordersCheck_ = nullptr;
    QCheckBox *generatePreviewsWithTransformations_ = nullptr;
    QComboBox *visibilityBorderResolution_ = nullptr;
    QDoubleSpinBox *nudgeStep_ = nullptr;
    QDoubleSpinBox *nudgeShiftStep_ = nullptr;
    QSpinBox *liveryTextureScale_ = nullptr;
    QSpinBox *autosaveIntervalMinutes_ = nullptr;
    QMap<QString, QLineEdit *> gameFolders_;
    QCheckBox *discardModelOnLiveryOpen_ = nullptr;
    QCheckBox *loadCarTextures_ = nullptr;
    QCheckBox *pasteInPlace_ = nullptr;
    QComboBox *toolbarViewCombo_ = nullptr;
    QCheckBox *separateSelectAndMarqueeToolsCheck_ = nullptr;
    QCheckBox *separateOpacityAndSkewToolsCheck_ = nullptr;
    QCheckBox *valueEditingWheelCheck_ = nullptr;
    QComboBox *systemIconSetCombo_ = nullptr;
    QCheckBox *projectFileAssociationCheck_ = nullptr;
    QLabel *projectFileAssociationStatus_ = nullptr;
    QVector<QKeySequenceEdit *> shortcutEdits_;
    QLabel *validationLabel_ = nullptr;
};

} // namespace gui
