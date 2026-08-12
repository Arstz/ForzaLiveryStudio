#pragma once

#include "layer.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

struct SceneRenderEntry {
    const fls::scene::Layer *node = nullptr;
    const fls::scene::Shape *shape = nullptr;
    const fls::scene::GuideLayer *guide = nullptr;
    QString nodeId;
    QString parentGroupId;
    QString sectionGroupId;
    fls::scene::LayerKind kind = fls::scene::LayerKind::Shape;
    QTransform worldTransform;
    QVector<QString> ancestorGroupIds;
    int drawOrder = 0;
};

// Matrix3 is column-vector (m[row][col]); QTransform is row-vector. Element mapping:
// m11=m[0][0], m12=m[1][0], m21=m[0][1], m22=m[1][1], dx=m[0][2], dy=m[1][2].
inline QTransform toQTransform(const fls::Matrix3 &m) {
    return QTransform(m.m[0][0], m.m[1][0], m.m[0][1], m.m[1][1], m.m[0][2], m.m[1][2]);
}

inline QTransform sceneWorldTransform(const fls::scene::Layer &node) {
    return toQTransform(node.worldMatrix());
}

inline QTransform sceneLocalTransform(const fls::scene::Layer &node) {
    return toQTransform(node.transform.matrix());
}

inline QRectF closedPathBounds(const fls::scene::GuideLayer &guide) {
    QRectF bounds;
    bool havePoint = false;
    for (const fls::scene::ClosedPathLoop &loop : guide.closedPaths) {
        for (const fls::scene::ClosedPathPoint &point : loop.points) {
            if (!havePoint) {
                bounds = QRectF(point.position, QSizeF());
                havePoint = true;
            } else {
                bounds = bounds.united(QRectF(point.position, QSizeF()));
            }
        }
    }
    return bounds;
}

inline QSizeF sceneNodeSize(const fls::scene::Layer &node, const ShapeGeometryStore &geometry) {
    if (node.kind() == fls::scene::LayerKind::Shape) {
        const auto &shape = static_cast<const fls::scene::Shape &>(node);
        if (shape.raster) {
            return QSizeF(shape.rasterWidth, shape.rasterHeight);
        }
        return geometry.shapeSize(shape.shapeId);
    }
    if (node.kind() == fls::scene::LayerKind::Guide) {
        const auto &guide = static_cast<const fls::scene::GuideLayer &>(node);
        if (guide.isClosedPath()) {
            return closedPathBounds(guide).size();
        }
        return guide.image != nullptr ? QSizeF(guide.image->width, guide.image->height) : QSizeF();
    }
    return QSizeF();
}

inline QRectF sceneLocalRect(const QSizeF &size) {
    return QRectF(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
}

inline QRectF sceneLocalRect(const fls::scene::Layer &node, const ShapeGeometryStore &geometry) {
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

template <typename LayerType, fls::scene::LayerKind Kind>
QVector<const LayerType *> sceneLeaves(const fls::scene::Group &root) {
    QVector<const LayerType *> leaves;
    std::function<void(const fls::scene::Layer &)> collect = [&](const fls::scene::Layer &node) {
        if (node.kind() == Kind) {
            leaves.push_back(static_cast<const LayerType *>(&node));
            return;
        }
        if (node.kind() != fls::scene::LayerKind::Group) {
            return;
        }
        for (const auto &child : static_cast<const fls::scene::Group &>(node).children) {
            collect(*child);
        }
    };
    for (const auto &child : root.children) {
        collect(*child);
    }

    return leaves;
}

inline QVector<const fls::scene::Shape *> sceneShapeLeaves(const fls::scene::Group &root) {
    return sceneLeaves<fls::scene::Shape, fls::scene::LayerKind::Shape>(root);
}

inline QVector<const fls::scene::GuideLayer *> sceneGuideLeaves(const fls::scene::Group &root) {
    return sceneLeaves<fls::scene::GuideLayer, fls::scene::LayerKind::Guide>(root);
}

} // namespace gui
