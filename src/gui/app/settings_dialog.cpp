#include "settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHash>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QAbstractItemView>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace gui {
namespace {

constexpr int ShortcutEditRole = Qt::UserRole + 10;

} // namespace

SettingsDialog::SettingsDialog(UiTheme theme,
                               const CanvasColorSettings &canvasSettings,
                               const QVector<ShortcutSettingsItem> &shortcuts,
                               QWidget *parent)
    : QDialog(parent)
    , initialTheme_(theme)
    , canvasSettings_(canvasSettings)
    , shortcuts_(shortcuts)
{
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
    updateCanvasColorControls();

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

        auto *edit = new QKeySequenceEdit(shortcuts_[row].currentSequence, shortcutTable_);
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

UiTheme SettingsDialog::selectedTheme() const
{
    return themeFromSettingsValue(themeCombo_->currentData().toString());
}

CanvasColorSettings SettingsDialog::selectedCanvasSettings() const
{
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

QVector<ShortcutSettingsItem> SettingsDialog::shortcutItems() const
{
    QVector<ShortcutSettingsItem> result = shortcuts_;
    for (int row = 0; row < result.size(); ++row) {
        auto *edit = qobject_cast<QKeySequenceEdit *>(shortcutTable_->cellWidget(row, 1));
        if (edit != nullptr) {
            result[row].currentSequence = edit->keySequence();
        }
    }
    return result;
}

void SettingsDialog::setThemeChangedCallback(std::function<void(UiTheme)> callback)
{
    themeChangedCallback_ = std::move(callback);
}

void SettingsDialog::resetShortcutRow(int row)
{
    auto *edit = qobject_cast<QKeySequenceEdit *>(shortcutTable_->cellWidget(row, 1));
    if (edit != nullptr && row >= 0 && row < shortcuts_.size()) {
        edit->setKeySequence(shortcuts_[row].defaultSequence);
    }
}

void SettingsDialog::resetAllShortcuts()
{
    for (int row = 0; row < shortcuts_.size(); ++row) {
        resetShortcutRow(row);
    }
}

void SettingsDialog::chooseCanvasColor(UiTheme theme)
{
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

void SettingsDialog::updateCanvasColorControls()
{
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
}

bool SettingsDialog::shortcutsAreValid()
{
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

void SettingsDialog::accept()
{
    if (!shortcutsAreValid()) {
        return;
    }
    QDialog::accept();
}

} // namespace gui
