#include "main_window.h"

#include "main_window_internal.h"
#include "title_menu_bar.h"

#include <QLockFile>

namespace gui {

using namespace mw_detail;

namespace {

constexpr int kGeneratedFillElapsedIntervalMs = 250;
constexpr int kScratchSaveIntervalMs = 60 * 1000;
constexpr int kLayoutSaveDelayMs = 500;

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
#ifdef Q_OS_WIN
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
#endif
    theme_ = loadUiTheme();
    setWindowTitle(QStringLiteral("Forza Livery Studio"));
    setMenuBar(createTitleMenuBar(*this));
    resize(screen()->availableGeometry().size() * kInitialWindowScreenFraction);
    setAcceptDrops(true);

    keyBindings_ = new KeyBindingRouter(this, this);
    state_ = new EditorState(this);
    autosaveTimer_ = new QTimer(this);
    connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::autosaveProject);
    scratchTimer_ = new QTimer(this);
    scratchTimer_->setInterval(kScratchSaveIntervalMs);
    connect(scratchTimer_, &QTimer::timeout, this, &MainWindow::saveScratchProject);
    initializeScratchSession();
    scratchTimer_->start();
    generatedFillElapsedTimer_ = new QTimer(this);
    generatedFillElapsedTimer_->setInterval(
        kGeneratedFillElapsedIntervalMs);
    connect(generatedFillElapsedTimer_, &QTimer::timeout,
            this, &MainWindow::refreshGeneratedFillElapsedTime);

    setupCanvas();
    setupTreeView();
    setupDocks();

    creatorName_ = QSettings().value(QStringLiteral("header/creatorName")).toString();

    connectEditorStateSignals();
    setupFileMenu();
    setupEditMenu();
#if FLS_ENABLE_IMGGEN_MENU
    setupImgGenMenu();
#endif
    setupOptionsMenu();
    setupToolbar();
    setupToolSettings();
    setupWindowMenu();

    defaultLayoutState_ = saveState();
    if (!restoreLayout()) {
        defaultLayoutPending_ = true;
        setWindowState(windowState() | Qt::WindowMaximized);
    }
    syncDockCollapseButtons();

    updateStatus();

    layoutSaveTimer_ = new QTimer(this);
    layoutSaveTimer_->setSingleShot(true);
    layoutSaveTimer_->setInterval(kLayoutSaveDelayMs);
    connect(layoutSaveTimer_, &QTimer::timeout, this, &MainWindow::saveLayout);
    for (QDockWidget *dock : dockWidgets_) {
        dock->installEventFilter(this);
        connect(dock, &QDockWidget::dockLocationChanged, this, &MainWindow::scheduleLayoutSave);
        connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::scheduleLayoutSave);
        connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::scheduleLayoutRepaint);
        connect(dock, &QDockWidget::visibilityChanged, this, &MainWindow::scheduleLayoutRepaint);
    }
    connect(this, &QMainWindow::tabifiedDockWidgetActivated, this, &MainWindow::scheduleLayoutRepaint);
    toolBar_->installEventFilter(this);
    toolSettingsBar_->installEventFilter(this);
    layoutInitialized_ = true;
    scheduleLayoutRepaint();
}

MainWindow::~MainWindow() {
    cancelActiveFills();
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (defaultLayoutPending_) {
        defaultLayoutPending_ = false;
        QTimer::singleShot(0, this, &MainWindow::applyDefaultLayoutSizes);
    }
    QTimer::singleShot(0, this, &MainWindow::scheduleLayoutSave);
}

void MainWindow::paintEvent(QPaintEvent *event) {
    QMainWindow::paintEvent(event);
    if (centralWidget() == nullptr) {
        return;
    }
    QPainter painter(this);
    const UiColors colors = uiColors(currentUiTheme());
    const QRect centralBounds = centralWidget()->geometry();
    QRect dockLayoutBounds = centralBounds;
    const int separatorExtent = style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, this);

    for (const QDockWidget *dock : dockWidgets_) {
        if (dock->isVisible() && !dock->isFloating()) {
            const QRect bounds = dock->geometry().intersected(rect());

            if (!bounds.isEmpty()) {
                dockLayoutBounds = dockLayoutBounds.united(bounds);
            }
        }
    }
    for (const DockAreaCollapseState &state : dockAreaCollapseStates_) {
        if (!state.collapsed || state.anchorDock == nullptr || !state.anchorDock->isVisible()
            || state.anchorDock->isFloating()) {
            continue;
        }
        QRect bounds = state.anchorDock->geometry();
        QRect separator;

        if (state.area == Qt::LeftDockWidgetArea || state.area == Qt::RightDockWidgetArea) {
            bounds.setTop(std::min(bounds.top(), centralBounds.top()));
            bounds.setBottom(std::max(bounds.bottom(), centralBounds.bottom()));
        } else if (state.area == Qt::TopDockWidgetArea || state.area == Qt::BottomDockWidgetArea) {
            const Qt::Corner leftCorner = state.area == Qt::TopDockWidgetArea ? Qt::TopLeftCorner : Qt::BottomLeftCorner;
            const Qt::Corner rightCorner = state.area == Qt::TopDockWidgetArea ? Qt::TopRightCorner : Qt::BottomRightCorner;
            const int left = corner(leftCorner) == state.area ? dockLayoutBounds.left() : centralBounds.left();
            const int right = corner(rightCorner) == state.area ? dockLayoutBounds.right() : centralBounds.right();

            bounds.setLeft(std::min(bounds.left(), left));
            bounds.setRight(std::max(bounds.right(), right));
        }
        switch (state.area) {
        case Qt::LeftDockWidgetArea:
            separator = QRect(bounds.right() + 1, bounds.top(), separatorExtent, bounds.height());
            break;
        case Qt::RightDockWidgetArea:
            separator = QRect(bounds.left() - separatorExtent, bounds.top(), separatorExtent, bounds.height());
            break;
        case Qt::TopDockWidgetArea:
            separator = QRect(bounds.left(), bounds.bottom() + 1, bounds.width(), separatorExtent);
            break;
        case Qt::BottomDockWidgetArea:
            separator = QRect(bounds.left(), bounds.top() - separatorExtent, bounds.width(), separatorExtent);
            break;
        default:
            continue;
        }
        painter.fillRect(bounds, colors.secondary);
        painter.fillRect(separator, colors.resizeHandle);
    }
}
} // namespace gui
