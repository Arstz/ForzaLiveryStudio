#pragma once

#include "pen_fill.h"
#include <cstdint>
#include <vector>

namespace gui {

struct CubicFitOptions {
    double tolerance = 1.0; // Source-image pixels, independent of view zoom.
    double cornerScale = 4.0;
    int refinementPasses = 3;
    // For masks, treat measured raster roughness as an uncertainty band. The tolerance
    // above is its minimum; narrow strokes cap how far this band can expand.
    bool adaptToRasterNoise = true;
};

// Pixel-square boundaries; foreground uses four-neighbour connectivity.
QVector<QPolygonF> pixelBoundaryLoops(const std::vector<std::uint8_t> &mask, QSize size,
                                      QRect bounds);
QVector<PenPoint> fitCubicContour(const QPolygonF &boundary, const CubicFitOptions &options = {});
QVector<PenLoop> cubicPathLoops(const QPainterPath &path, QString *error = nullptr);
QVector<PenLoop> fitMaskContours(const std::vector<std::uint8_t> &mask, QSize size, QRect bounds,
                                 const CubicFitOptions &options = {}, QString *error = nullptr);

} // namespace gui
