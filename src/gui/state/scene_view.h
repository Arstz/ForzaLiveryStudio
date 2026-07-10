#pragma once

// GUI-side view helpers over the unified fh6::scene tree: Matrix3<->QTransform
// conversion, a node's absolute (world) transform, intrinsic node size, centred
// local rects, a world-axis AABB accumulator, and leaf traversal over Project::root.
// Depth-first shape-leaf order is the editor draw order.

#include "layer.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

namespace gui {

struct SceneRenderEntry {
    const fh6::scene::Layer *node = nullptr;
    const fh6::scene::Shape *shape = nullptr;
    const fh6::scene::GuideLayer *guide = nullptr;
    QString nodeId;
    QString parentGroupId;
    QString sectionGroupId;
    fh6::scene::LayerKind kind = fh6::scene::LayerKind::Shape;
    QTransform worldTransform;
    QVector<QString> ancestorGroupIds;
    int drawOrder = 0;
};

// Matrix3 is column-vector (m[row][col]); QTransform is row-vector. Element mapping:
// m11=m[0][0], m12=m[1][0], m21=m[0][1], m22=m[1][1], dx=m[0][2], dy=m[1][2].
inline QTransform toQTransform(const fh6::Matrix3 &m)
{
    return QTransform(m.m[0][0], m.m[1][0], m.m[0][1], m.m[1][1], m.m[0][2], m.m[1][2]);
}

// A node's absolute (world) transform: ancestor group frames composed with its local
// transform. Same field order as the old entryTransform (translate->rotate->shear->scale).
inline QTransform sceneWorldTransform(const fh6::scene::Layer &node)
{
    return toQTransform(node.worldMatrix());
}

// A node's own local transform (parent frame excluded).
inline QTransform sceneLocalTransform(const fh6::scene::Layer &node)
{
    return toQTransform(node.transform.matrix());
}

// Intrinsic local size: raster shapes/guides use their pixel size, vector shapes the
// geometry-store size for their shapeId.
inline QSizeF sceneNodeSize(const fh6::scene::Layer &node, const ShapeGeometryStore &geometry)
{
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        const auto &shape = static_cast<const fh6::scene::Shape &>(node);
        if (shape.raster) {
            return QSizeF(shape.rasterWidth, shape.rasterHeight);
        }
        return geometry.shapeSize(shape.shapeId);
    }
    if (node.kind() == fh6::scene::LayerKind::Guide) {
        const auto &guide = static_cast<const fh6::scene::GuideLayer &>(node);
        return guide.image != nullptr ? QSizeF(guide.image->width, guide.image->height) : QSizeF();
    }
    return QSizeF();
}

// Item-local rectangle centred on the origin (shapes and guides both draw centred on
// their position).
inline QRectF sceneLocalRect(const QSizeF &size)
{
    return QRectF(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
}

inline QRectF sceneLocalRect(const fh6::scene::Layer &node, const ShapeGeometryStore &geometry)
{
    return sceneLocalRect(sceneNodeSize(node, geometry));
}

// World-axis AABB accumulator shared by the project/selection bounds loops.
class BoundsAccumulator {
public:
    void add(const QTransform &transform, const QRectF &localRect)
    {
        const QRectF mapped = transform.mapRect(localRect);
        bounds_ = hasBounds_ ? bounds_.united(mapped) : mapped;
        hasBounds_ = true;
    }

    bool hasBounds() const { return hasBounds_; }
    const QRectF &bounds() const { return bounds_; }

private:
    QRectF bounds_;
    bool hasBounds_ = false;
};

// Shape leaves of the tree in depth-first (== flat layer) order. Guides and groups
// are skipped; a group's children are visited in order.
inline QVector<const fh6::scene::Shape *> sceneShapeLeaves(const fh6::scene::Group &root)
{
    QVector<const fh6::scene::Shape *> leaves;
    struct Walker {
        QVector<const fh6::scene::Shape *> &out;
        void walk(const fh6::scene::Layer &node)
        {
            if (node.kind() == fh6::scene::LayerKind::Group) {
                for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                    walk(*child);
                }
            } else if (node.kind() == fh6::scene::LayerKind::Shape) {
                out.push_back(static_cast<const fh6::scene::Shape *>(&node));
            }
        }
    } walker{leaves};
    for (const auto &child : root.children) {
        walker.walk(*child);
    }
    return leaves;
}

// Guide leaves of the tree in depth-first order.
inline QVector<const fh6::scene::GuideLayer *> sceneGuideLeaves(const fh6::scene::Group &root)
{
    QVector<const fh6::scene::GuideLayer *> leaves;
    struct Walker {
        QVector<const fh6::scene::GuideLayer *> &out;
        void walk(const fh6::scene::Layer &node)
        {
            if (node.kind() == fh6::scene::LayerKind::Group) {
                for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                    walk(*child);
                }
            } else if (node.kind() == fh6::scene::LayerKind::Guide) {
                out.push_back(static_cast<const fh6::scene::GuideLayer *>(&node));
            }
        }
    } walker{leaves};
    for (const auto &child : root.children) {
        walker.walk(*child);
    }
    return leaves;
}

} // namespace gui
