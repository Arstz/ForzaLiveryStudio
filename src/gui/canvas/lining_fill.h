#pragma once

#include "pen_fill.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

struct LiningPath {
    QPainterPath centerline;
    QVector<PenBoundarySegment> segments;
    QString error;

    bool valid() const { return error.isEmpty() && !centerline.isEmpty() && !segments.isEmpty(); }
};

struct LiningFillRequest {
    QVector<PenPoint> points;
    QVector<PenPrimitive> primitives;
    double width = 8.0;
};

LiningPath buildLiningPath(const QVector<PenPoint> &points);
QPainterPath buildLiningRibbon(const QPainterPath &centerline, double width);
QVector<PenPrimitive> buildLiningPrimitiveCatalog(const ShapeGeometryStore &geometry);
PenFillResult fillLiningPath(const LiningFillRequest &request,
                             const std::function<bool()> &cancelled = {});

} // namespace gui
