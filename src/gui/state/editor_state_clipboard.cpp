// Clipboard, duplicate, delete, and insertion operations split out of
// editor_state.cpp: entry clipboard construction, copy/paste with id remapping
// and renaming, in-place duplication, entry removal, and insert-above-selection
// placement (including draw-order resync).

#include "editor_state.h"

#include "editor_state_internal.h"
#include "perf_utils.h"

#include <algorithm>
#include <functional>

namespace gui {

using namespace detail;

bool EditorState::buildEntryClipboard(const QVector<QString> &entries, ProjectClipboard &out) const
{
    ScopedPerf perf("EditorState::buildEntryClipboard");
    if (entries.isEmpty() || !hasProject_) {
        return false;
    }
    const ProjectEntryMaps maps = buildEntryMaps(project_);
    QVector<QString> orderedEntries = entries;
    QHash<QString, int> docOrder;
    int counter = 0;
    std::function<void(const QString &)> dfs = [&](const QString &id) {
        if (docOrder.contains(id)) {
            return;
        }
        docOrder.insert(id, counter++);
        if (const fh6::LayerGroup *group = maps.groups.value(id, nullptr)) {
            for (const QString &childId : group->childIds) {
                dfs(childId);
            }
        }
    };
    if (!project_.rootChildIds.isEmpty()) {
        for (const QString &rootId : project_.rootChildIds) {
            dfs(rootId);
        }
    } else {
        for (const fh6::ShapeLayer &layer : project_.layers) {
            docOrder.insert(layer.id, counter++);
        }
    }
    for (const fh6::GuideLayer &guide : project_.guideLayers) {
        docOrder.insert(guide.id, counter++);
    }
    QHash<QString, int> inputOrder;
    for (int i = 0; i < orderedEntries.size(); ++i) {
        inputOrder.insert(orderedEntries[i], i);
    }
    std::stable_sort(orderedEntries.begin(), orderedEntries.end(), [&](const QString &a, const QString &b) {
        return docOrder.value(a, inputOrder.value(a, 0)) < docOrder.value(b, inputOrder.value(b, 0));
    });

    for (const QString &entryId : orderedEntries) {
        for (const fh6::GuideLayer &guide : project_.guideLayers) {
            if (guide.id == entryId && guide.locked) {
                return false;
            }
        }
        if (subtreeHasLockedLayer(entryId, maps)) {
            return false;
        }
    }

    ProjectClipboard copied;
    copied.rootIds = orderedEntries;
    QSet<QString> copiedLayerIds;
    QSet<QString> copiedGuideLayerIds;
    QSet<QString> copiedGroupIds;
    std::function<void(const QString &)> collect = [&](const QString &entryId) {
        if (const fh6::ShapeLayer *layer = maps.layers.value(entryId, nullptr)) {
            if (!copiedLayerIds.contains(layer->id)) {
                copied.layers.push_back(*layer);
                copiedLayerIds.insert(layer->id);
            }
            return;
        }
        for (const fh6::GuideLayer &guide : project_.guideLayers) {
            if (guide.id == entryId && !copiedGuideLayerIds.contains(guide.id) && !guide.locked) {
                copied.guideLayers.push_back(guide);
                copiedGuideLayerIds.insert(guide.id);
                return;
            }
        }
        const fh6::LayerGroup *group = maps.groups.value(entryId, nullptr);
        if (group == nullptr || copiedGroupIds.contains(group->id)) {
            return;
        }
        copied.groups.push_back(*group);
        copiedGroupIds.insert(group->id);
        for (const QString &childId : group->childIds) {
            collect(childId);
        }
    };
    for (const QString &entryId : orderedEntries) {
        collect(entryId);
    }
    out = copied;
    return true;
}

bool EditorState::copyEntriesToClipboard(const QVector<QString> &entries)
{
    ProjectClipboard copied;
    if (!buildEntryClipboard(entries, copied)) {
        return false;
    }
    clipboard_ = copied;
    Q_EMIT clipboardChanged();
    return true;
}

bool EditorState::duplicateEntriesInPlace(const QVector<QString> &entryIds,
                                          QSet<QString> *newLayerSelection,
                                          QSet<QString> *newGuideLayerSelection)
{
    ScopedPerf perf("EditorState::duplicateEntriesInPlace");
    if (!hasProject_) {
        return false;
    }
    // Make root order explicit so insertion and draw-order sync below have a tree to work
    // with even for freshly imported flat projects (mirrors groupEntries()).
    if (project_.rootChildIds.isEmpty()) {
        for (const fh6::ShapeLayer &layer : project_.layers) {
            project_.rootChildIds.push_back(layer.id);
        }
    }

    QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.isEmpty()) {
        return false;
    }

    const ProjectEntryMaps maps = buildEntryMaps(project_);

    // Promote any group whose entire leaf set is selected to the group entry. A group
    // selection arrives from the canvas as its individual leaf layers, so without this a
    // duplicate would scatter the leaves instead of cloning the group as one unit.
    {
        QSet<QString> selectedLeaves;
        for (const QString &id : entries) {
            if (maps.groups.contains(id)) {
                for (const QString &leaf : leafIdsForEntry(id, maps)) {
                    selectedLeaves.insert(leaf);
                }
            } else {
                selectedLeaves.insert(id);  // layer or guide
            }
        }
        QSet<QString> coveredGroups;
        for (const fh6::LayerGroup &group : project_.groups) {
            const QVector<QString> leaves = leafIdsForEntry(group.id, maps);
            if (leaves.isEmpty()) {
                continue;
            }
            const bool allSelected = std::all_of(leaves.begin(), leaves.end(), [&](const QString &leaf) {
                return selectedLeaves.contains(leaf);
            });
            if (allSelected) {
                coveredGroups.insert(group.id);
            }
        }
        auto ancestorCovered = [&](const QString &id) {
            for (QString p = maps.parentByChild.value(id); !p.isEmpty(); p = maps.parentByChild.value(p)) {
                if (coveredGroups.contains(p)) {
                    return true;
                }
            }
            return false;
        };
        QVector<QString> promoted;
        QSet<QString> promotedSet;
        auto addPromoted = [&](const QString &id) {
            if (!promotedSet.contains(id)) {
                promoted.push_back(id);
                promotedSet.insert(id);
            }
        };
        // Maximal cover: a covered group whose parent is not also covered, plus any selected
        // leaf/guide that is not inside a covered group.
        for (const QString &groupId : coveredGroups) {
            if (!ancestorCovered(groupId)) {
                addPromoted(groupId);
            }
        }
        for (const QString &id : entries) {
            const bool isGroup = maps.groups.contains(id);
            if (!isGroup && !ancestorCovered(id)) {
                addPromoted(id);
            }
        }
        entries = promoted;
    }
    if (entries.isEmpty()) {
        return false;
    }

    // Order the duplicated roots by document order (depth-first over the tree, guides last) so
    // the clones keep the same relative arrangement as the originals regardless of the order
    // the selection was supplied in.
    QHash<QString, int> docOrder;
    int counter = 0;
    std::function<void(const QString &)> dfs = [&](const QString &id) {
        docOrder.insert(id, counter++);
        if (const fh6::LayerGroup *group = maps.groups.value(id, nullptr)) {
            for (const QString &childId : group->childIds) {
                dfs(childId);
            }
        }
    };
    for (const QString &rootId : project_.rootChildIds) {
        dfs(rootId);
    }
    for (const fh6::GuideLayer &guide : project_.guideLayers) {
        docOrder.insert(guide.id, counter++);
    }
    std::sort(entries.begin(), entries.end(), [&](const QString &a, const QString &b) {
        return docOrder.value(a, 0) < docOrder.value(b, 0);
    });

    ProjectClipboard copied;
    if (!buildEntryClipboard(entries, copied)) {
        return false;
    }

    // Anchor the insertion just above (foreground side of) the "most parent" selected entry:
    // the shallowest one, breaking ties toward the most-foreground sibling. In the
    // foreground-on-top layer list this places the whole duplicated block visually above it.
    auto depthOf = [&](const QString &id) {
        int depth = 0;
        for (QString p = maps.parentByChild.value(id); !p.isEmpty(); p = maps.parentByChild.value(p)) {
            ++depth;
        }
        return depth;
    };
    QString anchor;
    int anchorDepth = 0;
    int anchorOrder = -1;
    for (const QString &id : entries) {
        if (maps.orderByChild.value(id, -1) < 0) {
            continue;  // not a positioned tree entry (e.g. a guide)
        }
        const int depth = depthOf(id);
        const int order = maps.orderByChild.value(id);
        if (anchor.isEmpty() || depth < anchorDepth || (depth == anchorDepth && order > anchorOrder)) {
            anchor = id;
            anchorDepth = depth;
            anchorOrder = order;
        }
    }

    QString parentId;
    int insertAt = 0;
    bool haveTarget = false;
    if (!anchor.isEmpty()) {
        parentId = maps.parentByChild.value(anchor);
        insertAt = anchorOrder + 1;  // foreground side of the anchor
        haveTarget = true;
    }

    int guideInsertAt = -1;
    for (const QString &id : entries) {
        for (int i = 0; i < project_.guideLayers.size(); ++i) {
            if (project_.guideLayers[i].id == id) {
                const int candidate = i + 1;  // foreground side of the source guide
                guideInsertAt = (guideInsertAt < 0) ? candidate : std::min(guideInsertAt, candidate);
            }
        }
    }

    // Keep the original IDs' names (no " (Copy)" suffix) and no position offset so a duplicate
    // lands exactly on top of its source.
    insertClipboardAt(copied, parentId, insertAt, haveTarget, guideInsertAt,
                      newLayerSelection, newGuideLayerSelection, false);

    // Sync project_.layers (the draw order) to the flattened tree so the clones render exactly
    // where they now sit in the layer list rather than at the end of the array.
    const ProjectEntryMaps afterMaps = buildEntryMaps(project_);
    QVector<QString> layerOrder;
    layerOrder.reserve(project_.layers.size());
    for (const QString &rootId : project_.rootChildIds) {
        collectLayerOrder(rootId, afterMaps, &layerOrder);
    }
    applyLayerOrder(&project_, layerOrder);
    return true;
}

void EditorState::removeEntries(const QVector<QString> &entryIds)
{
    invalidateProjectIndexCache();
    const ProjectEntryMaps maps = buildEntryMaps(project_);
    QSet<QString> removed;
    for (const QString &entryId : entryIds) {
        removed.insert(entryId);
    }
    QSet<QString> removedLayerIds;
    QSet<QString> removedGroupIds;
    std::function<void(const QString &)> collect = [&](const QString &entryId) {
        if (maps.layers.contains(entryId)) {
            removedLayerIds.insert(entryId);
            return;
        }
        const fh6::LayerGroup *group = maps.groups.value(entryId, nullptr);
        if (group == nullptr || removedGroupIds.contains(entryId)) {
            return;
        }
        if (group->isLiverySection) {
            return;
        }
        removedGroupIds.insert(entryId);
        for (const QString &childId : group->childIds) {
            collect(childId);
        }
    };
    for (const QString &entryId : removed) {
        collect(entryId);
    }

    project_.layers.erase(std::remove_if(project_.layers.begin(), project_.layers.end(), [&](const fh6::ShapeLayer &layer) {
                              return removedLayerIds.contains(layer.id);
                          }),
                          project_.layers.end());
    project_.groups.erase(std::remove_if(project_.groups.begin(), project_.groups.end(), [&](const fh6::LayerGroup &group) {
                              return removedGroupIds.contains(group.id);
                          }),
                          project_.groups.end());

    auto pruneChildList = [&](QVector<QString> &children) {
        children.erase(std::remove_if(children.begin(), children.end(), [&](const QString &id) {
                           return removedLayerIds.contains(id) || removedGroupIds.contains(id);
                       }),
                       children.end());
    };
    pruneChildList(project_.rootChildIds);
    for (fh6::LayerGroup &group : project_.groups) {
        pruneChildList(group.childIds);
    }
    pruneEmptyGroups();
}

void EditorState::insertClipboardAt(const ProjectClipboard &clipboard,
                                    const QString &parentId, int insertAt, bool haveTarget, int guideInsertAt,
                                    QSet<QString> *newLayerSelection, QSet<QString> *newGuideLayerSelection,
                                    bool renameCopies)
{
    invalidateProjectIndexCache();
    QHash<QString, QString> idMap;
    QSet<QString> usedLayerIds;
    QSet<QString> usedGuideLayerIds;
    QSet<QString> usedGroupIds;
    for (const fh6::ShapeLayer &layer : project_.layers) {
        usedLayerIds.insert(layer.id);
    }
    for (const fh6::GuideLayer &guide : project_.guideLayers) {
        usedGuideLayerIds.insert(guide.id);
    }
    for (const fh6::LayerGroup &group : project_.groups) {
        usedGroupIds.insert(group.id);
    }
    int nextLayerIndex = project_.layers.size() + 1;
    for (const fh6::ShapeLayer &layer : clipboard.layers) {
        QString id;
        do {
            id = QStringLiteral("layer_copy_%1").arg(nextLayerIndex++);
        } while (usedLayerIds.contains(id));
        usedLayerIds.insert(id);
        idMap.insert(layer.id, id);
    }
    int nextGuideIndex = project_.guideLayers.size() + 1;
    for (const fh6::GuideLayer &guide : clipboard.guideLayers) {
        QString id;
        do {
            id = QStringLiteral("guide_copy_%1").arg(nextGuideIndex++);
        } while (usedGuideLayerIds.contains(id));
        usedGuideLayerIds.insert(id);
        idMap.insert(guide.id, id);
    }
    int nextGroupIndex = project_.groups.size() + 1;
    for (const fh6::LayerGroup &group : clipboard.groups) {
        QString id;
        do {
            id = QStringLiteral("group_copy_%1").arg(nextGroupIndex++);
        } while (usedGroupIds.contains(id));
        usedGroupIds.insert(id);
        idMap.insert(group.id, id);
    }

    for (fh6::ShapeLayer layer : clipboard.layers) {
        layer.id = idMap.value(layer.id);
        if (renameCopies) {
            layer.name = copyName(layer.name);
        }
        project_.layers.push_back(layer);
        if (newLayerSelection != nullptr) {
            newLayerSelection->insert(layer.id);
        }
    }
    if (!clipboard.guideLayers.isEmpty()) {
        int insertGuideAt = (guideInsertAt < 0)
            ? project_.guideLayers.size()
            : std::clamp(guideInsertAt, 0, static_cast<int>(project_.guideLayers.size()));
        for (fh6::GuideLayer guide : clipboard.guideLayers) {
            guide.id = idMap.value(guide.id);
            if (renameCopies) {
                guide.name = copyName(guide.name);
            }
            if (newGuideLayerSelection != nullptr) {
                newGuideLayerSelection->insert(guide.id);
            }
            project_.guideLayers.insert(insertGuideAt++, guide);
        }
    }
    for (fh6::LayerGroup group : clipboard.groups) {
        group.id = idMap.value(group.id);
        if (renameCopies) {
            group.name = copyName(group.name);
        }
        for (QString &childId : group.childIds) {
            childId = idMap.value(childId, childId);
        }
        group.sourceParentId.clear();
        group.sourcePreviousSiblingId.clear();
        group.sourceChildIds.clear();
        project_.groups.push_back(group);
    }

    if (!clipboard.layers.isEmpty() || !clipboard.groups.isEmpty()) {
        QSet<QString> originalGuideIds;
        for (const fh6::GuideLayer &guide : clipboard.guideLayers) {
            originalGuideIds.insert(guide.id);
        }
        QVector<QString> *targetChildren = haveTarget ? childListForParent(parentId) : &project_.rootChildIds;
        if (targetChildren == nullptr) {
            targetChildren = &project_.rootChildIds;
            insertAt = targetChildren->size();
        }
        insertAt = std::clamp(insertAt, 0, static_cast<int>(targetChildren->size()));
        int inserted = 0;
        for (const QString &rootId : clipboard.rootIds) {
            if (originalGuideIds.contains(rootId)) {
                continue;
            }
            targetChildren->insert(insertAt + inserted, idMap.value(rootId, rootId));
            ++inserted;
        }
    }
}

void EditorState::insertClipboardAboveSelection(const ProjectClipboard &clipboard,
                                                const QVector<QString> &selectedEntries,
                                                QSet<QString> *newLayerSelection,
                                                QSet<QString> *newGuideLayerSelection,
                                                bool renameCopies)
{
    const ProjectEntryMaps maps = buildEntryMaps(project_);
    const QVector<QString> normalizedSelection = normalizeEntrySelection(selectedEntries);
    QString parentId;
    int insertAt = 0;
    bool haveTarget = false;
    for (const QString &entryId : normalizedSelection) {
        const QString candidateParent = maps.parentByChild.value(entryId);
        const int order = maps.orderByChild.value(entryId, -1);
        if (order < 0) {
            continue;
        }
        if (!haveTarget || candidateParent != parentId || order + 1 > insertAt) {
            parentId = candidateParent;
            insertAt = order + 1;
            haveTarget = true;
        }
    }
    int guideInsertAt = -1;
    for (const QString &entryId : normalizedSelection) {
        for (int i = 0; i < project_.guideLayers.size(); ++i) {
            if (project_.guideLayers[i].id == entryId) {
                const int candidate = i + 1;
                guideInsertAt = (guideInsertAt < 0) ? candidate : std::max(guideInsertAt, candidate);
            }
        }
    }
    insertClipboardAt(clipboard, parentId, insertAt, haveTarget, guideInsertAt,
                      newLayerSelection, newGuideLayerSelection, renameCopies);
    const ProjectEntryMaps afterMaps = buildEntryMaps(project_);
    QVector<QString> layerOrder;
    layerOrder.reserve(project_.layers.size());
    for (const QString &rootId : project_.rootChildIds) {
        collectLayerOrder(rootId, afterMaps, &layerOrder);
    }
    applyLayerOrder(&project_, layerOrder);
    invalidateProjectIndexCache();
}

void EditorState::insertLayerAboveSelection(const fh6::ShapeLayer &layer, const QVector<QString> &selectedEntries)
{
    invalidateProjectIndexCache();
    if (project_.rootChildIds.isEmpty()) {
        for (const fh6::ShapeLayer &existing : project_.layers) {
            project_.rootChildIds.push_back(existing.id);
        }
    }
    const ProjectEntryMaps maps = buildEntryMaps(project_);
    const QVector<QString> normalizedSelection = normalizeEntrySelection(selectedEntries);
    QString parentId;
    int insertAt = 0;
    bool haveTarget = false;
    for (const QString &entryId : normalizedSelection) {
        const QString candidateParent = maps.parentByChild.value(entryId);
        const int order = maps.orderByChild.value(entryId, -1);
        if (order < 0) {
            continue;
        }
        if (!haveTarget || candidateParent != parentId || order + 1 > insertAt) {
            parentId = candidateParent;
            insertAt = order + 1;
            haveTarget = true;
        }
    }

    project_.layers.push_back(layer);
    QVector<QString> *targetChildren = haveTarget ? childListForParent(parentId) : &project_.rootChildIds;
    if (targetChildren == nullptr) {
        targetChildren = &project_.rootChildIds;
        insertAt = targetChildren->size();
    } else if (!haveTarget) {
        insertAt = targetChildren->size();
    }
    insertAt = std::clamp(insertAt, 0, static_cast<int>(targetChildren->size()));
    targetChildren->insert(insertAt, layer.id);
    const ProjectEntryMaps afterMaps = buildEntryMaps(project_);
    QVector<QString> layerOrder;
    layerOrder.reserve(project_.layers.size());
    for (const QString &rootId : project_.rootChildIds) {
        collectLayerOrder(rootId, afterMaps, &layerOrder);
    }
    applyLayerOrder(&project_, layerOrder);
    invalidateProjectIndexCache();
}

QString EditorState::copyName(const QString &name) const
{
    const QString suffix = QStringLiteral(" (Copy)");
    return name.endsWith(suffix) ? name : name + suffix;
}

const ProjectClipboard *EditorState::clipboard() const
{
    return clipboard_.has_value() ? &*clipboard_ : nullptr;
}

} // namespace gui
