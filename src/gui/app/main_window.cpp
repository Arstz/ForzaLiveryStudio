#include "main_window.h"

#include "main_window_internal.h"

#include <QLockFile>

namespace gui {

using namespace mw_detail;

namespace {

constexpr int kGeneratedFillElapsedIntervalMs = 250;
constexpr int kScratchSaveIntervalMs = 60 * 1000;

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
    theme_ = loadUiTheme();
    setWindowTitle(QStringLiteral("Forza Livery Studio"));
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
}

MainWindow::~MainWindow() {
    cancelActiveFills();
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (!defaultLayoutPending_) {
        return;
    }
    defaultLayoutPending_ = false;
    QTimer::singleShot(0, this, &MainWindow::applyDefaultLayoutSizes);
}
} // namespace gui
