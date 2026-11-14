#pragma once

#include <QImage>
#include <QPointF>
#include <QTransform>

namespace gui {

QPointF snapToColorEdge(const QImage &image, const QTransform &imageToScreen,
                       const QPointF &screenPoint, double radius);

} // namespace gui
