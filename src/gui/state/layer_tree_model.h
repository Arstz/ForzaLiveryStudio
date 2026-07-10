#pragma once

#include "core_types.h"
#include "editor_state.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

namespace gui {

// O(1) id -> entry lookup built once per refresh so tree rebuilds and preview
// regeneration stay linear instead of degrading to O(N^2) linear scans.
struct ProjectLookup {
    QHash<QString, const fh6::scene::Shape *> layers;
    QHash<QString, const fh6::scene::GuideLayer *> guides;
    QHash<QString, const fh6::scene::Group *> groups;
};

QImage renderProjectPreviewImage(const fh6::Project &project, const QSize &size);

class LayerTreeModel final : public QStandardItemModel {
public:
    static constexpr int EntryIdRole = Qt::UserRole;
    static constexpr int LeafIdsRole = Qt::UserRole + 1;
    static constexpr int IsGroupRole = Qt::UserRole + 2;
    static constexpr int VisibleRole = Qt::UserRole + 3;
    static constexpr int MaskRole = Qt::UserRole + 4;
    static constexpr int OwnLockedRole = Qt::UserRole + 5;
    static constexpr int EffectiveLockedRole = Qt::UserRole + 6;
    static constexpr int IsGuideRole = Qt::UserRole + 7;
    static constexpr int GuideIdsRole = Qt::UserRole + 8;
    // Draw-order position shown at the row's trailing edge: "#3" for a leaf, "#3-7" for a
    // group's covered range. Empty for guides. Position 1 is the top (foreground) of the list.
    static constexpr int PositionTextRole = Qt::UserRole + 9;

    explicit LayerTreeModel(QObject *parent = nullptr);
    void setEditorState(EditorState *state);
    void setProject(const fh6::Project *project);
    // Populate the tree with the contents of a single group (used for the
    // C_livery per-section view, where each section is shown on its own).
    void setProjectSection(const fh6::Project *project, const QString &sectionGroupId);
    void clearSectionCache();
    void refreshStateRoles(const fh6::Project *project);
    void refreshPreviews(const fh6::Project *project);

    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDragActions() const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;

private:
    QStandardItem *itemForId(const ProjectLookup &lookup, const QString &id, bool ancestorLocked = false) const;
    QIcon previewIconForId(const ProjectLookup &lookup, const QString &id) const;
    QIcon previewIconForNode(const fh6::scene::Layer &node) const;
    void cacheDisplayedSectionRows();
    void refreshStateRolesForParent(const ProjectLookup &lookup, const QModelIndex &parent, bool ancestorLocked);
    void refreshPreviewsForParent(const ProjectLookup &lookup, const QModelIndex &parent);

    ShapeGeometryStore geometry_;
    EditorState *state_ = nullptr;
    QString displayParentGroupId_;
    // Leaf id -> 1-based draw-order position (top of the list is 1), recomputed on each rebuild.
    QHash<QString, int> leafPositions_;
    QHash<QString, QList<QList<QStandardItem *>>> sectionRowsCache_;
    bool geometryLoaded_ = false;
    // Cache of rasterized shape/group thumbnails keyed by appearance signature (not entry id),
    // so entries that look identical share one pixmap. In particular a duplicated clone has the
    // same signature as its source, so duplication reuses the cached thumbnail instead of
    // re-rasterizing - the dominant cost on duplicate/stamp/paste. Live recolor only re-renders
    // the entries whose signature actually changed.
    mutable QHash<quint64, QIcon> previewCache_;
    mutable QHash<QString, quint64> previewSignatureCache_;
};

} // namespace gui
