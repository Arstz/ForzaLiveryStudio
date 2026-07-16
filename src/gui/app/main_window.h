#pragma once

#include "core_types.h"
#include "editor_state.h"
#include "layer_tree_model.h"
#include "project_canvas.h"
#include "settings_dialog.h"
#include "theme_manager.h"

#include <QtCore>
#include <QtWidgets>

#include <array>
#include <atomic>
#include <memory>

class QDockWidget;
class QAction;
class QLabel;
class QMenu;
class QObject;
class QModelIndex;
class QTreeView;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QUrl;
class QWidget;
class QToolButton;

namespace gui {

class CarPreviewWidget;
class ClipboardBufferWidget;
class ColorPaletteWidget;
class EditorState;
class HeaderMetadataWidget;
class LiverySectionBar;
class PropertyPanel;
class ShapesBrowserWidget;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool loadProject(const QString &path, QString *error = nullptr);
    bool loadLivery(const QString &path, QString *error = nullptr);
    bool importAny(const QString &path, QString *error = nullptr);
    bool newProject(QString *error = nullptr);
    bool newProject(bool livery, QString *error, int carId = 0);
    bool saveProjectJson(const QString &path, QString *error = nullptr);
    bool loadProjectJson(const QString &path, QString *error = nullptr);
    void deleteSelectedLayers();
    void copySelection();
    void cutSelection();
    void pasteClipboard();
    void stampSelection();
    void sampleGuideColorToSelection();
    void insertShape(int shapeId);
    void replaceSelectedShape(int shapeId);
    void insertLogo(quint32 rasterId, int width, int height);
    void placeTextDialog();
    void saveCurrentSelectionAsCustomGroup();
    void insertCustomGroup(const QString &name, const ProjectClipboard &clipboard);
    bool importGuideLayer(const QString &path, QString *error = nullptr);
    void groupOrUngroupSelection();
    void ungroupSelectionFlat();
    void collapseAllGroups();
    void undo();
    void redo();
    void noteProjectGeometryChanged(bool refreshPreviews = false);
    void noteProjectStructureChanged();
    void centerViewOnSelection();
    void alignSelection(ProjectCanvas::AlignEdge edge);
    void distributeSelection(ProjectCanvas::DistributeAxis axis);
    void setToolName(const QString &name);

private:
    bool event(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    bool confirmDiscardUnsavedChanges();
    void updateWindowTitle();

    void importFileDialog();
    void importGuideLayerDialog();
    bool importFM2023Folder(const QString &path, QString *error);
    void rebuildSectionBar();
    void setActiveSection(const QString &sectionGroupId);
    void prebakeLiverySectionCaches();
    QString activeLiverySectionId() const;
    int activeLiverySectionSlot() const;
    void applyLiverySectionVisibility(const QString &sectionGroupId);
    void updateCarUnwrapOverlay();
    void exportDialog();
    bool exportFolderImpl(const QString &folder, QString *error);
    void setTargetCarDialog();
    void setProjectNameDialog();
    void setCreatorNameDialog();
    void newProjectDialog();
    void saveProjectJsonDialog();
    void autosaveProject();
    void loadProjectJsonDialog();
    void openRecentProjectJson(const QString &path);
    void rememberRecentProjectJson(const QString &path);
    void refreshRecentProjectJsonMenu();
    void showHeaderMetadataDock();
    void applyHeaderMetadata();
    void refreshHeaderMetadataWidget();
    void saveLayout();
    bool restoreLayout();
    void resetLayout();
    void showSettingsDialog();
    void applyTheme(UiTheme theme, bool save = true);
    void refreshThemedIcons();
    void applyBehaviorSettings(const BehaviorSettings &settings, bool save = true);
    void configureAutosaveTimer(const BehaviorSettings &settings);
    QAction *registerShortcutAction(QAction *action,
                                    const QString &id,
                                    const QString &label,
                                    const QKeySequence &defaultShortcut,
                                    const QString &iconName = QString(),
                                    bool mirroredIcon = false,
                                    Qt::ShortcutContext context = Qt::WindowShortcut);
    QAction *trackIconAction(QAction *action, const QString &iconName, bool mirroredIcon = false);
    QVector<ShortcutSettingsItem> shortcutSettingsItems() const;
    void applyShortcutSettings(const QVector<ShortcutSettingsItem> &items);
    void refreshShortcutActionText(QAction *action, const QString &id, const QString &label) const;
    void setProject(fh6::Project project);
    void updateStatus();
    void updateClipboardWidget();
    void updateColorPaletteWidget();
    void updateLastSelectedShapeDefaults();
    void updateSelectionFromTree();
    void syncTreeSelectionFromIds();
    void revealTreeIndex(const QModelIndex &index);
    QVector<QString> selectedEntryIds() const;
    fh6::Project *project();
    QVector<fh6::scene::Group *> selectedGroups();
    void refreshSelectionProperties();
    void refreshPropertyBoxFieldsFromCanvas();
    void startPenFill(const QVector<PenPoint> &points);
    void startLassoFill(const QVector<QPointF> &points);
    void cancelGeneratedFill();
    void finishPenFill(quint64 generation, PenFillResult result);
    void finishLassoFill(quint64 generation, PolygonMeshResult result);
    void insertGeneratedFill(const QString &groupName,
                             const QString &displayName,
                             const QVector<QPair<int, QTransform>> &placements);
    bool copySelectionToClipboard();
    bool ensureProjectForInsertion();
    enum class ExternalDropKind {
        Unsupported,
        ProjectJson,
        CGroup,
        Image,
    };
    ExternalDropKind classifyExternalDropPath(const QString &path) const;
    bool handleExternalDropUrls(const QList<QUrl> &urls);
    QStringList idsForIndex(const QModelIndex &index) const;
    QSet<QString> existingLayerIds(const QSet<QString> &ids) const;
    void normalizeDockResizeCursor();
    void clearDockResizeCursorOverride();
    void installDockAreaCollapseButton(QDockWidget *dock, Qt::DockWidgetArea fallbackArea);
    void toggleDockAreaCollapsed(Qt::DockWidgetArea area, QDockWidget *anchorDock = nullptr, QToolButton *anchorButton = nullptr);
    void updateDockCollapseButton(QDockWidget *dock, Qt::DockWidgetArea area);
    void syncDockCollapseButtons();
    void updateDockCollapseButtonVisibility();
    QVector<QDockWidget *> dockWidgetsInArea(Qt::DockWidgetArea area) const;

    void setupCanvas();
    void setupTreeView();
    void setupDocks();
    QDockWidget *addPanelDock(const QString &title, const QString &objectName,
                              const QString &iconName, Qt::DockWidgetArea area, QWidget *content,
                              bool scrollable = false);
    void connectEditorStateSignals();
    void setupFileMenu();
    void setupEditMenu();
    void setupProjectMenu();
    void setupOptionsMenu();
    void setupToolbar();
    void setupWindowMenu();
    void importCarModel();
    void maybeAutoLoadCarForProject();
    QString findCarModelPath(const QString &folder, const QString &modelName) const;

    EditorState *state_ = nullptr;
    ProjectCanvas *canvas_ = nullptr;
    ClipboardBufferWidget *clipboardWidget_ = nullptr;
    ColorPaletteWidget *colorPalette_ = nullptr;
    ShapesBrowserWidget *shapesBrowser_ = nullptr;
    CarPreviewWidget *carPreview_ = nullptr;
    QDockWidget *carPreviewDock_ = nullptr;
    QAction *carUnwrapAction_ = nullptr;
    QTreeView *tree_ = nullptr;
    LiverySectionBar *sectionBar_ = nullptr;
    QLabel *details_ = nullptr;
    QTimer *autosaveTimer_ = nullptr;
    HeaderMetadataWidget *headerMetadata_ = nullptr;
    QDockWidget *headerMetadataDock_ = nullptr;
    PropertyPanel *properties_ = nullptr;
    LayerTreeModel *treeModel_ = nullptr;
    QMenu *recentProjectMenu_ = nullptr;
    QString creatorName_;
    QString projectJsonPath_;
    QByteArray defaultLayoutState_;
    bool syncingSelection_ = false;
    bool suppressTreeReveal_ = false;
    bool loadingProperties_ = false;
    QVector<QPersistentModelIndex> autoExpandedTreeIndexes_;
    QStringList autoExpandedGroupIds_;
    bool dockResizeCursorOverrideActive_ = false;
    bool promptedForCarModelsFolder_ = false;
    struct DockAreaCollapseState {
        Qt::DockWidgetArea area = Qt::NoDockWidgetArea;
        bool collapsed = false;
        QDockWidget *anchorDock = nullptr;
        QToolButton *anchorButton = nullptr;
        bool anchorWidgetWasVisible = false;
        QVector<QWidget *> hiddenTitleWidgets;
        QVector<QDockWidget *> hiddenDocks;
        QByteArray layoutState;
    };
    QVector<DockAreaCollapseState> dockAreaCollapseStates_;
    struct DockCollapseButton {
        QDockWidget *dock = nullptr;
        QToolButton *button = nullptr;
        Qt::DockWidgetArea fallbackArea = Qt::NoDockWidgetArea;
    };
    QVector<DockCollapseButton> dockCollapseButtons_;
    QVector<QDockWidget *> dockWidgets_;

    struct ShortcutAction {
        QString id;
        QString label;
        QKeySequence defaultShortcut;
        QAction *action = nullptr;
    };
    struct IconAction {
        QAction *action = nullptr;
        QString iconName;
        bool mirrored = false;
    };
    QVector<ShortcutAction> shortcutActions_;
    QVector<IconAction> iconActions_;
    UiTheme theme_ = UiTheme::Dark;
    bool haveLastSelectedShapeDefaults_ = false;
    std::array<quint8, 4> lastSelectedShapeColor_ = {255, 255, 255, 255};
    double lastSelectedShapeScaleX_ = 1.0;
    double lastSelectedShapeScaleY_ = 1.0;
    std::shared_ptr<std::atomic_bool> generatedFillCancel_;
    quint64 generatedFillGeneration_ = 0;
    QVector<QString> generatedFillInsertionEntries_;
    std::array<quint8, 4> generatedFillColor_ = {255, 255, 255, 255};
    QString generatedFillLabel_;
};

} // namespace gui
