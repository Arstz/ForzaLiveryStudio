#include "clipboard_buffer_widget.h"

#include "main_window.h"

#include "raster_decals.h"

#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace gui {
namespace {

QColor layerColor(const fh6::scene::Shape &layer) {
    return QColor(layer.color[2], layer.color[1], layer.color[0], std::clamp<int>(layer.color[3], 0, 255));
}

QTransform nodeTransform(const fh6::scene::Layer &node) {
    QTransform transform;
    transform.translate(node.transform.x, node.transform.y);
    transform.rotate(node.transform.rotation);
    transform.shear(node.transform.skew, 0.0);
    transform.scale(node.transform.scaleX, node.transform.scaleY);
    return transform;
}

int shapeCount(const ProjectClipboard &clipboard) {
    int count = 0;
    std::function<void(const fh6::scene::Layer &)> walk = [&](const fh6::scene::Layer &node) {
        if (node.kind() == fh6::scene::LayerKind::Shape) {
            ++count;
            return;
        }
        if (node.kind() == fh6::scene::LayerKind::Group) {
            for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                walk(*child);
            }
        }
    };
    for (const auto &node : clipboard.nodes) {
        if (node) {
            walk(*node);
        }
    }
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

    QRectF bounds;
    bool hasBounds = false;
    std::function<void(const fh6::scene::Layer &, const QTransform &)> accumulate =
        [&](const fh6::scene::Layer &node, const QTransform &parentWorld) {
            const QTransform world = nodeTransform(node) * parentWorld;
            if (node.kind() == fh6::scene::LayerKind::Shape) {
                const auto &shape = static_cast<const fh6::scene::Shape &>(node);
                if (!shape.visible) {
                    return;
                }
                const QRectF layerRect = layerBounds(shape, parentWorld);
                bounds = hasBounds ? bounds.united(layerRect) : layerRect;
                hasBounds = true;
                return;
            }
            if (node.kind() == fh6::scene::LayerKind::Group) {
                for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                    accumulate(*child, world);
                }
            }
        };
    for (const auto &node : clipboard_->nodes) {
        if (node) {
            accumulate(*node, QTransform());
        }
    }
    if (!hasBounds || bounds.isEmpty()) {
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
    std::function<void(const fh6::scene::Layer &, const QTransform &)> paintNode =
        [&](const fh6::scene::Layer &node, const QTransform &parentWorld) {
            const QTransform world = nodeTransform(node) * parentWorld;
            if (node.kind() == fh6::scene::LayerKind::Shape) {
                const auto &shape = static_cast<const fh6::scene::Shape &>(node);
                if (shape.visible) {
                    paintLayer(painter, shape, parentWorld);
                }
                return;
            }
            if (node.kind() == fh6::scene::LayerKind::Group) {
                for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                    paintNode(*child, world);
                }
            }
        };
    for (const auto &node : clipboard_->nodes) {
        if (node) {
            paintNode(*node, QTransform());
        }
    }
    painter.restore();
}

QRectF ClipboardBufferWidget::layerBounds(const fh6::scene::Shape &layer, const QTransform &parentWorld) const {
    const QSizeF size = layer.raster ? QSizeF(layer.rasterWidth, layer.rasterHeight)
                                     : geometry_.shapeSize(layer.shapeId);
    const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
    return (nodeTransform(layer) * parentWorld).mapRect(local);
}

void ClipboardBufferWidget::paintLayer(QPainter &painter, const fh6::scene::Shape &layer, const QTransform &parentWorld) const {
    painter.save();
    painter.setTransform(nodeTransform(layer) * parentWorld, true);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(layer.mask ? QPainter::CompositionMode_DestinationOut : QPainter::CompositionMode_SourceOver);

    if (layer.raster) {
        const fh6::RasterDecal decal = fh6::sharedRasterDecals().decal(layer.rasterId);
        if (decal.valid()) {
            const QImage decalImage(reinterpret_cast<const uchar *>(decal.rgba.constData()),
                                    decal.width,
                                    decal.height,
                                    QImage::Format_RGBA8888);
            const QRectF local(-decal.width * 0.5, -decal.height * 0.5, decal.width, decal.height);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setOpacity(static_cast<double>(layer.color[3]) / 255.0);
            // Mirror vertically to match the GL raster orientation under the Y-flip.
            painter.drawImage(local, decalImage.mirrored(false, true));
            painter.restore();
            return;
        }
    }

    const ShapeGeometry *shape = geometryLoaded_ ? geometry_.shape(layer.shapeId) : nullptr;
    if (layer.raster || shape == nullptr || shape->triangles.isEmpty()) {
        const QSizeF size = layer.raster ? QSizeF(layer.rasterWidth, layer.rasterHeight)
                                         : geometry_.shapeSize(layer.shapeId);
        painter.setBrush(layer.mask ? QColor(0, 0, 0, layer.color[3]) : layerColor(layer));
        painter.drawRect(QRectF(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height()));
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
