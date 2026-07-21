#pragma once

#include <QtCore>
#include <QtGui>

#include <cstdint>
#include <vector>

namespace gui {

struct RegionExtractionParams {
    int maxColorCount = 16;
    double colorMergeDistance = 30.0;
    double colorFrequencyFloor = 5e-4;
    double alphaThreshold = 0.5;
    int minRegionArea = 12;
    int smallRegionMergeArea = 12;
    double lineWidthCapFraction = 0.016;
    double lineWidthCapFloor = 3.0;
    double contrastThreshold = 0.22;
    double minElongation = 2.0;
    int maxDimension = 0;
    int blurPasses = 1;
    int traceSpeckle = 2;
    double traceAlphaMax = 1.0;
    double traceOptTolerance = 0.2;
};

struct ExtractedRegion {
    int id = 0;
    QColor color;
    bool lineart = false;
    QPainterPath outline;
    QRect bounds;
    int area = 0;
    double strokeWidth = 0.0;
    int debugColor = 0;
};

struct RegionRasterData {
    std::vector<int> labels;
    RegionExtractionParams traceParams;
};

struct RegionExtractionResult {
    QSize imageSize;
    QVector<ExtractedRegion> regions;
    QSharedPointer<const RegionRasterData> raster;
    int colorRegionCount = 0;
    int lineartRegionCount = 0;
    int mergedSmallRegionCount = 0;
    QString error;

    bool valid() const { return error.isEmpty() && !regions.isEmpty(); }
};

RegionExtractionResult extractRegions(const QImage &image,
                                      const RegionExtractionParams &params = {});

QPainterPath traceMaskToPath(const std::vector<std::uint8_t> &mask, int width, int height,
                             const QRect &bounds, const RegionExtractionParams &params = {});

} // namespace gui
