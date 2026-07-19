#pragma once

#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

enum class PenPointKind {
    Hard,
    Soft,
};

struct PenPoint {
    QPointF position;
    PenPointKind kind = PenPointKind::Soft;
};

struct PenBoundarySegment {
    QPointF start;
    QPointF control;
    QPointF end;
    bool curved = false;
};

struct PenContour {
    QPainterPath path;
    QVector<PenBoundarySegment> segments;
    QVector<QPointF> crossings;
    QString error;

    bool valid() const { return error.isEmpty() && crossings.isEmpty() && !path.isEmpty(); }
};

struct PenPrimitive {
    int shapeId = 0;
    QPainterPath silhouette;
    QVector<QPolygonF> contours;
    QRectF bounds;
    double area = 0.0;
};

struct PenPlacement {
    int shapeId = 0;
    QTransform transform;
    double area = 0.0;
};

struct PenFillRequest {
    QVector<PenPoint> points;
    QVector<PenPrimitive> primitives;
    double boundaryTolerance = 0.1;
};

struct PenFillResult {
    QVector<PenPlacement> placements;
    QPainterPath unfilled;
    double targetArea = 0.0;
    double coveredArea = 0.0;
    double outsideArea = 0.0;
    int shapeLimit = 0;
    bool cancelled = false;
    QString error;
};

PenContour buildPenContour(const QVector<PenPoint> &points, double flatnessTolerance = 0.01);
PenPrimitive buildPenPrimitive(int shapeId, const ShapeGeometry &geometry);
QVector<PenPrimitive> buildPenPrimitiveCatalog(const ShapeGeometryStore &geometry,
                                               int firstShapeId = 101,
                                               int lastShapeId = 130);
PenFillResult fillPenPath(const PenFillRequest &request,
                         const std::function<bool()> &cancelled = {});

} // namespace gui
