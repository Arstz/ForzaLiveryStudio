#include "color_edge_snap.h"

#include <QLineF>
#include <QRectF>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int kMinimumEdgeContrast = 32;
constexpr int kMaximumSearchSamples = 64;
constexpr double kContrastDistancePenalty = 2.0;
constexpr double kMaximumChannelValue = 255.0;

int colorContrast(QRgb first, QRgb second) {
    return std::max({std::abs(qRed(first) - qRed(second)),
                     std::abs(qGreen(first) - qGreen(second)),
                     std::abs(qBlue(first) - qBlue(second)),
                     std::abs(qAlpha(first) - qAlpha(second))});
}

QPointF nearestOnSegment(const QPointF &point, const QPointF &start, const QPointF &end) {
    const QPointF delta = end - start;
    const double lengthSquared = QPointF::dotProduct(delta, delta);
    const double fraction = lengthSquared > 0.0
        ? std::clamp(QPointF::dotProduct(point - start, delta) / lengthSquared, 0.0, 1.0)
        : 0.0;

    return start + delta * fraction;
}

} // namespace

QPointF snapToColorEdge(const QImage &image, const QTransform &imageToScreen,
                       const QPointF &screenPoint, double radius) {
    bool invertible = false;
    const QTransform screenToImage = imageToScreen.inverted(&invertible);

    if (image.isNull() || !invertible || !std::isfinite(radius) || radius <= 0.0) {
        return screenPoint;
    }
    const QRectF screenBounds(screenPoint - QPointF(radius, radius), QSizeF(radius * 2.0, radius * 2.0));
    const QRect bounds = screenToImage.mapRect(screenBounds).toAlignedRect()
        .adjusted(-1, -1, 1, 1).intersected(image.rect());
    const int stepX = std::max(1, static_cast<int>(std::ceil(static_cast<double>(bounds.width()) / kMaximumSearchSamples)));
    const int stepY = std::max(1, static_cast<int>(std::ceil(static_cast<double>(bounds.height()) / kMaximumSearchSamples)));
    QPointF result = screenPoint;
    double bestScore = radius + kContrastDistancePenalty;
    const auto consider = [&](const QPointF &start, const QPointF &end, int contrast) {
        if (contrast < kMinimumEdgeContrast) {
            return;
        }
        const QPointF candidate = nearestOnSegment(screenPoint, imageToScreen.map(start), imageToScreen.map(end));
        const double distance = QLineF(screenPoint, candidate).length();
        const double score = distance + kContrastDistancePenalty * (1.0 - contrast / kMaximumChannelValue);

        if (distance <= radius && score < bestScore) {
            result = candidate;
            bestScore = score;
        }
    };

    for (int y = bounds.top(); y <= bounds.bottom(); y += stepY) {
        for (int x = bounds.left(); x <= bounds.right(); x += stepX) {
            const QRgb pixel = image.pixel(x, y);
            const double centerX = x + 0.5;
            const double centerY = y + 0.5;

            if (x + stepX < image.width()) {
                const double edgeX = centerX + stepX * 0.5;
                consider(QPointF(edgeX, std::max(0.0, centerY - stepY * 0.5)),
                         QPointF(edgeX, std::min(static_cast<double>(image.height()), centerY + stepY * 0.5)),
                         colorContrast(pixel, image.pixel(x + stepX, y)));
            }
            if (y + stepY < image.height()) {
                const double edgeY = centerY + stepY * 0.5;
                consider(QPointF(std::max(0.0, centerX - stepX * 0.5), edgeY),
                         QPointF(std::min(static_cast<double>(image.width()), centerX + stepX * 0.5), edgeY),
                         colorContrast(pixel, image.pixel(x, y + stepY)));
            }
        }
    }

    return result;
}

} // namespace gui
