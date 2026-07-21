#include "clipboard_buffer_widget.h"

#include "editor_state.h"
#include "raster_decals.h"
#include "scene_view.h"

#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

QColor layerColor(const fh6::scene::Shape &layer) {
    return QColor(layer.color[2], layer.color[1], layer.color[0], std::clamp<int>(layer.color[3], 0, 255));
}

template <typename Fn>
void forEachClipboardShape(const ProjectClipboard &clipboard, const Fn &fn) {
    auto walk = [&](auto &self, const fh6::scene::Layer &node, const QTransform &parentWorld) -> void {
        const QTransform world = sceneLocalTransform(node) * parentWorld;
        if (node.kind() == fh6::scene::LayerKind::Shape) {
            fn(static_cast<const fh6::scene::Shape &>(node), world);
            return;
        }
        if (node.kind() == fh6::scene::LayerKind::Group) {
            for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                self(self, *child, world);
            }
        }
    };
    for (const auto &node : clipboard.nodes) {
        if (node) {
            walk(walk, *node, QTransform());
        }
    }

}

int shapeCount(const ProjectClipboard &clipboard) {
    int count = 0;
    forEachClipboardShape(clipboard, [&](const fh6::scene::Shape &, const QTransform &) {
        ++count;
    });

    return count;
}

} // namespace

ClipboardBufferWidget::ClipboardBufferWidget(QWidget *parent)
    : QWidget(parent) {
    setMinimumSize(180, 150);
    geometryLoaded_ = geometry_.loadDefault();
}

void ClipboardBufferWidget::setClipboard(const ProjectClipboard *clipboard) {
    clipboard_ = clipboard;
    update();
}

void ClipboardBufferWidget::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(24, 24, 24));

    if (clipboard_ == nullptr || clipboard_->nodes.empty()) {
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Buffer empty"));
        return;
    }

    const QString countText = QString::number(shapeCount(*clipboard_));
    painter.setPen(QColor(240, 240, 240));
    painter.drawText(QRect(width() - 76, 8, 66, 22), Qt::AlignRight | Qt::AlignVCenter, countText);

    const QRect previewRect = rect().adjusted(12, 34, -12, -12);
    if (previewRect.width() <= 0 || previewRect.height() <= 0) {
        return;
    }
    painter.fillRect(previewRect, QColor(16, 16, 16));
    painter.setPen(QPen(QColor(60, 60, 60), 1));
    painter.drawRect(previewRect.adjusted(0, 0, -1, -1));

    BoundsAccumulator boundsAccumulator;
    forEachClipboardShape(*clipboard_, [&](const fh6::scene::Shape &shape, const QTransform &world) {
        if (shape.visible) {
            boundsAccumulator.add(world, sceneLocalRect(sceneNodeSize(shape, geometry_)));
        }
    });
    QRectF bounds = boundsAccumulator.hasBounds() ? boundsAccumulator.bounds() : QRectF();
    if (bounds.isEmpty()) {
        bounds = QRectF(-64.0, -64.0, 128.0, 128.0);
    }

    painter.save();
    painter.setClipRect(previewRect);
    painter.translate(previewRect.center());
    const double scale = std::min(previewRect.width() / std::max(bounds.width(), 1.0),
                                  previewRect.height() / std::max(bounds.height(), 1.0)) * 0.86;
    painter.scale(scale, -scale);
    painter.translate(-bounds.center());
    painter.setPen(Qt::NoPen);
    forEachClipboardShape(*clipboard_, [&](const fh6::scene::Shape &shape, const QTransform &world) {
        if (shape.visible) {
            paintLayer(painter, shape, world);
        }
    });
    painter.restore();
}

void ClipboardBufferWidget::paintLayer(QPainter &painter,
                                       const fh6::scene::Shape &layer,
                                       const QTransform &world) const {
    painter.save();
    painter.setTransform(world, true);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(layer.mask ? QPainter::CompositionMode_DestinationOut : QPainter::CompositionMode_SourceOver);

    if (layer.raster) {
        const fh6::RasterDecal decal = fh6::sharedRasterDecals().decal(layer.rasterId);
        if (decal.valid()) {
            const QImage decalImage(reinterpret_cast<const uchar *>(decal.rgba.constData()),
                                    decal.width,
                                    decal.height,
                                    QImage::Format_RGBA8888);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setOpacity(static_cast<double>(layer.color[3]) / 255.0);
            // Mirror vertically to match the GL raster orientation under the Y-flip.
            painter.drawImage(sceneLocalRect(QSizeF(decal.width, decal.height)), decalImage.mirrored(false, true));
            painter.restore();
            return;
        }
    }

    const ShapeGeometry *shape = geometryLoaded_ ? geometry_.shape(layer.shapeId) : nullptr;
    if (layer.raster || shape == nullptr || shape->triangles.isEmpty()) {
        const QSizeF size = layer.raster ? QSizeF(layer.rasterWidth, layer.rasterHeight)
                                         : geometry_.shapeSize(layer.shapeId);
        painter.setBrush(layer.mask ? QColor(0, 0, 0, layer.color[3]) : layerColor(layer));
        painter.drawRect(sceneLocalRect(size));
        painter.restore();
        return;
    }

    painter.setRenderHint(QPainter::Antialiasing, false);
    for (const ShapeTriangle &triangle : shape->triangles) {
        const double alpha = std::clamp((triangle.alpha0 + triangle.alpha1 + triangle.alpha2) / 3.0, 0.0, 1.0);
        QColor color = layer.mask ? QColor(0, 0, 0, std::clamp(static_cast<int>(std::round(alpha * layer.color[3])), 0, 255))
                                  : layerColor(layer);
        if (!layer.mask) {
            color.setAlpha(std::clamp(static_cast<int>(std::round(layer.color[3] * alpha)), 0, 255));
        }
        painter.setBrush(color);
        const QPointF points[3] = {triangle.p0, triangle.p1, triangle.p2};
        painter.drawPolygon(points, 3);
    }
    painter.restore();
}

} // namespace gui
