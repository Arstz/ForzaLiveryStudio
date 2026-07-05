#pragma once

#include <QColor>
#include <QPixmap>

namespace gui {

// Accent color for the layer-tree drop indicator line.
QColor dropIndicatorColor();

// 16x16 cursor overlays shown while dragging layer rows.
QPixmap dropAllowedCursorPixmap();
QPixmap dropForbiddenCursorPixmap();

} // namespace gui
