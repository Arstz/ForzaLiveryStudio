#include "nested_payload.h"

#include "binary_io.h"
#include "matrix_math.h"

#include <QHash>
#include <QPointF>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

// Faithful port of the py-prototype build_grouped_payload / _pack_nested_group
// (commit e50f993, "Fixed nested group exporting error"), which produces nested
// C_group payloads the in-game editor accepts. Key rules:
//  * Each group carries a TRANSLATION-only transform to its bbox-center origin;
//    its shapes are packed RELATIVE to that origin, keeping child
//    scale/rotation/skew clean.
//  * Both the root and every non-root group header carry a child-type bitmap:
//    0x00 0x00 0x00 (fixed) + one bit per direct child (1 = group, 0 = shape).
//  * A group's FIRST child, when it is a shape, is written as a bare 31-byte
//    record (0x02 ...); later shapes use the 2-byte 0x00/0x01 0x02 marker.
//  * Transform markers depend on sibling position: 0x03 for the first entry,
//    0x00 0x03 after a shape sibling, and 0x00 (0x01 * depth) 0x03 after a group
//    sibling. A shape following a group is preceded by 0x00 (0x01 * (depth-1)).
//  * The payload ends with 0x00 (0x01 * (terminal_depth + 1)).

namespace fh6 {
namespace {

using detail::appendLeFloat;
using detail::appendLeU16;

QByteArray defaultPrefix()
{
    static constexpr char bytes[] = {
        'g', 'y', 'v', 'l',
        '\x01', '\x00', '\x00', '\x00',
        '\x00', '\x00', '\x00', '\x00',
        '\x03',
        '\x00', '\x00', '\x00', '\x00',
        '\x00', '\x00', '\x00', '\x00',
        '\x00', '\x00', '\x80', '\x3f',
        '\x00', '\x00', '\x00', '\x00',
    };
    return QByteArray(bytes, 29);
}

struct GroupEntry {
    enum Kind { Group, Layer, Virtual } kind = Layer;
    QString id;                   // Group/Layer id
    QVector<GroupEntry> children; // Virtual children (unused by current model)
};

struct ExportContext {
    const Project &project;
    QHash<QString, const ShapeLayer *> layersById;
    QHash<QString, const LayerGroup *> groupsById;
    QHash<QString, int> layerOrder; // id -> index in project.layers
    SpriteSizeFn spriteSize;
};

int checkChildCount(int count, const char *label)
{
    if (count <= 0) {
        throw std::runtime_error(std::string(label) + " has no children");
    }
    if (count > 0xffff) {
        throw std::runtime_error(std::string(label) + " has too many direct children");
    }
    const int childBlocks = (count + 7) / 8;
    if (childBlocks > 0xff) {
        throw std::runtime_error(std::string(label) + " needs too many child blocks");
    }
    return childBlocks;
}

QByteArray packShapeRelative(const ShapeLayer &layer, double offsetX, double offsetY,
                             quint8 firstMarker, bool bare)
{
    QByteArray out;
    out.reserve(32);
    if (bare) {
        out.append('\x02');
    } else {
        out.append(static_cast<char>(firstMarker));
        out.append('\x02');
    }
    appendLeU16(out, layer.shapeId);
    appendLeFloat(out, static_cast<float>(normalizeRotation(layer.rotation)));
    appendLeFloat(out, static_cast<float>(layer.x - offsetX));
    appendLeFloat(out, static_cast<float>(layer.y - offsetY));
    appendLeFloat(out, static_cast<float>(layer.scaleX));
    appendLeFloat(out, static_cast<float>(layer.scaleY));
    appendLeFloat(out, static_cast<float>(layer.skew));
    out.append(static_cast<char>(layer.color[0]));
    out.append(static_cast<char>(layer.color[1]));
    out.append(static_cast<char>(layer.color[2]));
    out.append(static_cast<char>(layer.color[3]));
    return out;
}

QByteArray packTranslationTransform(double x, double y, const QByteArray &marker)
{
    QByteArray out = marker;
    appendLeFloat(out, static_cast<float>(x));
    appendLeFloat(out, static_cast<float>(y));
    appendLeFloat(out, 1.0f);
    appendLeFloat(out, 0.0f);
    return out;
}

QByteArray childBitmap(const QVector<GroupEntry> &items, const char *label)
{
    const int childBlocks = checkChildCount(items.size(), label);
    QByteArray bitmap(childBlocks, '\x00');
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].kind == GroupEntry::Group || items[i].kind == GroupEntry::Virtual) {
            bitmap[i / 8] = static_cast<char>(static_cast<quint8>(bitmap[i / 8]) | (1 << (i % 8)));
        }
    }
    return bitmap;
}

// marker (0x20 normal / 0x60 mask) + u16 count + u8 child_blocks +
// 0x00 0x00 0x00 + child bitmap.
QByteArray packGroupHeader(int count, const QByteArray &bitmap, quint8 marker, const char *label)
{
    checkChildCount(count, label);
    QByteArray out;
    out.append(static_cast<char>(marker));
    appendLeU16(out, static_cast<quint16>(count));
    out.append(static_cast<char>(bitmap.size()));
    out.append(QByteArray(3, '\x00'));
    out.append(bitmap);
    return out;
}

// 0x00 + (0x01 * max(1, previousGroupDepth)) + 0x03
QByteArray siblingGroupTransformMarker(int previousGroupDepth)
{
    QByteArray out;
    out.append('\x00');
    out.append(QByteArray(std::max(1, previousGroupDepth), '\x01'));
    out.append('\x03');
    return out;
}

QPointF spriteHalfExtents(const ExportContext &ctx, quint16 shapeId)
{
    const QSizeF size = ctx.spriteSize ? ctx.spriteSize(shapeId) : QSizeF(128.0, 128.0);
    double w = size.width();
    double h = size.height();
    double side = std::max(w, h);
    if (side <= 0.0) {
        side = 1.0;
    }
    return QPointF(64.0 * w / side, 64.0 * h / side);
}

QVector<QPointF> layerCorners(const ExportContext &ctx, const ShapeLayer &layer)
{
    const QPointF he = spriteHalfExtents(ctx, layer.shapeId);
    const double hw = he.x();
    const double hh = he.y();
    FlattenedLayer fl;
    fl.rotation = layer.rotation;
    fl.posX = layer.x;
    fl.posY = layer.y;
    fl.scaleX = layer.scaleX;
    fl.scaleY = layer.scaleY;
    fl.skew = layer.skew;
    const Matrix3 m = shapeMatrix(fl);
    QVector<QPointF> corners;
    const double pts[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (const auto &p : pts) {
        const double x = m.m[0][0] * p[0] + m.m[0][1] * p[1] + m.m[0][2];
        const double y = m.m[1][0] * p[0] + m.m[1][1] * p[1] + m.m[1][2];
        corners.push_back(QPointF(x, y));
    }
    return corners;
}

QVector<QString> groupLeafIds(const ExportContext &ctx, const QString &groupId)
{
    const LayerGroup *group = ctx.groupsById.value(groupId, nullptr);
    if (!group) {
        return {};
    }
    QVector<QString> leaves;
    std::function<void(const QString &)> visit = [&](const QString &id) {
        if (ctx.groupsById.contains(id)) {
            for (const QString &childId : ctx.groupsById.value(id)->childIds) {
                visit(childId);
            }
        } else if (ctx.layersById.contains(id)) {
            leaves.push_back(id);
        }
    };
    for (const QString &childId : group->childIds) {
        visit(childId);
    }
    QVector<QString> unique;
    QHash<QString, bool> seen;
    for (const QString &id : leaves) {
        if (!seen.contains(id)) {
            seen.insert(id, true);
            unique.push_back(id);
        }
    }
    std::sort(unique.begin(), unique.end(), [&](const QString &a, const QString &b) {
        return ctx.layerOrder.value(a, -1) < ctx.layerOrder.value(b, -1);
    });
    return unique;
}

void entryLayers(const ExportContext &ctx, const GroupEntry &entry, QVector<const ShapeLayer *> &out)
{
    switch (entry.kind) {
    case GroupEntry::Layer:
        if (const ShapeLayer *l = ctx.layersById.value(entry.id, nullptr)) {
            out.push_back(l);
        }
        break;
    case GroupEntry::Group:
        for (const QString &id : groupLeafIds(ctx, entry.id)) {
            if (const ShapeLayer *l = ctx.layersById.value(id, nullptr)) {
                out.push_back(l);
            }
        }
        break;
    case GroupEntry::Virtual:
        for (const GroupEntry &child : entry.children) {
            entryLayers(ctx, child, out);
        }
        break;
    }
}

QPointF layersOrigin(const ExportContext &ctx, const QVector<const ShapeLayer *> &leaves)
{
    if (leaves.isEmpty()) {
        return QPointF(0.0, 0.0);
    }
    QVector<QPointF> points;
    for (const ShapeLayer *l : leaves) {
        points.append(layerCorners(ctx, *l));
    }
    if (points.isEmpty()) {
        return QPointF(leaves.front()->x, leaves.front()->y);
    }
    double minX = points.front().x(), maxX = minX;
    double minY = points.front().y(), maxY = minY;
    for (const QPointF &p : points) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    return QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
}

QPointF entryOrigin(const ExportContext &ctx, const GroupEntry &entry)
{
    QVector<const ShapeLayer *> leaves;
    entryLayers(ctx, entry, leaves);
    return layersOrigin(ctx, leaves);
}

// A group is encoded as a mask group (0x60) when every one of its leaf layers is
// masked. The game masks all descendants of a 0x60 group, so this reproduces a
// whole-group mask (e.g. an imported 0x60 group whose children all carry the
// inherited mask flag).
bool entryAllMasked(const ExportContext &ctx, const GroupEntry &entry)
{
    QVector<const ShapeLayer *> leaves;
    entryLayers(ctx, entry, leaves);
    if (leaves.isEmpty()) {
        return false;
    }
    for (const ShapeLayer *l : leaves) {
        if (!l->mask) {
            return false;
        }
    }
    return true;
}

QVector<GroupEntry> groupEntryChildren(const ExportContext &ctx, const GroupEntry &entry)
{
    if (entry.kind == GroupEntry::Group) {
        const LayerGroup *group = ctx.groupsById.value(entry.id, nullptr);
        if (!group) {
            throw std::runtime_error("missing group in nested export");
        }
        QVector<GroupEntry> entries;
        for (const QString &childId : group->childIds) {
            if (ctx.groupsById.contains(childId)) {
                entries.push_back({GroupEntry::Group, childId, {}});
            } else if (ctx.layersById.contains(childId)) {
                entries.push_back({GroupEntry::Layer, childId, {}});
            }
        }
        return entries;
    }
    if (entry.kind == GroupEntry::Virtual) {
        return entry.children;
    }
    throw std::runtime_error("layer entries cannot have children");
}

// Depth of the last-child chain (matches _group_entry_terminal_depth).
int terminalDepth(const ExportContext &ctx, const GroupEntry &entry)
{
    const QVector<GroupEntry> children = groupEntryChildren(ctx, entry);
    if (children.isEmpty()
        || (children.back().kind != GroupEntry::Group && children.back().kind != GroupEntry::Virtual)) {
        return 1;
    }
    return 1 + terminalDepth(ctx, children.back());
}

// The trailing mask flag also spans group boundaries: a group's transform marker
// leads with 0x00, and that slot masks the immediately preceding shape when set
// to 0x01. So flip a marker's leading 0x00 to 0x01 when the previous emitted
// shape was a mask.
QByteArray maybeFlipMaskFlag(QByteArray marker, bool prevShapeMask)
{
    if (prevShapeMask && !marker.isEmpty() && static_cast<quint8>(marker[0]) == 0x00) {
        marker[0] = '\x01';
    }
    return marker;
}

// `prevShapeMask` threads the "previous emitted shape was a mask" state across the
// whole recursive emission so the trailing 0x01 flag reaches the following record
// (shape or group transform), including the last shape of a mask group.
QByteArray packNestedGroup(const ExportContext &ctx, const GroupEntry &entry,
                           QPointF parentOffset, const QByteArray &transformMarker,
                           bool parentMask, bool &prevShapeMask)
{
    const QVector<GroupEntry> children = groupEntryChildren(ctx, entry);
    if (children.size() < 2) {
        throw std::runtime_error("nested group has fewer than two children");
    }
    // A group is a mask group (0x60) whenever all its leaves are masked. The game
    // marks EVERY such group, including mask groups nested inside mask groups, so
    // we do not gate on the parent's mask state.
    const bool isMaskGroup = entryAllMasked(ctx, entry);
    const bool childMask = parentMask || isMaskGroup;
    const QPointF origin = entryOrigin(ctx, entry);
    QByteArray out = packTranslationTransform(origin.x() - parentOffset.x(),
                                              origin.y() - parentOffset.y(), transformMarker);
    out.append(packGroupHeader(children.size(), childBitmap(children, "group"),
                               isMaskGroup ? 0x60 : 0x20, "group"));

    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    QString previousSiblingId;
    for (const GroupEntry &child : children) {
        if (child.kind == GroupEntry::Group || child.kind == GroupEntry::Virtual) {
            QByteArray marker;
            if (previousWasGroup) {
                marker = siblingGroupTransformMarker(previousGroupDepth);
            } else if (!previousSiblingId.isEmpty()) {
                marker = QByteArray("\x00\x03", 2);
            } else {
                marker = QByteArray("\x03", 1);
            }
            marker = maybeFlipMaskFlag(marker, prevShapeMask);
            prevShapeMask = false;  // consumed by this group's transform marker
            out.append(packNestedGroup(ctx, child, origin, marker, childMask, prevShapeMask));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(ctx, child);
        } else {
            const ShapeLayer *layer = ctx.layersById.value(child.id, nullptr);
            if (!layer) {
                continue;
            }
            if (!layer->visible) {
                throw std::runtime_error("grouped export cannot encode a hidden child inside a visible group");
            }
            if (previousWasGroup) {
                out.append('\x00');
                out.append(QByteArray(std::max(0, previousGroupDepth - 1), '\x01'));
            }
            // 0x01 02 trailing mask flag: a record leads with 0x01 when the
            // previous shape is a mask (this record masks it).
            const quint8 lead = (previousWasGroup || prevShapeMask) ? 0x01 : 0x00;
            out.append(packShapeRelative(*layer, origin.x(), origin.y(),
                                         lead, previousSiblingId.isEmpty()));
            previousWasGroup = false;
            previousGroupDepth = 0;
            prevShapeMask = layer->mask;
        }
        previousSiblingId = child.id;
    }
    return out;
}

QVector<GroupEntry> rootItems(const ExportContext &ctx)
{
    QHash<QString, QString> parentOf;
    for (const LayerGroup &group : ctx.project.groups) {
        for (const QString &childId : group.childIds) {
            parentOf.insert(childId, group.id);
        }
    }
    QVector<GroupEntry> items;
    QHash<QString, bool> emitted;
    for (const ShapeLayer &layer : ctx.project.layers) {
        QString topId = layer.id;
        while (parentOf.contains(topId)) {
            topId = parentOf.value(topId);
        }
        if (emitted.contains(topId)) {
            continue;
        }
        emitted.insert(topId, true);
        if (topId == layer.id) {
            items.push_back({GroupEntry::Layer, layer.id, {}});
        } else {
            items.push_back({GroupEntry::Group, topId, {}});
        }
    }
    return items;
}

QByteArray rootCloseBytes(const ExportContext &ctx, const QVector<GroupEntry> &items)
{
    int terminal = 0;
    if (!items.isEmpty() && items.back().kind == GroupEntry::Group) {
        terminal = terminalDepth(ctx, items.back());
    }
    QByteArray out;
    out.append('\x00');
    out.append(QByteArray(terminal + 1, '\x01'));
    return out;
}

} // namespace

QByteArray buildNestedPayload(const Project &project, const SpriteSizeFn &spriteSize)
{
    ExportContext ctx{project, {}, {}, {}, spriteSize};
    for (int i = 0; i < project.layers.size(); ++i) {
        ctx.layersById.insert(project.layers[i].id, &project.layers[i]);
        ctx.layerOrder.insert(project.layers[i].id, i);
    }
    for (const LayerGroup &group : project.groups) {
        ctx.groupsById.insert(group.id, &group);
    }

    QVector<GroupEntry> items;
    for (const GroupEntry &item : rootItems(ctx)) {
        if (item.kind == GroupEntry::Group) {
            items.push_back(item);
        } else if (const ShapeLayer *l = ctx.layersById.value(item.id, nullptr); l && l->visible) {
            items.push_back(item);
        }
    }
    if (items.isEmpty()) {
        throw std::runtime_error("project has no visible layers to export");
    }

    // Recenter the whole decal on the origin. Group origins cancel out in
    // reconstruction (a group's translation origin plus its shapes' relative
    // offsets always rebuild the original absolute position), so a nested payload
    // is geometrically identical to the flat one. But the game auto-centers a flat
    // single-group decal by its bounding box while placing a nested one at its raw
    // coordinates - so an off-origin project lands in a corner (top-right quadrant)
    // when exported nested. Subtracting the overall bbox center from every root
    // entry pins the group content at the origin, matching the flat export's
    // centered placement. (Already-centered projects have center ~= 0, so this is a
    // no-op for them.)
    QVector<const ShapeLayer *> allLeaves;
    for (const GroupEntry &item : items) {
        entryLayers(ctx, item, allLeaves);
    }
    const QPointF center = layersOrigin(ctx, allLeaves);

    QByteArray prefix = project.sourceDecPrefix.isEmpty() ? defaultPrefix() : project.sourceDecPrefix;
    prefix = prefix.left(0x1d);
    if (prefix.size() < 0x1d) {
        throw std::runtime_error("source decompressed prefix is shorter than 0x1d bytes");
    }
    QByteArray rootTransform;
    appendLeFloat(rootTransform, 0.0f);
    appendLeFloat(rootTransform, 0.0f);
    appendLeFloat(rootTransform, 1.0f);
    appendLeFloat(rootTransform, 0.0f);
    prefix.replace(0x0d, 16, rootTransform);

    const QByteArray rootBitmap = childBitmap(items, "root");
    QByteArray payload = prefix;
    payload.append('\x20');
    appendLeU16(payload, static_cast<quint16>(items.size()));
    payload.append(static_cast<char>(rootBitmap.size()));
    payload.append(QByteArray(3, '\x00'));
    payload.append(rootBitmap);

    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    bool prevShapeMask = false;
    QString previousSiblingId;
    for (const GroupEntry &item : items) {
        if (item.kind == GroupEntry::Group) {
            QByteArray marker;
            if (previousWasGroup) {
                marker = siblingGroupTransformMarker(previousGroupDepth);
            } else if (!previousSiblingId.isEmpty()) {
                marker = QByteArray("\x00\x03", 2);
            } else {
                marker = QByteArray("\x03", 1);
            }
            marker = maybeFlipMaskFlag(marker, prevShapeMask);
            prevShapeMask = false;  // consumed by this group's transform marker
            payload.append(packNestedGroup(ctx, item, center, marker,
                                           /*parentMask=*/false, prevShapeMask));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(ctx, item);
        } else {
            const ShapeLayer *layer = ctx.layersById.value(item.id, nullptr);
            if (!layer) {
                continue;
            }
            if (previousWasGroup) {
                payload.append('\x00');
                payload.append(QByteArray(std::max(0, previousGroupDepth - 1), '\x01'));
            }
            // The root's first child shape is bare (0x02), just like a group's
            // first shape: its leading byte overlaps the root child-type bitmap.
            // Otherwise lead with 0x01 (mask) when the previous root shape masks.
            const quint8 lead = (previousWasGroup || prevShapeMask) ? 0x01 : 0x00;
            payload.append(packShapeRelative(*layer, center.x(), center.y(), lead,
                                             previousSiblingId.isEmpty()));
            previousWasGroup = false;
            previousGroupDepth = 0;
            prevShapeMask = layer->mask;
        }
        previousSiblingId = item.id;
    }
    // If the final emitted shape is a mask, the closing bytes' leading 0x00 slot
    // masks it (same trailing-flag mechanism as a following group marker).
    payload.append(maybeFlipMaskFlag(rootCloseBytes(ctx, items), prevShapeMask));
    return payload;
}

} // namespace fh6
