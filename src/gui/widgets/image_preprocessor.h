#pragma once

#include <QImage>

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

    static ImagePreprocessSettings animeDetail();
};

struct ImagePreprocessResult {
    QImage image;
    int retainedColorCount = 0;
};

// Edge-free image preprocessing. Alpha and dimensions are preserved. Colour
// flattening is performed in HSV. Clusters below minimumColorFraction are
// bucketed into their closest retained colour, and detail is restored from a
// local high-frequency residual instead of an edge or silhouette mask.
ImagePreprocessResult preprocessImageDetailed(const QImage &source,
                                              const ImagePreprocessSettings &settings);
QImage preprocessImage(const QImage &source, const ImagePreprocessSettings &settings);

} // namespace gui
