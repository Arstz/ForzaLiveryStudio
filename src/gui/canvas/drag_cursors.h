#pragma once

#include <QColor>
#include <QPixmap>

namespace gui {

QColor dropIndicatorColor();

QPixmap dropAllowedCursorPixmap();
QPixmap dropForbiddenCursorPixmap();

} // namespace gui
