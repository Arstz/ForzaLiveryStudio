#include "image_preprocessor.h"

#include <QtCore>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace gui {
namespace {

struct HsvColor {
    double h = 0.0;
    double s = 0.0;
    double v = 0.0;
};

struct HistogramColor {
    HsvColor hsv;
    double weight = 0.0;
};

struct QuantizeResult {
    QImage image;
    int retainedColorCount = 0;
};

constexpr double kEpsilon = 1e-9;

HsvColor rgbToHsv(int red, int green, int blue)
{
    QColor color(red, green, blue);
    float hue = 0.0F;
    float saturation = 0.0F;
    float value = 0.0F;
    color.getHsvF(&hue, &saturation, &value);
    return {hue < 0.0 ? 0.0 : hue, saturation, value};
}

QRgb hsvToRgb(const HsvColor &hsv)
{
    return QColor::fromHsvF(hsv.h - std::floor(hsv.h),
                            std::clamp(hsv.s, 0.0, 1.0),
                            std::clamp(hsv.v, 0.0, 1.0)).rgb();
}

double hsvDistanceSquared(const HsvColor &left, const HsvColor &right)
{
    double hueDistance = std::abs(left.h - right.h);
    hueDistance = std::min(hueDistance, 1.0 - hueDistance);
    const double hueGate = std::sqrt(std::max(0.0, left.s * right.s));
    const double ds = left.s - right.s;
    const double dv = left.v - right.v;
    return 4.0 * hueGate * hueDistance * hueDistance
        + 0.35 * ds * ds
        + 0.65 * dv * dv;
}

int colorBin(QRgb pixel)
{
    return ((qRed(pixel) >> 3) << 10) | ((qGreen(pixel) >> 3) << 5) | (qBlue(pixel) >> 3);
}

QImage bilateralOneDimension(const QImage &source, int radius, double sigmaColor,
                             double sigmaSpace, bool horizontal)
{
    if (radius <= 0 || source.isNull()) {
        return source;
    }
    QImage result(source.size(), QImage::Format_ARGB32);
    const double colorDenominator = std::max(2.0 * sigmaColor * sigmaColor, kEpsilon);
    const double spatialDenominator = std::max(2.0 * sigmaSpace * sigmaSpace, kEpsilon);
    std::vector<double> spatial(static_cast<size_t>(radius + 1));
    for (int offset = 0; offset <= radius; ++offset) {
        spatial[static_cast<size_t>(offset)] = std::exp(-(offset * offset) / spatialDenominator);
    }

    for (int y = 0; y < source.height(); ++y) {
        const auto *sourceLine = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *targetLine = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const QRgb center = sourceLine[x];
            if (qAlpha(center) == 0) {
                targetLine[x] = center;
                continue;
            }
            double totalWeight = 0.0;
            double red = 0.0;
            double green = 0.0;
            double blue = 0.0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int nx = horizontal ? std::clamp(x + offset, 0, source.width() - 1) : x;
                const int ny = horizontal ? y : std::clamp(y + offset, 0, source.height() - 1);
                const auto *neighborLine = reinterpret_cast<const QRgb *>(source.constScanLine(ny));
                const QRgb neighbor = neighborLine[nx];
                if (qAlpha(neighbor) == 0) {
                    continue;
                }
                const double dr = qRed(neighbor) - qRed(center);
                const double dg = qGreen(neighbor) - qGreen(center);
                const double db = qBlue(neighbor) - qBlue(center);
                const double colorWeight = std::exp(-(dr * dr + dg * dg + db * db) / colorDenominator);
                const double weight = spatial[static_cast<size_t>(std::abs(offset))] * colorWeight;
                totalWeight += weight;
                red += weight * qRed(neighbor);
                green += weight * qGreen(neighbor);
                blue += weight * qBlue(neighbor);
            }
            if (totalWeight <= kEpsilon) {
                targetLine[x] = center;
            } else {
                targetLine[x] = qRgba(std::clamp(qRound(red / totalWeight), 0, 255),
                                      std::clamp(qRound(green / totalWeight), 0, 255),
                                      std::clamp(qRound(blue / totalWeight), 0, 255),
                                      qAlpha(center));
            }
        }
    }
    return result;
}

QImage bilateralSmooth(const QImage &source, const ImagePreprocessSettings &settings)
{
    QImage result = source;
    const int radius = std::max(0, settings.smoothingDiameter / 2);
    for (int pass = 0; pass < std::max(0, settings.smoothingPasses); ++pass) {
        result = bilateralOneDimension(result, radius, settings.sigmaColor, settings.sigmaSpace, true);
        result = bilateralOneDimension(result, radius, settings.sigmaColor, settings.sigmaSpace, false);
    }
    return result;
}

QImage medianBlur(const QImage &source, int radius)
{
    if (radius <= 0 || source.isNull()) {
        return source;
    }
    QImage result(source.size(), QImage::Format_ARGB32);
    const int diameter = radius * 2 + 1;
    const int maximumSamples = diameter * diameter;
    std::vector<int> red(static_cast<size_t>(maximumSamples));
    std::vector<int> green(static_cast<size_t>(maximumSamples));
    std::vector<int> blue(static_cast<size_t>(maximumSamples));
    for (int y = 0; y < source.height(); ++y) {
        auto *target = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const QRgb center = source.pixel(x, y);
            int count = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sy = std::clamp(y + dy, 0, source.height() - 1);
                const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(sy));
                for (int dx = -radius; dx <= radius; ++dx) {
                    const QRgb sample = line[std::clamp(x + dx, 0, source.width() - 1)];
                    if (qAlpha(sample) == 0) {
                        continue;
                    }
                    red[static_cast<size_t>(count)] = qRed(sample);
                    green[static_cast<size_t>(count)] = qGreen(sample);
                    blue[static_cast<size_t>(count)] = qBlue(sample);
                    ++count;
                }
            }
            if (count == 0) {
                target[x] = center;
                continue;
            }
            const int middle = count / 2;
            std::nth_element(red.begin(), red.begin() + middle, red.begin() + count);
            std::nth_element(green.begin(), green.begin() + middle, green.begin() + count);
            std::nth_element(blue.begin(), blue.begin() + middle, blue.begin() + count);
            target[x] = qRgba(red[static_cast<size_t>(middle)], green[static_cast<size_t>(middle)],
                              blue[static_cast<size_t>(middle)], qAlpha(center));
        }
    }
    return result;
}

QImage blendImages(const QImage &left, const QImage &right, double amount)
{
    const double alpha = std::clamp(amount, 0.0, 1.0);
    QImage result(left.size(), QImage::Format_ARGB32);
    for (int y = 0; y < left.height(); ++y) {
        const auto *a = reinterpret_cast<const QRgb *>(left.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(right.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < left.width(); ++x) {
            out[x] = qRgba(qRound(qRed(a[x]) * (1.0 - alpha) + qRed(b[x]) * alpha),
                           qRound(qGreen(a[x]) * (1.0 - alpha) + qGreen(b[x]) * alpha),
                           qRound(qBlue(a[x]) * (1.0 - alpha) + qBlue(b[x]) * alpha),
                           qAlpha(a[x]));
        }
    }
    return result;
}

QImage boxBlur(const QImage &source, int radius)
{
    if (radius <= 0 || source.isNull()) {
        return source;
    }
    QImage horizontal(source.size(), QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        auto *target = reinterpret_cast<QRgb *>(horizontal.scanLine(y));
        int red = 0;
        int green = 0;
        int blue = 0;
        int weight = 0;
        for (int dx = -radius; dx <= radius; ++dx) {
            const QRgb pixel = source.pixel(std::clamp(dx, 0, source.width() - 1), y);
            const int alpha = qAlpha(pixel);
            red += qRed(pixel) * alpha;
            green += qGreen(pixel) * alpha;
            blue += qBlue(pixel) * alpha;
            weight += alpha;
        }
        for (int x = 0; x < source.width(); ++x) {
            const QRgb center = source.pixel(x, y);
            target[x] = weight > 0
                ? qRgba(red / weight, green / weight, blue / weight, qAlpha(center))
                : center;
            const QRgb leaving = source.pixel(std::clamp(x - radius, 0, source.width() - 1), y);
            const QRgb entering = source.pixel(std::clamp(x + radius + 1, 0, source.width() - 1), y);
            red += qRed(entering) * qAlpha(entering) - qRed(leaving) * qAlpha(leaving);
            green += qGreen(entering) * qAlpha(entering) - qGreen(leaving) * qAlpha(leaving);
            blue += qBlue(entering) * qAlpha(entering) - qBlue(leaving) * qAlpha(leaving);
            weight += qAlpha(entering) - qAlpha(leaving);
        }
    }

    QImage result(source.size(), QImage::Format_ARGB32);
    for (int x = 0; x < source.width(); ++x) {
        int red = 0;
        int green = 0;
        int blue = 0;
        int weight = 0;
        for (int dy = -radius; dy <= radius; ++dy) {
            const QRgb pixel = horizontal.pixel(x, std::clamp(dy, 0, source.height() - 1));
            const int alpha = qAlpha(pixel);
            red += qRed(pixel) * alpha;
            green += qGreen(pixel) * alpha;
            blue += qBlue(pixel) * alpha;
            weight += alpha;
        }
        for (int y = 0; y < source.height(); ++y) {
            const QRgb center = source.pixel(x, y);
            auto *target = reinterpret_cast<QRgb *>(result.scanLine(y));
            target[x] = weight > 0
                ? qRgba(red / weight, green / weight, blue / weight, qAlpha(center))
                : center;
            const QRgb leaving = horizontal.pixel(x, std::clamp(y - radius, 0, source.height() - 1));
            const QRgb entering = horizontal.pixel(x, std::clamp(y + radius + 1, 0, source.height() - 1));
            red += qRed(entering) * qAlpha(entering) - qRed(leaving) * qAlpha(leaving);
            green += qGreen(entering) * qAlpha(entering) - qGreen(leaving) * qAlpha(leaving);
            blue += qBlue(entering) * qAlpha(entering) - qBlue(leaving) * qAlpha(leaving);
            weight += qAlpha(entering) - qAlpha(leaving);
        }
    }
    return result;
}

QuantizeResult quantizeHsv(const QImage &source, int requestedColors, int maximumIterations,
                           double minimumColorFraction)
{
    constexpr int histogramSize = 32 * 32 * 32;
    struct Accumulator {
        quint64 red = 0;
        quint64 green = 0;
        quint64 blue = 0;
        quint64 weight = 0;
    };
    std::vector<Accumulator> histogram(histogramSize);
    for (int y = 0; y < source.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = line[x];
            const int alpha = qAlpha(pixel);
            if (alpha == 0) {
                continue;
            }
            Accumulator &entry = histogram[static_cast<size_t>(colorBin(pixel))];
            entry.red += static_cast<quint64>(qRed(pixel)) * alpha;
            entry.green += static_cast<quint64>(qGreen(pixel)) * alpha;
            entry.blue += static_cast<quint64>(qBlue(pixel)) * alpha;
            entry.weight += alpha;
        }
    }

    std::vector<HistogramColor> colors;
    colors.reserve(histogramSize);
    for (const Accumulator &entry : histogram) {
        if (entry.weight == 0) {
            continue;
        }
        const int red = qRound(static_cast<double>(entry.red) / entry.weight);
        const int green = qRound(static_cast<double>(entry.green) / entry.weight);
        const int blue = qRound(static_cast<double>(entry.blue) / entry.weight);
        colors.push_back({rgbToHsv(red, green, blue), static_cast<double>(entry.weight)});
    }
    if (colors.empty()) {
        return {source, 0};
    }

    const int clusterCount = std::clamp(requestedColors, 1, std::min(256, static_cast<int>(colors.size())));
    std::vector<HsvColor> centers;
    centers.reserve(static_cast<size_t>(clusterCount));
    const auto mostFrequent = std::max_element(colors.begin(), colors.end(), [](const auto &left, const auto &right) {
        return left.weight < right.weight;
    });
    centers.push_back(mostFrequent->hsv);
    std::vector<double> nearestDistances(colors.size(), std::numeric_limits<double>::max());
    while (static_cast<int>(centers.size()) < clusterCount) {
        double bestScore = -1.0;
        HsvColor best = colors.front().hsv;
        const HsvColor &latest = centers.back();
        for (size_t index = 0; index < colors.size(); ++index) {
            const HistogramColor &color = colors[index];
            nearestDistances[index] = std::min(nearestDistances[index], hsvDistanceSquared(color.hsv, latest));
            const double score = nearestDistances[index] * color.weight;
            if (score > bestScore) {
                bestScore = score;
                best = color.hsv;
            }
        }
        centers.push_back(best);
    }

    std::vector<int> assignments(colors.size(), 0);
    for (int iteration = 0; iteration < std::max(1, maximumIterations); ++iteration) {
        struct ClusterSum {
            double hueSin = 0.0;
            double hueCos = 0.0;
            double hueWeight = 0.0;
            double saturation = 0.0;
            double value = 0.0;
            double weight = 0.0;
        };
        std::vector<ClusterSum> sums(static_cast<size_t>(clusterCount));
        for (size_t colorIndex = 0; colorIndex < colors.size(); ++colorIndex) {
            double nearest = std::numeric_limits<double>::max();
            int nearestIndex = 0;
            for (int centerIndex = 0; centerIndex < clusterCount; ++centerIndex) {
                const double distance = hsvDistanceSquared(colors[colorIndex].hsv, centers[static_cast<size_t>(centerIndex)]);
                if (distance < nearest) {
                    nearest = distance;
                    nearestIndex = centerIndex;
                }
            }
            assignments[colorIndex] = nearestIndex;
            ClusterSum &sum = sums[static_cast<size_t>(nearestIndex)];
            const double weight = colors[colorIndex].weight;
            const double hueWeight = weight * colors[colorIndex].hsv.s;
            const double angle = colors[colorIndex].hsv.h * 2.0 * std::numbers::pi;
            sum.hueSin += std::sin(angle) * hueWeight;
            sum.hueCos += std::cos(angle) * hueWeight;
            sum.hueWeight += hueWeight;
            sum.saturation += colors[colorIndex].hsv.s * weight;
            sum.value += colors[colorIndex].hsv.v * weight;
            sum.weight += weight;
        }
        double movement = 0.0;
        for (int index = 0; index < clusterCount; ++index) {
            const ClusterSum &sum = sums[static_cast<size_t>(index)];
            if (sum.weight <= kEpsilon) {
                continue;
            }
            double hue = centers[static_cast<size_t>(index)].h;
            if (sum.hueWeight > kEpsilon) {
                hue = std::atan2(sum.hueSin, sum.hueCos) / (2.0 * std::numbers::pi);
                if (hue < 0.0) {
                    hue += 1.0;
                }
            }
            const HsvColor updated{hue, sum.saturation / sum.weight, sum.value / sum.weight};
            movement += hsvDistanceSquared(centers[static_cast<size_t>(index)], updated);
            centers[static_cast<size_t>(index)] = updated;
        }
        if (movement < 0.01) {
            break;
        }
    }

    // Reassign once against the final centers before measuring cluster
    // significance; the last centroid update may have moved a boundary.
    for (size_t colorIndex = 0; colorIndex < colors.size(); ++colorIndex) {
        double nearest = std::numeric_limits<double>::max();
        for (int centerIndex = 0; centerIndex < clusterCount; ++centerIndex) {
            const double distance = hsvDistanceSquared(
                colors[colorIndex].hsv, centers[static_cast<size_t>(centerIndex)]);
            if (distance < nearest) {
                nearest = distance;
                assignments[colorIndex] = centerIndex;
            }
        }
    }

    std::vector<double> clusterWeights(static_cast<size_t>(clusterCount), 0.0);
    double totalWeight = 0.0;
    for (size_t index = 0; index < colors.size(); ++index) {
        clusterWeights[static_cast<size_t>(assignments[index])] += colors[index].weight;
        totalWeight += colors[index].weight;
    }
    const double floor = std::clamp(minimumColorFraction, 0.0, 1.0) * totalWeight;
    std::vector<int> retainedClusters;
    retainedClusters.reserve(static_cast<size_t>(clusterCount));
    for (int index = 0; index < clusterCount; ++index) {
        if (clusterWeights[static_cast<size_t>(index)] >= floor && clusterWeights[static_cast<size_t>(index)] > 0.0) {
            retainedClusters.push_back(index);
        }
    }
    if (retainedClusters.empty()) {
        retainedClusters.push_back(static_cast<int>(std::distance(
            clusterWeights.begin(), std::max_element(clusterWeights.begin(), clusterWeights.end()))));
    }

    std::vector<int> clusterRemap(static_cast<size_t>(clusterCount), 0);
    for (int cluster = 0; cluster < clusterCount; ++cluster) {
        double nearest = std::numeric_limits<double>::max();
        for (int paletteIndex = 0; paletteIndex < static_cast<int>(retainedClusters.size()); ++paletteIndex) {
            const int retained = retainedClusters[static_cast<size_t>(paletteIndex)];
            const double distance = hsvDistanceSquared(centers[static_cast<size_t>(cluster)],
                                                       centers[static_cast<size_t>(retained)]);
            if (distance < nearest) {
                nearest = distance;
                clusterRemap[static_cast<size_t>(cluster)] = paletteIndex;
            }
        }
    }

    std::array<int, histogramSize> binAssignments{};
    for (int bin = 0; bin < histogramSize; ++bin) {
        if (histogram[static_cast<size_t>(bin)].weight == 0) {
            continue;
        }
        const Accumulator &entry = histogram[static_cast<size_t>(bin)];
        const HsvColor hsv = rgbToHsv(qRound(static_cast<double>(entry.red) / entry.weight),
                                      qRound(static_cast<double>(entry.green) / entry.weight),
                                      qRound(static_cast<double>(entry.blue) / entry.weight));
        double nearest = std::numeric_limits<double>::max();
        for (int centerIndex = 0; centerIndex < clusterCount; ++centerIndex) {
            const double distance = hsvDistanceSquared(hsv, centers[static_cast<size_t>(centerIndex)]);
            if (distance < nearest) {
                nearest = distance;
                binAssignments[static_cast<size_t>(bin)] = clusterRemap[static_cast<size_t>(centerIndex)];
            }
        }
    }
    std::vector<QRgb> palette;
    palette.reserve(retainedClusters.size());
    for (int retained : retainedClusters) {
        palette.push_back(hsvToRgb(centers[static_cast<size_t>(retained)]));
    }

    QImage result(source.size(), QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        const auto *input = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *output = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const int alpha = qAlpha(input[x]);
            if (alpha == 0) {
                output[x] = input[x];
                continue;
            }
            const QRgb color = palette[static_cast<size_t>(binAssignments[static_cast<size_t>(colorBin(input[x]))])];
            output[x] = qRgba(qRed(color), qGreen(color), qBlue(color), alpha);
        }
    }
    return {result, static_cast<int>(palette.size())};
}

double saturationOf(QRgb pixel)
{
    const int maximum = std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)});
    const int minimum = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)});
    return maximum == 0 ? 0.0 : 255.0 * static_cast<double>(maximum - minimum) / maximum;
}

QImage restoreSaturationAndDetail(const QImage &quantized, const QImage &flattened,
                                  const ImagePreprocessSettings &settings)
{
    const QImage detailBlur = boxBlur(flattened, std::max(1, settings.detailRadius));
    QImage result(flattened.size(), QImage::Format_ARGB32);
    const double saturationAmount = std::clamp(settings.saturationRestore, 0.0, 1.0);
    const double detailAmount = std::clamp(settings.detailRestore, 0.0, 1.0);
    constexpr double transitionWidth = 32.0;
    for (int y = 0; y < flattened.height(); ++y) {
        const auto *quant = reinterpret_cast<const QRgb *>(quantized.constScanLine(y));
        const auto *base = reinterpret_cast<const QRgb *>(flattened.constScanLine(y));
        const auto *blur = reinterpret_cast<const QRgb *>(detailBlur.constScanLine(y));
        auto *output = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < flattened.width(); ++x) {
            const double mask = std::clamp((saturationOf(base[x]) - settings.saturationThreshold) / transitionWidth, 0.0, 1.0);
            const double saturationBlend = saturationAmount * mask;
            const double red = qRed(quant[x]) * (1.0 - saturationBlend) + qRed(base[x]) * saturationBlend
                + detailAmount * (qRed(base[x]) - qRed(blur[x]));
            const double green = qGreen(quant[x]) * (1.0 - saturationBlend) + qGreen(base[x]) * saturationBlend
                + detailAmount * (qGreen(base[x]) - qGreen(blur[x]));
            const double blue = qBlue(quant[x]) * (1.0 - saturationBlend) + qBlue(base[x]) * saturationBlend
                + detailAmount * (qBlue(base[x]) - qBlue(blur[x]));
            output[x] = qRgba(std::clamp(qRound(red), 0, 255),
                              std::clamp(qRound(green), 0, 255),
                              std::clamp(qRound(blue), 0, 255),
                              qAlpha(base[x]));
        }
    }
    return result;
}

} // namespace

ImagePreprocessSettings ImagePreprocessSettings::animeDetail()
{
    return {};
}

ImagePreprocessResult preprocessImageDetailed(const QImage &source,
                                              const ImagePreprocessSettings &settings)
{
    if (source.isNull()) {
        return {};
    }
    const QImage input = source.convertToFormat(QImage::Format_ARGB32);
    const QImage smoothed = bilateralSmooth(input, settings);
    const QImage median = medianBlur(smoothed, std::clamp(settings.flattenRadius, 0, 8));
    const QImage flattened = blendImages(smoothed, median, settings.flattenStrength);
    const QuantizeResult quantized = quantizeHsv(flattened, settings.colors,
                                                 settings.quantizationIterations,
                                                 settings.minimumColorFraction);
    return {
        restoreSaturationAndDetail(quantized.image, flattened, settings)
            .convertToFormat(QImage::Format_ARGB32_Premultiplied),
        quantized.retainedColorCount,
    };
}

QImage preprocessImage(const QImage &source, const ImagePreprocessSettings &settings)
{
    return preprocessImageDetailed(source, settings).image;
}

} // namespace gui
