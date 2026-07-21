#pragma once

#include "pen_fill.h"

#include <QColor>
#include <QPointF>
#include <QString>
#include <QVector>

#include <limits>
#include <optional>

namespace gui {

struct PenCurveHit {
    QPointF worldPosition;
    int insertIndex = -1;
    double screenDistance = std::numeric_limits<double>::max();

    bool valid() const { return insertIndex >= 0; }
};

struct PathInteraction {
    QVector<PenPoint> points;
    std::optional<QColor> fillColor;
    QVector<QPointF> crossings;
    QPointF hoverWorld;
    QPointF dragOffsetWorld;
    PenCurveHit hoverCurve;
    QString error;
    QString fillMessage;
    int hoverPoint = -1;
    int dragPoint = -1;
    bool closed = false;
    bool fillRunning = false;

    void resetHover() {
        hoverCurve = {};
        dragOffsetWorld = {};
        hoverPoint = -1;
        dragPoint = -1;
    }

    void reset() {
        points.clear();
        crossings.clear();
        fillColor.reset();
        error.clear();
        fillMessage.clear();
        resetHover();
        closed = false;
        fillRunning = false;
    }
};

} // namespace gui
