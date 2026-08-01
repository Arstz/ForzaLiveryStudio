#include "drag_cursors.h"

#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

namespace gui {
namespace {

constexpr int kCursorExtent = 16;
const QColor kDropIndicatorLineColor(244, 197, 66);
const QColor kAllowedBorderColor(30, 30, 30);
const QColor kAllowedFillColor(245, 245, 245);
const QColor kAllowedGlyphColor(70, 70, 70);
const QColor kForbiddenColor(180, 25, 25);
const QColor kForbiddenFillColor(255, 255, 255, 230);

} // namespace

QColor dropIndicatorColor() {
    return kDropIndicatorLineColor;
}

QPixmap dropAllowedCursorPixmap() {
    QPixmap pixmap(kCursorExtent, kCursorExtent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kAllowedBorderColor, 1));
    painter.setBrush(kAllowedFillColor);
    painter.drawRoundedRect(QRectF(2.0, 2.0, 12.0, 9.0), 2.0, 2.0);
    painter.setPen(QPen(kAllowedGlyphColor, 2));
    painter.drawLine(QPointF(5.0, 5.0), QPointF(11.0, 5.0));
    painter.drawLine(QPointF(5.0, 8.0), QPointF(11.0, 8.0));
    return pixmap;
}

QPixmap dropForbiddenCursorPixmap() {
    QPixmap pixmap(kCursorExtent, kCursorExtent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kForbiddenColor, 2));
    painter.setBrush(kForbiddenFillColor);
    painter.drawEllipse(QRectF(2.0, 2.0, 12.0, 12.0));
    painter.drawLine(QPointF(5.0, 11.0), QPointF(11.0, 5.0));
    return pixmap;
}

} // namespace gui
