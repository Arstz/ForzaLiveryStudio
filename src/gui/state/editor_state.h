#pragma once

#include "core_types.h"

#include <QHash>
#include <QObject>
#include <QPersistentModelIndex>
#include <QSet>
#include <QVector>

#include <optional>

namespace gui {

struct ProjectEditSnapshot {
    QVector<fh6::ShapeLayer> layers;
    QVector<fh6::GuideLayer> guideLayers;
    QVector<fh6::LayerGroup> groups;
    QVector<QString> rootChildIds;
};

struct ProjectSelectionSnapshot {
    QSet<QString> layerIds;
    QSet<QString> guideLayerIds;
};

enum class ProjectEditRefresh {
    GeometryOnly,
    Previews,
    Structure,
};

struct ProjectEditCommand {
    ProjectEditSnapshot before;
    ProjectEditSnapshot after;
    ProjectSelectionSnapshot undoSelection;
    ProjectSelectionSnapshot redoSelection;
    ProjectEditRefresh refresh = ProjectEditRefresh::Structure;
};

struct ProjectClipboard {
    QVector<QString> rootIds;
    QVector<fh6::ShapeLayer> layers;
    QVector<fh6::GuideLayer> guideLayers;
    QVector<fh6::LayerGroup> groups;
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

    QVector<fh6::ShapeLayer *> selectedLayers();
    QVector<fh6::GuideLayer *> selectedGuideLayers();
    QVector<fh6::LayerGroup *> selectedGroups(const QVector<QString> &entryIds);
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

    QSet<QString> selectedLayerIds() const;
    void setSelectedLayerIds(const QSet<QString> &ids);
    QSet<QString> selectedGuideLayerIds() const;
    void setSelectedGuideLayerIds(const QSet<QString> &ids);
    void setSelectionIds(const QSet<QString> &layerIds, const QSet<QString> &guideLayerIds);
    void clearSelection();
    void selectLayerAtPoint(const QString &layerId, Qt::KeyboardModifiers modifiers);

    void beginProjectEdit();
    void commitProjectEdit();
    void cancelProjectEdit();
    void beginTransformCommand();
    void commitTransformCommand();
    void cancelTransformCommand();
    void undo();
    void redo();

    void noteProjectGeometryChanged(bool refreshPreviews = false);
    // Lightweight live-edit signal: repaint the canvas only, skipping the tree preview/state
    // refresh and property-panel rebuild. Used for high-frequency previews (e.g. dragging the
    // colour picker) where the full geometry refresh is too costly per tick.
    void noteCanvasRepaint();
    void noteProjectStructureChanged();
    void noteClipboardChanged();
    void setActiveSectionId(const QString &sectionGroupId);
    void setToolName(const QString &name);

    QVector<QString> normalizeEntrySelection(const QVector<QString> &entryIds) const;
    bool copyEntriesToClipboard(const QVector<QString> &entries);
    void removeEntries(const QVector<QString> &entryIds);
    void insertClipboardAboveSelection(const ProjectClipboard &clipboard,
                                       const QVector<QString> &selectedEntries,
                                       QSet<QString> *newLayerSelection,
                                       QSet<QString> *newGuideLayerSelection = nullptr,
                                       bool renameCopies = true);
    bool duplicateEntriesInPlace(const QVector<QString> &entryIds,
                                 QSet<QString> *newLayerSelection = nullptr,
                                 QSet<QString> *newGuideLayerSelection = nullptr);
    void insertLayerAboveSelection(const fh6::ShapeLayer &layer, const QVector<QString> &selectedEntries);
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
    QVector<QString> *childListForParent(const QString &parentGroupId);
    const QVector<QString> *childListForParent(const QString &parentGroupId) const;
    QString uniqueLayerId() const;
    QString uniqueGuideLayerId() const;
    QString uniqueGroupId() const;
    bool buildEntryClipboard(const QVector<QString> &entries, ProjectClipboard &out) const;
    void insertClipboardAt(const ProjectClipboard &clipboard,
                           const QString &parentId, int insertAt, bool haveTarget, int guideInsertAt,
                           QSet<QString> *newLayerSelection, QSet<QString> *newGuideLayerSelection,
                           bool renameCopies);
    void pruneEmptyGroups();

    fh6::Project project_;
    bool hasProject_ = false;
    QString activeSectionId_;
    QSet<QString> selectedLayerIds_;
    QSet<QString> selectedGuideLayerIds_;
    bool modified_ = false;
    QVector<ProjectEditCommand> undoStack_;
    QVector<ProjectEditCommand> redoStack_;
    std::optional<ProjectEditCommand> pendingEdit_;
    std::optional<ProjectClipboard> clipboard_;

Q_SIGNALS:
    void projectReset();
    void projectGeometryChanged(bool refreshPreviews);
    void canvasRepaintRequested();
    void projectStructureChanged();
    void selectionChanged();
    void clipboardChanged();
    void historyChanged();
    void activeSectionChanged(const QString &sectionGroupId);
    void toolNameChanged(const QString &name);
    void modifiedChanged(bool modified);

private:
    struct ProjectIndexCache {
        QHash<QString, int> layerIndexById;
        QHash<QString, int> guideIndexById;
        QHash<QString, int> groupIndexById;
        QHash<QString, QString> parentByChild;
        QHash<QString, int> orderByChild;
        mutable QHash<QString, QVector<QString>> leafIdsByEntry;
        mutable std::optional<QSet<QString>> lockedLayerIds;
    };

    void invalidateProjectIndexCache() const;
    const ProjectIndexCache &projectIndexCache() const;
    QVector<QString> leafLayerIdsForEntryCached(const QString &entryId, const ProjectIndexCache &cache) const;
    bool entryHasLockedLayerCached(const QString &entryId, const ProjectIndexCache &cache) const;

    void applySnapshot(const ProjectEditSnapshot &snapshot);
    ProjectEditSnapshot captureSnapshot() const;
    bool snapshotsEqual(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const;
    ProjectEditRefresh classifySnapshotRefresh(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const;
    void refreshAfterHistoryCommand(ProjectEditRefresh refresh);

    mutable std::optional<ProjectIndexCache> indexCache_;
};

} // namespace gui
