#include "editor_state.h"

#include "matrix_math.h"

#include <algorithm>

namespace gui {
namespace {

std::unique_ptr<fh6::scene::Layer> takeNode(EditorState &state, const QString &id)
{
    fh6::scene::Group *parent = state.groupForId(state.parentGroupForEntry(id));
    if (parent == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < static_cast<int>(parent->children.size()); ++i) {
        if (parent->children[i]->id == id) {
            return parent->takeAt(i);
        }
    }
    return nullptr;
}

void collectLeafIds(const fh6::scene::Layer &node, QSet<QString> &out)
{
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        out.insert(node.id);
        return;
    }
    if (node.kind() == fh6::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
            collectLeafIds(*child, out);
        }
    }
}

void flattenLeaves(std::unique_ptr<fh6::scene::Layer> node, const fh6::Matrix3 &ancestorFrame,
                   std::vector<std::unique_ptr<fh6::scene::Layer>> &out)
{
    if (!node) {
        return;
    }
    const fh6::Matrix3 frame = fh6::detail::multiply(ancestorFrame, node->transform.matrix());
    if (node->kind() != fh6::scene::LayerKind::Group) {
        node->transform = fh6::decomposeTransform2D(frame);
        out.push_back(std::move(node));
        return;
    }
    auto *group = static_cast<fh6::scene::Group *>(node.get());
    while (!group->children.empty()) {
        flattenLeaves(group->takeAt(0), frame, out);
    }
}

template <typename Cache>
bool isContiguous(const QVector<QString> &entries, const Cache &cache)
{
    for (int i = 1; i < entries.size(); ++i) {
        if (cache.orderByChild.value(entries[i], -1) != cache.orderByChild.value(entries[i - 1], -1) + 1) {
            return false;
        }
    }
    return true;
}

} // namespace

void EditorState::groupEntries(const QVector<QString> &entryIds)
{
    if (!hasProject_) {
        return;
    }
    QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.isEmpty()) {
        return;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    QString parentId = cache.parentByChild.value(entries.front());
    for (const QString &entryId : entries) {
        if (cache.parentByChild.value(entryId) != parentId) {
            return;
        }
    }
    std::sort(entries.begin(), entries.end(), [&](const QString &a, const QString &b) {
        return cache.orderByChild.value(a, 0) < cache.orderByChild.value(b, 0);
    });
    if (!isContiguous(entries, cache)) {
        return;
    }
    fh6::scene::Group *parent = groupForId(parentId);
    if (parent == nullptr) {
        return;
    }
    const int insertAt = cache.orderByChild.value(entries.front(), 0);
    auto group = std::make_unique<fh6::scene::Group>();
    group->id = uniqueGroupId();
    group->name = QStringLiteral("Group");

    QSet<QString> selectedLeaves;
    for (int i = entries.size() - 1; i >= 0; --i) {
        if (std::unique_ptr<fh6::scene::Layer> node = takeNode(*this, entries[i])) {
            collectLeafIds(*node, selectedLeaves);
            group->insert(0, std::move(node));
        }
    }
    parent->insert(insertAt, std::move(group));
    selectedLayerIds_ = selectedLeaves;
    invalidateProjectIndexCache();
}

void EditorState::ungroupEntries(const QVector<QString> &entryIds, bool flatten)
{
    if (!hasProject_) {
        return;
    }
    const QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.isEmpty()) {
        return;
    }
    QSet<QString> selectedLeaves;
    for (const QString &entryId : entries) {
        fh6::scene::Group *parent = groupForId(parentGroupForEntry(entryId));
        fh6::scene::Group *group = groupForId(entryId);
        if (parent == nullptr || group == nullptr || group->isLiverySection) {
            continue;
        }
        const int row = projectIndexCache().orderByChild.value(entryId, -1);
        std::unique_ptr<fh6::scene::Layer> removed = takeNode(*this, entryId);
        if (!removed) {
            continue;
        }
        std::vector<std::unique_ptr<fh6::scene::Layer>> replacement;
        if (flatten) {
            flattenLeaves(std::move(removed), fh6::Matrix3{}, replacement);
        } else {
            const fh6::Matrix3 groupFrame = removed->transform.matrix();
            auto *removedGroup = static_cast<fh6::scene::Group *>(removed.get());
            while (!removedGroup->children.empty()) {
                std::unique_ptr<fh6::scene::Layer> child = removedGroup->takeAt(0);
                child->transform = fh6::decomposeTransform2D(
                    fh6::detail::multiply(groupFrame, child->transform.matrix()));
                replacement.push_back(std::move(child));
            }
        }
        int insertAt = row;
        for (auto &node : replacement) {
            collectLeafIds(*node, selectedLeaves);
            parent->insert(insertAt++, std::move(node));
        }
    }
    selectedLayerIds_ = selectedLeaves;
    invalidateProjectIndexCache();
}

bool EditorState::reorderEntries(const QString &parentGroupId, const QVector<QString> &entryIds, int insertRow)
{
    if (!hasProject_ || entryIds.isEmpty()) {
        return false;
    }
    fh6::scene::Group *parent = groupForId(parentGroupId);
    if (parent == nullptr) {
        return false;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    QVector<QString> movedIds;
    QSet<QString> movedSet;
    for (const QString &entryId : entryIds) {
        if (entryId.isEmpty() || movedSet.contains(entryId)) {
            continue;
        }
        if (cache.parentByChild.value(entryId) != parentGroupId || entryHasLockedLayer(entryId)) {
            return false;
        }
        movedIds.push_back(entryId);
        movedSet.insert(entryId);
    }
    if (movedIds.isEmpty()) {
        return false;
    }
    QVector<QString> before;
    for (const auto &child : parent->children) {
        before.push_back(child->id);
    }
    insertRow = std::clamp(insertRow, 0, static_cast<int>(parent->children.size()));
    std::vector<std::unique_ptr<fh6::scene::Layer>> moving;
    for (int i = static_cast<int>(parent->children.size()) - 1; i >= 0; --i) {
        if (movedSet.contains(parent->children[i]->id)) {
            if (i < insertRow) {
                --insertRow;
            }
            moving.insert(moving.begin(), parent->takeAt(i));
        }
    }
    insertRow = std::clamp(insertRow, 0, static_cast<int>(parent->children.size()));
    for (auto &node : moving) {
        parent->insert(insertRow++, std::move(node));
    }
    QVector<QString> after;
    for (const auto &child : parent->children) {
        after.push_back(child->id);
    }
    if (before == after) {
        return false;
    }
    invalidateProjectIndexCache();
    return true;
}

bool EditorState::reorderGuideLayers(const QVector<QString> &guideIds, int insertRow)
{
    if (!hasProject_ || project_.isLivery || guideIds.isEmpty() || !project_.root) {
        return false;
    }
    QSet<QString> movedSet;
    for (const QString &id : guideIds) {
        if (fh6::scene::GuideLayer *guide = projectIndexCache().guides.value(id, nullptr)) {
            if (guide->locked || parentGroupForEntry(id).isEmpty() == false) {
                return false;
            }
            movedSet.insert(id);
        }
    }
    if (movedSet.isEmpty()) {
        return false;
    }
    auto *root = project_.root.get();
    QVector<QString> before;
    for (const auto &child : root->children) {
        if (child->kind() == fh6::scene::LayerKind::Guide) {
            before.push_back(child->id);
        }
    }
    std::vector<std::unique_ptr<fh6::scene::Layer>> guides;
    int guideOrdinal = 0;
    int rootInsert = static_cast<int>(root->children.size());
    for (int i = static_cast<int>(root->children.size()) - 1; i >= 0; --i) {
        if (root->children[i]->kind() != fh6::scene::LayerKind::Guide) {
            continue;
        }
        if (guideOrdinal == insertRow) {
            rootInsert = i + 1;
        }
        if (movedSet.contains(root->children[i]->id)) {
            guides.insert(guides.begin(), root->takeAt(i));
            if (i < rootInsert) {
                --rootInsert;
            }
        }
        ++guideOrdinal;
    }
    rootInsert = std::clamp(rootInsert, 0, static_cast<int>(root->children.size()));
    for (auto &guide : guides) {
        root->insert(rootInsert++, std::move(guide));
    }
    selectedGuideLayerIds_ = movedSet;
    invalidateProjectIndexCache();
    QVector<QString> after;
    for (const auto &child : root->children) {
        if (child->kind() == fh6::scene::LayerKind::Guide) {
            after.push_back(child->id);
        }
    }
    return before != after;
}

void EditorState::pruneEmptyGroups()
{
    if (!project_.root) {
        return;
    }
    std::function<void(fh6::scene::Group &)> prune = [&](fh6::scene::Group &group) {
        for (int i = static_cast<int>(group.children.size()) - 1; i >= 0; --i) {
            if (group.children[i]->kind() != fh6::scene::LayerKind::Group) {
                continue;
            }
            auto &childGroup = static_cast<fh6::scene::Group &>(*group.children[i]);
            prune(childGroup);
            if (childGroup.children.empty() && !childGroup.isLiverySection) {
                group.takeAt(i);
            }
        }
    };
    prune(*project_.root);
    invalidateProjectIndexCache();
}

} // namespace gui
