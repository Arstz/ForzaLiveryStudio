// EditorState core: construction, project ownership, the project index cache,
// lock/visibility/mask/color propagation, tree queries, unique-id generation,
// and the shared Qt signals. Selection, clipboard, grouping, and undo/redo
// live in the editor_state_*.cpp sibling units; helpers shared between them
// are in editor_state_internal.h / editor_state_util.cpp.

#include "editor_state.h"

#include "editor_state_internal.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace gui {
namespace {

const fh6::LayerGroup *groupForId(const fh6::Project &project, const QString &id)
{
    for (const fh6::LayerGroup &group : project.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

fh6::LayerGroup *groupForId(fh6::Project &project, const QString &id)
{
    for (fh6::LayerGroup &group : project.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

} // namespace

EditorState::EditorState(QObject *parent)
    : QObject(parent)
{
}

void EditorState::invalidateProjectIndexCache() const
{
    indexCache_.reset();
}

const EditorState::ProjectIndexCache &EditorState::projectIndexCache() const
{
    if (!indexCache_.has_value()) {
        ProjectIndexCache cache;
        cache.layerIndexById.reserve(project_.layers.size());
        cache.guideIndexById.reserve(project_.guideLayers.size());
        cache.groupIndexById.reserve(project_.groups.size());
        cache.parentByChild.reserve(project_.rootChildIds.size() + project_.layers.size() + project_.groups.size());
        cache.orderByChild.reserve(project_.rootChildIds.size() + project_.layers.size() + project_.groups.size());
        for (int i = 0; i < project_.layers.size(); ++i) {
            cache.layerIndexById.insert(project_.layers[i].id, i);
        }
        for (int i = 0; i < project_.guideLayers.size(); ++i) {
            cache.guideIndexById.insert(project_.guideLayers[i].id, i);
        }
        for (int i = 0; i < project_.groups.size(); ++i) {
            cache.groupIndexById.insert(project_.groups[i].id, i);
        }
        for (int i = 0; i < project_.rootChildIds.size(); ++i) {
            cache.parentByChild.insert(project_.rootChildIds[i], QString());
            cache.orderByChild.insert(project_.rootChildIds[i], i);
        }
        for (const fh6::LayerGroup &group : project_.groups) {
            for (int i = 0; i < group.childIds.size(); ++i) {
                cache.parentByChild.insert(group.childIds[i], group.id);
                cache.orderByChild.insert(group.childIds[i], i);
            }
        }
        indexCache_ = std::move(cache);
    }
    return *indexCache_;
}

QVector<QString> EditorState::leafLayerIdsForEntryCached(const QString &entryId, const ProjectIndexCache &cache) const
{
    const auto cached = cache.leafIdsByEntry.constFind(entryId);
    if (cached != cache.leafIdsByEntry.constEnd()) {
        return cached.value();
    }
    QVector<QString> ids;
    if (cache.layerIndexById.contains(entryId)) {
        ids.push_back(entryId);
    } else {
        const auto groupIt = cache.groupIndexById.constFind(entryId);
        if (groupIt != cache.groupIndexById.constEnd()) {
            const fh6::LayerGroup &group = project_.groups[groupIt.value()];
            for (const QString &childId : group.childIds) {
                ids += leafLayerIdsForEntryCached(childId, cache);
            }
        }
    }
    cache.leafIdsByEntry.insert(entryId, ids);
    return ids;
}

bool EditorState::entryHasLockedLayerCached(const QString &entryId, const ProjectIndexCache &cache) const
{
    const QSet<QString> locked = lockedLayerIds();
    if (cache.layerIndexById.contains(entryId)) {
        return locked.contains(entryId);
    }
    const auto groupIt = cache.groupIndexById.constFind(entryId);
    if (groupIt == cache.groupIndexById.constEnd()) {
        return false;
    }
    const fh6::LayerGroup &group = project_.groups[groupIt.value()];
    if (group.locked) {
        return true;
    }
    for (const QString &leafId : leafLayerIdsForEntryCached(entryId, cache)) {
        if (locked.contains(leafId)) {
            return true;
        }
    }
    return false;
}

fh6::Project *EditorState::project()
{
    return hasProject_ ? &project_ : nullptr;
}

const fh6::Project *EditorState::project() const
{
    return hasProject_ ? &project_ : nullptr;
}

bool EditorState::hasProject() const
{
    return hasProject_;
}

void EditorState::setProject(fh6::Project project)
{
    project_ = std::move(project);
    invalidateProjectIndexCache();
    hasProject_ = true;
    activeSectionId_.clear();
    selectedLayerIds_.clear();
    selectedGuideLayerIds_.clear();
    undoStack_.clear();
    redoStack_.clear();
    pendingEdit_.reset();
    setModified(false);
    Q_EMIT projectReset();
    Q_EMIT selectionChanged();
    Q_EMIT historyChanged();
}

bool EditorState::isModified() const
{
    return modified_;
}

void EditorState::setModified(bool modified)
{
    if (modified_ == modified) {
        return;
    }
    modified_ = modified;
    Q_EMIT modifiedChanged(modified_);
}

bool EditorState::isLayerLocked(const QString &layerId) const
{
    return lockedLayerIds().contains(layerId);
}

QSet<QString> EditorState::lockedLayerIds() const
{
    if (!hasProject_) {
        return {};
    }
    const ProjectIndexCache &cache = projectIndexCache();
    if (cache.lockedLayerIds.has_value()) {
        return *cache.lockedLayerIds;
    }
    QSet<QString> locked;
    for (const fh6::ShapeLayer &layer : project_.layers) {
        if (layer.locked) {
            locked.insert(layer.id);
            continue;
        }
        for (QString parent = cache.parentByChild.value(layer.id); !parent.isEmpty(); parent = cache.parentByChild.value(parent)) {
            const auto groupIt = cache.groupIndexById.constFind(parent);
            if (groupIt != cache.groupIndexById.constEnd() && project_.groups[groupIt.value()].locked) {
                locked.insert(layer.id);
                break;
            }
        }
    }
    cache.lockedLayerIds = locked;
    return locked;
}

QVector<QString> EditorState::leafLayerIdsForEntry(const QString &entryId) const
{
    if (!hasProject_) {
        return {};
    }
    return leafLayerIdsForEntryCached(entryId, projectIndexCache());
}

bool EditorState::entryHasLockedLayer(const QString &entryId) const
{
    if (!hasProject_) {
        return false;
    }
    return entryHasLockedLayerCached(entryId, projectIndexCache());
}

bool EditorState::entryIsGroup(const QString &entryId) const
{
    return hasProject_ && projectIndexCache().groupIndexById.contains(entryId);
}

bool EditorState::entryIsGuide(const QString &entryId) const
{
    return hasProject_ && projectIndexCache().guideIndexById.contains(entryId);
}

void EditorState::setGroupAndDescendantLocked(const QString &groupId, bool locked)
{
    invalidateProjectIndexCache();
    fh6::LayerGroup *group = groupForId(project_, groupId);
    if (group == nullptr) {
        return;
    }
    group->locked = locked;
    const QVector<QString> children = group->childIds;
    for (const QString &childId : children) {
        if (fh6::LayerGroup *childGroup = groupForId(project_, childId)) {
            setGroupAndDescendantLocked(childGroup->id, locked);
            continue;
        }
        for (fh6::ShapeLayer &layer : project_.layers) {
            if (layer.id == childId) {
                layer.locked = locked;
                break;
            }
        }
    }
}

void EditorState::setLayerLockScope(const QString &layerId, bool locked)
{
    invalidateProjectIndexCache();
    const QString parentGroupId = parentGroupForEntry(layerId);
    if (!parentGroupId.isEmpty()) {
        setGroupAndDescendantLocked(parentGroupId, locked);
        return;
    }
    for (fh6::ShapeLayer &layer : project_.layers) {
        if (layer.id == layerId) {
            layer.locked = locked;
            return;
        }
    }
}

void EditorState::setLayerVisible(const QString &layerId, bool visible)
{
    for (fh6::ShapeLayer &layer : project_.layers) {
        if (layer.id == layerId) {
            layer.visible = visible;
            return;
        }
    }
}

void EditorState::setLayerMask(const QString &layerId, bool mask)
{
    for (fh6::ShapeLayer &layer : project_.layers) {
        if (layer.id == layerId) {
            layer.mask = mask;
            return;
        }
    }
}

void EditorState::setGuideLayerVisible(const QString &guideId, bool visible)
{
    for (fh6::GuideLayer &guide : project_.guideLayers) {
        if (guide.id == guideId) {
            guide.visible = visible;
            return;
        }
    }
}

void EditorState::setGuideLayerLocked(const QString &guideId, bool locked)
{
    for (fh6::GuideLayer &guide : project_.guideLayers) {
        if (guide.id == guideId) {
            guide.locked = locked;
            return;
        }
    }
}

void EditorState::setGroupDescendantVisible(const QString &groupId, bool visible)
{
    const fh6::LayerGroup *group = groupForId(project_, groupId);
    if (group == nullptr) {
        return;
    }
    const QVector<QString> children = group->childIds;
    for (const QString &childId : children) {
        if (groupForId(project_, childId) != nullptr) {
            setGroupDescendantVisible(childId, visible);
            continue;
        }
        for (fh6::ShapeLayer &layer : project_.layers) {
            if (layer.id == childId) {
                layer.visible = visible;
                break;
            }
        }
    }
}

void EditorState::setGroupDescendantMask(const QString &groupId, bool mask)
{
    const fh6::LayerGroup *group = groupForId(project_, groupId);
    if (group == nullptr) {
        return;
    }
    const QVector<QString> children = group->childIds;
    for (const QString &childId : children) {
        if (groupForId(project_, childId) != nullptr) {
            setGroupDescendantMask(childId, mask);
            continue;
        }
        for (fh6::ShapeLayer &layer : project_.layers) {
            if (layer.id == childId) {
                layer.mask = mask;
                break;
            }
        }
    }
}

void EditorState::setGroupDescendantColor(const QString &groupId, const std::array<quint8, 4> &color)
{
    if (groupForId(project_, groupId) == nullptr) {
        return;
    }
    // Collect every descendant leaf id once, then apply in a single pass over the layer array.
    // This keeps a group recolor O(leaves + layers) instead of O(leaves * layers), which the
    // live colour picker hammers per mouse tick.
    QSet<QString> leafIds;
    std::function<void(const QString &)> collect = [&](const QString &id) {
        if (const fh6::LayerGroup *child = groupForId(project_, id)) {
            for (const QString &childId : child->childIds) {
                collect(childId);
            }
        } else {
            leafIds.insert(id);
        }
    };
    collect(groupId);
    for (fh6::ShapeLayer &layer : project_.layers) {
        if (leafIds.contains(layer.id)) {
            layer.color = color;
        }
    }
}

void EditorState::setGroupDescendantOpacity(const QString &groupId, double opacity)
{
    if (groupForId(project_, groupId) == nullptr) {
        return;
    }
    const quint8 alpha = static_cast<quint8>(std::clamp(static_cast<int>(std::round(opacity * 255.0)), 0, 255));
    QSet<QString> leafIds;
    std::function<void(const QString &)> collect = [&](const QString &id) {
        if (const fh6::LayerGroup *child = groupForId(project_, id)) {
            for (const QString &childId : child->childIds) {
                collect(childId);
            }
        } else {
            leafIds.insert(id);
        }
    };
    collect(groupId);
    for (fh6::ShapeLayer &layer : project_.layers) {
        if (leafIds.contains(layer.id)) {
            layer.color[3] = alpha;
        }
    }
}

void EditorState::noteProjectGeometryChanged(bool refreshPreviews)
{
    Q_EMIT projectGeometryChanged(refreshPreviews);
}

void EditorState::noteCanvasRepaint()
{
    Q_EMIT canvasRepaintRequested();
}

void EditorState::noteProjectStructureChanged()
{
    invalidateProjectIndexCache();
    selectedLayerIds_ = existingLayerIds(selectedLayerIds_);
    selectedGuideLayerIds_ = existingGuideLayerIds(selectedGuideLayerIds_);
    Q_EMIT projectStructureChanged();
}

void EditorState::noteClipboardChanged()
{
    Q_EMIT clipboardChanged();
}

void EditorState::setActiveSectionId(const QString &sectionGroupId)
{
    if (activeSectionId_ == sectionGroupId) {
        return;
    }
    activeSectionId_ = sectionGroupId;
    Q_EMIT activeSectionChanged(sectionGroupId);
}

void EditorState::setToolName(const QString &name)
{
    Q_EMIT toolNameChanged(name);
}

QSet<QString> EditorState::existingLayerIds(const QSet<QString> &ids) const
{
    QSet<QString> existing;
    if (!hasProject_) {
        return existing;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    for (const QString &id : ids) {
        if (cache.layerIndexById.contains(id)) {
            existing.insert(id);
        }
    }
    return existing;
}

QSet<QString> EditorState::existingGuideLayerIds(const QSet<QString> &ids) const
{
    QSet<QString> existing;
    if (!hasProject_) {
        return existing;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    for (const QString &id : ids) {
        if (cache.guideIndexById.contains(id)) {
            existing.insert(id);
        }
    }
    return existing;
}

QString EditorState::parentGroupForEntry(const QString &entryId) const
{
    if (!hasProject_) {
        return {};
    }
    return projectIndexCache().parentByChild.value(entryId);
}

QString EditorState::topmostGroupForEntry(const QString &entryId) const
{
    if (!hasProject_) {
        return {};
    }
    const ProjectIndexCache &cache = projectIndexCache();
    QString topmost;
    for (QString parent = cache.parentByChild.value(entryId); !parent.isEmpty(); parent = cache.parentByChild.value(parent)) {
        topmost = parent;
    }
    return topmost;
}

QVector<QString> *EditorState::childListForParent(const QString &parentGroupId)
{
    if (parentGroupId.isEmpty()) {
        return &project_.rootChildIds;
    }
    fh6::LayerGroup *group = groupForId(project_, parentGroupId);
    return group == nullptr ? nullptr : &group->childIds;
}

const QVector<QString> *EditorState::childListForParent(const QString &parentGroupId) const
{
    if (parentGroupId.isEmpty()) {
        return &project_.rootChildIds;
    }
    const fh6::LayerGroup *group = groupForId(project_, parentGroupId);
    return group == nullptr ? nullptr : &group->childIds;
}

QString EditorState::uniqueLayerId() const
{
    return detail::uniqueEntryId(project_.layers, QStringLiteral("layer"));
}

QString EditorState::uniqueGuideLayerId() const
{
    return detail::uniqueEntryId(project_.guideLayers, QStringLiteral("guide"));
}

QString EditorState::uniqueGroupId() const
{
    return detail::uniqueEntryId(project_.groups, QStringLiteral("group"));
}

} // namespace gui
