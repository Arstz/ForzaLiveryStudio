#include "main_window.h"

#include "clipboard_buffer_widget.h"
#include "dock_chrome.h"
#include "fh6_core.h"
#include "gui_assets.h"
#include "header_metadata_widget.h"
#include "image_io.h"
#include "import_locations.h"
#include "layer_state_delegate.h"
#include "layer_tree_view.h"
#include "livery_section_bar.h"
#include "perf_utils.h"
#include "project_codec.h"
#include "property_panel.h"
#include "shape_geometry_store.h"
#include "settings_dialog.h"
#include "font_glyphs.h"
#include "shape_registry.h"
#include "shapes_browser_widget.h"
#include "theme_manager.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QScrollArea>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QAbstractButton>
#include <QCloseEvent>
#include <QDebug>
#include <QElapsedTimer>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QRectF>
#include <QMimeData>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QSettings>
#include <QShortcut>
#include <QSize>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QTransform>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QItemSelectionModel>
#include <QKeyEvent>

#include <algorithm>
#include <exception>
#include <functional>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <wincodec.h>
#endif

namespace gui {

namespace {

// Constructor layout constants (previously inlined magic numbers).
constexpr int InitialWindowWidth = 1200;
constexpr int InitialWindowHeight = 780;
constexpr int TreeIconExtent = 64;
constexpr int ToolbarIconExtent = 18;
constexpr int DockSplitterHandleWidth = 6;
constexpr int DetailsLabelMargin = 10;

QString shortcutActionText(const QString &id, const QString &label, const QKeySequence &shortcut)
{
    if (!id.startsWith(QStringLiteral("tool_")) || shortcut.isEmpty()) {
        return label;
    }
    return QStringLiteral("%1 (%2)").arg(label, shortcut.toString(QKeySequence::NativeText));
}

QString safeLayerGroupName(QString name)
{
    name = name.trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("Project");
    }
    for (QChar &ch : name) {
        if (ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char(':') || ch == QLatin1Char('"')
            || ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char('|') || ch == QLatin1Char('?')
            || ch == QLatin1Char('*') || ch.category() == QChar::Other_Control) {
            ch = QLatin1Char('_');
        }
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("Project") : name;
}

QString layerGroupExportFolder(const QString &pickedFolder, const QString &projectName)
{
    const QFileInfo pickedInfo(pickedFolder);
    if (pickedInfo.fileName().startsWith(QStringLiteral("LayerGroup_")) && pickedInfo.fileName().size() > 11) {
        return pickedInfo.absoluteFilePath();
    }

    QDir base(pickedFolder);
    const QString baseName = QStringLiteral("LayerGroup_%1").arg(safeLayerGroupName(projectName));
    QString candidate = baseName;
    int suffix = 2;
    while (base.exists(candidate)) {
        candidate = QStringLiteral("%1_%2").arg(baseName).arg(suffix++);
    }
    if (!base.mkpath(candidate)) {
        throw std::runtime_error(("could not create export folder: " + base.filePath(candidate)).toStdString());
    }
    return base.filePath(candidate);
}

QString entryNameForId(const fh6::Project &project, const QString &id)
{
    for (const fh6::LayerGroup &group : project.groups) {
        if (group.id == id) {
            return group.name;
        }
    }
    for (const fh6::ShapeLayer &layer : project.layers) {
        if (layer.id == id) {
            return layer.name;
        }
    }
    return {};
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    theme_ = loadUiTheme();
    setWindowTitle(QStringLiteral("Forza Livery Studio"));
    resize(InitialWindowWidth, InitialWindowHeight);
    setAcceptDrops(true);

    state_ = new EditorState(this);

    setupCanvas();
    setupTreeView();
    setupDocks();

    creatorName_ = QSettings().value(QStringLiteral("header/creatorName")).toString();

    connectEditorStateSignals();
    setupFileMenu();
    setupEditMenu();
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

void MainWindow::setupCanvas()
{
    canvas_ = new ProjectCanvas(this);
    canvas_->setEditorState(state_);
    canvas_->setTransformRelativeMode(!loadTransformModeSettings().relativeMode);
    applyBehaviorSettings(loadBehaviorSettings(), false);
    QString geometryError;
    if (!canvas_->loadGeometry(&geometryError)) {
        statusBar()->showMessage(geometryError);
    }
    setCentralWidget(canvas_);
}

void MainWindow::setupTreeView()
{
    treeModel_ = new LayerTreeModel(this);
    treeModel_->setEditorState(state_);
    tree_ = new LayerTreeView(this);
    tree_->setModel(treeModel_);
    tree_->setHeaderHidden(false);
    tree_->setIconSize(QSize(TreeIconExtent, TreeIconExtent));
    tree_->setUniformRowHeights(false);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setExpandsOnDoubleClick(false);
    tree_->header()->setStretchLastSection(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setItemDelegate(new LayerStateDelegate(state_, tree_));
    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
        updateSelectionFromTree();
    });

    // C_livery section selector ("tabs"), mirroring the shapes-browser category
    // list. Hidden unless a livery is loaded. Selecting a section swaps the tree
    // contents and the single section drawn on the canvas.
    sectionBar_ = new LiverySectionBar(this);
    connect(sectionBar_, &LiverySectionBar::sectionActivated, this, &MainWindow::setActiveSection);
}

QDockWidget *MainWindow::addPanelDock(const QString &title, const QString &objectName,
                                      const QString &iconName, Qt::DockWidgetArea area, QWidget *content,
                                      bool scrollable)
{
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    setDockTitleIcon(dock, iconName);
    installDockAreaCollapseButton(dock, area);
    if (scrollable) {
        auto *scroll = new QScrollArea(dock);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        dock->setWidget(scroll);
    } else {
        dock->setWidget(content);
    }
    addDockWidget(area, dock);
    dockWidgets_.push_back(dock);
    return dock;
}

void MainWindow::setupDocks()
{
    auto *layersContainer = new QSplitter(Qt::Vertical, this);
    layersContainer->setChildrenCollapsible(false);
    layersContainer->setHandleWidth(DockSplitterHandleWidth);
    layersContainer->addWidget(sectionBar_);
    layersContainer->addWidget(tree_);
    layersContainer->setStretchFactor(1, 1);
    installSplitterResizeCursor(layersContainer);

    auto *layersDock = addPanelDock(QStringLiteral("Layers"), QStringLiteral("LayersDock"),
                                    QStringLiteral("WidgetLayers.xpm"), Qt::RightDockWidgetArea, layersContainer);

    clipboardWidget_ = new ClipboardBufferWidget(this);
    auto *clipboardDock = addPanelDock(QStringLiteral("Buffer"), QStringLiteral("BufferDock"),
                                       QStringLiteral("WidgetBuffer.xpm"), Qt::RightDockWidgetArea, clipboardWidget_);
    splitDockWidget(layersDock, clipboardDock, Qt::Vertical);

    details_ = new QLabel(this);
    details_->setMargin(DetailsLabelMargin);
    details_->setWordWrap(true);
    auto *detailsDock = addPanelDock(QStringLiteral("Project"), QStringLiteral("ProjectDock"),
                                     QStringLiteral("WidgetProject.xpm"), Qt::RightDockWidgetArea, details_);

    headerMetadata_ = new HeaderMetadataWidget(this);
    headerMetadata_->setApplyCallback([this]() { applyHeaderMetadata(); });
    headerMetadataDock_ = addPanelDock(QStringLiteral("Header"), QStringLiteral("HeaderMetadataDock"),
                                       QStringLiteral("WidgetProject.xpm"), Qt::RightDockWidgetArea, headerMetadata_, true);
    tabifyDockWidget(detailsDock, headerMetadataDock_);
    detailsDock->raise();

    properties_ = new PropertyPanel(state_, this);
    // Multi/group transforms pivot about the selection's visual bounding box, which
    // needs shape sizes from the canvas geometry store.
    properties_->setSpriteSizeFn([this](int id) {
        return canvas_ != nullptr ? canvas_->shapeSize(id) : QSizeF(0.0, 0.0);
    });
    applyBehaviorSettings(loadBehaviorSettings(), false);
    auto *propertiesDock = addPanelDock(QStringLiteral("Properties"), QStringLiteral("PropertiesDock"),
                                        QStringLiteral("WidgetProperties.xpm"), Qt::LeftDockWidgetArea, properties_, true);

    shapesBrowser_ = new ShapesBrowserWidget(this);
    shapesBrowser_->setShapeSelectedCallback([this](int shapeId) { insertShape(shapeId); });
    shapesBrowser_->setCustomGroupSelectedCallback([this](const CustomShapeGroup &group) { insertCustomGroup(group.name, group.clipboard); });
    shapesBrowser_->setAddCurrentSelectionCallback([this]() { saveCurrentSelectionAsCustomGroup(); });
    auto *shapesDock = addPanelDock(QStringLiteral("Shapes"), QStringLiteral("ShapesDock"),
                                    QStringLiteral("WidgetShapesBrowser.xpm"), Qt::LeftDockWidgetArea, shapesBrowser_);
    splitDockWidget(propertiesDock, shapesDock, Qt::Vertical);
}

void MainWindow::connectEditorStateSignals()
{
    connect(state_, &EditorState::selectionChanged, this, [this]() {
        updateLastSelectedShapeDefaults();
        syncTreeSelectionFromIds();
        canvas_->invalidateSelectionCache();
        canvas_->update();
        refreshSelectionProperties();
    });
    connect(state_, &EditorState::projectGeometryChanged, this, &MainWindow::noteProjectGeometryChanged);
    connect(state_, &EditorState::canvasRepaintRequested, this, [this]() {
        canvas_->invalidateSceneCache();
        canvas_->update();
    });
    connect(state_, &EditorState::projectStructureChanged, this, &MainWindow::noteProjectStructureChanged);
    connect(state_, &EditorState::clipboardChanged, this, &MainWindow::updateClipboardWidget);
    connect(state_, &EditorState::toolNameChanged, this, &MainWindow::setToolName);
    connect(state_, &EditorState::modifiedChanged, this, [this]() { updateWindowTitle(); });
    connect(state_, &EditorState::projectReset, this, [this]() {
        haveLastSelectedShapeDefaults_ = false;
        lastSelectedShapeColor_ = {255, 255, 255, 255};
        lastSelectedShapeScaleX_ = 1.0;
        lastSelectedShapeScaleY_ = 1.0;
        autoExpandedTreeIndexes_.clear();
        autoExpandedGroupIds_.clear();
        canvas_->setProject(project());
        updateClipboardWidget();
        updateStatus();
        refreshHeaderMetadataWidget();
    });
}

void MainWindow::setupFileMenu()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto addShortcutEntry = [this, fileMenu](const QString &text, const QString &id, const QString &label,
                                             const QKeySequence &shortcut, auto slot) {
        QAction *action = fileMenu->addAction(text);
        registerShortcutAction(action, id, label, shortcut);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    auto addIconEntry = [this, fileMenu](const QString &iconName, const QString &text, auto slot) {
        QAction *action = fileMenu->addAction(assetIcon(iconName), text);
        trackIconAction(action, iconName);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    addShortcutEntry(QStringLiteral("&New Project"), QStringLiteral("new_project"),
                     QStringLiteral("New Project"), QKeySequence::New, &MainWindow::newProjectDialog);
    addShortcutEntry(QStringLiteral("&Open Project..."), QStringLiteral("open_project_json"),
                     QStringLiteral("Open Project"), QKeySequence::Open, &MainWindow::loadProjectJsonDialog);
    recentProjectMenu_ = fileMenu->addMenu(QStringLiteral("Open &Recent Project"));
    refreshRecentProjectJsonMenu();
    addShortcutEntry(QStringLiteral("&Save Project..."), QStringLiteral("save_project_json"),
                     QStringLiteral("Save Project"), QKeySequence::Save, &MainWindow::saveProjectJsonDialog);
    fileMenu->addSeparator();
    addIconEntry(QStringLiteral("MenuOpenCGroup.xpm"), QStringLiteral("&Import..."), &MainWindow::importFileDialog);
    addIconEntry(QStringLiteral("WidgetProject.xpm"), QStringLiteral("Import &Guide Layer..."), &MainWindow::importGuideLayerDialog);
    addIconEntry(QStringLiteral("MenuExportFlat.xpm"), QStringLiteral("&Export..."), &MainWindow::exportDialog);
    fileMenu->addSeparator();
    addIconEntry(QStringLiteral("MenuExit.xpm"), QStringLiteral("E&xit"), &QWidget::close);
}

void MainWindow::setupEditMenu()
{
    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    // Application-context entries are also added to the window itself so their
    // shortcuts fire regardless of which widget has focus.
    auto addEditEntry = [this, editMenu](const QString &text, const QString &id, const QString &label,
                                         const QKeySequence &shortcut, const QString &iconName, auto slot,
                                         Qt::ShortcutContext context = Qt::ApplicationShortcut,
                                         bool mirroredIcon = false) {
        QAction *action = editMenu->addAction(mirroredIcon ? mirroredAssetIcon(iconName) : assetIcon(iconName), text);
        registerShortcutAction(action, id, label, shortcut, iconName, mirroredIcon, context);
        if (context == Qt::ApplicationShortcut) {
            addAction(action);
        }
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    addEditEntry(QStringLiteral("&Undo"), QStringLiteral("undo"), QStringLiteral("Undo"),
                 QKeySequence::Undo, QStringLiteral("MenuRevert.xpm"), &MainWindow::undo, Qt::WindowShortcut);
    addEditEntry(QStringLiteral("&Redo"), QStringLiteral("redo"), QStringLiteral("Redo"),
                 QKeySequence::Redo, QStringLiteral("MenuRevert.xpm"), &MainWindow::redo, Qt::WindowShortcut, true);
    editMenu->addSeparator();
    addEditEntry(QStringLiteral("&Copy"), QStringLiteral("copy"), QStringLiteral("Copy"),
                 QKeySequence::Copy, QStringLiteral("MenuCopy.xpm"), &MainWindow::copySelection);
    addEditEntry(QStringLiteral("Cu&t"), QStringLiteral("cut"), QStringLiteral("Cut"),
                 QKeySequence::Cut, QStringLiteral("MenuCut.xpm"), &MainWindow::cutSelection);
    addEditEntry(QStringLiteral("&Paste"), QStringLiteral("paste"), QStringLiteral("Paste"),
                 QKeySequence::Paste, QStringLiteral("MenuPaste.xpm"), &MainWindow::pasteClipboard);
    addEditEntry(QStringLiteral("&Group / Ungroup"), QStringLiteral("group_ungroup"), QStringLiteral("Group / Ungroup"),
                 QKeySequence(Qt::CTRL | Qt::Key_G), QStringLiteral("MenuGroup.xpm"), &MainWindow::groupOrUngroupSelection);
    addEditEntry(QStringLiteral("Ungroup &Flat"), QStringLiteral("ungroup_flat"), QStringLiteral("Ungroup Flat"),
                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), QStringLiteral("MenuUngroupFlat.xpm"), &MainWindow::ungroupSelectionFlat);
    addEditEntry(QStringLiteral("Fold All Groups"), QStringLiteral("fold_all_groups"), QStringLiteral("Fold All Groups"),
                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), QStringLiteral("MenuFoldAllGroups.xpm"), &MainWindow::collapseAllGroups);
    addEditEntry(QStringLiteral("&Delete Selected"), QStringLiteral("delete_selected"), QStringLiteral("Delete Selected"),
                 QKeySequence(Qt::Key_Delete), QStringLiteral("MenuDelete.xpm"), &MainWindow::deleteSelectedLayers);
    editMenu->addSeparator();
    addEditEntry(QStringLiteral("Stamp (Duplicate in Place)"), QStringLiteral("stamp"), QStringLiteral("Stamp"),
                 QKeySequence(Qt::Key_Y), QStringLiteral("MenuPaste.xpm"), &MainWindow::stampSelection);
}

void MainWindow::setupOptionsMenu()
{
    auto *optionsMenu = menuBar()->addMenu(QStringLiteral("&Options"));
    auto addBehaviorOption = [this, optionsMenu](const QString &text, bool BehaviorSettings::*member) {
        QAction *action = optionsMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(loadBehaviorSettings().*member);
        connect(action, &QAction::toggled, this, [this, member](bool checked) {
            BehaviorSettings settings = loadBehaviorSettings();
            settings.*member = checked;
            applyBehaviorSettings(settings);
        });
        return action;
    };
    addBehaviorOption(QStringLiteral("Use Last Selected Color for New Shapes"), &BehaviorSettings::insertShapeWithLastSelectedColor);
    addBehaviorOption(QStringLiteral("Use Last Selected Shape Scale for New Shapes"), &BehaviorSettings::insertShapeWithLastSelectedScale);
    addBehaviorOption(QStringLiteral("Show Property Debug"), &BehaviorSettings::showPropertyDebug);
    addBehaviorOption(QStringLiteral("Move Tool Auto-Select"), &BehaviorSettings::moveToolAutoSelect);
    QAction *flashOption = addBehaviorOption(QStringLiteral("Flash Selected Layers"), &BehaviorSettings::selectionFlashEnabled);
    registerShortcutAction(flashOption, QStringLiteral("toggle_selection_flash"), QStringLiteral("Flash Selected Layers"), QKeySequence(QStringLiteral("\\")), QString(), false, Qt::ApplicationShortcut);
    QAction *transformRelativeOption = optionsMenu->addAction(QStringLiteral("Transform Relative Mode"));
    transformRelativeOption->setCheckable(true);
    const TransformModeSettings initialTransformSettings = loadTransformModeSettings();
    transformRelativeOption->setChecked(initialTransformSettings.relativeMode);
    auto applyTransformRelativeOption = [this](bool checked) {
        TransformModeSettings settings = loadTransformModeSettings();
        settings.relativeMode = checked;
        saveTransformModeSettings(settings);
        canvas_->setTransformRelativeMode(!checked);
    };
    connect(transformRelativeOption, &QAction::toggled, this, applyTransformRelativeOption);
    canvas_->setTransformRelativeMode(!initialTransformSettings.relativeMode);
}

void MainWindow::setupToolbar()
{
    auto *toolBar = addToolBar(QStringLiteral("Tools"));
    toolBar->setObjectName(QStringLiteral("MainToolBar"));
    toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBar->setIconSize(QSize(ToolbarIconExtent, ToolbarIconExtent));
    auto *toolGroup = new QActionGroup(toolBar);
    toolGroup->setExclusive(true);
    auto addTool = [this, toolBar](const QString &label, const QString &tool, const QKeySequence &shortcut, const QString &iconName) {
        QAction *action = toolBar->addAction(assetIcon(iconName), label);
        action->setCheckable(true);
        registerShortcutAction(action, QStringLiteral("tool_%1").arg(tool), label, shortcut, iconName, false, Qt::ApplicationShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, tool]() { canvas_->setTool(tool); });
        return action;
    };
    QAction *selectTool = addTool(QStringLiteral("Select"), QStringLiteral("select"), QKeySequence(Qt::Key_S), QStringLiteral("ToolbarSelect.xpm"));
    toolGroup->addAction(selectTool);
    toolGroup->addAction(addTool(QStringLiteral("Move"), QStringLiteral("move"), QKeySequence(Qt::Key_V), QStringLiteral("ToolbarMove.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Marquee"), QStringLiteral("marquee"), QKeySequence(Qt::Key_F), QStringLiteral("ToolbarMarquee.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Transform"), QStringLiteral("transform"), QKeySequence(Qt::Key_T), QStringLiteral("ToolbarScale.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Rotate"), QStringLiteral("rotate"), QKeySequence(Qt::Key_R), QStringLiteral("ToolbarRotate.xpm")));
    selectTool->setChecked(true);
    toolBar->addSeparator();
    QAction *placeTextAction = toolBar->addAction(assetIcon(QStringLiteral("PropertyName.xpm")), QStringLiteral("Place Text"));
    trackIconAction(placeTextAction, QStringLiteral("PropertyName.xpm"));
    connect(placeTextAction, &QAction::triggered, this, [this]() { placeTextDialog(); });
    QAction *addGuideAction = toolBar->addAction(assetIcon(QStringLiteral("WidgetProject.xpm")), QStringLiteral("Add Guide Layer"));
    trackIconAction(addGuideAction, QStringLiteral("WidgetProject.xpm"));
    connect(addGuideAction, &QAction::triggered, this, [this]() { importGuideLayerDialog(); });
}

void MainWindow::setupWindowMenu()
{
    auto *windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
    for (QDockWidget *dock : dockWidgets_) {
        windowMenu->addAction(dock->toggleViewAction());
    }
    windowMenu->addSeparator();
    auto *saveLayoutAction = windowMenu->addAction(QStringLiteral("Save &Layout"));
    connect(saveLayoutAction, &QAction::triggered, this, [this]() { saveLayout(); });
    auto *resetLayoutAction = windowMenu->addAction(QStringLiteral("&Reset Layout"));
    connect(resetLayoutAction, &QAction::triggered, this, [this]() { resetLayout(); });
    windowMenu->addSeparator();
    auto *settingsAction = windowMenu->addAction(QStringLiteral("&Settings..."));
    registerShortcutAction(settingsAction, QStringLiteral("settings"), QStringLiteral("Settings"), QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(settingsAction, &QAction::triggered, this, [this]() { showSettingsDialog(); });
}

QAction *MainWindow::trackIconAction(QAction *action, const QString &iconName, bool mirroredIcon)
{
    if (action == nullptr || iconName.isEmpty()) {
        return action;
    }
    iconActions_.push_back({action, iconName, mirroredIcon});
    action->setIcon(mirroredIcon ? mirroredAssetIcon(iconName) : assetIcon(iconName));
    return action;
}

QAction *MainWindow::registerShortcutAction(QAction *action,
                                            const QString &id,
                                            const QString &label,
                                            const QKeySequence &defaultShortcut,
                                            const QString &iconName,
                                            bool mirroredIcon,
                                            Qt::ShortcutContext context)
{
    if (action == nullptr) {
        return nullptr;
    }
    if (!iconName.isEmpty()) {
        trackIconAction(action, iconName, mirroredIcon);
    }
    const QString settingsKey = QStringLiteral("shortcuts/%1").arg(id);
    const QString stored = QSettings().value(settingsKey).toString();
    action->setShortcut(stored.isEmpty() ? defaultShortcut : QKeySequence(stored));
    action->setShortcutContext(context);
    refreshShortcutActionText(action, id, label);
    shortcutActions_.push_back({id, label, defaultShortcut, action});
    return action;
}

QVector<ShortcutSettingsItem> MainWindow::shortcutSettingsItems() const
{
    QVector<ShortcutSettingsItem> items;
    items.reserve(shortcutActions_.size());
    for (const ShortcutAction &binding : shortcutActions_) {
        if (binding.action == nullptr) {
            continue;
        }
        items.push_back({binding.id, binding.label, binding.defaultShortcut, binding.action->shortcut()});
    }
    return items;
}

void MainWindow::applyShortcutSettings(const QVector<ShortcutSettingsItem> &items)
{
    QHash<QString, QKeySequence> byId;
    for (const ShortcutSettingsItem &item : items) {
        byId.insert(item.id, item.currentSequence);
    }
    for (const ShortcutAction &binding : shortcutActions_) {
        const auto it = byId.constFind(binding.id);
        if (binding.action == nullptr || it == byId.constEnd()) {
            continue;
        }
        binding.action->setShortcut(it.value());
        refreshShortcutActionText(binding.action, binding.id, binding.label);
        const QString settingsKey = QStringLiteral("shortcuts/%1").arg(binding.id);
        if (it.value() == binding.defaultShortcut) {
            QSettings().remove(settingsKey);
        } else {
            QSettings().setValue(settingsKey, it.value().toString(QKeySequence::PortableText));
        }
    }
}

void MainWindow::applyTheme(UiTheme theme, bool save)
{
    theme_ = theme;
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        applyUiTheme(*app, theme);
    }
    if (save) {
        saveUiTheme(theme);
    }
    refreshThemedIcons();
    if (shapesBrowser_ != nullptr) {
        shapesBrowser_->refreshTheme();
    }
    if (sectionBar_ != nullptr) {
        sectionBar_->refreshTheme();
    }
    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        refreshDockTitleIcon(dock);
    }
    for (QLabel *label : findChildren<QLabel *>()) {
        const QString iconName = label->property("fh6PropertyIconName").toString();
        if (!iconName.isEmpty()) {
            label->setPixmap(assetIcon(iconName).pixmap(14, 14));
        }
    }
    if (canvas_ != nullptr) {
        canvas_->setCanvasColor(canvasColorForTheme(theme, loadCanvasColorSettings()));
    }
}

void MainWindow::refreshThemedIcons()
{
    for (const IconAction &binding : iconActions_) {
        if (binding.action != nullptr) {
            binding.action->setIcon(binding.mirrored ? mirroredAssetIcon(binding.iconName) : assetIcon(binding.iconName));
        }
    }
}

void MainWindow::applyBehaviorSettings(const BehaviorSettings &settings, bool save)
{
    if (properties_ != nullptr) {
        properties_->setDebugVisible(settings.showPropertyDebug);
    }
    if (canvas_ != nullptr) {
        canvas_->setMoveToolAutoSelect(settings.moveToolAutoSelect);
        canvas_->setSelectionFlashEnabled(settings.selectionFlashEnabled);
    }
    if (save) {
        saveBehaviorSettings(settings);
    }
}

void MainWindow::refreshShortcutActionText(QAction *action, const QString &id, const QString &label) const
{
    if (action == nullptr) {
        return;
    }
    action->setText(shortcutActionText(id, label, action->shortcut()));
}

void MainWindow::showSettingsDialog()
{
    const UiTheme originalTheme = theme_;
    SettingsDialog dialog(theme_, loadCanvasColorSettings(), shortcutSettingsItems(), this);
    dialog.setThemeChangedCallback([this](UiTheme theme) {
        applyTheme(theme, false);
    });
    if (dialog.exec() != QDialog::Accepted) {
        applyTheme(originalTheme, false);
        return;
    }
    applyShortcutSettings(dialog.shortcutItems());
    saveCanvasColorSettings(dialog.selectedCanvasSettings());
    applyTheme(dialog.selectedTheme());
}

QVector<QDockWidget *> MainWindow::dockWidgetsInArea(Qt::DockWidgetArea area) const
{
    QVector<QDockWidget *> docks;
    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        if (dock != nullptr
            && !dock->isFloating()
            && dockWidgetArea(dock) == area) {
            docks.push_back(dock);
        }
    }
    std::sort(docks.begin(), docks.end(), [area](const QDockWidget *left, const QDockWidget *right) {
        const QRect leftGeometry = left != nullptr ? left->geometry() : QRect();
        const QRect rightGeometry = right != nullptr ? right->geometry() : QRect();
        if (area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea) {
            if (leftGeometry.left() != rightGeometry.left()) {
                return leftGeometry.left() < rightGeometry.left();
            }
            return leftGeometry.top() < rightGeometry.top();
        }
        if (leftGeometry.top() != rightGeometry.top()) {
            return leftGeometry.top() < rightGeometry.top();
        }
        return leftGeometry.left() < rightGeometry.left();
    });
    return docks;
}

void MainWindow::installDockAreaCollapseButton(QDockWidget *dock, Qt::DockWidgetArea fallbackArea)
{
    QToolButton *button = addDockAreaCollapseButton(dock);
    if (button == nullptr) {
        return;
    }
    configureDockAreaCollapseButton(button, fallbackArea, false);
    dockCollapseButtons_.push_back({dock, button, fallbackArea});
    connect(button, &QToolButton::clicked, this, [this, dock, fallbackArea, button]() {
        Qt::DockWidgetArea area = fallbackArea;
        if (dock != nullptr && !dock->isFloating()) {
            const Qt::DockWidgetArea currentArea = dockWidgetArea(dock);
            if (currentArea != Qt::NoDockWidgetArea) {
                area = currentArea;
            }
        }
        toggleDockAreaCollapsed(area, dock, button);
    });
    // Keep the arrow glyph pointing at the dock's real area: dockLocationChanged
    // fires both on layout restore and when the user re-anchors the dock.
    if (dock != nullptr) {
        connect(dock, &QDockWidget::dockLocationChanged, this,
                [this, dock](Qt::DockWidgetArea area) { updateDockCollapseButton(dock, area); });
    }
}

void MainWindow::updateDockCollapseButton(QDockWidget *dock, Qt::DockWidgetArea area)
{
    for (const DockCollapseButton &entry : dockCollapseButtons_) {
        if (entry.dock != dock || entry.button == nullptr) {
            continue;
        }
        Qt::DockWidgetArea effectiveArea = area;
        if (effectiveArea == Qt::NoDockWidgetArea) {
            if (dock != nullptr && !dock->isFloating()) {
                effectiveArea = dockWidgetArea(dock);
            }
            if (effectiveArea == Qt::NoDockWidgetArea) {
                effectiveArea = entry.fallbackArea;
            }
        }
        bool collapsed = false;
        for (const DockAreaCollapseState &state : dockAreaCollapseStates_) {
            if (state.area == effectiveArea) {
                collapsed = state.collapsed;
                break;
            }
        }
        configureDockAreaCollapseButton(entry.button, effectiveArea, collapsed);
        return;
    }
}

void MainWindow::syncDockCollapseButtons()
{
    for (const DockCollapseButton &entry : dockCollapseButtons_) {
        updateDockCollapseButton(entry.dock, Qt::NoDockWidgetArea);
    }
}

void MainWindow::toggleDockAreaCollapsed(Qt::DockWidgetArea area, QDockWidget *anchorDock, QToolButton *anchorButton)
{
    for (DockAreaCollapseState &control : dockAreaCollapseStates_) {
        if (control.area != area) {
            continue;
        }
        if (control.collapsed) {
            if (control.anchorDock != nullptr && control.anchorDock->widget() != nullptr && control.anchorWidgetWasVisible) {
                control.anchorDock->widget()->show();
            }
            for (QWidget *widget : control.hiddenTitleWidgets) {
                if (widget != nullptr) {
                    widget->show();
                }
            }
            control.hiddenTitleWidgets.clear();
            for (QDockWidget *dock : control.hiddenDocks) {
                if (dock != nullptr) {
                    dock->show();
                }
            }
            // Mark the area expanded before restoreState(): re-showing the sibling
            // docks emits dockLocationChanged, and updateDockCollapseButton() reads
            // this flag to decide each arrow's direction.
            control.collapsed = false;
            if (!control.layoutState.isEmpty()) {
                restoreState(control.layoutState);
            }
            for (QDockWidget *dock : control.hiddenDocks) {
                if (dock != nullptr) {
                    dock->show();
                }
            }
            control.hiddenDocks.clear();
            control.layoutState.clear();
            control.anchorDock = nullptr;
            control.anchorButton = nullptr;
            control.anchorWidgetWasVisible = false;
            // Correct every button in the area, not just the anchor's.
            syncDockCollapseButtons();
        } else {
            control.hiddenDocks.clear();
            control.anchorDock = anchorDock;
            control.anchorButton = anchorButton;
            control.anchorWidgetWasVisible = anchorDock != nullptr && anchorDock->widget() != nullptr && anchorDock->widget()->isVisible();
            control.layoutState = saveState();
            for (QDockWidget *dock : dockWidgetsInArea(area)) {
                if (dock != nullptr && dock != anchorDock && !dock->isHidden()) {
                    control.hiddenDocks.push_back(dock);
                }
            }
            if (control.hiddenDocks.isEmpty() && anchorDock == nullptr) {
                control.layoutState.clear();
                control.anchorDock = nullptr;
                control.anchorButton = nullptr;
                return;
            }
            for (QDockWidget *dock : control.hiddenDocks) {
                dock->hide();
            }
            if (anchorDock != nullptr && anchorDock->widget() != nullptr) {
                anchorDock->widget()->hide();
            }
            if (anchorDock != nullptr && anchorDock->titleBarWidget() != nullptr) {
                for (QWidget *widget : anchorDock->titleBarWidget()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
                    if (widget != nullptr && widget != anchorButton && widget->isVisible()) {
                        control.hiddenTitleWidgets.push_back(widget);
                        widget->hide();
                    }
                }
            }
            control.collapsed = true;
            configureDockAreaCollapseButton(anchorButton, area, true);
        }
        return;
    }

    DockAreaCollapseState state;
    state.area = area;
    dockAreaCollapseStates_.push_back(state);
    toggleDockAreaCollapsed(area, anchorDock, anchorButton);
}

bool MainWindow::event(QEvent *event)
{
    if (event != nullptr && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) && canvas_ != nullptr) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            if (keyEvent->isAutoRepeat()) {
                event->accept();
                return true;
            }
            QCoreApplication::sendEvent(canvas_, event);
            event->accept();
            return true;
        }
    }
    const bool result = QMainWindow::event(event);
    switch (event->type()) {
    case QEvent::HoverMove:
        normalizeDockResizeCursor();
        break;
    case QEvent::Leave:
    case QEvent::WindowDeactivate:
    case QEvent::MouseButtonRelease:
        clearDockResizeCursorOverride();
        break;
    default:
        break;
    }
    return result;
}

MainWindow::ExternalDropKind MainWindow::classifyExternalDropPath(const QString &path) const
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return ExternalDropKind::Unsupported;
    }
    if (info.isDir()) {
        const QDir dir(info.absoluteFilePath());
        if (QFileInfo(dir.filePath(QStringLiteral("C_group"))).isFile()
            || QFileInfo(dir.filePath(QStringLiteral("C_livery"))).isFile()) {
            return ExternalDropKind::CGroup;
        }
        return ExternalDropKind::Unsupported;
    }

    // Editor project documents: gzip container (.3so) and legacy bare JSON (.json).
    if (info.suffix().compare(QStringLiteral("3so"), Qt::CaseInsensitive) == 0
        || info.suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0) {
        return ExternalDropKind::ProjectJson;
    }
    if (info.fileName().compare(QStringLiteral("C_group"), Qt::CaseInsensitive) == 0
        || info.fileName().compare(QStringLiteral("C_livery"), Qt::CaseInsensitive) == 0) {
        return ExternalDropKind::CGroup;
    }

    if (supportedImageSuffixes().contains(info.suffix().toLower())) {
        return ExternalDropKind::Image;
    }
    return ExternalDropKind::Unsupported;
}

bool MainWindow::handleExternalDropUrls(const QList<QUrl> &urls)
{
    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    if (paths.size() != 1) {
        statusBar()->showMessage(QStringLiteral("Drop one project, C_group, or image file at a time"), 4000);
        return false;
    }

    const QString path = paths.front();
    const ExternalDropKind kind = classifyExternalDropPath(path);
    if (kind == ExternalDropKind::Unsupported) {
        statusBar()->showMessage(QStringLiteral("Unsupported dropped file: %1").arg(QFileInfo(path).fileName()), 4000);
        return false;
    }

    QString error;
    switch (kind) {
    case ExternalDropKind::ProjectJson:
        if (!confirmDiscardUnsavedChanges()) {
            return false;
        }
        rememberImportDirectory(path, QStringLiteral("projectJson"));
        if (!loadProjectJson(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("Open failed"), error);
            return false;
        }
        return true;
    case ExternalDropKind::CGroup:
        if (!confirmDiscardUnsavedChanges()) {
            return false;
        }
        if (!importAny(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("Import failed"), error);
            return false;
        }
        return true;
    case ExternalDropKind::Image:
        rememberImportDirectory(path, QStringLiteral("guideLayer"));
        if (!importGuideLayer(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("Guide layer import failed"), error);
            return false;
        }
        return true;
    case ExternalDropKind::Unsupported:
        break;
    }
    return false;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile() && classifyExternalDropPath(url.toLocalFile()) != ExternalDropKind::Unsupported) {
                event->setDropAction(Qt::CopyAction);
                event->accept();
                return;
            }
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QMainWindow::dragMoveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        if (handleExternalDropUrls(event->mimeData()->urls())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }
    QMainWindow::dropEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmDiscardUnsavedChanges()) {
        event->accept();
    } else {
        event->ignore();
    }
}

// Prompt to save when the project has unsaved edits. Returns true when it is safe
// to proceed (saved, exported, or the user chose to discard), false to abort.
bool MainWindow::confirmDiscardUnsavedChanges()
{
    if (state_ == nullptr || !state_->isModified()) {
        return true;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unsaved Changes"));
    box.setText(QStringLiteral("This project has unsaved changes."));
    box.setInformativeText(QStringLiteral("How would you like to save before closing?"));
    QPushButton *saveJsonBtn = box.addButton(QStringLiteral("Save Project JSON"), QMessageBox::AcceptRole);
    QPushButton *dontSaveBtn = box.addButton(QStringLiteral("Don't Save"), QMessageBox::DestructiveRole);
    QPushButton *cancelBtn = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(saveJsonBtn);
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked == dontSaveBtn) {
        return true;
    }
    if (clicked == saveJsonBtn) {
        saveProjectJsonDialog();
        // A cancelled or failed save leaves the project modified; abort the close.
        return !state_->isModified();
    }
    // Cancel or the dialog being dismissed: stay open.
    Q_UNUSED(cancelBtn);
    return false;
}

void MainWindow::updateWindowTitle()
{
    const QString base = QStringLiteral("Forza Livery Studio");
    const bool dirty = state_ != nullptr && state_->isModified();
    setWindowTitle(dirty ? base + QStringLiteral(" *") : base);
}

bool MainWindow::loadProject(const QString &path, QString *error)
{
    try {
        setProject(fh6::importCGroupNested(path));
        statusBar()->showMessage(QStringLiteral("Imported %1").arg(path), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::loadLivery(const QString &path, QString *error)
{
    try {
        setProject(fh6::importCLivery(path));
        statusBar()->showMessage(QStringLiteral("Imported livery %1").arg(path), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::exportFolderImpl(const QString &folder, bool nested, QString *error)
{
    if (!state_->hasProject_) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    try {
        fh6::Project exportProject = state_->project_;
        fh6::HeaderMetadata meta;
        if (exportProject.headerMetadata) {
            meta = *exportProject.headerMetadata;
        } else if (!exportProject.sourceHeader.isEmpty()) {
            try {
                meta = fh6::parseHeader(exportProject.sourceHeader);
            } catch (const std::exception &) {
                meta = fh6::defaultDraftHeader(exportProject.name, creatorName_);
            }
            // A published imported header only decodes name + description; its draft-only
            // fields (creator, date, guid, type, ...) are never parsed. Flipping the flag
            // alone would export a blank stub, so rebuild a clean draft instead - the export
            // is always unpublished for projects imported from a C_group file.
            if (meta.published) {
                fh6::HeaderMetadata draft = fh6::defaultDraftHeader(exportProject.name, creatorName_);
                draft.name = meta.name;
                meta = draft;
            }
        } else {
            meta = fh6::defaultDraftHeader(exportProject.name, creatorName_);
        }
        meta.published = false;
        meta.description.clear();
        exportProject.sourceHeader.clear();
        exportProject.headerMetadata = meta;

        const QString targetFolder = layerGroupExportFolder(folder, exportProject.name);
        if (nested) {
            // Nested export needs each shape's sprite size to place group origins.
            ShapeGeometryStore geometry;
            geometry.loadDefault();
            const fh6::SpriteSizeFn spriteSize = [&geometry](quint16 id) {
                return geometry.shapeSize(static_cast<int>(id));
            };
            fh6::exportNestedProjectFolder(exportProject, targetFolder, exportProject.name, spriteSize);
        } else {
            fh6::exportFlatProjectFolder(exportProject, targetFolder, exportProject.name);
        }
        const QImage thumb = renderProjectPreviewImage(exportProject, QSize(256, 256));
        if (!QImageWriter::supportedImageFormats().contains("webp")) {
            throw std::runtime_error("Qt WEBP image writer is not available; ensure qwebp.dll is deployed in the imageformats plugin folder");
        } else if (!thumb.isNull() && !thumb.save(QDir(targetFolder).filePath(QStringLiteral("thumb.webp")), "WEBP")) {
            throw std::runtime_error("could not write thumb.webp");
        }
        statusBar()->showMessage(QStringLiteral("Exported %1").arg(targetFolder), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::newProject(QString *error)
{
    Q_UNUSED(error);
    fh6::Project project;
    project.name = QStringLiteral("Untitled");
    project.headerMetadata = fh6::defaultDraftHeader(project.name, creatorName_);
    setProject(std::move(project));
    statusBar()->showMessage(QStringLiteral("New project created"), 5000);
    return true;
}

bool MainWindow::saveProjectJson(const QString &path, QString *error)
{
    if (!state_->hasProject_) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    try {
        // Projects always save to the `.3so` container. A legacy `.json` target is
        // migrated: write the `.3so` sibling and delete the old `.json` on success.
        const bool wasLegacyJson =
            QFileInfo(path).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
        const QString targetPath = wasLegacyJson
            ? path.chopped(4) + QStringLiteral("3so")  // ".json" -> ".3so"
            : path;

        const QByteArray bytes = fh6::encodeProjectDocument(state_->project_);
        QFile file(targetPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            throw std::runtime_error(("could not open project file for writing: " + targetPath).toStdString());
        }
        if (file.write(bytes) != bytes.size()) {
            throw std::runtime_error("short write while saving project");
        }
        file.close();
        if (wasLegacyJson) {
            QFile::remove(path);  // targetPath is the .3so sibling, always distinct
        }
        projectJsonPath_ = targetPath;
        rememberRecentProjectJson(targetPath);
        state_->setModified(false);
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(targetPath), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::loadProjectJson(const QString &path, QString *error)
{
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error(("could not open project file: " + path).toStdString());
        }
        setProject(fh6::decodeProjectDocument(file.readAll()));
        projectJsonPath_ = path;  // set after setProject (which clears it)
        rememberRecentProjectJson(path);
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(path), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

fh6::Project *MainWindow::project()
{
    return state_->project();
}

QVector<fh6::LayerGroup *> MainWindow::selectedGroups()
{
    return state_->selectedGroups(selectedEntryIds());
}

void MainWindow::refreshSelectionProperties()
{
    if (properties_ == nullptr) {
        return;
    }
    ScopedPerf perf("refreshSelectionProperties");
    properties_->setSelection(state_->selectedLayers(), state_->selectedGuideLayers(), selectedGroups());
}

void MainWindow::deleteSelectedLayers()
{
    if (!state_->hasProject() || (state_->selectedLayerIds().isEmpty() && state_->selectedGuideLayerIds().isEmpty())) {
        return;
    }
    const QSet<QString> lockedIds = state_->lockedLayerIds();
    for (const QString &layerId : state_->selectedLayerIds()) {
        if (lockedIds.contains(layerId)) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }
    for (fh6::GuideLayer *guide : state_->selectedGuideLayers()) {
        if (guide->locked) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }

    state_->beginProjectEdit();
    const QSet<QString> deletedIds = state_->selectedLayerIds();
    const QSet<QString> deletedGuideIds = state_->selectedGuideLayerIds();
    QVector<fh6::ShapeLayer> remaining;
    remaining.reserve(state_->project_.layers.size());
    for (const fh6::ShapeLayer &layer : state_->project_.layers) {
        if (!deletedIds.contains(layer.id)) {
            remaining.push_back(layer);
        }
    }
    state_->project_.layers = remaining;
    QVector<fh6::GuideLayer> remainingGuides;
    remainingGuides.reserve(state_->project_.guideLayers.size());
    for (const fh6::GuideLayer &guide : state_->project_.guideLayers) {
        if (!deletedGuideIds.contains(guide.id)) {
            remainingGuides.push_back(guide);
        }
    }
    state_->project_.guideLayers = remainingGuides;
    state_->pruneEmptyGroups();
    state_->selectedLayerIds_.clear();
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
}

void MainWindow::copySelection()
{
    if (copySelectionToClipboard()) {
        statusBar()->showMessage(QStringLiteral("Copied selection"), 1500);
    }
}

void MainWindow::cutSelection()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to cut"), 2500);
        return;
    }
    if (!state_->copyEntriesToClipboard(entries)) {
        statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
        return;
    }

    state_->beginProjectEdit();
    state_->removeEntries(entries);
    state_->selectedLayerIds_.clear();
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Cut selection"), 1500);
}

void MainWindow::pasteClipboard()
{
    if (!state_->hasProject() || state_->clipboard() == nullptr) {
        statusBar()->showMessage(QStringLiteral("Clipboard is empty"), 2500);
        return;
    }

    state_->beginProjectEdit();
    QSet<QString> newLayerSelection;
    QSet<QString> newGuideSelection;
    state_->insertClipboardAboveSelection(*state_->clipboard(), selectedEntryIds(), &newLayerSelection, &newGuideSelection);
    state_->selectedLayerIds_ = newLayerSelection;
    state_->selectedGuideLayerIds_ = newGuideSelection;
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Pasted selection"), 1500);
}

void MainWindow::stampSelection()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to stamp"), 2500);
        return;
    }
    state_->beginProjectEdit();
    const bool stamped = state_->duplicateEntriesInPlace(entries);
    state_->commitProjectEdit();
    if (!stamped) {
        // duplicateEntriesInPlace blocks on locked subtrees; the no-op commit recorded nothing.
        statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
        return;
    }
    // Selection is intentionally left on the originals (the stamp leaves them unchanged).
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Stamped selection"), 1500);
}

void MainWindow::insertShape(int shapeId)
{
    if (!ensureProjectForInsertion()) {
        return;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    fh6::ShapeLayer layer;
    layer.id = state_->uniqueLayerId();
    layer.name = fh6::detail::shapeName(static_cast<quint16>(shapeId));
    layer.shapeId = static_cast<quint16>(shapeId);
    layer.x = center.x();
    layer.y = center.y();
    updateLastSelectedShapeDefaults();
    const BehaviorSettings behavior = loadBehaviorSettings();
    if (behavior.insertShapeWithLastSelectedColor && haveLastSelectedShapeDefaults_) {
        layer.color = lastSelectedShapeColor_;
    }
    if (behavior.insertShapeWithLastSelectedScale && haveLastSelectedShapeDefaults_) {
        layer.scaleX = lastSelectedShapeScaleX_;
        layer.scaleY = lastSelectedShapeScaleY_;
    }

    state_->beginProjectEdit();
    state_->insertLayerAboveSelection(layer, selectedEntryIds());
    state_->selectedLayerIds_ = {layer.id};
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Inserted %1").arg(layer.name), 1500);
}

void MainWindow::placeTextDialog()
{
    if (!ensureProjectForInsertion()) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Place Text"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *fontCombo = new QComboBox(&dialog);
    fontCombo->addItems(gui::fontglyphs::fontNames());
    const QString lastFont = QSettings().value(QStringLiteral("placeText/font")).toString();
    const int lastFontIndex = fontCombo->findText(lastFont);
    if (lastFontIndex >= 0) {
        fontCombo->setCurrentIndex(lastFontIndex);
    }
    auto *textEdit = new QLineEdit(&dialog);
    textEdit->setPlaceholderText(QStringLiteral("Type text to place..."));
    auto *monoCheck = new QCheckBox(QStringLiteral("Monospace"), &dialog);
    monoCheck->setToolTip(QStringLiteral(
        "Advance every glyph by a fixed cell (the font's average glyph width, "
        "computed separately for upper- and lower-case) instead of each glyph's "
        "own width."));
    monoCheck->setChecked(QSettings().value(QStringLiteral("placeText/monospace"), false).toBool());
    form->addRow(QStringLiteral("Font"), fontCombo);
    form->addRow(QStringLiteral("Text"), textEdit);
    form->addRow(QString(), monoCheck);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    textEdit->setFocus();

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString fontName = fontCombo->currentText();
    const QString text = textEdit->text();
    const bool monospace = monoCheck->isChecked();
    if (text.trimmed().isEmpty()) {
        return;
    }
    QSettings().setValue(QStringLiteral("placeText/font"), fontName);
    QSettings().setValue(QStringLiteral("placeText/monospace"), monospace);

    constexpr double kGlyphGap = 12.0;
    constexpr double kSpaceWidth = 64.0;

    // Real inked bounding box of a glyph shape, in local coords about its origin.
    // Font glyphs all declare a 220x220 square whose only job is the transparent
    // corner markers (already dropped by the geometry loader), so the declared
    // size tells us nothing about spacing; the ink bounds give each glyph's true
    // width and horizontal offset.
    const auto inkBounds = [this](int shapeId) {
        return canvas_ != nullptr ? canvas_->shapeInkBounds(shapeId)
                                  : QRectF(-64.0, -64.0, 128.0, 128.0);
    };

    // Monospace mode advances every glyph by a fixed cell rather than its own
    // width. Upper- and lower-case blocks differ enough in size that a single
    // cell would look wrong, so each case gets its own cell derived from the
    // average inked width of that block's letters (A-Z / a-z) in this font.
    double upperCell = 0.0;
    double lowerCell = 0.0;
    if (monospace) {
        const auto averageBlockWidth = [&](ushort first, ushort last) {
            double sum = 0.0;
            int count = 0;
            for (ushort u = first; u <= last; ++u) {
                const int id = gui::fontglyphs::glyphShapeId(fontName, QChar(u));
                if (id < 0) {
                    continue;
                }
                sum += std::max(inkBounds(id).width(), 1.0);
                ++count;
            }
            return count > 0 ? sum / count : 128.0;
        };
        upperCell = averageBlockWidth('A', 'Z');
        lowerCell = averageBlockWidth('a', 'z');
    }
    const double monoSpaceWidth = std::max(upperCell, lowerCell);

    // First pass: resolve each character to a glyph and record where to place its
    // shape origin (`originX`) so the glyphs sit on one baseline. Proportional
    // mode packs each glyph so its inked left edge meets the cursor; monospace
    // mode centres each glyph's ink within its per-case cell so they align on a
    // fixed grid. A space inserts a blank advance.
    struct PlacedGlyph {
        int shapeId;
        double originX;
    };
    QVector<PlacedGlyph> glyphs;
    QString skipped;
    double cursor = 0.0;
    double lineRight = 0.0;
    for (const QChar ch : text) {
        if (ch == QLatin1Char(' ')) {
            cursor += monospace ? monoSpaceWidth : kSpaceWidth;
            lineRight = cursor;
            continue;
        }
        const int shapeId = gui::fontglyphs::glyphShapeId(fontName, ch);
        if (shapeId < 0) {
            if (!skipped.contains(ch)) {
                skipped.append(ch);
            }
            continue;
        }
        const QRectF ink = inkBounds(shapeId);
        double originX = 0.0;
        double advance = 0.0;
        if (monospace) {
            const double cell = gui::fontglyphs::isUpperBlockShape(shapeId) ? upperCell : lowerCell;
            originX = cursor + cell * 0.5 - ink.center().x();
            advance = cell;
        } else {
            originX = cursor - ink.left();
            advance = std::max(ink.width(), 1.0);
        }
        glyphs.push_back({shapeId, originX});
        cursor += advance;
        lineRight = cursor;
        cursor += kGlyphGap;
    }

    if (glyphs.isEmpty()) {
        QMessageBox::information(this,
                                QStringLiteral("Place Text"),
                                QStringLiteral("None of the typed characters have a glyph in %1.").arg(fontName));
        return;
    }

    // Centre the whole line on the current view centre.
    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    const double startX = center.x() - lineRight * 0.5;

    state_->beginProjectEdit();
    QVector<QString> newIds;
    newIds.reserve(glyphs.size());
    for (const PlacedGlyph &glyph : glyphs) {
        fh6::ShapeLayer layer;
        layer.id = state_->uniqueLayerId();
        layer.name = fh6::detail::shapeName(static_cast<quint16>(glyph.shapeId));
        layer.shapeId = static_cast<quint16>(glyph.shapeId);
        layer.x = startX + glyph.originX;
        layer.y = center.y();
        // Append at the root end (empty selection) so the glyphs land as
        // consecutive siblings, which groupEntries() requires.
        state_->insertLayerAboveSelection(layer, {});
        newIds.push_back(layer.id);
    }

    QString groupName = text.trimmed();
    if (newIds.size() > 1) {
        state_->groupEntries(newIds);
        if (!state_->project_.groups.isEmpty()) {
            state_->project_.groups.back().name = groupName;
        }
    } else {
        state_->selectedLayerIds_ = {newIds.front()};
        state_->selectedGuideLayerIds_.clear();
    }
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }

    if (!skipped.isEmpty()) {
        QMessageBox::information(this,
                                QStringLiteral("Place Text"),
                                QStringLiteral("Placed \"%1\" in %2.\n\nSkipped characters with no glyph in this font: %3")
                                    .arg(groupName, fontName, skipped));
    } else {
        statusBar()->showMessage(QStringLiteral("Placed \"%1\" (%2)").arg(groupName, fontName), 2000);
    }
}

void MainWindow::saveCurrentSelectionAsCustomGroup()
{
    if (!state_->hasProject()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("Open a project before saving a custom group."));
        return;
    }
    QVector<QString> entries;
    for (const QString &id : selectedEntryIds()) {
        if (!state_->entryIsGuide(id)) {
            entries.push_back(id);
        }
    }
    entries = state_->normalizeEntrySelection(entries);
    if (entries.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("Select one or more shape layers or groups first."));
        return;
    }

    ProjectClipboard clipboard;
    if (!state_->buildEntryClipboard(entries, clipboard) || clipboard.layers.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("The selected layers cannot be saved as a custom group."));
        return;
    }

    QString defaultName = entryNameForId(state_->project_, entries.front()).trimmed();
    if (defaultName.isEmpty()) {
        defaultName = QStringLiteral("Custom Group");
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Add Custom Group"),
                                               QStringLiteral("Name"),
                                               QLineEdit::Normal,
                                               defaultName,
                                               &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    shapesBrowser_->addCustomGroup(name, clipboard);
    statusBar()->showMessage(QStringLiteral("Saved custom group %1").arg(name), 2000);
}

void MainWindow::insertCustomGroup(const QString &name, const ProjectClipboard &clipboard)
{
    if (!ensureProjectForInsertion()) {
        return;
    }
    if (clipboard.layers.isEmpty()) {
        return;
    }
    QSet<QString> layerSelection;
    QSet<QString> guideSelection;
    const int firstNewGroup = state_->project_.groups.size();
    state_->beginProjectEdit();
    state_->insertClipboardAboveSelection(clipboard, selectedEntryIds(), &layerSelection, &guideSelection, true);
    QRectF insertedBounds;
    bool hasInsertedBounds = false;
    for (const fh6::ShapeLayer &layer : state_->project_.layers) {
        if (!layerSelection.contains(layer.id)) {
            continue;
        }
        QTransform transform;
        transform.translate(layer.x, layer.y);
        transform.rotate(layer.rotation);
        transform.shear(layer.skew, 0.0);
        transform.scale(layer.scaleX, layer.scaleY);
        const QSizeF size = canvas_ == nullptr ? QSizeF(128.0, 128.0) : canvas_->shapeSize(layer.shapeId);
        const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
        const QRectF bounds = transform.mapRect(local);
        insertedBounds = hasInsertedBounds ? insertedBounds.united(bounds) : bounds;
        hasInsertedBounds = true;
    }
    if (hasInsertedBounds) {
        const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
        const QPointF delta = center - insertedBounds.center();
        for (fh6::ShapeLayer &layer : state_->project_.layers) {
            if (layerSelection.contains(layer.id)) {
                layer.x += delta.x();
                layer.y += delta.y();
            }
        }
    }
    QSet<QString> rootIds;
    for (const QString &rootId : clipboard.rootIds) {
        rootIds.insert(rootId);
    }
    for (int i = 0; i < clipboard.groups.size() && firstNewGroup + i < state_->project_.groups.size(); ++i) {
        if (rootIds.contains(clipboard.groups[i].id)) {
            state_->project_.groups[firstNewGroup + i].name = name.trimmed().isEmpty() ? QStringLiteral("Custom Group") : name.trimmed();
        }
    }
    state_->selectedLayerIds_ = layerSelection;
    state_->selectedGuideLayerIds_ = guideSelection;
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Inserted custom group"), 1500);
}

bool MainWindow::importGuideLayer(const QString &path, QString *error)
{
    if (!ensureProjectForInsertion()) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    QString decodeError;
    QByteArray decodedFormat;
    const QImage image = readGuideImage(path, &decodedFormat, &decodeError);
    if (image.isNull()) {
        if (error != nullptr) {
            *error = decodeError.isEmpty()
                ? QStringLiteral("could not decode image: %1").arg(path)
                : decodeError;
        }
        return false;
    }

    // Embed a compressed (WEBP) copy rather than the raw source file: it keeps the
    // project JSON small and is guaranteed to be Qt-decodable on reload.
    QString embedFormat;
    QProgressDialog progress(QStringLiteral("Converting guide image for project storage..."),
                             QString(),
                             0,
                             0,
                             this);
    progress.setWindowTitle(QStringLiteral("Converting Image"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    progress.show();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const QByteArray embedBytes = encodeGuideImage(image, &embedFormat);
    progress.close();
    if (embedBytes.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("could not encode guide image: %1").arg(path);
        }
        return false;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    fh6::GuideLayer guide;
    guide.id = state_->uniqueGuideLayerId();
    guide.name = QFileInfo(path).completeBaseName();
    if (guide.name.isEmpty()) {
        guide.name = QStringLiteral("Guide");
    }
    guide.sourcePath = QFileInfo(path).absoluteFilePath();
    guide.imageBytes = embedBytes;
    // Cached decoded pixels for fast in-session rendering; not serialized to JSON.
    guide.pixelBytes = QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    guide.imageFormat = embedFormat;
    guide.width = image.width();
    guide.height = image.height();
    guide.x = center.x();
    guide.y = center.y();
    guide.opacity = 0.5;

    state_->beginProjectEdit();
    state_->project_.guideLayers.push_back(guide);
    state_->selectedLayerIds_.clear();
    state_->selectedGuideLayerIds_ = {guide.id};
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setTool(QStringLiteral("transform"));
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Added guide layer %1").arg(guide.name), 2500);
    return true;
}

void MainWindow::groupOrUngroupSelection()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to group"), 2500);
        return;
    }

    for (const QString &entryId : entries) {
        if (state_->entryHasLockedLayer(entryId)) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }
    if (entries.size() == 1 && state_->entryIsGroup(entries.front())) {
        state_->beginProjectEdit();
        state_->ungroupEntries(entries, false);
        state_->commitProjectEdit();
        state_->noteProjectStructureChanged();
        statusBar()->showMessage(QStringLiteral("Ungrouped selection"), 1500);
        return;
    }

    // Guides live outside the group tree (they render behind every shape), so grouping them -
    // with or without regular layers - is not supported.
    for (const QString &entryId : entries) {
        if (state_->entryIsGuide(entryId)) {
            statusBar()->showMessage(QStringLiteral("Guide layers cannot be grouped"), 3000);
            return;
        }
    }
    // A group needs at least two members; grouping a single element just adds a redundant level.
    if (entries.size() < 2) {
        statusBar()->showMessage(QStringLiteral("Select at least two layers to group"), 3000);
        return;
    }
    const QString parentId = state_->parentGroupForEntry(entries.front());
    const QVector<QString> *siblings = state_->childListForParent(parentId);
    if (siblings == nullptr) {
        statusBar()->showMessage(QStringLiteral("Selection cannot be grouped"), 3000);
        return;
    }
    QVector<int> orders;
    orders.reserve(entries.size());
    for (const QString &entryId : entries) {
        if (state_->parentGroupForEntry(entryId) != parentId) {
            statusBar()->showMessage(QStringLiteral("Only direct sibling layers can be grouped"), 3000);
            return;
        }
        const int order = siblings->indexOf(entryId);
        if (order < 0) {
            statusBar()->showMessage(QStringLiteral("Selection cannot be grouped"), 3000);
            return;
        }
        orders.push_back(order);
    }
    std::sort(orders.begin(), orders.end());
    for (int i = 1; i < orders.size(); ++i) {
        if (orders[i] != orders[i - 1] + 1) {
            statusBar()->showMessage(QStringLiteral("Select adjacent sibling layers to group"), 3000);
            return;
        }
    }

    state_->beginProjectEdit();
    state_->groupEntries(entries);
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Grouped selection"), 1500);
}

void MainWindow::ungroupSelectionFlat()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to ungroup"), 2500);
        return;
    }

    for (const QString &entryId : entries) {
        if (state_->entryHasLockedLayer(entryId)) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }

    state_->beginProjectEdit();
    state_->ungroupEntries(entries, true);
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Flat ungrouped selection"), 1500);
}

void MainWindow::collapseAllGroups()
{
    if (tree_ == nullptr) {
        return;
    }
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    tree_->collapseAll();
}

void MainWindow::undo()
{
    state_->undo();
    canvas_->resetRelativeSelectionFrame();
}

void MainWindow::redo()
{
    state_->redo();
    canvas_->resetRelativeSelectionFrame();
}

void MainWindow::noteProjectGeometryChanged(bool refreshPreviews)
{
    ScopedPerf perf(refreshPreviews ? "noteProjectGeometryChanged(previews)" : "noteProjectGeometryChanged");
    if (canvas_ != nullptr) {
        canvas_->invalidateSelectionCache();
        canvas_->invalidateSceneCache();
        canvas_->update();
    }
    if (refreshPreviews) {
        ScopedPerf perfPrev("  refreshPreviews");
        // Refresh the visibility/mask/lock badges and row styling in place.
        // refreshPreviews() only updates thumbnails, so without this a
        // visibility/mask toggle would not repaint its badge until the next
        // full tree rebuild (e.g. selecting another row).
        treeModel_->refreshStateRoles(&state_->project_);
        treeModel_->refreshPreviews(&state_->project_);
        refreshSelectionProperties();
    }
    updateStatus();
}

void MainWindow::noteProjectStructureChanged()
{
    ScopedPerf perf("noteProjectStructureChanged");
    state_->selectedLayerIds_ = existingLayerIds(state_->selectedLayerIds_);
    state_->selectedGuideLayerIds_ = state_->existingGuideLayerIds(state_->selectedGuideLayerIds_);
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    if (state_->project_.isLivery) {
        const QString sectionId = activeLiverySectionId();
        if (!sectionId.isEmpty()) {
            applyLiverySectionVisibility(sectionId);
            treeModel_->clearSectionCache();
            treeModel_->clear();
            {
                ScopedPerf perfTree("  treeModel_->setProjectSection");
                treeModel_->setProjectSection(&state_->project_, sectionId);
            }
        } else {
            treeModel_->setProject(&state_->project_);
        }
    } else {
        {
            ScopedPerf perfTree("  treeModel_->setProject");
            treeModel_->setProject(&state_->project_);
        }
    }
    syncTreeSelectionFromIds();
    noteProjectGeometryChanged();
    refreshSelectionProperties();
}

void MainWindow::setToolName(const QString &name)
{
    statusBar()->showMessage(QStringLiteral("Tool: %1").arg(name), 1500);
}

// Context-aware import: detect whether a chosen path (a C_group/C_livery file or a
// folder containing one) is a livery or a group source and route accordingly.
bool MainWindow::importAny(const QString &path, QString *error)
{
    const QFileInfo info(path);
    bool isLivery = false;
    if (info.isDir()) {
        // A folder is a livery source when it holds a C_livery file, otherwise treat
        // it as a C_group folder (importCGroupNested resolves the C_group inside).
        isLivery = QFileInfo(QDir(path).filePath(QStringLiteral("C_livery"))).isFile();
    } else {
        isLivery = info.fileName().compare(QStringLiteral("C_livery"), Qt::CaseInsensitive) == 0;
    }

    if (isLivery) {
        rememberImportDirectory(path, QStringLiteral("liveryFolder"));
        return loadLivery(path, error);
    }
    rememberImportDirectory(path, info.isDir() ? QStringLiteral("cgroupFolder") : QStringLiteral("cgroupFile"));
    return loadProject(path, error);
}

void MainWindow::importFileDialog()
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Import C_group or C_livery"),
                                                      importDialogStartDirectory(this, QStringLiteral("cgroupFile")),
                                                      QStringLiteral("Forza source (C_group C_livery);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    if (!importAny(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Import failed"), error);
    }
}

void MainWindow::importGuideLayerDialog()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Add Guide Layer"),
                                                      importDialogStartDirectory(this, QStringLiteral("guideLayer")),
                                                      imageDialogFilter());
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("guideLayer"));

    QString error;
    if (!importGuideLayer(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Guide layer import failed"), error);
    }
}

void MainWindow::exportDialog()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Open a project before exporting."));
        return;
    }

    // Let the user pick the export format up front, then a destination folder.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Export Project"));
    box.setText(QStringLiteral("Choose an export format."));
    box.setInformativeText(QStringLiteral(
        "Flat: the stable game export path.\n"
        "Nested: preserve group structure."));
    QPushButton *flatBtn = box.addButton(QStringLiteral("Flat Folder"), QMessageBox::AcceptRole);
    QPushButton *nestedBtn = box.addButton(QStringLiteral("Nested Folder"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(flatBtn);
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked != flatBtn && clicked != nestedBtn) {
        return;
    }
    const bool nested = (clicked == nestedBtn);

    const QString folder = QFileDialog::getExistingDirectory(this,
                                                            nested ? QStringLiteral("Export Nested Folder")
                                                                   : QStringLiteral("Export Flat Folder"),
                                                            importDialogStartDirectory(this, QStringLiteral("exportFlat")));
    if (folder.isEmpty()) {
        return;
    }
    rememberImportDirectory(folder, QStringLiteral("exportFlat"));

    QString error;
    if (!exportFolderImpl(folder, nested, &error)) {
        QMessageBox::critical(this, QStringLiteral("Export failed"), error);
    }
}

void MainWindow::newProjectDialog()
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    QString error;
    if (!newProject(&error)) {
        QMessageBox::critical(this, QStringLiteral("New project failed"), error);
    }
}

void MainWindow::saveProjectJsonDialog()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Save Project"), QStringLiteral("Create or open a project before saving."));
        return;
    }

    // If this project already has an associated file, overwrite it silently instead
    // of prompting. A legacy .json is migrated to .3so by saveProjectJson.
    if (!projectJsonPath_.isEmpty()) {
        QString error;
        if (!saveProjectJson(projectJsonPath_, &error)) {
            QMessageBox::critical(this, QStringLiteral("Save failed"), error);
        }
        return;
    }

    const QString suggested = state_->project_.name.isEmpty() ? QStringLiteral("project") : state_->project_.name;
    const QString suggestedPath = QDir(importDialogStartDirectory(this, QStringLiteral("projectJson")))
                                      .filePath(suggested + QStringLiteral(".3so"));
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("Save Project"),
                                                      suggestedPath,
                                                      QStringLiteral("Forza Project (*.3so);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("projectJson"));

    QString error;
    if (!saveProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Save failed"), error);
    }
}

void MainWindow::loadProjectJsonDialog()
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Open Project"),
                                                      importDialogStartDirectory(this, QStringLiteral("projectJson")),
                                                      QStringLiteral("Forza Project (*.3so *.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("projectJson"));

    QString error;
    if (!loadProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Open failed"), error);
    }
}

void MainWindow::openRecentProjectJson(const QString &path)
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    QString error;
    if (!loadProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Open failed"), error);
        refreshRecentProjectJsonMenu();
    }
}

void MainWindow::rememberRecentProjectJson(const QString &path)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix();
    const bool isProjectFile = suffix.compare(QStringLiteral("3so"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
    if (!info.isFile() || !isProjectFile) {
        return;
    }
    QStringList recent = QSettings().value(QStringLiteral("recent/projectJsons")).toStringList();
    recent.removeAll(info.absoluteFilePath());
    recent.push_front(info.absoluteFilePath());
    while (recent.size() > 10) {
        recent.removeLast();
    }
    QSettings().setValue(QStringLiteral("recent/projectJsons"), recent);
    refreshRecentProjectJsonMenu();
}

void MainWindow::refreshRecentProjectJsonMenu()
{
    if (recentProjectMenu_ == nullptr) {
        return;
    }
    recentProjectMenu_->clear();
    QStringList kept;
    const QStringList recent = QSettings().value(QStringLiteral("recent/projectJsons")).toStringList();
    for (const QString &path : recent) {
        const QFileInfo info(path);
        const QString suffix = info.suffix();
        const bool isProjectFile = suffix.compare(QStringLiteral("3so"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
        if (!info.isFile() || !isProjectFile || kept.contains(info.absoluteFilePath())) {
            continue;
        }
        kept.push_back(info.absoluteFilePath());
        QAction *action = recentProjectMenu_->addAction(info.fileName());
        action->setToolTip(info.absoluteFilePath());
        connect(action, &QAction::triggered, this, [this, path = info.absoluteFilePath()]() {
            openRecentProjectJson(path);
        });
    }
    QSettings().setValue(QStringLiteral("recent/projectJsons"), kept);
    if (kept.isEmpty()) {
        QAction *empty = recentProjectMenu_->addAction(QStringLiteral("(No recent projects)"));
        empty->setEnabled(false);
    }
}

void MainWindow::refreshHeaderMetadataWidget()
{
    if (headerMetadata_ == nullptr) {
        return;
    }

    if (!state_->hasProject_) {
        headerMetadata_->setMetadata({}, false, false);
        return;
    }

    // Seed editable metadata: prefer existing metadata, else parse imported header bytes,
    // else start from sensible draft defaults.
    fh6::HeaderMetadata meta;
    if (state_->project_.headerMetadata) {
        meta = *state_->project_.headerMetadata;
    } else if (!state_->project_.sourceHeader.isEmpty()) {
        try {
            meta = fh6::parseHeader(state_->project_.sourceHeader);
        } catch (const std::exception &) {
            meta = fh6::defaultDraftHeader(state_->project_.name, creatorName_);
        }
    } else {
        meta = fh6::defaultDraftHeader(state_->project_.name, creatorName_);
    }
    if (meta.name.isEmpty()) {
        meta.name = state_->project_.name;
    }

    const bool importedDraft = !state_->project_.sourceHeader.isEmpty();
    headerMetadata_->setMetadata(meta, importedDraft, true);
}

void MainWindow::showHeaderMetadataDock()
{
    if (headerMetadataDock_ == nullptr) {
        return;
    }
    refreshHeaderMetadataWidget();
    headerMetadataDock_->show();
    headerMetadataDock_->raise();
    if (headerMetadata_ != nullptr) {
        headerMetadata_->setFocus();
    }
}

void MainWindow::applyHeaderMetadata()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Header Metadata"), QStringLiteral("Create or open a project first."));
        return;
    }

    const fh6::HeaderMetadata meta = headerMetadata_->metadata();
    const bool importedDraft = !state_->project_.sourceHeader.isEmpty();
    const bool rebuild = headerMetadata_->rebuildRequested();

    state_->project_.name = meta.name;
    creatorName_ = meta.creatorName;
    QSettings().setValue(QStringLiteral("header/creatorName"), creatorName_);

    if (importedDraft && !rebuild) {
        // Keep the byte-exact source header; export will rename it to the new name.
        state_->project_.headerMetadata = meta; // store edits for reference / future rebuilds
    } else {
        state_->project_.sourceHeader.clear(); // export will synthesize from metadata
        state_->project_.headerMetadata = meta;
    }
    updateStatus();
    refreshHeaderMetadataWidget(); // reflect the (possibly dropped) source-header state
    statusBar()->showMessage(QStringLiteral("Header metadata updated"), 5000);
}

void MainWindow::saveLayout()
{
    QSettings settings;
    // Persist geometry alongside the dock state. restoreState() stores dock
    // sizes relative to the window size at save time, so the geometry must be
    // restored first or the splits (e.g. a bottom-docked Shapes panel) drift.
    settings.setValue(QStringLiteral("layout/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("layout/state"), saveState());
    statusBar()->showMessage(QStringLiteral("Layout saved"), 5000);
}

bool MainWindow::restoreLayout()
{
    QSettings settings;
    const QByteArray state = settings.value(QStringLiteral("layout/state")).toByteArray();
    if (state.isEmpty()) {
        return false;
    }
    // Restore geometry before state so restoreState() reconstructs dock sizes
    // against the same window size they were saved with.
    const QByteArray geometry = settings.value(QStringLiteral("layout/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    return restoreState(state);
}

void MainWindow::resetLayout()
{
    if (!defaultLayoutState_.isEmpty()) {
        restoreState(defaultLayoutState_);
        syncDockCollapseButtons();
    }
    QSettings settings;
    settings.remove(QStringLiteral("layout/state"));
    settings.remove(QStringLiteral("layout/geometry"));
    statusBar()->showMessage(QStringLiteral("Layout reset to default"), 5000);
}

void MainWindow::setProject(fh6::Project project)
{
    state_->setProject(std::move(project));
    projectJsonPath_.clear();  // imports/new projects have no associated .json yet
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    if (canvas_ != nullptr) {
        canvas_->setProject(&state_->project_);
    }
    if (state_->project_.isLivery) {
        rebuildSectionBar();  // selects the first populated section (tree + canvas)
        prebakeLiverySectionCaches();
    } else {
        if (sectionBar_ != nullptr) {
            sectionBar_->setSections({});
        }
        treeModel_->setProject(&state_->project_);
    }
    updateClipboardWidget();
    updateStatus();
}

void MainWindow::rebuildSectionBar()
{
    if (sectionBar_ == nullptr) {
        return;
    }
    QVector<LiverySectionBar::SectionInfo> sections;
    for (const QString &sectionId : state_->project_.rootChildIds) {
        const fh6::LayerGroup *group = nullptr;
        for (const fh6::LayerGroup &candidate : state_->project_.groups) {
            if (candidate.id == sectionId) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr || !group->isLiverySection) {
            continue;
        }
        sections.push_back({sectionId, group->name, static_cast<int>(state_->leafLayerIdsForEntry(sectionId).size())});
    }
    sectionBar_->setSections(sections);
}

void MainWindow::setActiveSection(const QString &sectionGroupId)
{
    state_->setActiveSectionId(sectionGroupId);

    // Draw only this section on the canvas by toggling layer visibility (a view
    // operation; livery view is read-only so this does not disturb edits). Do
    // this before rebuilding the tree because thumbnail generation skips hidden
    // leaves and group badges read descendant visibility.
    applyLiverySectionVisibility(sectionGroupId);

    // Show only this section's contents in the tree.
    treeModel_->setProjectSection(&state_->project_, sectionGroupId);

    state_->selectedLayerIds_.clear();
    if (canvas_ != nullptr) {
        // Refit to the now-visible section (projectBounds() ignores hidden
        // layers) without replacing the project pointer, so per-section canvas
        // render caches survive tab switches.
        canvas_->refitView();
        canvas_->invalidateSelectionCache();
        canvas_->update();
    }
    refreshSelectionProperties();
}

void MainWindow::prebakeLiverySectionCaches()
{
    if (state_ == nullptr || !state_->hasProject_ || !state_->project_.isLivery || canvas_ == nullptr || treeModel_ == nullptr) {
        return;
    }
    const QString restoreSectionId = activeLiverySectionId();
    if (restoreSectionId.isEmpty()) {
        return;
    }

    QVector<QString> sectionIds;
    sectionIds.reserve(state_->project_.rootChildIds.size());
    for (const QString &sectionId : state_->project_.rootChildIds) {
        for (const fh6::LayerGroup &group : state_->project_.groups) {
            if (group.id == sectionId && group.isLiverySection) {
                sectionIds.push_back(sectionId);
                break;
            }
        }
    }

    const QSet<QString> restoreLayerSelection = state_->selectedLayerIds_;
    const QSet<QString> restoreGuideSelection = state_->selectedGuideLayerIds_;
    for (const QString &sectionId : sectionIds) {
        state_->setActiveSectionId(sectionId);
        applyLiverySectionVisibility(sectionId);
        treeModel_->setProjectSection(&state_->project_, sectionId);
        canvas_->refitView();
        canvas_->invalidateSelectionCache();
        canvas_->repaint();
    }

    state_->setActiveSectionId(restoreSectionId);
    applyLiverySectionVisibility(restoreSectionId);
    treeModel_->setProjectSection(&state_->project_, restoreSectionId);
    state_->selectedLayerIds_ = restoreLayerSelection;
    state_->selectedGuideLayerIds_ = restoreGuideSelection;
    canvas_->refitView();
    canvas_->invalidateSelectionCache();
    canvas_->update();
    refreshSelectionProperties();
}

QString MainWindow::activeLiverySectionId() const
{
    if (state_ == nullptr || !state_->hasProject_ || !state_->project_.isLivery) {
        return {};
    }
    auto isExistingSection = [this](const QString &id) {
        for (const fh6::LayerGroup &group : state_->project_.groups) {
            if (group.id == id && group.isLiverySection) {
                return true;
            }
        }
        return false;
    };
    if (isExistingSection(state_->activeSectionId_)) {
        return state_->activeSectionId_;
    }
    for (const QString &sectionId : state_->project_.rootChildIds) {
        if (isExistingSection(sectionId)) {
            state_->setActiveSectionId(sectionId);
            return sectionId;
        }
    }
    return {};
}

void MainWindow::applyLiverySectionVisibility(const QString &sectionGroupId)
{
    if (state_ == nullptr || !state_->hasProject_ || sectionGroupId.isEmpty()) {
        return;
    }
    const QVector<QString> activeLeaves = state_->leafLayerIdsForEntry(sectionGroupId);
    const QSet<QString> activeSet(activeLeaves.begin(), activeLeaves.end());
    for (fh6::ShapeLayer &layer : state_->project_.layers) {
        layer.visible = activeSet.contains(layer.id);
    }
}

void MainWindow::updateStatus()
{
    if (!state_->hasProject_) {
        details_->setText(QStringLiteral("No project loaded"));
        return;
    }

    const QString creator = state_->project_.headerMetadata ? state_->project_.headerMetadata->creatorName : QString();
    details_->setText(QStringLiteral("%1\nCreator: %2\nSource: %3\nLayers: %4\nGroups: %5")
                          .arg(state_->project_.name)
                          .arg(creator.isEmpty() ? QStringLiteral("(unset)") : creator)
                          .arg(state_->project_.sourceFolder.isEmpty() ? QStringLiteral("(new project)") : state_->project_.sourceFolder)
                          .arg(state_->project_.layers.size())
                          .arg(state_->project_.groups.size())
                      + QStringLiteral("\nGuide layers: %1").arg(state_->project_.guideLayers.size()));
}

void MainWindow::updateClipboardWidget()
{
    if (clipboardWidget_ == nullptr) {
        return;
    }
    clipboardWidget_->setClipboard(state_->clipboard());
}

void MainWindow::updateLastSelectedShapeDefaults()
{
    const QVector<fh6::ShapeLayer *> selected = state_->selectedLayers();
    if (selected.isEmpty() || selected.front() == nullptr) {
        return;
    }
    const fh6::ShapeLayer *layer = selected.front();
    lastSelectedShapeColor_ = layer->color;
    lastSelectedShapeScaleX_ = layer->scaleX;
    lastSelectedShapeScaleY_ = layer->scaleY;
    haveLastSelectedShapeDefaults_ = true;
}

void MainWindow::updateSelectionFromTree()
{
    if (syncingSelection_) {
        return;
    }
    QSet<QString> ids;
    QSet<QString> guideIds;
    for (const QModelIndex &index : tree_->selectionModel()->selectedRows()) {
        for (const QString &id : idsForIndex(index)) {
            ids.insert(id);
        }
        for (const QString &id : index.data(LayerTreeModel::GuideIdsRole).toStringList()) {
            guideIds.insert(id);
        }
    }
    // The selection change below re-enters syncTreeSelectionFromIds() synchronously; suppress
    // its reveal so a click in the layers list doesn't scroll the list back to that row.
    suppressTreeReveal_ = true;
    state_->setSelectionIds(ids, guideIds);
    suppressTreeReveal_ = false;
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
}

void MainWindow::syncTreeSelectionFromIds()
{
    if (tree_->selectionModel() == nullptr) {
        return;
    }
    ScopedPerf perf("syncTreeSelectionFromIds");
    syncingSelection_ = true;
    tree_->selectionModel()->clearSelection();

    QModelIndex firstSelected;
    QModelIndex firstExactLeaf;
    QModelIndex firstFullGroup;
    // A row is "covered" when all of its leaves are selected: a group when its whole
    // leaf set is a subset of the selection, a leaf when it is itself selected, a guide
    // when its guide id is selected. Selection is stored only as flat leaf ids, so this
    // is how we recover which group rows to highlight. Computed bottom-up and memoized by
    // entry id (a group is covered iff every child is covered) so we never re-walk a group's
    // whole leaf list once per child - that was O(rows x groupLeaves) and dominated the cost
    // of selecting large groups.
    QHash<QString, bool> coveredById;
    std::function<bool(const QModelIndex &)> coveredFor = [&](const QModelIndex &index) -> bool {
        if (!index.isValid()) {
            return false;
        }
        const QString id = index.data(LayerTreeModel::EntryIdRole).toString();
        const auto cached = coveredById.constFind(id);
        if (cached != coveredById.constEnd()) {
            return cached.value();
        }
        bool result = false;
        if (index.data(LayerTreeModel::IsGuideRole).toBool()) {
            const QStringList guideIds = index.data(LayerTreeModel::GuideIdsRole).toStringList();
            result = guideIds.size() == 1 && state_->selectedGuideLayerIds_.contains(guideIds.front());
        } else if (index.data(LayerTreeModel::IsGroupRole).toBool()) {
            const int childRows = treeModel_->rowCount(index);
            result = childRows > 0;
            for (int r = 0; r < childRows && result; ++r) {
                result = coveredFor(treeModel_->index(r, 0, index));
            }
        } else {
            const QStringList leafIds = index.data(LayerTreeModel::LeafIdsRole).toStringList();
            result = leafIds.size() == 1 && state_->selectedLayerIds_.contains(leafIds.front());
        }
        coveredById.insert(id, result);
        return result;
    };
    std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
        const int rows = treeModel_->rowCount(parent);
        const bool parentCovered = coveredFor(parent);  // false at the root; O(1) once memoized
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = treeModel_->index(row, 0, parent);
            visit(index);
            if (!index.isValid()) {
                continue;
            }
            // Select the topmost covered row: if an ancestor group is also fully covered
            // it already represents this row, so selecting several groups highlights the
            // group rows rather than expanding to (and selecting) their individual leaves.
            const bool selected = coveredFor(index) && !parentCovered;
            if (selected) {
                tree_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                if (!firstSelected.isValid()) {
                    firstSelected = index;
                }
                const bool isGroup = index.data(LayerTreeModel::IsGroupRole).toBool();
                if (!firstFullGroup.isValid() && isGroup
                    && index.data(LayerTreeModel::LeafIdsRole).toStringList().size() > 1) {
                    firstFullGroup = index;
                } else if (!firstExactLeaf.isValid() && !isGroup) {
                    firstExactLeaf = index;
                }
            }
        }
    };
    visit(QModelIndex());
    syncingSelection_ = false;

    if (!suppressTreeReveal_) {
        const QModelIndex revealIndex = firstFullGroup.isValid() ? firstFullGroup : (firstExactLeaf.isValid() ? firstExactLeaf : firstSelected);
        revealTreeIndex(revealIndex);
    }
}

void MainWindow::revealTreeIndex(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    QVector<QModelIndex> parents;
    QStringList parentIds;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        parents.push_front(parent);
        parentIds.push_front(parent.data(LayerTreeModel::EntryIdRole).toString());
    }

    if (parentIds != autoExpandedGroupIds_) {
        for (auto it = autoExpandedTreeIndexes_.crbegin(); it != autoExpandedTreeIndexes_.crend(); ++it) {
            if (it->isValid()) {
                tree_->collapse(*it);
            }
        }
        autoExpandedTreeIndexes_.clear();
        autoExpandedGroupIds_.clear();
    }
    for (const QModelIndex &parent : parents) {
        if (!tree_->isExpanded(parent)) {
            autoExpandedTreeIndexes_.push_back(QPersistentModelIndex(parent));
        }
        tree_->expand(parent);
    }
    autoExpandedGroupIds_ = parentIds;
    tree_->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

QVector<QString> MainWindow::selectedEntryIds() const
{
    QVector<QString> ids;
    QSet<QString> seen;
    if (tree_ != nullptr && tree_->selectionModel() != nullptr) {
        for (const QModelIndex &index : tree_->selectionModel()->selectedRows()) {
            const QString id = index.data(LayerTreeModel::EntryIdRole).toString();
            if (!id.isEmpty() && !seen.contains(id)) {
                ids.push_back(id);
                seen.insert(id);
            }
        }
    }
    if (!ids.isEmpty()) {
        return ids;
    }
    for (const QString &id : state_->selectedLayerIds_) {
        ids.push_back(id);
    }
    for (const QString &id : state_->selectedGuideLayerIds_) {
        ids.push_back(id);
    }
    return ids;
}

bool MainWindow::copySelectionToClipboard()
{
    if (!state_->hasProject()) {
        return false;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to copy"), 2500);
        return false;
    }
    if (!state_->copyEntriesToClipboard(entries)) {
        statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
        return false;
    }
    return true;
}

bool MainWindow::ensureProjectForInsertion()
{
    if (state_->hasProject_) {
        return true;
    }

    const QString folder = QFileDialog::getExistingDirectory(this,
                                                            QStringLiteral("Create Project Folder"),
                                                            importDialogStartDirectory(this, QStringLiteral("newProject")));
    if (folder.isEmpty()) {
        return false;
    }
    rememberImportDirectory(folder, QStringLiteral("newProject"));

    fh6::Project project;
    project.name = QFileInfo(folder).fileName();
    if (project.name.isEmpty()) {
        project.name = QStringLiteral("Untitled");
    }
    project.sourceFolder = folder;
    project.headerMetadata = fh6::defaultDraftHeader(project.name, creatorName_);
    setProject(std::move(project));
    return true;
}

QStringList MainWindow::idsForIndex(const QModelIndex &index) const
{
    return index.data(LayerTreeModel::LeafIdsRole).toStringList();
}

QSet<QString> MainWindow::existingLayerIds(const QSet<QString> &ids) const
{
    QSet<QString> existing;
    if (!state_->hasProject_) {
        return existing;
    }
    for (const fh6::ShapeLayer &layer : state_->project_.layers) {
        if (ids.contains(layer.id)) {
            existing.insert(layer.id);
        }
    }
    return existing;
}

void MainWindow::normalizeDockResizeCursor()
{
    Qt::CursorShape shape;
    switch (cursor().shape()) {
    case Qt::SplitHCursor:
        shape = Qt::SizeHorCursor;
        break;
    case Qt::SplitVCursor:
        shape = Qt::SizeVerCursor;
        break;
    default:
        clearDockResizeCursorOverride();
        return;
    }

    const QCursor replacement(shape);
    if (dockResizeCursorOverrideActive_) {
        QApplication::changeOverrideCursor(replacement);
    } else {
        QApplication::setOverrideCursor(replacement);
        dockResizeCursorOverrideActive_ = true;
    }
}

void MainWindow::clearDockResizeCursorOverride()
{
    if (!dockResizeCursorOverrideActive_) {
        return;
    }
    QApplication::restoreOverrideCursor();
    dockResizeCursorOverrideActive_ = false;
}

} // namespace gui
