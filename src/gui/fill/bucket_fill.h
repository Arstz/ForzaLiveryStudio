#pragma once

#include <QtCore>
#include <QtGui>

#include <cstdint>
#include <vector>
#include <functional>

namespace gui {

inline const QColor kTransparentBucketColor(255, 0, 255);
inline constexpr int kDefaultBucketTolerance = 16;

struct BucketRegion {
    QColor color;
    QPoint seed;
    QRect bounds;
    int area = 0;
};

struct BucketRegionsResult {
    QSize imageSize;
    QVector<int> labels;
    QVector<BucketRegion> regions;
    QString error;
    bool cancelled = false;
};

struct BucketFillResult {
    QSize imageSize;
    std::vector<std::uint8_t> mask;
    QRect bounds;
    QColor seedColor;
    QColor averageColor;
    QString error;
    int area = 0;
    bool transparentTarget = false;

    bool valid() const {
        return error.isEmpty()
            && area > 0
            && imageSize.width() > 0
            && imageSize.height() > 0
            && mask.size() == static_cast<size_t>(imageSize.width()) * imageSize.height();
    }
};

BucketFillResult floodGuideRegion(const QImage &image,
                                  const QPoint &seed,
                                  int tolerance);

BucketRegionsResult floodGuideRegions(
    const QImage &image, int tolerance = kDefaultBucketTolerance,
    const std::function<bool()> &cancelled = {},
    const std::function<void(int, int)> &progress = {});

QImage bucketMaskPreview(const BucketFillResult &fill,
                         const QColor &color = QColor(64, 164, 255, 112));

} // namespace gui
