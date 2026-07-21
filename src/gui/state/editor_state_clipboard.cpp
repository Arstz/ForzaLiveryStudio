#include "editor_state.h"

#include "perf_utils.h"

#include <algorithm>
#include <functional>

namespace gui {
namespace {

std::unique_ptr<fh6::scene::Layer> takeNode(EditorState &state, const QString &id) {
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

void walkNode(fh6::scene::Layer &node, const std::function<void(fh6::scene::Layer &)> &fn) {
    fn(node);
    if (node.kind() == fh6::scene::LayerKind::Group) {
        for (const auto &child : static_cast<fh6::scene::Group &>(node).children) {
            walkNode(*child, fn);
        }
    }
}

void collectLeafIds(const fh6::scene::Layer &node, QSet<QString> &layers, QSet<QString> &guides) {
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        layers.insert(node.id);
    } else if (node.kind() == fh6::scene::LayerKind::Guide) {
        guides.insert(node.id);
    } else {
        for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
            collectLeafIds(*child, layers, guides);
        }
    }
}

fh6::scene::Group *defaultInsertionTarget(EditorState &state) {
    if (state.project_.isLivery && !state.activeSectionId_.isEmpty()) {
        if (fh6::scene::Group *section = state.groupForId(state.activeSectionId_);
            section != nullptr && section->isLiverySection) {
            return section;
        }
    }
    return state.project_.root.get();
}

} // namespace

bool EditorState::buildEntryClipboard(const QVector<QString> &entries, ProjectClipboard &out) const {
    ScopedPerf perf("EditorState::buildEntryClipboard");
    if (entries.isEmpty() || !hasProject_) {
        return false;
    }
    QVector<QString> ordered = normalizeEntrySelection(entries);
    const ProjectIndexCache &cache = projectIndexCache();
    std::stable_sort(ordered.begin(), ordered.end(), [&](const QString &a, const QString &b) {
        const QString pa = cache.parentByChild.value(a);
        const QString pb = cache.parentByChild.value(b);
        if (pa == pb) {
            return cache.orderByChild.value(a, 0) < cache.orderByChild.value(b, 0);
        }
        return a < b;
    });
    ProjectClipboard copied;
    copied.rootIds = ordered;
    for (const QString &entryId : ordered) {
        fh6::scene::Layer *node = cache.nodes.value(entryId, nullptr);
        if (node == nullptr || entryHasLockedLayer(entryId) || node->locked) {
            return false;
        }
        copied.nodes.push_back(node->clone());
    }
    out = copied;
    return true;
}

bool EditorState::copyEntriesToClipboard(const QVector<QString> &entries) {
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
                                          QSet<QString> *newGuideLayerSelection) {
    ScopedPerf perf("EditorState::duplicateEntriesInPlace");
    if (!hasProject_) {
        return false;
    }
    QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.isEmpty()) {
        return false;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    std::sort(entries.begin(), entries.end(), [&](const QString &a, const QString &b) {
        return cache.orderByChild.value(a, 0) < cache.orderByChild.value(b, 0);
    });
    ProjectClipboard copied;
    if (!buildEntryClipboard(entries, copied)) {
        return false;
    }

    struct InsertionBatch {
        QString parentId;
        int insertAt = 0;
        ProjectClipboard clipboard;
    };
    QVector<InsertionBatch> batches;
    QHash<QString, int> batchByParent;
    for (int i = 0; i < copied.rootIds.size(); ++i) {
        const QString &id = copied.rootIds[i];
        const QString parentId = cache.parentByChild.value(id);
        int batchIndex = batchByParent.value(parentId, -1);
        if (batchIndex < 0) {
            batchIndex = batches.size();
            batchByParent.insert(parentId, batchIndex);
            batches.push_back({parentId, 0, {}});
        }
        InsertionBatch &batch = batches[batchIndex];
        batch.insertAt = std::max(batch.insertAt, cache.orderByChild.value(id, -1) + 1);
        batch.clipboard.rootIds.push_back(id);
        batch.clipboard.nodes.push_back(std::move(copied.nodes[static_cast<size_t>(i)]));
    }
    for (const InsertionBatch &batch : batches) {
        insertClipboardAt(batch.clipboard, batch.parentId, batch.insertAt, true, batch.insertAt,
                          newLayerSelection, newGuideLayerSelection, false, nullptr);
    }
    return true;
}

void EditorState::removeEntries(const QVector<QString> &entryIds) {
    if (!hasProject_) {
        return;
    }
    const QVector<QString> entries = normalizeEntrySelection(entryIds);
    for (const QString &entryId : entries) {
        if (fh6::scene::Group *group = groupForId(entryId); group != nullptr && group->isLiverySection) {
            continue;
        }
        takeNode(*this, entryId);
    }
    pruneEmptyGroups();
    invalidateProjectIndexCache();
}

void EditorState::insertClipboardAt(const ProjectClipboard &clipboard,
                                    const QString &parentId, int insertAt, bool haveTarget, int guideInsertAt,
                                    QSet<QString> *newLayerSelection, QSet<QString> *newGuideLayerSelection,
                                    bool renameCopies, QVector<QString> *newRootEntryIds) {
    if (!hasProject_) {
        return;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    QHash<QString, QString> idMap;
    QSet<QString> used;
    for (auto it = cache.nodes.constBegin(); it != cache.nodes.constEnd(); ++it) {
        used.insert(it.key());
    }
    int nextLayerIndex = cache.layers.size() + 1;
    int nextGuideIndex = cache.guides.size() + 1;
    int nextGroupIndex = cache.groups.size() + 1;

    for (const auto &root : clipboard.nodes) {
        if (!root) {
            continue;
        }
        std::unique_ptr<fh6::scene::Layer> temp = root->clone();
        walkNode(*temp, [&](fh6::scene::Layer &node) {
            QString prefix;
            int *counter = nullptr;
            switch (node.kind()) {
            case fh6::scene::LayerKind::Shape:
                prefix = QStringLiteral("layer_copy");
                counter = &nextLayerIndex;
                break;
            case fh6::scene::LayerKind::Guide:
                prefix = QStringLiteral("guide_copy");
                counter = &nextGuideIndex;
                break;
            case fh6::scene::LayerKind::Group:
                prefix = QStringLiteral("group_copy");
                counter = &nextGroupIndex;
                break;
            }
            QString id;
            do {
                id = QStringLiteral("%1_%2").arg(prefix).arg((*counter)++);
            } while (used.contains(id));
            used.insert(id);
            idMap.insert(node.id, id);
        });
    }

    fh6::scene::Group *target = haveTarget ? groupForId(parentId) : defaultInsertionTarget(*this);
    if (target == nullptr) {
        target = project_.root.get();
        insertAt = static_cast<int>(target->children.size());
    } else if (!haveTarget) {
        insertAt = static_cast<int>(target->children.size());
    }
    insertAt = std::clamp(insertAt, 0, static_cast<int>(target->children.size()));
    int insertGuideAt = guideInsertAt < 0 ? static_cast<int>(project_.root->children.size())
                                          : std::clamp(guideInsertAt, 0, static_cast<int>(project_.root->children.size()));

    for (const auto &root : clipboard.nodes) {
        if (!root) {
            continue;
        }
        std::unique_ptr<fh6::scene::Layer> node = root->clone();
        walkNode(*node, [&](fh6::scene::Layer &entry) {
            entry.id = idMap.value(entry.id, entry.id);
            if (renameCopies) {
                entry.name = copyName(entry.name);
            }
            if (entry.kind() == fh6::scene::LayerKind::Group) {
                auto &group = static_cast<fh6::scene::Group &>(entry);
                group.sourceParentId.clear();
                group.sourcePreviousSiblingId.clear();
                group.sourceChildren.clear();
            }
        });
        if (newRootEntryIds != nullptr) {
            newRootEntryIds->push_back(node->id);
        }
        QSet<QString> insertedLayers;
        QSet<QString> insertedGuides;
        collectLeafIds(*node, insertedLayers, insertedGuides);
        if (newLayerSelection != nullptr) {
            *newLayerSelection += insertedLayers;
        }
        if (newGuideLayerSelection != nullptr) {
            *newGuideLayerSelection += insertedGuides;
        }
        if (node->kind() == fh6::scene::LayerKind::Guide && target != project_.root.get()) {
            project_.root->insert(insertGuideAt++, std::move(node));
        } else {
            target->insert(insertAt++, std::move(node));
        }
    }
    invalidateProjectIndexCache();
}

void EditorState::insertClipboardAboveSelection(const ProjectClipboard &clipboard,
                                                const QVector<QString> &selectedEntries,
                                                QSet<QString> *newLayerSelection,
                                                QSet<QString> *newGuideLayerSelection,
                                                bool renameCopies,
                                                QVector<QString> *newRootEntryIds) {
    const ProjectIndexCache &cache = projectIndexCache();
    const QVector<QString> normalizedSelection = normalizeEntrySelection(selectedEntries);
    QString parentId;
    int insertAt = 0;
    bool haveTarget = false;
    for (const QString &entryId : normalizedSelection) {
        const QString candidateParent = cache.parentByChild.value(entryId);
        const int order = cache.orderByChild.value(entryId, -1);
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
        if (cache.guides.contains(entryId)) {
            guideInsertAt = std::max(guideInsertAt, cache.orderByChild.value(entryId, -1) + 1);
        }
    }
    insertClipboardAt(clipboard, parentId, insertAt, haveTarget, guideInsertAt,
                      newLayerSelection, newGuideLayerSelection, renameCopies, newRootEntryIds);
}

void EditorState::insertLayerAboveSelection(std::unique_ptr<fh6::scene::Layer> layer, const QVector<QString> &selectedEntries) {
    if (!layer || !project_.root) {
        return;
    }
    const ProjectIndexCache &cache = projectIndexCache();
    const QVector<QString> normalizedSelection = normalizeEntrySelection(selectedEntries);
    QString parentId;
    int insertAt = 0;
    bool haveTarget = false;
    for (const QString &entryId : normalizedSelection) {
        const QString candidateParent = cache.parentByChild.value(entryId);
        const int order = cache.orderByChild.value(entryId, -1);
        if (order < 0) {
            continue;
        }
        if (!haveTarget || candidateParent != parentId || order + 1 > insertAt) {
            parentId = candidateParent;
            insertAt = order + 1;
            haveTarget = true;
        }
    }
    fh6::scene::Group *target = haveTarget ? groupForId(parentId) : defaultInsertionTarget(*this);
    if (target == nullptr) {
        target = project_.root.get();
    }
    if (!haveTarget) {
        insertAt = static_cast<int>(target->children.size());
    }
    target->insert(std::clamp(insertAt, 0, static_cast<int>(target->children.size())), std::move(layer));
    invalidateProjectIndexCache();
}

QString EditorState::copyName(const QString &name) const {
    const QString suffix = QStringLiteral(" (Copy)");
    return name.endsWith(suffix) ? name : name + suffix;
}

const ProjectClipboard *EditorState::clipboard() const {
    return clipboard_.has_value() ? &*clipboard_ : nullptr;
}

} // namespace gui
