#include "clipboard_buffer_widget.h"

#include "main_window.h"

#include <QPainter>
#include <QPaintEvent>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

QColor layerColor(const fh6::ShapeLayer &layer)
{
    return QColor(layer.color[2], layer.color[1], layer.color[0], std::clamp<int>(layer.color[3], 0, 255));
}

QTransform layerTransform(const fh6::ShapeLayer &layer)
{
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

} // namespace

ClipboardBufferWidget::ClipboardBufferWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(180, 150);
    geometryLoaded_ = geometry_.loadDefault();
}

void ClipboardBufferWidget::setClipboard(const ProjectClipboard *clipboard)
{
    clipboard_ = clipboard;
    update();
}

void ClipboardBufferWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(24, 24, 24));

    if (clipboard_ == nullptr || clipboard_->layers.isEmpty()) {
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Buffer empty"));
        return;
    }

    const QString countText = QString::number(clipboard_->layers.size());
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
    for (const fh6::ShapeLayer &layer : clipboard_->layers) {
        if (!layer.visible) {
            continue;
        }
        const QRectF layerRect = layerBounds(layer);
        bounds = hasBounds ? bounds.united(layerRect) : layerRect;
        hasBounds = true;
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
    for (const fh6::ShapeLayer &layer : clipboard_->layers) {
        if (!layer.visible) {
            continue;
        }
        paintLayer(painter, layer);
    }
    painter.restore();
}

QRectF ClipboardBufferWidget::layerBounds(const fh6::ShapeLayer &layer) const
{
    const QSizeF size = geometry_.shapeSize(layer.shapeId);
    const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
    return layerTransform(layer).mapRect(local);
}

void ClipboardBufferWidget::paintLayer(QPainter &painter, const fh6::ShapeLayer &layer) const
{
    painter.save();
    painter.setTransform(layerTransform(layer), true);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(layer.mask ? QPainter::CompositionMode_DestinationOut : QPainter::CompositionMode_SourceOver);

    const ShapeGeometry *shape = geometryLoaded_ ? geometry_.shape(layer.shapeId) : nullptr;
    if (shape == nullptr || shape->triangles.isEmpty()) {
        const QSizeF size = geometry_.shapeSize(layer.shapeId);
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
