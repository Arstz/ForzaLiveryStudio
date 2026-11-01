#include "editor_state.h"

#include "matrix_math.h"
#include "contour_mesh.h"
#include "contour_mesh_diagnostics.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

void collectLeafIds(const fls::scene::Layer &node, QSet<QString> &out) {
    if (node.kind() == fls::scene::LayerKind::Shape
        || fls::scene::isContourGeometry(node.kind())) {
        out.insert(node.id);
        return;
    }
    if (node.kind() == fls::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fls::scene::Group &>(node).children) {
            collectLeafIds(*child, out);
        }
    }
}

void flattenLeaves(std::unique_ptr<fls::scene::Layer> node, const fls::Matrix3 &ancestorFrame,
                   std::vector<std::unique_ptr<fls::scene::Layer>> &out) {
    if (!node) {
        return;
    }
    const fls::Matrix3 frame = fls::detail::multiply(ancestorFrame, node->transform.matrix());
    if (node->kind() != fls::scene::LayerKind::Group) {
        node->transform = fls::decomposeTransform2D(frame);
        out.push_back(std::move(node));
        return;
    }
    auto *group = static_cast<fls::scene::Group *>(node.get());
    while (!group->children.empty()) {
        flattenLeaves(group->takeAt(0), frame, out);
    }
}

template <typename Cache>
bool isContiguous(const QVector<QString> &entries, const Cache &cache) {
    for (int i = 1; i < entries.size(); ++i) {
        if (cache.orderByChild.value(entries[i], -1) != cache.orderByChild.value(entries[i - 1], -1) + 1) {
            return false;
        }
    }
    return true;
}

QVector<QString> childIds(const fls::scene::Group &group,
                          std::optional<fls::scene::LayerKind> kind = std::nullopt) {
    QVector<QString> ids;
    for (const auto &child : group.children) {
        if (!kind.has_value() || child->kind() == *kind) {
            ids.push_back(child->id);
        }
    }

    return ids;
}

} // namespace

bool EditorState::groupEntries(const QVector<QString> &entryIds, QString *error) {
    const auto reject = [&](const QString &reason) {
        if (error != nullptr) *error = reason;

        return false;
    };
    if (!hasProject_) {
        return reject(QStringLiteral("Open a project first"));
    }
    QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.size() < 2) {
        return reject(QStringLiteral("Select at least two adjacent layers to group"));
    }
    const ProjectIndexCache &cache = projectIndexCache();
    QString parentId = cache.parentByChild.value(entries.front());
    for (const QString &entryId : entries) {
        if (cache.parentByChild.value(entryId) != parentId) {
            return reject(QStringLiteral("Select adjacent sibling layers to group"));
        }
    }
    std::sort(entries.begin(), entries.end(), [&](const QString &a, const QString &b) {
        return cache.orderByChild.value(a, 0) < cache.orderByChild.value(b, 0);
    });
    if (!isContiguous(entries, cache)) {
        return reject(QStringLiteral("Select adjacent sibling layers to group"));
    }
    fls::scene::Group *parent = groupForId(parentId);
    if (parent == nullptr) {
        return reject(QStringLiteral("Invalid group parent"));
    }
    const int insertAt = cache.orderByChild.value(entries.front(), 0);
    bool hasContour = false;
    bool hasOther = false;
    fls::scene::Mesh *existingMesh = nullptr;
    for (const auto &id : entries) {
        auto *node = sceneNode(id);
        if (node == nullptr || entryHasLockedLayer(id)) return reject(QStringLiteral("Invalid or locked grouping target"));
        hasContour |= fls::scene::containsContourGeometry(*node);
        hasOther |= !fls::scene::isContourGeometry(node->kind());
        if (node->kind() == fls::scene::LayerKind::Mesh && existingMesh == nullptr) {
            existingMesh = static_cast<fls::scene::Mesh *>(node);
        }
    }
    if (hasContour && hasOther) {
        return reject(QStringLiteral("Contours and meshes can only be grouped with contours or meshes"));
    }
    if (hasContour) {
        auto mesh = std::make_unique<fls::scene::Mesh>();
        const QTransform frame = existingMesh != nullptr ? sceneLocalTransform(*existingMesh) : QTransform();
        bool invertible = false;
        const QTransform inverse = frame.inverted(&invertible);
        if (!invertible) return reject(QStringLiteral("The mesh has a zero scale"));
        mesh->id = existingMesh != nullptr ? existingMesh->id : uniqueLayerId();
        mesh->name = existingMesh != nullptr ? existingMesh->name : QStringLiteral("Mesh");
        if (existingMesh != nullptr) mesh->transform = existingMesh->transform;
        if (existingMesh != nullptr) appendMeshGeometry(mesh->data, *existingMesh, QTransform());
        for (const auto &id : entries) {
            const auto *node = sceneNode(id);
            if (node == existingMesh) continue;
            appendMeshGeometry(mesh->data, *node, sceneLocalTransform(*node) * inverse);
        }
        const auto input = mesh->data;
        QJsonObject diagnostics;
        const bool connected = connectMeshContours(mesh->data, error, &diagnostics);
        writeMeshReconstructionDiagnostics(input, connected ? &mesh->data : nullptr, diagnostics, entries);
        if (!connected) return false;
        const QString meshId = mesh->id;
        for (int index = entries.size() - 1; index >= 0; --index) takeEntry(entries[index]);
        parent->insert(insertAt, std::move(mesh));
        selectedLayerIds_ = {meshId};
        selectedGuideLayerIds_.clear();
        selectedEntryIds_ = {meshId};
        invalidateProjectIndexCache();

        return true;
    }
    auto group = std::make_unique<fls::scene::Group>();
    group->id = uniqueGroupId();
    group->name = QStringLiteral("Group");

    QSet<QString> selectedLeaves;
    for (int i = entries.size() - 1; i >= 0; --i) {
        if (std::unique_ptr<fls::scene::Layer> node = takeEntry(entries[i])) {
            collectLeafIds(*node, selectedLeaves);
            group->insert(0, std::move(node));
        }
    }
    parent->insert(insertAt, std::move(group));
    selectedLayerIds_ = selectedLeaves;
    selectedEntryIds_.clear();
    invalidateProjectIndexCache();

    return true;
}

void EditorState::ungroupEntries(const QVector<QString> &entryIds, bool flatten) {
    if (!hasProject_) {
        return;
    }
    const QVector<QString> entries = normalizeEntrySelection(entryIds);
    if (entries.isEmpty()) {
        return;
    }
    QSet<QString> selectedLeaves;
    for (const QString &entryId : entries) {
        fls::scene::Group *parent = groupForId(parentGroupForEntry(entryId));
        const auto *mesh = dynamic_cast<const fls::scene::Mesh *>(sceneNode(entryId));
        if (parent != nullptr && mesh != nullptr) {
            const int row = projectIndexCache().orderByChild.value(entryId, -1);
            const auto removed = takeEntry(entryId);
            for (int index = 0; index < mesh->data.contours.size(); ++index) {
                const auto &contour = mesh->data.contours[index];
                auto figure = std::make_unique<fls::scene::ContourFigure>();
                invalidateProjectIndexCache();
                figure->id = uniqueLayerId();
                figure->name = contour.name;
                figure->transform = mesh->transform;
                figure->visible = mesh->visible && contour.visible;
                figure->opacity = mesh->opacity * contour.opacity;
                figure->data = mesh->data.contourData(index);
                selectedLeaves.insert(figure->id);
                parent->insert(row + index, std::move(figure));
            }
            invalidateProjectIndexCache();
            continue;
        }
        fls::scene::Group *group = groupForId(entryId);
        if (parent == nullptr || group == nullptr || group->isLiverySection) {
            continue;
        }
        const int row = projectIndexCache().orderByChild.value(entryId, -1);
        std::unique_ptr<fls::scene::Layer> removed = takeEntry(entryId);
        if (!removed) {
            continue;
        }
        std::vector<std::unique_ptr<fls::scene::Layer>> replacement;
        if (flatten) {
            flattenLeaves(std::move(removed), fls::Matrix3{}, replacement);
        } else {
            const fls::Matrix3 groupFrame = removed->transform.matrix();
            auto *removedGroup = static_cast<fls::scene::Group *>(removed.get());
            while (!removedGroup->children.empty()) {
                std::unique_ptr<fls::scene::Layer> child = removedGroup->takeAt(0);
                child->transform = fls::decomposeTransform2D(
                    fls::detail::multiply(groupFrame, child->transform.matrix()));
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

bool EditorState::reorderEntries(const QString &parentGroupId, const QVector<QString> &entryIds, int insertRow) {
    if (!hasProject_ || entryIds.isEmpty()) {
        return false;
    }
    fls::scene::Group *parent = groupForId(parentGroupId);
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
    const QVector<QString> before = childIds(*parent);
    insertRow = std::clamp(insertRow, 0, static_cast<int>(parent->children.size()));
    std::vector<std::unique_ptr<fls::scene::Layer>> moving;
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
    const QVector<QString> after = childIds(*parent);
    if (before == after) {
        return false;
    }
    invalidateProjectIndexCache();
    return true;
}

bool EditorState::reorderGuideLayers(const QVector<QString> &guideIds, int insertRow) {
    if (!hasProject_ || project_.isLivery || guideIds.isEmpty() || !project_.root) {
        return false;
    }
    QSet<QString> movedSet;
    for (const QString &id : guideIds) {
        if (fls::scene::GuideLayer *guide = projectIndexCache().guides.value(id, nullptr)) {
            if (guide->locked || !parentGroupForEntry(id).isEmpty()) {
                return false;
            }
            movedSet.insert(id);
        }
    }
    if (movedSet.isEmpty()) {
        return false;
    }
    auto *root = project_.root.get();
    const QVector<QString> before = childIds(*root, fls::scene::LayerKind::Guide);
    std::vector<std::unique_ptr<fls::scene::Layer>> guides;
    int guideOrdinal = 0;
    int rootInsert = static_cast<int>(root->children.size());
    for (int i = static_cast<int>(root->children.size()) - 1; i >= 0; --i) {
        if (root->children[i]->kind() != fls::scene::LayerKind::Guide) {
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
    return before != childIds(*root, fls::scene::LayerKind::Guide);
}

QHash<QString, QString> EditorState::pruneRedundantGroups() {
    QHash<QString, QString> replacements;

    if (!project_.root) {
        return replacements;
    }
    auto prune = [&](auto &self, fls::scene::Group &group) -> void {
        for (int i = static_cast<int>(group.children.size()) - 1; i >= 0; --i) {
            if (group.children[i]->kind() != fls::scene::LayerKind::Group) {
                continue;
            }
            auto &childGroup = static_cast<fls::scene::Group &>(*group.children[i]);
            self(self, childGroup);
            if (childGroup.isLiverySection || childGroup.children.size() > 1) {
                continue;
            }
            const QString removedId = childGroup.id;
            if (childGroup.children.empty()) {
                group.takeAt(i);
                replacements.insert(removedId, QString());
                continue;
            }
            auto child = childGroup.takeAt(0);
            if (!sceneLocalTransform(childGroup).isIdentity()) {
                const fls::Matrix3 frame = fls::detail::multiply(childGroup.transform.matrix(), child->transform.matrix());
                child->transform = fls::decomposeTransform2D(frame);
                if (frame.m[0][0] == 0.0 && frame.m[1][0] == 0.0) {
                    child->rotation = std::atan2(-frame.m[0][1], frame.m[1][1]) * 180.0 / std::acos(-1.0);
                }
            }
            child->visible = child->visible && childGroup.visible;
            child->locked = child->locked || childGroup.locked;
            constexpr int kMaskFlag = 64;
            if ((childGroup.flags & kMaskFlag) != 0) {
                if (auto *shape = dynamic_cast<fls::scene::Shape *>(child.get())) {
                    shape->mask = true;
                } else if (auto *nested = dynamic_cast<fls::scene::Group *>(child.get())) {
                    nested->flags |= kMaskFlag;
                }
            }
            replacements.insert(removedId, child->id);
            group.takeAt(i);
            group.insert(i, std::move(child));
        }
    };
    prune(prune, *project_.root);
    QVector<QString> selected;
    for (const QString &id : selectedEntryIds_) {
        const QString replacement = replacements.value(id, id);
        if (!replacement.isEmpty() && !selected.contains(replacement)) {
            selected.push_back(replacement);
        }
    }
    selectedEntryIds_ = selected;
    invalidateProjectIndexCache();

    return replacements;
}

std::unique_ptr<fls::scene::Layer> EditorState::takeEntry(const QString &entryId) {
    fls::scene::Group *parent = groupForId(parentGroupForEntry(entryId));
    if (parent == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < static_cast<int>(parent->children.size()); ++i) {
        if (parent->children[i]->id == entryId) {
            return parent->takeAt(i);
        }
    }

    return nullptr;
}

} // namespace gui
