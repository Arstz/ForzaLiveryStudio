// Selection state and queries split out of editor_state.cpp: selected
// layer/guide id sets, pointer resolution over the project arrays, group
// selection, click-selection semantics, and ancestor-aware selection
// normalization.

#include "editor_state.h"

#include "editor_state_internal.h"

namespace gui {

QVector<fh6::ShapeLayer *> EditorState::selectedLayers()
{
    if (!hasProject_) {
        return {};
    }
    return detail::selectedItems(project_.layers, selectedLayerIds_);
}

QVector<fh6::GuideLayer *> EditorState::selectedGuideLayers()
{
    if (!hasProject_) {
        return {};
    }
    return detail::selectedItems(project_.guideLayers, selectedGuideLayerIds_);
}

QVector<fh6::LayerGroup *> EditorState::selectedGroups(const QVector<QString> &entryIds)
{
    QVector<fh6::LayerGroup *> result;
    if (!hasProject_) {
        return result;
    }
    const QVector<QString> entries = normalizeEntrySelection(entryIds);
    const ProjectIndexCache &cache = projectIndexCache();
    QSet<QString> seen;
    for (const QString &entryId : entries) {
        if (seen.contains(entryId)) {
            continue;
        }
        const auto groupIt = cache.groupIndexById.constFind(entryId);
        if (groupIt != cache.groupIndexById.constEnd()) {
            result.push_back(&project_.groups[groupIt.value()]);
            seen.insert(entryId);
        }
    }
    return result;
}

QSet<QString> EditorState::selectedLayerIds() const
{
    return selectedLayerIds_;
}

void EditorState::setSelectedLayerIds(const QSet<QString> &ids)
{
    setSelectionIds(ids, {});
}

QSet<QString> EditorState::selectedGuideLayerIds() const
{
    return selectedGuideLayerIds_;
}

void EditorState::setSelectedGuideLayerIds(const QSet<QString> &ids)
{
    setSelectionIds({}, ids);
}

void EditorState::setSelectionIds(const QSet<QString> &layerIds, const QSet<QString> &guideLayerIds)
{
    const QSet<QString> existingLayers = existingLayerIds(layerIds);
    const QSet<QString> existingGuides = existingGuideLayerIds(guideLayerIds);
    if (existingLayers == selectedLayerIds_ && existingGuides == selectedGuideLayerIds_) {
        return;
    }
    selectedLayerIds_ = existingLayers;
    selectedGuideLayerIds_ = existingGuides;
    Q_EMIT selectionChanged();
}

void EditorState::clearSelection()
{
    setSelectionIds({}, {});
}

void EditorState::selectLayerAtPoint(const QString &layerId, Qt::KeyboardModifiers modifiers)
{
    QSet<QString> ids = selectedLayerIds_;
    if (layerId.isEmpty()) {
        if (!(modifiers & (Qt::ShiftModifier | Qt::ControlModifier))) {
            ids.clear();
        }
    } else if (modifiers & Qt::ControlModifier) {
        if (ids.contains(layerId)) {
            ids.remove(layerId);
        } else {
            ids.insert(layerId);
        }
    } else if (modifiers & Qt::ShiftModifier) {
        const QString groupId = topmostGroupForEntry(layerId);
        if (!groupId.isEmpty()) {
            ids.clear();
            for (const QString &id : leafLayerIdsForEntry(groupId)) {
                ids.insert(id);
            }
        } else {
            ids = {layerId};
        }
    } else {
        ids = {layerId};
    }
    setSelectedLayerIds(ids);
}

QVector<QString> EditorState::normalizeEntrySelection(const QVector<QString> &entryIds) const
{
    QVector<QString> result;
    QSet<QString> selectedSet;
    for (const QString &id : entryIds) {
        selectedSet.insert(id);
    }
    const ProjectIndexCache &cache = projectIndexCache();
    QSet<QString> resultSet;
    for (const QString &id : entryIds) {
        bool hasSelectedAncestor = false;
        for (QString parent = cache.parentByChild.value(id); !parent.isEmpty(); parent = cache.parentByChild.value(parent)) {
            if (selectedSet.contains(parent)) {
                hasSelectedAncestor = true;
                break;
            }
        }
        if (!hasSelectedAncestor && !resultSet.contains(id)) {
            result.push_back(id);
            resultSet.insert(id);
        }
    }
    return result;
}

} // namespace gui
