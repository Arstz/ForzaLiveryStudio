// Grouping and ordering operations split out of editor_state.cpp: group and
// ungroup (nested or flattened), sibling reordering for tree entries and guide
// layers, and empty-group pruning.

#include "editor_state.h"

#include "editor_state_internal.h"

#include <QSet>

#include <algorithm>

namespace gui {

using namespace detail;

namespace {

void collectGroupIdsForEntry(const QString &entryId, const ProjectEntryMaps &maps, QSet<QString> *groupIds)
{
    const fh6::LayerGroup *group = maps.groups.value(entryId, nullptr);
    if (group == nullptr || groupIds == nullptr || groupIds->contains(group->id)) {
        return;
    }
    groupIds->insert(group->id);
    for (const QString &childId : group->childIds) {
        collectGroupIdsForEntry(childId, maps, groupIds);
    }
}

} // namespace

void EditorState::groupEntries(const QVector<QString> &entryIds)
{
    invalidateProjectIndexCache();
    if (project_.rootChildIds.isEmpty()) {
        for (const fh6::ShapeLayer &layer : project_.layers) {
            project_.rootChildIds.push_back(layer.id);
        }
    }
    QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.isEmpty()) {
        return;
    }
    const ProjectEntryMaps maps = buildEntryMaps(project_);

    QString parentId = maps.parentByChild.value(entries.front());
    int insertAt = maps.orderByChild.value(entries.front(), 0);
    for (const QString &entryId : entries) {
        const QString candidateParent = maps.parentByChild.value(entryId);
        const int order = maps.orderByChild.value(entryId, insertAt);
        if (candidateParent != parentId || order < 0) {
            return;
        }
        insertAt = std::min(insertAt, order);
    }
    std::sort(entries.begin(), entries.end(), [&](const QString &a, const QString &b) {
        return maps.orderByChild.value(a, 0) < maps.orderByChild.value(b, 0);
    });
    for (int i = 1; i < entries.size(); ++i) {
        if (maps.orderByChild.value(entries[i], -1) != maps.orderByChild.value(entries[i - 1], -1) + 1) {
            return;
        }
    }

    QSet<QString> groupedIds;
    QSet<QString> selectedLeaves;
    for (const QString &entryId : entries) {
        groupedIds.insert(entryId);
        for (const QString &leafId : leafIdsForEntry(entryId, maps)) {
            selectedLeaves.insert(leafId);
        }
    }

    auto removeGroupedIds = [&](QVector<QString> &children) {
        children.erase(std::remove_if(children.begin(), children.end(), [&](const QString &id) {
                           return groupedIds.contains(id);
                       }),
                       children.end());
    };
    removeGroupedIds(project_.rootChildIds);
    for (fh6::LayerGroup &group : project_.groups) {
        removeGroupedIds(group.childIds);
    }

    fh6::LayerGroup group;
    group.id = uniqueGroupId();
    group.name = QStringLiteral("Group");
    group.childIds = entries;
    project_.groups.push_back(group);

    QVector<QString> *targetChildren = childListForParent(parentId);
    if (targetChildren == nullptr) {
        targetChildren = &project_.rootChildIds;
        insertAt = targetChildren->size();
    }
    insertAt = std::clamp(insertAt, 0, static_cast<int>(targetChildren->size()));
    targetChildren->insert(insertAt, group.id);
    selectedLayerIds_ = selectedLeaves;
}

void EditorState::ungroupEntries(const QVector<QString> &entryIds, bool flatten)
{
    invalidateProjectIndexCache();
    const QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.isEmpty()) {
        return;
    }
    const ProjectEntryMaps maps = buildEntryMaps(project_);
    QSet<QString> selected;
    for (const QString &entryId : entries) {
        selected.insert(entryId);
    }

    QSet<QString> removedGroupIds;
    QSet<QString> selectedLeaves;
    auto replaceSelected = [&](QVector<QString> &children) {
        QVector<QString> replacement;
        replacement.reserve(children.size());
        for (const QString &childId : children) {
            if (!selected.contains(childId)) {
                replacement.push_back(childId);
                continue;
            }

            const fh6::LayerGroup *group = maps.groups.value(childId, nullptr);
            if (group == nullptr) {
                replacement.push_back(childId);
                selectedLeaves.insert(childId);
                continue;
            }
            if (group->isLiverySection) {
                replacement.push_back(childId);
                for (const QString &leafId : leafIdsForEntry(childId, maps)) {
                    selectedLeaves.insert(leafId);
                }
                continue;
            }

            const QVector<QString> ungrouped = flatten ? leafIdsForEntry(childId, maps) : group->childIds;
            replacement += ungrouped;
            for (const QString &entryId : ungrouped) {
                for (const QString &leafId : leafIdsForEntry(entryId, maps)) {
                    selectedLeaves.insert(leafId);
                }
            }
            if (flatten) {
                collectGroupIdsForEntry(childId, maps, &removedGroupIds);
            } else {
                removedGroupIds.insert(childId);
            }
        }
        children = replacement;
    };

    replaceSelected(project_.rootChildIds);
    for (fh6::LayerGroup &group : project_.groups) {
        if (!removedGroupIds.contains(group.id)) {
            replaceSelected(group.childIds);
        }
    }

    project_.groups.erase(std::remove_if(project_.groups.begin(), project_.groups.end(), [&](const fh6::LayerGroup &group) {
                              return removedGroupIds.contains(group.id);
                          }),
                          project_.groups.end());
    selectedLayerIds_ = selectedLeaves;
}

bool EditorState::reorderEntries(const QString &parentGroupId, const QVector<QString> &entryIds, int insertRow)
{
    invalidateProjectIndexCache();
    if (!hasProject_ || project_.isLivery || entryIds.isEmpty()) {
        return false;
    }
    const ProjectEntryMaps maps = buildEntryMaps(project_);
    QVector<QString> movedIds;
    QSet<QString> movedSet;
    QSet<QString> selectedLeaves = selectedLayerIds_;
    for (const QString &entryId : entryIds) {
        if (entryId.isEmpty() || movedSet.contains(entryId)) {
            continue;
        }
        if (maps.parentByChild.value(entryId) != parentGroupId || subtreeHasLockedLayer(entryId, maps)) {
            return false;
        }
        movedIds.push_back(entryId);
        movedSet.insert(entryId);
        for (const QString &leafId : leafIdsForEntry(entryId, maps)) {
            selectedLeaves.insert(leafId);
        }
    }
    if (movedIds.isEmpty()) {
        return false;
    }

    if (project_.rootChildIds.isEmpty() && parentGroupId.isEmpty()) {
        QVector<QString> order;
        order.reserve(project_.layers.size());
        for (const fh6::ShapeLayer &layer : project_.layers) {
            order.push_back(layer.id);
        }
        for (const QString &id : movedIds) {
            if (!order.contains(id)) {
                return false;
            }
        }
        insertRow = std::clamp(insertRow, 0, static_cast<int>(order.size()));
        for (int i = order.size() - 1; i >= 0; --i) {
            if (movedSet.contains(order[i])) {
                if (i < insertRow) {
                    --insertRow;
                }
                order.removeAt(i);
            }
        }
        if (insertRow < 0) {
            insertRow = 0;
        }
        for (int i = 0; i < movedIds.size(); ++i) {
            order.insert(insertRow + i, movedIds[i]);
        }
        QVector<QString> before;
        before.reserve(project_.layers.size());
        for (const fh6::ShapeLayer &layer : project_.layers) {
            before.push_back(layer.id);
        }
        if (order == before) {
            return false;
        }
        applyLayerOrder(&project_, order);
        selectedLayerIds_ = selectedLeaves;
        return true;
    }

    QVector<QString> *children = childListForParent(parentGroupId);
    if (children == nullptr) {
        return false;
    }
    for (const QString &id : movedIds) {
        if (!children->contains(id)) {
            return false;
        }
    }

    const QVector<QString> beforeChildren = *children;
    insertRow = std::clamp(insertRow, 0, static_cast<int>(children->size()));
    for (int i = children->size() - 1; i >= 0; --i) {
        if (movedSet.contains(children->at(i))) {
            if (i < insertRow) {
                --insertRow;
            }
            children->removeAt(i);
        }
    }
    insertRow = std::clamp(insertRow, 0, static_cast<int>(children->size()));
    for (int i = 0; i < movedIds.size(); ++i) {
        children->insert(insertRow + i, movedIds[i]);
    }
    if (*children == beforeChildren) {
        return false;
    }

    const ProjectEntryMaps afterMaps = buildEntryMaps(project_);
    QVector<QString> layerOrder;
    layerOrder.reserve(project_.layers.size());
    for (const QString &rootId : project_.rootChildIds) {
        collectLayerOrder(rootId, afterMaps, &layerOrder);
    }
    applyLayerOrder(&project_, layerOrder);
    selectedLayerIds_ = selectedLeaves;
    return true;
}

bool EditorState::reorderGuideLayers(const QVector<QString> &guideIds, int insertRow)
{
    invalidateProjectIndexCache();
    if (!hasProject_ || project_.isLivery || guideIds.isEmpty()) {
        return false;
    }
    QSet<QString> movedSet;
    QVector<QString> movedIds;
    for (const QString &id : guideIds) {
        if (!id.isEmpty() && !movedSet.contains(id)) {
            movedIds.push_back(id);
            movedSet.insert(id);
        }
    }
    if (movedIds.isEmpty()) {
        return false;
    }
    for (const fh6::GuideLayer &guide : project_.guideLayers) {
        if (movedSet.contains(guide.id) && guide.locked) {
            return false;
        }
    }

    QVector<fh6::GuideLayer> moving;
    QVector<fh6::GuideLayer> remaining;
    moving.reserve(movedIds.size());
    remaining.reserve(project_.guideLayers.size());
    for (const fh6::GuideLayer &guide : project_.guideLayers) {
        if (movedSet.contains(guide.id)) {
            moving.push_back(guide);
        } else {
            remaining.push_back(guide);
        }
    }
    if (moving.size() != movedIds.size()) {
        return false;
    }
    insertRow = std::clamp(insertRow, 0, static_cast<int>(project_.guideLayers.size()));
    for (int i = 0; i < project_.guideLayers.size(); ++i) {
        if (movedSet.contains(project_.guideLayers[i].id) && i < insertRow) {
            --insertRow;
        }
    }
    insertRow = std::clamp(insertRow, 0, static_cast<int>(remaining.size()));
    for (int i = 0; i < moving.size(); ++i) {
        remaining.insert(insertRow + i, moving[i]);
    }
    bool unchanged = remaining.size() == project_.guideLayers.size();
    for (int i = 0; unchanged && i < remaining.size(); ++i) {
        unchanged = guideLayersEqual(remaining[i], project_.guideLayers[i]);
    }
    if (unchanged) {
        return false;
    }
    project_.guideLayers = remaining;
    selectedGuideLayerIds_ = movedSet;
    return true;
}

void EditorState::pruneEmptyGroups()
{
    invalidateProjectIndexCache();
    QSet<QString> validLayerIds;
    for (const fh6::ShapeLayer &layer : project_.layers) {
        validLayerIds.insert(layer.id);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        QSet<QString> groupIds;
        for (const fh6::LayerGroup &group : project_.groups) {
            groupIds.insert(group.id);
        }
        QVector<fh6::LayerGroup> groups;
        groups.reserve(project_.groups.size());
        QSet<QString> keptGroups;
        for (fh6::LayerGroup group : project_.groups) {
            QVector<QString> children;
            for (const QString &childId : group.childIds) {
                if (validLayerIds.contains(childId) || groupIds.contains(childId)) {
                    children.push_back(childId);
                } else {
                    changed = true;
                }
            }
            group.childIds = children;
            if (!group.childIds.isEmpty()) {
                keptGroups.insert(group.id);
                groups.push_back(group);
            } else if (group.isLiverySection) {
                keptGroups.insert(group.id);
                groups.push_back(group);
            } else {
                changed = true;
            }
        }
        project_.groups = groups;
        QVector<QString> rootIds;
        for (const QString &id : project_.rootChildIds) {
            if (validLayerIds.contains(id) || keptGroups.contains(id)) {
                rootIds.push_back(id);
            } else {
                changed = true;
            }
        }
        project_.rootChildIds = rootIds;
    }
}

} // namespace gui
