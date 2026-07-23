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

void MainWindow::setupCanvas() {
    canvas_ = new ProjectCanvas(this);
    canvas_->setEditorState(state_);
    canvas_->setTransformRelativeMode(loadTransformModeSettings().relativeMode);
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
    regionFillProgress_ = new QProgressBar(statusBar());
    regionFillProgress_->setTextVisible(true);
    regionFillProgress_->setFormat(QStringLiteral("Fill Regions %v/%m"));
    regionFillProgress_->setMinimumWidth(190);
    regionFillProgress_->setMaximumWidth(280);
    regionFillProgress_->hide();
    statusBar()->addPermanentWidget(regionFillProgress_);
    keyBindings_->registerInteraction(
        KeyInteraction::CancelActiveFill, this, KeyBindingRouter::Scope::Window,
        [this](KeyInteraction, KeyEventPhase phase, bool) {
            if (phase != KeyEventPhase::Press || regionFillCancel_ == nullptr) {
                return false;
            }
            cancelRegionFill();
            return true;
        },
        [this]() { return regionFillCancel_ != nullptr; }, kOverrideKeyBindingPriority);
    const QVector<KeyInteraction> canvasInteractions = {
        KeyInteraction::CanvasPan,
        KeyInteraction::CanvasRemovePathPoint,
        KeyInteraction::CanvasCancelInteraction,
        KeyInteraction::CanvasCommitInteraction,
        KeyInteraction::CanvasNudgeLeft,
        KeyInteraction::CanvasNudgeRight,
        KeyInteraction::CanvasNudgeUp,
        KeyInteraction::CanvasNudgeDown,
        KeyInteraction::CanvasNudgeLeftFast,
        KeyInteraction::CanvasNudgeRightFast,
        KeyInteraction::CanvasNudgeUpFast,
        KeyInteraction::CanvasNudgeDownFast,
    };
    for (KeyInteraction interaction : canvasInteractions) {
        keyBindings_->registerInteraction(
            interaction, canvas_, KeyBindingRouter::Scope::Focus,
            [this](KeyInteraction command, KeyEventPhase phase, bool autoRepeat) {
                return canvas_->handleKeyBinding(command, phase, autoRepeat);
            }, {}, 0, interaction == KeyInteraction::CanvasPan);
    }
    setCentralWidget(canvas_);
}

void MainWindow::setupTreeView() {
    treeModel_ = new LayerTreeModel(this);
    treeModel_->setEditorState(state_);
    tree_ = new LayerTreeView(this);
    tree_->setModel(treeModel_);
    tree_->setHeaderHidden(false);
    tree_->setIconSize(QSize(kTreeIconExtent, kTreeIconExtent));
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
                                      bool scrollable) {
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

void MainWindow::setupDocks() {
    const BehaviorSettings behaviorSettings = loadBehaviorSettings();
    const PreviewBackgroundSettings previewBackgroundSettings = loadPreviewBackgroundSettings();

    layersSplitter_ = new QSplitter(Qt::Vertical, this);
    layersSplitter_->setChildrenCollapsible(false);
    layersSplitter_->setHandleWidth(kDockSplitterHandleWidth);
    layersSplitter_->addWidget(sectionBar_);
    layersSplitter_->addWidget(tree_);
    layersSplitter_->setStretchFactor(1, 1);
    installSplitterResizeCursor(layersSplitter_);
    const QByteArray layersSplitterState = QSettings().value(
        QStringLiteral("ui/splitters/layersSections")).toByteArray();
    if (!layersSplitterState.isEmpty()) {
        layersSplitter_->restoreState(layersSplitterState);
    }
    connect(layersSplitter_, &QSplitter::splitterMoved, this, [this]() {
        QSettings().setValue(QStringLiteral("ui/splitters/layersSections"),
                             layersSplitter_->saveState());
    });

    auto *layersDock = addPanelDock(QStringLiteral("Layers"), QStringLiteral("LayersDock"),
                                    QStringLiteral("WidgetLayers.xpm"), Qt::RightDockWidgetArea, layersSplitter_);

    clipboardWidget_ = new ClipboardBufferWidget(this);
    clipboardWidget_->setPreviewBackground(previewBackgroundSettings.buffer, theme_);
    treeModel_->setPreviewBackground(previewBackgroundSettings.layers, theme_);
    auto *clipboardDock = addPanelDock(QStringLiteral("Buffer"), QStringLiteral("BufferDock"),
                                       QStringLiteral("WidgetBuffer.xpm"), Qt::RightDockWidgetArea, clipboardWidget_);
    splitDockWidget(layersDock, clipboardDock, Qt::Vertical);

    details_ = new QLabel(this);
    details_->setMargin(kDetailsLabelMargin);
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
    applyBehaviorSettings(behaviorSettings, false);
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
    carPreview_->setLoadCarTextures(behaviorSettings.loadCarTextures);
    carPreview_->setEditorState(state_);
    carPreview_->setProject(project());
    keyBindings_->registerInteraction(
        KeyInteraction::PreviewCycleDebugMode, carPreview_, KeyBindingRouter::Scope::Focus,
        [this](KeyInteraction, KeyEventPhase phase, bool) {
            if (phase != KeyEventPhase::Press) {
                return false;
            }
            carPreview_->cycleDebugMode();
            return true;
        });
    carPreviewDock_ = addPanelDock(QStringLiteral("3D Preview"), QStringLiteral("CarPreviewDock"),
                                   QStringLiteral("WidgetProject.xpm"), Qt::RightDockWidgetArea, carPreview_);
    tabifyDockWidget(layersDock, carPreviewDock_);
    carPreviewDock_->hide();
}

void MainWindow::connectEditorStateSignals() {
    connect(state_, &EditorState::selectionChanged, this, [this]() {
        updateLastSelectedShapeDefaults();
        syncTreeSelectionFromIds();
        canvas_->invalidateSelectionCache();
        canvas_->update();
        refreshSelectionProperties();
    });
    connect(state_, &EditorState::projectGeometryChanged, this, &MainWindow::noteProjectGeometryChanged);
    connect(state_, &EditorState::projectGeometryChanged, this, &MainWindow::cancelActiveFills);
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
    connect(state_, &EditorState::projectStructureChanged, this, &MainWindow::cancelActiveFills);
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

void MainWindow::setupFileMenu() {
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto addShortcutEntry = [this, fileMenu](const QString &text, const QString &id, const QString &label, auto slot) {
        QAction *action = fileMenu->addAction(text);
        registerShortcutAction(action, id, label);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };
    auto addIconEntry = [this, fileMenu](const QString &iconName, const QString &text, const QString &id,
                                         const QString &label, auto slot) {
        QAction *action = fileMenu->addAction(assetIcon(iconName), text);
        registerShortcutAction(action, id, label, iconName);
        addAction(action);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    addShortcutEntry(QStringLiteral("&New Project"), QStringLiteral("new_project"),
                     QStringLiteral("New Project"), &MainWindow::newProjectDialog);
    addShortcutEntry(QStringLiteral("&Open Project..."), QStringLiteral("open_project_json"),
                     QStringLiteral("Open Project"), &MainWindow::loadProjectJsonDialog);
    recentProjectMenu_ = fileMenu->addMenu(QStringLiteral("Open &Recent Project"));
    refreshRecentProjectJsonMenu();
    addShortcutEntry(QStringLiteral("&Save Project..."), QStringLiteral("save_project_json"),
                     QStringLiteral("Save Project"), &MainWindow::saveProjectJsonDialog);
    addShortcutEntry(QStringLiteral("Save Project &As..."), QStringLiteral("save_project_json_as"),
                     QStringLiteral("Save Project As"), &MainWindow::saveProjectJsonAsDialog);
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

void MainWindow::setupEditMenu() {
    auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    auto addEditEntry = [this, editMenu](const QString &text, const QString &id, const QString &label,
                                         const QString &iconName, auto slot, bool mirroredIcon = false) {
        QAction *action = editMenu->addAction(mirroredIcon ? mirroredAssetIcon(iconName) : assetIcon(iconName), text);
        registerShortcutAction(action, id, label, iconName, mirroredIcon);
        addAction(action);
        connect(action, &QAction::triggered, this, slot);
        return action;
    };

    addEditEntry(QStringLiteral("&Undo"), QStringLiteral("undo"), QStringLiteral("Undo"),
                 QStringLiteral("MenuRevert.xpm"), &MainWindow::undo);
    addEditEntry(QStringLiteral("&Redo"), QStringLiteral("redo"), QStringLiteral("Redo"),
                 QStringLiteral("MenuRevert.xpm"), &MainWindow::redo, true);
    editMenu->addSeparator();
    addEditEntry(QStringLiteral("&Copy"), QStringLiteral("copy"), QStringLiteral("Copy"),
                 QStringLiteral("MenuCopy.xpm"), &MainWindow::copySelection);
    addEditEntry(QStringLiteral("Cu&t"), QStringLiteral("cut"), QStringLiteral("Cut"),
                 QStringLiteral("MenuCut.xpm"), &MainWindow::cutSelection);
    addEditEntry(QStringLiteral("&Paste"), QStringLiteral("paste"), QStringLiteral("Paste"),
                 QStringLiteral("MenuPaste.xpm"), &MainWindow::pasteClipboard);
    addEditEntry(QStringLiteral("&Group / Ungroup"), QStringLiteral("group_ungroup"), QStringLiteral("Group / Ungroup"),
                 QStringLiteral("MenuGroup.xpm"), &MainWindow::groupOrUngroupSelection);
    addEditEntry(QStringLiteral("Ungroup &Flat"), QStringLiteral("ungroup_flat"), QStringLiteral("Ungroup Flat"),
                 QStringLiteral("MenuUngroupFlat.xpm"), &MainWindow::ungroupSelectionFlat);
    addEditEntry(QStringLiteral("Fold All Groups"), QStringLiteral("fold_all_groups"), QStringLiteral("Fold All Groups"),
                 QStringLiteral("MenuFoldAllGroups.xpm"), &MainWindow::collapseAllGroups);
    addEditEntry(QStringLiteral("&Delete Selected"), QStringLiteral("delete_selected"), QStringLiteral("Delete Selected"),
                 QStringLiteral("MenuDelete.xpm"), &MainWindow::deleteSelectedLayers);
    editMenu->addSeparator();
    addEditEntry(QStringLiteral("Stamp (Duplicate in Place)"), QStringLiteral("stamp"), QStringLiteral("Stamp"),
                 QStringLiteral("MenuPaste.xpm"), &MainWindow::stampSelection);
    QAction *flipSelection = editMenu->addAction(QStringLiteral("Flip Selection"));
    registerShortcutAction(flipSelection, QStringLiteral("flip_selection"), QStringLiteral("Flip Selection"));
    addAction(flipSelection);
    connect(flipSelection, &QAction::triggered, this, [this]() {
        if (canvas_ != nullptr) {
            canvas_->cycleFlipSelection();
        }
    });
    editMenu->addSeparator();
    addEditEntry(QStringLiteral("Center View on Selection"), QStringLiteral("center_view_on_selection"), QStringLiteral("Center View on Selection"),
                 QStringLiteral("ToolbarSelect.xpm"), &MainWindow::centerViewOnSelection);

    editMenu->addSeparator();
    auto *alignMenu = editMenu->addMenu(QStringLiteral("&Align"));
    auto addAlignEntry = [this, alignMenu](const QString &text, const QString &id, ProjectCanvas::AlignEdge edge) {
        QAction *action = alignMenu->addAction(text);
        registerShortcutAction(action, id, text);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, edge]() { alignSelection(edge); });
        return action;
    };
    addAlignEntry(QStringLiteral("&Top"), QStringLiteral("align_top"), ProjectCanvas::AlignEdge::Top);
    addAlignEntry(QStringLiteral("&Bottom"), QStringLiteral("align_bottom"), ProjectCanvas::AlignEdge::Bottom);
    addAlignEntry(QStringLiteral("&Left"), QStringLiteral("align_left"), ProjectCanvas::AlignEdge::Left);
    addAlignEntry(QStringLiteral("&Right"), QStringLiteral("align_right"), ProjectCanvas::AlignEdge::Right);
    addAlignEntry(QStringLiteral("Horizontal &Centre"), QStringLiteral("align_centre"), ProjectCanvas::AlignEdge::VCenter);
    addAlignEntry(QStringLiteral("Vertical C&entre"), QStringLiteral("align_vertical_centre"), ProjectCanvas::AlignEdge::HCenter);

    auto *distributeMenu = editMenu->addMenu(QStringLiteral("&Distribute"));
    auto addDistributeEntry = [this, distributeMenu](const QString &text, const QString &id,
                                                     ProjectCanvas::DistributeAxis axis) {
        QAction *action = distributeMenu->addAction(text);
        registerShortcutAction(action, id, text);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, axis]() { distributeSelection(axis); });
        return action;
    };
    addDistributeEntry(QStringLiteral("&Vertical"), QStringLiteral("distribute_vertical"),
                       ProjectCanvas::DistributeAxis::Vertical);
    addDistributeEntry(QStringLiteral("&Horizontal"), QStringLiteral("distribute_horizontal"),
                       ProjectCanvas::DistributeAxis::Horizontal);
}

void MainWindow::alignSelection(ProjectCanvas::AlignEdge edge) {
    if (canvas_ == nullptr || !canvas_->alignSelection(edge)) {
        statusBar()->showMessage(QStringLiteral("Select two or more layers or groups to align"), 2500);
    }
}

void MainWindow::distributeSelection(ProjectCanvas::DistributeAxis axis) {
    if (canvas_ == nullptr || !canvas_->distributeSelection(axis)) {
        statusBar()->showMessage(QStringLiteral("Select three or more layers or groups to distribute"), 2500);
    }
}

void MainWindow::setupProjectMenu() {
    auto *projectMenu = menuBar()->addMenu(QStringLiteral("&Project"));
    auto addProjectEntry = [this, projectMenu](const QString &text, const QString &id, const QString &iconName,
                                               auto slot) {
        QAction *action = iconName.isEmpty() ? projectMenu->addAction(text)
                                             : projectMenu->addAction(assetIcon(iconName), text);
        registerShortcutAction(action, id, text, iconName);
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

void MainWindow::setupImgGenMenu() {
    auto *imgGenMenu = menuBar()->addMenu(QStringLiteral("&ImgGen"));
    auto addEntry = [this, imgGenMenu](const QString &text, const QString &id,
                                       const QString &label, auto slot) {
        QAction *action = imgGenMenu->addAction(text);
        registerShortcutAction(action, id, label);
        addAction(action);
        connect(action, &QAction::triggered, this, slot);
    };
    addEntry(QStringLiteral("&Preprocess Image..."), QStringLiteral("preprocess_image"),
             QStringLiteral("Preprocess Image"), &MainWindow::preprocessSelectedGuide);
    imgGenMenu->addSeparator();
    regionMergeAreaThreshold_ = std::clamp(
        QSettings().value(QStringLiteral("imggen/smallRegionMergeArea"), 12).toInt(), 0, 512);
    auto *mergeAction = new QWidgetAction(imgGenMenu);
    auto *mergeRow = new QWidget(imgGenMenu);
    auto *mergeLayout = new QHBoxLayout(mergeRow);
    mergeLayout->setContentsMargins(8, 3, 8, 3);
    auto *mergeLabel = new QLabel(QStringLiteral("Merge regions below"), mergeRow);
    auto *mergeSlider = new QSlider(Qt::Horizontal, mergeRow);
    mergeSlider->setRange(0, 512);
    mergeSlider->setValue(regionMergeAreaThreshold_);
    mergeSlider->setMinimumWidth(120);
    mergeSlider->setToolTip(QStringLiteral("Merge smaller connected regions into the adjacent region with the closest RGB color"));
    auto *mergeValue = new QLabel(QStringLiteral("%1 px").arg(regionMergeAreaThreshold_), mergeRow);
    mergeValue->setMinimumWidth(48);
    mergeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mergeLayout->addWidget(mergeLabel);
    mergeLayout->addWidget(mergeSlider, 1);
    mergeLayout->addWidget(mergeValue);
    mergeAction->setDefaultWidget(mergeRow);
    imgGenMenu->addAction(mergeAction);
    connect(mergeSlider, &QSlider::valueChanged, this, [this, mergeValue](int value) {
        regionMergeAreaThreshold_ = value;
        mergeValue->setText(QStringLiteral("%1 px").arg(value));
        QSettings().setValue(QStringLiteral("imggen/smallRegionMergeArea"), value);
    });
    imgGenMenu->addSeparator();
    addEntry(QStringLiteral("Create &Regions"), QStringLiteral("create_regions"),
             QStringLiteral("Create Regions"), &MainWindow::createRegions);
    addEntry(QStringLiteral("&Fill Regions"), QStringLiteral("fill_regions"),
             QStringLiteral("Fill Regions"), &MainWindow::fillRegions);
}

void MainWindow::setupOptionsMenu() {
    auto *optionsMenu = menuBar()->addMenu(QStringLiteral("&Options"));
    auto addBehaviorOption = [this](QMenu *menu, const QString &text, const QString &id,
                                    bool BehaviorSettings::*member) {
        QAction *action = menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(loadBehaviorSettings().*member);
        registerShortcutAction(action, id, text);
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
    addBehaviorOption(optionsMenu, QStringLiteral("Allow Move Outside Bounding Box"),
                      QStringLiteral("toggle_allow_move_outside_bounding_box"),
                      &BehaviorSettings::allowMoveOutsideBoundingBox);
    addBehaviorOption(optionsMenu, QStringLiteral("Show Selection Flash"), QStringLiteral("toggle_selection_flash"), &BehaviorSettings::selectionFlashEnabled);

    QMenu *guidesMenu = optionsMenu->addMenu(QStringLiteral("&Guides"));
    addBehaviorOption(guidesMenu, QStringLiteral("Show Guidelines"), QStringLiteral("show_guidelines"),
                      &BehaviorSettings::guidelinesVisible);
    addBehaviorOption(guidesMenu, QStringLiteral("Lock Guidelines"), QStringLiteral("toggle_guidelines_locked"),
                      &BehaviorSettings::guidelinesLocked);
    QAction *deleteGuidelines = guidesMenu->addAction(QStringLiteral("Delete All Guidelines"));
    registerShortcutAction(deleteGuidelines, QStringLiteral("delete_all_guidelines"),
                           QStringLiteral("Delete All Guidelines"));
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
                      &BehaviorSettings::guideLayersOnTop);

    addBehaviorOption(optionsMenu, QStringLiteral("Show Visibility Borders"), QStringLiteral("toggle_visibility_borders"), &BehaviorSettings::visibilityBordersEnabled);
    QAction *transformRelativeOption = optionsMenu->addAction(QStringLiteral("Transform Relative Mode"));
    transformRelativeOption->setCheckable(true);
    const TransformModeSettings initialTransformSettings = loadTransformModeSettings();
    transformRelativeOption->setChecked(initialTransformSettings.relativeMode);
    registerShortcutAction(transformRelativeOption, QStringLiteral("toggle_transform_relative"),
                           QStringLiteral("Transform Relative Mode"));
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
    registerShortcutAction(carUnwrapAction_, QStringLiteral("toggle_car_uv_unwrap"),
                           QStringLiteral("Show Car UV Unwrap"));
    addAction(carUnwrapAction_);
    connect(carUnwrapAction_, &QAction::toggled, this, [this](bool checked) {
        if (canvas_ != nullptr) {
            canvas_->setCarUnwrapVisible(checked);
        }
    });
}

void MainWindow::setupToolbar() {
    toolBar_ = new QToolBar(QStringLiteral("Tools"), this);
    toolBar_->setObjectName(QStringLiteral("MainToolBar"));
    toolBar_->setIconSize(QSize(kToolbarIconExtent, kToolbarIconExtent));
    auto *toolGroup = new QActionGroup(toolBar_);
    toolGroup->setExclusive(true);
    auto addTool = [this](const QString &label, const QString &tool, const QString &iconName) {
        QAction *action = iconName.isEmpty()
            ? toolBar_->addAction(label)
            : toolBar_->addAction(assetIcon(iconName), label);
        action->setCheckable(true);
        action->setProperty("canvasToolName", tool);
        registerShortcutAction(action, QStringLiteral("tool_%1").arg(tool), label, iconName);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, tool]() { canvas_->setTool(tool); });
        return action;
    };
    QAction *selectTool = addTool(QStringLiteral("Select"), QStringLiteral("select"), QStringLiteral("ToolbarSelect.xpm"));
    toolGroup->addAction(selectTool);
    toolGroup->addAction(addTool(QStringLiteral("Move"), QStringLiteral("move"), QStringLiteral("ToolbarMove.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Marquee"), QStringLiteral("marquee"), QStringLiteral("ToolbarMarquee.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Transform"), QStringLiteral("transform"), QStringLiteral("ToolbarScale.xpm")));
    skewToolAction_ = addTool(QStringLiteral("Skew"), QStringLiteral("skew"), QStringLiteral("ToolbarSkew.xpm"));
    toolGroup->addAction(skewToolAction_);
    opacityToolAction_ = addTool(QStringLiteral("Opacity"), QStringLiteral("opacity"), QStringLiteral("ToolOpacity.xpm"));
    toolGroup->addAction(opacityToolAction_);
    toolGroup->addAction(addTool(QStringLiteral("Rotate"), QStringLiteral("rotate"), QStringLiteral("ToolbarRotate.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Pipette"), QStringLiteral("pipette"), QStringLiteral("ToolPipette.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Pen"), QStringLiteral("pen"), QStringLiteral("ToolbarPen.xpm")));
    toolGroup->addAction(addTool(QStringLiteral("Lining"), QStringLiteral("lining"), QStringLiteral("ToolLining.xpm")));
    liningWidthLabel_ = new QLabel(QStringLiteral("Width"), toolBar_);
    liningWidthSpin_ = new QDoubleSpinBox(toolBar_);
    liningWidthSpin_->setRange(0.1, 256.0);
    liningWidthSpin_->setDecimals(2);
    liningWidthSpin_->setSingleStep(0.5);
    liningWidthSpin_->setValue(canvas_->liningWidth());
    liningWidthSpin_->setSuffix(QStringLiteral(" units"));
    liningWidthLabel_->setVisible(false);
    liningWidthSpin_->setVisible(false);
    toolBar_->addWidget(liningWidthLabel_);
    toolBar_->addWidget(liningWidthSpin_);
    connect(liningWidthSpin_, &QDoubleSpinBox::valueChanged,
            canvas_, &ProjectCanvas::setLiningWidth);
    canvas_->setLiningWidthChangedCallback([this](double width) {
        if (liningWidthSpin_ == nullptr) {
            return;
        }
        const QSignalBlocker blocker(liningWidthSpin_);
        liningWidthSpin_->setValue(width);
    });
    toolGroup->addAction(addTool(QStringLiteral("Bucket"), QStringLiteral("bucket"), QStringLiteral("ToolBucket.xpm")));
    selectTool->setChecked(true);
    toolBar_->addSeparator();
    QAction *placeTextAction = toolBar_->addAction(assetIcon(QStringLiteral("PropertyName.xpm")), QStringLiteral("Place Text"));
    registerShortcutAction(placeTextAction, QStringLiteral("place_text"), QStringLiteral("Place Text"),
                           QStringLiteral("PropertyName.xpm"));
    addAction(placeTextAction);
    connect(placeTextAction, &QAction::triggered, this, [this]() { placeTextDialog(); });
    const BehaviorSettings settings = loadBehaviorSettings();
    skewToolAction_->setVisible(settings.separateOpacityAndSkewTools);
    opacityToolAction_->setVisible(settings.separateOpacityAndSkewTools);
    applyToolbarStyle(settings.verticalToolbar);
}

void MainWindow::applyToolbarStyle(bool vertical) {
    if (toolBar_ == nullptr) {
        return;
    }
    const Qt::ToolBarArea targetArea = vertical ? Qt::LeftToolBarArea : Qt::TopToolBarArea;
    toolBar_->setAllowedAreas(targetArea);
    if (toolBarArea(toolBar_) != targetArea) {
        addToolBar(targetArea, toolBar_);
    }
    toolBar_->setToolButtonStyle(vertical ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    const bool lining = canvas_ != nullptr && canvas_->tool() == QStringLiteral("lining");
    if (liningWidthLabel_ != nullptr) {
        liningWidthLabel_->setVisible(lining && !vertical);
    }
    if (liningWidthSpin_ != nullptr) {
        liningWidthSpin_->setVisible(lining);
        liningWidthSpin_->setMaximumWidth(vertical ? kToolbarIconExtent * 2 : QWIDGETSIZE_MAX);
    }
}

void MainWindow::importCarModel() {
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

void MainWindow::updateCarUnwrapOverlay() {
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

void MainWindow::setupWindowMenu() {
    auto *windowMenu = menuBar()->addMenu(QStringLiteral("&Window"));
    for (QDockWidget *dock : dockWidgets_) {
        windowMenu->addAction(dock->toggleViewAction());
    }
    windowMenu->addSeparator();
    auto *saveLayoutAction = windowMenu->addAction(QStringLiteral("Save &Layout"));
    registerShortcutAction(saveLayoutAction, QStringLiteral("save_layout"), QStringLiteral("Save Layout"));
    addAction(saveLayoutAction);
    connect(saveLayoutAction, &QAction::triggered, this, [this]() { saveLayout(); });
    auto *resetLayoutAction = windowMenu->addAction(QStringLiteral("&Reset Layout"));
    registerShortcutAction(resetLayoutAction, QStringLiteral("reset_layout"), QStringLiteral("Reset Layout"));
    addAction(resetLayoutAction);
    connect(resetLayoutAction, &QAction::triggered, this, [this]() { resetLayout(); });
    windowMenu->addSeparator();
    auto *settingsAction = windowMenu->addAction(QStringLiteral("&Settings..."));
    registerShortcutAction(settingsAction, QStringLiteral("settings"), QStringLiteral("Settings"));
    connect(settingsAction, &QAction::triggered, this, [this]() { showSettingsDialog(); });
}

QAction *MainWindow::trackIconAction(QAction *action, const QString &iconName, bool mirroredIcon) {
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
                                            const QString &iconName,
                                            bool mirroredIcon) {
    if (action == nullptr) {
        return nullptr;
    }
    if (!iconName.isEmpty()) {
        trackIconAction(action, iconName, mirroredIcon);
    }
    const QString settingsKey = QStringLiteral("shortcuts/%1").arg(id);
    const QKeySequence defaultSequence = defaultShortcut(id);
    QSettings settings;
    const QKeySequence currentSequence = settings.contains(settingsKey)
        ? QKeySequence(settings.value(settingsKey).toString())
        : defaultSequence;
    keyBindings_->registerAction(id, action, currentSequence);
    refreshShortcutActionText(action, id, label, currentSequence);
    QString settingsLabel = label;
    settingsLabel.remove(QLatin1Char('&'));
    shortcutActions_.push_back({id, settingsLabel, label, defaultSequence, currentSequence, action});
    return action;
}

QVector<ShortcutSettingsItem> MainWindow::shortcutSettingsItems() const {
    QVector<ShortcutSettingsItem> items;
    items.reserve(shortcutActions_.size());
    for (const ShortcutAction &binding : shortcutActions_) {
        if (binding.action == nullptr) {
            continue;
        }
        items.push_back({binding.id, binding.label, binding.defaultShortcut, binding.currentShortcut});
    }
    return items;
}

void MainWindow::applyShortcutSettings(const QVector<ShortcutSettingsItem> &items) {
    QHash<QString, QKeySequence> byId;
    for (const ShortcutSettingsItem &item : items) {
        byId.insert(item.id, item.currentSequence);
    }
    for (ShortcutAction &binding : shortcutActions_) {
        const auto it = byId.constFind(binding.id);
        if (binding.action == nullptr || it == byId.constEnd()) {
            continue;
        }
        binding.currentShortcut = it.value();
        keyBindings_->setActionSequence(binding.id, binding.currentShortcut);
        refreshShortcutActionText(binding.action, binding.id, binding.actionLabel, binding.currentShortcut);
        const QString settingsKey = QStringLiteral("shortcuts/%1").arg(binding.id);
        if (binding.currentShortcut == binding.defaultShortcut) {
            QSettings().remove(settingsKey);
        } else {
            QSettings().setValue(settingsKey, binding.currentShortcut.toString(QKeySequence::PortableText));
        }
    }
    if (toolBar_ != nullptr) {
        for (QAction *action : toolBar_->actions()) {
            if (auto *button = qobject_cast<QToolButton *>(toolBar_->widgetForAction(action))) {
                button->adjustSize();
                button->updateGeometry();
            }
        }
        if (QLayout *layout = toolBar_->layout()) {
            layout->invalidate();
            layout->activate();
        }
        toolBar_->updateGeometry();
    }
}

void MainWindow::applyTheme(UiTheme theme, bool save) {
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
    const PreviewBackgroundSettings previewBackgroundSettings = loadPreviewBackgroundSettings();
    if (clipboardWidget_ != nullptr) {
        clipboardWidget_->setPreviewBackground(previewBackgroundSettings.buffer, theme);
    }
    if (treeModel_ != nullptr) {
        treeModel_->clearSectionCache();
        treeModel_->setPreviewBackground(previewBackgroundSettings.layers, theme);
        if (state_ != nullptr && state_->hasProject_) {
            treeModel_->refreshPreviews(&state_->project_);
        }
    }
}

void MainWindow::refreshThemedIcons() {
    for (const IconAction &binding : iconActions_) {
        if (binding.action != nullptr) {
            binding.action->setIcon(binding.mirrored ? mirroredAssetIcon(binding.iconName) : assetIcon(binding.iconName));
        }
    }
}

void MainWindow::applyBehaviorSettings(const BehaviorSettings &settings, bool save) {
    const bool saveToolbarLayout = save
        && loadBehaviorSettings().verticalToolbar != settings.verticalToolbar;
    if (treeModel_ != nullptr) {
        const bool previewSettingChanged = treeModel_->generatePreviewsWithTransformations()
            != settings.generatePreviewsWithTransformations;
        treeModel_->setGeneratePreviewsWithTransformations(settings.generatePreviewsWithTransformations);
        if (previewSettingChanged && state_ != nullptr && state_->hasProject_) {
            treeModel_->refreshPreviews(&state_->project_);
        }
    }
    if (properties_ != nullptr) {
        properties_->setDebugVisible(settings.showPropertyDebug);
        properties_->setValueEditingWheelEnabled(settings.valueEditingWheelEnabled);
    }
    if (canvas_ != nullptr) {
        canvas_->setMoveToolAutoSelect(settings.moveToolAutoSelect);
        canvas_->setAllowMoveOutsideBoundingBox(settings.allowMoveOutsideBoundingBox);
        canvas_->setSelectionFlashEnabled(settings.selectionFlashEnabled);
        canvas_->setDisplayAnchorsDuringTransformDrag(settings.displayAnchorsDuringTransformDrag);
        canvas_->setSeparateOpacityAndSkewTools(settings.separateOpacityAndSkewTools);
        canvas_->setGuideLayersVisible(settings.guideLayersVisible);
        canvas_->setGuideLayersOnTop(settings.guideLayersOnTop);
        canvas_->setGuidelinesVisible(settings.guidelinesVisible);
        canvas_->setGuidelinesLocked(settings.guidelinesLocked);
        canvas_->setGuidelineColor(settings.guidelineColor);
        canvas_->setVisibilityBordersEnabled(settings.visibilityBordersEnabled);
        canvas_->setPositionLimitBorderEnabled(settings.positionLimitBorderEnabled);
        canvas_->setVisibilityBorderResolution(settings.visibilityBorderResolution);
        canvas_->setNudgeSteps(settings.nudgeStep, settings.nudgeShiftStep);
        if (!settings.separateOpacityAndSkewTools
            && (canvas_->tool() == QStringLiteral("skew")
                || canvas_->tool() == QStringLiteral("opacity"))) {
            canvas_->setTool(QStringLiteral("transform"));
        }
    }
    if (skewToolAction_ != nullptr) {
        skewToolAction_->setVisible(settings.separateOpacityAndSkewTools);
    }
    if (opacityToolAction_ != nullptr) {
        opacityToolAction_->setVisible(settings.separateOpacityAndSkewTools);
    }
    if (carPreview_ != nullptr) {
        carPreview_->setLiveryTextureScale(settings.liveryTextureScale);
        carPreview_->setLoadCarTextures(settings.loadCarTextures);
    }
    applyToolbarStyle(settings.verticalToolbar);
    configureAutosaveTimer(settings);
    if (save) {
        saveBehaviorSettings(settings);
        if (saveToolbarLayout) {
            saveLayout();
        }
    }
}

void MainWindow::configureAutosaveTimer(const BehaviorSettings &settings) {
    if (autosaveTimer_ == nullptr) {
        return;
    }
    if (!settings.autosaveEnabled || settings.autosaveIntervalMinutes <= 0) {
        autosaveTimer_->stop();
        return;
    }
    autosaveTimer_->setInterval(settings.autosaveIntervalMinutes * 60 * 1000);
    if (!autosaveTimer_->isActive()) {
        autosaveTimer_->start();
    }
}

void MainWindow::refreshShortcutActionText(QAction *action,
                                           const QString &id,
                                           const QString &label,
                                           const QKeySequence &shortcut) const {
    if (action == nullptr) {
        return;
    }
    action->setText(shortcutActionText(id, label, shortcut));
}

void MainWindow::showSettingsDialog() {
    const UiTheme originalTheme = theme_;
    SettingsDialog dialog(theme_, loadCanvasColorSettings(), loadPreviewBackgroundSettings(),
                          loadBehaviorSettings(), shortcutSettingsItems(), this);
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
    savePreviewBackgroundSettings(dialog.selectedPreviewBackgroundSettings());
    applyTheme(dialog.selectedTheme());
}

QVector<QDockWidget *> MainWindow::dockWidgetsInArea(Qt::DockWidgetArea area) const {
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

void MainWindow::installDockAreaCollapseButton(QDockWidget *dock, Qt::DockWidgetArea fallbackArea) {
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

void MainWindow::updateDockCollapseButtonVisibility() {
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

void MainWindow::updateDockCollapseButton(QDockWidget *dock, Qt::DockWidgetArea area) {
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

void MainWindow::syncDockCollapseButtons() {
    for (const DockCollapseButton &entry : dockCollapseButtons_) {
        updateDockCollapseButton(entry.dock, Qt::NoDockWidgetArea);
    }
    updateDockCollapseButtonVisibility();
}

void MainWindow::toggleDockAreaCollapsed(Qt::DockWidgetArea area, QDockWidget *anchorDock, QToolButton *anchorButton) {
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

bool MainWindow::event(QEvent *event) {
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

void MainWindow::normalizeDockResizeCursor() {
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

void MainWindow::clearDockResizeCursorOverride() {
    if (!dockResizeCursorOverrideActive_) {
        return;
    }
    QApplication::restoreOverrideCursor();
    dockResizeCursorOverrideActive_ = false;
}

} // namespace gui
