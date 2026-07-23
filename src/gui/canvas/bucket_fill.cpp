#include "bucket_fill.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

bool matchesSeed(QRgb pixel, QRgb seed, int tolerance) {
    if (qAlpha(pixel) == 0) {
        return false;
    }
    return std::abs(qRed(pixel) - qRed(seed)) <= tolerance
        && std::abs(qGreen(pixel) - qGreen(seed)) <= tolerance
        && std::abs(qBlue(pixel) - qBlue(seed)) <= tolerance
        && std::abs(qAlpha(pixel) - qAlpha(seed)) <= tolerance;
}

} // namespace

BucketFillResult floodGuideRegion(const QImage &source,
                                  const QPoint &seed,
                                  int tolerance) {
    BucketFillResult result;
    if (source.isNull()) {
        result.error = QStringLiteral("The selected guide layer has no image");
        return result;
    }
    if (tolerance < 0 || tolerance > 255) {
        result.error = QStringLiteral("Bucket tolerance must be between 0 and 255");
        return result;
    }

    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    result.imageSize = image.size();
    if (!QRect(QPoint(0, 0), image.size()).contains(seed)) {
        result.error = QStringLiteral("Hover inside the selected guide image");
        return result;
    }
    const QRgb seedPixel = reinterpret_cast<const QRgb *>(image.constScanLine(seed.y()))[seed.x()];
    result.seedColor = QColor::fromRgba(seedPixel);
    if (qAlpha(seedPixel) == 0) {
        result.error = QStringLiteral("The hovered guide pixel is transparent");
        return result;
    }

    const int width = image.width();
    const int height = image.height();
    const size_t pixelCount = static_cast<size_t>(width) * height;
    result.mask.assign(pixelCount, 0);
    std::vector<std::uint8_t> visited(pixelCount, 0);
    std::vector<int> stack;
    stack.reserve(std::min<size_t>(pixelCount, 64 * 1024));
    stack.push_back(seed.y() * width + seed.x());

    int left = seed.x();
    int right = seed.x();
    int top = seed.y();
    int bottom = seed.y();
    quint64 redSum = 0;
    quint64 greenSum = 0;
    quint64 blueSum = 0;
    quint64 alphaSum = 0;
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        if (visited[static_cast<size_t>(index)] != 0) {
            continue;
        }
        visited[static_cast<size_t>(index)] = 1;
        const int x = index % width;
        const int y = index / width;
        const QRgb pixel = reinterpret_cast<const QRgb *>(image.constScanLine(y))[x];
        if (!matchesSeed(pixel, seedPixel, tolerance)) {
            continue;
        }

        result.mask[static_cast<size_t>(index)] = 1;
        ++result.area;
        redSum += qRed(pixel);
        greenSum += qGreen(pixel);
        blueSum += qBlue(pixel);
        alphaSum += qAlpha(pixel);
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);

        if (x > 0) {
            stack.push_back(index - 1);
        }
        if (x + 1 < width) {
            stack.push_back(index + 1);
        }
        if (y > 0) {
            stack.push_back(index - width);
        }
        if (y + 1 < height) {
            stack.push_back(index + width);
        }
    }

    if (result.area == 0) {
        result.error = QStringLiteral("The hovered guide region is empty");
        return result;
    }
    result.bounds = QRect(QPoint(left, top), QPoint(right, bottom));
    result.averageColor = QColor(static_cast<int>(redSum / result.area),
                                 static_cast<int>(greenSum / result.area),
                                 static_cast<int>(blueSum / result.area),
                                 static_cast<int>(alphaSum / result.area));
    return result;
}

QImage bucketMaskPreview(const BucketFillResult &fill, const QColor &color) {
    if (!fill.valid()) {
        return {};
    }
    QImage preview(fill.imageSize, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::transparent);
    const QRgb previewPixel = qPremultiply(color.rgba());
    for (int y = fill.bounds.top(); y <= fill.bounds.bottom(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(preview.scanLine(y));
        const size_t offset = static_cast<size_t>(y) * fill.imageSize.width();
        for (int x = fill.bounds.left(); x <= fill.bounds.right(); ++x) {
            if (fill.mask[offset + x] != 0) {
                row[x] = previewPixel;
            }
        }
    }
    return preview;
}

} // namespace gui
