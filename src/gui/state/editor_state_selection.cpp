#include "editor_state.h"

namespace gui {
namespace {

template <typename LayerType>
QVector<LayerType *> selectedNodes(const QSet<QString> &ids, const QHash<QString, LayerType *> &nodes) {
    QVector<LayerType *> result;
    for (const QString &id : ids) {
        if (LayerType *node = nodes.value(id, nullptr)) {
            result.push_back(node);
        }
    }

    return result;
}

}

QVector<fls::scene::Shape *> EditorState::selectedLayers() {
    if (!hasProject_) {
        return {};
    }

    return selectedNodes(selectedLayerIds_, projectIndexCache().layers);
}

QVector<fls::scene::Layer *> EditorState::selectedContours() {
    if (!hasProject_) {
        return {};
    }

    QVector<fls::scene::Layer *> result;
    for (const auto &id : selectedLayerIds_) {
        auto *node = sceneNode(id);
        if (node != nullptr && fls::scene::isContourGeometry(node->kind())) result.push_back(node);
    }

    return result;
}

QVector<fls::scene::GuideLayer *> EditorState::selectedGuideLayers() {
    if (!hasProject_) {
        return {};
    }

    return selectedNodes(selectedGuideLayerIds_, projectIndexCache().guides);
}

QVector<fls::scene::Group *> EditorState::selectedGroups(const QVector<QString> &entryIds) {
    QVector<fls::scene::Group *> result;
    if (!hasProject_) {
        return result;
    }
    const QVector<QString> entries = normalizeEntrySelection(entryIds);
    const ProjectIndexCache &cache = projectIndexCache();
    for (const QString &entryId : entries) {
        if (fls::scene::Group *group = cache.groups.value(entryId, nullptr)) {
            result.push_back(group);
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
    const QVector<QString> existingEntries = existingEntryIds(entryIds);
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
        const QString groupId = nextGroupForLayerSelection(layerId);
        if (!groupId.isEmpty()) {
            QSet<QString> guideIds;

            ids.clear();
            for (const QString &id : leafLayerIdsForEntry(groupId)) {
                ids.insert(id);
            }
            for (const fls::scene::GuideLayer *guide : sceneGuideLeaves(*groupForId(groupId))) {
                guideIds.insert(guide->id);
            }
            setSelectionFromEntries(ids, guideIds, {groupId});

            return;
        } else {
            ids = {layerId};
        }
    } else {
        ids = {layerId};
    }
    setSelectedLayerIds(ids);
}

QString EditorState::nextGroupForLayerSelection(const QString &layerId) const {
    QString groupId = parentGroupForEntry(layerId);
    const fls::scene::Group *selectedGroup = selectedEntryIds_.size() == 1
        ? groupForId(selectedEntryIds_.front()) : nullptr;

    if (selectedGroup != nullptr && !selectedGroup->isLiverySection && selectedLayerIds_.contains(layerId)) {
        for (QString ancestor = groupId; !ancestor.isEmpty(); ancestor = parentGroupForEntry(ancestor)) {
            if (ancestor != selectedGroup->id) {
                continue;
            }
            const QString parentId = parentGroupForEntry(ancestor);
            const fls::scene::Group *parent = parentId.isEmpty() ? nullptr : groupForId(parentId);

            groupId = parent != nullptr && !parent->isLiverySection ? parentId : ancestor;
            break;
        }
    }
    const fls::scene::Group *group = groupId.isEmpty() ? nullptr : groupForId(groupId);

    return group != nullptr && !group->isLiverySection ? groupId : QString();
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
