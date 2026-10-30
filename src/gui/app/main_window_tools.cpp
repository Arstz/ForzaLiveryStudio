#include "main_window.h"
#include "tool_settings_widget.h"

#include <array>

namespace gui {
namespace {

class ToolSettingsScrollArea final : public QScrollArea {
public:
    explicit ToolSettingsScrollArea(QWidget *controls, QWidget *parent) : QScrollArea(parent) {
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setWidget(controls);
        controls->installEventFilter(this);
        connect(horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this]() { updateGeometry(); });
    }

    QSize sizeHint() const override {
        if (widget() == nullptr) return QSize(0, 0);
        const auto *bar = horizontalScrollBar();
        const int scrollHeight = bar->maximum() > bar->minimum() ? bar->sizeHint().height() : 0;

        return QSize(0, widget()->sizeHint().height() + scrollHeight);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        const bool handled = QScrollArea::eventFilter(watched, event);
        if (watched == widget() && event->type() == QEvent::LayoutRequest) updateGeometry();

        return handled;
    }
};

struct ToolFlag {
    const char *key;
    const char *action;
    const char *label;
    bool BehaviorSettings::*member;
};

const auto kToolFlags = std::to_array<ToolFlag>({
    {"moveToolAutoSelect", "toggle_move_auto_select", "Move: Auto Select", &BehaviorSettings::moveToolAutoSelect},
    {"automaticSnapping", "toggle_automatic_snapping", "Automatic snapping", &BehaviorSettings::automaticSnapping},
    {"selectionFlashEnabled", "toggle_selection_flash", "Select: Selection Flash", &BehaviorSettings::selectionFlashEnabled},
    {"pipetteAutoReturn", "toggle_pipette_auto_return", "Pipette: Auto Return", &BehaviorSettings::pipetteAutoReturn},
    {"autoSavePenPaths", "toggle_auto_save_pen_paths", "Pen: Save Path", &BehaviorSettings::autoSavePenPaths},
    {"autoSaveLiningPaths", "toggle_auto_save_lining_paths", "Lining: Save Path", &BehaviorSettings::autoSaveLiningPaths},
    {"displayAnchorsDuringTransformDrag", "", "", &BehaviorSettings::displayAnchorsDuringTransformDrag},
    {"selectShapesByBoundingBox", "", "", &BehaviorSettings::selectShapesByBoundingBox},
    {"showSelectedShapeOutlines", "", "", &BehaviorSettings::showSelectedShapeOutlines},
});

} // namespace

void MainWindow::setupToolSettings() {
    toolSettingsBar_ = new QToolBar(QStringLiteral("Tool Settings"), this);
    toolSettingsBar_->setObjectName(QStringLiteral("ToolSettingsBar"));
    toolSettingsBar_->setAllowedAreas(Qt::TopToolBarArea);
    toolSettingsBar_->setMovable(false);
    toolSettingsBar_->setFloatable(false);
    toolSettingsBar_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toolSettings_ = new ToolSettingsWidget;
    auto *scroll = new ToolSettingsScrollArea(toolSettings_, toolSettingsBar_);
    scroll->setObjectName(QStringLiteral("ToolSettingsPanel"));
    toolSettingsBar_->addWidget(scroll);
    positionToolSettings();
    toolSettings_->optionChanged = [this](const QString &key, const QVariant &value) { setToolOption(key, value); };
    toolSettings_->editingFinished = [this]() { canvas_->setFocus(Qt::OtherFocusReason); };
    setupToolOptionShortcuts();
    canvas_->setLiningWidthChangedCallback([this](double width) {
        toolSettings_->setValue(QStringLiteral("liningWidth"), width);
    });
    canvas_->setBucketToleranceChangedCallback([this](int tolerance) {
        toolSettings_->setValue(QStringLiteral("bucketTolerance"), tolerance);
    });
    toolSettings_->setTool(canvas_->tool());
    refreshToolSettings();
}

void MainWindow::setupToolOptionShortcuts() {
    const auto addToggleShortcut = [this](const QString &key, const QString &id, const QString &label) {
        auto *action = new QAction(label, toolSettings_);
        action->setCheckable(true);
        registerShortcutAction(action, id, label);
        addAction(action);
        connect(action, &QAction::toggled, this, [this, key](bool checked) { setToolOption(key, checked); });
    };
    for (const auto &flag : kToolFlags) {
        if (*flag.action) {
            addToggleShortcut(QString::fromLatin1(flag.key), QString::fromLatin1(flag.action), QString::fromLatin1(flag.label));
        }
    }
    addToggleShortcut(QStringLiteral("relativeMode"), QStringLiteral("toggle_transform_relative"), QStringLiteral("Transform: Relative Mode"));
    auto *fillModes = new QActionGroup(toolSettings_);
    const std::array<QString, 2> modeIds{QStringLiteral("select_analytic_contour_fill"), QStringLiteral("select_compact_contour_fill")};
    const std::array<QString, 2> modeLabels{QStringLiteral("Pen: Analytical Fill"), QStringLiteral("Pen: Compact Fill")};
    for (int index = 0; index < static_cast<int>(modeIds.size()); ++index) {
        auto *action = new QAction(modeLabels[index], toolSettings_);
        action->setCheckable(true);
        fillModes->addAction(action);
        registerShortcutAction(action, modeIds[index], modeLabels[index]);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, index]() { setToolOption(QStringLiteral("contourFillMode"), index); });
    }
}

void MainWindow::positionToolSettings() {
    if (toolSettingsBar_ == nullptr) return;
    if (toolBarArea(toolSettingsBar_) == Qt::TopToolBarArea
            && (toolBarArea(toolBar_) != Qt::TopToolBarArea || toolBarBreak(toolSettingsBar_))) {
        toolSettingsBar_->show();
        return;
    }
    removeToolBarBreak(toolSettingsBar_);
    removeToolBar(toolSettingsBar_);
    addToolBarBreak(Qt::TopToolBarArea);
    addToolBar(Qt::TopToolBarArea, toolSettingsBar_);
    toolSettingsBar_->show();
}

void MainWindow::refreshToolSettings() {
    if (toolSettings_ == nullptr) return;
    const auto settings = loadBehaviorSettings();
    const auto checkAction = [this](const QString &id, bool checked) {
        if (auto *action = findChild<QAction *>(id)) {
            const QSignalBlocker blocker(action);
            action->setChecked(checked);
        }
    };
    for (const auto &flag : kToolFlags) {
        const bool checked = settings.*flag.member;
        toolSettings_->setValue(QString::fromLatin1(flag.key), checked);
        if (*flag.action) checkAction(QString::fromLatin1(flag.action), checked);
    }
    toolSettings_->setValue(QStringLiteral("contourFillMode"), static_cast<int>(settings.contourFillMode));
    toolSettings_->setValue(QStringLiteral("bucketTolerance"), settings.bucketTolerance);
    toolSettings_->setValue(QStringLiteral("liningWidth"), canvas_->liningWidth());
    toolSettings_->setValue(QStringLiteral("liningOutsideMargin"), settings.liningOutsideMargin * 100.0);
    const bool relative = loadTransformModeSettings().relativeMode;
    toolSettings_->setValue(QStringLiteral("relativeMode"), relative);
    checkAction(QStringLiteral("toggle_transform_relative"), relative);
    checkAction(QStringLiteral("select_analytic_contour_fill"), settings.contourFillMode == ContourFillMode::Analytic);
    checkAction(QStringLiteral("select_compact_contour_fill"), settings.contourFillMode == ContourFillMode::CompactFit);
}

void MainWindow::setToolOption(const QString &key, const QVariant &value) {
    if (key == QStringLiteral("relativeMode")) {
        auto settings = loadTransformModeSettings();
        settings.relativeMode = value.toBool();
        saveTransformModeSettings(settings);
        canvas_->setTransformRelativeMode(settings.relativeMode);
        refreshToolSettings();
        return;
    }
    auto settings = loadBehaviorSettings();
    for (const auto &flag : kToolFlags) {
        if (key == QString::fromLatin1(flag.key)) settings.*flag.member = value.toBool();
    }
    if (key == QStringLiteral("contourFillMode")) settings.contourFillMode = static_cast<ContourFillMode>(value.toInt());
    if (key == QStringLiteral("bucketTolerance")) settings.bucketTolerance = value.toInt();
    if (key == QStringLiteral("liningWidth")) settings.liningWidth = value.toDouble();
    if (key == QStringLiteral("liningOutsideMargin")) settings.liningOutsideMargin = value.toDouble() / 100.0;
    applyBehaviorSettings(settings);
    if (key == QStringLiteral("bucketTolerance")) canvas_->setBucketTolerance(settings.bucketTolerance);
    if (key == QStringLiteral("liningWidth")) canvas_->setLiningWidth(settings.liningWidth);
    refreshToolSettings();
}

} // namespace gui
