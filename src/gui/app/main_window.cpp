#include "main_window.h"

#include "main_window_internal.h"

namespace gui {

using namespace mw_detail;

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

    setupCanvas();
    setupTreeView();
    setupDocks();

    creatorName_ = QSettings().value(QStringLiteral("header/creatorName")).toString();

    connectEditorStateSignals();
    setupFileMenu();
    setupEditMenu();
#if FH6_ENABLE_IMGGEN_MENU
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
