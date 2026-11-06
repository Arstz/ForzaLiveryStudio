#pragma once

#include <QImage>
#include <QPainterPath>

namespace gui {

struct ShapeGeometry;

bool vectorHasSoftAlpha(const ShapeGeometry &geometry);
bool rasterHasSoftAlpha(const QImage &image);
QPainterPath vectorSelectionBody(const ShapeGeometry &geometry);
QPainterPath rasterSelectionBody(const QImage &image);

} // namespace gui
