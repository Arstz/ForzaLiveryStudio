#include "editor_state.h"

#include "matrix_math.h"
#include "scene_codec.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace gui {
namespace {

fh6::Matrix3 fromQTransform(const QTransform &t) {
    fh6::Matrix3 m;
    m.m[0][0] = t.m11();
    m.m[0][1] = t.m21();
    m.m[0][2] = t.dx();
    m.m[1][0] = t.m12();
    m.m[1][1] = t.m22();
    m.m[1][2] = t.dy();
    m.m[2][0] = 0.0;
    m.m[2][1] = 0.0;
    m.m[2][2] = 1.0;
    return m;
}

QTransform frameOf(const fh6::scene::Layer &node) {
    return sceneLocalTransform(node);
}

void storeFrame(fh6::scene::Layer &node, const QTransform &frame) {
    node.transform = fh6::decomposeTransform2D(fromQTransform(frame));
}

template <typename Fn>
void walkGroup(fh6::scene::Group &group, Fn fn) {
    for (const auto &child : group.children) {
        fn(*child);
        if (child->kind() == fh6::scene::LayerKind::Group) {
            walkGroup(static_cast<fh6::scene::Group &>(*child), fn);
        }
    }
}

template <typename Fn>
void walkShapes(fh6::scene::Group &group, Fn fn) {
    walkGroup(group, [&](fh6::scene::Layer &node) {
        if (node.kind() == fh6::scene::LayerKind::Shape) {
            fn(static_cast<fh6::scene::Shape &>(node));
        }
    });
}

template <typename LayerType>
QSet<QString> existingIds(const QSet<QString> &ids, const QHash<QString, LayerType *> &nodes) {
    QSet<QString> existing;
    for (const QString &id : ids) {
        if (nodes.contains(id)) {
            existing.insert(id);
        }
    }

    return existing;
}

std::vector<std::unique_ptr<fh6::scene::Layer>> cloneNodes(
    const std::vector<std::unique_ptr<fh6::scene::Layer>> &nodes) {
    std::vector<std::unique_ptr<fh6::scene::Layer>> copies;
    copies.reserve(nodes.size());
    for (const auto &node : nodes) {
        copies.push_back(node ? node->clone() : nullptr);
    }

    return copies;
}

void collectGuideIds(const fh6::scene::Layer &node, QSet<QString> &out) {
    if (node.kind() == fh6::scene::LayerKind::Guide) {
        out.insert(node.id);
        return;
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return;
    }
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        collectGuideIds(*child, out);
    }
}

} // namespace

ProjectClipboard::ProjectClipboard(const ProjectClipboard &other)
    : rootIds(other.rootIds),
      nodes(cloneNodes(other.nodes)) {
}

ProjectClipboard &ProjectClipboard::operator=(const ProjectClipboard &other) {
    if (this == &other) {
        return *this;
    }
    rootIds = other.rootIds;
    nodes = cloneNodes(other.nodes);

    return *this;
}

EditorState::EditorState(QObject *parent)
    : QObject(parent) {
}

void EditorState::invalidateProjectIndexCache() const {
    indexCache_.reset();
    invalidateSceneTree();
}

void EditorState::invalidateSceneTree() const {
    sceneNodeById_.clear();
    invalidateRenderCache();
}

void EditorState::invalidateRenderCache() const {
    renderCacheDirty_ = true;
    renderEntries_.clear();
    renderEntryByNodeId_.clear();
}

void EditorState::ensureSceneTree() const {
    if (!sceneNodeById_.isEmpty() || !hasProject_ || !project_.root) {
        return;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    sceneNodeById_ = cache.nodes;
}

const fh6::scene::Group *EditorState::sceneRoot() const {
    if (!hasProject_) {
        return nullptr;
    }
    ensureSceneTree();
    return project_.root.get();
}

fh6::scene::Layer *EditorState::sceneNode(const QString &id) const {
    if (!hasProject_) {
        return nullptr;
    }
    ensureSceneTree();
    return sceneNodeById_.value(id, nullptr);
}

void EditorState::ensureRenderCache() const {
    if (!renderCacheDirty_) {
        return;
    }
    renderEntries_.clear();
    renderEntryByNodeId_.clear();
    if (!hasProject_ || !project_.root) {
        renderCacheDirty_ = false;
        return;
    }

    int drawOrder = 0;
    std::function<void(const fh6::scene::Layer &, const QTransform &, QVector<QString>, QString)> walk =
        [&](const fh6::scene::Layer &node, const QTransform &parentWorld, QVector<QString> ancestors, QString sectionId) {
            const QTransform world = sceneLocalTransform(node) * parentWorld;
            if (node.kind() == fh6::scene::LayerKind::Group) {
                const auto &group = static_cast<const fh6::scene::Group &>(node);
                if (group.isLiverySection) {
                    sectionId = group.id;
                }
                QVector<QString> childAncestors = ancestors;
                childAncestors.push_back(group.id);
                for (const auto &child : group.children) {
                    walk(*child, world, childAncestors, sectionId);
                }
                return;
            }

            SceneRenderEntry entry;
            entry.node = &node;
            entry.nodeId = node.id;
            entry.kind = node.kind();
            entry.worldTransform = world;
            entry.ancestorGroupIds = ancestors;
            entry.parentGroupId = parentGroupForEntry(node.id);
            entry.sectionGroupId = sectionId;
            entry.drawOrder = drawOrder++;
            if (node.kind() == fh6::scene::LayerKind::Shape) {
                entry.shape = static_cast<const fh6::scene::Shape *>(&node);
            } else if (node.kind() == fh6::scene::LayerKind::Guide) {
                entry.guide = static_cast<const fh6::scene::GuideLayer *>(&node);
            }
            renderEntryByNodeId_.insert(entry.nodeId, renderEntries_.size());
            renderEntries_.push_back(entry);
        };

    for (const auto &child : project_.root->children) {
        walk(*child, QTransform(), {}, {});
    }
    renderCacheDirty_ = false;
}

void EditorState::updateRenderCacheTransforms(const QVector<QString> &targetIds) const {
    if (renderCacheDirty_ || targetIds.isEmpty()) {
        return;
    }
    ensureSceneTree();
    std::function<void(const fh6::scene::Layer &, const QTransform &)> apply =
        [&](const fh6::scene::Layer &node, const QTransform &parentWorld) {
            const QTransform world = sceneLocalTransform(node) * parentWorld;
            const int idx = renderEntryByNodeId_.value(node.id, -1);
            if (idx >= 0) {
                renderEntries_[idx].worldTransform = world;
            }
            if (node.kind() == fh6::scene::LayerKind::Group) {
                for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                    apply(*child, world);
                }
            }
        };
    for (const QString &id : targetIds) {
        const fh6::scene::Layer *node = sceneNodeById_.value(id, nullptr);
        if (node == nullptr) {
            continue;
        }
        const QTransform parentWorld =
            node->parent() != nullptr ? toQTransform(node->parent()->worldMatrix()) : QTransform();
        apply(*node, parentWorld);
    }
}

const QVector<SceneRenderEntry> &EditorState::renderEntries() const {
    ensureRenderCache();
    return renderEntries_;
}

const EditorState::ProjectIndexCache &EditorState::projectIndexCache() const {
    if (!indexCache_.has_value()) {
        ProjectIndexCache cache;
        if (project_.root) {
            auto *root = const_cast<fh6::scene::Group *>(project_.root.get());
            std::function<void(fh6::scene::Layer &, fh6::scene::Group *, int)> walk =
                [&](fh6::scene::Layer &node, fh6::scene::Group *parent, int row) {
                    cache.nodes.insert(node.id, &node);
                    if (parent != nullptr) {
                        cache.parentGroupByChild.insert(node.id, parent);
                        cache.parentByChild.insert(node.id, parent == project_.root.get() ? QString() : parent->id);
                        cache.orderByChild.insert(node.id, row);
                    }
                    switch (node.kind()) {
                    case fh6::scene::LayerKind::Shape:
                        cache.layers.insert(node.id, static_cast<fh6::scene::Shape *>(&node));
                        break;
                    case fh6::scene::LayerKind::Guide:
                        cache.guides.insert(node.id, static_cast<fh6::scene::GuideLayer *>(&node));
                        break;
                    case fh6::scene::LayerKind::Group: {
                        auto &group = static_cast<fh6::scene::Group &>(node);
                        cache.groups.insert(node.id, &group);
                        for (int i = 0; i < static_cast<int>(group.children.size()); ++i) {
                            walk(*group.children[i], &group, i);
                        }
                        break;
                    }
                    }
                };
            for (int i = 0; i < static_cast<int>(root->children.size()); ++i) {
                walk(*root->children[i], root, i);
            }
        }
        indexCache_ = std::move(cache);
    }
    return *indexCache_;
}

QVector<QString> EditorState::leafLayerIdsForEntryCached(const QString &entryId, const ProjectIndexCache &cache) const {
    const auto cached = cache.leafIdsByEntry.constFind(entryId);
    if (cached != cache.leafIdsByEntry.constEnd()) {
        return cached.value();
    }
    QVector<QString> ids;
    if (cache.layers.contains(entryId)) {
        ids.push_back(entryId);
    } else if (fh6::scene::Group *group = cache.groups.value(entryId, nullptr)) {
        for (const auto &child : group->children) {
            ids += leafLayerIdsForEntryCached(child->id, cache);
        }
    }
    cache.leafIdsByEntry.insert(entryId, ids);
    return ids;
}

bool EditorState::entryHasLockedLayerCached(const QString &entryId, const ProjectIndexCache &cache) const {
    if (fh6::scene::Shape *shape = cache.layers.value(entryId, nullptr)) {
        return shape->locked || lockedLayerIds().contains(entryId);
    }
    if (fh6::scene::Group *group = cache.groups.value(entryId, nullptr)) {
        if (group->locked) {
            return true;
        }
        for (const QString &leaf : leafLayerIdsForEntryCached(entryId, cache)) {
            if (lockedLayerIds().contains(leaf)) {
                return true;
            }
        }
    }
    return false;
}

fh6::Project *EditorState::project() {
    return hasProject_ ? &project_ : nullptr;
}

const fh6::Project *EditorState::project() const {
    return hasProject_ ? &project_ : nullptr;
}

bool EditorState::hasProject() const {
    return hasProject_;
}

void EditorState::setProject(fh6::Project project) {
    project_ = std::move(project);
    fh6::scene::ensureProjectSceneRoot(project_);
    invalidateProjectIndexCache();
    hasProject_ = true;
    activeSectionId_.clear();
    selectedLayerIds_.clear();
    selectedGuideLayerIds_.clear();
    selectedEntryIds_.clear();
    undoStack_.clear();
    redoStack_.clear();
    pendingEdit_.reset();
    setModified(false);
    Q_EMIT projectReset();
    Q_EMIT selectionChanged();
    Q_EMIT historyChanged();
}

bool EditorState::isModified() const {
    return modified_;
}

void EditorState::setModified(bool modified) {
    if (modified_ == modified) {
        return;
    }
    modified_ = modified;
    Q_EMIT modifiedChanged(modified_);
}

bool EditorState::isLayerLocked(const QString &layerId) const {
    return lockedLayerIds().contains(layerId);
}

QSet<QString> EditorState::lockedLayerIds() const {
    if (!hasProject_) {
        return {};
    }
    const ProjectIndexCache &cache = projectIndexCache();
    if (cache.lockedLayerIds.has_value()) {
        return *cache.lockedLayerIds;
    }
    QSet<QString> locked;
    if (project_.root) {
        std::function<void(const fh6::scene::Layer &, bool)> walk = [&](const fh6::scene::Layer &node, bool inherited) {
            const bool effective = inherited || node.locked;
            if (node.kind() == fh6::scene::LayerKind::Shape && effective) {
                locked.insert(node.id);
            } else if (node.kind() == fh6::scene::LayerKind::Group) {
                for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                    walk(*child, effective);
                }
            }
        };
        for (const auto &child : project_.root->children) {
            walk(*child, false);
        }
    }
    cache.lockedLayerIds = locked;
    return locked;
}

QVector<QString> EditorState::leafLayerIdsForEntry(const QString &entryId) const {
    if (!hasProject_) {
        return {};
    }
    return leafLayerIdsForEntryCached(entryId, projectIndexCache());
}

bool EditorState::entryHasLockedLayer(const QString &entryId) const {
    return hasProject_ && entryHasLockedLayerCached(entryId, projectIndexCache());
}

bool EditorState::entryIsGroup(const QString &entryId) const {
    return hasProject_ && projectIndexCache().groups.contains(entryId);
}

bool EditorState::entryIsGuide(const QString &entryId) const {
    return hasProject_ && projectIndexCache().guides.contains(entryId);
}

void EditorState::setGroupAndDescendantLocked(const QString &groupId, bool locked) {
    fh6::scene::Group *group = groupForId(groupId);
    if (group == nullptr) {
        return;
    }
    group->locked = locked;
    walkGroup(*group, [&](fh6::scene::Layer &node) {
        node.locked = locked;
    });
    invalidateProjectIndexCache();
}

void EditorState::setLayerLockScope(const QString &layerId, bool locked) {
    const QString parentGroupId = parentGroupForEntry(layerId);
    if (!parentGroupId.isEmpty()) {
        setGroupAndDescendantLocked(parentGroupId, locked);
        return;
    }
    if (fh6::scene::Shape *shape = projectIndexCache().layers.value(layerId, nullptr)) {
        shape->locked = locked;
        invalidateProjectIndexCache();
    }
}

void EditorState::setLayerVisible(const QString &layerId, bool visible) {
    if (fh6::scene::Shape *shape = projectIndexCache().layers.value(layerId, nullptr)) {
        shape->visible = visible;
    }
}

void EditorState::setLayerMask(const QString &layerId, bool mask) {
    if (fh6::scene::Shape *shape = projectIndexCache().layers.value(layerId, nullptr)) {
        shape->mask = mask;
    }
}

void EditorState::setGuideLayerVisible(const QString &guideId, bool visible) {
    if (fh6::scene::GuideLayer *guide = projectIndexCache().guides.value(guideId, nullptr)) {
        guide->visible = visible;
    }
}

void EditorState::setGuideLayerLocked(const QString &guideId, bool locked) {
    if (fh6::scene::GuideLayer *guide = projectIndexCache().guides.value(guideId, nullptr)) {
        guide->locked = locked;
        invalidateProjectIndexCache();
    }
}

void EditorState::setGroupDescendantVisible(const QString &groupId, bool visible) {
    if (fh6::scene::Group *group = groupForId(groupId)) {
        walkShapes(*group, [&](fh6::scene::Shape &shape) {
            shape.visible = visible;
        });
    }
}

void EditorState::setGroupDescendantMask(const QString &groupId, bool mask) {
    if (fh6::scene::Group *group = groupForId(groupId)) {
        walkShapes(*group, [&](fh6::scene::Shape &shape) {
            shape.mask = mask;
        });
    }
}

void EditorState::setGroupDescendantColor(const QString &groupId, const std::array<quint8, 4> &color) {
    if (fh6::scene::Group *group = groupForId(groupId)) {
        walkShapes(*group, [&](fh6::scene::Shape &shape) {
            if (!shape.raster) {
                shape.color = color;
            }
        });
    }
}

void EditorState::setGroupDescendantOpacity(const QString &groupId, double opacity) {
    const double clamped = std::clamp(opacity, 0.0, 1.0);
    const quint8 alpha = static_cast<quint8>(std::clamp(static_cast<int>(std::round(clamped * 255.0)), 0, 255));
    if (fh6::scene::Group *group = groupForId(groupId)) {
        walkShapes(*group, [&](fh6::scene::Shape &shape) {
            shape.opacity = clamped;
            shape.color[3] = alpha;
        });
    }
}

QTransform EditorState::groupLocalFrame(const QString &groupId) const {
    const fh6::scene::Group *group = groupForId(groupId);
    return group != nullptr ? frameOf(*group) : QTransform();
}

QTransform EditorState::groupParentWorld(const QString &groupId) const {
    const fh6::scene::Group *group = groupForId(groupId);
    if (group == nullptr || group->parent() == nullptr) {
        return {};
    }
    return toQTransform(group->parent()->worldMatrix());
}

void EditorState::transformGroupFrames(const QVector<QString> &groupIds, const QTransform &worldT) {
    if (worldT.isIdentity()) {
        return;
    }
    for (const QString &id : groupIds) {
        fh6::scene::Group *group = groupForId(id);
        if (group == nullptr) {
            continue;
        }
        const QTransform parentWorld = groupParentWorld(id);
        const QTransform inner = parentWorld * worldT * parentWorld.inverted();
        storeFrame(*group, frameOf(*group) * inner);
    }
}

void EditorState::setGroupFramesFromStart(const QHash<QString, QTransform> &startLocalFrames,
                                          const QTransform &worldT) {
    for (auto it = startLocalFrames.constBegin(); it != startLocalFrames.constEnd(); ++it) {
        fh6::scene::Group *group = groupForId(it.key());
        if (group == nullptr) {
            continue;
        }
        const QTransform parentWorld = groupParentWorld(it.key());
        const QTransform inner = parentWorld * worldT * parentWorld.inverted();
        storeFrame(*group, it.value() * inner);
    }
}

QVector<QString> EditorState::fullySelectedTopGroupIds() const {
    if (!hasProject_) {
        return {};
    }
    if (!selectedEntryIds_.isEmpty()) {
        QVector<QString> result;
        QSet<QString> seen;
        const ProjectIndexCache &cache = projectIndexCache();
        for (const QString &id : normalizeEntrySelection(selectedEntryIds_)) {
            if (seen.contains(id)) {
                continue;
            }
            if (cache.groups.contains(id)) {
                result.push_back(id);
                seen.insert(id);
            }
        }
        return result;
    }

    QSet<QString> fully;
    const ProjectIndexCache &cache = projectIndexCache();
    for (fh6::scene::Group *group : cache.groups) {
        const QVector<QString> leaves = leafLayerIdsForEntry(group->id);
        QSet<QString> guides;
        collectGuideIds(*group, guides);
        const bool hasSelectableDescendants = !leaves.isEmpty() || !guides.isEmpty();
        const bool allLeavesSelected = std::all_of(leaves.begin(), leaves.end(), [&](const QString &id) {
            return selectedLayerIds_.contains(id);
        });
        const bool allGuidesSelected = std::all_of(guides.begin(), guides.end(), [&](const QString &id) {
            return selectedGuideLayerIds_.contains(id);
        });
        if (hasSelectableDescendants && allLeavesSelected && allGuidesSelected) {
            fully.insert(group->id);
        }
    }
    QVector<QString> result;
    for (const QString &id : fully) {
        const QString parent = parentGroupForEntry(id);
        if (parent.isEmpty() || !fully.contains(parent)) {
            result.push_back(id);
        }
    }
    return result;
}

QVector<QString> EditorState::selectedTransformTargetIds() const {
    if (!hasProject_) {
        return {};
    }
    QVector<QString> result;
    QSet<QString> seen;
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    const QVector<QString> groupIds = fullySelectedTopGroupIds();
    for (const QString &id : groupIds) {
        if (seen.contains(id)) {
            continue;
        }
        result.push_back(id);
        seen.insert(id);
        for (const QString &leafId : leafLayerIdsForEntry(id)) {
            groupedLayerIds.insert(leafId);
        }
        if (const fh6::scene::Group *group = groupForId(id)) {
            collectGuideIds(*group, groupedGuideIds);
        }
    }
    for (const QString &id : selectedLayerIds_) {
        if (!groupedLayerIds.contains(id) && !seen.contains(id)) {
            result.push_back(id);
            seen.insert(id);
        }
    }
    for (const QString &id : selectedGuideLayerIds_) {
        if (!groupedGuideIds.contains(id) && !seen.contains(id)) {
            result.push_back(id);
            seen.insert(id);
        }
    }
    return result;
}

void EditorState::noteProjectGeometryChanged(bool refreshPreviews, const QVector<QString> &changedNodeIds) {
    invalidateSceneTree();
    Q_EMIT projectGeometryChanged(refreshPreviews, changedNodeIds);
}

void EditorState::noteTransformLiveChanged(const QVector<QString> &targetIds) {
    updateRenderCacheTransforms(targetIds);
    Q_EMIT transformLiveChanged(targetIds);
}

void EditorState::noteCanvasRepaint() {
    Q_EMIT canvasRepaintRequested();
}

void EditorState::noteProjectStructureChanged() {
    invalidateProjectIndexCache();
    selectedLayerIds_ = existingLayerIds(selectedLayerIds_);
    selectedGuideLayerIds_ = existingGuideLayerIds(selectedGuideLayerIds_);
    selectedEntryIds_ = existingEntryIds(selectedEntryIds_);
    Q_EMIT projectStructureChanged();
    Q_EMIT selectionChanged();
}

void EditorState::noteClipboardChanged() {
    Q_EMIT clipboardChanged();
}

void EditorState::setActiveSectionId(const QString &sectionGroupId) {
    if (activeSectionId_ == sectionGroupId) {
        return;
    }
    activeSectionId_ = sectionGroupId;
    Q_EMIT activeSectionChanged(sectionGroupId);
}

void EditorState::setToolName(const QString &name) {
    Q_EMIT toolNameChanged(name);
}

QSet<QString> EditorState::sectionIdsForNodes(const QVector<QString> &nodeIds) const {
    QSet<QString> sections;
    if (!hasProject_ || !project_.isLivery) {
        return sections;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    for (const QString &nodeId : nodeIds) {
        for (QString id = nodeId; !id.isEmpty(); id = cache.parentByChild.value(id)) {
            const fh6::scene::Group *group = cache.groups.value(id, nullptr);
            if (group != nullptr && group->isLiverySection) {
                sections.insert(id);
                break;
            }
        }
    }
    return sections;
}

QSet<QString> EditorState::existingLayerIds(const QSet<QString> &ids) const {
    if (!hasProject_) {
        return {};
    }

    return existingIds(ids, projectIndexCache().layers);
}

QSet<QString> EditorState::existingGuideLayerIds(const QSet<QString> &ids) const {
    if (!hasProject_) {
        return {};
    }

    return existingIds(ids, projectIndexCache().guides);
}

QVector<QString> EditorState::existingEntryIds(const QVector<QString> &entryIds) const {
    if (!hasProject_) {
        return {};
    }
    QVector<QString> existing;
    QSet<QString> seen;
    const ProjectIndexCache &cache = projectIndexCache();
    existing.reserve(entryIds.size());
    for (const QString &id : entryIds) {
        if (!id.isEmpty() && !seen.contains(id) && cache.nodes.contains(id)) {
            existing.push_back(id);
            seen.insert(id);
        }
    }

    return normalizeEntrySelection(existing);
}

QString EditorState::parentGroupForEntry(const QString &entryId) const {
    return hasProject_ ? projectIndexCache().parentByChild.value(entryId) : QString();
}

QString EditorState::topmostGroupForEntry(const QString &entryId) const {
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

fh6::scene::Group *EditorState::groupForId(const QString &groupId) {
    if (!hasProject_) {
        return nullptr;
    }
    if (groupId.isEmpty()) {
        return project_.root.get();
    }
    return projectIndexCache().groups.value(groupId, nullptr);
}

const fh6::scene::Group *EditorState::groupForId(const QString &groupId) const {
    if (!hasProject_) {
        return nullptr;
    }
    if (groupId.isEmpty()) {
        return project_.root.get();
    }
    return projectIndexCache().groups.value(groupId, nullptr);
}

QString EditorState::uniqueLayerId() const {
    return uniqueId(QStringLiteral("layer"), projectIndexCache().layers.size() + 1);
}

QString EditorState::uniqueGuideLayerId() const {
    return uniqueId(QStringLiteral("guide"), projectIndexCache().guides.size() + 1);
}

QString EditorState::uniqueGroupId() const {
    return uniqueId(QStringLiteral("group"), projectIndexCache().groups.size() + 1);
}

QString EditorState::uniqueId(const QString &prefix, int startingIndex) const {
    const auto &cache = projectIndexCache();
    QString id;
    do {
        id = QStringLiteral("%1_%2").arg(prefix).arg(startingIndex++);
    } while (cache.nodes.contains(id));

    return id;
}

} // namespace gui
