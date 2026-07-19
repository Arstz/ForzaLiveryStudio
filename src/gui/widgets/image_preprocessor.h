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
    QVector<QColor> paletteColors;
    bool fixedPalette = false;

    static ImagePreprocessSettings animeDetail();
};

struct ImagePreprocessResult {
    QImage image;
    int retainedColorCount = 0;
    QVector<QColor> retainedPalette;
};

// Edge-free image preprocessing. Dimensions and fully transparent pixels are
// preserved; every visible output pixel is opaque. Colour flattening is
// performed in HSV. Clusters below minimumColorFraction are bucketed into their
// closest retained colour, detail is restored from a local high-frequency
// residual instead of an edge or silhouette mask, and small colour components
// are merged into a visible neighbour.
ImagePreprocessResult preprocessImageDetailed(const QImage &source,
                                              const ImagePreprocessSettings &settings);
QImage preprocessImage(const QImage &source, const ImagePreprocessSettings &settings);

} // namespace gui
