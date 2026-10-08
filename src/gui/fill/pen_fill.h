#pragma once

#include "fill_contour.h"
#include "contour_data.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

using PenPointKind = fls::scene::ContourPointKind;
using PenPoint = fls::scene::ContourPoint;

enum class PenLoopKind {
    Outer,
    Cutout,
};

struct PenLoop {
    QVector<PenPoint> points;
    PenLoopKind kind = PenLoopKind::Outer;
};

using PenBoundarySegment = FillBoundarySegment;

QVector<PenBoundarySegment> penSegments(const QVector<PenPoint> &points, bool closed = true);
void splitCurvedHardSpans(QVector<PenPoint> &points);
QPainterPath penPath(const QVector<PenPoint> &points, bool closed = true);
void materializePenHandles(QVector<PenPoint> &points, bool closed = true);
void transformPenPoint(PenPoint &point, const QTransform &transform);
void insertPenAnchor(QVector<PenPoint> &points, int insertIndex, const QPointF &nearPoint);

struct PenContourLoop {
    QPainterPath path;
    QVector<PenBoundarySegment> segments;
    PenLoopKind kind = PenLoopKind::Outer;
};

struct PenContour {
    QPainterPath path;
    QVector<PenBoundarySegment> segments;
    QVector<PenContourLoop> loops;
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
    bool coreEllipse = false;
    QVector<int> ownedFeatureIds;
    double exposedContourArc = 0.0;
};

struct PenFillRequest {
    QVector<PenPoint> points;
    QVector<PenLoop> loops;
    QVector<PenPrimitive> primitives;
    double boundaryTolerance = 0.1;
    bool discardNegligiblePlacements = true;
};

struct PenFillResult {
    QVector<PenPlacement> placements;
    QPainterPath unfilled;
    double targetArea = 0.0;
    double coveredArea = 0.0;
    double outsideArea = 0.0;
    int shapeLimit = 0;
    bool cancelled = false;
    bool timedOut = false;
    QString error;
};

PenContour buildPenContour(const QVector<PenPoint> &points, double flatnessTolerance = 0.01);
PenContour buildPenContour(const QVector<PenLoop> &loops, double flatnessTolerance = 0.01);
PenPrimitive buildPenPrimitive(int shapeId, const ShapeGeometry &geometry);
QVector<PenPrimitive> buildPenPrimitiveCatalog(const ShapeGeometryStore &geometry,
                                               int firstShapeId = 101,
                                               int lastShapeId = 0x084b);
PenFillResult fillPenPath(const PenFillRequest &request,
                         const std::function<bool()> &cancelled = {});

} // namespace gui
