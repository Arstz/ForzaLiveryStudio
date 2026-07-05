// Shared helpers for the EditorState translation units, declared in
// editor_state_internal.h. Moved verbatim from the anonymous namespace of the
// pre-split editor_state.cpp.

#include "editor_state_internal.h"

#include <QSet>

namespace gui::detail {

ProjectEntryMaps buildEntryMaps(const fh6::Project &project)
{
    ProjectEntryMaps maps;
    maps.layers.reserve(project.layers.size());
    maps.groups.reserve(project.groups.size());
    maps.parentByChild.reserve(project.rootChildIds.size() + project.layers.size() + project.groups.size());
    maps.orderByChild.reserve(project.rootChildIds.size() + project.layers.size() + project.groups.size());
    for (const fh6::ShapeLayer &layer : project.layers) {
        maps.layers.insert(layer.id, &layer);
    }
    for (const fh6::LayerGroup &group : project.groups) {
        maps.groups.insert(group.id, &group);
    }
    for (int i = 0; i < project.rootChildIds.size(); ++i) {
        maps.parentByChild.insert(project.rootChildIds[i], QString());
        maps.orderByChild.insert(project.rootChildIds[i], i);
    }
    for (const fh6::LayerGroup &group : project.groups) {
        for (int i = 0; i < group.childIds.size(); ++i) {
            maps.parentByChild.insert(group.childIds[i], group.id);
            maps.orderByChild.insert(group.childIds[i], i);
        }
    }
    return maps;
}

bool subtreeHasLockedLayer(const QString &entryId, const ProjectEntryMaps &maps)
{
    if (const fh6::ShapeLayer *layer = maps.layers.value(entryId, nullptr)) {
        return layer->locked;
    }
    const fh6::LayerGroup *group = maps.groups.value(entryId, nullptr);
    if (group == nullptr) {
        return false;
    }
    if (group->locked) {
        return true;
    }
    for (const QString &childId : group->childIds) {
        if (subtreeHasLockedLayer(childId, maps)) {
            return true;
        }
    }
    return false;
}

QVector<QString> leafIdsForEntry(const QString &entryId, const ProjectEntryMaps &maps)
{
    if (maps.layers.contains(entryId)) {
        return {entryId};
    }
    const fh6::LayerGroup *group = maps.groups.value(entryId, nullptr);
    if (group == nullptr) {
        return {};
    }
    QVector<QString> ids;
    for (const QString &childId : group->childIds) {
        ids += leafIdsForEntry(childId, maps);
    }
    return ids;
}

void collectLayerOrder(const QString &entryId, const ProjectEntryMaps &maps, QVector<QString> *layerIds)
{
    if (layerIds == nullptr) {
        return;
    }
    if (maps.layers.contains(entryId)) {
        layerIds->push_back(entryId);
        return;
    }
    const fh6::LayerGroup *group = maps.groups.value(entryId, nullptr);
    if (group == nullptr) {
        return;
    }
    for (const QString &childId : group->childIds) {
        collectLayerOrder(childId, maps, layerIds);
    }
}

void applyLayerOrder(fh6::Project *project, const QVector<QString> &orderedLayerIds)
{
    if (project == nullptr) {
        return;
    }
    QHash<QString, fh6::ShapeLayer> byId;
    byId.reserve(project->layers.size());
    for (const fh6::ShapeLayer &layer : project->layers) {
        byId.insert(layer.id, layer);
    }

    QVector<fh6::ShapeLayer> reordered;
    reordered.reserve(project->layers.size());
    QSet<QString> seen;
    for (const QString &id : orderedLayerIds) {
        auto it = byId.constFind(id);
        if (it == byId.constEnd() || seen.contains(id)) {
            continue;
        }
        reordered.push_back(it.value());
        seen.insert(id);
    }
    for (const fh6::ShapeLayer &layer : project->layers) {
        if (!seen.contains(layer.id)) {
            reordered.push_back(layer);
        }
    }
    project->layers = reordered;
}

bool guideLayersEqual(const fh6::GuideLayer &a, const fh6::GuideLayer &b)
{
    return commonEntryFieldsEqual(a, b)
        && a.sourcePath == b.sourcePath
        && a.imageBytes == b.imageBytes
        && a.pixelBytes == b.pixelBytes
        && a.imageFormat == b.imageFormat
        && a.width == b.width
        && a.height == b.height
        && a.opacity == b.opacity;
}

} // namespace gui::detail
