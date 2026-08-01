#include "main_window.h"

#include "main_window_internal.h"

namespace gui {

using namespace mw_detail;

namespace {

constexpr int kGeneratedFillElapsedIntervalMs = 250;

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
    theme_ = loadUiTheme();
    setWindowTitle(QStringLiteral("Forza Livery Studio"));
    resize(kInitialWindowWidth, kInitialWindowHeight);
    setAcceptDrops(true);

    keyBindings_ = new KeyBindingRouter(this, this);
    state_ = new EditorState(this);
    autosaveTimer_ = new QTimer(this);
    connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::autosaveProject);
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
    setupWindowMenu();

    defaultLayoutState_ = saveState();
    restoreLayout();
    syncDockCollapseButtons();

    updateStatus();
}

MainWindow::~MainWindow() {
    cancelActiveFills();
}
} // namespace gui
