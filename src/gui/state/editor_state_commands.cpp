#include "editor_state.h"

#include "matrix_math.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

bool reject(QString *error, const QString &reason) {
    if (error != nullptr) {
        *error = reason;
    }

    return false;
}

bool isFlag(const QString &property) {
    return property == QStringLiteral("visible") || property == QStringLiteral("mask")
        || property == QStringLiteral("locked");
}

bool isTransform(const QString &property) {
    return property == QStringLiteral("x") || property == QStringLiteral("y")
        || property == QStringLiteral("scaleX") || property == QStringLiteral("scaleY")
        || property == QStringLiteral("rotation") || property == QStringLiteral("skew");
}

fls::scene::Transform2D fromTransform(const QTransform &transform) {
    fls::Matrix3 matrix;
    matrix.m[0][0] = transform.m11();
    matrix.m[1][0] = transform.m12();
    matrix.m[0][1] = transform.m21();
    matrix.m[1][1] = transform.m22();
    matrix.m[0][2] = transform.dx();
    matrix.m[1][2] = transform.dy();
    auto result = fls::decomposeTransform2D(matrix);
    if (transform.m11() == 0.0 && transform.m12() == 0.0) {
        result.rotation = std::atan2(-transform.m21(), transform.m22()) * 180.0 / std::acos(-1.0);
        result.scaleX = 0.0;
        result.scaleY = std::hypot(transform.m21(), transform.m22());
        result.skew = 0.0;
    }

    return result;
}

void collectNodes(const fls::scene::Layer &node, QVector<QString> &ids) {
    ids.push_back(node.id);
    if (node.kind() == fls::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fls::scene::Group &>(node).children) {
            collectNodes(*child, ids);
        }
    }
}

} // namespace

QVector<QString> EditorState::selectionEntries() const {
    if (!selectedEntryIds_.isEmpty()) {
        return normalizeEntrySelection(selectedEntryIds_);
    }
    QVector<QString> entries = selectedLayerIds_.values();
    entries += selectedGuideLayerIds_.values();

    return normalizeEntrySelection(entries);
}

QVector<QString> EditorState::propertyTargets(const QVector<QString> &entries, const QString &property) const {
    QVector<QString> targets;
    QSet<QString> seen;

    for (const QString &entry : normalizeEntrySelection(entries)) {
        const fls::scene::Layer *node = sceneNode(entry);
        if (node == nullptr) {
            continue;
        }
        QVector<QString> descendants;
        if (property == QStringLiteral("name") || isTransform(property)) {
            descendants.push_back(entry);
        } else {
            collectNodes(*node, descendants);
        }
        for (const QString &id : descendants) {
            const fls::scene::Layer *child = sceneNode(id);
            const auto *shape = dynamic_cast<const fls::scene::Shape *>(child);
            if (seen.contains(id)
                || (property == QStringLiteral("contourData") && !fls::scene::isContourGeometry(child->kind()))
                || (property == QStringLiteral("meshData") && child->kind() != fls::scene::LayerKind::Mesh)
                || ((property == QStringLiteral("mask") || property == QStringLiteral("shapeId")) && shape == nullptr)
                || (property == QStringLiteral("color") && (child->kind() == fls::scene::LayerKind::Group
                    || child->kind() == fls::scene::LayerKind::Guide || (shape != nullptr && shape->isRaster())))
                || (property == QStringLiteral("opacity") && child->kind() == fls::scene::LayerKind::Group)) {
                continue;
            }
            seen.insert(id);
            targets.push_back(id);
        }
    }

    return targets;
}

Qt::CheckState EditorState::propertyCheckState(const QVector<QString> &entries, const QString &property) const {
    bool anyTrue = false;
    bool anyFalse = false;

    for (const QString &id : propertyTargets(entries, property)) {
        const fls::scene::Layer *node = sceneNode(id);
        const bool value = property == QStringLiteral("mask")
            ? static_cast<const fls::scene::Shape *>(node)->mask
            : property == QStringLiteral("locked") ? isLayerLocked(id) : node->visible;
        anyTrue |= value;
        anyFalse |= !value;
    }

    return anyTrue ? (anyFalse ? Qt::PartiallyChecked : Qt::Checked) : Qt::Unchecked;
}

bool EditorState::applyPropertyCommand(const EditorCommand &command, QString *error) {
    const QString &property = command.property;
    const QVector<QString> targets = propertyTargets(command.entries, property);
    const bool flag = isFlag(property);
    const bool numeric = isTransform(property) || property == QStringLiteral("opacity");

    if (!flag && !numeric && property != QStringLiteral("name")
        && property != QStringLiteral("shapeId") && property != QStringLiteral("color")
        && !(property == QStringLiteral("contourData") && command.contourData.has_value())
        && !(property == QStringLiteral("meshData") && command.meshData.has_value())) {
        return reject(error, QStringLiteral("Unknown property"));
    }
    if (targets.isEmpty()) {
        return reject(error, QStringLiteral("Selection has no applicable layers"));
    }
    if (property == QStringLiteral("meshData") && !command.meshData->valid()) {
        return reject(error, QStringLiteral("Mesh edits must retain valid shared topology"));
    }
    for (const QString &id : targets) {
        if (!flag && isLayerLocked(id)) {
            return reject(error, QStringLiteral("Selection contains locked layers"));
        }
        if (numeric && !std::isfinite(command.values.value(id, command.value.toDouble()))) {
            return reject(error, QStringLiteral("Property value must be finite"));
        }
    }
    for (const QString &id : targets) {
        fls::scene::Layer *node = sceneNode(id);
        auto *shape = dynamic_cast<fls::scene::Shape *>(node);
        auto *contour = dynamic_cast<fls::scene::ContourFigure *>(node);
        auto *mesh = dynamic_cast<fls::scene::Mesh *>(node);
        const double number = command.values.value(id, command.value.toDouble());
        const auto adjusted = [&](double current) { return command.relative ? current + number : number; };

        if (property == QStringLiteral("meshData")) {
            mesh->data = *command.meshData;
        } else if (property == QStringLiteral("contourData")) {
            if (mesh != nullptr) {
                if (!mesh->data.replaceContour(command.meshContourIndex, *command.contourData, error)) return false;
            } else {
                contour->data = *command.contourData;
            }
        } else if (property == QStringLiteral("name")) {
            node->name = command.value.toString();
        } else if (property == QStringLiteral("visible")) {
            node->visible = command.value.toBool();
        } else if (property == QStringLiteral("locked")) {
            node->locked = command.value.toBool();
            if (!node->locked) {
                QVector<fls::scene::Layer *> ancestors;
                for (fls::scene::Layer *ancestor = node->parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
                    ancestors.push_front(ancestor);
                }
                for (fls::scene::Layer *ancestor : ancestors) {
                    if (ancestor->locked) {
                        if (ancestor->kind() == fls::scene::LayerKind::Group) {
                            setGroupAndDescendantLocked(ancestor->id, true);
                        }
                        ancestor->locked = false;
                    }
                }
                node->locked = false;
            }
        } else if (property == QStringLiteral("mask")) {
            shape->mask = command.value.toBool();
        } else if (property == QStringLiteral("shapeId")) {
            if (!shape->isRaster()) {
                shape->setVectorShape(static_cast<quint16>(command.value.toUInt()));
            }
        } else if (property == QStringLiteral("color")) {
            const QColor color = command.value.value<QColor>();
            if (shape != nullptr) {
                shape->color = {static_cast<quint8>(color.blue()), static_cast<quint8>(color.green()),
                                static_cast<quint8>(color.red()), static_cast<quint8>(color.alpha())};
                shape->opacity = color.alphaF();
            } else if (contour != nullptr) {
                contour->data.fillColor = color;
            } else if (mesh != nullptr) {
                for (auto &entry : mesh->data.contours) entry.style.fillColor = color;
            }
        } else if (property == QStringLiteral("opacity")) {
            const double current = shape != nullptr ? shape->color[3] / 255.0 : node->opacity;
            node->opacity = std::clamp(adjusted(current), 0.0, 1.0);
            if (shape != nullptr) {
                shape->color[3] = static_cast<quint8>(std::lround(node->opacity * 255.0));
            } else if (contour != nullptr) {
                QColor color = fls::scene::contourFillColor(contour->data);
                color.setAlphaF(node->opacity);
                contour->data.fillColor = color;
            }
        } else if (property == QStringLiteral("x")) {
            node->x = adjusted(node->x);
        } else if (property == QStringLiteral("y")) {
            node->y = adjusted(node->y);
        } else if (property == QStringLiteral("scaleX")) {
            node->scaleX = adjusted(node->scaleX);
        } else if (property == QStringLiteral("scaleY")) {
            node->scaleY = adjusted(node->scaleY);
        } else if (property == QStringLiteral("rotation")) {
            node->rotation = fls::normalizeRotation(adjusted(node->rotation));
        } else if (property == QStringLiteral("skew")) {
            node->skew = adjusted(node->skew);
        }
    }
    invalidateProjectIndexCache();

    return true;
}

bool EditorState::canMoveEntries(const QVector<QString> &entries, const QString &parentId, QString *error) const {
    const fls::scene::Group *parent = groupForId(parentId);
    const QVector<QString> roots = normalizeEntrySelection(entries);
    bool invertible = false;

    if (parent == nullptr || roots.isEmpty()) {
        return reject(error, QStringLiteral("Invalid move destination"));
    }
    const QTransform inverse = sceneWorldTransform(*parent).inverted(&invertible);
    if (isLayerLocked(parentId)) {
        return reject(error, QStringLiteral("Destination group is locked"));
    }
    for (const QString &id : roots) {
        const fls::scene::Layer *node = sceneNode(id);
        const auto *group = dynamic_cast<const fls::scene::Group *>(node);
        if (node == nullptr || (group != nullptr && group->isLiverySection) || entryHasLockedLayer(id)) {
            return reject(error, QStringLiteral("Selection contains a locked or structural layer"));
        }
        if (fls::scene::isContourGeometry(node->kind()) && parent != project_.root.get() && !parent->isLiverySection) {
            return reject(error, QStringLiteral("Contours and meshes cannot be placed inside ordinary groups"));
        }
        for (const fls::scene::Layer *ancestor = parent; ancestor != nullptr; ancestor = ancestor->parent()) {
            if (ancestor == node) {
                return reject(error, QStringLiteral("A group cannot be moved into itself or its descendants"));
            }
        }
        if (node->parent() != parent) {
            if (!invertible) {
                return reject(error, QStringLiteral("The destination group has a zero scale"));
            }
            const QTransform local = sceneWorldTransform(*node) * inverse;
            const QTransform rebuilt = toQTransform(fromTransform(local).matrix());
            constexpr double kTransformTolerance = 1e-8;
            for (const QPointF &point : {QPointF(), QPointF(1, 0), QPointF(0, 1)}) {
                if (QLineF(local.map(point), rebuilt.map(point)).length()
                    > kTransformTolerance * std::max(1.0, QLineF(QPointF(), local.map(point)).length())) {
                    return reject(error, QStringLiteral("The parent transform cannot be represented without changing the shape"));
                }
            }
        }
    }

    return true;
}

bool EditorState::moveEntries(const QVector<QString> &entries, const QString &parentId, int row) {
    fls::scene::Group *parent = groupForId(parentId);
    QVector<QString> roots = normalizeEntrySelection(entries);
    QVector<QString> sceneOrder;
    QHash<QString, fls::scene::Transform2D> frames;
    std::vector<std::unique_ptr<fls::scene::Layer>> moving;
    const QTransform inverse = sceneWorldTransform(*parent).inverted();

    collectNodes(*project_.root, sceneOrder);
    std::sort(roots.begin(), roots.end(), [&](const QString &left, const QString &right) {
        return sceneOrder.indexOf(left) < sceneOrder.indexOf(right);
    });
    row = row < 0 ? static_cast<int>(parent->children.size())
                  : std::clamp(row, 0, static_cast<int>(parent->children.size()));
    int removedBefore = 0;
    for (const QString &id : roots) {
        const fls::scene::Layer *node = sceneNode(id);
        if (node->parent() == parent) {
            const int sourceRow = projectIndexCache().orderByChild.value(id);
            removedBefore += sourceRow < row;
            frames.insert(id, node->transform);
        } else {
            frames.insert(id, fromTransform(sceneWorldTransform(*node) * inverse));
        }
    }
    for (const QString &id : roots) {
        moving.push_back(takeEntry(id));
    }
    row -= removedBefore;
    for (auto &node : moving) {
        node->transform = frames.value(node->id);
        parent->insert(row++, std::move(node));
    }
    const auto replacements = pruneRedundantGroups();
    for (QString &id : roots) {
        id = replacements.value(id, id);
    }
    roots.removeAll(QString());
    invalidateProjectIndexCache();
    QSet<QString> layers;
    QSet<QString> guides;
    for (const QString &id : roots) {
        for (const QString &leaf : leafLayerIdsForEntry(id)) {
            layers.insert(leaf);
        }
        if (entryIsGuide(id)) {
            guides.insert(id);
        } else if (const auto *group = dynamic_cast<const fls::scene::Group *>(sceneNode(id))) {
            for (const auto *guide : sceneGuideLeaves(*group)) {
                guides.insert(guide->id);
            }
        }
    }
    setSelectionFromEntries(layers, guides, roots);

    return true;
}

bool EditorState::executeCommand(const EditorCommand &request, QString *error) {
    EditorCommand command = request;
    QString reason;
    const auto fail = [&]() {
        if (error != nullptr) {
            *error = reason;
        }
        Q_EMIT commandRejected(command.type, reason);

        return false;
    };

    if (command.type == EditorCommandType::Action) {
        Q_EMIT editActionRequested(command.property);
        Q_EMIT commandExecuted(command.type, command.entries);

        return true;
    }
    if (!hasProject_) {
        reason = QStringLiteral("Open a project first");
        return fail();
    }
    command.entries = normalizeEntrySelection(command.entries);
    if (command.type == EditorCommandType::Undo || command.type == EditorCommandType::Redo) {
        command.type == EditorCommandType::Undo ? undo() : redo();
        Q_EMIT commandExecuted(command.type, command.entries);

        return true;
    }
    if (command.type == EditorCommandType::Selection) {
        QSet<QString> layers;
        QSet<QString> guides;
        for (const QString &id : command.entries) {
            for (const QString &leaf : leafLayerIdsForEntry(id)) {
                layers.insert(leaf);
            }
            if (entryIsGuide(id)) {
                guides.insert(id);
            } else if (const auto *group = dynamic_cast<const fls::scene::Group *>(sceneNode(id))) {
                for (const auto *guide : sceneGuideLeaves(*group)) {
                    guides.insert(guide->id);
                }
            }
        }
        setSelectionFromEntries(layers, guides, command.entries);
        Q_EMIT commandExecuted(command.type, command.entries);

        return true;
    }
    if (command.type == EditorCommandType::Copy) {
        if (!copyEntriesToClipboard(command.entries)) {
            reason = QStringLiteral("Selection contains locked layers or is empty");
            return fail();
        }
        Q_EMIT commandExecuted(command.type, command.entries);

        return true;
    }
    const bool property = command.type == EditorCommandType::Property;
    const bool transform = command.type == EditorCommandType::WorldTransform || command.type == EditorCommandType::LocalTransforms;
    if (command.entries.isEmpty() && command.type != EditorCommandType::Paste && command.type != EditorCommandType::Insert) {
        reason = QStringLiteral("No selection to edit");
        return fail();
    }
    for (const QString &id : command.entries) {
        const auto *node = sceneNode(id);
        const auto *group = dynamic_cast<const fls::scene::Group *>(node);
        if (node == nullptr || (!property && group != nullptr && group->isLiverySection)) {
            reason = QStringLiteral("Invalid or structural edit target");
            return fail();
        }
    }
    if (command.type == EditorCommandType::MoveEntries && !canMoveEntries(command.entries, command.parentId, &reason)) {
        return fail();
    }
    if (!property && command.type != EditorCommandType::Paste && command.type != EditorCommandType::Insert) {
        for (const QString &id : command.entries) {
            if (entryHasLockedLayer(id)) {
                reason = QStringLiteral("Selection contains locked layers");
                return fail();
            }
        }
    }
    if (command.type == EditorCommandType::Paste && !command.clipboard && !clipboard_) {
        reason = QStringLiteral("Clipboard is empty");
        return fail();
    }
    if (command.type == EditorCommandType::LocalTransforms) {
        for (auto it = command.transforms.cbegin(); it != command.transforms.cend(); ++it) {
            const auto &frame = it.value();
            const std::initializer_list<double> values{frame.x, frame.y, frame.scaleX, frame.scaleY, frame.rotation, frame.skew};
            if (!command.entries.contains(it.key())
                || !std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); })) {
                reason = QStringLiteral("Invalid transform target or value");
                return fail();
            }
        }
    }
    if (command.type == EditorCommandType::Insert) {
        if (!command.clipboard || command.clipboard->nodes.empty()) {
            reason = QStringLiteral("No layers to insert");
            return fail();
        }
        QSet<QString> inserted;
        for (const auto &node : command.clipboard->nodes) {
            QVector<QString> ids;
            if (!node) {
                reason = QStringLiteral("Invalid layer to insert");
                return fail();
            }
            collectNodes(*node, ids);
            for (const QString &id : ids) {
                if (id.isEmpty() || inserted.contains(id) || sceneNode(id) != nullptr) {
                    reason = QStringLiteral("Inserted layers must have unique identifiers");
                    return fail();
                }
                inserted.insert(id);
            }
        }
    }
    if (command.type == EditorCommandType::Group) {
        if (command.entries.size() < 2) {
            reason = QStringLiteral("Select at least two adjacent layers to group");
            return fail();
        }
        const QString parentId = parentGroupForEntry(command.entries.front());
        QVector<int> rows;
        for (const QString &id : command.entries) {
            if (parentGroupForEntry(id) != parentId || entryIsGuide(id)
                || (entryIsGroup(id) && groupForId(id)->isLiverySection)) {
                reason = QStringLiteral("Select adjacent sibling layers");
                return fail();
            }
            rows.push_back(projectIndexCache().orderByChild.value(id));
        }
        std::sort(rows.begin(), rows.end());
        if (rows.back() - rows.front() + 1 != rows.size()) {
            reason = QStringLiteral("Select adjacent sibling layers");
            return fail();
        }
    }
    const bool owned = !pendingEdit_.has_value();
    if (owned) {
        if (transform || (property && isTransform(command.property))) {
            beginTransformCommand(command.entries);
        } else {
            beginProjectEdit();
        }
    }
    bool accepted = true;
    switch (command.type) {
    case EditorCommandType::Property:
        accepted = applyPropertyCommand(command, &reason);
        break;
    case EditorCommandType::LocalTransforms:
        for (auto it = command.transforms.cbegin(); it != command.transforms.cend(); ++it) {
            if (auto *node = sceneNode(it.key())) {
                node->transform = it.value();
            }
        }
        break;
    case EditorCommandType::WorldTransform:
        transformEntryFrames(command.entries, command.worldTransform);
        break;
    case EditorCommandType::MoveEntries:
        accepted = moveEntries(command.entries, command.parentId, command.row);
        break;
    case EditorCommandType::Cut:
        accepted = copyEntriesToClipboard(command.entries);
        if (!accepted) {
            break;
        }
        [[fallthrough]];
    case EditorCommandType::Remove:
        removeEntries(command.entries);
        pruneRedundantGroups();
        clearSelection();
        break;
    case EditorCommandType::Paste: {
        const ProjectClipboard &source = command.clipboard ? *command.clipboard : *clipboard_;
        QSet<QString> layers;
        QSet<QString> guides;
        QVector<QString> inserted;
        insertClipboardAboveSelection(source, command.entries, &layers, &guides,
                                      command.type == EditorCommandType::Paste, &inserted);
        setSelectionFromEntries(layers, guides, inserted);
        break;
    }
    case EditorCommandType::Insert: {
        QVector<QString> ids;
        if (!command.clipboard) {
            accepted = false;
            break;
        }
        for (const auto &node : command.clipboard->nodes) {
            ids.push_back(node->id);
            insertLayerAboveSelection(node->clone(), command.entries);
        }
        EditorCommand selection;
        selection.type = EditorCommandType::Selection;
        selection.entries = ids;
        executeCommand(selection);
        break;
    }
    case EditorCommandType::Duplicate:
        accepted = duplicateEntriesInPlace(command.entries);
        break;
    case EditorCommandType::Group:
        accepted = groupEntries(command.entries, &reason);
        if (accepted && (selectedEntryIds_.size() != 1
            || sceneNode(selectedEntryIds_.front())->kind() != fls::scene::LayerKind::Mesh)) {
            selectedEntryIds_.clear();
            selectedEntryIds_ = fullySelectedTopGroupIds();
        }
        break;
    case EditorCommandType::Ungroup:
        ungroupEntries(command.entries, command.flatten);
        pruneRedundantGroups();
        selectedEntryIds_.clear();
        break;
    default:
        accepted = false;
        reason = QStringLiteral("Unsupported edit command");
        break;
    }
    if (!accepted) {
        if (owned) {
            pendingEdit_.reset();
        }
        return fail();
    }
    if (owned) {
        commitProjectEdit();
    }
    const auto notify = [this, command, property, transform]() {
        if (transform || (property && isTransform(command.property))) {
            noteProjectGeometryChanged(false, command.entries);
        } else if (property && command.property != QStringLiteral("name")) {
            noteProjectGeometryChanged(true, command.entries);
        } else {
            noteProjectStructureChanged();
        }
    };
    if (command.deferRefresh) {
        QMetaObject::invokeMethod(this, notify, Qt::QueuedConnection);
    } else {
        notify();
    }
    Q_EMIT commandExecuted(command.type, command.entries);

    return true;
}

} // namespace gui
