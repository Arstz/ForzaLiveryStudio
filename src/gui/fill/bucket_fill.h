#pragma once

#include <QtCore>
#include <QtGui>

#include <cstdint>
#include <vector>

namespace gui {

struct BucketFillResult {
    QSize imageSize;
    std::vector<std::uint8_t> mask;
    QRect bounds;
    QColor seedColor;
    QColor averageColor;
    int area = 0;
    QString error;

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

QImage bucketMaskPreview(const BucketFillResult &fill,
                         const QColor &color = QColor(64, 164, 255, 112));

} // namespace gui
