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
constexpr int kLineFringeBandRadius = 3;
constexpr int kLineFringeMinimumVotes = 2;
constexpr int kLineFringeMinimumStreak = 2;
constexpr double kLineFringeMinimumBandFraction = 0.8;
constexpr double kLineFringeMinimumContactFraction = 0.08;
constexpr double kLineFringeMinimumOverlapFraction = 0.05;
constexpr double kLineFringeMaximumBlendResidual = 18.0;
constexpr double kLineFringeMinimumTargetFraction = 0.05;
constexpr double kLineFringeMaximumTargetFraction = 0.95;
constexpr double kLineFringePersistentVoteFraction = 0.3;
constexpr double kLineFringePersistentStreakFraction = 0.2;
constexpr double kLineFringePalettePrior = 0.35;
constexpr double kLineFringePaletteMeanVoteFraction = 0.2;
constexpr double kLineFringePalettePersistentCoverage = 0.4;
constexpr double kLineFringeDirectMeanVoteFraction = 0.3;
constexpr double kLineFringeDirectPersistentCoverage = 0.55;
constexpr double kLineFringeBlendMeanVoteFraction = 0.2;
constexpr double kLineFringeBlendOverlapCoverage = 0.8;
constexpr int kResidualLineWidthMultiplier = 2;
constexpr int kResidualLineExtendedWidthMultiplier = 3;
constexpr double kResidualLinePalettePrior = 0.35;
constexpr double kResidualLineMinimumOverlapCoverage = 0.25;
constexpr double kResidualLineMinimumMeanVoteFraction = 0.05;
constexpr double kResidualLineExtendedOverlapCoverage = 0.9;
constexpr double kResidualLineExtendedPersistentCoverage = 0.3;
constexpr double kResidualLineHistoricalOverlapCoverage = 0.9;
constexpr double kResidualLineHistoricalMeanVoteFraction = 0.05;
constexpr double kResidualLineDirectPalettePrior = 0.1;
constexpr double kResidualLineDirectOverlapCoverage = 0.2;
constexpr double kResidualLineDirectMeanVoteFraction = 0.02;
constexpr double kResidualLineDirectContactFraction = 0.1;

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

struct ExtractionPassDiagnostic {
    int requestedColors = 0;
    int paletteColors = 0;
    int regions = 0;
    int lineartRegions = 0;
    int accumulatedLineartPixels = 0;
    int mergedSmallRegions = 0;
};

struct RegionDiagnosticMetrics {
    QHash<int, int> neighborEdges;
    qint64 lineRedSum = 0;
    qint64 lineGreenSum = 0;
    qint64 lineBlueSum = 0;
    int lineSamples = 0;
    int lineOverlapPixels = 0;
    int lineBoundaryEdges = 0;
    int perimeterEdges = 0;
    int backgroundEdges = 0;
    int bandPixels1 = 0;
    int bandPixels2 = 0;
    int bandPixels3 = 0;
};

struct LineFringeBlend {
    double targetFraction = 0.0;
    double residual = std::numeric_limits<double>::infinity();
    bool valid = false;
};

enum class LineFringeAction {
    Retained,
    MergedIntoColor,
    AddedToLineart,
};

struct LineFringeDecision {
    QString reason = QStringLiteral("not-evaluated");
    QString evidenceReason = QStringLiteral("none");
    qint64 voteSum = 0;
    int persistentPixels = 0;
    int streakPixels = 0;
    int overlapPixels = 0;
    int lineBoundaryEdges = 0;
    int perimeterEdges = 0;
    int firstLinePass = -1;
    int lastLinePass = -1;
    int graphRoot = -1;
    int targetLabel = -1;
    int targetSharedEdges = 0;
    int blendTargetLabel = -1;
    double paletteLinePrior = 0.0;
    double blendTargetFraction = 0.0;
    double blendResidual = std::numeric_limits<double>::infinity();
    bool narrow = false;
    bool evidenceCandidate = false;
    bool residualCandidate = false;
    bool cleanupCandidate = false;
    LineFringeAction action = LineFringeAction::Retained;
};

struct LineFringeCleanupResult {
    std::vector<LineFringeDecision> decisions;
    std::vector<qint64> paletteArea;
    std::vector<qint64> paletteLineartArea;
    int passCount = 0;
    int voteThreshold = 0;
    int streakThreshold = 0;
    int referenceLineHalfWidth = 0;
    int residualLineHalfWidth = 0;
    int residualLineExtendedHalfWidth = 0;
    int candidateGraphs = 0;
    int candidateLabels = 0;
    int candidatePixels = 0;
    int residualCandidateLabels = 0;
    int residualCandidatePixels = 0;
    int mergedLabels = 0;
    int mergedPixels = 0;
    int lineartLabels = 0;
    int lineartPixels = 0;
    int foregroundPixelsBefore = 0;
    int foregroundPixelsAfter = 0;
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

double rgbDistance(const QColor &left, const QColor &right);
LineFringeBlend lineFringeBlend(const QColor &sample,
                                const QColor &line,
                                const QColor &target);

void rebuildPassTopology(PassResult *pass, int width, int height) {
    const size_t pixelCount = static_cast<size_t>(width) * height;
    const int distanceInfinity = width + height + 1;
    std::vector<int> distance(pixelCount, 0);
    for (RegionAccum &accum : pass->accums) {
        accum.area = 0;
        accum.minX = width;
        accum.minY = height;
        accum.maxX = -1;
        accum.maxY = -1;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass->labels[index];
            if (label < 0) {
                continue;
            }
            RegionAccum &accum = pass->accums[static_cast<size_t>(label)];
            ++accum.area;
            accum.minX = std::min(accum.minX, x);
            accum.minY = std::min(accum.minY, y);
            accum.maxX = std::max(accum.maxX, x);
            accum.maxY = std::max(accum.maxY, y);
            const bool boundary = x == 0 || pass->labels[index - 1] != label
                || x == width - 1 || pass->labels[index + 1] != label
                || y == 0 || pass->labels[index - width] != label
                || y == height - 1 || pass->labels[index + width] != label;
            distance[index] = boundary ? 0 : distanceInfinity;
        }
    }

    pass->adjacency.assign(pass->accums.size(), {});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass->labels[index];
            if (label < 0) {
                continue;
            }
            if (x + 1 < width) {
                const int neighbor = pass->labels[index + 1];
                if (neighbor >= 0 && neighbor != label) {
                    pass->adjacency[static_cast<size_t>(label)].insert(neighbor);
                    pass->adjacency[static_cast<size_t>(neighbor)].insert(label);
                }
            }
            if (y + 1 < height) {
                const int neighbor = pass->labels[index + width];
                if (neighbor >= 0 && neighbor != label) {
                    pass->adjacency[static_cast<size_t>(label)].insert(neighbor);
                    pass->adjacency[static_cast<size_t>(neighbor)].insert(label);
                }
            }
        }
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass->labels[index];
            if (label < 0 || distance[index] == 0) {
                continue;
            }
            int best = distance[index];
            if (x > 0 && pass->labels[index - 1] == label) {
                best = std::min(best, distance[index - 1] + 1);
            }
            if (y > 0 && pass->labels[index - width] == label) {
                best = std::min(best, distance[index - width] + 1);
            }
            distance[index] = best;
        }
    }
    pass->maxDistance.assign(pass->accums.size(), 0);
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass->labels[index];
            if (label < 0) {
                continue;
            }
            int best = distance[index];
            if (best != 0) {
                if (x + 1 < width && pass->labels[index + 1] == label) {
                    best = std::min(best, distance[index + 1] + 1);
                }
                if (y + 1 < height && pass->labels[index + width] == label) {
                    best = std::min(best, distance[index + width] + 1);
                }
                distance[index] = best;
            }
            pass->maxDistance[static_cast<size_t>(label)] = std::max(
                pass->maxDistance[static_cast<size_t>(label)], best);
        }
    }
    for (RegionAccum &accum : pass->accums) {
        if (accum.area == 0) {
            accum.minX = 0;
            accum.minY = 0;
            accum.maxX = 0;
            accum.maxY = 0;
        }
    }
}

LineFringeCleanupResult cleanLineFringes(
    PassResult *pass,
    int width,
    int height,
    int minimumArea,
    int passCount,
    const std::vector<int> &lineVotes,
    const std::vector<int> &longestLineStreak,
    const std::vector<int> &firstLinePass,
    const std::vector<int> &lastLinePass,
    std::vector<bool> *lineartMask) {
    const int labelCount = static_cast<int>(pass->accums.size());
    const int paletteCount = pass->palette.size();
    const int voteThreshold = std::max(
        kLineFringeMinimumVotes,
        static_cast<int>(std::ceil(passCount * kLineFringePersistentVoteFraction)));
    const int streakThreshold = std::max(
        kLineFringeMinimumStreak,
        static_cast<int>(std::ceil(passCount * kLineFringePersistentStreakFraction)));
    const int directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    QVector<int> recognizedLineHalfWidths;
    std::vector<QHash<int, int>> boundaryEdges(static_cast<size_t>(labelCount));
    std::vector<int> graphLabel(static_cast<size_t>(labelCount), -1);
    LineFringeCleanupResult result;
    for (int label = 0; label < labelCount; ++label) {
        if (pass->lineart[static_cast<size_t>(label)]
            && pass->accums[static_cast<size_t>(label)].area >= minimumArea) {
            recognizedLineHalfWidths.push_back(
                pass->maxDistance[static_cast<size_t>(label)]);
        }
    }
    std::sort(recognizedLineHalfWidths.begin(), recognizedLineHalfWidths.end());
    const int referenceLineHalfWidth = recognizedLineHalfWidths.isEmpty()
        ? 1
        : std::max(1, recognizedLineHalfWidths[recognizedLineHalfWidths.size() / 2]);
    result.decisions.resize(static_cast<size_t>(labelCount));
    result.paletteArea.assign(static_cast<size_t>(paletteCount), 0);
    result.paletteLineartArea.assign(static_cast<size_t>(paletteCount), 0);
    result.passCount = passCount;
    result.voteThreshold = voteThreshold;
    result.streakThreshold = streakThreshold;
    result.referenceLineHalfWidth = referenceLineHalfWidth;
    result.residualLineHalfWidth = referenceLineHalfWidth * kResidualLineWidthMultiplier;
    result.residualLineExtendedHalfWidth = referenceLineHalfWidth
        * kResidualLineExtendedWidthMultiplier;
    for (int label = 0; label < labelCount; ++label) {
        const RegionAccum &accum = pass->accums[static_cast<size_t>(label)];
        result.foregroundPixelsBefore += accum.area;
        result.paletteArea[static_cast<size_t>(accum.paletteIndex)] += accum.area;
        if (pass->lineart[static_cast<size_t>(label)]) {
            result.paletteLineartArea[static_cast<size_t>(accum.paletteIndex)] += accum.area;
        }
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = pass->labels[index];
            if (label < 0) {
                continue;
            }
            LineFringeDecision &decision = result.decisions[static_cast<size_t>(label)];
            decision.voteSum += lineVotes[index];
            if (lineVotes[index] >= voteThreshold) {
                ++decision.persistentPixels;
            }
            if (longestLineStreak[index] >= streakThreshold) {
                ++decision.streakPixels;
            }
            if ((*lineartMask)[index]) {
                ++decision.overlapPixels;
            }
            if (firstLinePass[index] >= 0
                && (decision.firstLinePass < 0
                    || firstLinePass[index] < decision.firstLinePass)) {
                decision.firstLinePass = firstLinePass[index];
            }
            decision.lastLinePass = std::max(decision.lastLinePass, lastLinePass[index]);
            for (const auto &direction : directions) {
                const int neighborX = x + direction[0];
                const int neighborY = y + direction[1];
                if (neighborX < 0 || neighborY < 0
                    || neighborX >= width || neighborY >= height) {
                    ++decision.perimeterEdges;
                    continue;
                }
                const size_t neighborIndex = static_cast<size_t>(neighborY) * width + neighborX;
                const int neighborLabel = pass->labels[neighborIndex];
                if (neighborLabel == label) {
                    continue;
                }
                ++decision.perimeterEdges;
                if (neighborLabel >= 0) {
                    ++boundaryEdges[static_cast<size_t>(label)][neighborLabel];
                    if (pass->lineart[static_cast<size_t>(neighborLabel)]) {
                        ++decision.lineBoundaryEdges;
                    }
                }
            }
        }
    }

    for (int label = 0; label < labelCount; ++label) {
        const RegionAccum &accum = pass->accums[static_cast<size_t>(label)];
        LineFringeDecision &decision = result.decisions[static_cast<size_t>(label)];
        const double area = std::max(1, accum.area);
        const double meanVoteFraction = passCount > 0
            ? decision.voteSum / (area * passCount) : 0.0;
        const double persistentCoverage = decision.persistentPixels / area;
        const double overlapCoverage = decision.overlapPixels / area;
        const double contactFraction = decision.lineBoundaryEdges
            / static_cast<double>(std::max(1, decision.perimeterEdges));
        const qint64 paletteArea = result.paletteArea[static_cast<size_t>(accum.paletteIndex)];
        decision.paletteLinePrior = paletteArea > 0
            ? static_cast<double>(result.paletteLineartArea[
                  static_cast<size_t>(accum.paletteIndex)]) / paletteArea
            : 0.0;
        decision.narrow = 2.0 * pass->maxDistance[static_cast<size_t>(label)]
            <= pass->widthCap;
        if (pass->lineart[static_cast<size_t>(label)]) {
            decision.reason = QStringLiteral("final-lineart");
            continue;
        }
        if (accum.area < minimumArea) {
            decision.reason = QStringLiteral("below-minimum-area");
            continue;
        }
        if (!decision.narrow) {
            decision.reason = QStringLiteral("too-wide");
            continue;
        }
        const bool paletteEvidence = decision.paletteLinePrior >= kLineFringePalettePrior
            && meanVoteFraction >= kLineFringePaletteMeanVoteFraction
            && persistentCoverage >= kLineFringePalettePersistentCoverage;
        const bool strongDirectPersistence = decision.lineBoundaryEdges > 0
            && meanVoteFraction >= kLineFringeDirectMeanVoteFraction
            && persistentCoverage >= kLineFringeDirectPersistentCoverage;
        qint64 lineRed = 0;
        qint64 lineGreen = 0;
        qint64 lineBlue = 0;
        int lineEdges = 0;
        for (auto it = boundaryEdges[static_cast<size_t>(label)].cbegin();
             it != boundaryEdges[static_cast<size_t>(label)].cend(); ++it) {
            const int neighbor = it.key();
            if (!pass->lineart[static_cast<size_t>(neighbor)]) {
                continue;
            }
            const RegionAccum &neighborAccum = pass->accums[static_cast<size_t>(neighbor)];
            const QColor neighborColor = pass->palette[neighborAccum.paletteIndex];
            lineRed += static_cast<qint64>(neighborColor.red()) * it.value();
            lineGreen += static_cast<qint64>(neighborColor.green()) * it.value();
            lineBlue += static_cast<qint64>(neighborColor.blue()) * it.value();
            lineEdges += it.value();
        }
        LineFringeBlend bestBlend;
        if (lineEdges > 0) {
            const QColor lineColor(static_cast<int>(lineRed / lineEdges),
                                   static_cast<int>(lineGreen / lineEdges),
                                   static_cast<int>(lineBlue / lineEdges));
            const QColor sampleColor = pass->palette[accum.paletteIndex];
            int bestSharedEdges = -1;
            for (auto it = boundaryEdges[static_cast<size_t>(label)].cbegin();
                 it != boundaryEdges[static_cast<size_t>(label)].cend(); ++it) {
                const int neighbor = it.key();
                if (pass->lineart[static_cast<size_t>(neighbor)]) {
                    continue;
                }
                const RegionAccum &neighborAccum = pass->accums[static_cast<size_t>(neighbor)];
                const QColor targetColor = pass->palette[neighborAccum.paletteIndex];
                const LineFringeBlend blend = lineFringeBlend(
                    sampleColor, lineColor, targetColor);
                if (blend.valid
                    && (!bestBlend.valid || blend.residual < bestBlend.residual
                        || (blend.residual == bestBlend.residual
                            && it.value() > bestSharedEdges)
                        || (blend.residual == bestBlend.residual
                            && it.value() == bestSharedEdges
                            && neighbor < decision.blendTargetLabel))) {
                    bestBlend = blend;
                    bestSharedEdges = it.value();
                    decision.blendTargetLabel = neighbor;
                }
            }
        }
        if (bestBlend.valid) {
            decision.blendTargetFraction = bestBlend.targetFraction;
            decision.blendResidual = bestBlend.residual;
        }
        const bool blendSupport = bestBlend.valid
            && bestBlend.residual <= kLineFringeMaximumBlendResidual
            && bestBlend.targetFraction >= kLineFringeMinimumTargetFraction
            && bestBlend.targetFraction <= kLineFringeMaximumTargetFraction;
        const bool blendSupportedPersistence = decision.lineBoundaryEdges > 0
            && blendSupport
            && meanVoteFraction >= kLineFringeBlendMeanVoteFraction
            && overlapCoverage >= kLineFringeBlendOverlapCoverage;
        const bool directPersistentEvidence = strongDirectPersistence
            || blendSupportedPersistence;
        const int halfWidth = pass->maxDistance[static_cast<size_t>(label)];
        const bool residualPaletteWidth = halfWidth <= result.residualLineHalfWidth
            || (halfWidth <= result.residualLineExtendedHalfWidth
                && persistentCoverage >= kResidualLineExtendedPersistentCoverage
                && overlapCoverage >= kResidualLineExtendedOverlapCoverage);
        const bool residualPaletteEvidence = residualPaletteWidth
            && decision.paletteLinePrior >= kResidualLinePalettePrior
            && overlapCoverage >= kResidualLineMinimumOverlapCoverage
            && meanVoteFraction >= kResidualLineMinimumMeanVoteFraction;
        const bool residualHistoricalEvidence = halfWidth <= result.residualLineHalfWidth
            && overlapCoverage >= kResidualLineHistoricalOverlapCoverage
            && meanVoteFraction >= kResidualLineHistoricalMeanVoteFraction;
        const bool residualDirectEvidence = halfWidth <= result.referenceLineHalfWidth
            && decision.paletteLinePrior >= kResidualLineDirectPalettePrior
            && overlapCoverage >= kResidualLineDirectOverlapCoverage
            && meanVoteFraction >= kResidualLineDirectMeanVoteFraction
            && contactFraction >= kResidualLineDirectContactFraction;
        decision.residualCandidate = residualPaletteEvidence
            || residualHistoricalEvidence || residualDirectEvidence;
        decision.evidenceCandidate = paletteEvidence
            || directPersistentEvidence || decision.residualCandidate;
        if (paletteEvidence || directPersistentEvidence) {
            decision.evidenceReason = paletteEvidence
                ? QStringLiteral("palette-persistent")
                : QStringLiteral("boundary-persistent");
        } else if (decision.residualCandidate) {
            decision.evidenceReason = residualPaletteEvidence
                ? QStringLiteral("residual-line-palette")
                : residualHistoricalEvidence
                    ? QStringLiteral("residual-line-history")
                    : QStringLiteral("residual-line-contact");
        } else {
            decision.evidenceReason = QStringLiteral("insufficient-persistence");
        }
        decision.reason = decision.evidenceReason;
    }

    for (int label = 0; label < labelCount; ++label) {
        if (!result.decisions[static_cast<size_t>(label)].evidenceCandidate
            || graphLabel[static_cast<size_t>(label)] >= 0) {
            continue;
        }
        QVector<int> members;
        QVector<int> pending{label};
        graphLabel[static_cast<size_t>(label)] = label;
        bool touchesLineart = false;
        bool containsResidualCandidate = false;
        while (!pending.isEmpty()) {
            const int current = pending.back();
            pending.pop_back();
            members.push_back(current);
            LineFringeDecision &decision = result.decisions[static_cast<size_t>(current)];
            touchesLineart = touchesLineart || decision.lineBoundaryEdges > 0;
            containsResidualCandidate = containsResidualCandidate
                || decision.residualCandidate;
            for (auto it = boundaryEdges[static_cast<size_t>(current)].cbegin();
                 it != boundaryEdges[static_cast<size_t>(current)].cend(); ++it) {
                const int neighbor = it.key();
                if (result.decisions[static_cast<size_t>(neighbor)].evidenceCandidate
                    && graphLabel[static_cast<size_t>(neighbor)] < 0) {
                    graphLabel[static_cast<size_t>(neighbor)] = label;
                    pending.push_back(neighbor);
                }
            }
        }
        if (!touchesLineart && !containsResidualCandidate) {
            for (const int member : members) {
                result.decisions[static_cast<size_t>(member)].reason =
                    QStringLiteral("candidate-detached-from-lineart");
            }
            continue;
        }

        QHash<int, int> targetEdges;
        qint64 graphRed = 0;
        qint64 graphGreen = 0;
        qint64 graphBlue = 0;
        int graphArea = 0;
        for (const int member : members) {
            LineFringeDecision &decision = result.decisions[static_cast<size_t>(member)];
            const RegionAccum &accum = pass->accums[static_cast<size_t>(member)];
            const QColor color = pass->palette[accum.paletteIndex];
            decision.cleanupCandidate = true;
            decision.graphRoot = label;
            graphRed += static_cast<qint64>(color.red()) * accum.area;
            graphGreen += static_cast<qint64>(color.green()) * accum.area;
            graphBlue += static_cast<qint64>(color.blue()) * accum.area;
            graphArea += accum.area;
            ++result.candidateLabels;
            result.candidatePixels += accum.area;
            if (decision.residualCandidate) {
                ++result.residualCandidateLabels;
                result.residualCandidatePixels += accum.area;
            }
            for (auto it = boundaryEdges[static_cast<size_t>(member)].cbegin();
                 it != boundaryEdges[static_cast<size_t>(member)].cend(); ++it) {
                const int neighbor = it.key();
                if (!result.decisions[static_cast<size_t>(neighbor)].evidenceCandidate
                    && !pass->lineart[static_cast<size_t>(neighbor)]
                    && pass->accums[static_cast<size_t>(neighbor)].area > 0) {
                    targetEdges[neighbor] += it.value();
                }
            }
        }
        ++result.candidateGraphs;

        int target = -1;
        int bestSharedEdges = -1;
        int bestTargetArea = -1;
        qint64 bestColorDistance = std::numeric_limits<qint64>::max();
        for (auto it = targetEdges.cbegin(); it != targetEdges.cend(); ++it) {
            const int candidateTarget = it.key();
            const RegionAccum &targetAccum = pass->accums[static_cast<size_t>(candidateTarget)];
            const QColor targetColor = pass->palette[targetAccum.paletteIndex];
            const qint64 red = graphRed - static_cast<qint64>(targetColor.red()) * graphArea;
            const qint64 green = graphGreen - static_cast<qint64>(targetColor.green()) * graphArea;
            const qint64 blue = graphBlue - static_cast<qint64>(targetColor.blue()) * graphArea;
            const qint64 colorDistance = red * red + green * green + blue * blue;
            if (it.value() > bestSharedEdges
                || (it.value() == bestSharedEdges && colorDistance < bestColorDistance)
                || (it.value() == bestSharedEdges && colorDistance == bestColorDistance
                    && targetAccum.area > bestTargetArea)
                || (it.value() == bestSharedEdges && colorDistance == bestColorDistance
                    && targetAccum.area == bestTargetArea && candidateTarget < target)) {
                target = candidateTarget;
                bestSharedEdges = it.value();
                bestTargetArea = targetAccum.area;
                bestColorDistance = colorDistance;
            }
        }
        for (const int member : members) {
            LineFringeDecision &decision = result.decisions[static_cast<size_t>(member)];
            decision.targetLabel = target;
            decision.targetSharedEdges = bestSharedEdges;
            if (target >= 0) {
                decision.action = LineFringeAction::MergedIntoColor;
                decision.reason = QStringLiteral("merged-by-shared-boundary");
                ++result.mergedLabels;
                result.mergedPixels += pass->accums[static_cast<size_t>(member)].area;
            } else {
                decision.action = LineFringeAction::AddedToLineart;
                decision.reason = QStringLiteral("enclosed-by-lineart");
                ++result.lineartLabels;
                result.lineartPixels += pass->accums[static_cast<size_t>(member)].area;
            }
        }
    }

    for (size_t index = 0; index < pass->labels.size(); ++index) {
        const int label = pass->labels[index];
        if (label < 0) {
            continue;
        }
        const LineFringeDecision &decision = result.decisions[static_cast<size_t>(label)];
        if (decision.action == LineFringeAction::MergedIntoColor) {
            pass->labels[index] = decision.targetLabel;
        } else if (decision.action == LineFringeAction::AddedToLineart) {
            pass->labels[index] = -1;
            (*lineartMask)[index] = true;
        }
    }
    rebuildPassTopology(pass, width, height);
    result.foregroundPixelsAfter = static_cast<int>(std::count_if(
        pass->labels.cbegin(), pass->labels.cend(), [](int label) { return label >= 0; }));

    return result;
}

double rgbDistance(const QColor &left, const QColor &right) {
    const double red = left.red() - right.red();
    const double green = left.green() - right.green();
    const double blue = left.blue() - right.blue();

    return std::sqrt(red * red + green * green + blue * blue);
}

LineFringeBlend lineFringeBlend(const QColor &sample,
                                const QColor &line,
                                const QColor &target) {
    LineFringeBlend result;
    if (!line.isValid() || !target.isValid()) {
        return result;
    }
    const double red = target.red() - line.red();
    const double green = target.green() - line.green();
    const double blue = target.blue() - line.blue();
    const double lengthSquared = red * red + green * green + blue * blue;
    if (lengthSquared <= 1e-9) {
        return result;
    }
    const double sampleRed = sample.red() - line.red();
    const double sampleGreen = sample.green() - line.green();
    const double sampleBlue = sample.blue() - line.blue();
    result.targetFraction = (sampleRed * red + sampleGreen * green + sampleBlue * blue)
        / lengthSquared;
    const double clampedFraction = std::clamp(result.targetFraction, 0.0, 1.0);
    const QColor projected(
        static_cast<int>(std::lround(line.red() + clampedFraction * red)),
        static_cast<int>(std::lround(line.green() + clampedFraction * green)),
        static_cast<int>(std::lround(line.blue() + clampedFraction * blue)));
    result.residual = rgbDistance(sample, projected);
    result.valid = true;

    return result;
}

std::vector<int> distanceToLineart(const std::vector<bool> &lineartMask,
                                   int width,
                                   int height) {
    const int infinity = width + height + 1;
    std::vector<int> distance(lineartMask.size(), infinity);
    for (size_t index = 0; index < lineartMask.size(); ++index) {
        if (lineartMask[index]) {
            distance[index] = 0;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (x > 0) {
                distance[index] = std::min(distance[index], distance[index - 1] + 1);
            }
            if (y > 0) {
                distance[index] = std::min(distance[index], distance[index - width] + 1);
            }
        }
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (x + 1 < width) {
                distance[index] = std::min(distance[index], distance[index + 1] + 1);
            }
            if (y + 1 < height) {
                distance[index] = std::min(distance[index], distance[index + width] + 1);
            }
        }
    }

    return distance;
}

QColor diagnosticLineColor(const RegionDiagnosticMetrics &metrics,
                           const QColor &fallback) {
    if (metrics.lineSamples <= 0) {
        return fallback;
    }

    return QColor(static_cast<int>(metrics.lineRedSum / metrics.lineSamples),
                  static_cast<int>(metrics.lineGreenSum / metrics.lineSamples),
                  static_cast<int>(metrics.lineBlueSum / metrics.lineSamples));
}

QString lineFringeActionName(LineFringeAction action) {
    switch (action) {
    case LineFringeAction::Retained:
        return QStringLiteral("retained");
    case LineFringeAction::MergedIntoColor:
        return QStringLiteral("merged-color");
    case LineFringeAction::AddedToLineart:
        return QStringLiteral("added-lineart");
    }

    return QStringLiteral("unknown");
}

void writeRegionExtractionDiagnostic(
    const QImage &image,
    const RegionExtractionParams &params,
    const QVector<QColor> &naturalPalette,
    const PassResult &finalSeg,
    const std::vector<bool> &lineartMask,
    const PassResult &cleanedSeg,
    const std::vector<bool> &outputLineartMask,
    const LineFringeCleanupResult &fringeCleanup,
    const QVector<ExtractionPassDiagnostic> &passDiagnostics,
    const QSet<int> &extractedColorLabels,
    const RegionExtractionResult &result) {
    const QString path = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("region_extract.log"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning().noquote() << "Could not write region extraction log to" << path;
        return;
    }
    const int width = image.width();
    const int height = image.height();
    const int outputLineartPixels = static_cast<int>(std::count(
        outputLineartMask.cbegin(), outputLineartMask.cend(), true));
    std::vector<bool> coreLineartMask(lineartMask.size(), false);
    for (size_t index = 0; index < finalSeg.labels.size(); ++index) {
        const int label = finalSeg.labels[index];
        coreLineartMask[index] = label >= 0
            && finalSeg.lineart[static_cast<size_t>(label)] != 0;
    }
    const std::vector<int> lineDistance = distanceToLineart(coreLineartMask, width, height);
    std::vector<RegionDiagnosticMetrics> metrics(finalSeg.accums.size());
    qint64 accumulatedLineRed = 0;
    qint64 accumulatedLineGreen = 0;
    qint64 accumulatedLineBlue = 0;
    qint64 coreLineRed = 0;
    qint64 coreLineGreen = 0;
    qint64 coreLineBlue = 0;
    int accumulatedLinePixels = 0;
    int coreLinePixels = 0;
    for (int y = 0; y < height; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            if (lineartMask[index]) {
                accumulatedLineRed += qRed(row[x]);
                accumulatedLineGreen += qGreen(row[x]);
                accumulatedLineBlue += qBlue(row[x]);
                ++accumulatedLinePixels;
            }
            if (coreLineartMask[index]) {
                coreLineRed += qRed(row[x]);
                coreLineGreen += qGreen(row[x]);
                coreLineBlue += qBlue(row[x]);
                ++coreLinePixels;
            }
        }
    }
    const QColor accumulatedLineColor = accumulatedLinePixels > 0
        ? QColor(static_cast<int>(accumulatedLineRed / accumulatedLinePixels),
                 static_cast<int>(accumulatedLineGreen / accumulatedLinePixels),
                 static_cast<int>(accumulatedLineBlue / accumulatedLinePixels))
        : QColor();
    const QColor globalLineColor = coreLinePixels > 0
        ? QColor(static_cast<int>(coreLineRed / coreLinePixels),
                 static_cast<int>(coreLineGreen / coreLinePixels),
                 static_cast<int>(coreLineBlue / coreLinePixels))
        : accumulatedLineColor;
    const int directions[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int y = 0; y < height; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const int label = finalSeg.labels[index];
            if (label < 0) {
                continue;
            }
            RegionDiagnosticMetrics &entry = metrics[static_cast<size_t>(label)];
            if (lineDistance[index] <= 1) {
                ++entry.bandPixels1;
            }
            if (lineDistance[index] <= 2) {
                ++entry.bandPixels2;
            }
            if (lineDistance[index] <= kLineFringeBandRadius) {
                ++entry.bandPixels3;
            }
            if (lineartMask[index]) {
                ++entry.lineOverlapPixels;
            }
            if (coreLineartMask[index]) {
                entry.lineRedSum += qRed(row[x]);
                entry.lineGreenSum += qGreen(row[x]);
                entry.lineBlueSum += qBlue(row[x]);
                ++entry.lineSamples;
            }
            for (const auto &direction : directions) {
                const int neighborX = x + direction[0];
                const int neighborY = y + direction[1];
                if (neighborX < 0 || neighborY < 0
                    || neighborX >= width || neighborY >= height) {
                    ++entry.perimeterEdges;
                    ++entry.backgroundEdges;
                    continue;
                }
                const size_t neighborIndex = static_cast<size_t>(neighborY) * width + neighborX;
                const int neighborLabel = finalSeg.labels[neighborIndex];
                if (neighborLabel != label) {
                    ++entry.perimeterEdges;
                    if (neighborLabel >= 0) {
                        ++entry.neighborEdges[neighborLabel];
                    } else {
                        ++entry.backgroundEdges;
                    }
                }
                if (!coreLineartMask[index] && coreLineartMask[neighborIndex]) {
                    const QRgb neighborPixel = reinterpret_cast<const QRgb *>(
                        image.constScanLine(neighborY))[neighborX];
                    ++entry.lineBoundaryEdges;
                    entry.lineRedSum += qRed(neighborPixel);
                    entry.lineGreenSum += qGreen(neighborPixel);
                    entry.lineBlueSum += qBlue(neighborPixel);
                    ++entry.lineSamples;
                }
            }
        }
    }

    QTextStream stream(&file);
    stream.setRealNumberNotation(QTextStream::FixedNotation);
    stream.setRealNumberPrecision(6);
    stream << "Region extraction diagnostic\n"
           << "image=" << width << 'x' << height << '\n'
           << "params max_colors=" << params.maxColorCount
           << " min_area=" << params.minRegionArea
           << " small_merge_area=" << params.smallRegionMergeArea
           << " color_merge_distance=" << params.colorMergeDistance
           << " color_frequency_floor=" << params.colorFrequencyFloor
           << " line_width_cap_fraction=" << params.lineWidthCapFraction
           << " line_width_cap_floor=" << params.lineWidthCapFloor
           << " contrast_threshold=" << params.contrastThreshold
           << " min_elongation=" << params.minElongation << '\n'
           << "natural_palette_count=" << naturalPalette.size()
           << " final_palette_count=" << finalSeg.palette.size()
           << " final_label_count=" << finalSeg.accums.size()
           << " accumulated_lineart_pixels=" << accumulatedLinePixels
           << " output_lineart_pixels=" << outputLineartPixels
           << " core_lineart_pixels=" << coreLinePixels
           << " accumulated_line_color="
           << (accumulatedLineColor.isValid()
                   ? accumulatedLineColor.name() : QStringLiteral("none"))
           << " global_line_color="
           << (globalLineColor.isValid() ? globalLineColor.name() : QStringLiteral("none"))
           << '\n'
           << "heuristic band_radius=" << kLineFringeBandRadius
           << " minimum_band_fraction=" << kLineFringeMinimumBandFraction
           << " minimum_contact_fraction=" << kLineFringeMinimumContactFraction
           << " minimum_overlap_fraction=" << kLineFringeMinimumOverlapFraction
           << " maximum_blend_residual=" << kLineFringeMaximumBlendResidual
           << " target_fraction_range=" << kLineFringeMinimumTargetFraction
           << ',' << kLineFringeMaximumTargetFraction << '\n'
           << "cleanup passes=" << fringeCleanup.passCount
           << " vote_threshold=" << fringeCleanup.voteThreshold
           << " streak_threshold=" << fringeCleanup.streakThreshold
           << " palette_line_prior=" << kLineFringePalettePrior
           << " palette_mean_vote_fraction=" << kLineFringePaletteMeanVoteFraction
           << " palette_persistent_coverage=" << kLineFringePalettePersistentCoverage
           << " direct_mean_vote_fraction=" << kLineFringeDirectMeanVoteFraction
           << " direct_persistent_coverage=" << kLineFringeDirectPersistentCoverage
           << " blend_mean_vote_fraction=" << kLineFringeBlendMeanVoteFraction
           << " blend_overlap_coverage=" << kLineFringeBlendOverlapCoverage << '\n'
           << "residual reference_half_width=" << fringeCleanup.referenceLineHalfWidth
           << " half_width=" << fringeCleanup.residualLineHalfWidth
           << " extended_half_width=" << fringeCleanup.residualLineExtendedHalfWidth
           << " palette_line_prior=" << kResidualLinePalettePrior
           << " minimum_overlap_coverage=" << kResidualLineMinimumOverlapCoverage
           << " minimum_mean_vote_fraction=" << kResidualLineMinimumMeanVoteFraction
           << " extended_overlap_coverage=" << kResidualLineExtendedOverlapCoverage
           << " extended_persistent_coverage="
           << kResidualLineExtendedPersistentCoverage
           << " historical_overlap_coverage="
           << kResidualLineHistoricalOverlapCoverage
           << " historical_mean_vote_fraction="
           << kResidualLineHistoricalMeanVoteFraction
           << " direct_palette_prior=" << kResidualLineDirectPalettePrior
           << " direct_overlap_coverage=" << kResidualLineDirectOverlapCoverage
           << " direct_mean_vote_fraction=" << kResidualLineDirectMeanVoteFraction
           << " direct_contact_fraction=" << kResidualLineDirectContactFraction << '\n';
    for (int index = 0; index < naturalPalette.size(); ++index) {
        stream << "natural_palette[" << index << "]=" << naturalPalette[index].name() << '\n';
    }
    for (int index = 0; index < finalSeg.palette.size(); ++index) {
        const qint64 paletteArea = fringeCleanup.paletteArea[static_cast<size_t>(index)];
        const qint64 lineartArea = fringeCleanup.paletteLineartArea[static_cast<size_t>(index)];
        const double linePrior = paletteArea > 0
            ? static_cast<double>(lineartArea) / paletteArea : 0.0;
        stream << "final_palette[" << index << "]=" << finalSeg.palette[index].name()
               << " area=" << paletteArea
               << " lineart_area=" << lineartArea
               << " line_prior=" << linePrior << '\n';
    }
    for (const ExtractionPassDiagnostic &pass : passDiagnostics) {
        stream << "pass requested_colors=" << pass.requestedColors
               << " palette_colors=" << pass.paletteColors
               << " regions=" << pass.regions
               << " lineart_regions=" << pass.lineartRegions
               << " accumulated_lineart_pixels=" << pass.accumulatedLineartPixels
               << " merged_small_regions=" << pass.mergedSmallRegions << '\n';
    }

    std::vector<int> colorOrdinals(finalSeg.accums.size(), -1);
    int nextColorOrdinal = 0;
    for (int label = 0; label < static_cast<int>(finalSeg.accums.size()); ++label) {
        if (extractedColorLabels.contains(label)) {
            colorOrdinals[static_cast<size_t>(label)] = nextColorOrdinal++;
        }
    }
    int blendSupportCandidates = 0;
    int blendSupportCandidatePixels = 0;
    for (int label = 0; label < static_cast<int>(finalSeg.accums.size()); ++label) {
        const RegionAccum &accum = finalSeg.accums[static_cast<size_t>(label)];
        const RegionAccum &cleanedAccum = cleanedSeg.accums[static_cast<size_t>(label)];
        const RegionDiagnosticMetrics &entry = metrics[static_cast<size_t>(label)];
        const LineFringeDecision &cleanup = fringeCleanup.decisions[static_cast<size_t>(label)];
        const QColor color = finalSeg.palette[accum.paletteIndex];
        const QColor localLineColor = diagnosticLineColor(entry, globalLineColor);
        const double area = std::max(1, accum.area);
        const double perimeter = std::max(1, entry.perimeterEdges);
        const double bandFraction = entry.bandPixels3 / area;
        const double contactFraction = entry.lineBoundaryEdges / perimeter;
        const double overlapFraction = entry.lineOverlapPixels / area;
        const double meanVoteFraction = fringeCleanup.passCount > 0
            ? cleanup.voteSum / (area * fringeCleanup.passCount) : 0.0;
        const double persistentCoverage = cleanup.persistentPixels / area;
        const double streakCoverage = cleanup.streakPixels / area;
        QVector<int> neighbors = entry.neighborEdges.keys();
        std::sort(neighbors.begin(), neighbors.end());
        int bestTarget = -1;
        int bestSharedEdges = -1;
        LineFringeBlend bestBlend;
        for (const int neighbor : neighbors) {
            if (finalSeg.lineart[static_cast<size_t>(neighbor)]) {
                continue;
            }
            const RegionAccum &neighborAccum = finalSeg.accums[static_cast<size_t>(neighbor)];
            const QColor neighborColor = finalSeg.palette[neighborAccum.paletteIndex];
            const LineFringeBlend blend = lineFringeBlend(color, localLineColor, neighborColor);
            const int sharedEdges = entry.neighborEdges.value(neighbor);
            if (blend.valid
                && (!bestBlend.valid || blend.residual < bestBlend.residual - 1e-9
                    || (std::abs(blend.residual - bestBlend.residual) <= 1e-9
                        && sharedEdges > bestSharedEdges)
                    || (std::abs(blend.residual - bestBlend.residual) <= 1e-9
                        && sharedEdges == bestSharedEdges
                        && neighborAccum.area
                            > finalSeg.accums[static_cast<size_t>(bestTarget)].area))) {
                bestTarget = neighbor;
                bestSharedEdges = sharedEdges;
                bestBlend = blend;
            }
        }
        const bool lineRelated = contactFraction >= kLineFringeMinimumContactFraction
            || overlapFraction >= kLineFringeMinimumOverlapFraction;
        const bool blendSupportCandidate = !finalSeg.lineart[static_cast<size_t>(label)]
            && bandFraction >= kLineFringeMinimumBandFraction
            && lineRelated
            && bestBlend.valid
            && bestBlend.residual <= kLineFringeMaximumBlendResidual
            && bestBlend.targetFraction >= kLineFringeMinimumTargetFraction
            && bestBlend.targetFraction <= kLineFringeMaximumTargetFraction;
        if (blendSupportCandidate) {
            ++blendSupportCandidates;
            blendSupportCandidatePixels += accum.area;
        }
        QString disposition = QStringLiteral("trace-failed");
        if (cleanup.action == LineFringeAction::MergedIntoColor) {
            disposition = QStringLiteral("merged-line-fringe");
        } else if (cleanup.action == LineFringeAction::AddedToLineart) {
            disposition = QStringLiteral("promoted-line-fringe");
        } else if (finalSeg.lineart[static_cast<size_t>(label)]) {
            disposition = QStringLiteral("final-lineart");
        } else if (accum.area < params.minRegionArea) {
            disposition = QStringLiteral("below-minimum-area");
        } else if (extractedColorLabels.contains(label)) {
            disposition = QStringLiteral("color-region");
        }
        stream << "\nlabel=" << label
               << " disposition=" << disposition
               << " color=" << color.name()
               << " palette_index=" << accum.paletteIndex
               << " area=" << accum.area
               << " cleaned_area=" << cleanedAccum.area
               << " bounds=" << accum.minX << ',' << accum.minY << ','
               << accum.maxX << ',' << accum.maxY
               << " half_width=" << finalSeg.maxDistance[static_cast<size_t>(label)]
               << " output_half_width="
               << cleanedSeg.maxDistance[static_cast<size_t>(label)]
               << " fill_region=" << colorOrdinals[static_cast<size_t>(label)]
               << " gui_group="
               << (colorOrdinals[static_cast<size_t>(label)] >= 0
                       ? colorOrdinals[static_cast<size_t>(label)] + 1 : -1)
               << " final_lineart="
               << (finalSeg.lineart[static_cast<size_t>(label)] ? "yes" : "no") << '\n'
               << "line band1=" << entry.bandPixels1
               << " band2=" << entry.bandPixels2
               << " band3=" << entry.bandPixels3
               << " band3_fraction=" << bandFraction
               << " overlap_pixels=" << entry.lineOverlapPixels
               << " overlap_fraction=" << overlapFraction
               << " boundary_edges=" << entry.lineBoundaryEdges
               << " contact_fraction=" << contactFraction
               << " local_color="
               << (localLineColor.isValid() ? localLineColor.name() : QStringLiteral("none"))
               << '\n'
               << "topology perimeter_edges=" << entry.perimeterEdges
               << " background_edges=" << entry.backgroundEdges
               << " neighbor_count=" << neighbors.size() << '\n'
               << "votes sum=" << cleanup.voteSum
               << " mean_fraction=" << meanVoteFraction
               << " persistent_pixels=" << cleanup.persistentPixels
               << " persistent_coverage=" << persistentCoverage
               << " streak_pixels=" << cleanup.streakPixels
               << " streak_coverage=" << streakCoverage
               << " first_requested_colors=" << cleanup.firstLinePass
               << " last_requested_colors=" << cleanup.lastLinePass << '\n'
               << "cleanup narrow=" << (cleanup.narrow ? "yes" : "no")
               << " palette_line_prior=" << cleanup.paletteLinePrior
               << " blend_target=" << cleanup.blendTargetLabel
               << " blend_target_fraction="
               << (std::isfinite(cleanup.blendResidual)
                       ? QString::number(cleanup.blendTargetFraction, 'f', 6)
                       : QStringLiteral("none"))
               << " blend_residual="
               << (std::isfinite(cleanup.blendResidual)
                       ? QString::number(cleanup.blendResidual, 'f', 6)
                       : QStringLiteral("none"))
               << " evidence_candidate=" << (cleanup.evidenceCandidate ? "yes" : "no")
               << " residual_evidence=" << (cleanup.residualCandidate ? "yes" : "no")
               << " candidate=" << (cleanup.cleanupCandidate ? "yes" : "no")
               << " graph_root=" << cleanup.graphRoot
               << " action=" << lineFringeActionName(cleanup.action)
               << " target=" << cleanup.targetLabel
               << " target_shared_edges=" << cleanup.targetSharedEdges
               << " evidence_reason=" << cleanup.evidenceReason
               << " reason=" << cleanup.reason << '\n'
               << "blend_support_candidate=" << (blendSupportCandidate ? "yes" : "no")
               << " best_target=" << bestTarget
               << " best_shared_edges=" << bestSharedEdges
               << " best_target_fraction="
               << (bestBlend.valid ? QString::number(bestBlend.targetFraction, 'f', 6)
                                   : QStringLiteral("none"))
               << " best_blend_residual="
               << (bestBlend.valid ? QString::number(bestBlend.residual, 'f', 6)
                                   : QStringLiteral("none")) << '\n';
        for (const int neighbor : neighbors) {
            const RegionAccum &neighborAccum = finalSeg.accums[static_cast<size_t>(neighbor)];
            const QColor neighborColor = finalSeg.palette[neighborAccum.paletteIndex];
            const LineFringeBlend blend = lineFringeBlend(color, localLineColor, neighborColor);
            stream << "neighbor label=" << neighbor
                   << " color=" << neighborColor.name()
                   << " area=" << neighborAccum.area
                   << " shared_edges=" << entry.neighborEdges.value(neighbor)
                   << " final_lineart="
                   << (finalSeg.lineart[static_cast<size_t>(neighbor)] ? "yes" : "no")
                   << " target_fraction="
                   << (blend.valid ? QString::number(blend.targetFraction, 'f', 6)
                                   : QStringLiteral("none"))
                   << " blend_residual="
                   << (blend.valid ? QString::number(blend.residual, 'f', 6)
                                   : QStringLiteral("none"))
                   << " color_distance=" << rgbDistance(color, neighborColor) << '\n';
        }
    }
    stream << "\nsummary extracted_color_regions=" << result.colorRegionCount
           << " extracted_lineart_regions=" << result.lineartRegionCount
           << " merged_small_regions=" << result.mergedSmallRegionCount
           << " cleanup_candidate_graphs=" << fringeCleanup.candidateGraphs
           << " cleanup_candidate_labels=" << fringeCleanup.candidateLabels
           << " cleanup_candidate_pixels=" << fringeCleanup.candidatePixels
           << " cleanup_residual_evidence_labels="
           << fringeCleanup.residualCandidateLabels
           << " cleanup_residual_evidence_pixels="
           << fringeCleanup.residualCandidatePixels
           << " cleanup_merged_labels=" << fringeCleanup.mergedLabels
           << " cleanup_merged_pixels=" << fringeCleanup.mergedPixels
           << " cleanup_lineart_labels=" << fringeCleanup.lineartLabels
           << " cleanup_lineart_pixels=" << fringeCleanup.lineartPixels
           << " foreground_pixels_before=" << fringeCleanup.foregroundPixelsBefore
           << " foreground_pixels_after=" << fringeCleanup.foregroundPixelsAfter
           << " foreground_accounted="
           << (fringeCleanup.foregroundPixelsBefore
                   == fringeCleanup.foregroundPixelsAfter + fringeCleanup.lineartPixels
                   ? "yes" : "no")
           << " blend_support_candidates=" << blendSupportCandidates
           << " blend_support_candidate_pixels=" << blendSupportCandidatePixels << '\n';
    file.close();
    qWarning().noquote() << "Region extraction log written to" << path;
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
    std::vector<int> lineVotes(pixelCount, 0);
    std::vector<int> currentLineStreak(pixelCount, 0);
    std::vector<int> longestLineStreak(pixelCount, 0);
    std::vector<int> firstLinePass(pixelCount, -1);
    std::vector<int> lastLinePass(pixelCount, -1);
    QVector<ExtractionPassDiagnostic> passDiagnostics;
    PassResult finalSeg;
    int passCount = 0;
    bool haveFinal = false;
    for (int colorCount = 2; colorCount <= maxColors; ++colorCount) {
        PassResult pass = segmentPass(image, width, height, alpha, colorCount, params);
        if (pass.accums.empty()) {
            continue;
        }
        ++passCount;
        for (size_t i = 0; i < pixelCount; ++i) {
            const int label = pass.labels[i];
            if (label >= 0 && pass.lineart[label]) {
                lineartMask[i] = true;
                ++lineVotes[i];
                ++currentLineStreak[i];
                longestLineStreak[i] = std::max(longestLineStreak[i], currentLineStreak[i]);
                if (firstLinePass[i] < 0) {
                    firstLinePass[i] = colorCount;
                }
                lastLinePass[i] = colorCount;
            } else {
                currentLineStreak[i] = 0;
            }
        }
        ExtractionPassDiagnostic diagnostic;
        diagnostic.requestedColors = colorCount;
        diagnostic.paletteColors = pass.palette.size();
        diagnostic.regions = pass.accums.size();
        diagnostic.lineartRegions = static_cast<int>(std::count(
            pass.lineart.cbegin(), pass.lineart.cend(), std::uint8_t{1}));
        diagnostic.accumulatedLineartPixels = static_cast<int>(std::count(
            lineartMask.cbegin(), lineartMask.cend(), true));
        diagnostic.mergedSmallRegions = pass.mergedSmallRegions;
        passDiagnostics.push_back(diagnostic);
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
    const PassResult diagnosticSeg = finalSeg;
    const std::vector<bool> diagnosticLineartMask = lineartMask;
    const LineFringeCleanupResult fringeCleanup = cleanLineFringes(
        &finalSeg, width, height, params.minRegionArea, passCount, lineVotes,
        longestLineStreak, firstLinePass, lastLinePass, &lineartMask);

    std::vector<QSet<int>> colorAdj = finalSeg.adjacency;
    QSet<int> extractedColorLabels;
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
        extractedColorLabels.insert(label);
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
    auto raster = QSharedPointer<RegionRasterData>::create();
    raster->labels = finalSeg.labels;
    raster->foreground.resize(pixelCount, 0);
    raster->lineart.resize(pixelCount, 0);
    for (int y = 0; y < height; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            raster->foreground[static_cast<size_t>(y) * width + x] =
                qAlpha(row[x]) >= alpha ? 1 : 0;
            raster->lineart[static_cast<size_t>(y) * width + x] =
                lineartMask[static_cast<size_t>(y) * width + x] ? 1 : 0;
        }
    }
    raster->traceParams = params;
    result.raster = raster;
    writeRegionExtractionDiagnostic(image, params, naturalPalette, diagnosticSeg,
                                    diagnosticLineartMask, finalSeg, lineartMask,
                                    fringeCleanup, passDiagnostics,
                                    extractedColorLabels, result);
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
