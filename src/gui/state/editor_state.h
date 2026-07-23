#pragma once

#include "core_types.h"
#include "layer.h"
#include "scene_view.h"

#include <QtCore>
#include <QtGui>

#include <memory>
#include <optional>

namespace gui {

struct ProjectEditSnapshot {
    fh6::Project project;
};

struct ProjectSelectionSnapshot {
    QSet<QString> layerIds;
    QSet<QString> guideLayerIds;
    QVector<QString> entryIds;
};

enum class ProjectEditCommandKind {
    Snapshot,
    Transform,
};

enum class ProjectEditRefresh {
    GeometryOnly,
    Previews,
    Structure,
};

struct ProjectTransformEdit {
    QString nodeId;
    fh6::scene::Transform2D before;
    fh6::scene::Transform2D after;
};

struct ProjectEditCommand {
    ProjectEditCommandKind kind = ProjectEditCommandKind::Snapshot;
    ProjectEditSnapshot before;
    ProjectEditSnapshot after;
    QVector<ProjectTransformEdit> transforms;
    ProjectSelectionSnapshot undoSelection;
    ProjectSelectionSnapshot redoSelection;
    ProjectEditRefresh refresh = ProjectEditRefresh::Structure;
};

struct ProjectClipboard {
    ProjectClipboard() = default;
    ProjectClipboard(const ProjectClipboard &other);
    ProjectClipboard &operator=(const ProjectClipboard &other);
    ProjectClipboard(ProjectClipboard &&other) noexcept = default;
    ProjectClipboard &operator=(ProjectClipboard &&other) noexcept = default;

    QVector<QString> rootIds;
    std::vector<std::unique_ptr<fh6::scene::Layer>> nodes;
};

class EditorState final : public QObject {
    Q_OBJECT

public:
    explicit EditorState(QObject *parent = nullptr);

    fh6::Project *project();
    const fh6::Project *project() const;
    bool hasProject() const;
    void setProject(fh6::Project project);

    bool isModified() const;
    void setModified(bool modified);

    QVector<fh6::scene::Shape *> selectedLayers();
    QVector<fh6::scene::GuideLayer *> selectedGuideLayers();
    QVector<fh6::scene::Group *> selectedGroups(const QVector<QString> &entryIds);
    bool isLayerLocked(const QString &layerId) const;
    QSet<QString> lockedLayerIds() const;
    QVector<QString> leafLayerIdsForEntry(const QString &entryId) const;
    bool entryHasLockedLayer(const QString &entryId) const;
    bool entryIsGroup(const QString &entryId) const;
    bool entryIsGuide(const QString &entryId) const;
    void setGroupAndDescendantLocked(const QString &groupId, bool locked);
    void setLayerLockScope(const QString &layerId, bool locked);
    void setLayerVisible(const QString &layerId, bool visible);
    void setLayerMask(const QString &layerId, bool mask);
    void setGuideLayerVisible(const QString &guideId, bool visible);
    void setGuideLayerLocked(const QString &guideId, bool locked);
    void setGroupDescendantVisible(const QString &groupId, bool visible);
    void setGroupDescendantMask(const QString &groupId, bool mask);
    void setGroupDescendantColor(const QString &groupId, const std::array<quint8, 4> &color);
    void setGroupDescendantOpacity(const QString &groupId, double opacity);

    QTransform groupLocalFrame(const QString &groupId) const;
    QTransform groupParentWorld(const QString &groupId) const;
    void transformGroupFrames(const QVector<QString> &groupIds, const QTransform &worldT);
    void setGroupFramesFromStart(const QHash<QString, QTransform> &startLocalFrames,
                                 const QTransform &worldT);
    QVector<QString> fullySelectedTopGroupIds() const;

    const fh6::scene::Group *sceneRoot() const;
    fh6::scene::Layer *sceneNode(const QString &id) const;

    QSet<QString> selectedLayerIds() const;
    void setSelectedLayerIds(const QSet<QString> &ids);
    QSet<QString> selectedGuideLayerIds() const;
    void setSelectedGuideLayerIds(const QSet<QString> &ids);
    void setSelectionIds(const QSet<QString> &layerIds, const QSet<QString> &guideLayerIds);
    void setSelectionFromEntries(const QSet<QString> &layerIds,
                                 const QSet<QString> &guideLayerIds,
                                 const QVector<QString> &entryIds);
    void clearSelection();
    void selectLayerAtPoint(const QString &layerId, Qt::KeyboardModifiers modifiers);

    void beginProjectEdit();
    void commitProjectEdit();
    void cancelProjectEdit();
    void beginTransformCommand();
    void beginTransformCommand(const QVector<QString> &targetIds);
    void commitTransformCommand();
    void cancelTransformCommand();
    void undo();
    void redo();

    void noteProjectGeometryChanged(bool refreshPreviews = false, const QVector<QString> &changedNodeIds = {});
    void noteTransformLiveChanged(const QVector<QString> &targetIds);
    void noteCanvasRepaint();
    void noteProjectStructureChanged();
    void noteClipboardChanged();
    void setActiveSectionId(const QString &sectionGroupId);
    void setToolName(const QString &name);
    const QVector<SceneRenderEntry> &renderEntries() const;
    QSet<QString> sectionIdsForNodes(const QVector<QString> &nodeIds) const;

    QVector<QString> normalizeEntrySelection(const QVector<QString> &entryIds) const;
    QVector<QString> selectedTransformTargetIds() const;
    bool copyEntriesToClipboard(const QVector<QString> &entries);
    void removeEntries(const QVector<QString> &entryIds);
    void insertClipboardAboveSelection(const ProjectClipboard &clipboard,
                                       const QVector<QString> &selectedEntries,
                                       QSet<QString> *newLayerSelection,
                                       QSet<QString> *newGuideLayerSelection = nullptr,
                                       bool renameCopies = true,
                                       QVector<QString> *newRootEntryIds = nullptr);
    bool duplicateEntriesInPlace(const QVector<QString> &entryIds,
                                 QSet<QString> *newLayerSelection = nullptr,
                                 QSet<QString> *newGuideLayerSelection = nullptr);
    void insertLayerAboveSelection(std::unique_ptr<fh6::scene::Layer> layer, const QVector<QString> &selectedEntries);
    void groupEntries(const QVector<QString> &entryIds);
    void ungroupEntries(const QVector<QString> &entryIds, bool flatten);
    bool reorderEntries(const QString &parentGroupId, const QVector<QString> &entryIds, int insertRow);
    bool reorderGuideLayers(const QVector<QString> &guideIds, int insertRow);
    QString copyName(const QString &name) const;
    const ProjectClipboard *clipboard() const;

    QSet<QString> existingLayerIds(const QSet<QString> &ids) const;
    QSet<QString> existingGuideLayerIds(const QSet<QString> &ids) const;
    QString parentGroupForEntry(const QString &entryId) const;
    QString topmostGroupForEntry(const QString &entryId) const;
    fh6::scene::Group *groupForId(const QString &groupId);
    const fh6::scene::Group *groupForId(const QString &groupId) const;
    QString uniqueLayerId() const;
    QString uniqueGuideLayerId() const;
    QString uniqueGroupId() const;
    bool buildEntryClipboard(const QVector<QString> &entries, ProjectClipboard &out) const;
    void insertClipboardAt(const ProjectClipboard &clipboard,
                           const QString &parentId, int insertAt, bool haveTarget, int guideInsertAt,
                           QSet<QString> *newLayerSelection, QSet<QString> *newGuideLayerSelection,
                           bool renameCopies, QVector<QString> *newRootEntryIds);
    void pruneEmptyGroups();

    fh6::Project project_;
    bool hasProject_ = false;
    QString activeSectionId_;
    QSet<QString> selectedLayerIds_;
    QSet<QString> selectedGuideLayerIds_;
    QVector<QString> selectedEntryIds_;
    bool modified_ = false;
    QVector<ProjectEditCommand> undoStack_;
    QVector<ProjectEditCommand> redoStack_;
    std::optional<ProjectEditCommand> pendingEdit_;
    std::optional<ProjectClipboard> clipboard_;

Q_SIGNALS:
    void projectReset();
    void projectGeometryChanged(bool refreshPreviews, const QVector<QString> &changedNodeIds);
    void transformLiveChanged(const QVector<QString> &targetIds);
    void canvasRepaintRequested();
    void projectStructureChanged();
    void selectionChanged();
    void clipboardChanged();
    void historyChanged();
    void activeSectionChanged(const QString &sectionGroupId);
    void toolNameChanged(const QString &name);
    void modifiedChanged(bool modified);

private:
    enum class HistoryDirection {
        Undo,
        Redo,
    };

    struct ProjectIndexCache {
        QHash<QString, fh6::scene::Shape *> layers;
        QHash<QString, fh6::scene::GuideLayer *> guides;
        QHash<QString, fh6::scene::Group *> groups;
        QHash<QString, fh6::scene::Layer *> nodes;
        QHash<QString, fh6::scene::Group *> parentGroupByChild;
        QHash<QString, QString> parentByChild;
        QHash<QString, int> orderByChild;
        mutable QHash<QString, QVector<QString>> leafIdsByEntry;
        mutable std::optional<QSet<QString>> lockedLayerIds;
    };

    struct EntryInsertionPoint {
        QVector<QString> entries;
        QString parentId;
        int row = 0;
        bool hasTarget = false;
    };

    void invalidateProjectIndexCache() const;
    const ProjectIndexCache &projectIndexCache() const;
    void ensureSceneTree() const;
    void invalidateSceneTree() const;
    void invalidateRenderCache() const;
    void ensureRenderCache() const;
    void updateRenderCacheTransforms(const QVector<QString> &targetIds) const;
    QVector<QString> existingEntryIds(const QVector<QString> &entryIds) const;
    QVector<QString> leafLayerIdsForEntryCached(const QString &entryId, const ProjectIndexCache &cache) const;
    bool entryHasLockedLayerCached(const QString &entryId, const ProjectIndexCache &cache) const;

    void applySnapshot(const ProjectEditSnapshot &snapshot);
    ProjectEditSnapshot captureSnapshot() const;
    ProjectSelectionSnapshot captureSelection() const;
    bool snapshotsEqual(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const;
    ProjectEditRefresh classifySnapshotRefresh(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const;
    void applyTransformEdits(const QVector<ProjectTransformEdit> &edits, bool useAfter);
    void applyHistoryCommand(HistoryDirection direction);
    void restoreSelection(const ProjectSelectionSnapshot &selection);
    void refreshAfterHistoryCommand(ProjectEditRefresh refresh);
    EntryInsertionPoint insertionPointAboveSelection(const QVector<QString> &selectedEntries) const;
    std::unique_ptr<fh6::scene::Layer> takeEntry(const QString &entryId);
    QString uniqueId(const QString &prefix, int startingIndex) const;

    mutable std::optional<ProjectIndexCache> indexCache_;

    mutable QHash<QString, fh6::scene::Layer *> sceneNodeById_;
    mutable bool renderCacheDirty_ = true;
    mutable QVector<SceneRenderEntry> renderEntries_;
    mutable QHash<QString, int> renderEntryByNodeId_;
};

} // namespace gui
