#pragma once

#include "compact_fit.h"
#include "bucket_fill.h"
#include "region_fill.h"

namespace gui {

inline constexpr int kDefaultImageGeneratorColors = 16;
inline constexpr int kDefaultImageGeneratorFragmentArea = 12;
inline constexpr int kDefaultImageGeneratorFragmentTolerance = 128;
inline constexpr int kDefaultImageGeneratorEvaluations = compact::kDefaultEvaluationBudget;
inline constexpr int kDefaultImageGeneratorLiningSeconds = 30;
inline constexpr double kDefaultImageGeneratorExtension = 1.0;
inline constexpr double kMaximumImageGeneratorExtension = 2.0;

enum class ImageLiningMode { Mixed, Bottom, Top };

struct ImageGeneratorOptions {
    ImageLiningMode liningMode = ImageLiningMode::Mixed;
    int colors = kDefaultImageGeneratorColors;
    int bucketTolerance = kDefaultBucketTolerance;
    int fragmentArea = kDefaultImageGeneratorFragmentArea;
    int fragmentTolerance = kDefaultImageGeneratorFragmentTolerance;
    int evaluationBudget = kDefaultImageGeneratorEvaluations;
    int liningSeconds = kDefaultImageGeneratorLiningSeconds;
    double outlineExtension = kDefaultImageGeneratorExtension;
    double boundaryAllowance = compact::kDefaultBoundaryAllowance;
    bool reducePalette = true;
    bool cleanRasterNoise = true;
    bool isolateBackground = kDefaultIsolateSolidBackground;
    bool optimizeTopology = true;
    bool detectLining = true;
    bool useGpu = true;
};

struct ImageGeneratorRequest {
    QImage source;
    QVector<PenPrimitive> primitives;
    QVector<catalog::Primitive> compactPrimitives;
    QVector<catalog::Primitive> liningPrimitives;
    ImageGeneratorOptions options;
};

struct ImageGeneratorResult {
    QVector<RegionFillLayer> fills;
    QStringList groupNames;
    QJsonObject diagnostics;
    QString error;
    bool cancelled = false;
};

using ImageGeneratorProgress = std::function<void(const QString &, int, int)>;

int pruneImageComposite(
    QVector<RegionFillLayer> *fills, const QHash<int, QPainterPath> &silhouettes,
    QImage *rendered, QImage *foreground,
    const std::function<bool()> &cancelled = {});

ImageGeneratorResult generateImage(
    const ImageGeneratorRequest &request,
    const ImageGeneratorProgress &progress = {},
    const std::function<bool()> &cancelled = {});

QVector<GeneratedRegionVariant> imageGeneratorWorldVariants(
    const ImageGeneratorResult &result, const QTransform &imageToWorld);

} // namespace gui
