#include "region_extract.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include <potracelib.h>
}

namespace gui {
namespace {

constexpr int kPotraceWordBits = static_cast<int>(sizeof(potrace_word) * 8);

// potrace works in a y-up space with the bitmap's bottom-left at the origin;
// map a traced point back to image-pixel space (y-down). Derivation: a 1px
// top/bottom margin is added, and the image's top row is placed at the high-y
// end of the bitmap, so image_y = bounds.top - 1 + (bitmapHeight - p.y).
QPointF potracePoint(const potrace_dpoint_t &point, const QRect &bounds, int bitmapHeight) {
    return QPointF(bounds.left() - 1 + point.x,
                   bounds.top() - 1 + (bitmapHeight - point.y));
}

QPainterPath tracePotraceBitmap(const std::function<bool(int, int)> &set,
                                const QRect &bounds,
                                const RegionExtractionParams &params) {
    const int bitmapWidth = bounds.width() + 2;
    const int bitmapHeight = bounds.height() + 2;
    const int wordsPerRow = (bitmapWidth + kPotraceWordBits - 1) / kPotraceWordBits;
    std::vector<potrace_word> map(static_cast<size_t>(wordsPerRow) * bitmapHeight, 0);
    for (int localY = 0; localY < bounds.height(); ++localY) {
        const int imageY = bounds.top() + localY;
        const int bitmapY = bitmapHeight - 2 - localY; // flip y, 1px top margin
        for (int localX = 0; localX < bounds.width(); ++localX) {
            const int imageX = bounds.left() + localX;
            if (!set(imageX, imageY)) {
                continue;
            }
            const int bitmapX = localX + 1;
            map[static_cast<size_t>(bitmapY) * wordsPerRow + bitmapX / kPotraceWordBits]
                |= (static_cast<potrace_word>(1) << (kPotraceWordBits - 1 - bitmapX % kPotraceWordBits));
        }
    }

    potrace_bitmap_t bitmap;
    bitmap.w = bitmapWidth;
    bitmap.h = bitmapHeight;
    bitmap.dy = wordsPerRow;
    bitmap.map = map.data();

    potrace_param_t *param = potrace_param_default();
    if (param == nullptr) {
        return {};
    }
    param->turdsize = params.traceSpeckle;
    param->alphamax = params.traceAlphaMax;
    param->opttolerance = params.traceOptTolerance;

    potrace_state_t *state = potrace_trace(param, &bitmap);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    if (state != nullptr && state->status == POTRACE_STATUS_OK) {
        for (const potrace_path_t *p = state->plist; p != nullptr; p = p->next) {
            const potrace_curve_t &curve = p->curve;
            const int segments = curve.n;
            if (segments <= 0) {
                continue;
            }
            path.moveTo(potracePoint(curve.c[segments - 1][2], bounds, bitmapHeight));
            for (int i = 0; i < segments; ++i) {
                if (curve.tag[i] == POTRACE_CORNER) {
                    path.lineTo(potracePoint(curve.c[i][1], bounds, bitmapHeight));
                    path.lineTo(potracePoint(curve.c[i][2], bounds, bitmapHeight));
                } else {
                    path.cubicTo(potracePoint(curve.c[i][0], bounds, bitmapHeight),
                                 potracePoint(curve.c[i][1], bounds, bitmapHeight),
                                 potracePoint(curve.c[i][2], bounds, bitmapHeight));
                }
            }
            path.closeSubpath();
        }
    }
    if (state != nullptr) {
        potrace_state_free(state);
    }
    potrace_param_free(param);
    return path;
}

QPainterPath tracePotrace(const std::vector<int> &labels,
                          int width,
                          int label,
                          const QRect &bounds,
                          const RegionExtractionParams &params) {
    return tracePotraceBitmap([&](int x, int y) {
        return labels[static_cast<size_t>(y) * width + x] == label;
    }, bounds, params);
}

double luminance(const QColor &color) {
    return (0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF());
}

struct PaletteBucket {
    quint64 count = 0;
    quint64 rSum = 0;
    quint64 gSum = 0;
    quint64 bSum = 0;
};

QVector<QColor> buildPalette(const QImage &image, int alpha, int colorCount,
                             double mergeDistance, double frequencyFloor) {
    QHash<quint32, PaletteBucket> buckets;
    buckets.reserve(4096);
    quint64 opaquePixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = row[x];
            if (qAlpha(pixel) < alpha) {
                continue;
            }
            const int r = qRed(pixel);
            const int g = qGreen(pixel);
            const int b = qBlue(pixel);
            const quint32 key = (static_cast<quint32>(r >> 3) << 10)
                | (static_cast<quint32>(g >> 3) << 5)
                | static_cast<quint32>(b >> 3);
            PaletteBucket &bucket = buckets[key];
            ++bucket.count;
            bucket.rSum += r;
            bucket.gSum += g;
            bucket.bSum += b;
            ++opaquePixels;
        }
    }
    QList<PaletteBucket> ordered = buckets.values();
    std::sort(ordered.begin(), ordered.end(), [](const PaletteBucket &a, const PaletteBucket &b) {
        return a.count > b.count;
    });

    const quint64 floor = static_cast<quint64>(
        std::max(1.0, frequencyFloor * static_cast<double>(opaquePixels)));
    const double mergeDistanceSq = mergeDistance * mergeDistance;
    QVector<QColor> palette;
    palette.reserve(std::min(colorCount, static_cast<int>(ordered.size())));
    for (const PaletteBucket &bucket : ordered) {
        if (palette.size() >= colorCount) {
            break;
        }
        if (bucket.count < floor && !palette.isEmpty()) {
            break;
        }
        const QColor candidate(static_cast<int>(bucket.rSum / bucket.count),
                               static_cast<int>(bucket.gSum / bucket.count),
                               static_cast<int>(bucket.bSum / bucket.count));
        bool distinct = true;
        for (const QColor &chosen : palette) {
            const double dr = candidate.red() - chosen.red();
            const double dg = candidate.green() - chosen.green();
            const double db = candidate.blue() - chosen.blue();
            if (dr * dr + dg * dg + db * db < mergeDistanceSq) {
                distinct = false;
                break;
            }
        }
        if (distinct) {
            palette.push_back(candidate);
        }
    }
    if (palette.isEmpty() && !ordered.isEmpty()) {
        const PaletteBucket &bucket = ordered.front();
        palette.push_back(QColor(static_cast<int>(bucket.rSum / bucket.count),
                                 static_cast<int>(bucket.gSum / bucket.count),
                                 static_cast<int>(bucket.bSum / bucket.count)));
    }
    return palette;
}

int nearestPaletteIndex(const QVector<QColor> &palette, QRgb pixel) {
    const int r = qRed(pixel);
    const int g = qGreen(pixel);
    const int b = qBlue(pixel);
    int best = 0;
    qint64 bestDistance = std::numeric_limits<qint64>::max();
    for (int i = 0; i < palette.size(); ++i) {
        const qint64 dr = r - palette[i].red();
        const qint64 dg = g - palette[i].green();
        const qint64 db = b - palette[i].blue();
        const qint64 distance = dr * dr + dg * dg + db * db;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

QImage boxBlur(const QImage &input) {
    const QImage src = input.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = src.width();
    const int height = src.height();
    QImage out(width, height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < height; ++y) {
        const QRgb *rows[3] = {
            reinterpret_cast<const QRgb *>(src.constScanLine(std::max(0, y - 1))),
            reinterpret_cast<const QRgb *>(src.constScanLine(y)),
            reinterpret_cast<const QRgb *>(src.constScanLine(std::min(height - 1, y + 1))),
        };
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int x0 = std::max(0, x - 1);
            const int x1 = x;
            const int x2 = std::min(width - 1, x + 1);
            int a = 0;
            int r = 0;
            int g = 0;
            int b = 0;
            for (const QRgb *row : rows) {
                for (int xx : {x0, x1, x2}) {
                    const QRgb pixel = row[xx];
                    a += qAlpha(pixel);
                    r += qRed(pixel);
                    g += qGreen(pixel);
                    b += qBlue(pixel);
                }
            }
            dst[x] = qRgba(r / 9, g / 9, b / 9, a / 9);
        }
    }
    return out;
}

QImage preprocess(const QImage &image, const RegionExtractionParams &params) {
    QImage working = image;
    if (params.maxDimension > 0) {
        const int longest = std::max(working.width(), working.height());
        if (longest > params.maxDimension) {
            const double scale = static_cast<double>(params.maxDimension) / longest;
            working = working.scaled(std::max(1, qRound(working.width() * scale)),
                                     std::max(1, qRound(working.height() * scale)),
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
        }
    }
    for (int pass = 0; pass < params.blurPasses; ++pass) {
        working = boxBlur(working);
    }
    return working.convertToFormat(QImage::Format_ARGB32);
}

struct RegionAccum {
    int paletteIndex = 0;
    int area = 0;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
};

struct PassResult {
    std::vector<int> labels;          // per pixel, -1 = background/excluded
    std::vector<RegionAccum> accums;  // per label
    QVector<QColor> palette;
    std::vector<char> lineart;        // per label (1 = classified lineart)
    std::vector<int> maxDistance;     // per label, city-block half-width
    std::vector<QSet<int>> adjacency; // per label
    double widthCap = 0.0;
    int mergedSmallRegions = 0;
};

int mergeSmallRegions(PassResult *pass, int width, int height, int areaThreshold) {
    const int threshold = std::max(0, areaThreshold);
    const int labelCount = static_cast<int>(pass->accums.size());
    if (threshold <= 1 || labelCount < 2) {
        return 0;
    }

    std::vector<int> parent(static_cast<size_t>(labelCount));
    std::vector<int> rootArea(static_cast<size_t>(labelCount));
    std::vector<QHash<int, int>> boundaryCounts(static_cast<size_t>(labelCount));
    for (int label = 0; label < labelCount; ++label) {
        parent[static_cast<size_t>(label)] = label;
        rootArea[static_cast<size_t>(label)] = pass->accums[static_cast<size_t>(label)].area;
    }
    const auto addBoundary = [&](int left, int right) {
        if (left >= 0 && right >= 0 && left != right) {
            ++boundaryCounts[static_cast<size_t>(left)][right];
            ++boundaryCounts[static_cast<size_t>(right)][left];
        }
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int label = pass->labels[static_cast<size_t>(y) * width + x];
            if (x + 1 < width) {
                addBoundary(label, pass->labels[static_cast<size_t>(y) * width + x + 1]);
            }
            if (y + 1 < height) {
                addBoundary(label, pass->labels[static_cast<size_t>(y + 1) * width + x]);
            }
        }
    }
    const auto findRoot = [&](int label) {
        int root = label;
        while (parent[static_cast<size_t>(root)] != root) {
            root = parent[static_cast<size_t>(root)];
        }
        int current = label;
        while (parent[static_cast<size_t>(current)] != current) {
            const int next = parent[static_cast<size_t>(current)];
            parent[static_cast<size_t>(current)] = root;
            current = next;
        }
        return root;
    };

    QVector<int> order;
    order.reserve(labelCount);
    for (int label = 0; label < labelCount; ++label) {
        order.push_back(label);
    }
    std::sort(order.begin(), order.end(), [&](int left, int right) {
        if (pass->accums[static_cast<size_t>(left)].area
            != pass->accums[static_cast<size_t>(right)].area) {
            return pass->accums[static_cast<size_t>(left)].area
                < pass->accums[static_cast<size_t>(right)].area;
        }
        return left < right;
    });

    int merged = 0;
    for (const int label : order) {
        const int source = findRoot(label);
        if (source != label || rootArea[static_cast<size_t>(source)] >= threshold) {
            continue;
        }
        QHash<int, int> resolvedBoundary;
        for (auto it = boundaryCounts[static_cast<size_t>(source)].cbegin();
             it != boundaryCounts[static_cast<size_t>(source)].cend(); ++it) {
            const int neighbor = findRoot(it.key());
            if (neighbor != source) {
                resolvedBoundary[neighbor] += it.value();
            }
        }
        if (resolvedBoundary.isEmpty()) {
            continue;
        }

        const QColor sourceColor = pass->palette[
            pass->accums[static_cast<size_t>(source)].paletteIndex];
        int best = -1;
        qint64 bestDistance = std::numeric_limits<qint64>::max();
        int bestBoundary = -1;
        int bestArea = -1;
        for (auto it = resolvedBoundary.cbegin(); it != resolvedBoundary.cend(); ++it) {
            const int neighbor = it.key();
            const QColor neighborColor = pass->palette[
                pass->accums[static_cast<size_t>(neighbor)].paletteIndex];
            const qint64 dr = sourceColor.red() - neighborColor.red();
            const qint64 dg = sourceColor.green() - neighborColor.green();
            const qint64 db = sourceColor.blue() - neighborColor.blue();
            const qint64 distance = dr * dr + dg * dg + db * db;
            const int neighborArea = rootArea[static_cast<size_t>(neighbor)];
            if (distance < bestDistance
                || (distance == bestDistance && it.value() > bestBoundary)
                || (distance == bestDistance && it.value() == bestBoundary
                    && neighborArea > bestArea)
                || (distance == bestDistance && it.value() == bestBoundary
                    && neighborArea == bestArea && neighbor < best)) {
                best = neighbor;
                bestDistance = distance;
                bestBoundary = it.value();
                bestArea = neighborArea;
            }
        }
        if (best < 0) {
            continue;
        }
        parent[static_cast<size_t>(source)] = best;
        rootArea[static_cast<size_t>(best)] += rootArea[static_cast<size_t>(source)];
        for (auto it = boundaryCounts[static_cast<size_t>(source)].cbegin();
             it != boundaryCounts[static_cast<size_t>(source)].cend(); ++it) {
            boundaryCounts[static_cast<size_t>(best)][it.key()] += it.value();
        }
        ++merged;
    }
    if (merged == 0) {
        return 0;
    }

    QHash<int, int> compactLabel;
    std::vector<RegionAccum> compactAccums;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int oldLabel = pass->labels[index];
            if (oldLabel < 0) {
                continue;
            }
            const int root = findRoot(oldLabel);
            auto found = compactLabel.find(root);
            int newLabel = 0;
            if (found == compactLabel.end()) {
                newLabel = static_cast<int>(compactAccums.size());
                compactLabel.insert(root, newLabel);
                RegionAccum accum;
                accum.paletteIndex = pass->accums[static_cast<size_t>(root)].paletteIndex;
                accum.minX = accum.maxX = x;
                accum.minY = accum.maxY = y;
                compactAccums.push_back(accum);
            } else {
                newLabel = found.value();
            }
            pass->labels[index] = newLabel;
            RegionAccum &accum = compactAccums[static_cast<size_t>(newLabel)];
            ++accum.area;
            accum.minX = std::min(accum.minX, x);
            accum.minY = std::min(accum.minY, y);
            accum.maxX = std::max(accum.maxX, x);
            accum.maxY = std::max(accum.maxY, y);
        }
    }
    pass->accums = std::move(compactAccums);
    return merged;
}

PassResult segmentPass(const QImage &image,
                       int width,
                       int height,
                       int alpha,
                       int colorCount,
                       const RegionExtractionParams &params) {
    PassResult pass;
    pass.palette = buildPalette(image, alpha, std::max(1, colorCount),
                                params.colorMergeDistance, params.colorFrequencyFloor);
    if (pass.palette.isEmpty()) {
        return pass;
    }
    const size_t pixelCount = static_cast<size_t>(width) * height;

    std::vector<int> paletteIndex(pixelCount, -1);
    for (int y = 0; y < height; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(row[x]) < alpha) {
                continue;
            }
            paletteIndex[static_cast<size_t>(y) * width + x] = nearestPaletteIndex(pass.palette, row[x]);
        }
    }

    pass.labels.assign(pixelCount, -1);
    std::vector<int> stack;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t seed = static_cast<size_t>(y) * width + x;
            if (paletteIndex[seed] < 0 || pass.labels[seed] >= 0) {
                continue;
            }
            const int paletteId = paletteIndex[seed];
            const int label = static_cast<int>(pass.accums.size());
            RegionAccum accum;
            accum.paletteIndex = paletteId;
            accum.minX = accum.maxX = x;
            accum.minY = accum.maxY = y;
            stack.clear();
            stack.push_back(static_cast<int>(seed));
            pass.labels[seed] = label;
            while (!stack.empty()) {
                const int index = stack.back();
                stack.pop_back();
                const int px = index % width;
                const int py = index / width;
                ++accum.area;
                accum.minX = std::min(accum.minX, px);
                accum.minY = std::min(accum.minY, py);
                accum.maxX = std::max(accum.maxX, px);
                accum.maxY = std::max(accum.maxY, py);
                const int neighbors[4][2] = {{px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1}};
                for (const auto &neighbor : neighbors) {
                    const int nx = neighbor[0];
                    const int ny = neighbor[1];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const size_t nIndex = static_cast<size_t>(ny) * width + nx;
                    if (pass.labels[nIndex] < 0 && paletteIndex[nIndex] == paletteId) {
                        pass.labels[nIndex] = label;
                        stack.push_back(static_cast<int>(nIndex));
                    }
                }
            }
            pass.accums.push_back(accum);
        }
    }
    if (pass.accums.empty()) {
        return pass;
    }

    pass.mergedSmallRegions = mergeSmallRegions(
        &pass, width, height, params.smallRegionMergeArea);

    pass.adjacency.assign(pass.accums.size(), {});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int label = pass.labels[static_cast<size_t>(y) * width + x];
            if (label < 0) {
                continue;
            }
            const auto neighborLabel = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                    return -1;
                }
                return pass.labels[static_cast<size_t>(ny) * width + nx];
            };
            for (const auto &neighbor : {std::pair{x - 1, y}, std::pair{x + 1, y},
                                         std::pair{x, y - 1}, std::pair{x, y + 1}}) {
                const int other = neighborLabel(neighbor.first, neighbor.second);
                if (other >= 0 && other != label) {
                    pass.adjacency[label].insert(other);
                }
            }
        }
    }

    double lumWeighted = 0.0;
    double areaTotal = 0.0;
    for (const RegionAccum &accum : pass.accums) {
        lumWeighted += luminance(pass.palette[accum.paletteIndex]) * accum.area;
        areaTotal += accum.area;
    }
    const double meanLum = areaTotal > 0.0 ? lumWeighted / areaTotal : 0.5;
    pass.widthCap = std::max(params.lineWidthCapFloor,
                             std::min(width, height) * params.lineWidthCapFraction);

    const int distanceInfinity = width + height + 1;
    std::vector<int> distance(pixelCount, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass.labels[index];
            if (label < 0) {
                continue;
            }
            const bool boundary = (x == 0 || pass.labels[index - 1] != label)
                || (x == width - 1 || pass.labels[index + 1] != label)
                || (y == 0 || pass.labels[index - width] != label)
                || (y == height - 1 || pass.labels[index + width] != label);
            distance[index] = boundary ? 0 : distanceInfinity;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass.labels[index];
            if (label < 0 || distance[index] == 0) {
                continue;
            }
            int best = distance[index];
            if (x > 0 && pass.labels[index - 1] == label) {
                best = std::min(best, distance[index - 1] + 1);
            }
            if (y > 0 && pass.labels[index - width] == label) {
                best = std::min(best, distance[index - width] + 1);
            }
            distance[index] = best;
        }
    }
    pass.maxDistance.assign(pass.accums.size(), 0);
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass.labels[index];
            if (label < 0) {
                continue;
            }
            int best = distance[index];
            if (best != 0) {
                if (x < width - 1 && pass.labels[index + 1] == label) {
                    best = std::min(best, distance[index + 1] + 1);
                }
                if (y < height - 1 && pass.labels[index + width] == label) {
                    best = std::min(best, distance[index + width] + 1);
                }
                distance[index] = best;
            }
            pass.maxDistance[label] = std::max(pass.maxDistance[label], best);
        }
    }

    pass.lineart.assign(pass.accums.size(), 0);
    for (int label = 0; label < static_cast<int>(pass.accums.size()); ++label) {
        const RegionAccum &accum = pass.accums[label];
        if (accum.area < params.minRegionArea) {
            continue;
        }
        const double halfWidth = std::max(0.5, static_cast<double>(pass.maxDistance[label]));
        if (2.0 * halfWidth > pass.widthCap) {
            continue;
        }
        const double elongation = accum.area / (4.0 * halfWidth * halfWidth);
        const double contrast = std::abs(luminance(pass.palette[accum.paletteIndex]) - meanLum);
        int bigNeighbors = 0;
        for (int neighbor : pass.adjacency[label]) {
            if (pass.accums[neighbor].area >= std::max<int>(params.minRegionArea,
                                                            static_cast<int>(1.5 * accum.area))) {
                ++bigNeighbors;
            }
        }
        const bool separates = bigNeighbors >= 2;
        if (separates
            || (contrast >= params.contrastThreshold && elongation >= params.minElongation)) {
            pass.lineart[label] = 1;
        }
    }
    return pass;
}

} // namespace

RegionExtractionResult extractRegions(const QImage &sourceImage,
                                      const RegionExtractionParams &params) {
    RegionExtractionResult result;
    if (sourceImage.isNull() || sourceImage.width() < 1 || sourceImage.height() < 1) {
        result.error = QStringLiteral("The guide layer has no image data");
        return result;
    }

    const QImage image = preprocess(sourceImage, params);
    const int width = image.width();
    const int height = image.height();
    result.imageSize = QSize(width, height);
    const int alpha = std::clamp(static_cast<int>(params.alphaThreshold * 255.0), 0, 255);

    const QVector<QColor> naturalPalette = buildPalette(image, alpha, params.maxColorCount,
                                                        params.colorMergeDistance,
                                                        params.colorFrequencyFloor);
    const int maxColors = std::clamp(static_cast<int>(naturalPalette.size()),
                                     2, std::max(2, params.maxColorCount));
    const size_t pixelCount = static_cast<size_t>(width) * height;

    std::vector<bool> lineartMask(pixelCount, false);
    PassResult finalSeg;
    bool haveFinal = false;
    for (int colorCount = 2; colorCount <= maxColors; ++colorCount) {
        PassResult pass = segmentPass(image, width, height, alpha, colorCount, params);
        if (pass.accums.empty()) {
            continue;
        }
        for (size_t i = 0; i < pixelCount; ++i) {
            const int label = pass.labels[i];
            if (label >= 0 && pass.lineart[label]) {
                lineartMask[i] = true;
            }
        }
        if (colorCount == maxColors) {
            finalSeg = std::move(pass);
            haveFinal = true;
        }
    }
    if (!haveFinal || finalSeg.accums.empty()) {
        result.error = QStringLiteral("The guide layer produced no regions");
        return result;
    }
    result.mergedSmallRegionCount = finalSeg.mergedSmallRegions;

    std::vector<QSet<int>> colorAdj = finalSeg.adjacency;
    for (int label = 0; label < static_cast<int>(finalSeg.accums.size()); ++label) {
        if (2.0 * finalSeg.maxDistance[label] > finalSeg.widthCap) {
            continue;
        }
        const QList<int> around(finalSeg.adjacency[label].begin(), finalSeg.adjacency[label].end());
        for (int i = 0; i < around.size(); ++i) {
            for (int j = i + 1; j < around.size(); ++j) {
                colorAdj[around[i]].insert(around[j]);
                colorAdj[around[j]].insert(around[i]);
            }
        }
    }
    std::vector<int> colorOf(finalSeg.accums.size(), -1);
    std::vector<int> colorOrder(finalSeg.accums.size());
    for (int i = 0; i < static_cast<int>(finalSeg.accums.size()); ++i) {
        colorOrder[i] = i;
    }
    std::sort(colorOrder.begin(), colorOrder.end(), [&](int a, int b) {
        return colorAdj[a].size() > colorAdj[b].size();
    });
    for (int label : colorOrder) {
        QSet<int> usedColors;
        for (int neighbor : colorAdj[label]) {
            if (colorOf[neighbor] >= 0) {
                usedColors.insert(colorOf[neighbor]);
            }
        }
        int color = 0;
        while (usedColors.contains(color)) {
            ++color;
        }
        colorOf[label] = color;
    }

    for (int label = 0; label < static_cast<int>(finalSeg.accums.size()); ++label) {
        const RegionAccum &accum = finalSeg.accums[label];
        if (accum.area < params.minRegionArea || finalSeg.lineart[label]) {
            continue;
        }
        const QRect bounds(accum.minX, accum.minY,
                           accum.maxX - accum.minX + 1,
                           accum.maxY - accum.minY + 1);
        QPainterPath outline = tracePotrace(finalSeg.labels, width, label, bounds, params);
        if (outline.isEmpty()) {
            continue;
        }
        ExtractedRegion region;
        region.id = label;
        region.color = finalSeg.palette[accum.paletteIndex];
        region.outline = outline;
        region.bounds = bounds;
        region.area = accum.area;
        region.debugColor = colorOf[label];
        region.strokeWidth = 2.0 * std::max(0.5, static_cast<double>(finalSeg.maxDistance[label]));
        region.lineart = false;
        result.regions.push_back(std::move(region));
        ++result.colorRegionCount;
    }

    std::vector<int> lineLabels(pixelCount, -1);
    struct LineAccum {
        int area = 0;
        int minX = 0;
        int minY = 0;
        int maxX = 0;
        int maxY = 0;
        double rSum = 0.0;
        double gSum = 0.0;
        double bSum = 0.0;
    };
    std::vector<LineAccum> lineAccums;
    std::vector<int> lineStack;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t seed = static_cast<size_t>(y) * width + x;
            if (!lineartMask[seed] || lineLabels[seed] >= 0) {
                continue;
            }
            const int compId = static_cast<int>(lineAccums.size());
            LineAccum accum;
            accum.minX = accum.maxX = x;
            accum.minY = accum.maxY = y;
            lineStack.clear();
            lineStack.push_back(static_cast<int>(seed));
            lineLabels[seed] = compId;
            while (!lineStack.empty()) {
                const int index = lineStack.back();
                lineStack.pop_back();
                const int px = index % width;
                const int py = index / width;
                ++accum.area;
                accum.minX = std::min(accum.minX, px);
                accum.minY = std::min(accum.minY, py);
                accum.maxX = std::max(accum.maxX, px);
                accum.maxY = std::max(accum.maxY, py);
                const QRgb pixel = reinterpret_cast<const QRgb *>(image.constScanLine(py))[px];
                accum.rSum += qRed(pixel);
                accum.gSum += qGreen(pixel);
                accum.bSum += qBlue(pixel);
                const int neighbors[4][2] = {{px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1}};
                for (const auto &neighbor : neighbors) {
                    const int nx = neighbor[0];
                    const int ny = neighbor[1];
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const size_t nIndex = static_cast<size_t>(ny) * width + nx;
                    if (lineartMask[nIndex] && lineLabels[nIndex] < 0) {
                        lineLabels[nIndex] = compId;
                        lineStack.push_back(static_cast<int>(nIndex));
                    }
                }
            }
            lineAccums.push_back(accum);
        }
    }
    for (int compId = 0; compId < static_cast<int>(lineAccums.size()); ++compId) {
        const LineAccum &accum = lineAccums[compId];
        if (accum.area < params.minRegionArea) {
            continue;
        }
        const QRect bounds(accum.minX, accum.minY,
                           accum.maxX - accum.minX + 1,
                           accum.maxY - accum.minY + 1);
        QPainterPath outline = tracePotrace(lineLabels, width, compId, bounds, params);
        if (outline.isEmpty()) {
            continue;
        }
        ExtractedRegion region;
        region.id = compId;
        region.color = QColor(static_cast<int>(accum.rSum / accum.area),
                              static_cast<int>(accum.gSum / accum.area),
                              static_cast<int>(accum.bSum / accum.area));
        region.outline = outline;
        region.bounds = bounds;
        region.area = accum.area;
        region.strokeWidth = 0.0;
        region.lineart = true;
        result.regions.push_back(std::move(region));
        ++result.lineartRegionCount;
    }

    if (result.regions.isEmpty()) {
        result.error = QStringLiteral("No regions survived the minimum-area filter");
    }
    return result;
}

QPainterPath traceMaskToPath(const std::vector<std::uint8_t> &mask, int width, int height,
                             const QRect &bounds, const RegionExtractionParams &params) {
    const QRect clipped = bounds.intersected(QRect(0, 0, width, height));
    if (clipped.isEmpty()) {
        return {};
    }
    return tracePotraceBitmap([&](int x, int y) {
        return mask[static_cast<size_t>(y) * width + x] != 0;
    }, clipped, params);
}

} // namespace gui
