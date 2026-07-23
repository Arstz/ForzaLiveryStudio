#pragma once

#include <QImage>
#include <QColor>
#include <QVector>

namespace gui {

struct ImagePreprocessSettings {
    int colors = 16;
    double flattenStrength = 0.18;
    double saturationRestore = 0.18;
    double detailRestore = 0.28;

    int smoothingPasses = 2;
    int smoothingDiameter = 9;
    double sigmaColor = 35.0;
    double sigmaSpace = 35.0;
    int saturationThreshold = 155;
    int flattenRadius = 2;
    int detailRadius = 2;
    int quantizationIterations = 60;
    double minimumColorFraction = 5e-4;
    int speckleSize = 4;
    bool noDetailNearEdges = true;
    int edgeCleanupPasses = 1;
    int edgeCleanupWindow = 3;
    bool forceFlatFills = false;
    int flatFillMinimumArea = 128;
    bool lineMode = true;
    QColor lineColor;
    QVector<QColor> paletteColors;
    bool fixedPalette = false;

    static ImagePreprocessSettings animeDetail();
};

struct ImagePreprocessResult {
    QImage image;
    int retainedColorCount = 0;
    QVector<QColor> retainedPalette;
};

// Label-based image preprocessing. Dimensions and fully transparent pixels are
// preserved; every visible output pixel is opaque and exactly matches a retained
// palette colour. After HSV clustering, cleanup and edge-safe detail selection
// operate on palette indices. Optional line pixels use an immutable dedicated
// palette label.
ImagePreprocessResult preprocessImageDetailed(const QImage &source,
                                              const ImagePreprocessSettings &settings);
QImage preprocessImage(const QImage &source, const ImagePreprocessSettings &settings);

} // namespace gui
