#pragma once

#include "layer.h"
#include "shape_geometry_store.h"
#include "pen_fill.h"

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
        return guide.image != nullptr ? QSizeF(guide.image->width, guide.image->height) : QSizeF();
    }
    return QSizeF();
}

inline QRectF sceneLocalRect(const QSizeF &size) {
    return QRectF(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
}

inline QPainterPath contourDataPath(const fls::scene::ContourData &data) {
    auto points = data.points;
    if (data.pathKind == fls::scene::ContourPathKind::Lining && !points.isEmpty()) {
        points.front().kind = points.back().kind = PenPointKind::Hard;
    }
    QPainterPath path = penPath(points,
        data.pathKind == fls::scene::ContourPathKind::Pen && data.closed);

    path.setFillRule(Qt::OddEvenFill);
    for (int index = 0; index < data.cutouts.size(); ++index) {
        path.addPath(penPath(data.cutouts[index],
                            data.cutoutClosed || index + 1 < data.cutouts.size()));
    }

    return path;
}

inline QPainterPath contourFigurePath(const fls::scene::ContourFigure &figure) {
    return contourDataPath(figure.data);
}

template <typename Fn>
void forEachContour(const fls::scene::Layer &node, const Fn &function) {
    if (node.kind() == fls::scene::LayerKind::Contour) {
        const auto &figure = static_cast<const fls::scene::ContourFigure &>(node);
        function(figure.data, node.visible, node.opacity);
    } else if (node.kind() == fls::scene::LayerKind::Mesh) {
        const auto &mesh = static_cast<const fls::scene::Mesh &>(node);
        for (int index = 0; index < mesh.data.contours.size(); ++index) {
            const auto &contour = mesh.data.contours[index];
            function(mesh.data.contourData(index), node.visible && contour.visible, node.opacity * contour.opacity);
        }
    }
}

inline QPainterPath contourGeometryPath(const fls::scene::Layer &node) {
    QPainterPath path;

    forEachContour(node, [&](const fls::scene::ContourData &data, bool visible, double) {
        if (visible) path.addPath(contourDataPath(data));
    });

    return path;
}

inline QColor contourOutlineStrokeColor(const QColor &color) {
    constexpr int kDarkColorThreshold = 128;

    return qGray(color.rgb()) < kDarkColorThreshold ? QColor(Qt::white) : QColor(Qt::black);
}

inline void paintContourOutline(QPainter &painter, const QPainterPath &path, const QColor &color) {
    constexpr double kOutlineWidth = 2.0;
    constexpr double kStrokeWidth = 4.0;
    QPen stroke(contourOutlineStrokeColor(color), kStrokeWidth);
    QPen outline(color, kOutlineWidth);

    stroke.setCosmetic(true);
    outline.setCosmetic(true);
    painter.strokePath(path, stroke);
    painter.strokePath(path, outline);
}

inline QRectF sceneLocalRect(const fls::scene::Layer &node, const ShapeGeometryStore &geometry) {
    if (node.kind() == fls::scene::LayerKind::Mesh) {
        QRectF bounds;
        const auto &mesh = static_cast<const fls::scene::Mesh &>(node);
        for (int index = 0; index < mesh.data.contours.size(); ++index) {
            fls::scene::ContourFigure figure;
            figure.data = mesh.data.contourData(index);
            bounds = bounds.united(sceneLocalRect(figure, geometry));
        }

        return bounds;
    }
    if (node.kind() == fls::scene::LayerKind::Contour) {
        const auto &figure = static_cast<const fls::scene::ContourFigure &>(node);
        QRectF bounds = contourFigurePath(figure).boundingRect();
        if (figure.data.pathKind == fls::scene::ContourPathKind::Lining) {
            double width = figure.data.strokeWidth;
            for (const auto &point : figure.data.points) {
                width = std::max(width, point.strokeWidth);
            }
            double capScale = 1.0;
            if (!figure.data.points.isEmpty()) capScale = std::max({capScale, figure.data.points.front().endCap, figure.data.points.back().endCap});
            const double extent = width * capScale * 0.5;
            bounds.adjust(-extent, -extent, extent, extent);
        }

        return bounds;
    }
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
