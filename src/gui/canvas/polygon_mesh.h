#pragma once

#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

struct PolygonContour {
    QPolygonF polygon;
    QPainterPath path;
    QVector<QPointF> crossings;
    QString error;

    bool valid() const { return error.isEmpty() && crossings.isEmpty() && polygon.size() >= 3; }
};

struct PolygonMeshSources {
    QPolygonF square;
    QPolygonF triangle;

    bool valid() const { return square.size() == 4 && triangle.size() == 3; }
};

struct PolygonMeshPlacement {
    int shapeId = 0;
    QTransform transform;
};

struct PolygonMeshRequest {
    QVector<QPointF> points;
    PolygonMeshSources sources;
    bool mergeSquares = true;
};

struct PolygonMeshResult {
    QVector<PolygonMeshPlacement> placements;
    QPainterPath contour;
    bool cancelled = false;
    QString error;
};

// tolerance is used only to remove duplicate and visually redundant input points;
// topology checks use an independent floating-point precision threshold.
PolygonContour buildPolygonContour(const QVector<QPointF> &points,
                                   double tolerance = 1e-7);
PolygonMeshSources buildPolygonMeshSources(const ShapeGeometryStore &geometry);
PolygonMeshResult meshPolygon(const PolygonMeshRequest &request,
                              const std::function<bool()> &cancelled = {});

} // namespace gui
