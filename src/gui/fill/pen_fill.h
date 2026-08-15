#pragma once

#include "differential_cover.h"
#include "fill_contour.h"
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

enum class PenLoopKind {
    Outer,
    Cutout,
};

struct PenLoop {
    QVector<PenPoint> points;
    PenLoopKind kind = PenLoopKind::Outer;
};

using PenBoundarySegment = FillBoundarySegment;

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
    QVector<PenPoint> curvePoints;
    QVector<PenBoundarySegment> curveSegments;
    QRectF bounds;
    double area = 0.0;
};

enum class PenBoundaryFitKind {
    None,
    OutwardSpan,
    InwardSpan,
    InwardSegment,
    StraightTriangle,
    StraightSquare,
};

struct PenPlacement {
    int shapeId = 0;
    QTransform transform;
    double area = 0.0;
    bool coreEllipse = false;
    QVector<int> ownedFeatureIds;
    double exposedContourArc = 0.0;
    double exposedCurveArc = 0.0;
    int exposedContourSegments = 0;
    int exposedCurveSegments = 0;
    int boundaryLoopIndex = -1;
    int boundaryFirstSegment = -1;
    int boundaryLastSegment = -1;
    QPointF boundaryStart;
    QPointF boundaryEnd;
    PenBoundaryFitKind boundaryFitKind = PenBoundaryFitKind::None;
};

struct PenFillRequest {
    QVector<PenPoint> points;
    QVector<PenLoop> loops;
    QVector<PenPrimitive> primitives;
    QVector<cover::ShapeMesh> curveMeshes;
    QPainterPath targetPath;
    QPainterPath legalEnvelope;
    double boundaryTolerance = 0.1;
    int shapeLimit = 0;
    int curveTimeBudgetMs = 0;
    bool discardNegligiblePlacements = true;
    bool useGpu = false;
};

struct PenFillProgress {
    QString phase;
    int completed = 0;
    int total = 0;
    int placementCount = 0;
    double targetArea = 0.0;
    double coveredArea = 0.0;
};

using PenFillProgressCallback = std::function<void(const PenFillProgress &)>;

struct PenCurveLoopDiagnostics {
    int chordedCurveSegments = 0;
    int supportedCurveSegments = 0;
};

struct PenFillResult {
    QVector<PenPlacement> placements;
    QVector<PenCurveLoopDiagnostics> curveLoopDiagnostics;
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
                         const std::function<bool()> &cancelled = {},
                         const PenFillProgressCallback &progress = {});

} // namespace gui
