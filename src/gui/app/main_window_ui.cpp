#include "main_window.h"

#include "main_window_internal.h"

#include "car_preview_widget.h"
#include "clipboard_buffer_widget.h"
#include "color_palette_widget.h"
#include "dock_chrome.h"
#include "gui_assets.h"
#include "header_metadata_widget.h"
#include "layer_state_delegate.h"
#include "layer_tree_view.h"
#include "livery_section_bar.h"
#include "property_panel.h"
#include "shapes_browser_widget.h"

#include <QtGui>

#include <algorithm>
#include <optional>

namespace gui {

using namespace mw_detail;

void MainWindow::setupCanvas()
{
    canvas_ = new ProjectCanvas(this);
    canvas_->setEditorState(state_);
    canvas_->setTransformRelativeMode(loadTransformModeSettings().relativeMode);
    applyBehaviorSettings(loadBehaviorSettings(), false);
    QString geometryError;
    if (!canvas_->loadGeometry(&geometryError)) {
        statusBar()->showMessage(geometryError);
    }
    canvas_->setPenFillRequestedCallback(
        [this](const QVector<PenPoint> &points, const std::optional<QColor> &fillColor) {
        startPenFill(points, fillColor);
    });
    canvas_->setPenFillCancelCallback([this]() { cancelGeneratedFill(); });
    canvas_->setLiningFillRequestedCallback(
        [this](const QVector<PenPoint> &points,
               double width,
               const std::optional<QColor> &fillColor) {
        startLiningFill(points, width, fillColor);
    });
    canvas_->setLiningFillCancelCallback([this]() { cancelGeneratedFill(); });
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
    properties_->setSpriteSizeFn([this](int id) {
        return canvas_ != nullptr ? canvas_->shapeSize(id) : QSizeF(0.0, 0.0);
    });
    properties_->setShapeVisualBoundsFn([this](int id) {
        return canvas_ != nullptr ? canvas_->shapeInkBounds(id) : QRectF();
    });
    properties_->setGuideColorSampleFn([this]() -> std::optional<QColor> {
        if (canvas_ == nullptr) {
            return std::nullopt;
        }
        return canvas_->guideColorAtScreenPoint(canvas_->mapFromGlobal(QCursor::pos()));
    });
    applyBehaviorSettings(loadBehaviorSettings(), false);
    auto *propertiesDock = addPanelDock(QStringLiteral("Properties"), QStringLiteral("PropertiesDock"),
                                        QStringLiteral("WidgetProperties.xpm"), Qt::LeftDockWidgetArea, properties_, true);

    colorPalette_ = new ColorPaletteWidget(this);
    colorPalette_->setCurrentColorProvider([this]() -> std::optional<ColorPaletteWidget::Color> {
        return properties_ != nullptr ? properties_->currentSelectionColor() : std::nullopt;
    });
    colorPalette_->setApplyColorCallback([this](const ColorPaletteWidget::Color &color) {
        if (properties_ != nullptr) {
            properties_->applyColorToSelection(color);
        }
    });
    colorPalette_->setEditCallbacks([this]() {
        if (state_ != nullptr && state_->hasProject()) {
            state_->beginProjectEdit();
        }
    }, [this]() {
        if (state_ != nullptr && state_->hasProject()) {
            state_->commitProjectEdit();
        }
    });
    canvas_->setPipetteColorPickedCallback([this](const QColor &color) {
        if (properties_ != nullptr) {
            properties_->applyColorToSelection({static_cast<quint8>(color.blue()),
                                                static_cast<quint8>(color.green()),
                                                static_cast<quint8>(color.red()),
                                                static_cast<quint8>(color.alpha())});
        }
        if (colorPalette_ != nullptr && colorPalette_->addColor(color)) {
            statusBar()->showMessage(QStringLiteral("Added picked color to swatches"), 1500);
        } else {
            statusBar()->showMessage(QStringLiteral("Picked color already in swatches"), 1500);
        }
    });
    auto *paletteDock = addPanelDock(QStringLiteral("Swatches"), QStringLiteral("SwatchesDock"),
                                     QStringLiteral("PropertyColor.xpm"), Qt::LeftDockWidgetArea, colorPalette_);

    shapesBrowser_ = new ShapesBrowserWidget(this);
    shapesBrowser_->setShapeSelectedCallback([this](int shapeId) { insertShape(shapeId); });
    shapesBrowser_->setShapeReplaceCallback([this](int shapeId) { replaceSelectedShape(shapeId); });
    shapesBrowser_->setLogoSelectedCallback([this](quint32 rasterId, int width, int height) {
        insertLogo(rasterId, width, height);
    });
    shapesBrowser_->setCustomGroupSelectedCallback([this](const CustomShapeGroup &group) { insertCustomGroup(group.name, group.clipboard); });
    shapesBrowser_->setAddCurrentSelectionCallback([this]() { saveCurrentSelectionAsCustomGroup(); });
    auto *shapesDock = addPanelDock(QStringLiteral("Shapes"), QStringLiteral("ShapesDock"),
                                    QStringLiteral("WidgetShapesBrowser.xpm"), Qt::LeftDockWidgetArea, shapesBrowser_);
    splitDockWidget(propertiesDock, paletteDock, Qt::Vertical);
    splitDockWidget(paletteDock, shapesDock, Qt::Vertical);

    carPreview_ = new CarPreviewWidget(this);
    carPreview_->setLoadCarTextures(loadBehaviorSettings().loadCarTextures);
    carPreview_->setEditorState(state_);
    carPreview_->setProject(project());
    carPreviewDock_ = addPanelDock(QStringLiteral("3D Preview"), QStringLiteral("CarPreviewDock"),
                                   QStringLiteral("WidgetProject.xpm"), Qt::RightDockWidgetArea, carPreview_);
    tabifyDockWidget(layersDock, carPreviewDock_);
    carPreviewDock_->hide();
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
    connect(state_, &EditorState::projectGeometryChanged, this, [this]() {
        if (generatedFillCancel_ != nullptr) {
            cancelGeneratedFill();
        }
    });
    connect(state_, &EditorState::transformLiveChanged, this, [this]() {
        if (canvas_ != nullptr) {
            canvas_->invalidateSceneCache();
            canvas_->update();
        }
        if (properties_ != nullptr) {
            properties_->refreshTransformFields();
            refreshPropertyBoxFieldsFromCanvas();
        }
    });
    connect(state_, &EditorState::canvasRepaintRequested, this, [this]() {
        canvas_->invalidateSceneCache();
        canvas_->update();
    });
    connect(state_, &EditorState::projectStructureChanged, this, &MainWindow::noteProjectStructureChanged);
    connect(state_, &EditorState::projectStructureChanged, this, [this]() {
        if (generatedFillCancel_ != nullptr) {
            cancelGeneratedFill();
        }
    });
    connect(state_, &EditorState::clipboardChanged, this, &MainWindow::updateClipboardWidget);
    connect(state_, &EditorState::toolNameChanged, this, &MainWindow::setToolName);
    connect(state_, &EditorState::modifiedChanged, this, [this]() { updateWindowTitle(); });
    connect(state_, &EditorState::projectReset, this, [this]() {
        cancelGeneratedFill();
        haveLastSelectedShapeDefaults_ = false;
        lastSelectedShapeColor_ = {255, 255, 255, 255};
        lastSelectedShapeScaleX_ = 1.0;
        lastSelectedShapeScaleY_ = 1.0;
        autoExpandedTreeIndexes_.clear();
        autoExpandedGroupIds_.clear();
        canvas_->setProject(project());
        if (carPreview_ != nullptr) {
            carPreview_->setProject(project());
        }
        updateColorPaletteWidget();
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
    auto addIconEntry = [this, fileMenu](const QString &iconName, const QString &text, const QString &id,
                                         const QString &label, auto slot) {
        QAction *action = fileMenu->addAction(assetIcon(iconName), text);
        registerShortcutAction(action, id, label, QKeySequence(), iconName, false, Qt::ApplicationShortcut);
        addAction(action);
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
    addShortcutEntry(QStringLiteral("Save Project &As..."), QStringLiteral("save_project_json_as"),
                     QStringLiteral("Save Project As"),
                     QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
                     &MainWindow::saveProjectJsonAsDialog);
    fileMenu->addSeparator();
    addIconEntry(QStringLiteral("MenuOpenCGroup.xpm"), QStringLiteral("&Import..."),
                 QStringLiteral("import"), QStringLiteral("Import"), &MainWindow::importFileDialog);
    addIconEntry(QStringLiteral("ImportCar.xpm"), QStringLiteral("Import &Car Model..."),
                 QStringLiteral("import_car_model"), QStringLiteral("Import Car Model"), &MainWindow::importCarModel);
    addIconEntry(QStringLiteral("ImportGuide.xpm"), QStringLiteral("Import &Guide Layer..."),
                 QStringLiteral("import_guide_layer"), QStringLiteral("Import Guide Layer"), &MainWindow::importGuideLayerDialog);
    addIconEntry(QStringLiteral("MenuExportFlat.xpm"), QStringLiteral("&Export..."),
                 QStringLiteral("export"), QStringLiteral("Export"), &MainWindow::exportDialog);
    fileMenu->addSeparator();
    addIconEntry(QStringLiteral("MenuExit.xpm"), QStringLiteral("E&xit"),
                 QStringLiteral("exit"), QStringLiteral("Exit"), &QWidget::close);
}

void MainWindow::setupEditMenu()
{
    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
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
    QAction *flipSelection = editMenu->addAction(QStringLiteral("Flip Selection"));
    registerShortcutAction(flipSelection, QStringLiteral("flip_selection"), QStringLiteral("Flip Selection"),
                           QKeySequence(Qt::Key_Tab), QString(), false, Qt::ApplicationShortcut);
    addAction(flipSelection);
    connect(flipSelection, &QAction::triggered, this, [this]() {
        if (canvas_ != nullptr) {
            canvas_->cycleFlipSelection();
        }
    });
    editMenu->addSeparator();
    addEditEntry(QStringLiteral("Center View on Selection"), QStringLiteral("center_view_on_selection"), QStringLiteral("Center View on Selection"),
                 QKeySequence(Qt::Key_F1), QStringLiteral("ToolbarSelect.xpm"), &MainWindow::centerViewOnSelection);

    editMenu->addSeparator();
    auto *alignMenu = editMenu->addMenu(QStringLiteral("&Align"));
    auto addAlignEntry = [this, alignMenu](const QString &text, const QString &id, const QKeySequence &shortcut,
                                           ProjectCanvas::AlignEdge edge) {
        QAction *action = alignMenu->addAction(text);
        registerShortcutAction(action, id, text, shortcut, QString(), false, Qt::ApplicationShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, edge]() { alignSelection(edge); });
        return action;
    };
    addAlignEntry(QStringLiteral("&Top"), QStringLiteral("align_top"),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up), ProjectCanvas::AlignEdge::Top);
    addAlignEntry(QStringLiteral("&Bottom"), QStringLiteral("align_bottom"),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down), ProjectCanvas::AlignEdge::Bottom);
    addAlignEntry(QStringLiteral("&Left"), QStringLiteral("align_left"),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Left), ProjectCanvas::AlignEdge::Left);
    addAlignEntry(QStringLiteral("&Right"), QStringLiteral("align_right"),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Right), ProjectCanvas::AlignEdge::Right);
    addAlignEntry(QStringLiteral("Horizontal &Centre"), QStringLiteral("align_centre"),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), ProjectCanvas::AlignEdge::VCenter);
    addAlignEntry(QStringLiteral("Vertical C&entre"), QStringLiteral("align_vertical_centre"),
                  QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), ProjectCanvas::AlignEdge::HCenter);

    auto *distributeMenu = editMenu->addMenu(QStringLiteral("&Distribute"));
    auto addDistributeEntry = [this, distributeMenu](const QString &text, const QString &id, const QKeySequence &shortcut,
                                                     ProjectCanvas::DistributeAxis axis) {
        QAction *action = distributeMenu->addAction(text);
        registerShortcutAction(action, id, text, shortcut, QString(), false, Qt::ApplicationShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, axis]() { distributeSelection(axis); });
        return action;
    };
    addDistributeEntry(QStringLiteral("&Vertical"), QStringLiteral("distribute_vertical"),
                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), ProjectCanvas::DistributeAxis::Vertical);
    addDistributeEntry(QStringLiteral("&Horizontal"), QStringLiteral("distribute_horizontal"),
                       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H), ProjectCanvas::DistributeAxis::Horizontal);
}

void MainWindow::alignSelection(ProjectCanvas::AlignEdge edge)
{
    if (canvas_ == nullptr || !canvas_->alignSelection(edge)) {
        statusBar()->showMessage(QStringLiteral("Select two or more layers or groups to align"), 2500);
    }
}

void MainWindow::distributeSelection(ProjectCanvas::DistributeAxis axis)
{
    if (canvas_ == nullptr || !canvas_->distributeSelection(axis)) {
        statusBar()->showMessage(QStringLiteral("Select three or more layers or groups to distribute"), 2500);
    }
}

void MainWindow::setupProjectMenu()
{
    auto *projectMenu = menuBar()->addMenu(QStringLiteral("&Project"));
    auto addProjectEntry = [this, projectMenu](const QString &text, const QString &id, const QString &iconName,
                                               auto slot) {
        QAction *action = iconName.isEmpty() ? projectMenu->addAction(text)
                                             : projectMenu->addAction(assetIcon(iconName), text);
        registerShortcutAction(action, id, text, QKeySequence(), iconName, false, Qt::ApplicationShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    addProjectEntry(QStringLiteral("&Target Car..."), QStringLiteral("set_target_car"),
                    QStringLiteral("ImportCar.xpm"), &MainWindow::setTargetCarDialog);
    addProjectEntry(QStringLiteral("Project &Name..."), QStringLiteral("set_project_name"),
                    QString(), &MainWindow::setProjectNameDialog);
    addProjectEntry(QStringLiteral("&Creator Name..."), QStringLiteral("set_creator_name"),
                    QString(), &MainWindow::setCreatorNameDialog);
}

void MainWindow::setupImgGenMenu()
{
    auto *imgGenMenu = menuBar()->addMenu(QStringLiteral("&ImgGen"));
    auto addEntry = [this, imgGenMenu](const QString &text, const QString &id,
                                       const QString &label, auto slot) {
        QAction *action = imgGenMenu->addAction(text);
        registerShortcutAction(action, id, label, QKeySequence(), QString(), false,
                               Qt::ApplicationShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, slot);
    };
    addEntry(QStringLiteral("&Preprocess Image..."), QStringLiteral("preprocess_image"),
             QStringLiteral("Preprocess Image"), &MainWindow::preprocessSelectedGuide);
    imgGenMenu->addSeparator();
    addEntry(QStringLiteral("Create &Regions"), QStringLiteral("create_regions"),
             QStringLiteral("Create Regions"), &MainWindow::createRegions);
    addEntry(QStringLiteral("&Fill Regions"), QStringLiteral("fill_regions"),
             QStringLiteral("Fill Regions"), &MainWindow::fillRegions);
}

void MainWindow::setupOptionsMenu()
{
    auto *optionsMenu = menuBar()->addMenu(QStringLiteral("&Options"));
    auto addBehaviorOption = [this](QMenu *menu, const QString &text, const QString &id, bool BehaviorSettings::*member,
                                    const QKeySequence &shortcut = QKeySequence()) {
        QAction *action = menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(loadBehaviorSettings().*member);
        registerShortcutAction(action, id, text, shortcut, QString(), false, Qt::ApplicationShortcut);
        addAction(action);
        connect(action, &QAction::toggled, this, [this, member](bool checked) {
            BehaviorSettings settings = loadBehaviorSettings();
            settings.*member = checked;
            applyBehaviorSettings(settings);
        });
        return action;
    };
    addBehaviorOption(optionsMenu, QStringLiteral("Use Last Selected Color for New Shapes"), QStringLiteral("toggle_insert_last_color"), &BehaviorSettings::insertShapeWithLastSelectedColor);
    addBehaviorOption(optionsMenu, QStringLiteral("Use Last Selected Shape Scale for New Shapes"), QStringLiteral("toggle_insert_last_scale"), &BehaviorSettings::insertShapeWithLastSelectedScale);
    addBehaviorOption(optionsMenu, QStringLiteral("Show Property Debug"), QStringLiteral("toggle_property_debug"), &BehaviorSettings::showPropertyDebug);
    addBehaviorOption(optionsMenu, QStringLiteral("Move Tool Auto-Select"), QStringLiteral("toggle_move_auto_select"), &BehaviorSettings::moveToolAutoSelect);
    addBehaviorOption(optionsMenu, QStringLiteral("Show Selection Flash"), QStringLiteral("toggle_selection_flash"), &BehaviorSettings::selectionFlashEnabled, QKeySequence(QStringLiteral("\\")));

    QMenu *guidesMenu = optionsMenu->addMenu(QStringLiteral("&Guides"));
    addBehaviorOption(guidesMenu, QStringLiteral("Show Guidelines"), QStringLiteral("show_guidelines"),
                      &BehaviorSettings::guidelinesVisible, QKeySequence(Qt::CTRL | Qt::Key_Semicolon));
    addBehaviorOption(guidesMenu, QStringLiteral("Lock Guidelines"), QStringLiteral("toggle_guidelines_locked"),
                      &BehaviorSettings::guidelinesLocked, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Semicolon));
    QAction *deleteGuidelines = guidesMenu->addAction(QStringLiteral("Delete All Guidelines"));
    registerShortcutAction(deleteGuidelines, QStringLiteral("delete_all_guidelines"), QStringLiteral("Delete All Guidelines"),
                           QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Semicolon),
                           QString(), false, Qt::ApplicationShortcut);
    addAction(deleteGuidelines);
    connect(deleteGuidelines, &QAction::triggered, this, [this]() {
        if (canvas_ != nullptr) {
            canvas_->deleteAllGuidelines();
        }
    });
    guidesMenu->addSeparator();
    addBehaviorOption(guidesMenu, QStringLiteral("Show Guide Layers"), QStringLiteral("toggle_guide_layer_visibility"),
                      &BehaviorSettings::guideLayersVisible);
    addBehaviorOption(guidesMenu, QStringLiteral("Show Guide Layers On Top"), QStringLiteral("toggle_guide_layers_on_top"),
                      &BehaviorSettings::guideLayersOnTop, QKeySequence(Qt::Key_QuoteLeft));

    addBehaviorOption(optionsMenu, QStringLiteral("Show Visibility Borders"), QStringLiteral("toggle_visibility_borders"), &BehaviorSettings::visibilityBordersEnabled);
    QAction *transformRelativeOption = optionsMenu->addAction(QStringLiteral("Transform Relative Mode"));
    transformRelativeOption->setCheckable(true);
    const TransformModeSettings initialTransformSettings = loadTransformModeSettings();
    transformRelativeOption->setChecked(initialTransformSettings.relativeMode);
    registerShortcutAction(transformRelativeOption, QStringLiteral("toggle_transform_relative"), QStringLiteral("Transform Relative Mode"),
                           QKeySequence(), QString(), false, Qt::ApplicationShortcut);
    addAction(transformRelativeOption);
    auto applyTransformRelativeOption = [this](bool checked) {
        TransformModeSettings settings = loadTransformModeSettings();
        settings.relativeMode = checked;
        saveTransformModeSettings(settings);
        canvas_->setTransformRelativeMode(checked);
    };
    connect(transformRelativeOption, &QAction::toggled, this, applyTransformRelativeOption);
    canvas_->setTransformRelativeMode(initialTransformSettings.relativeMode);

    carUnwrapAction_ = optionsMenu->addAction(QStringLiteral("Show Car UV Unwrap"));
    carUnwrapAction_->setCheckable(true);
    carUnwrapAction_->setEnabled(false);
    registerShortcutAction(carUnwrapAction_, QStringLiteral("toggle_car_uv_unwrap"), QStringLiteral("Show Car UV Unwrap"),
                           QKeySequence(), QString(), false, Qt::ApplicationShortcut);
    addAction(carUnwrapAction_);
    connect(carUnwrapAction_, &QAction::toggled, this, [this](bool checked) {
        if (canvas_ != nullptr) {
            canvas_->setCarUnwrapVisible(checked);
        }
    });
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
        QAction *action = iconName.isEmpty()
            ? toolBar->addAction(label)
            : toolBar->addAction(assetIcon(iconName), label);
        action->setCheckable(true);
        action->setProperty("canvasToolName", tool);
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
    toolGroup->addAction(addTool(QStringLiteral("Pipette"), QStringLiteral("pipette"), QKeySequence(Qt::Key_I), QStringLiteral("ToolPipette.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Pen"), QStringLiteral("pen"), QKeySequence(Qt::Key_P), QStringLiteral("ToolbarPen.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Lining"), QStringLiteral("lining"), QKeySequence(Qt::Key_L), QStringLiteral("ToolLining.xpm")));
    liningWidthLabel_ = new QLabel(QStringLiteral("Width"), toolBar);
    liningWidthSpin_ = new QDoubleSpinBox(toolBar);
    liningWidthSpin_->setRange(0.1, 256.0);
    liningWidthSpin_->setDecimals(2);
    liningWidthSpin_->setSingleStep(0.5);
    liningWidthSpin_->setValue(canvas_->liningWidth());
    liningWidthSpin_->setSuffix(QStringLiteral(" units"));
    liningWidthLabel_->setVisible(false);
    liningWidthSpin_->setVisible(false);
    toolBar->addWidget(liningWidthLabel_);
    toolBar->addWidget(liningWidthSpin_);
    connect(liningWidthSpin_, &QDoubleSpinBox::valueChanged,
            canvas_, &ProjectCanvas::setLiningWidth);
    canvas_->setLiningWidthChangedCallback([this](double width) {
        if (liningWidthSpin_ == nullptr) {
            return;
        }
        const QSignalBlocker blocker(liningWidthSpin_);
        liningWidthSpin_->setValue(width);
    });
    toolGroup->addAction(addTool(QStringLiteral("Bucket"), QStringLiteral("bucket"), QKeySequence(Qt::Key_B), QStringLiteral("ToolBucket.xpm")));
    selectTool->setChecked(true);
    toolBar->addSeparator();
    QAction *placeTextAction = toolBar->addAction(assetIcon(QStringLiteral("PropertyName.xpm")), QStringLiteral("Place Text"));
    registerShortcutAction(placeTextAction, QStringLiteral("place_text"), QStringLiteral("Place Text"),
                           QKeySequence(), QStringLiteral("PropertyName.xpm"), false, Qt::ApplicationShortcut);
    addAction(placeTextAction);
    connect(placeTextAction, &QAction::triggered, this, [this]() { placeTextDialog(); });
}

void MainWindow::importCarModel()
{
    if (carPreview_ == nullptr) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Car Model"), QString(),
        QStringLiteral("Forza models (*.modelbin *.carbin *.zip);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!carPreview_->loadCar(path, &error)) {
        statusBar()->showMessage(error.isEmpty() ? QStringLiteral("Failed to load car model") : error);
        return;
    }
    if (carPreviewDock_ != nullptr) {
        carPreviewDock_->show();
        carPreviewDock_->raise();
    }
    updateCarUnwrapOverlay();
    statusBar()->showMessage(QStringLiteral("Loaded car model: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::updateCarUnwrapOverlay()
{
    if (canvas_ == nullptr) {
        return;
    }
    int sectionSlot = -1;
    if (state_ != nullptr && state_->hasProject_ && state_->project_.isLivery) {
        sectionSlot = activeLiverySectionSlot();
        if (sectionSlot < 0) {
            canvas_->setCarUnwrapOverlay({});
            if (carUnwrapAction_ != nullptr) {
                carUnwrapAction_->setEnabled(false);
            }
            return;
        }
    }
    const QImage overlay = carPreview_ != nullptr ? carPreview_->unwrapOverlay(sectionSlot) : QImage();
    canvas_->setCarUnwrapOverlay(overlay);
    if (carUnwrapAction_ != nullptr) {
        carUnwrapAction_->setEnabled(!overlay.isNull());
    }
}

void MainWindow::setupWindowMenu()
{
    auto *windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
    for (QDockWidget *dock : dockWidgets_) {
        windowMenu->addAction(dock->toggleViewAction());
    }
    windowMenu->addSeparator();
    auto *saveLayoutAction = windowMenu->addAction(QStringLiteral("Save &Layout"));
    registerShortcutAction(saveLayoutAction, QStringLiteral("save_layout"), QStringLiteral("Save Layout"),
                           QKeySequence(), QString(), false, Qt::ApplicationShortcut);
    addAction(saveLayoutAction);
    connect(saveLayoutAction, &QAction::triggered, this, [this]() { saveLayout(); });
    auto *resetLayoutAction = windowMenu->addAction(QStringLiteral("&Reset Layout"));
    registerShortcutAction(resetLayoutAction, QStringLiteral("reset_layout"), QStringLiteral("Reset Layout"),
                           QKeySequence(), QString(), false, Qt::ApplicationShortcut);
    addAction(resetLayoutAction);
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
    if (colorPalette_ != nullptr) {
        colorPalette_->refreshTheme();
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
        properties_->setValueEditingWheelEnabled(settings.valueEditingWheelEnabled);
    }
    if (canvas_ != nullptr) {
        canvas_->setMoveToolAutoSelect(settings.moveToolAutoSelect);
        canvas_->setSelectionFlashEnabled(settings.selectionFlashEnabled);
        canvas_->setDisplayAnchorsDuringTransformDrag(settings.displayAnchorsDuringTransformDrag);
        canvas_->setGuideLayersVisible(settings.guideLayersVisible);
        canvas_->setGuideLayersOnTop(settings.guideLayersOnTop);
        canvas_->setGuidelinesVisible(settings.guidelinesVisible);
        canvas_->setGuidelinesLocked(settings.guidelinesLocked);
        canvas_->setGuidelineColor(settings.guidelineColor);
        canvas_->setVisibilityBordersEnabled(settings.visibilityBordersEnabled);
        canvas_->setPositionLimitBorderEnabled(settings.positionLimitBorderEnabled);
        canvas_->setVisibilityBorderResolution(settings.visibilityBorderResolution);
        canvas_->setNudgeSteps(settings.nudgeStep, settings.nudgeShiftStep);
    }
    if (carPreview_ != nullptr) {
        carPreview_->setLiveryTextureScale(settings.liveryTextureScale);
        carPreview_->setLoadCarTextures(settings.loadCarTextures);
    }
    configureAutosaveTimer(settings);
    if (save) {
        saveBehaviorSettings(settings);
    }
}

void MainWindow::configureAutosaveTimer(const BehaviorSettings &settings)
{
    if (autosaveTimer_ == nullptr) {
        return;
    }
    if (settings.autosaveIntervalMinutes <= 0) {
        autosaveTimer_->stop();
        return;
    }
    autosaveTimer_->setInterval(settings.autosaveIntervalMinutes * 60 * 1000);
    if (!autosaveTimer_->isActive()) {
        autosaveTimer_->start();
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
    SettingsDialog dialog(theme_, loadCanvasColorSettings(), loadBehaviorSettings(), shortcutSettingsItems(), this);
    dialog.setThemeChangedCallback([this](UiTheme theme) {
        applyTheme(theme, false);
    });
    if (dialog.exec() != QDialog::Accepted) {
        applyTheme(originalTheme, false);
        return;
    }
    applyShortcutSettings(dialog.shortcutItems());
    applyBehaviorSettings(dialog.selectedBehaviorSettings());
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
    if (dock != nullptr) {
        connect(dock, &QDockWidget::dockLocationChanged, this,
                [this, dock](Qt::DockWidgetArea area) {
                    updateDockCollapseButton(dock, area);
                    updateDockCollapseButtonVisibility();
                });
        connect(dock, &QDockWidget::topLevelChanged, this,
                [this](bool) { updateDockCollapseButtonVisibility(); });
        connect(dock, &QDockWidget::visibilityChanged, this,
                [this](bool) { updateDockCollapseButtonVisibility(); });
    }
}

void MainWindow::updateDockCollapseButtonVisibility()
{
    QHash<QDockWidget *, QToolButton *> buttonFor;
    for (const DockCollapseButton &entry : dockCollapseButtons_) {
        if (entry.button != nullptr) {
            buttonFor.insert(entry.dock, entry.button);
            entry.button->setVisible(false);
        }
    }
    const Qt::DockWidgetArea areas[] = {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea,
                                        Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea};
    for (Qt::DockWidgetArea area : areas) {
        QVector<QDockWidget *> granted;
        for (QDockWidget *dock : dockWidgetsInArea(area)) {
            QToolButton *button = buttonFor.value(dock, nullptr);
            if (button == nullptr || dock == nullptr || !dock->isVisible()) {
                continue;
            }
            const QList<QDockWidget *> tabSiblings = tabifiedDockWidgets(dock);
            bool duplicatesVisible = false;
            for (QDockWidget *shown : granted) {
                if (!tabSiblings.contains(shown)) {
                    duplicatesVisible = true;
                    break;
                }
            }
            if (!duplicatesVisible) {
                button->setVisible(true);
                granted.push_back(dock);
            }
        }
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
    updateDockCollapseButtonVisibility();
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
        const Qt::KeyboardModifiers shortcutModifiers =
            keyEvent->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        QKeySequence pressed(shortcutModifiers | keyEvent->key());
        if (keyEvent->key() == Qt::Key_Backtab) {
            pressed = QKeySequence(Qt::ShiftModifier | Qt::Key_Tab);
        }
        QAction *matchedAction = nullptr;
        if (keyEvent->key() == Qt::Key_Tab || keyEvent->key() == Qt::Key_Backtab) {
            for (const ShortcutAction &binding : std::as_const(shortcutActions_)) {
                if (binding.action != nullptr
                    && binding.action->isEnabled()
                    && !binding.action->shortcut().isEmpty()
                    && binding.action->shortcut().matches(pressed) == QKeySequence::ExactMatch) {
                    matchedAction = binding.action;
                    break;
                }
            }
        }
        if (matchedAction != nullptr) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            if (keyEvent->isAutoRepeat()) {
                event->accept();
                return true;
            }
            matchedAction->trigger();
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
