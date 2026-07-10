#include "drag_cursors.h"

#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

namespace gui {
namespace {

constexpr int CursorExtent = 11;
const QColor DropIndicatorLineColor(244, 197, 66);
const QColor AllowedBorderColor(30, 30, 30);
const QColor AllowedFillColor(245, 245, 245);
const QColor AllowedGlyphColor(70, 70, 70);
const QColor ForbiddenColor(180, 25, 25);
const QColor ForbiddenFillColor(255, 255, 255, 230);

} // namespace

QColor dropIndicatorColor()
{
    return DropIndicatorLineColor;
}

QPixmap dropAllowedCursorPixmap()
{
    QPixmap pixmap(CursorExtent, CursorExtent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(AllowedBorderColor, 1));
    painter.setBrush(AllowedFillColor);
    painter.drawRoundedRect(QRectF(2.0, 2.0, 12.0, 9.0), 2.0, 2.0);
    painter.setPen(QPen(AllowedGlyphColor, 2));
    painter.drawLine(QPointF(5.0, 5.0), QPointF(11.0, 5.0));
    painter.drawLine(QPointF(5.0, 8.0), QPointF(11.0, 8.0));
    return pixmap;
}

QPixmap dropForbiddenCursorPixmap()
{
    QPixmap pixmap(CursorExtent, CursorExtent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(ForbiddenColor, 2));
    painter.setBrush(ForbiddenFillColor);
    painter.drawEllipse(QRectF(2.0, 2.0, 12.0, 12.0));
    painter.drawLine(QPointF(5.0, 11.0), QPointF(11.0, 5.0));
    return pixmap;
}

} // namespace gui
