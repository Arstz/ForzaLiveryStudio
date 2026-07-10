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
    setupOptionsMenu();
    setupToolbar();
    setupWindowMenu();

    // Capture the freshly built layout so Reset Layout can return to it, then
    // restore any previously saved layout (mirrors the Python _restore_layout).
    defaultLayoutState_ = saveState();
    restoreLayout();
    // restoreLayout() may drop docks into different areas than their install-time
    // fallback, so re-point every collapse arrow at its dock's actual area.
    syncDockCollapseButtons();

    updateStatus();
}
} // namespace gui
