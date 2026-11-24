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

using PathInteractionState = fls::scene::ContourData;

struct PathInteraction {
    explicit PathInteraction(fls::scene::ContourPathKind kind = fls::scene::ContourPathKind::Pen)
        : pathKind(kind) {
        if (pathKind == fls::scene::ContourPathKind::Lining) {
            fillColor = fls::scene::kDefaultLiningFillColor;
        }
    }

    QVector<PenPoint> points;
    QVector<QVector<PenPoint>> cutouts;
    std::optional<QColor> fillColor;
    QVector<QPointF> crossings;
    QPointF hoverWorld;
    QPointF dragOffsetWorld;
    std::optional<QPointF> anchorPressScreen;
    PenCurveHit hoverCurve;
    QString error;
    QString fillMessage;
    int hoverPoint = -1;
    int hoverLoop = -1;
    int hoverHandle = 0;
    int dragPoint = -1;
    int dragLoop = -1;
    int dragHandle = 0;
    int activeCutout = -1;
    bool closed = false;
    bool cutoutClosed = true;
    fls::scene::ContourPathKind pathKind = fls::scene::ContourPathKind::Pen;
    double strokeWidth = fls::scene::kDefaultLiningWidth;
    bool fillMask = false;
    bool fillRunning = false;
    QVector<PathInteractionState> undoStack;
    QVector<PathInteractionState> redoStack;
    std::optional<PathInteractionState> pendingEdit;

    void resetHover() {
        hoverCurve = {};
        dragOffsetWorld = {};
        anchorPressScreen.reset();
        hoverPoint = -1;
        hoverLoop = -1;
        dragPoint = -1;
        dragLoop = -1;
        hoverHandle = dragHandle = 0;
    }

    void reset() {
        points.clear();
        cutouts.clear();
        crossings.clear();
        fillColor = pathKind == fls::scene::ContourPathKind::Lining
            ? std::optional<QColor>(fls::scene::kDefaultLiningFillColor) : std::nullopt;
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
