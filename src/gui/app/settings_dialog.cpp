#include "settings_dialog.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <algorithm>

namespace gui {
namespace {

class ShortcutSequenceEdit final : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;

protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) {
                if (event->type() == QEvent::KeyPress) {
                    Qt::KeyboardModifiers modifiers = keyEvent->modifiers();
                    if (keyEvent->key() == Qt::Key_Backtab) {
                        modifiers |= Qt::ShiftModifier;
                    }
                    setKeySequence(QKeySequence(modifiers | Qt::Key_Tab));
                }
                event->accept();
                return true;
            }
        }
        return QKeySequenceEdit::event(event);
    }
};

} // namespace

SettingsDialog::SettingsDialog(UiTheme theme,
                               const CanvasColorSettings &canvasSettings,
                               const BehaviorSettings &behaviorSettings,
                               const QVector<ShortcutSettingsItem> &shortcuts,
                               QWidget *parent)
    : QDialog(parent)
    , canvasSettings_(canvasSettings)
    , behaviorSettings_(behaviorSettings)
    , shortcuts_(shortcuts) {
    setWindowTitle(QStringLiteral("Settings"));
    resize(620, 420);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);

    auto *general = new QWidget(tabs);
    auto *generalLayout = new QFormLayout(general);
    themeCombo_ = new QComboBox(general);
    themeCombo_->addItem(QStringLiteral("Dark"), themeSettingsValue(UiTheme::Dark));
    themeCombo_->addItem(QStringLiteral("Light"), themeSettingsValue(UiTheme::Light));
    themeCombo_->setCurrentIndex(theme == UiTheme::Light ? 1 : 0);
    QObject::connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        if (themeChangedCallback_) {
            themeChangedCallback_(selectedTheme());
        }
    });
    generalLayout->addRow(QStringLiteral("Theme"), themeCombo_);

    auto makeCanvasRow = [&](UiTheme rowTheme, QComboBox **modeOut, QPushButton **buttonOut) {
        auto *row = new QWidget(general);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto *mode = new QComboBox(row);
        mode->addItem(QStringLiteral("Theme default"), QStringLiteral("default"));
        mode->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
        auto *button = new QPushButton(row);
        QObject::connect(mode, &QComboBox::currentIndexChanged, this, [this]() { updateCanvasColorControls(); });
        QObject::connect(button, &QPushButton::clicked, this, [this, rowTheme]() { chooseCanvasColor(rowTheme); });
        rowLayout->addWidget(mode, 1);
        rowLayout->addWidget(button);
        *modeOut = mode;
        *buttonOut = button;
        return row;
    };
    generalLayout->addRow(QStringLiteral("Dark canvas"), makeCanvasRow(UiTheme::Dark, &darkCanvasMode_, &darkCanvasColorButton_));
    generalLayout->addRow(QStringLiteral("Light canvas"), makeCanvasRow(UiTheme::Light, &lightCanvasMode_, &lightCanvasColorButton_));
    darkCanvasMode_->setCurrentIndex(canvasSettings_.darkMode == CanvasColorMode::Custom ? 1 : 0);
    lightCanvasMode_->setCurrentIndex(canvasSettings_.lightMode == CanvasColorMode::Custom ? 1 : 0);

    guidelineColorButton_ = new QPushButton(general);
    QObject::connect(guidelineColorButton_, &QPushButton::clicked, this, &SettingsDialog::chooseGuidelineColor);
    generalLayout->addRow(QStringLiteral("Guideline color"), guidelineColorButton_);
    updateCanvasColorControls();

    visibilityBordersCheck_ = new QCheckBox(general);
    visibilityBordersCheck_->setChecked(behaviorSettings_.visibilityBordersEnabled);
    generalLayout->addRow(QStringLiteral("Show visibility borders"), visibilityBordersCheck_);

    positionLimitBorderCheck_ = new QCheckBox(general);
    positionLimitBorderCheck_->setChecked(behaviorSettings_.positionLimitBorderEnabled);
    generalLayout->addRow(QStringLiteral("Position Limit Border"), positionLimitBorderCheck_);

    displayAnchorsDuringTransformDrag_ = new QCheckBox(general);
    displayAnchorsDuringTransformDrag_->setChecked(behaviorSettings_.displayAnchorsDuringTransformDrag);
    generalLayout->addRow(QStringLiteral("Display anchors during transform drag"), displayAnchorsDuringTransformDrag_);

    generatePreviewsWithTransformations_ = new QCheckBox(general);
    generatePreviewsWithTransformations_->setChecked(behaviorSettings_.generatePreviewsWithTransformations);
    generalLayout->addRow(QStringLiteral("Generate previews with transformations applied"), generatePreviewsWithTransformations_);

    visibilityBorderResolution_ = new QComboBox(general);
    const QVector<QSize> visibilityResolutions = {QSize(1920, 1080), QSize(2560, 1440), QSize(3840, 2160)};
    for (const QSize &resolution : visibilityResolutions) {
        visibilityBorderResolution_->addItem(QStringLiteral("%1x%2").arg(resolution.width()).arg(resolution.height()), resolution);
    }
    const int resolutionIndex = visibilityBorderResolution_->findData(behaviorSettings_.visibilityBorderResolution);
    visibilityBorderResolution_->setCurrentIndex(resolutionIndex >= 0 ? resolutionIndex : 0);
    generalLayout->addRow(QStringLiteral("Border resolution"), visibilityBorderResolution_);

    auto makeNudgeSpinBox = [general](double value) {
        auto *spin = new QDoubleSpinBox(general);
        spin->setDecimals(3);
        spin->setRange(0.001, 1000.0);
        spin->setSingleStep(0.1);
        spin->setValue(value);
        return spin;
    };
    nudgeStep_ = makeNudgeSpinBox(behaviorSettings_.nudgeStep);
    generalLayout->addRow(QStringLiteral("Arrow nudge step"), nudgeStep_);
    nudgeShiftStep_ = makeNudgeSpinBox(behaviorSettings_.nudgeShiftStep);
    generalLayout->addRow(QStringLiteral("Shift arrow nudge step"), nudgeShiftStep_);

    liveryTextureScale_ = new QSpinBox(general);
    liveryTextureScale_->setRange(1, 8);
    liveryTextureScale_->setSingleStep(1);
    liveryTextureScale_->setValue(std::clamp(behaviorSettings_.liveryTextureScale, 1, 8));
    generalLayout->addRow(QStringLiteral("3D livery texture scale"), liveryTextureScale_);

    autosaveIntervalMinutes_ = new QSpinBox(general);
    autosaveIntervalMinutes_->setRange(0, 1440);
    autosaveIntervalMinutes_->setSingleStep(1);
    autosaveIntervalMinutes_->setSuffix(QStringLiteral(" min"));
    autosaveIntervalMinutes_->setSpecialValueText(QStringLiteral("Disabled"));
    autosaveIntervalMinutes_->setValue(std::clamp(behaviorSettings_.autosaveIntervalMinutes, 0, 1440));
    generalLayout->addRow(QStringLiteral("Autosave interval"), autosaveIntervalMinutes_);

    valueEditingWheelCheck_ = new QCheckBox(general);
    valueEditingWheelCheck_->setChecked(behaviorSettings_.valueEditingWheelEnabled);
    generalLayout->addRow(QStringLiteral("Edit values with mouse wheel"), valueEditingWheelCheck_);

    {
        auto *row = new QWidget(general);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        carModelsFolder_ = new QLineEdit(row);
        carModelsFolder_->setText(behaviorSettings_.carModelsFolder);
        carModelsFolder_->setPlaceholderText(QStringLiteral("Folder of extracted car models"));
        carModelsFolder_->setClearButtonEnabled(true);
        auto *browse = new QPushButton(QStringLiteral("Browse…"), row);
        QObject::connect(browse, &QPushButton::clicked, this, [this]() {
            const QString start = carModelsFolder_->text().isEmpty() ? QString() : carModelsFolder_->text();
            const QString picked = QFileDialog::getExistingDirectory(this, QStringLiteral("Car Models Folder"), start);
            if (!picked.isEmpty()) {
                carModelsFolder_->setText(picked);
            }
        });
        rowLayout->addWidget(carModelsFolder_, 1);
        rowLayout->addWidget(browse);
        generalLayout->addRow(QStringLiteral("Car models folder"), row);
    }

    discardModelOnLiveryOpen_ = new QCheckBox(general);
    discardModelOnLiveryOpen_->setChecked(behaviorSettings_.discardModelOnLiveryOpen);
    generalLayout->addRow(QStringLiteral("Discard current model on livery open"), discardModelOnLiveryOpen_);

    loadCarTextures_ = new QCheckBox(general);
    loadCarTextures_->setChecked(behaviorSettings_.loadCarTextures);
    generalLayout->addRow(QStringLiteral("Load car textures"), loadCarTextures_);

    tabs->addTab(general, QStringLiteral("General"));

    auto *keybinds = new QWidget(tabs);
    auto *keybindLayout = new QVBoxLayout(keybinds);
    shortcutTable_ = new QTableWidget(shortcuts_.size(), 3, keybinds);
    shortcutTable_->setHorizontalHeaderLabels({QStringLiteral("Action"), QStringLiteral("Shortcut"), QStringLiteral("Reset")});
    shortcutTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    shortcutTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    shortcutTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    shortcutTable_->verticalHeader()->setVisible(false);
    shortcutTable_->setSelectionMode(QAbstractItemView::NoSelection);
    shortcutTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int row = 0; row < shortcuts_.size(); ++row) {
        auto *name = new QTableWidgetItem(shortcuts_[row].label);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        shortcutTable_->setItem(row, 0, name);

        auto *edit = new ShortcutSequenceEdit(shortcuts_[row].currentSequence, shortcutTable_);
        shortcutTable_->setCellWidget(row, 1, edit);

        auto *reset = new QPushButton(QStringLiteral("Reset"), shortcutTable_);
        QObject::connect(reset, &QPushButton::clicked, this, [this, row]() { resetShortcutRow(row); });
        shortcutTable_->setCellWidget(row, 2, reset);
    }
    keybindLayout->addWidget(shortcutTable_, 1);
    auto *resetAll = new QPushButton(QStringLiteral("Reset All"), keybinds);
    QObject::connect(resetAll, &QPushButton::clicked, this, [this]() { resetAllShortcuts(); });
    keybindLayout->addWidget(resetAll, 0, Qt::AlignLeft);
    validationLabel_ = new QLabel(keybinds);
    validationLabel_->setWordWrap(true);
    validationLabel_->setStyleSheet(QStringLiteral("color: #d07070;"));
    keybindLayout->addWidget(validationLabel_);
    tabs->addTab(keybinds, QStringLiteral("Keybinds"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    layout->addWidget(buttons);
}

UiTheme SettingsDialog::selectedTheme() const {
    return themeFromSettingsValue(themeCombo_->currentData().toString());
}

CanvasColorSettings SettingsDialog::selectedCanvasSettings() const {
    CanvasColorSettings result = canvasSettings_;
    result.darkMode = darkCanvasMode_->currentData().toString() == QStringLiteral("custom")
        ? CanvasColorMode::Custom
        : CanvasColorMode::ThemeDefault;
    result.lightMode = lightCanvasMode_->currentData().toString() == QStringLiteral("custom")
        ? CanvasColorMode::Custom
        : CanvasColorMode::ThemeDefault;
    if (!result.darkCustom.isValid()) {
        result.darkCustom = defaultCanvasColor(UiTheme::Dark);
    }
    if (!result.lightCustom.isValid()) {
        result.lightCustom = defaultCanvasColor(UiTheme::Light);
    }
    return result;
}

BehaviorSettings SettingsDialog::selectedBehaviorSettings() const {
    const BehaviorSettings defaults;
    BehaviorSettings result = behaviorSettings_;
    const QSize resolution = visibilityBorderResolution_->currentData().toSize();

    result.visibilityBordersEnabled = visibilityBordersCheck_->isChecked();
    result.positionLimitBorderEnabled = positionLimitBorderCheck_->isChecked();
    result.displayAnchorsDuringTransformDrag = displayAnchorsDuringTransformDrag_->isChecked();
    result.generatePreviewsWithTransformations = generatePreviewsWithTransformations_->isChecked();
    result.visibilityBorderResolution = resolution.isValid()
        ? resolution
        : defaults.visibilityBorderResolution;
    result.nudgeStep = nudgeStep_->value();
    result.nudgeShiftStep = nudgeShiftStep_->value();
    result.liveryTextureScale = liveryTextureScale_->value();
    result.autosaveIntervalMinutes = autosaveIntervalMinutes_->value();
    result.valueEditingWheelEnabled = valueEditingWheelCheck_->isChecked();
    result.carModelsFolder = carModelsFolder_->text().trimmed();
    result.discardModelOnLiveryOpen = discardModelOnLiveryOpen_->isChecked();
    result.loadCarTextures = loadCarTextures_->isChecked();

    return result;
}

QVector<ShortcutSettingsItem> SettingsDialog::shortcutItems() const {
    QVector<ShortcutSettingsItem> result = shortcuts_;
    for (int row = 0; row < result.size(); ++row) {
        auto *edit = qobject_cast<QKeySequenceEdit *>(shortcutTable_->cellWidget(row, 1));
        if (edit != nullptr) {
            result[row].currentSequence = edit->keySequence();
        }
    }
    return result;
}

void SettingsDialog::setThemeChangedCallback(std::function<void(UiTheme)> callback) {
    themeChangedCallback_ = std::move(callback);
}

void SettingsDialog::resetShortcutRow(int row) {
    if (row < 0 || row >= shortcuts_.size()) {
        return;
    }
    auto *edit = qobject_cast<QKeySequenceEdit *>(shortcutTable_->cellWidget(row, 1));
    if (edit != nullptr) {
        edit->setKeySequence(shortcuts_[row].defaultSequence);
    }
}

void SettingsDialog::resetAllShortcuts() {
    for (int row = 0; row < shortcuts_.size(); ++row) {
        resetShortcutRow(row);
    }
}

void SettingsDialog::chooseCanvasColor(UiTheme theme) {
    const QColor current = theme == UiTheme::Light
        ? (canvasSettings_.lightCustom.isValid() ? canvasSettings_.lightCustom : defaultCanvasColor(UiTheme::Light))
        : (canvasSettings_.darkCustom.isValid() ? canvasSettings_.darkCustom : defaultCanvasColor(UiTheme::Dark));
    const QColor picked = QColorDialog::getColor(current, this, QStringLiteral("Canvas Color"));
    if (!picked.isValid()) {
        return;
    }
    if (theme == UiTheme::Light) {
        canvasSettings_.lightCustom = picked;
        lightCanvasMode_->setCurrentIndex(1);
    } else {
        canvasSettings_.darkCustom = picked;
        darkCanvasMode_->setCurrentIndex(1);
    }
    updateCanvasColorControls();
}

void SettingsDialog::chooseGuidelineColor() {
    const QColor current = behaviorSettings_.guidelineColor.isValid()
        ? behaviorSettings_.guidelineColor
        : QColor(0, 170, 255);
    const QColor picked = QColorDialog::getColor(current, this, QStringLiteral("Guideline Color"),
                                                 QColorDialog::ShowAlphaChannel);
    if (!picked.isValid()) {
        return;
    }
    behaviorSettings_.guidelineColor = picked;
    updateCanvasColorControls();
}

void SettingsDialog::updateCanvasColorControls() {
    const auto updateButton = [](QPushButton *button, const QColor &color, bool customEnabled) {
        if (button == nullptr) {
            return;
        }
        button->setEnabled(customEnabled);
        button->setText(color.name(QColor::HexRgb).toUpper());
        button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                  .arg(color.name(QColor::HexRgb),
                                       color.lightness() < 128 ? QStringLiteral("#ffffff") : QStringLiteral("#202225")));
    };
    const CanvasColorSettings settings = selectedCanvasSettings();
    updateButton(darkCanvasColorButton_,
                 settings.darkMode == CanvasColorMode::Custom ? settings.darkCustom : defaultCanvasColor(UiTheme::Dark),
                 settings.darkMode == CanvasColorMode::Custom);
    updateButton(lightCanvasColorButton_,
                 settings.lightMode == CanvasColorMode::Custom ? settings.lightCustom : defaultCanvasColor(UiTheme::Light),
                 settings.lightMode == CanvasColorMode::Custom);
    updateButton(guidelineColorButton_,
                 behaviorSettings_.guidelineColor.isValid() ? behaviorSettings_.guidelineColor : QColor(0, 170, 255),
                 true);
}

bool SettingsDialog::shortcutsAreValid() {
    validationLabel_->clear();
    QHash<QString, QString> seen;
    for (const ShortcutSettingsItem &item : shortcutItems()) {
        if (item.currentSequence.isEmpty()) {
            continue;
        }
        const QString normalized = item.currentSequence.toString(QKeySequence::PortableText);
        const auto it = seen.constFind(normalized);
        if (it != seen.constEnd()) {
            validationLabel_->setText(QStringLiteral("Shortcut %1 is assigned to both %2 and %3.")
                                          .arg(item.currentSequence.toString(QKeySequence::NativeText), it.value(), item.label));
            return false;
        }
        seen.insert(normalized, item.label);
    }
    return true;
}

void SettingsDialog::accept() {
    if (!shortcutsAreValid()) {
        return;
    }
    QDialog::accept();
}

} // namespace gui
