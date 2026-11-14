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
    }
    toolBar_->installEventFilter(this);
    toolSettingsBar_->installEventFilter(this);
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
} // namespace gui
