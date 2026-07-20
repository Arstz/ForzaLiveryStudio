#pragma once

#include "pen_fill.h"
#include "polygon_mesh.h"
#include "region_extract.h"

#include <QtGui>

#include <array>
#include <cstdint>
#include <functional>

namespace gui {

// One filled colour region: the affine primitive placements the Pen fitter
// produced, plus its source colour and area. Results retain extraction order.
struct RegionFillLayer {
    QColor color;
    double area = 0.0;
    QVector<PenPlacement> placements;
};

// A single fill shape ready to be inserted into the project scene: shape id,
// world-space transform, and RGBA colour.
struct GeneratedRegionShape {
    int shapeId = 0;
    QTransform transform;
    std::array<std::uint8_t, 4> color = {255, 255, 255, 255};
};

// One generated scene group per successfully filled region.
struct GeneratedRegionGroup {
    QVector<GeneratedRegionShape> shapes;
};

struct RegionFillBatchRequest {
    RegionExtractionResult regions;
    QVector<PenPrimitive> primitives;
    PolygonMeshSources meshSources;
    QString overlayGuideId;
    quint64 overlayGeneration = 0;
};

struct RegionFillBatchResult {
    QVector<RegionFillLayer> fills;
    QHash<int, QPainterPath> silhouettes;
    QString overlayGuideId;
    quint64 overlayGeneration = 0;
    QString summary;
    QString error;
    bool cancelled = false;
    int completedRegions = 0;
    int totalRegions = 0;
};

RegionFillBatchResult computeRegionFills(
    const RegionFillBatchRequest &request,
    const std::function<void(int completed, int total)> &progress = {},
    const std::function<bool()> &cancelled = {});

struct RegionPenConversionOptions {
    // Maximum extra boundary deviation introduced by removing Potrace cubic
    // junctions, in the outline's coordinate system (image pixels for traces).
    double mergeTolerance = 0.5;
    double closureTolerance = 1e-6;
    int maxOptimizedPointCount = 128;
};

struct RegionPenConversionResult {
    QVector<PenPoint> points;
    QString error;
    int originalPointCount = 0;
    int removedHardPoints = 0;
    bool optimizationSkipped = false;
    double baselineDeviation = 0.0;
    double maximumDeviation = 0.0;

    bool valid() const { return error.isEmpty() && points.size() >= 3; }
};

struct RegionFillContourStats {
    int originalPointCount = 0;
    int optimizedPointCount = 0;
    int flattenedPointCount = 0;
    int removedHardPoints = 0;
    bool optimizationSkipped = false;
};

RegionPenConversionResult regionOutlineToPenPoints(
    const QPainterPath &outline,
    const RegionPenConversionOptions &options = {});

// Fill one region outline with affine primitives via the Pen fitter. When
// requested, optimizedContour is populated before fitting so callers can mesh
// that exact contour if the time budget interrupts the Primitive search;
// contourStats reports its conversion and flattening point counts, while
// optimizedPenPoints optionally returns the optimized Hard/Soft point sequence.
PenFillResult fillRegionOutline(const QPainterPath &outline,
                                const QVector<PenPrimitive> &primitives,
                                double boundaryTolerance,
                                const std::function<bool()> &cancelled = {},
                                QPolygonF *optimizedContour = nullptr,
                                RegionFillContourStats *contourStats = nullptr,
                                QVector<PenPoint> *optimizedPenPoints = nullptr);

// Ramer-Douglas-Peucker simplification of a closed polygon: drops vertices that
// lie within `epsilon` of the chord between kept neighbours. Fewer vertices ->
// fewer triangles/shapes. Returns the input unchanged when epsilon <= 0.
QPolygonF simplifyClosedPolygon(const QPolygonF &polygon, double epsilon);

// Corridor-constrained closed-polygon RDP for the occlusion model. Behaves like
// simplifyClosedPolygon for chords within `epsilon` (visible edges stay tight),
// but ALSO accepts a chord that deviates further when `chordInFreeSpace(a, b)`
// reports it runs entirely through free (occluded) space -- so the boundary
// bridges notches a higher layer will repaint, collapsing occluded detail to a
// straight edge. `chordInFreeSpace` never fires on chords over visible exterior,
// so no spill is introduced.
QPolygonF simplifyClosedPolygonCorridor(
    const QPolygonF &polygon, double epsilon,
    const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace);

// Outer (largest-area) contour of an outline, flattened to a simple polygon with
// any closing duplicate removed. Empty when the outline has no fillable contour.
QPolygonF regionOuterContour(const QPainterPath &outline);

// Triangulate a simple closed polygon into Squares/Triangles (no curve caps).
PenFillResult fillPolygonMesh(const QPolygonF &polygon,
                              const PolygonMeshSources &sources,
                              const std::function<bool()> &cancelled = {});

// Robust fallback: directly triangulate the flattened outer contour into
// Squares/Triangles, bypassing the Pen curve-cap machinery. potrace's outer
// contour is a simple closed curve, so this succeeds on the complex/thin
// regions where the lossy Pen-point conversion self-intersects. The flattened
// contour is RDP-simplified with `simplifyEpsilon` (image-pixel units) first to
// keep the triangle count down. Produces more (but always valid) placements; no
// circle caps.
PenFillResult fillRegionOutlineMesh(const QPainterPath &outline,
                                    const PolygonMeshSources &sources,
                                    double simplifyEpsilon,
                                    const std::function<bool()> &cancelled = {});

} // namespace gui
