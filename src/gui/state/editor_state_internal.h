#pragma once

// Shared file-local helpers for the EditorState translation units
// (editor_state.cpp and its editor_state_*.cpp splits). These were anonymous-
// namespace helpers when EditorState was a single file; only the helpers used
// by more than one unit live here, the rest stay local to their unit.

#include "core_types.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace gui::detail {

// Returns pointers to the items whose id is in ids, in container order.
// Collapses the identical ShapeLayer/GuideLayer selection loops.
template <typename T>
QVector<T *> selectedItems(QVector<T> &items, const QSet<QString> &ids)
{
    QVector<T *> result;
    for (T &item : items) {
        if (ids.contains(item.id)) {
            result.push_back(&item);
        }
    }
    return result;
}

// First "<prefix>_<n>" id not already used by an item in the container.
// Collapses the identical uniqueLayerId/uniqueGuideLayerId/uniqueGroupId loops.
template <typename Container>
QString uniqueEntryId(const Container &items, const QString &prefix)
{
    QSet<QString> used;
    for (const auto &item : items) {
        used.insert(item.id);
    }
    int index = static_cast<int>(items.size()) + 1;
    QString id;
    do {
        id = QStringLiteral("%1_%2").arg(prefix).arg(index++);
    } while (used.contains(id));
    return id;
}

// Equality over the id/name/transform/visibility subset that ShapeLayer and
// GuideLayer share; layersEqual/guideLayersEqual add their type-specific fields.
template <typename T>
bool commonEntryFieldsEqual(const T &a, const T &b)
{
    return a.id == b.id
        && a.name == b.name
        && a.x == b.x
        && a.y == b.y
        && a.scaleX == b.scaleX
        && a.scaleY == b.scaleY
        && a.rotation == b.rotation
        && a.visible == b.visible
        && a.locked == b.locked;
}

// Id-indexed lookup tables over a project's layers/groups and the child->parent
// and child->sibling-order relations of the entry tree.
struct ProjectEntryMaps {
    QHash<QString, const fh6::ShapeLayer *> layers;
    QHash<QString, const fh6::LayerGroup *> groups;
    QHash<QString, QString> parentByChild;
    QHash<QString, int> orderByChild;
};

ProjectEntryMaps buildEntryMaps(const fh6::Project &project);
bool subtreeHasLockedLayer(const QString &entryId, const ProjectEntryMaps &maps);
QVector<QString> leafIdsForEntry(const QString &entryId, const ProjectEntryMaps &maps);
void collectLayerOrder(const QString &entryId, const ProjectEntryMaps &maps, QVector<QString> *layerIds);
// Rewrites project->layers into orderedLayerIds order (unknown ids appended in
// their existing relative order).
void applyLayerOrder(fh6::Project *project, const QVector<QString> &orderedLayerIds);
bool guideLayersEqual(const fh6::GuideLayer &a, const fh6::GuideLayer &b);

} // namespace gui::detail
