#pragma once

#include <QtCore>

namespace gui {

struct FillBoundarySegment {
    QPointF start;
    QPointF control;
    QPointF end;
    bool curved = false;
};

} // namespace gui
