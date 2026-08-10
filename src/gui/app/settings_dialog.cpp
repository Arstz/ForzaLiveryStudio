#include "settings_dialog.h"

#include "gui_assets.h"
#include "gui/key_bindings.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <algorithm>

namespace gui {
namespace {

struct SettingTip {
    bool hasTip = false;
    QString tip;
};

using SettingTips = QHash<QString, SettingTip>;

SettingTips loadSettingTips() {
    QFile file(assetPath(QStringLiteral("settings_tips.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("Could not open settings tips: %1").arg(file.fileName());
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning().noquote() << QStringLiteral("Could not parse settings tips: %1").arg(error.errorString());
        return {};
    }

    SettingTips result;
    const QJsonObject root = document.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject object = it.value().toObject();
        result.insert(it.key(), {
            object.value(QStringLiteral("hasTip")).toBool(false),
            object.value(QStringLiteral("tip")).toString(),
        });
    }

    return result;
}

QWidget *settingLabel(const QString &text,
                      const QString &id,
                      const SettingTips &tips,
                      QWidget *parent) {
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 8, 0, 8);
    layout->setSpacing(5);
    auto *label = new QLabel(text, container);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch(1);
    const SettingTip tip = tips.value(id);
    if (tip.hasTip && !tip.tip.isEmpty()) {
        auto *help = new QToolButton(container);
        help->setText(QStringLiteral("?"));
        help->setAutoRaise(true);
        help->setCursor(Qt::WhatsThisCursor);
        help->setToolTip(tip.tip);
        help->setAccessibleName(QStringLiteral("Help for %1").arg(text));
        layout->addWidget(help);
    }

    return container;
}

struct SettingsPage {
    QWidget *widget = nullptr;
    QGridLayout *grid = nullptr;
    int nextRow = 0;
};

struct SettingsSurface {
    QWidget *page = nullptr;
    QFrame *content = nullptr;
    QVBoxLayout *layout = nullptr;
};

SettingsSurface createSettingsSurface(QStackedWidget *pages) {
    auto *page = new QWidget(pages);
    auto *pageLayout = new QVBoxLayout(page);
    auto *scroll = new QScrollArea(page);
    auto *content = new QFrame(scroll);
    auto *contentLayout = new QVBoxLayout(content);
    content->setFrameShape(QFrame::StyledPanel);
    content->setBackgroundRole(QPalette::Base);
    content->setAutoFillBackground(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(content);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);
    pages->addWidget(page);

    return {page, content, contentLayout};
}

void addSettingRow(SettingsPage &page,
                   const QString &id,
                   const QString &label,
                   QWidget *field,
                   const SettingTips &tips) {
    if (page.nextRow > 0) {
        auto *leftSeparator = new QFrame(page.widget);
        auto *middleSeparator = new QFrame(page.widget);
        auto *rightSeparator = new QFrame(page.widget);
        leftSeparator->setFrameShape(QFrame::HLine);
        leftSeparator->setFrameShadow(QFrame::Sunken);
        middleSeparator->setFrameShape(QFrame::VLine);
        middleSeparator->setFrameShadow(QFrame::Sunken);
        rightSeparator->setFrameShape(QFrame::HLine);
        rightSeparator->setFrameShadow(QFrame::Sunken);
        page.grid->addWidget(leftSeparator, page.nextRow, 0);
        page.grid->addWidget(middleSeparator, page.nextRow, 1);
        page.grid->addWidget(rightSeparator, page.nextRow++, 2);
    }
    auto *verticalSeparator = new QFrame(page.widget);
    auto *fieldContainer = new QWidget(page.widget);
    auto *fieldLayout = new QHBoxLayout(fieldContainer);
    verticalSeparator->setFrameShape(QFrame::VLine);
    verticalSeparator->setFrameShadow(QFrame::Sunken);
    fieldLayout->setContentsMargins(0, 8, 0, 8);
    fieldLayout->addWidget(field, 1);
    field->setSizePolicy(QSizePolicy::Expanding, field->sizePolicy().verticalPolicy());
    page.grid->addWidget(settingLabel(label, id, tips, page.widget),
                         page.nextRow, 0, Qt::AlignVCenter);
    page.grid->addWidget(verticalSeparator, page.nextRow, 1);
    page.grid->addWidget(fieldContainer, page.nextRow, 2);
    ++page.nextRow;
}

SettingsPage createSettingsPage(QStackedWidget *pages) {
    const SettingsSurface surface = createSettingsSurface(pages);
    auto *grid = new QGridLayout;
    grid->setColumnStretch(0, 3);
    grid->setColumnMinimumWidth(1, 1);
    grid->setColumnStretch(2, 7);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(0);
    surface.layout->addLayout(grid);
    surface.layout->addStretch(1);

    return {surface.content, grid, 0};
}

class ShortcutSequenceEdit final : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;

protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const std::optional<QKeySequence> captured = capturedTabShortcut(*keyEvent);
            if (captured.has_value()) {
                if (event->type() == QEvent::KeyPress) {
                    setKeySequence(captured.value());
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
                               const PreviewBackgroundSettings &previewBackgroundSettings,
                               const BehaviorSettings &behaviorSettings,
                               const cover::FillOptions &differentialFillOptions,
                               const QVector<ShortcutSettingsItem> &shortcuts,
                               QWidget *parent)
    : QDialog(parent)
    , canvasSettings_(canvasSettings)
    , previewBackgroundSettings_(previewBackgroundSettings)
    , behaviorSettings_(behaviorSettings)
    , differentialFillOptions_(differentialFillOptions)
    , shortcuts_(shortcuts) {
    setWindowTitle(QStringLiteral("Settings"));
    resize(940, 680);

    const SettingTips tips = loadSettingTips();
    auto *layout = new QVBoxLayout(this);
    auto *contentLayout = new QHBoxLayout;
    auto *navigation = new QListWidget(this);
    auto *pages = new QStackedWidget(this);
    navigation->addItems({QStringLiteral("General"), QStringLiteral("Theme"),
                          QStringLiteral("Tools"), QStringLiteral("Editor"),
                          QStringLiteral("Advanced"),
                          QStringLiteral("Keybinds"), QStringLiteral("System")});
    navigation->setCurrentRow(0);
    navigation->setFixedWidth(150);
    navigation->setSpacing(3);
    navigation->setUniformItemSizes(true);
    contentLayout->addWidget(navigation);
    contentLayout->addWidget(pages, 1);
    layout->addLayout(contentLayout, 1);

    SettingsPage general = createSettingsPage(pages);
    auto *gameFolderRow = new QWidget(general.widget);
    auto *gameFolderLayout = new QHBoxLayout(gameFolderRow);
    gameFolderLayout->setContentsMargins(0, 0, 0, 0);
    gameFolderLayout->setSpacing(8);
    gameFolder_ = new QLineEdit(gameFolderRow);
    gameFolder_->setText(behaviorSettings_.gameFolder);
    gameFolder_->setPlaceholderText(QStringLiteral("Forza game install folder"));
    gameFolder_->setClearButtonEnabled(true);
    auto *browse = new QPushButton(QStringLiteral("Browse"), gameFolderRow);
    QObject::connect(browse, &QPushButton::clicked, this, [this]() {
        const QString start = gameFolder_->text().isEmpty() ? QString() : gameFolder_->text();
        const QString picked = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Forza Game Folder"), start);
        if (!picked.isEmpty()) {
            gameFolder_->setText(picked);
        }
    });
    gameFolderLayout->addWidget(gameFolder_, 1);
    gameFolderLayout->addWidget(browse);
    addSettingRow(general, QStringLiteral("general.game_folder"),
                  QStringLiteral("Game folder"), gameFolderRow, tips);

    autosaveIntervalMinutes_ = new QSpinBox(general.widget);
    autosaveIntervalMinutes_->setRange(0, 1440);
    autosaveIntervalMinutes_->setSingleStep(1);
    autosaveIntervalMinutes_->setSuffix(QStringLiteral(" min"));
    autosaveIntervalMinutes_->setSpecialValueText(QStringLiteral("Disabled"));
    autosaveIntervalMinutes_->setValue(
        std::clamp(behaviorSettings_.autosaveIntervalMinutes, 0, 1440));
    addSettingRow(general, QStringLiteral("general.autosave_interval"),
                  QStringLiteral("Autosave interval"), autosaveIntervalMinutes_, tips);

    SettingsPage themePage = createSettingsPage(pages);
    themeCombo_ = new QComboBox(themePage.widget);
    themeCombo_->addItem(QStringLiteral("Dark"), themeSettingsValue(UiTheme::Dark));
    themeCombo_->addItem(QStringLiteral("Light"), themeSettingsValue(UiTheme::Light));
    themeCombo_->setCurrentIndex(theme == UiTheme::Light ? 1 : 0);
    QObject::connect(themeCombo_, &QComboBox::currentIndexChanged, this, [this]() {
        if (themeChangedCallback_) {
            themeChangedCallback_(selectedTheme());
        }
        updateCanvasColorControls();
        updatePreviewBackgroundControls();
    });
    addSettingRow(themePage, QStringLiteral("theme.theme"),
                  QStringLiteral("Theme"), themeCombo_, tips);

    const auto makeCanvasRow = [&](UiTheme rowTheme,
                                   QComboBox **modeOut,
                                   QPushButton **buttonOut) {
        auto *row = new QWidget(themePage.widget);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto *mode = new QComboBox(row);
        mode->addItem(QStringLiteral("Theme default"), QStringLiteral("default"));
        mode->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
        auto *button = new QPushButton(row);
        QObject::connect(mode, &QComboBox::currentIndexChanged,
                         this, [this]() { updateCanvasColorControls(); });
        QObject::connect(button, &QPushButton::clicked,
                         this, [this, rowTheme]() { chooseCanvasColor(rowTheme); });
        rowLayout->addWidget(mode, 1);
        rowLayout->addWidget(button);
        *modeOut = mode;
        *buttonOut = button;
        return row;
    };
    addSettingRow(themePage, QStringLiteral("theme.dark_canvas"),
                  QStringLiteral("Dark canvas"),
                  makeCanvasRow(UiTheme::Dark, &darkCanvasMode_, &darkCanvasColorButton_), tips);
    addSettingRow(themePage, QStringLiteral("theme.light_canvas"),
                  QStringLiteral("Light canvas"),
                  makeCanvasRow(UiTheme::Light, &lightCanvasMode_, &lightCanvasColorButton_), tips);
    darkCanvasMode_->setCurrentIndex(canvasSettings_.darkMode == CanvasColorMode::Custom ? 1 : 0);
    lightCanvasMode_->setCurrentIndex(canvasSettings_.lightMode == CanvasColorMode::Custom ? 1 : 0);

    const auto makeBackgroundRow = [&](bool buffer,
                                       QComboBox **modeOut,
                                       QPushButton **buttonOut) {
        auto *row = new QWidget(themePage.widget);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto *mode = new QComboBox(row);
        mode->addItem(QStringLiteral("Theme default"), QStringLiteral("default"));
        mode->addItem(QStringLiteral("Checkerboard"), QStringLiteral("checkerboard"));
        mode->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
        auto *button = new QPushButton(row);
        QObject::connect(mode, &QComboBox::currentIndexChanged,
                         this, [this]() { updatePreviewBackgroundControls(); });
        QObject::connect(button, &QPushButton::clicked,
                         this, [this, buffer]() { choosePreviewBackgroundColor(buffer); });
        rowLayout->addWidget(mode, 1);
        rowLayout->addWidget(button);
        *modeOut = mode;
        *buttonOut = button;
        return row;
    };
    addSettingRow(themePage, QStringLiteral("theme.buffer_background"),
                  QStringLiteral("Buffer background"),
                  makeBackgroundRow(true, &bufferBackgroundMode_, &bufferBackgroundColorButton_), tips);
    addSettingRow(themePage, QStringLiteral("theme.layer_background"),
                  QStringLiteral("Layer background"),
                  makeBackgroundRow(false, &layersBackgroundMode_, &layersBackgroundColorButton_), tips);
    const auto backgroundModeIndex = [](PreviewBackgroundMode mode) {
        return mode == PreviewBackgroundMode::Checkerboard ? 1
            : mode == PreviewBackgroundMode::Custom ? 2 : 0;
    };
    bufferBackgroundMode_->setCurrentIndex(
        backgroundModeIndex(previewBackgroundSettings_.buffer.mode));
    layersBackgroundMode_->setCurrentIndex(
        backgroundModeIndex(previewBackgroundSettings_.layers.mode));

    SettingsPage toolsPage = createSettingsPage(pages);
    displayAnchorsDuringTransformDrag_ = new QCheckBox(toolsPage.widget);
    displayAnchorsDuringTransformDrag_->setChecked(
        behaviorSettings_.displayAnchorsDuringTransformDrag);
    addSettingRow(toolsPage, QStringLiteral("tools.display_anchors"),
                  QStringLiteral("Display anchors during transform drag"),
                  displayAnchorsDuringTransformDrag_, tips);
    const auto makeNudgeSpinBox = [toolsPage](double value) {
        auto *spin = new QDoubleSpinBox(toolsPage.widget);
        spin->setDecimals(3);
        spin->setRange(0.001, 1000.0);
        spin->setSingleStep(0.1);
        spin->setValue(value);
        return spin;
    };
    nudgeStep_ = makeNudgeSpinBox(behaviorSettings_.nudgeStep);
    addSettingRow(toolsPage, QStringLiteral("tools.nudge_small"),
                  QStringLiteral("Arrow nudge small step"), nudgeStep_, tips);
    nudgeShiftStep_ = makeNudgeSpinBox(behaviorSettings_.nudgeShiftStep);
    addSettingRow(toolsPage, QStringLiteral("tools.nudge_big"),
                  QStringLiteral("Arrow nudge big step"), nudgeShiftStep_, tips);
    toolbarViewCombo_ = new QComboBox(toolsPage.widget);
    toolbarViewCombo_->addItem(QStringLiteral("Horizontal with labels"), false);
    toolbarViewCombo_->addItem(QStringLiteral("Vertical icons only"), true);
    toolbarViewCombo_->setCurrentIndex(behaviorSettings_.verticalToolbar ? 1 : 0);
    addSettingRow(toolsPage, QStringLiteral("tools.toolbar_view"),
                  QStringLiteral("Toolbar view"), toolbarViewCombo_, tips);
    separateOpacityAndSkewToolsCheck_ = new QCheckBox(toolsPage.widget);
    separateOpacityAndSkewToolsCheck_->setChecked(
        behaviorSettings_.separateOpacityAndSkewTools);
    addSettingRow(toolsPage, QStringLiteral("tools.separate_opacity_skew"),
                  QStringLiteral("Separate opacity and skew tools"),
                  separateOpacityAndSkewToolsCheck_, tips);

    SettingsPage editorPage = createSettingsPage(pages);
    guidelineColorButton_ = new QPushButton(editorPage.widget);
    QObject::connect(guidelineColorButton_, &QPushButton::clicked,
                     this, &SettingsDialog::chooseGuidelineColor);
    addSettingRow(editorPage, QStringLiteral("editor.guideline_color"),
                  QStringLiteral("Guideline color"), guidelineColorButton_, tips);
    visibilityBordersCheck_ = new QCheckBox(editorPage.widget);
    visibilityBordersCheck_->setChecked(behaviorSettings_.visibilityBordersEnabled);
    addSettingRow(editorPage, QStringLiteral("editor.visibility_borders"),
                  QStringLiteral("Show visibility borders"), visibilityBordersCheck_, tips);
    generatePreviewsWithTransformations_ = new QCheckBox(editorPage.widget);
    generatePreviewsWithTransformations_->setChecked(
        behaviorSettings_.generatePreviewsWithTransformations);
    addSettingRow(editorPage, QStringLiteral("editor.transformed_previews"),
                  QStringLiteral("Generate previews with transformations applied"),
                  generatePreviewsWithTransformations_, tips);
    visibilityBorderResolution_ = new QComboBox(editorPage.widget);
    const QVector<QSize> visibilityResolutions = {
        QSize(1920, 1080), QSize(2560, 1440), QSize(3840, 2160),
    };
    for (const QSize &resolution : visibilityResolutions) {
        visibilityBorderResolution_->addItem(
            QStringLiteral("%1x%2").arg(resolution.width()).arg(resolution.height()), resolution);
    }
    const int resolutionIndex = visibilityBorderResolution_->findData(
        behaviorSettings_.visibilityBorderResolution);
    visibilityBorderResolution_->setCurrentIndex(resolutionIndex >= 0 ? resolutionIndex : 0);
    addSettingRow(editorPage, QStringLiteral("editor.border_resolution"),
                  QStringLiteral("Border resolution"), visibilityBorderResolution_, tips);
    liveryTextureScale_ = new QSpinBox(editorPage.widget);
    liveryTextureScale_->setRange(1, 8);
    liveryTextureScale_->setSingleStep(1);
    liveryTextureScale_->setValue(std::clamp(behaviorSettings_.liveryTextureScale, 1, 8));
    addSettingRow(editorPage, QStringLiteral("editor.livery_texture_scale"),
                  QStringLiteral("3D livery texture scale"), liveryTextureScale_, tips);
    valueEditingWheelCheck_ = new QCheckBox(editorPage.widget);
    valueEditingWheelCheck_->setChecked(behaviorSettings_.valueEditingWheelEnabled);
    addSettingRow(editorPage, QStringLiteral("editor.value_wheel"),
                  QStringLiteral("Edit values with mouse wheel"), valueEditingWheelCheck_, tips);
    discardModelOnLiveryOpen_ = new QCheckBox(editorPage.widget);
    discardModelOnLiveryOpen_->setChecked(behaviorSettings_.discardModelOnLiveryOpen);
    addSettingRow(editorPage, QStringLiteral("editor.discard_model"),
                  QStringLiteral("Discard current model on livery open"),
                  discardModelOnLiveryOpen_, tips);
    loadCarTextures_ = new QCheckBox(editorPage.widget);
    loadCarTextures_->setChecked(behaviorSettings_.loadCarTextures);
    addSettingRow(editorPage, QStringLiteral("editor.load_car_textures"),
                  QStringLiteral("Load car textures"), loadCarTextures_, tips);

    SettingsPage advancedPage = createSettingsPage(pages);
    const auto makeIntegerOption = [advancedPage](int minimum,
                                                   int maximum,
                                                   int value) {
        auto *spin = new QSpinBox(advancedPage.widget);
        spin->setRange(minimum, maximum);
        spin->setValue(std::clamp(value, minimum, maximum));
        return spin;
    };
    const auto makeRealOption = [advancedPage](double minimum,
                                                double maximum,
                                                double value,
                                                int decimals,
                                                double step) {
        auto *spin = new QDoubleSpinBox(advancedPage.widget);
        spin->setDecimals(decimals);
        spin->setRange(minimum, maximum);
        spin->setSingleStep(step);
        spin->setValue(std::clamp(value, minimum, maximum));
        return spin;
    };
    differentialBudget_ = makeIntegerOption(
        cover::kMinimumBudget, cover::kMaximumBudget,
        differentialFillOptions_.budget);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_budget"),
                  QStringLiteral("Placement budget"), differentialBudget_, tips);
    differentialAdamIterations_ = makeIntegerOption(
        cover::kMinimumAdamIterations, cover::kMaximumAdamIterations,
        differentialFillOptions_.adamIterations);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_adam_iterations"),
                  QStringLiteral("Optimizer iterations"),
                  differentialAdamIterations_, tips);
    differentialRestarts_ = makeIntegerOption(
        cover::kMinimumRestarts, cover::kMaximumRestarts,
        differentialFillOptions_.restarts);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_restarts"),
                  QStringLiteral("Optimizer restarts"), differentialRestarts_, tips);
    differentialSpillWeight_ = makeRealOption(
        cover::kMinimumSpillWeight, cover::kMaximumSpillWeight,
        differentialFillOptions_.spillWeight, 3, 0.1);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_spill_weight"),
                  QStringLiteral("Spill penalty"), differentialSpillWeight_, tips);
    differentialEpsArea_ = makeRealOption(
        cover::kMinimumEpsArea, cover::kMaximumEpsArea,
        differentialFillOptions_.epsArea, 3, 0.1);
    differentialEpsArea_->setSuffix(QStringLiteral(" canvas²"));
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_eps_area"),
                  QStringLiteral("Residual-area threshold"), differentialEpsArea_, tips);
    differentialEpsGain_ = makeRealOption(
        cover::kMinimumEpsGain, cover::kMaximumEpsGain,
        differentialFillOptions_.epsGain, 3, 0.1);
    differentialEpsGain_->setSuffix(QStringLiteral(" canvas²"));
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_eps_gain"),
                  QStringLiteral("Minimum placement gain"), differentialEpsGain_, tips);
    differentialEpsSpill_ = makeRealOption(
        cover::kMinimumEpsSpill, cover::kMaximumEpsSpill,
        differentialFillOptions_.epsSpill, 3, 0.1);
    differentialEpsSpill_->setSuffix(QStringLiteral(" canvas²"));
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_eps_spill"),
                  QStringLiteral("Outside-area tolerance"), differentialEpsSpill_, tips);
    differentialAdamLearningRate_ = makeRealOption(
        cover::kMinimumAdamLearningRate, cover::kMaximumAdamLearningRate,
        differentialFillOptions_.adamLearningRate, 4, 0.01);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_adam_learning_rate"),
                  QStringLiteral("Optimizer learning rate"),
                  differentialAdamLearningRate_, tips);
    differentialInactivityTimeout_ = makeRealOption(
        cover::kMinimumInactivityTimeoutSeconds,
        cover::kMaximumInactivityTimeoutSeconds,
        differentialFillOptions_.inactivityTimeoutSeconds, 1, 5.0);
    differentialInactivityTimeout_->setSuffix(QStringLiteral(" s"));
    differentialInactivityTimeout_->setSpecialValueText(QStringLiteral("Disabled"));
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_inactivity_timeout"),
                  QStringLiteral("Inactivity timeout"),
                  differentialInactivityTimeout_, tips);
    differentialBoundaryTolerance_ = makeRealOption(
        cover::kMinimumBoundaryTolerance, cover::kMaximumBoundaryTolerance,
        differentialFillOptions_.boundaryTolerance, 3, 0.01);
    differentialBoundaryTolerance_->setSuffix(QStringLiteral(" canvas"));
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_boundary_tolerance"),
                  QStringLiteral("Boundary tolerance"),
                  differentialBoundaryTolerance_, tips);
    differentialOutwardMargin_ = makeRealOption(
        cover::kMinimumOutwardMargin, cover::kMaximumOutwardMargin,
        differentialFillOptions_.outwardMargin, 3, 0.1);
    differentialOutwardMargin_->setSuffix(QStringLiteral(" canvas"));
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_outward_margin"),
                  QStringLiteral("Outward margin"), differentialOutwardMargin_, tips);
    differentialAreaWindowRatio_ = makeRealOption(
        cover::kMinimumAreaWindowRatio, cover::kMaximumAreaWindowRatio,
        differentialFillOptions_.areaWindowRatio, 3, 0.025);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_area_window_ratio"),
                  QStringLiteral("Candidate area window"),
                  differentialAreaWindowRatio_, tips);
    differentialTargetCoverageRatio_ = makeRealOption(
        cover::kMinimumTargetCoverageRatio,
        cover::kMaximumTargetCoverageRatio,
        differentialFillOptions_.targetCoverageRatio, 3, 0.001);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_target_coverage_ratio"),
                  QStringLiteral("Completion coverage"),
                  differentialTargetCoverageRatio_, tips);
    differentialTverskyAlpha_ = makeRealOption(
        cover::kMinimumTverskyAlpha, cover::kMaximumTverskyAlpha,
        differentialFillOptions_.tverskyAlpha, 3, 0.05);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_tversky_alpha"),
                  QStringLiteral("Tversky spill penalty"),
                  differentialTverskyAlpha_, tips);
    differentialTverskyBeta_ = makeRealOption(
        cover::kMinimumTverskyBeta, cover::kMaximumTverskyBeta,
        differentialFillOptions_.tverskyBeta, 3, 0.05);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_tversky_beta"),
                  QStringLiteral("Tversky missing-area penalty"),
                  differentialTverskyBeta_, tips);
    differentialFeatureWeight_ = makeRealOption(
        cover::kMinimumFeatureWeight, cover::kMaximumFeatureWeight,
        differentialFillOptions_.featureWeight, 3, 0.1);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_feature_weight"),
                  QStringLiteral("Contour feature weight"),
                  differentialFeatureWeight_, tips);
    differentialFeatureRestarts_ = makeIntegerOption(
        cover::kMinimumFeatureRestarts, cover::kMaximumFeatureRestarts,
        differentialFillOptions_.featureRestarts);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_feature_restarts"),
                  QStringLiteral("Contour feature restarts"),
                  differentialFeatureRestarts_, tips);
    differentialSeed_ = makeIntegerOption(
        cover::kMinimumSeed, cover::kMaximumSeed,
        static_cast<int>(std::min<std::uint64_t>(
            differentialFillOptions_.seed,
            static_cast<std::uint64_t>(cover::kMaximumSeed))));
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_seed"),
                  QStringLiteral("Random seed"), differentialSeed_, tips);
    differentialUseRouter_ = new QCheckBox(advancedPage.widget);
    differentialUseRouter_->setChecked(differentialFillOptions_.useRouter);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_router"),
                  QStringLiteral("Candidate routing"), differentialUseRouter_, tips);
    differentialUseGpu_ = new QCheckBox(advancedPage.widget);
    differentialUseGpu_->setChecked(differentialFillOptions_.useGpu);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_gpu"),
                  QStringLiteral("GPU acceleration"), differentialUseGpu_, tips);
    differentialUseWeightedContour_ = new QCheckBox(advancedPage.widget);
    differentialUseWeightedContour_->setChecked(
        differentialFillOptions_.useWeightedContour);
    addSettingRow(advancedPage, QStringLiteral("advanced.differential_weighted_contour"),
                  QStringLiteral("Feature-weighted contour"),
                  differentialUseWeightedContour_, tips);

    SettingsPage keybindsPage = createSettingsPage(pages);
    shortcutEdits_.reserve(shortcuts_.size());
    for (int row = 0; row < shortcuts_.size(); ++row) {
        auto *controls = new QWidget(keybindsPage.widget);
        auto *controlsLayout = new QHBoxLayout(controls);
        auto *edit = new ShortcutSequenceEdit(shortcuts_[row].currentSequence, controls);
        auto *reset = new QPushButton(QStringLiteral("Reset"), controls);
        controlsLayout->setContentsMargins(0, 0, 0, 0);
        controlsLayout->setSpacing(8);
        controlsLayout->addWidget(edit, 1);
        controlsLayout->addWidget(reset);
        shortcutEdits_.append(edit);
        QObject::connect(reset, &QPushButton::clicked,
                         this, [this, row]() { resetShortcutRow(row); });
        addSettingRow(keybindsPage, QStringLiteral("keybinds.shortcuts"),
                      shortcuts_[row].label, controls, tips);
    }
    auto *resetControls = new QWidget(keybindsPage.widget);
    auto *resetLayout = new QHBoxLayout(resetControls);
    auto *resetAll = new QPushButton(QStringLiteral("Reset All"), resetControls);
    validationLabel_ = new QLabel(resetControls);
    validationLabel_->setWordWrap(true);
    validationLabel_->setStyleSheet(QStringLiteral("color: #d07070;"));
    resetLayout->setContentsMargins(0, 0, 0, 0);
    resetLayout->setSpacing(8);
    resetLayout->addWidget(validationLabel_, 1);
    resetLayout->addWidget(resetAll);
    QObject::connect(resetAll, &QPushButton::clicked,
                     this, [this]() { resetAllShortcuts(); });
    addSettingRow(keybindsPage, QStringLiteral("keybinds.shortcuts"),
                  QStringLiteral("All keybinds"), resetControls, tips);

    SettingsPage systemPage = createSettingsPage(pages);
    const SystemIconSet iconSet = loadSystemIconSet();
    systemIconSetCombo_ = new QComboBox(systemPage.widget);
    systemIconSetCombo_->addItem(
        QStringLiteral("Light"), systemIconSetSettingsValue(SystemIconSet::Light));
    systemIconSetCombo_->addItem(
        QStringLiteral("Dark"), systemIconSetSettingsValue(SystemIconSet::Dark));
    systemIconSetCombo_->setCurrentIndex(iconSet == SystemIconSet::Dark ? 1 : 0);
    addSettingRow(systemPage, QStringLiteral("system.icon_pack"),
                  QStringLiteral("Icon pack"), systemIconSetCombo_, tips);

    auto *association = new QWidget(systemPage.widget);
    auto *associationLayout = new QVBoxLayout(association);
    associationLayout->setContentsMargins(0, 0, 0, 0);
    projectFileAssociationCheck_ = new QCheckBox(
        QStringLiteral("Associate .3so files with this application"), association);
    projectFileAssociationStatus_ = new QLabel(association);
    projectFileAssociationStatus_->setWordWrap(true);
    const ProjectFileAssociationState associationState = projectFileAssociationState(iconSet);
    projectFileAssociationCheck_->setChecked(
        associationState != ProjectFileAssociationState::NotRegistered);
    associationLayout->addWidget(projectFileAssociationCheck_);
    associationLayout->addWidget(projectFileAssociationStatus_);
    addSettingRow(systemPage, QStringLiteral("system.file_association"),
                  QStringLiteral("File association"), association, tips);
    const auto updateAssociationStatus = [this]() {
        if (!projectFileAssociationCheck_->isChecked()) {
            projectFileAssociationStatus_->setText(QStringLiteral("Not registered"));
            return;
        }
        const ProjectFileAssociationState state =
            projectFileAssociationState(selectedSystemIconSet());
        projectFileAssociationStatus_->setText(
            state == ProjectFileAssociationState::Registered
                ? QStringLiteral("Registered to this application")
                : state == ProjectFileAssociationState::NeedsRepair
                    ? QStringLiteral("Registered; apply settings to update it")
                    : QStringLiteral("Will be registered when settings are applied"));
    };
    QObject::connect(projectFileAssociationCheck_, &QCheckBox::toggled,
                     this, [updateAssociationStatus](bool) { updateAssociationStatus(); });
    QObject::connect(systemIconSetCombo_, &QComboBox::currentIndexChanged,
                     this, [updateAssociationStatus](int) { updateAssociationStatus(); });
    updateAssociationStatus();

    updateCanvasColorControls();
    updatePreviewBackgroundControls();
    QObject::connect(navigation, &QListWidget::currentRowChanged,
                     pages, &QStackedWidget::setCurrentIndex);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
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
        ? CanvasColorMode::Custom : CanvasColorMode::ThemeDefault;
    result.lightMode = lightCanvasMode_->currentData().toString() == QStringLiteral("custom")
        ? CanvasColorMode::Custom : CanvasColorMode::ThemeDefault;
    if (!result.darkCustom.isValid()) {
        result.darkCustom = defaultCanvasColor(UiTheme::Dark);
    }
    if (!result.lightCustom.isValid()) {
        result.lightCustom = defaultCanvasColor(UiTheme::Light);
    }

    return result;
}

PreviewBackgroundSettings SettingsDialog::selectedPreviewBackgroundSettings() const {
    PreviewBackgroundSettings result = previewBackgroundSettings_;
    const auto selectedMode = [](const QComboBox *mode) {
        const QString value = mode->currentData().toString();
        if (value == QStringLiteral("checkerboard")) {
            return PreviewBackgroundMode::Checkerboard;
        }
        if (value == QStringLiteral("custom")) {
            return PreviewBackgroundMode::Custom;
        }
        return PreviewBackgroundMode::ThemeDefault;
    };
    result.buffer.mode = selectedMode(bufferBackgroundMode_);
    result.layers.mode = selectedMode(layersBackgroundMode_);
    if (!result.buffer.custom.isValid()) {
        result.buffer.custom = defaultPreviewBackgroundColor(selectedTheme());
    }
    if (!result.layers.custom.isValid()) {
        result.layers.custom = defaultPreviewBackgroundColor(selectedTheme());
    }

    return result;
}

BehaviorSettings SettingsDialog::selectedBehaviorSettings() const {
    const BehaviorSettings defaults;
    BehaviorSettings result = behaviorSettings_;
    const QSize resolution = visibilityBorderResolution_->currentData().toSize();

    result.visibilityBordersEnabled = visibilityBordersCheck_->isChecked();
    result.displayAnchorsDuringTransformDrag = displayAnchorsDuringTransformDrag_->isChecked();
    result.generatePreviewsWithTransformations = generatePreviewsWithTransformations_->isChecked();
    result.visibilityBorderResolution = resolution.isValid()
        ? resolution : defaults.visibilityBorderResolution;
    result.nudgeStep = nudgeStep_->value();
    result.nudgeShiftStep = nudgeShiftStep_->value();
    result.liveryTextureScale = liveryTextureScale_->value();
    result.autosaveIntervalMinutes = autosaveIntervalMinutes_->value();
    result.valueEditingWheelEnabled = valueEditingWheelCheck_->isChecked();
    result.verticalToolbar = toolbarViewCombo_->currentData().toBool();
    result.separateOpacityAndSkewTools = separateOpacityAndSkewToolsCheck_->isChecked();
    result.gameFolder = gameFolder_->text().trimmed();
    result.discardModelOnLiveryOpen = discardModelOnLiveryOpen_->isChecked();
    result.loadCarTextures = loadCarTextures_->isChecked();

    return result;
}

cover::FillOptions SettingsDialog::selectedDifferentialFillOptions() const {
    cover::FillOptions result = differentialFillOptions_;
    result.budget = differentialBudget_->value();
    result.adamIterations = differentialAdamIterations_->value();
    result.restarts = differentialRestarts_->value();
    result.spillWeight = differentialSpillWeight_->value();
    result.epsArea = differentialEpsArea_->value();
    result.epsGain = differentialEpsGain_->value();
    result.epsSpill = differentialEpsSpill_->value();
    result.adamLearningRate = differentialAdamLearningRate_->value();
    result.inactivityTimeoutSeconds = differentialInactivityTimeout_->value();
    result.boundaryTolerance = differentialBoundaryTolerance_->value();
    result.outwardMargin = differentialOutwardMargin_->value();
    result.areaWindowRatio = differentialAreaWindowRatio_->value();
    result.targetCoverageRatio = differentialTargetCoverageRatio_->value();
    result.tverskyAlpha = differentialTverskyAlpha_->value();
    result.tverskyBeta = differentialTverskyBeta_->value();
    result.featureWeight = differentialFeatureWeight_->value();
    result.featureRestarts = differentialFeatureRestarts_->value();
    result.seed = static_cast<std::uint64_t>(differentialSeed_->value());
    result.useRouter = differentialUseRouter_->isChecked();
    result.useGpu = differentialUseGpu_->isChecked();
    result.useWeightedContour = differentialUseWeightedContour_->isChecked();

    return result;
}

SystemIconSet SettingsDialog::selectedSystemIconSet() const {
    return systemIconSetFromSettingsValue(systemIconSetCombo_->currentData().toString());
}

bool SettingsDialog::projectFileAssociationEnabled() const {
    return projectFileAssociationCheck_->isChecked();
}

QVector<ShortcutSettingsItem> SettingsDialog::shortcutItems() const {
    QVector<ShortcutSettingsItem> result = shortcuts_;
    for (int row = 0; row < result.size(); ++row) {
        if (row < shortcutEdits_.size()) {
            result[row].currentSequence = shortcutEdits_[row]->keySequence();
        }
    }
    return result;
}

void SettingsDialog::setThemeChangedCallback(std::function<void(UiTheme)> callback) {
    themeChangedCallback_ = std::move(callback);
}

void SettingsDialog::resetShortcutRow(int row) {
    if (row < 0 || row >= shortcuts_.size() || row >= shortcutEdits_.size()) {
        return;
    }
    shortcutEdits_[row]->setKeySequence(shortcuts_[row].defaultSequence);
}

void SettingsDialog::resetAllShortcuts() {
    for (int row = 0; row < shortcuts_.size(); ++row) {
        resetShortcutRow(row);
    }
}

void SettingsDialog::chooseCanvasColor(UiTheme theme) {
    const QColor current = theme == UiTheme::Light
        ? (canvasSettings_.lightCustom.isValid()
               ? canvasSettings_.lightCustom : defaultCanvasColor(UiTheme::Light))
        : (canvasSettings_.darkCustom.isValid()
               ? canvasSettings_.darkCustom : defaultCanvasColor(UiTheme::Dark));
    const QColor picked = QColorDialog::getColor(
        current, this, QStringLiteral("Canvas Color"));
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

void SettingsDialog::choosePreviewBackgroundColor(bool buffer) {
    PreviewBackground &background = buffer
        ? previewBackgroundSettings_.buffer : previewBackgroundSettings_.layers;
    const QColor current = background.custom.isValid()
        ? background.custom : defaultPreviewBackgroundColor(selectedTheme());
    const QColor picked = QColorDialog::getColor(
        current, this, buffer ? QStringLiteral("Buffer Background Color")
                              : QStringLiteral("Layer Background Color"));
    if (!picked.isValid()) {
        return;
    }
    background.custom = picked;
    QComboBox *mode = buffer ? bufferBackgroundMode_ : layersBackgroundMode_;
    mode->setCurrentIndex(mode->findData(QStringLiteral("custom")));
    updatePreviewBackgroundControls();
}

void SettingsDialog::chooseGuidelineColor() {
    const QColor current = behaviorSettings_.guidelineColor.isValid()
        ? behaviorSettings_.guidelineColor : QColor(0, 170, 255);
    const QColor picked = QColorDialog::getColor(
        current, this, QStringLiteral("Guideline Color"), QColorDialog::ShowAlphaChannel);
    if (!picked.isValid()) {
        return;
    }
    behaviorSettings_.guidelineColor = picked;
    updateCanvasColorControls();
}

void SettingsDialog::updateCanvasColorControls() {
    const auto updateButton = [](QPushButton *button,
                                 const QColor &color,
                                 bool customEnabled) {
        if (button == nullptr) {
            return;
        }
        button->setEnabled(customEnabled);
        button->setText(color.name(QColor::HexRgb).toUpper());
        button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                  .arg(color.name(QColor::HexRgb),
                                       color.lightness() < 128
                                           ? QStringLiteral("#ffffff")
                                           : QStringLiteral("#202225")));
    };
    const CanvasColorSettings settings = selectedCanvasSettings();
    updateButton(darkCanvasColorButton_,
                 settings.darkMode == CanvasColorMode::Custom
                     ? settings.darkCustom : defaultCanvasColor(UiTheme::Dark),
                 settings.darkMode == CanvasColorMode::Custom);
    updateButton(lightCanvasColorButton_,
                 settings.lightMode == CanvasColorMode::Custom
                     ? settings.lightCustom : defaultCanvasColor(UiTheme::Light),
                 settings.lightMode == CanvasColorMode::Custom);
    updateButton(guidelineColorButton_,
                 behaviorSettings_.guidelineColor.isValid()
                     ? behaviorSettings_.guidelineColor : QColor(0, 170, 255),
                 true);
}

void SettingsDialog::updatePreviewBackgroundControls() {
    const auto updateButton = [this](QPushButton *button,
                                     const PreviewBackground &background) {
        if (button == nullptr) {
            return;
        }
        const bool custom = background.mode == PreviewBackgroundMode::Custom;
        const QColor color = custom && background.custom.isValid()
            ? background.custom : defaultPreviewBackgroundColor(selectedTheme());
        button->setEnabled(custom);
        button->setText(color.name(QColor::HexRgb).toUpper());
        button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                                  .arg(color.name(QColor::HexRgb),
                                       color.lightness() < 128
                                           ? QStringLiteral("#ffffff")
                                           : QStringLiteral("#202225")));
    };
    const PreviewBackgroundSettings settings = selectedPreviewBackgroundSettings();
    updateButton(bufferBackgroundColorButton_, settings.buffer);
    updateButton(layersBackgroundColorButton_, settings.layers);
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
            validationLabel_->setText(
                QStringLiteral("Shortcut %1 is assigned to both %2 and %3.")
                    .arg(item.currentSequence.toString(QKeySequence::NativeText),
                         it.value(), item.label));
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
