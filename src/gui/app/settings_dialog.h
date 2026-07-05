#pragma once

#include "theme_manager.h"

#include <QDialog>
#include <QKeySequence>
#include <QVector>

#include <functional>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QPushButton;
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
                   const QVector<ShortcutSettingsItem> &shortcuts,
                   QWidget *parent = nullptr);

    UiTheme selectedTheme() const;
    CanvasColorSettings selectedCanvasSettings() const;
    QVector<ShortcutSettingsItem> shortcutItems() const;
    bool shortcutsAreValid();
    void setThemeChangedCallback(std::function<void(UiTheme)> callback);

private:
    void resetShortcutRow(int row);
    void resetAllShortcuts();
    void chooseCanvasColor(UiTheme theme);
    void updateCanvasColorControls();
    void accept() override;

    UiTheme initialTheme_;
    std::function<void(UiTheme)> themeChangedCallback_;
    CanvasColorSettings canvasSettings_;
    QVector<ShortcutSettingsItem> shortcuts_;
    QComboBox *themeCombo_ = nullptr;
    QComboBox *darkCanvasMode_ = nullptr;
    QPushButton *darkCanvasColorButton_ = nullptr;
    QComboBox *lightCanvasMode_ = nullptr;
    QPushButton *lightCanvasColorButton_ = nullptr;
    QTableWidget *shortcutTable_ = nullptr;
    QLabel *validationLabel_ = nullptr;
};

} // namespace gui
