#include "editor_state.h"

namespace gui {

QVector<fh6::scene::Shape *> EditorState::selectedLayers() {
    QVector<fh6::scene::Shape *> result;
    if (!hasProject_) {
        return result;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    for (const QString &id : selectedLayerIds_) {
        if (fh6::scene::Shape *shape = cache.layers.value(id, nullptr)) {
            result.push_back(shape);
        }
    }
    return result;
}

QVector<fh6::scene::GuideLayer *> EditorState::selectedGuideLayers() {
    QVector<fh6::scene::GuideLayer *> result;
    if (!hasProject_) {
        return result;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    for (const QString &id : selectedGuideLayerIds_) {
        if (fh6::scene::GuideLayer *guide = cache.guides.value(id, nullptr)) {
            result.push_back(guide);
        }
    }
    return result;
}

QVector<fh6::scene::Group *> EditorState::selectedGroups(const QVector<QString> &entryIds) {
    QVector<fh6::scene::Group *> result;
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
        if (fh6::scene::Group *group = cache.groups.value(entryId, nullptr)) {
            result.push_back(group);
            seen.insert(entryId);
        }
    }
    return result;
}

QSet<QString> EditorState::selectedLayerIds() const {
    return selectedLayerIds_;
}

void EditorState::setSelectedLayerIds(const QSet<QString> &ids) {
    setSelectionIds(ids, {});
}

QSet<QString> EditorState::selectedGuideLayerIds() const {
    return selectedGuideLayerIds_;
}

void EditorState::setSelectedGuideLayerIds(const QSet<QString> &ids) {
    setSelectionIds({}, ids);
}

void EditorState::setSelectionIds(const QSet<QString> &layerIds, const QSet<QString> &guideLayerIds) {
    const QSet<QString> existingLayers = existingLayerIds(layerIds);
    const QSet<QString> existingGuides = existingGuideLayerIds(guideLayerIds);
    if (existingLayers == selectedLayerIds_ && existingGuides == selectedGuideLayerIds_
        && selectedEntryIds_.isEmpty()) {
        return;
    }
    selectedLayerIds_ = existingLayers;
    selectedGuideLayerIds_ = existingGuides;
    selectedEntryIds_.clear();
    Q_EMIT selectionChanged();
}

void EditorState::setSelectionFromEntries(const QSet<QString> &layerIds,
                                          const QSet<QString> &guideLayerIds,
                                          const QVector<QString> &entryIds) {
    const QSet<QString> existingLayers = existingLayerIds(layerIds);
    const QSet<QString> existingGuides = existingGuideLayerIds(guideLayerIds);
    QVector<QString> existingEntries;
    existingEntries.reserve(entryIds.size());
    QSet<QString> seen;
    const ProjectIndexCache &cache = projectIndexCache();
    for (const QString &id : entryIds) {
        if (!id.isEmpty() && !seen.contains(id) && cache.nodes.contains(id)) {
            existingEntries.push_back(id);
            seen.insert(id);
        }
    }
    existingEntries = normalizeEntrySelection(existingEntries);
    if (existingLayers == selectedLayerIds_ && existingGuides == selectedGuideLayerIds_
        && existingEntries == selectedEntryIds_) {
        return;
    }
    selectedLayerIds_ = existingLayers;
    selectedGuideLayerIds_ = existingGuides;
    selectedEntryIds_ = existingEntries;
    Q_EMIT selectionChanged();
}

void EditorState::clearSelection() {
    setSelectionIds({}, {});
}

void EditorState::selectLayerAtPoint(const QString &layerId, Qt::KeyboardModifiers modifiers) {
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

QVector<QString> EditorState::normalizeEntrySelection(const QVector<QString> &entryIds) const {
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
