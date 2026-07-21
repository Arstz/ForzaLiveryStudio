#pragma once

#include "core_types.h"
#include "editor_state.h"
#include "shape_geometry_store.h"
#include "theme_manager.h"

#include <QtCore>
#include <QtGui>

namespace gui {

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
    static constexpr int PositionTextRole = Qt::UserRole + 9;

    explicit LayerTreeModel(QObject *parent = nullptr);
    ~LayerTreeModel() override;
    void setEditorState(EditorState *state);
    void setGeneratePreviewsWithTransformations(bool enabled);
    void setPreviewBackground(const PreviewBackground &background, UiTheme theme);
    bool generatePreviewsWithTransformations() const;
    void setProject(const fh6::Project *project);
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
    QIcon previewIconForNode(const fh6::scene::Layer &node) const;
    void populateGroup(const ProjectLookup &lookup, const fh6::scene::Group &group);
    void cacheDisplayedSectionRows();
    bool updateItemState(QStandardItem &item, const fh6::scene::Layer &node, bool ancestorLocked) const;
    void updateItemPreview(QStandardItem &item, const fh6::scene::Layer &node) const;
    void refreshStateRolesForParent(const ProjectLookup &lookup, const QModelIndex &parent, bool ancestorLocked);
    void refreshPreviewsForParent(const ProjectLookup &lookup, const QModelIndex &parent);

    ShapeGeometryStore geometry_;
    PreviewBackground previewBackground_;
    UiTheme theme_ = UiTheme::Dark;
    EditorState *state_ = nullptr;
    QString displayParentGroupId_;
    QHash<QString, int> leafPositions_;
    QHash<QString, QList<QList<QStandardItem *>>> sectionRowsCache_;
    bool geometryLoaded_ = false;
    bool generatePreviewsWithTransformations_ = false;
    mutable QHash<quint64, QIcon> previewCache_;
    mutable QHash<QString, quint64> previewSignatureCache_;
};

} // namespace gui
