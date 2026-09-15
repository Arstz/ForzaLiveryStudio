#pragma once

#include "pen_fill.h"
#include "svg_vector_objects.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

// One vector object to be covered with shapes. The outline is in the source
// image's pixel space (y down) and may contain several islands, each with
// interior cutouts.
struct ImageImportFillUnit {
    QPainterPath outline;
    QColor color;
};

// How a raster image becomes import units: one unit per colour region, with
// pixels below the alpha threshold left empty.
struct RasterImportOptions {
    double alphaThreshold = 0.5;      // fraction of full alpha a pixel needs to count
    int maximumColors = 8;            // palette size the regions are quantised to
    int minimumRegionArea = 128;      // pixels; smaller regions merge into a neighbour
    int speckleSize = 2;              // tracer speckle suppression, in pixels
    double traceSmoothing = 1.0;      // tracer corner smoothing, 0 keeps every corner
    int maximumDimension = 1024;      // longest side processed, 0 processes full size
    bool separateThinLines = false;   // give thin high-contrast strokes their own regions
    // Pixels each region grows under the neighbours drawn on top of it before
    // tracing, so adjacent outlines overlap instead of meeting along a traced
    // edge that the two sides smooth differently and leave a hairline gap
    // between. Growth only goes under a region that is drawn later, never over
    // one drawn earlier and never into empty pixels, so nothing visible changes
    // size; 0 traces each region exactly as extracted.
    int neighbourOverlap = 2;
};

struct RasterImportUnits {
    QVector<ImageImportFillUnit> units;  // source pixel space, largest area first
    QSize processedSize;                 // size the regions were extracted at
    int thinLineCount = 0;               // units that came from thin-line regions
    QString error;

    bool valid() const { return error.isEmpty() && !units.isEmpty(); }
};

struct ImageImportFillRequest {
    QVector<ImageImportFillUnit> units;
    QVector<PenPrimitive> primitives;
    double boundaryTolerance = 0.1;
    // Outline simplification tolerance in pixels. Zero keeps the source curves
    // and only samples an outline the Pen rules reject; a positive value
    // samples every outline at that tolerance, trading fidelity for fewer
    // shapes.
    double outlineSimplification = 0.0;
    qint64 componentBudgetMs = 3000;
    qint64 componentBudgetMsPerPoint = 40;
    int shapeLimitPerPoint = 6;
};

// Pen loops for one connected component: the outer loop first, then cutouts.
struct ImageImportPenLoops {
    QVector<PenLoop> loops;
    QString via;
    QString error;

    bool valid() const { return error.isEmpty() && !loops.isEmpty(); }
};

// Pen loops for every connected component of one object.
struct ImageImportUnitLoops {
    QVector<QVector<PenLoop>> components;
    QVector<QPainterPath> outlines;  // the component each entry of components came from
    QString via;
    QString error;

    bool valid() const { return error.isEmpty() && !components.isEmpty(); }
};

// The outcome for one connected component of one object.
struct ImageImportComponentResult {
    QString error;
    QString via;  // set when the component only filled after its outline was resampled
    qint64 elapsedMs = 0;
    int loopCount = 0;
    int pointCount = 0;
    int placementCount = 0;
    bool timedOut = false;

    bool filled() const { return error.isEmpty() && placementCount > 0; }
};

struct ImageImportFilledUnit {
    QColor color;
    QVector<PenPlacement> placements;
    QVector<ImageImportComponentResult> components;
    QString via;
    QString error;
    qint64 elapsedMs = 0;
    int componentCount = 0;
    int failedComponentCount = 0;
    int timedOutComponentCount = 0;
    int loopCount = 0;
    bool timedOut = false;

    bool filled() const { return error.isEmpty() && !placements.isEmpty(); }
};

struct ImageImportFillResult {
    QVector<ImageImportFilledUnit> units;
    QHash<QString, int> componentFailureReasons;
    QString summary;
    QString error;
    bool cancelled = false;
    int filledCount = 0;
    int partialCount = 0;
    int failedCount = 0;
    int timedOutCount = 0;
    int placementCount = 0;
    int componentCount = 0;
    int filledComponentCount = 0;
    int failedComponentCount = 0;
    int timedOutComponentCount = 0;

    bool complete() const { return error.isEmpty() && !cancelled && failedComponentCount == 0; }
};

using ImageImportFillProgress = std::function<void(int completed, int total)>;

QVector<ImageImportFillUnit> svgImageImportUnits(const SvgVectorDocument &document);

RasterImportUnits rasterImageImportUnits(const QImage &image,
                                         const RasterImportOptions &options = {});

QVector<QPainterPath> imageImportOutlineComponents(const QPainterPath &outline);

ImageImportPenLoops imageImportPenLoops(const QPainterPath &component,
                                        bool preferSampled = false,
                                        double simplifyEpsilon = 0.0);

ImageImportUnitLoops imageImportUnitLoops(const QPainterPath &outline,
                                          double outlineSimplification = 0.0);

ImageImportFillResult computeImageImportFills(
    const ImageImportFillRequest &request,
    const ImageImportFillProgress &progress = {},
    const std::function<bool()> &cancelled = {});

} // namespace gui
