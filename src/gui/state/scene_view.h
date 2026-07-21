#pragma once

#include "layer.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <functional>

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
inline QTransform toQTransform(const fh6::Matrix3 &m) {
    return QTransform(m.m[0][0], m.m[1][0], m.m[0][1], m.m[1][1], m.m[0][2], m.m[1][2]);
}

inline QTransform sceneWorldTransform(const fh6::scene::Layer &node) {
    return toQTransform(node.worldMatrix());
}

inline QTransform sceneLocalTransform(const fh6::scene::Layer &node) {
    return toQTransform(node.transform.matrix());
}

inline QSizeF sceneNodeSize(const fh6::scene::Layer &node, const ShapeGeometryStore &geometry) {
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

inline QRectF sceneLocalRect(const QSizeF &size) {
    return QRectF(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
}

inline QRectF sceneLocalRect(const fh6::scene::Layer &node, const ShapeGeometryStore &geometry) {
    return sceneLocalRect(sceneNodeSize(node, geometry));
}

class BoundsAccumulator {
public:
    void add(const QTransform &transform, const QRectF &localRect) {
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

template <typename LayerType, fh6::scene::LayerKind Kind>
QVector<const LayerType *> sceneLeaves(const fh6::scene::Group &root) {
    QVector<const LayerType *> leaves;
    std::function<void(const fh6::scene::Layer &)> collect = [&](const fh6::scene::Layer &node) {
        if (node.kind() == Kind) {
            leaves.push_back(static_cast<const LayerType *>(&node));
            return;
        }
        if (node.kind() != fh6::scene::LayerKind::Group) {
            return;
        }
        for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
            collect(*child);
        }
    };
    for (const auto &child : root.children) {
        collect(*child);
    }

    return leaves;
}

inline QVector<const fh6::scene::Shape *> sceneShapeLeaves(const fh6::scene::Group &root) {
    return sceneLeaves<fh6::scene::Shape, fh6::scene::LayerKind::Shape>(root);
}

inline QVector<const fh6::scene::GuideLayer *> sceneGuideLeaves(const fh6::scene::Group &root) {
    return sceneLeaves<fh6::scene::GuideLayer, fh6::scene::LayerKind::Guide>(root);
}

} // namespace gui
