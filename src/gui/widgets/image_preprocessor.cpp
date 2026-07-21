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
    QVector<QColor> retainedPalette;
};

constexpr double kEpsilon = 1e-9;

HsvColor rgbToHsv(int red, int green, int blue) {
    QColor color(red, green, blue);
    float hue = 0.0F;
    float saturation = 0.0F;
    float value = 0.0F;
    color.getHsvF(&hue, &saturation, &value);
    return {hue < 0.0 ? 0.0 : hue, saturation, value};
}

QRgb hsvToRgb(const HsvColor &hsv) {
    return QColor::fromHsvF(hsv.h - std::floor(hsv.h),
                            std::clamp(hsv.s, 0.0, 1.0),
                            std::clamp(hsv.v, 0.0, 1.0)).rgb();
}

double hsvDistanceSquared(const HsvColor &left, const HsvColor &right) {
    double hueDistance = std::abs(left.h - right.h);
    hueDistance = std::min(hueDistance, 1.0 - hueDistance);
    const double hueGate = std::sqrt(std::max(0.0, left.s * right.s));
    const double ds = left.s - right.s;
    const double dv = left.v - right.v;
    return 4.0 * hueGate * hueDistance * hueDistance
        + 0.35 * ds * ds
        + 0.65 * dv * dv;
}

int colorBin(QRgb pixel) {
    return ((qRed(pixel) >> 3) << 10) | ((qGreen(pixel) >> 3) << 5) | (qBlue(pixel) >> 3);
}

QImage normalizeBinaryAlpha(const QImage &source) {
    QImage result = source.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            if (qAlpha(line[x]) > 0) {
                line[x] = qRgb(qRed(line[x]), qGreen(line[x]), qBlue(line[x]));
            } else {
                line[x] = qRgba(0, 0, 0, 0);
            }
        }
    }
    return result;
}

QVector<QColor> normalizedPalette(const QVector<QColor> &colors) {
    QVector<QColor> result;
    result.reserve(std::min(256, static_cast<int>(colors.size())));
    for (const QColor &input : colors) {
        if (!input.isValid()) {
            continue;
        }
        const QColor color(input.red(), input.green(), input.blue(), 255);
        const bool duplicate = std::any_of(result.cbegin(), result.cend(), [&](const QColor &existing) {
            return existing.rgb() == color.rgb();
        });
        if (!duplicate) {
            result.push_back(color);
            if (result.size() == 256) {
                break;
            }
        }
    }
    return result;
}

QuantizeResult quantizeToFixedPalette(const QImage &source, const QVector<QColor> &inputPalette) {
    constexpr int histogramSize = 32 * 32 * 32;
    const QVector<QColor> palette = normalizedPalette(inputPalette);
    if (palette.isEmpty()) {
        return {};
    }
    QVector<HsvColor> paletteHsv;
    paletteHsv.reserve(palette.size());
    for (const QColor &color : palette) {
        paletteHsv.push_back(rgbToHsv(color.red(), color.green(), color.blue()));
    }
    std::array<int, histogramSize> assignments;
    assignments.fill(-1);
    QImage result(source.size(), QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        const auto *input = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *output = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            if (qAlpha(input[x]) == 0) {
                output[x] = input[x];
                continue;
            }
            const int bin = colorBin(input[x]);
            int &assignment = assignments[static_cast<size_t>(bin)];
            if (assignment < 0) {
                const HsvColor hsv = rgbToHsv(qRed(input[x]), qGreen(input[x]), qBlue(input[x]));
                double nearest = std::numeric_limits<double>::max();
                for (int index = 0; index < paletteHsv.size(); ++index) {
                    const double distance = hsvDistanceSquared(hsv, paletteHsv[index]);
                    if (distance < nearest) {
                        nearest = distance;
                        assignment = index;
                    }
                }
            }
            const QColor color = palette[assignment];
            output[x] = qRgba(color.red(), color.green(), color.blue(), qAlpha(input[x]));
        }
    }
    return {result, static_cast<int>(palette.size()), palette};
}

QImage bilateralOneDimension(const QImage &source, int radius, double sigmaColor,
                             double sigmaSpace, bool horizontal) {
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

QImage bilateralSmooth(const QImage &source, const ImagePreprocessSettings &settings) {
    QImage result = source;
    const int radius = std::max(0, settings.smoothingDiameter / 2);
    for (int pass = 0; pass < std::max(0, settings.smoothingPasses); ++pass) {
        result = bilateralOneDimension(result, radius, settings.sigmaColor, settings.sigmaSpace, true);
        result = bilateralOneDimension(result, radius, settings.sigmaColor, settings.sigmaSpace, false);
    }
    return result;
}

QImage medianBlur(const QImage &source, int radius) {
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

QImage blendImages(const QImage &left, const QImage &right, double amount) {
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

QImage boxBlur(const QImage &source, int radius) {
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
                           double minimumColorFraction, const QVector<QColor> &lockedInput) {
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
    const QVector<QColor> lockedPalette = normalizedPalette(lockedInput);
    if (colors.empty()) {
        return {source, static_cast<int>(lockedPalette.size()), lockedPalette};
    }

    const int fixedCenterCount = lockedPalette.size();
    const int clusterCount = std::clamp(std::max(requestedColors, fixedCenterCount), 1,
                                        std::min(256, fixedCenterCount + static_cast<int>(colors.size())));
    std::vector<HsvColor> centers;
    centers.reserve(static_cast<size_t>(clusterCount));
    for (const QColor &color : lockedPalette) {
        centers.push_back(rgbToHsv(color.red(), color.green(), color.blue()));
    }
    const auto mostFrequent = std::max_element(colors.begin(), colors.end(), [](const auto &left, const auto &right) {
        return left.weight < right.weight;
    });
    if (centers.empty()) {
        centers.push_back(mostFrequent->hsv);
    }
    std::vector<double> nearestDistances(colors.size(), std::numeric_limits<double>::max());
    for (const HsvColor &center : centers) {
        for (size_t index = 0; index < colors.size(); ++index) {
            nearestDistances[index] = std::min(
                nearestDistances[index], hsvDistanceSquared(colors[index].hsv, center));
        }
    }
    while (static_cast<int>(centers.size()) < clusterCount) {
        double bestScore = -1.0;
        HsvColor best = colors.front().hsv;
        for (size_t index = 0; index < colors.size(); ++index) {
            const HistogramColor &color = colors[index];
            const double score = nearestDistances[index] * color.weight;
            if (score > bestScore) {
                bestScore = score;
                best = color.hsv;
            }
        }
        centers.push_back(best);
        for (size_t index = 0; index < colors.size(); ++index) {
            nearestDistances[index] = std::min(
                nearestDistances[index], hsvDistanceSquared(colors[index].hsv, best));
        }
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
            if (index < fixedCenterCount) {
                continue;
            }
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
        if (index < fixedCenterCount
            || (clusterWeights[static_cast<size_t>(index)] >= floor
                && clusterWeights[static_cast<size_t>(index)] > 0.0)) {
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
    QVector<QColor> retainedPalette;
    palette.reserve(retainedClusters.size());
    retainedPalette.reserve(static_cast<qsizetype>(retainedClusters.size()));
    for (int retained : retainedClusters) {
        const QColor color = retained < fixedCenterCount
            ? lockedPalette[retained]
            : QColor::fromRgb(hsvToRgb(centers[static_cast<size_t>(retained)]));
        palette.push_back(color.rgb());
        retainedPalette.push_back(color);
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
    return {result, static_cast<int>(palette.size()), retainedPalette};
}

double saturationOf(QRgb pixel) {
    const int maximum = std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)});
    const int minimum = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)});
    return maximum == 0 ? 0.0 : 255.0 * static_cast<double>(maximum - minimum) / maximum;
}

constexpr quint16 kTransparentLabel = std::numeric_limits<quint16>::max();

quint16 labelAt(const QImage &image, int x, int y) {
    return reinterpret_cast<const quint16 *>(image.constScanLine(y))[x];
}

int nearestPaletteLabel(QRgb pixel, const QVector<QColor> &palette) {
    const HsvColor hsv = rgbToHsv(qRed(pixel), qGreen(pixel), qBlue(pixel));
    double nearest = std::numeric_limits<double>::max();
    int nearestIndex = 0;
    for (int index = 0; index < palette.size(); ++index) {
        const QColor &candidate = palette[index];
        const double distance = hsvDistanceSquared(
            hsv, rgbToHsv(candidate.red(), candidate.green(), candidate.blue()));
        if (distance < nearest) {
            nearest = distance;
            nearestIndex = index;
        }
    }
    return nearestIndex;
}

QImage buildIndexImage(const QImage &quantized, const QVector<QColor> &palette) {
    QImage result(quantized.size(), QImage::Format_Grayscale16);
    QHash<QRgb, int> paletteIndices;
    for (int index = 0; index < palette.size(); ++index) {
        paletteIndices.insert(palette[index].rgb(), index);
    }
    for (int y = 0; y < quantized.height(); ++y) {
        const auto *source = reinterpret_cast<const QRgb *>(quantized.constScanLine(y));
        auto *labels = reinterpret_cast<quint16 *>(result.scanLine(y));
        for (int x = 0; x < quantized.width(); ++x) {
            if (qAlpha(source[x]) == 0) {
                labels[x] = kTransparentLabel;
                continue;
            }
            const auto found = paletteIndices.constFind(QColor::fromRgb(source[x]).rgb());
            labels[x] = static_cast<quint16>(found == paletteIndices.cend()
                ? nearestPaletteLabel(source[x], palette) : found.value());
        }
    }
    return result;
}

QImage renderIndexImage(const QImage &indexImage, const QVector<QColor> &palette) {
    QImage result(indexImage.size(), QImage::Format_ARGB32);
    for (int y = 0; y < indexImage.height(); ++y) {
        const auto *labels = reinterpret_cast<const quint16 *>(indexImage.constScanLine(y));
        auto *output = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < indexImage.width(); ++x) {
            if (labels[x] == kTransparentLabel) {
                output[x] = qRgba(0, 0, 0, 0);
            } else {
                const QColor &color = palette[labels[x]];
                output[x] = qRgb(color.red(), color.green(), color.blue());
            }
        }
    }
    return result;
}

struct EdgeData {
    std::vector<double> gradient;
    std::vector<quint8> core;
    std::vector<quint8> edgeMask;
    std::vector<double> distance;
};

double pixelLuminance(QRgb pixel) {
    return 0.2126 * qRed(pixel) + 0.7152 * qGreen(pixel) + 0.0722 * qBlue(pixel);
}

EdgeData detectEdges(const QImage &source) {
    const int width = source.width();
    const int height = source.height();
    const size_t count = static_cast<size_t>(width) * height;
    EdgeData result;
    result.gradient.assign(count, 0.0);
    result.core.assign(count, 0);
    result.edgeMask.assign(count, 0);
    result.distance.assign(count, 3.0);
    if (width <= 0 || height <= 0) {
        return result;
    }

    std::vector<double> luminance(count, 0.0);
    std::vector<quint8> visible(count, 0);
    for (int y = 0; y < height; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            visible[index] = qAlpha(line[x]) > 0;
            luminance[index] = pixelLuminance(line[x]);
        }
    }
    const auto sample = [&](int x, int y, int centerX, int centerY) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        const size_t index = static_cast<size_t>(y) * width + x;
        return visible[index] != 0 ? luminance[index]
                                   : luminance[static_cast<size_t>(centerY) * width + centerX];
    };
    double gradientSum = 0.0;
    int gradientCount = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (visible[index] == 0) {
                continue;
            }
            const double gx = -sample(x - 1, y - 1, x, y) + sample(x + 1, y - 1, x, y)
                - 2.0 * sample(x - 1, y, x, y) + 2.0 * sample(x + 1, y, x, y)
                - sample(x - 1, y + 1, x, y) + sample(x + 1, y + 1, x, y);
            const double gy = -sample(x - 1, y - 1, x, y) - 2.0 * sample(x, y - 1, x, y)
                - sample(x + 1, y - 1, x, y) + sample(x - 1, y + 1, x, y)
                + 2.0 * sample(x, y + 1, x, y) + sample(x + 1, y + 1, x, y);
            double magnitude = std::sqrt(gx * gx + gy * gy);
            for (int dy = -1; dy <= 1 && magnitude < 1020.0; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < width && ny < height
                        && visible[static_cast<size_t>(ny) * width + nx] == 0) {
                        magnitude = 1020.0;
                        break;
                    }
                }
            }
            result.gradient[index] = magnitude;
            gradientSum += magnitude;
            ++gradientCount;
        }
    }
    const double meanGradient = gradientCount > 0 ? gradientSum / gradientCount : 0.0;
    const double threshold = std::clamp(meanGradient * 1.6, 90.0, 320.0);
    for (size_t index = 0; index < count; ++index) {
        result.core[index] = visible[index] != 0 && result.gradient[index] >= threshold;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (visible[index] == 0) {
                continue;
            }
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height
                        || result.core[static_cast<size_t>(ny) * width + nx] == 0) {
                        continue;
                    }
                    const double distance = std::sqrt(static_cast<double>(dx * dx + dy * dy));
                    result.distance[index] = std::min(result.distance[index], distance);
                    if (std::abs(dx) <= 1 && std::abs(dy) <= 1) {
                        result.edgeMask[index] = 1;
                    }
                }
            }
        }
    }
    return result;
}

struct LineData {
    std::vector<quint8> lineMask;
    QColor derivedColor;
};

LineData detectLines(const QImage &source, const EdgeData &edges) {
    const int width = source.width();
    const int height = source.height();
    const size_t count = static_cast<size_t>(width) * height;
    LineData result;
    result.lineMask.assign(count, 0);
    std::vector<int> values;
    values.reserve(count);
    for (int y = 0; y < height; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(line[x]) == 0) {
                continue;
            }
            const double lum = pixelLuminance(line[x]);
            values.push_back(qRound(lum));
        }
    }
    if (values.empty()) {
        return result;
    }
    const size_t percentile = values.size() / 5;
    std::nth_element(values.begin(), values.begin() + static_cast<qsizetype>(percentile), values.end());
    const double darkThreshold = std::clamp(static_cast<double>(values[percentile] + 12), 35.0, 112.0);
    quint64 red = 0;
    quint64 green = 0;
    quint64 blue = 0;
    quint64 pixels = 0;
    for (int y = 0; y < height; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (qAlpha(line[x]) == 0 || pixelLuminance(line[x]) > darkThreshold) {
                continue;
            }
            double neighborMean = 0.0;
            int neighborCount = 0;
            for (const QPoint offset : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                const int nx = x + offset.x();
                const int ny = y + offset.y();
                if (nx >= 0 && ny >= 0 && nx < width && ny < height) {
                    const QRgb neighbor = source.pixel(nx, ny);
                    if (qAlpha(neighbor) > 0) {
                        neighborMean += pixelLuminance(neighbor);
                        ++neighborCount;
                    }
                }
            }
            neighborMean = neighborCount > 0 ? neighborMean / neighborCount : pixelLuminance(line[x]);
            const bool thinDarkRidge = neighborMean - pixelLuminance(line[x]) >= 22.0;
            if (edges.edgeMask[index] == 0 && !thinDarkRidge) {
                continue;
            }
            result.lineMask[index] = 1;
            red += qRed(line[x]);
            green += qGreen(line[x]);
            blue += qBlue(line[x]);
            ++pixels;
        }
    }
    if (pixels > 0) {
        result.derivedColor = QColor(static_cast<int>(red / pixels),
                                     static_cast<int>(green / pixels),
                                     static_cast<int>(blue / pixels));
    }
    return result;
}

QImage restoreLabelsWithDetail(const QImage &indexImage, const QVector<QColor> &palette,
                               const QImage &flattened, const ImagePreprocessSettings &settings,
                               const EdgeData &edges, const std::vector<quint8> &lineMask) {
    const QImage detailBlur = boxBlur(flattened, std::max(1, settings.detailRadius));
    QImage result = indexImage.copy();
    const double saturationAmount = std::clamp(settings.saturationRestore, 0.0, 1.0);
    const double detailAmount = std::clamp(settings.detailRestore, 0.0, 1.0);
    constexpr int histogramSize = 32 * 32 * 32;
    std::array<int, histogramSize> labelCache;
    labelCache.fill(-1);
    constexpr double transitionWidth = 32.0;
    for (int y = 0; y < flattened.height(); ++y) {
        const auto *sourceLabels = reinterpret_cast<const quint16 *>(indexImage.constScanLine(y));
        auto *outputLabels = reinterpret_cast<quint16 *>(result.scanLine(y));
        const auto *base = reinterpret_cast<const QRgb *>(flattened.constScanLine(y));
        const auto *blur = reinterpret_cast<const QRgb *>(detailBlur.constScanLine(y));
        for (int x = 0; x < flattened.width(); ++x) {
            const size_t index = static_cast<size_t>(y) * flattened.width() + x;
            if (sourceLabels[x] == kTransparentLabel || lineMask[index] != 0) {
                continue;
            }
            const QColor &quantized = palette[sourceLabels[x]];
            const double mask = std::clamp((saturationOf(base[x]) - settings.saturationThreshold) / transitionWidth, 0.0, 1.0);
            const double saturationBlend = saturationAmount * mask;
            double edgeWeight = std::clamp(edges.distance[index] / 2.0, 0.0, 1.0);
            if (settings.noDetailNearEdges && edges.edgeMask[index] != 0) {
                edgeWeight = 0.0;
            }
            const double red = quantized.red() * (1.0 - saturationBlend) + qRed(base[x]) * saturationBlend
                + detailAmount * edgeWeight * (qRed(base[x]) - qRed(blur[x]));
            const double green = quantized.green() * (1.0 - saturationBlend) + qGreen(base[x]) * saturationBlend
                + detailAmount * edgeWeight * (qGreen(base[x]) - qGreen(blur[x]));
            const double blue = quantized.blue() * (1.0 - saturationBlend) + qBlue(base[x]) * saturationBlend
                + detailAmount * edgeWeight * (qBlue(base[x]) - qBlue(blur[x]));
            const QRgb restored = qRgb(std::clamp(qRound(red), 0, 255),
                                       std::clamp(qRound(green), 0, 255),
                                       std::clamp(qRound(blue), 0, 255));
            int &cachedLabel = labelCache[static_cast<size_t>(colorBin(restored))];
            if (cachedLabel < 0) {
                cachedLabel = nearestPaletteLabel(restored, palette);
            }
            outputLabels[x] = static_cast<quint16>(cachedLabel);
        }
    }
    return result;
}

std::vector<int> componentSizes(const QImage &indexImage) {
    const int width = indexImage.width();
    const int height = indexImage.height();
    const int count = width * height;
    std::vector<int> sizes(static_cast<size_t>(count), 0);
    std::vector<quint8> visited(static_cast<size_t>(count), 0);
    std::vector<int> queue;
    std::vector<int> component;
    constexpr std::array<QPoint, 8> neighbors = {
        QPoint(-1, -1), QPoint(0, -1), QPoint(1, -1), QPoint(-1, 0),
        QPoint(1, 0), QPoint(-1, 1), QPoint(0, 1), QPoint(1, 1),
    };
    for (int seed = 0; seed < count; ++seed) {
        if (visited[static_cast<size_t>(seed)] != 0) {
            continue;
        }
        const quint16 label = labelAt(indexImage, seed % width, seed / width);
        visited[static_cast<size_t>(seed)] = 1;
        if (label == kTransparentLabel) {
            continue;
        }
        queue.assign(1, seed);
        component.clear();
        for (size_t head = 0; head < queue.size(); ++head) {
            const int index = queue[head];
            component.push_back(index);
            const int x = index % width;
            const int y = index / width;
            for (const QPoint &offset : neighbors) {
                const int nx = x + offset.x();
                const int ny = y + offset.y();
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                    continue;
                }
                const int neighbor = ny * width + nx;
                if (visited[static_cast<size_t>(neighbor)] == 0
                    && labelAt(indexImage, nx, ny) == label) {
                    visited[static_cast<size_t>(neighbor)] = 1;
                    queue.push_back(neighbor);
                }
            }
        }
        for (const int index : component) {
            sizes[static_cast<size_t>(index)] = static_cast<int>(component.size());
        }
    }
    return sizes;
}

QImage majorityCleanup(const QImage &input, const ImagePreprocessSettings &settings,
                       const std::vector<quint8> &lineMask, int lineLabel) {
    QImage result = input;
    const int width = result.width();
    const int height = result.height();
    const int passes = std::clamp(settings.edgeCleanupPasses, 0, 2);
    const int threshold = std::clamp(settings.speckleSize, 0, 64);
    const int radius = settings.edgeCleanupWindow >= 5 ? 2 : 1;
    for (int pass = 0; pass < passes && threshold > 0; ++pass) {
        const QImage snapshot = result;
        const std::vector<int> sizes = componentSizes(snapshot);
        for (int y = 0; y < height; ++y) {
            auto *output = reinterpret_cast<quint16 *>(result.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const size_t index = static_cast<size_t>(y) * width + x;
                const quint16 center = labelAt(snapshot, x, y);
                if (center == kTransparentLabel || lineMask[index] != 0
                    || sizes[index] > threshold) {
                    continue;
                }
                std::array<int, 256> votes{};
                bool boundary = false;
                for (int dy = -radius; dy <= radius; ++dy) {
                    const int sy = std::clamp(y + dy, 0, height - 1);
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int sx = std::clamp(x + dx, 0, width - 1);
                        const size_t sampleIndex = static_cast<size_t>(sy) * width + sx;
                        const quint16 label = labelAt(snapshot, sx, sy);
                        if (label == kTransparentLabel || lineMask[sampleIndex] != 0
                            || label == lineLabel) {
                            continue;
                        }
                        ++votes[label];
                        boundary = boundary || label != center;
                    }
                }
                if (!boundary) {
                    continue;
                }
                int majority = center;
                int bestVotes = votes[center];
                for (int label = 0; label < static_cast<int>(votes.size()); ++label) {
                    if (votes[static_cast<size_t>(label)] > bestVotes) {
                        majority = label;
                        bestVotes = votes[static_cast<size_t>(label)];
                    }
                }
                if (majority != center) {
                    output[x] = static_cast<quint16>(majority);
                }
            }
        }
    }
    return result;
}

void flattenEdgeBoundedRegions(QImage &indexImage, const ImagePreprocessSettings &settings,
                               const EdgeData &edges, const std::vector<quint8> &lineMask,
                               int lineLabel) {
    if (!settings.forceFlatFills) {
        return;
    }
    const int width = indexImage.width();
    const int height = indexImage.height();
    const int count = width * height;
    const int minimumArea = std::max(1, settings.flatFillMinimumArea);
    std::vector<quint8> visited(static_cast<size_t>(count), 0);
    std::vector<int> queue;
    std::vector<int> component;
    constexpr std::array<QPoint, 4> neighbors = {
        QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1),
    };
    for (int seed = 0; seed < count; ++seed) {
        if (visited[static_cast<size_t>(seed)] != 0 || edges.edgeMask[static_cast<size_t>(seed)] != 0
            || lineMask[static_cast<size_t>(seed)] != 0
            || labelAt(indexImage, seed % width, seed / width) == kTransparentLabel) {
            continue;
        }
        visited[static_cast<size_t>(seed)] = 1;
        queue.assign(1, seed);
        component.clear();
        std::array<int, 256> votes{};
        for (size_t head = 0; head < queue.size(); ++head) {
            const int index = queue[head];
            component.push_back(index);
            const quint16 label = labelAt(indexImage, index % width, index / width);
            if (label != lineLabel) {
                ++votes[label];
            }
            const int x = index % width;
            const int y = index / width;
            for (const QPoint &offset : neighbors) {
                const int nx = x + offset.x();
                const int ny = y + offset.y();
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                    continue;
                }
                const int neighbor = ny * width + nx;
                const size_t neighborIndex = static_cast<size_t>(neighbor);
                if (visited[neighborIndex] == 0 && edges.edgeMask[neighborIndex] == 0
                    && lineMask[neighborIndex] == 0
                    && labelAt(indexImage, nx, ny) != kTransparentLabel) {
                    visited[neighborIndex] = 1;
                    queue.push_back(neighbor);
                }
            }
        }
        if (static_cast<int>(component.size()) < minimumArea) {
            continue;
        }
        const int dominant = static_cast<int>(std::distance(
            votes.begin(), std::max_element(votes.begin(), votes.end())));
        if (votes[static_cast<size_t>(dominant)] == 0) {
            continue;
        }
        for (const int index : component) {
            reinterpret_cast<quint16 *>(indexImage.scanLine(index / width))[index % width]
                = static_cast<quint16>(dominant);
        }
    }
}

} // namespace

ImagePreprocessSettings ImagePreprocessSettings::animeDetail() {
    return {};
}

ImagePreprocessResult preprocessImageDetailed(const QImage &source,
                                              const ImagePreprocessSettings &settings) {
    if (source.isNull()) {
        return {};
    }
    const QImage input = normalizeBinaryAlpha(source);
    const QImage smoothed = bilateralSmooth(input, settings);
    const QImage median = medianBlur(smoothed, std::clamp(settings.flattenRadius, 0, 8));
    const QImage flattened = blendImages(smoothed, median, settings.flattenStrength);
    const EdgeData edges = detectEdges(smoothed);
    LineData lines;
    lines.lineMask.assign(static_cast<size_t>(input.width()) * input.height(), 0);
    if (settings.lineMode) {
        lines = detectLines(smoothed, edges);
    }

    QVector<QColor> clusteringPalette = normalizedPalette(settings.paletteColors);
    QColor lineColor;
    if (settings.lineMode && lines.derivedColor.isValid()) {
        lineColor = settings.lineColor.isValid()
            ? QColor(settings.lineColor.red(), settings.lineColor.green(), settings.lineColor.blue())
            : lines.derivedColor;
        const auto exactLine = std::find_if(
            clusteringPalette.cbegin(), clusteringPalette.cend(), [&](const QColor &color) {
                return color.rgb() == lineColor.rgb();
            });
        if (exactLine == clusteringPalette.cend()) {
            const bool hasGeneratedSlot = !settings.fixedPalette
                && clusteringPalette.size() < std::clamp(settings.colors, 1, 256);
            const bool explicitFixedLine = settings.fixedPalette && settings.lineColor.isValid();
            if ((hasGeneratedSlot || explicitFixedLine || clusteringPalette.isEmpty())
                && clusteringPalette.size() < 256) {
                clusteringPalette.push_back(lineColor);
            } else {
                lineColor = clusteringPalette[nearestPaletteLabel(lineColor.rgb(), clusteringPalette)];
            }
        }
    }
    const QuantizeResult quantized = settings.fixedPalette
        ? quantizeToFixedPalette(flattened, clusteringPalette)
        : quantizeHsv(flattened, settings.colors,
                      settings.quantizationIterations,
                      settings.minimumColorFraction,
                      clusteringPalette);
    if (quantized.image.isNull()) {
        return {};
    }
    if (quantized.retainedPalette.isEmpty()) {
        return {input.convertToFormat(QImage::Format_ARGB32_Premultiplied), 0, {}};
    }

    QImage indexImage = buildIndexImage(quantized.image, quantized.retainedPalette);
    int lineLabel = -1;
    if (settings.lineMode && lineColor.isValid()) {
        lineLabel = nearestPaletteLabel(lineColor.rgb(), quantized.retainedPalette);
        for (int y = 0; y < indexImage.height(); ++y) {
            auto *labels = reinterpret_cast<quint16 *>(indexImage.scanLine(y));
            for (int x = 0; x < indexImage.width(); ++x) {
                const size_t index = static_cast<size_t>(y) * indexImage.width() + x;
                if (lines.lineMask[index] != 0 && labels[x] != kTransparentLabel) {
                    labels[x] = static_cast<quint16>(lineLabel);
                }
            }
        }
    }
    indexImage = restoreLabelsWithDetail(indexImage, quantized.retainedPalette, flattened,
                                         settings, edges, lines.lineMask);
    indexImage = majorityCleanup(indexImage, settings, lines.lineMask, lineLabel);
    flattenEdgeBoundedRegions(indexImage, settings, edges, lines.lineMask, lineLabel);
    return {
        renderIndexImage(indexImage, quantized.retainedPalette)
            .convertToFormat(QImage::Format_ARGB32_Premultiplied),
        quantized.retainedColorCount,
        quantized.retainedPalette,
    };
}

QImage preprocessImage(const QImage &source, const ImagePreprocessSettings &settings) {
    return preprocessImageDetailed(source, settings).image;
}

} // namespace gui
