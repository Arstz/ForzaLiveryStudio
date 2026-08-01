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
    int loopIndex = -1;
    double screenDistance = std::numeric_limits<double>::max();

    bool valid() const { return insertIndex >= 0; }
};

struct PathInteractionState {
    QVector<PenPoint> points;
    QVector<QVector<PenPoint>> cutouts;
    std::optional<QColor> fillColor;
    bool closed = false;
    bool cutoutClosed = true;
    bool fillMask = false;
};

struct PathInteraction {
    QVector<PenPoint> points;
    QVector<QVector<PenPoint>> cutouts;
    std::optional<QColor> fillColor;
    QVector<QPointF> crossings;
    QPointF hoverWorld;
    QPointF dragOffsetWorld;
    PenCurveHit hoverCurve;
    QString error;
    QString fillMessage;
    int hoverPoint = -1;
    int hoverLoop = -1;
    int dragPoint = -1;
    int dragLoop = -1;
    int activeCutout = -1;
    bool closed = false;
    bool cutoutClosed = true;
    bool fillMask = false;
    bool fillRunning = false;
    QVector<PathInteractionState> undoStack;
    QVector<PathInteractionState> redoStack;
    std::optional<PathInteractionState> pendingEdit;

    void resetHover() {
        hoverCurve = {};
        dragOffsetWorld = {};
        hoverPoint = -1;
        hoverLoop = -1;
        dragPoint = -1;
        dragLoop = -1;
    }

    void reset() {
        points.clear();
        cutouts.clear();
        crossings.clear();
        fillColor.reset();
        error.clear();
        fillMessage.clear();
        resetHover();
        closed = false;
        cutoutClosed = true;
        activeCutout = -1;
        fillMask = false;
        fillRunning = false;
        undoStack.clear();
        redoStack.clear();
        pendingEdit.reset();
    }
};

} // namespace gui
