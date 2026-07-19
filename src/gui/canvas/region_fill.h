#pragma once

#include "pen_fill.h"
#include "polygon_mesh.h"

#include <QtGui>

#include <array>
#include <cstdint>
#include <functional>

namespace gui {

// One filled colour region: the affine primitive placements the Pen fitter
// produced, plus the fill colour and area (area drives largest-first draw order
// so nested regions land on top).
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

// Convert the outer contour of a traced region outline into Pen points. The
// potrace trace already encodes hardpoints: a LineTo element is a corner (Hard),
// a CurveTo (cubic) becomes a Soft quadratic control plus a Hard endpoint, so
// the curve-capper still engages on rounded boundaries. Holes are dropped - the
// largest-area subpath is used. Returns fewer than 3 points on failure.
QVector<PenPoint> regionOutlineToPenPoints(const QPainterPath &outline);

// Fill one region outline with affine primitives via the Pen fitter.
PenFillResult fillRegionOutline(const QPainterPath &outline,
                                const QVector<PenPrimitive> &primitives,
                                double boundaryTolerance,
                                const std::function<bool()> &cancelled = {});

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
