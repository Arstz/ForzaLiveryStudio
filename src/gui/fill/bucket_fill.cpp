#include "bucket_fill.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

bool matchesSeed(QRgb pixel, QRgb seed, int tolerance) {
    if (qAlpha(seed) == 0) {
        return qAlpha(pixel) == 0;
    }
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
    result.transparentTarget = qAlpha(seedPixel) == 0;

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

BucketRegionsResult floodGuideRegions(
    const QImage &source, int tolerance,
    const std::function<bool()> &cancelled,
    const std::function<void(int, int)> &progress) {
    BucketRegionsResult result;
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    QHash<QRgb, QVector<int>> pixelsByColor;
    QVector<int> queue;
    const int width = image.width();
    const int height = image.height();
    const int pixelCount = width * height;
    const QPoint offsets[] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    auto stopped = [&]() { return cancelled && cancelled(); };

    if (image.isNull() || tolerance < 0 || tolerance > 255) {
        result.error = QStringLiteral("Bucket extraction requires an image and a tolerance between 0 and 255");
        return result;
    }
    result.imageSize = image.size();
    result.labels.fill(-1, pixelCount);
    for (int y = 0; y < height; ++y) {
        if (stopped()) {
            result.cancelled = true;
            return result;
        }
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(row[x]) != 0) {
                pixelsByColor[row[x]].push_back(y * width + x);
            }
        }
    }
    QList<QRgb> colors = pixelsByColor.keys();
    std::sort(colors.begin(), colors.end(), [&](QRgb left, QRgb right) {
        const qsizetype leftCount = pixelsByColor.constFind(left)->size();
        const qsizetype rightCount = pixelsByColor.constFind(right)->size();

        return leftCount != rightCount ? leftCount > rightCount : left < right;
    });
    int completed = 0;
    int reportedPercent = -1;
    for (const QRgb seedColor : colors) {
        const int percent = static_cast<int>(100LL * completed / std::max<qsizetype>(1, colors.size()));
        if (progress && percent != reportedPercent) {
            progress(completed, colors.size());
            reportedPercent = percent;
        }
        for (const int seed : pixelsByColor.value(seedColor)) {
            if (stopped()) {
                result.cancelled = true;
                return result;
            }
            if (result.labels[seed] >= 0) {
                continue;
            }
            BucketRegion region;
            region.seed = QPoint(seed % width, seed / width);
            region.bounds = QRect(region.seed, QSize(1, 1));
            const int label = result.regions.size();
            quint64 red = 0;
            quint64 green = 0;
            quint64 blue = 0;
            quint64 alpha = 0;
            queue.clear();
            queue.push_back(seed);
            result.labels[seed] = label;
            for (int cursor = 0; cursor < queue.size(); ++cursor) {
                if ((cursor & 4095) == 0 && stopped()) {
                    result.cancelled = true;
                    return result;
                }
                const int pixel = queue[cursor];
                const int x = pixel % width;
                const int y = pixel / width;
                const QRgb color = reinterpret_cast<const QRgb *>(image.constScanLine(y))[x];
                red += qRed(color);
                green += qGreen(color);
                blue += qBlue(color);
                alpha += qAlpha(color);
                region.bounds = region.bounds.united(QRect(x, y, 1, 1));
                for (const QPoint &offset : offsets) {
                    const int nx = x + offset.x();
                    const int ny = y + offset.y();
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const int neighbor = ny * width + nx;
                    if (result.labels[neighbor] >= 0
                        || !matchesSeed(reinterpret_cast<const QRgb *>(image.constScanLine(ny))[nx],
                                        seedColor, tolerance)) {
                        continue;
                    }
                    result.labels[neighbor] = label;
                    queue.push_back(neighbor);
                }
            }
            region.area = queue.size();
            region.color = QColor(static_cast<int>(red / region.area),
                                  static_cast<int>(green / region.area),
                                  static_cast<int>(blue / region.area),
                                  static_cast<int>(alpha / region.area));
            result.regions.push_back(std::move(region));
        }
        ++completed;
    }
    if (progress) {
        progress(colors.size(), colors.size());
    }

    return result;
}

QImage bucketMaskPreview(const BucketFillResult &fill, const QColor &color) {
    if (!fill.valid()) {
        return {};
    }
    QColor previewColor = fill.transparentTarget
        ? kTransparentBucketColor
        : color;
    previewColor.setAlpha(color.alpha());
    QImage preview(fill.imageSize, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::transparent);
    const QRgb previewPixel = qPremultiply(previewColor.rgba());
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
