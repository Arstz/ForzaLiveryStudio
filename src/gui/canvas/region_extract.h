#pragma once

#include <QtCore>
#include <QtGui>

#include <cstdint>
#include <vector>

namespace gui {

// First-draft raster -> region segmentation for the cover pipeline
// (see docs/COVER_PROBLEM.md). Quantizes an image to a frequency-based
// palette, labels connected same-colour components, and classifies thin,
// contrasting components as lineart. Geometry is emitted in image-pixel
// coordinates so the canvas can draw it under the guide layer's transform.

struct RegionExtractionParams {
    int colorCount = 16;             // palette size CAP; the effective count is
                                     // content-aware (see below) and usually lower
    double colorMergeDistance = 30.0;// min RGB euclidean distance for a colour to
                                     // count as distinct; near-duplicates (mostly
                                     // anti-aliasing fringes) merge into the more
                                     // frequent colour instead of forming regions
    double colorFrequencyFloor = 5e-4;// palette candidates rarer than this fraction
                                     // of opaque pixels are treated as noise
    double alphaThreshold = 0.5;     // pixels below this alpha are background
    int minRegionArea = 12;          // components smaller than this are dropped
    double lineWidthFactor = 0.016;  // max stroke-width cap as a fraction of min(w,h)
    double lineWidthFloor = 3.0;     // absolute max stroke-width cap in pixels
    double contrastThreshold = 0.22; // luminance distance from the mean to be lineart
    double minElongation = 2.0;      // length/width ratio required for lineart
    int maxDimension = 0;            // downscale so max(w,h) <= this (0 = no downscale)
    int blurPasses = 1;              // 3x3 box-blur passes before quantizing (denoise)
    int traceSpeckle = 2;            // potrace turdsize: suppress specks <= this area
    double traceAlphaMax = 1.0;      // potrace corner threshold (higher = smoother)
    double traceOptTolerance = 0.2;  // potrace curve-optimization tolerance
};

struct ExtractedRegion {
    int id = 0;
    QColor color;                    // palette colour of the region
    bool lineart = false;
    QPainterPath outline;            // traced boundary (smooth beziers), image-pixel coords
    QRect bounds;
    int area = 0;                    // pixel count
    double strokeWidth = 0.0;        // max stroke width (2 * max distance-to-boundary)
    int debugColor = 0;              // graph-coloring index; adjacent regions differ
};

struct RegionExtractionResult {
    QSize imageSize;
    QVector<ExtractedRegion> regions;
    int colorRegionCount = 0;
    int lineartRegionCount = 0;
    QString error;

    bool valid() const { return error.isEmpty() && !regions.isEmpty(); }
};

RegionExtractionResult extractRegions(const QImage &image,
                                      const RegionExtractionParams &params = {});

// Vectorize an arbitrary binary mask (nonzero == set) with potrace, returning a
// smooth bezier QPainterPath (outer + hole subpaths, WindingFill) in image-pixel
// coordinates. `bounds` is the tight region of interest inside the mask. Used by
// the occlusion fill to re-trace a region grown into its occluded leeway.
QPainterPath traceMaskToPath(const std::vector<std::uint8_t> &mask, int width, int height,
                             const QRect &bounds, const RegionExtractionParams &params = {});

} // namespace gui
