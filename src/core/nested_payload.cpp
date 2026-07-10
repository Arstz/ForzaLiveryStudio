#include "nested_payload.h"

#include "binary_io.h"
#include "layer.h"
#include "matrix_math.h"

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fh6 {
namespace {

constexpr int kMaxDirectChildren = 0xff * 8;

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
    const scene::Group *group = nullptr;
    const scene::Shape *shape = nullptr;
    QVector<GroupEntry> children;

    QString id() const
    {
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
};

struct ExportShape {
    const scene::Shape *node = nullptr;
    quint16 shapeId = 0;
    double rotation = 0.0;
    double x = 0.0;
    double y = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double skew = 0.0;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool mask = false;
};

ExportShape exportShape(const scene::Shape &shape)
{
    const scene::Transform2D world = decomposeTransform2D(shape.worldMatrix());
    ExportShape out;
    out.node = &shape;
    out.shapeId = shape.shapeId;
    out.rotation = world.rotation;
    out.x = world.x;
    out.y = world.y;
    out.scaleX = world.scaleX;
    out.scaleY = world.scaleY;
    out.skew = world.skew;
    out.color = shape.color;
    out.color[3] = static_cast<quint8>(std::clamp<int>(std::lround(shape.opacity * 255.0), 0, 255));
    out.mask = shape.mask;
    return out;
}

int checkChildCount(int count, const char *label)
{
    if (count <= 0) {
        throw std::runtime_error(std::string(label) + " has no children");
    }
    if (count > kMaxDirectChildren) {
        throw std::runtime_error(std::string(label) + " has too many direct children for the child bitmap");
    }
    const int blocks = (count + 7) / 8;
    if (blocks > 0xff) {
        throw std::runtime_error(std::string(label) + " needs too many child blocks");
    }
    return blocks;
}

QByteArray packShapeRelative(const ExportShape &shape, double offsetX, double offsetY,
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
    appendLeU16(out, shape.shapeId);
    appendLeFloat(out, static_cast<float>(normalizeRotation(shape.rotation)));
    appendLeFloat(out, static_cast<float>(shape.x - offsetX));
    appendLeFloat(out, static_cast<float>(shape.y - offsetY));
    appendLeFloat(out, static_cast<float>(shape.scaleX));
    appendLeFloat(out, static_cast<float>(shape.scaleY));
    appendLeFloat(out, static_cast<float>(shape.skew));
    out.append(static_cast<char>(shape.color[0]));
    out.append(static_cast<char>(shape.color[1]));
    out.append(static_cast<char>(shape.color[2]));
    out.append(static_cast<char>(shape.color[3]));
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
    const int blocks = checkChildCount(items.size(), label);
    QByteArray bitmap(blocks, '\x00');
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].kind == GroupEntry::Group || items[i].kind == GroupEntry::Virtual) {
            bitmap[i / 8] = static_cast<char>(static_cast<quint8>(bitmap[i / 8]) | (1 << (i % 8)));
        }
    }
    return bitmap;
}

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

QVector<QPointF> shapeCorners(const ExportContext &ctx, const scene::Shape &shape)
{
    const ExportShape s = exportShape(shape);
    const QPointF he = spriteHalfExtents(ctx, s.shapeId);
    FlattenedLayer fl;
    fl.rotation = s.rotation;
    fl.posX = s.x;
    fl.posY = s.y;
    fl.scaleX = s.scaleX;
    fl.scaleY = s.scaleY;
    fl.skew = s.skew;
    const Matrix3 m = shapeMatrix(fl);
    QVector<QPointF> corners;
    const double pts[4][2] = {{-he.x(), -he.y()}, {he.x(), -he.y()}, {he.x(), he.y()}, {-he.x(), he.y()}};
    for (const auto &p : pts) {
        corners.push_back(QPointF(m.m[0][0] * p[0] + m.m[0][1] * p[1] + m.m[0][2],
                                  m.m[1][0] * p[0] + m.m[1][1] * p[1] + m.m[1][2]));
    }
    return corners;
}

void collectShapeLeaves(const scene::Layer &node, QVector<const scene::Shape *> &out)
{
    if (node.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectShapeLeaves(*child, out);
        }
    } else if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(static_cast<const scene::Shape *>(&node));
    }
}

void entryShapes(const GroupEntry &entry, QVector<const scene::Shape *> &out)
{
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
    case GroupEntry::Virtual:
        for (const GroupEntry &child : entry.children) {
            entryShapes(child, out);
        }
        break;
    }
}

QPointF shapesOrigin(const ExportContext &ctx, const QVector<const scene::Shape *> &leaves)
{
    if (leaves.isEmpty()) {
        return QPointF(0.0, 0.0);
    }
    QVector<QPointF> points;
    for (const scene::Shape *shape : leaves) {
        points.append(shapeCorners(ctx, *shape));
    }
    if (points.isEmpty()) {
        const ExportShape first = exportShape(*leaves.front());
        return QPointF(first.x, first.y);
    }
    double minX = points.front().x();
    double maxX = minX;
    double minY = points.front().y();
    double maxY = minY;
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
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    return shapesOrigin(ctx, leaves);
}

bool entryAllMasked(const GroupEntry &entry)
{
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

QVector<GroupEntry> directChildren(const GroupEntry &entry)
{
    if (entry.kind == GroupEntry::Group) {
        if (entry.group == nullptr) {
            throw std::runtime_error("missing group in nested export");
        }
        QVector<GroupEntry> entries;
        for (const auto &child : entry.group->children) {
            if (child->kind() == scene::LayerKind::Group) {
                entries.push_back({GroupEntry::Group, static_cast<const scene::Group *>(child.get()), nullptr, {}});
            } else if (child->kind() == scene::LayerKind::Shape) {
                entries.push_back({GroupEntry::Layer, nullptr, static_cast<const scene::Shape *>(child.get()), {}});
            }
        }
        return entries;
    }
    if (entry.kind == GroupEntry::Virtual) {
        return entry.children;
    }
    throw std::runtime_error("layer entries cannot have children");
}

int terminalDepth(const GroupEntry &entry)
{
    const QVector<GroupEntry> children = directChildren(entry);
    if (children.isEmpty()
        || (children.back().kind != GroupEntry::Group && children.back().kind != GroupEntry::Virtual)) {
        return 1;
    }
    return 1 + terminalDepth(children.back());
}

QByteArray maybeFlipMaskFlag(QByteArray marker, bool previousShapeMask)
{
    if (previousShapeMask && !marker.isEmpty() && static_cast<quint8>(marker[0]) == 0x00) {
        marker[0] = '\x01';
    }
    return marker;
}

QByteArray packNestedGroup(const ExportContext &ctx, const GroupEntry &entry,
                           QPointF parentOffset, const QByteArray &transformMarker,
                           bool parentMask, bool &previousShapeMask)
{
    const QVector<GroupEntry> children = directChildren(entry);
    if (children.size() < 2) {
        throw std::runtime_error("nested group has fewer than two children");
    }
    const bool isMaskGroup = entryAllMasked(entry);
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
                out.append('\x00');
                out.append(QByteArray(std::max(0, previousGroupDepth - 1), '\x01'));
            }
            const ExportShape packed = exportShape(*child.shape);
            const quint8 lead = (previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            out.append(packShapeRelative(packed, origin.x(), origin.y(), lead, previousSiblingId.isEmpty()));
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = packed.mask;
        }
        previousSiblingId = child.id();
    }
    return out;
}

QVector<GroupEntry> rootItems(const Project &project)
{
    QVector<GroupEntry> items;
    if (!project.root) {
        return items;
    }
    for (const auto &child : project.root->children) {
        if (!child->visible) {
            continue;
        }
        if (child->kind() == scene::LayerKind::Group) {
            items.push_back({GroupEntry::Group, static_cast<const scene::Group *>(child.get()), nullptr, {}});
        } else if (child->kind() == scene::LayerKind::Shape) {
            items.push_back({GroupEntry::Layer, nullptr, static_cast<const scene::Shape *>(child.get()), {}});
        }
    }
    return items;
}

QByteArray rootCloseBytes(const QVector<GroupEntry> &items)
{
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

QByteArray buildNestedPayload(const Project &project, const SpriteSizeFn &spriteSize)
{
    ExportContext ctx{project, spriteSize};
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
    const QPointF center = shapesOrigin(ctx, allLeaves);

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
    bool previousShapeMask = false;
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
                payload.append('\x00');
                payload.append(QByteArray(std::max(0, previousGroupDepth - 1), '\x01'));
            }
            const ExportShape packed = exportShape(*item.shape);
            const quint8 lead = (previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            payload.append(packShapeRelative(packed, center.x(), center.y(), lead, previousSiblingId.isEmpty()));
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = packed.mask;
        }
        previousSiblingId = item.id();
    }
    payload.append(maybeFlipMaskFlag(rootCloseBytes(items), previousShapeMask));
    return payload;
}

} // namespace fh6
