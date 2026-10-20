#include "nested_payload.h"
#include "vinyl_framing.h"
#include "vinyl_encoding.h"

#include "binary_io.h"
#include "layer.h"
#include "matrix_math.h"

#include <QPointF>

#include <algorithm>
#include <stdexcept>

namespace fls {
namespace {

constexpr int kMaxDirectChildren = 0xffff;

using detail::appendLeFloat;

struct GroupEntry {
    enum Kind { Group, Layer } kind = Layer;
    const scene::Group *group = nullptr;
    const scene::Shape *shape = nullptr;

    QString id() const {
        if (group != nullptr) {
            return group->id;
        }
        if (shape != nullptr) {
            return shape->id;
        }
        return {};
    }
};

struct ExportContext {
    const Project &project;
    SpriteSizeFn spriteSize;
    GroupRecordGeneration generation = GroupRecordGeneration::Current;
    bool embeddedLivery = false;
};

int checkChildCount(int count, const char *label) {
    if (count <= 0) {
        throw std::runtime_error(std::string(label) + " has no children");
    }
    if (count > kMaxDirectChildren) {
        throw std::runtime_error(std::string(label) + " has too many direct children for the child bitmap");
    }
    const int blocks = (count + 7) / 8;
    return blocks;
}

QByteArray packShapeRelative(const detail::VinylShapePlacement &shape, double offsetX, double offsetY,
                             quint8 firstMarker, bool bare, GroupRecordGeneration generation) {
    QByteArray out;
    const std::optional<quint8> state = !bare && generation == GroupRecordGeneration::Current
        ? std::optional<quint8>{firstMarker} : std::nullopt;
    const bool currentGeneration = generation == GroupRecordGeneration::Current;

    out.reserve(32);
    detail::appendVinylShapeRecord(out, shape, offsetX, offsetY,
                                   currentGeneration ? 0x02 : 0x01, state);

    return out;
}

QByteArray packTranslationTransform(double x, double y, const QByteArray &marker) {
    QByteArray out = marker;
    appendLeFloat(out, static_cast<float>(x));
    appendLeFloat(out, static_cast<float>(y));
    appendLeFloat(out, 1.0f);
    appendLeFloat(out, 0.0f);
    return out;
}

QByteArray childBitmap(const QVector<GroupEntry> &items, const char *label,
                       GroupRecordGeneration generation) {
    const int blocks = checkChildCount(items.size(), label);
    QByteArray bitmap(blocks, '\x00');
    for (int i = 0; i < items.size(); ++i) {
        const bool maskedShapeGroup = generation != GroupRecordGeneration::Current
            && items[i].shape != nullptr && items[i].shape->mask;
        if (items[i].kind == GroupEntry::Group || maskedShapeGroup) {
            bitmap[i / 8] = static_cast<char>(static_cast<quint8>(bitmap[i / 8]) | (1 << (i % 8)));
        }
    }
    return bitmap;
}

QByteArray packGroupHeader(int count, const QByteArray &bitmap, quint8 marker, const char *label) {
    checkChildCount(count, label);

    return detail::writeVinylChildHeader(count, bitmap, marker);
}

QByteArray packNativeShape(const detail::VinylShapePlacement &shape, const QPointF &origin,
                           quint8 lead, bool bare, GroupRecordGeneration generation) {
    if (generation != GroupRecordGeneration::Current && shape.mask) {
        QByteArray out = packGroupHeader(1, QByteArray(1, '\0'), 0x60, "mask");
        out.append(packShapeRelative(shape, origin.x(), origin.y(), lead, true, generation));

        return out;
    }

    return packShapeRelative(shape, origin.x(), origin.y(), lead, bare, generation);
}

QByteArray groupTransformMarker(GroupRecordGeneration generation, bool previousWasGroup,
                                int previousGroupDepth, bool hasPreviousSibling) {
    if (generation == GroupRecordGeneration::First) {
        return QByteArray(1, '\x01');
    }
    if (generation == GroupRecordGeneration::Second) {
        QByteArray marker(previousWasGroup ? std::max(1, previousGroupDepth) : 0, '\x01');
        marker.append('\x02');

        return marker;
    }
    return detail::vinylGroupTransformMarker(
        previousWasGroup, previousGroupDepth, hasPreviousSibling, QByteArray(1, '\x03'));
}

void appendGroupShapeTransition(QByteArray &out, int previousGroupDepth,
                                GroupRecordGeneration generation) {
    if (generation == GroupRecordGeneration::Current) {
        detail::appendVinylGroupShapeTransition(out, previousGroupDepth);
    } else if (generation == GroupRecordGeneration::Second) {
        out.append(QByteArray(std::max(1, previousGroupDepth), '\x01'));
    }
}

QPointF spriteHalfExtents(const ExportContext &ctx, quint16 shapeId) {
    const QSizeF size = ctx.spriteSize ? ctx.spriteSize(shapeId) : QSizeF(128.0, 128.0);
    double w = size.width();
    double h = size.height();
    double side = std::max(w, h);
    if (side <= 0.0) {
        side = 1.0;
    }
    return QPointF(64.0 * w / side, 64.0 * h / side);
}

QVector<QPointF> shapeCorners(const ExportContext &ctx, const scene::Shape &shape) {
    const detail::VinylShapePlacement s = detail::vinylShapePlacement(shape);
    const QPointF he = spriteHalfExtents(ctx, s.shapeId);

    return detail::vinylShapeCorners(s, he);
}

void collectShapeLeaves(const scene::Layer &node, QVector<const scene::Shape *> &out) {
    if (node.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectShapeLeaves(*child, out);
        }
    } else if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(static_cast<const scene::Shape *>(&node));
    }
}

void entryShapes(const GroupEntry &entry, QVector<const scene::Shape *> &out) {
    switch (entry.kind) {
    case GroupEntry::Layer:
        if (entry.shape != nullptr) {
            out.push_back(entry.shape);
        }
        break;
    case GroupEntry::Group:
        if (entry.group != nullptr) {
            collectShapeLeaves(*entry.group, out);
        }
        break;
    }
}

QString entryShapeRange(const ExportContext &ctx, const GroupEntry &entry) {
    QVector<const scene::Shape *> entryLeaves;
    entryShapes(entry, entryLeaves);
    QVector<const scene::Shape *> projectLeaves;
    if (ctx.project.root != nullptr) {
        collectShapeLeaves(*ctx.project.root, projectLeaves);
    }

    int minPosition = 0;
    int maxPosition = 0;
    for (const scene::Shape *shape : entryLeaves) {
        const int index = projectLeaves.indexOf(shape);
        if (index < 0) {
            continue;
        }
        const int position = index + 1;
        minPosition = minPosition == 0 ? position : std::min(minPosition, position);
        maxPosition = std::max(maxPosition, position);
    }
    return QStringLiteral("%1-%2").arg(minPosition).arg(maxPosition);
}

QPointF shapesOrigin(const ExportContext &ctx, const QVector<const scene::Shape *> &leaves) {
    if (leaves.isEmpty()) {
        return QPointF(0.0, 0.0);
    }
    QVector<QPointF> points;
    for (const scene::Shape *shape : leaves) {
        points.append(shapeCorners(ctx, *shape));
    }
    if (points.isEmpty()) {
        const detail::VinylShapePlacement first = detail::vinylShapePlacement(*leaves.front());
        return QPointF(first.x, first.y);
    }
    return pointBoundsCenter(points);
}

QPointF entryOrigin(const ExportContext &ctx, const GroupEntry &entry) {
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    return shapesOrigin(ctx, leaves);
}

bool entryAllMasked(const GroupEntry &entry) {
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    if (leaves.isEmpty()) {
        return false;
    }
    for (const scene::Shape *shape : leaves) {
        if (!shape->mask) {
            return false;
        }
    }
    return true;
}

bool containsContourFigure(const scene::Layer &node) {
    if (node.kind() == scene::LayerKind::Contour) {
        return true;
    }
    if (node.kind() != scene::LayerKind::Group) {
        return false;
    }
    const auto &group = static_cast<const scene::Group &>(node);

    return std::any_of(group.children.cbegin(), group.children.cend(), [](const auto &child) {
        return containsContourFigure(*child);
    });
}

void appendGameEntry(const scene::Layer &node, QVector<GroupEntry> &entries);

QVector<GroupEntry> directChildren(const GroupEntry &entry) {
    if (entry.kind == GroupEntry::Group) {
        if (entry.group == nullptr) {
            throw std::runtime_error("missing group in nested export");
        }
        QVector<GroupEntry> entries;
        for (const auto &child : entry.group->children) {
            appendGameEntry(*child, entries);
        }
        return entries;
    }
    throw std::runtime_error("layer entries cannot have children");
}

void appendGameEntry(const scene::Layer &node, QVector<GroupEntry> &entries) {
    if (node.kind() == scene::LayerKind::Shape) {
        entries.push_back({GroupEntry::Layer, nullptr, static_cast<const scene::Shape *>(&node)});
        return;
    }
    if (node.kind() != scene::LayerKind::Group) {
        return;
    }
    const GroupEntry entry{GroupEntry::Group, static_cast<const scene::Group *>(&node), nullptr};

    if (containsContourFigure(node)) {
        const QVector<GroupEntry> children = directChildren(entry);
        if (children.size() < 2) {
            entries += children;
            return;
        }
    }
    entries.push_back(entry);
}

int terminalDepth(const GroupEntry &entry) {
    const QVector<GroupEntry> children = directChildren(entry);
    if (children.isEmpty()
        || children.back().kind != GroupEntry::Group) {
        return 1;
    }
    return 1 + terminalDepth(children.back());
}

QByteArray maybeFlipMaskFlag(QByteArray marker, bool previousShapeMask) {
    if (previousShapeMask && !marker.isEmpty() && static_cast<quint8>(marker[0]) == 0x00) {
        marker[0] = '\x01';
    }
    return marker;
}

QByteArray packNestedGroup(const ExportContext &ctx, const GroupEntry &entry,
                           QPointF parentOffset, const QByteArray &transformMarker,
                           bool parentMask, bool &previousShapeMask) {
    const QVector<GroupEntry> children = directChildren(entry);
    if (children.isEmpty()
        || (ctx.generation == GroupRecordGeneration::Current && children.size() < 2)) {
        throw std::runtime_error(
            QStringLiteral("nested group has fewer than two children: %1")
                .arg(entryShapeRange(ctx, entry))
                .toStdString());
    }
    const bool isMaskGroup = entryAllMasked(entry);
    const bool childMask = parentMask || isMaskGroup;
    const bool currentGeneration = ctx.generation == GroupRecordGeneration::Current;
    const QPointF origin = entryOrigin(ctx, entry);
    const QByteArray nativeMarker = ctx.embeddedLivery ? QByteArray() : transformMarker;
    const QByteArray header = packGroupHeader(children.size(), childBitmap(children, "group", ctx.generation),
                                             isMaskGroup ? 0x60 : 0x20, "group");
    QByteArray out = packTranslationTransform(origin.x() - parentOffset.x(), origin.y() - parentOffset.y(), nativeMarker);
    if (ctx.generation == GroupRecordGeneration::First && !isMaskGroup && children.size() > 1) {
        out.append('\0');
        out.append(header.mid(1));
    } else {
        out.append(header);
    }

    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    QString previousSiblingId;
    for (const GroupEntry &child : children) {
        if (child.kind == GroupEntry::Group) {
            QByteArray marker = groupTransformMarker(ctx.generation, previousWasGroup,
                                                     previousGroupDepth, !previousSiblingId.isEmpty());
            marker = maybeFlipMaskFlag(marker, previousShapeMask);
            previousShapeMask = false;
            out.append(packNestedGroup(ctx, child, origin, marker, childMask, previousShapeMask));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(child);
        } else {
            if (child.shape == nullptr) {
                continue;
            }
            if (!child.shape->visible) {
                throw std::runtime_error("grouped export cannot encode a hidden child inside a visible group");
            }
            if (previousWasGroup) {
                appendGroupShapeTransition(out, previousGroupDepth, ctx.generation);
            }
            const detail::VinylShapePlacement packed = detail::vinylShapePlacement(*child.shape);
            const quint8 lead = currentGeneration && (previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            out.append(packNativeShape(packed, origin, lead, previousSiblingId.isEmpty(), ctx.generation));
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = packed.mask;
        }
        previousSiblingId = child.id();
    }
    return out;
}

QVector<GroupEntry> rootItems(const Project &project) {
    QVector<GroupEntry> items;
    if (!project.root) {
        return items;
    }
    for (const auto &child : project.root->children) {
        if (!child->visible) {
            continue;
        }
        appendGameEntry(*child, items);
    }
    return items;
}

QByteArray rootCloseBytes(const QVector<GroupEntry> &items) {
    int terminal = 0;
    if (!items.isEmpty() && items.back().kind == GroupEntry::Group) {
        terminal = terminalDepth(items.back());
    }
    QByteArray out;
    out.append('\x00');
    out.append(QByteArray(terminal + 1, '\x01'));
    return out;
}

} // namespace

QByteArray buildNestedPayload(const Project &project, const SpriteSizeFn &spriteSize,
                             GroupRecordGeneration generation, bool centerArtwork,
                             bool embeddedLivery) {
    ExportContext ctx{project, spriteSize, generation, embeddedLivery};
    QVector<GroupEntry> items = rootItems(project);
    items.erase(std::remove_if(items.begin(), items.end(), [](const GroupEntry &entry) {
                    return entry.kind == GroupEntry::Layer && (entry.shape == nullptr || !entry.shape->visible);
                }),
                items.end());
    if (items.isEmpty()) {
        throw std::runtime_error("project has no visible layers to export");
    }

    QVector<const scene::Shape *> allLeaves;
    for (const GroupEntry &item : items) {
        entryShapes(item, allLeaves);
    }
    const QPointF center = centerArtwork ? shapesOrigin(ctx, allLeaves) : QPointF();

    const QByteArray prefix = detail::standaloneVinylPrefix(
        project.sourceDecPrefix, static_cast<quint8>(generation));

    const QByteArray rootBitmap = childBitmap(items, "root", generation);
    QByteArray payload = prefix;
    payload.append(detail::writeVinylChildHeader(items.size(), rootBitmap,
        generation == GroupRecordGeneration::First ? 0x00 : 0x20, true));

    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    bool previousShapeMask = false;
    QString previousSiblingId;
    for (const GroupEntry &item : items) {
        if (item.kind == GroupEntry::Group) {
            QByteArray marker = groupTransformMarker(generation, previousWasGroup,
                                                     previousGroupDepth, !previousSiblingId.isEmpty());
            marker = maybeFlipMaskFlag(marker, previousShapeMask);
            previousShapeMask = false;
            payload.append(packNestedGroup(ctx, item, center, marker, false, previousShapeMask));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(item);
        } else {
            if (item.shape == nullptr) {
                continue;
            }
            if (previousWasGroup) {
                appendGroupShapeTransition(payload, previousGroupDepth, generation);
            }
            const detail::VinylShapePlacement packed = detail::vinylShapePlacement(*item.shape);
            const quint8 lead = generation == GroupRecordGeneration::Current
                && (previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            payload.append(packNativeShape(packed, center, lead, previousSiblingId.isEmpty(), generation));
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = packed.mask;
        }
        previousSiblingId = item.id();
    }
    if (generation == GroupRecordGeneration::Current) {
        payload.append(maybeFlipMaskFlag(rootCloseBytes(items), previousShapeMask));
    } else if (generation == GroupRecordGeneration::Second) {
        payload.append(rootCloseBytes(items).mid(1));
    }
    return payload;
}

} // namespace fls
