#pragma once

#include "core_types.h"
#include "editor_state.h"
#include "layer_tree_model.h"
#include "project_canvas.h"
#include "settings_dialog.h"
#include "theme_manager.h"

#include <QMainWindow>
#include <QPersistentModelIndex>
#include <QSet>
#include <QVector>

#include <array>

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

namespace gui {

class ClipboardBufferWidget;
class EditorState;
class HeaderMetadataWidget;
class LiverySectionBar;
class PropertyPanel;
class ShapesBrowserWidget;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);

    bool loadProject(const QString &path, QString *error = nullptr);
    bool loadLivery(const QString &path, QString *error = nullptr);
    bool importAny(const QString &path, QString *error = nullptr);
    bool newProject(QString *error = nullptr);
    bool saveProjectJson(const QString &path, QString *error = nullptr);
    bool loadProjectJson(const QString &path, QString *error = nullptr);
    void deleteSelectedLayers();
    void copySelection();
    void cutSelection();
    void pasteClipboard();
    void stampSelection();
    void insertShape(int shapeId);
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
    void rebuildSectionBar();
    void setActiveSection(const QString &sectionGroupId);
    void prebakeLiverySectionCaches();
    QString activeLiverySectionId() const;
    void applyLiverySectionVisibility(const QString &sectionGroupId);
    void exportDialog();
    bool exportFolderImpl(const QString &folder, bool nested, QString *error);
    void newProjectDialog();
    void saveProjectJsonDialog();
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
    void updateLastSelectedShapeDefaults();
    void updateSelectionFromTree();
    void syncTreeSelectionFromIds();
    void revealTreeIndex(const QModelIndex &index);
    QVector<QString> selectedEntryIds() const;
    fh6::Project *project();
    // Tree-aware selection helpers: resolve the tree's entry selection against state_.
    QVector<fh6::LayerGroup *> selectedGroups();
    void refreshSelectionProperties();
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
    // Repoints a collapse button's arrow at the dock's current area so the glyph
    // stays correct after layout restore or when the dock is re-anchored.
    void updateDockCollapseButton(QDockWidget *dock, Qt::DockWidgetArea area);
    void syncDockCollapseButtons();
    QVector<QDockWidget *> dockWidgetsInArea(Qt::DockWidgetArea area) const;

    // Constructor helpers: each builds one slice of the UI so the constructor reads
    // as an orchestration sequence. addPanelDock() factors the repeated dock setup.
    void setupCanvas();
    void setupTreeView();
    void setupDocks();
    QDockWidget *addPanelDock(const QString &title, const QString &objectName,
                              const QString &iconName, Qt::DockWidgetArea area, QWidget *content);
    void connectEditorStateSignals();
    void setupFileMenu();
    void setupEditMenu();
    void setupOptionsMenu();
    void setupToolbar();
    void setupWindowMenu();

    EditorState *state_ = nullptr;
    ProjectCanvas *canvas_ = nullptr;
    ClipboardBufferWidget *clipboardWidget_ = nullptr;
    ShapesBrowserWidget *shapesBrowser_ = nullptr;
    QTreeView *tree_ = nullptr;
    LiverySectionBar *sectionBar_ = nullptr;  // C_livery section tabs (hidden otherwise)
    QLabel *details_ = nullptr;
    HeaderMetadataWidget *headerMetadata_ = nullptr;
    QDockWidget *headerMetadataDock_ = nullptr;
    PropertyPanel *properties_ = nullptr;
    LayerTreeModel *treeModel_ = nullptr;
    QMenu *recentProjectMenu_ = nullptr;
    QString creatorName_;
    QString projectJsonPath_;  // path of the associated project file (Save overwrites it directly)
    QByteArray defaultLayoutState_;
    bool syncingSelection_ = false;
    // Set while the tree drives a selection change so the ensuing tree resync does not scroll
    // (snap) the list back to the row the user just clicked. Canvas-driven selections still reveal.
    bool suppressTreeReveal_ = false;
    bool loadingProperties_ = false;
    QVector<QPersistentModelIndex> autoExpandedTreeIndexes_;
    QStringList autoExpandedGroupIds_;
    bool dockResizeCursorOverrideActive_ = false;
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
    // Panel docks in Window-menu order; populated by addPanelDock().
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
};

} // namespace gui
