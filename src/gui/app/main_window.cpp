#include "main_window.h"

#include "main_window_internal.h"

namespace gui {

using namespace mw_detail;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    theme_ = loadUiTheme();
    setWindowTitle(QStringLiteral("Forza Livery Studio"));
    resize(InitialWindowWidth, InitialWindowHeight);
    setAcceptDrops(true);

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
    setupProjectMenu();
    setupImgGenMenu();
    setupOptionsMenu();
    setupToolbar();
    setupWindowMenu();

    defaultLayoutState_ = saveState();
    restoreLayout();
    syncDockCollapseButtons();

    updateStatus();
}

MainWindow::~MainWindow()
{
    cancelGeneratedFill();
}
} // namespace gui
