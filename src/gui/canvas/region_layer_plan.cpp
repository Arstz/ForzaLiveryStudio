#include "region_layer_plan.h"

#include "region_fill.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <vector>

namespace gui {
namespace {

constexpr double kMaximumAbsorbedAreaFraction = 0.1;
constexpr int kNearbyMergeMaximumGap = 6;
constexpr int kNearbyMergeMaximumDetour = 2;

class DisjointSet {
public:
    explicit DisjointSet(int size)
        : parents_(static_cast<size_t>(size)), ranks_(static_cast<size_t>(size), 0) {
        std::iota(parents_.begin(), parents_.end(), 0);
    }

    int root(int value) {
        int &parent = parents_[static_cast<size_t>(value)];
        if (parent != value) {
            parent = root(parent);
        }

        return parent;
    }

    bool unite(int left, int right) {
        left = root(left);
        right = root(right);
        if (left == right) {
            return false;
        }
        if (ranks_[static_cast<size_t>(left)] < ranks_[static_cast<size_t>(right)]) {
            std::swap(left, right);
        }
        parents_[static_cast<size_t>(right)] = left;
        if (ranks_[static_cast<size_t>(left)] == ranks_[static_cast<size_t>(right)]) {
            ++ranks_[static_cast<size_t>(left)];
        }

        return true;
    }

private:
    std::vector<int> parents_;
    std::vector<std::uint8_t> ranks_;
};

struct SourceRegion {
    QPainterPath outerPath;
    QVector<int> pixels;
    int resultIndex = -1;
    int label = -1;
};

struct LayerGroup {
    QColor color;
    QPainterPath outline;
    QPainterPath outerPath;
    QVector<int> sourceRegionIndices;
    QVector<int> pixels;
    QVector<int> absorbedRegionIndices;
    int firstSourceIndex = -1;
    int sourceArea = 0;
    int fillArea = 0;
};

struct NearbyMergeCandidate {
    int leftSource = -1;
    int rightSource = -1;
    int leftPixel = -1;
    int rightPixel = -1;
    int gap = 0;
    int edgeClearance = 0;
};

struct NearbyMergeBridge {
    QVector<int> pixels;
    quint64 pairKey = 0;
    int leftSource = -1;
};

QPainterPath outerPath(const QPainterPath &outline) {
    const QPolygonF contour = regionOuterContour(outline);
    if (contour.size() < 3) {
        return {};
    }
    QPainterPath path;
    path.moveTo(contour.front());
    for (int i = 1; i < contour.size(); ++i) {
        path.lineTo(contour[i]);
    }
    path.closeSubpath();

    return path;
}

bool sameColor(const QColor &left, const QColor &right) {
    return left.rgba() == right.rgba();
}

bool strictlyContains(const QPainterPath &parent, const QPainterPath &child) {
    const QRectF parentBounds = parent.boundingRect();
    const QRectF childBounds = child.boundingRect();

    return !parent.isEmpty() && !child.isEmpty()
        && parentBounds != childBounds
        && parentBounds.contains(childBounds)
        && parent.contains(child);
}

double enclosureSize(const QPainterPath &path) {
    const QRectF bounds = path.boundingRect();

    return bounds.width() * bounds.height();
}

QRect pixelBounds(const QVector<int> &pixels, int width) {
    if (pixels.isEmpty() || width <= 0) {
        return {};
    }
    int minX = width;
    int minY = std::numeric_limits<int>::max();
    int maxX = -1;
    int maxY = -1;
    for (const int pixel : pixels) {
        const int x = pixel % width;
        const int y = pixel / width;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }

    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

QPainterPath tracePixels(const QVector<int> &pixels,
                         const QSize &imageSize,
                         const RegionExtractionParams &params) {
    const int width = imageSize.width();
    const int height = imageSize.height();
    std::vector<std::uint8_t> mask(static_cast<size_t>(width) * height, 0);
    for (const int pixel : pixels) {
        if (pixel >= 0 && pixel < width * height) {
            mask[static_cast<size_t>(pixel)] = 1;
        }
    }

    return traceMaskToPath(mask, width, height, pixelBounds(pixels, width), params);
}

quint64 sourcePairKey(int left, int right) {
    if (left > right) {
        std::swap(left, right);
    }

    return (static_cast<quint64>(static_cast<quint32>(left)) << 32)
        | static_cast<quint32>(right);
}

QVector<NearbyMergeCandidate> findNearbyMergeCandidates(
    const RegionExtractionResult &regions,
    const QVector<SourceRegion> &sources,
    const std::vector<int> &sourceDataAtPixel) {
    QHash<quint64, NearbyMergeCandidate> candidateByPair;
    const int width = regions.imageSize.width();
    const int height = regions.imageSize.height();
    const int maximumPixelDistance = kNearbyMergeMaximumGap + 1;

    for (int pixel = 0; pixel < static_cast<int>(sourceDataAtPixel.size()); ++pixel) {
        const int source = sourceDataAtPixel[static_cast<size_t>(pixel)];
        if (source < 0) {
            continue;
        }
        const int x = pixel % width;
        const int y = pixel / width;
        const int directNeighbors[] = {
            x > 0 ? pixel - 1 : -1,
            x + 1 < width ? pixel + 1 : -1,
            y > 0 ? pixel - width : -1,
            y + 1 < height ? pixel + width : -1,
        };
        const bool boundary = std::any_of(
            std::begin(directNeighbors), std::end(directNeighbors),
            [&](int neighbor) {
                return neighbor < 0
                    || sourceDataAtPixel[static_cast<size_t>(neighbor)] != source;
            });
        if (!boundary) {
            continue;
        }
        for (int offsetY = -maximumPixelDistance;
             offsetY <= maximumPixelDistance; ++offsetY) {
            const int targetY = y + offsetY;
            if (targetY < 0 || targetY >= height) {
                continue;
            }
            const int horizontalLimit = maximumPixelDistance - std::abs(offsetY);
            for (int offsetX = -horizontalLimit; offsetX <= horizontalLimit; ++offsetX) {
                const int distance = std::abs(offsetX) + std::abs(offsetY);
                if (distance < 2) {
                    continue;
                }
                const int targetX = x + offsetX;
                if (targetX < 0 || targetX >= width) {
                    continue;
                }
                const int targetPixel = targetY * width + targetX;
                const int targetSource =
                    sourceDataAtPixel[static_cast<size_t>(targetPixel)];
                if (targetSource <= source
                    || !sameColor(regions.regions[sources[source].resultIndex].color,
                                  regions.regions[sources[targetSource].resultIndex].color)) {
                    continue;
                }
                const quint64 key = sourcePairKey(source, targetSource);
                const int gap = distance - 1;
                const int edgeClearance = std::min({
                    x,
                    y,
                    width - x - 1,
                    height - y - 1,
                    targetX,
                    targetY,
                    width - targetX - 1,
                    height - targetY - 1,
                });
                auto candidate = candidateByPair.find(key);
                if (candidate == candidateByPair.end()
                    || gap < candidate->gap
                    || (gap == candidate->gap
                        && edgeClearance > candidate->edgeClearance)
                    || (gap == candidate->gap
                        && edgeClearance == candidate->edgeClearance
                        && pixel < candidate->leftPixel)
                    || (gap == candidate->gap
                        && edgeClearance == candidate->edgeClearance
                        && pixel == candidate->leftPixel
                        && targetPixel < candidate->rightPixel)) {
                    NearbyMergeCandidate value;
                    value.leftSource = source;
                    value.rightSource = targetSource;
                    value.leftPixel = pixel;
                    value.rightPixel = targetPixel;
                    value.gap = gap;
                    value.edgeClearance = edgeClearance;
                    candidateByPair.insert(key, value);
                }
            }
        }
    }
    QVector<NearbyMergeCandidate> result = candidateByPair.values();
    std::sort(result.begin(), result.end(),
              [](const NearbyMergeCandidate &left, const NearbyMergeCandidate &right) {
                  if (left.gap != right.gap) {
                      return left.gap < right.gap;
                  }
                  if (left.leftSource != right.leftSource) {
                      return left.leftSource < right.leftSource;
                  }
                  return left.rightSource < right.rightSource;
              });

    return result;
}

QVector<int> foregroundBridge(int start,
                              int target,
                              const QSize &imageSize,
                              const std::vector<std::uint8_t> &foreground) {
    const int width = imageSize.width();
    const int height = imageSize.height();
    const QPoint startPoint(start % width, start / width);
    const QPoint targetPoint(target % width, target / width);
    const int directDistance = std::abs(startPoint.x() - targetPoint.x())
        + std::abs(startPoint.y() - targetPoint.y());
    const int maximumSteps = directDistance + kNearbyMergeMaximumDetour;
    const QRect searchBounds = QRect(startPoint, targetPoint).normalized()
        .adjusted(-kNearbyMergeMaximumDetour, -kNearbyMergeMaximumDetour,
                  kNearbyMergeMaximumDetour, kNearbyMergeMaximumDetour)
        .intersected(QRect(0, 0, width, height));
    const int localWidth = searchBounds.width();
    const int localHeight = searchBounds.height();
    QVector<int> parents(localWidth * localHeight, -2);
    QVector<int> distances(localWidth * localHeight, -1);
    QVector<int> queue;
    const auto localIndex = [&](int pixel) {
        const int x = pixel % width - searchBounds.left();
        const int y = pixel / width - searchBounds.top();
        return y * localWidth + x;
    };
    const int startLocal = localIndex(start);
    parents[startLocal] = -1;
    distances[startLocal] = 0;
    queue.push_back(start);
    int queueIndex = 0;
    while (queueIndex < queue.size()) {
        const int pixel = queue[queueIndex++];
        if (pixel == target) {
            break;
        }
        const int distance = distances[localIndex(pixel)];
        if (distance >= maximumSteps) {
            continue;
        }
        const int x = pixel % width;
        const int y = pixel / width;
        const int neighbors[] = {
            x > searchBounds.left() ? pixel - 1 : -1,
            x < searchBounds.right() ? pixel + 1 : -1,
            y > searchBounds.top() ? pixel - width : -1,
            y < searchBounds.bottom() ? pixel + width : -1,
        };
        for (const int neighbor : neighbors) {
            if (neighbor < 0 || foreground[static_cast<size_t>(neighbor)] == 0) {
                continue;
            }
            const int neighborLocal = localIndex(neighbor);
            if (parents[neighborLocal] != -2) {
                continue;
            }
            parents[neighborLocal] = pixel;
            distances[neighborLocal] = distance + 1;
            queue.push_back(neighbor);
        }
    }
    const int targetLocal = localIndex(target);
    if (parents[targetLocal] == -2 || distances[targetLocal] > maximumSteps) {
        return {};
    }
    QVector<int> result;
    for (int pixel = target; pixel >= 0; pixel = parents[localIndex(pixel)]) {
        result.push_back(pixel);
        if (pixel == start) {
            break;
        }
    }
    std::reverse(result.begin(), result.end());

    return result;
}

RegionLayerPlan fallbackPlan(const RegionExtractionResult &regions,
                             RegionLayerPlan plan,
                             const QString &reason) {
    plan.units.clear();
    plan.fallback = true;
    plan.fallbackReason = reason;
    plan.diagnostics << QStringLiteral("fallback reason=%1").arg(reason);
    for (int i = 0; i < regions.regions.size(); ++i) {
        const ExtractedRegion &region = regions.regions[i];
        if (region.lineart) {
            continue;
        }
        RegionLayerUnit unit;
        unit.color = region.color;
        unit.outline = region.outline;
        unit.sourceRegionIndices.push_back(i);
        unit.area = region.area;
        plan.units.push_back(std::move(unit));
    }
    QVector<QPainterPath> outerPaths;
    QVector<QVector<int>> outgoing(plan.units.size());
    QVector<int> indegree(plan.units.size(), 0);
    int fallbackEdges = 0;
    outerPaths.reserve(plan.units.size());
    for (const RegionLayerUnit &unit : plan.units) {
        outerPaths.push_back(outerPath(unit.outline));
    }
    for (int child = 0; child < plan.units.size(); ++child) {
        for (int parent = 0; parent < plan.units.size(); ++parent) {
            if (child != parent
                && strictlyContains(outerPaths[parent], outerPaths[child])) {
                outgoing[parent].push_back(child);
                ++indegree[child];
                ++fallbackEdges;
            }
        }
    }
    QVector<int> order;
    QVector<bool> scheduled(plan.units.size(), false);
    order.reserve(plan.units.size());
    while (order.size() < plan.units.size()) {
        int next = -1;
        for (int i = 0; i < plan.units.size(); ++i) {
            const int sourceIndex = plan.units[i].sourceRegionIndices.front();
            if (indegree[i] == 0 && !scheduled[i]
                && (next < 0
                    || sourceIndex < plan.units[next].sourceRegionIndices.front())) {
                next = i;
            }
        }
        if (next < 0) {
            plan.diagnostics << QStringLiteral("fallback_order=source-order cycle=yes");
            return plan;
        }
        order.push_back(next);
        scheduled[next] = true;
        for (const int child : outgoing[next]) {
            --indegree[child];
        }
    }
    QVector<RegionLayerUnit> orderedUnits;
    orderedUnits.reserve(plan.units.size());
    for (const int index : order) {
        orderedUnits.push_back(std::move(plan.units[index]));
    }
    plan.units = std::move(orderedUnits);
    plan.diagnostics << QStringLiteral("fallback_order=containment ordering_edges=%1")
                            .arg(fallbackEdges);

    return plan;
}

QImage renderUnits(const QSize &imageSize, const QVector<RegionLayerUnit> &units) {
    QImage rendered(imageSize, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const RegionLayerUnit &unit : units) {
        painter.setBrush(unit.color);
        painter.drawPath(outerPath(unit.outline));
    }
    painter.end();

    return rendered;
}

QImage renderOwners(const QSize &imageSize,
                    const QVector<RegionLayerUnit> &units,
                    const QVector<int> &groupByRegion) {
    QImage rendered(imageSize, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const RegionLayerUnit &unit : units) {
        const int source = unit.sourceRegionIndices.front();
        const int group = groupByRegion[source];
        const QRgb encodedOwner = 0xff000000u | static_cast<QRgb>(group + 1);
        painter.setBrush(QColor::fromRgba(encodedOwner));
        painter.drawPath(outerPath(unit.outline));
    }
    painter.end();

    return rendered;
}

int decodedOwner(QRgb encodedOwner) {
    return qAlpha(encodedOwner) > 0
        ? static_cast<int>(encodedOwner & 0x00ffffffu) - 1 : -1;
}

int validatePlan(const QSize &imageSize,
                 const QVector<RegionLayerUnit> &baselineUnits,
                 const QVector<RegionLayerUnit> &units,
                 const QImage &baselineOwners,
                 const QVector<int> &groupByRegion,
                 QSet<int> &mismatchGroups) {
    const QImage expected = renderUnits(imageSize, baselineUnits);
    const QImage actual = renderUnits(imageSize, units);
    const QImage actualOwners = renderOwners(imageSize, units, groupByRegion);
    int mismatches = 0;
    mismatchGroups.clear();
    for (int y = 0; y < imageSize.height(); ++y) {
        const QRgb *expectedRow = reinterpret_cast<const QRgb *>(expected.constScanLine(y));
        const QRgb *actualRow = reinterpret_cast<const QRgb *>(actual.constScanLine(y));
        for (int x = 0; x < imageSize.width(); ++x) {
            if (expectedRow[x] != actualRow[x]) {
                ++mismatches;
                const int expectedOwner = decodedOwner(baselineOwners.pixel(x, y));
                const int actualOwner = decodedOwner(actualOwners.pixel(x, y));
                if (expectedOwner >= 0) {
                    mismatchGroups.insert(expectedOwner);
                }
                if (actualOwner >= 0) {
                    mismatchGroups.insert(actualOwner);
                }
            }
        }
    }

    return mismatches;
}

QString indicesText(const QVector<int> &indices) {
    QStringList values;
    values.reserve(indices.size());
    for (const int index : indices) {
        values.push_back(QString::number(index));
    }

    return values.join(QLatin1Char(','));
}

QVector<int> dependencyCycle(const QVector<QVector<int>> &outgoing) {
    QVector<int> states(outgoing.size(), 0);
    QVector<int> stackPositions(outgoing.size(), -1);
    QVector<int> stack;
    QVector<int> result;
    std::function<bool(int)> visit;
    visit = [&](int node) {
        states[node] = 1;
        stackPositions[node] = stack.size();
        stack.push_back(node);
        for (const int next : outgoing[node]) {
            if (states[next] == 0 && visit(next)) {
                return true;
            }
            if (states[next] == 1) {
                result = stack.mid(stackPositions[next]);
                return true;
            }
        }
        stack.pop_back();
        stackPositions[node] = -1;
        states[node] = 2;

        return false;
    };
    for (int node = 0; node < outgoing.size(); ++node) {
        if (states[node] == 0 && visit(node)) {
            break;
        }
    }

    return result;
}

} // namespace

namespace {

RegionLayerPlan buildRegionLayerPlanAttempt(
    const RegionExtractionResult &regions,
    const QSet<quint64> &disabledNearbyPairs,
    const QSet<int> &isolatedConflictSources,
    QSet<quint64> &cycleNearbyPairs,
    QSet<int> &cycleIsolationSources) {
    RegionLayerPlan plan;
    QVector<SourceRegion> sources;
    cycleNearbyPairs.clear();
    cycleIsolationSources.clear();
    plan.conflictIsolatedSourceCount = isolatedConflictSources.size();
    for (int i = 0; i < regions.regions.size(); ++i) {
        const ExtractedRegion &region = regions.regions[i];
        if (region.lineart) {
            continue;
        }
        SourceRegion source;
        source.outerPath = outerPath(region.outline);
        source.resultIndex = i;
        source.label = region.id;
        sources.push_back(std::move(source));
    }
    plan.inputRegionCount = sources.size();
    plan.diagnostics << QStringLiteral("source_regions=%1 raster=%2")
                            .arg(plan.inputRegionCount)
                            .arg(regions.raster ? QStringLiteral("yes")
                                                : QStringLiteral("no"));
    if (sources.isEmpty()) {
        return plan;
    }
    if (!regions.raster
        || regions.raster->labels.size()
            != static_cast<size_t>(regions.imageSize.width()) * regions.imageSize.height()) {
        return fallbackPlan(regions, std::move(plan),
                            QStringLiteral("cleaned raster labels are unavailable"));
    }

    int maximumLabel = -1;
    for (const int label : regions.raster->labels) {
        maximumLabel = std::max(maximumLabel, label);
    }
    for (const SourceRegion &source : sources) {
        maximumLabel = std::max(maximumLabel, source.label);
    }
    QVector<int> sourceDataByLabel(maximumLabel + 1, -1);
    for (int i = 0; i < sources.size(); ++i) {
        const int label = sources[i].label;
        if (label < 0 || sourceDataByLabel[label] >= 0 || sources[i].outerPath.isEmpty()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("source labels or contours are invalid"));
        }
        sourceDataByLabel[label] = i;
    }
    const std::vector<int> &labels = regions.raster->labels;
    std::vector<std::uint8_t> derivedForeground;
    const std::vector<std::uint8_t> *foreground = &regions.raster->foreground;
    if (foreground->size() != labels.size()) {
        derivedForeground.resize(labels.size(), 0);
        for (size_t pixel = 0; pixel < labels.size(); ++pixel) {
            derivedForeground[pixel] = labels[pixel] >= 0 ? 1 : 0;
        }
        foreground = &derivedForeground;
    }
    std::vector<int> sourceDataAtPixel(labels.size(), -1);
    for (int pixel = 0; pixel < static_cast<int>(labels.size()); ++pixel) {
        const int label = labels[static_cast<size_t>(pixel)];
        if (label >= 0 && label < sourceDataByLabel.size()) {
            const int sourceDataIndex = sourceDataByLabel[label];
            if (sourceDataIndex >= 0) {
                sources[sourceDataIndex].pixels.push_back(pixel);
                sourceDataAtPixel[static_cast<size_t>(pixel)] = sourceDataIndex;
            }
        }
    }
    if (std::any_of(sources.cbegin(), sources.cend(),
                    [](const SourceRegion &source) { return source.pixels.isEmpty(); })) {
        return fallbackPlan(regions, std::move(plan),
                            QStringLiteral("an extracted source has no raster ownership"));
    }

    DisjointSet sets(sources.size());
    const int width = regions.imageSize.width();
    const int height = regions.imageSize.height();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int pixel = y * width + x;
            const int label = labels[static_cast<size_t>(pixel)];
            const int source = label >= 0 && label < sourceDataByLabel.size()
                ? sourceDataByLabel[label] : -1;
            if (source < 0) {
                continue;
            }
            const int neighbors[] = {
                x + 1 < width ? pixel + 1 : -1,
                y + 1 < height ? pixel + width : -1,
            };
            for (const int neighborPixel : neighbors) {
                if (neighborPixel < 0) {
                    continue;
                }
                const int neighborLabel = labels[static_cast<size_t>(neighborPixel)];
                const int neighbor = neighborLabel >= 0
                        && neighborLabel < sourceDataByLabel.size()
                    ? sourceDataByLabel[neighborLabel] : -1;
                if (neighbor >= 0 && neighbor != source
                    && !isolatedConflictSources.contains(sources[source].resultIndex)
                    && !isolatedConflictSources.contains(sources[neighbor].resultIndex)
                    && sameColor(regions.regions[sources[source].resultIndex].color,
                                 regions.regions[sources[neighbor].resultIndex].color)
                    && sets.unite(source, neighbor)) {
                    ++plan.sameColorMergeCount;
                }
            }
        }
    }

    const QVector<NearbyMergeCandidate> nearbyCandidates =
        findNearbyMergeCandidates(regions, sources, sourceDataAtPixel);
    QVector<NearbyMergeBridge> nearbyBridges;
    int alreadyConnectedCandidates = 0;
    int rejectedForegroundCandidates = 0;
    for (const NearbyMergeCandidate &candidate : nearbyCandidates) {
        const quint64 pairKey =
            sourcePairKey(candidate.leftSource, candidate.rightSource);
        const bool isolated =
            isolatedConflictSources.contains(sources[candidate.leftSource].resultIndex)
            || isolatedConflictSources.contains(sources[candidate.rightSource].resultIndex);
        if (disabledNearbyPairs.contains(pairKey) || isolated) {
            ++plan.nearbyConflictRejectCount;
            plan.diagnostics << QStringLiteral("nearby rejected=%1 "
                                               "sources=%2,%3 gap=%4")
                                    .arg(isolated ? QStringLiteral("isolated-cycle-group")
                                                  : QStringLiteral("dependency-cycle"))
                                    .arg(sources[candidate.leftSource].resultIndex)
                                    .arg(sources[candidate.rightSource].resultIndex)
                                    .arg(candidate.gap);
            continue;
        }
        if (sets.root(candidate.leftSource) == sets.root(candidate.rightSource)) {
            ++alreadyConnectedCandidates;
            continue;
        }
        QVector<int> bridge = foregroundBridge(candidate.leftPixel,
                                               candidate.rightPixel,
                                               regions.imageSize,
                                               *foreground);
        if (bridge.isEmpty()) {
            ++rejectedForegroundCandidates;
            continue;
        }
        if (sets.unite(candidate.leftSource, candidate.rightSource)) {
            ++plan.sameColorMergeCount;
            ++plan.nearbySameColorMergeCount;
        }
        for (const int bridgePixel : bridge) {
            const int bridgeSource = sourceDataAtPixel[static_cast<size_t>(bridgePixel)];
            if (bridgeSource >= 0
                && sameColor(regions.regions[sources[candidate.leftSource].resultIndex].color,
                             regions.regions[sources[bridgeSource].resultIndex].color)
                && sets.unite(candidate.leftSource, bridgeSource)) {
                ++plan.sameColorMergeCount;
                ++plan.nearbySameColorMergeCount;
            }
        }
        NearbyMergeBridge accepted;
        accepted.pixels = std::move(bridge);
        accepted.pairKey = pairKey;
        accepted.leftSource = candidate.leftSource;
        nearbyBridges.push_back(std::move(accepted));
        plan.diagnostics << QStringLiteral("nearby color=%1 sources=%2,%3 gap=%4 "
                                           "bridge_steps=%5 detour=%6")
                                .arg(regions.regions[sources[candidate.leftSource]
                                                        .resultIndex].color.name(QColor::HexRgb))
                                .arg(sources[candidate.leftSource].resultIndex)
                                .arg(sources[candidate.rightSource].resultIndex)
                                .arg(candidate.gap)
                                .arg(nearbyBridges.back().pixels.size() - 1)
                                .arg(nearbyBridges.back().pixels.size()
                                     - candidate.gap - 2);
    }
    plan.diagnostics << QStringLiteral("nearby_candidates=%1 accepted_bridges=%2 "
                                       "conflict_rejections=%3 already_connected=%4 "
                                       "no_foreground_path=%5")
                            .arg(nearbyCandidates.size())
                            .arg(nearbyBridges.size())
                            .arg(plan.nearbyConflictRejectCount)
                            .arg(alreadyConnectedCandidates)
                            .arg(rejectedForegroundCandidates);
    QVector<int> parentBySource(sources.size(), -1);
    for (int child = 0; child < sources.size(); ++child) {
        double bestSize = std::numeric_limits<double>::max();
        for (int parent = 0; parent < sources.size(); ++parent) {
            if (child == parent
                || !strictlyContains(sources[parent].outerPath, sources[child].outerPath)) {
                continue;
            }
            const double size = enclosureSize(sources[parent].outerPath);
            if (size < bestSize) {
                parentBySource[child] = parent;
                bestSize = size;
            }
        }
        const int parent = parentBySource[child];
        if (parent >= 0
            && !isolatedConflictSources.contains(sources[child].resultIndex)
            && !isolatedConflictSources.contains(sources[parent].resultIndex)
            && sameColor(regions.regions[sources[child].resultIndex].color,
                         regions.regions[sources[parent].resultIndex].color)
            && sets.unite(child, parent)) {
            ++plan.sameColorMergeCount;
        }
    }

    QHash<int, int> groupByRoot;
    QVector<LayerGroup> groups;
    QVector<int> groupBySourceData(sources.size(), -1);
    for (int i = 0; i < sources.size(); ++i) {
        const int root = sets.root(i);
        int groupIndex = groupByRoot.value(root, -1);
        if (groupIndex < 0) {
            groupIndex = groups.size();
            groupByRoot.insert(root, groupIndex);
            groups.push_back(LayerGroup{});
        }
        LayerGroup &group = groups[groupIndex];
        groupBySourceData[i] = groupIndex;
        const ExtractedRegion &region = regions.regions[sources[i].resultIndex];
        if (!group.color.isValid()) {
            group.color = region.color;
        }
        group.sourceRegionIndices.push_back(sources[i].resultIndex);
        group.pixels += sources[i].pixels;
        group.sourceArea += sources[i].pixels.size();
        if (group.firstSourceIndex < 0 || sources[i].resultIndex < group.firstSourceIndex) {
            group.firstSourceIndex = sources[i].resultIndex;
        }
    }
    for (const NearbyMergeBridge &bridge : nearbyBridges) {
        const int groupIndex = groupBySourceData[bridge.leftSource];
        groups[groupIndex].pixels += bridge.pixels;
    }
    for (LayerGroup &group : groups) {
        std::sort(group.sourceRegionIndices.begin(), group.sourceRegionIndices.end());
        std::sort(group.pixels.begin(), group.pixels.end());
        group.pixels.erase(std::unique(group.pixels.begin(), group.pixels.end()),
                           group.pixels.end());
        group.fillArea = group.pixels.size();
        if (group.sourceRegionIndices.size() == 1) {
            group.outline = regions.regions[group.sourceRegionIndices.front()].outline;
        } else {
            group.outline = tracePixels(group.pixels, regions.imageSize,
                                        regions.raster->traceParams);
        }
        group.outerPath = outerPath(group.outline);
        if (group.outerPath.isEmpty()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("a same-colour union could not be traced"));
        }
    }

    QVector<int> parentByGroup(groups.size(), -1);
    for (int child = 0; child < groups.size(); ++child) {
        double bestSize = std::numeric_limits<double>::max();
        for (int parent = 0; parent < groups.size(); ++parent) {
            if (child == parent
                || !strictlyContains(groups[parent].outerPath, groups[child].outerPath)) {
                continue;
            }
            const double size = enclosureSize(groups[parent].outerPath);
            if (size < bestSize) {
                parentByGroup[child] = parent;
                bestSize = size;
            }
        }
    }

    QVector<QVector<int>> absorbedChildren(groups.size());
    for (int child = 0; child < groups.size(); ++child) {
        const int parent = parentByGroup[child];
        if (parent < 0
            || sameColor(groups[parent].color, groups[child].color)
            || isolatedConflictSources.contains(groups[parent].firstSourceIndex)
            || isolatedConflictSources.contains(groups[child].firstSourceIndex)) {
            continue;
        }
        const double fraction = static_cast<double>(groups[child].sourceArea)
            / std::max(1, groups[parent].sourceArea);
        if (fraction <= kMaximumAbsorbedAreaFraction) {
            absorbedChildren[parent].push_back(child);
            plan.absorbedRegionCount += groups[child].sourceRegionIndices.size();
        }
    }
    std::function<void(int, QVector<int> *, QVector<int> *)> appendAbsorbed;
    appendAbsorbed = [&](int groupIndex, QVector<int> *pixels, QVector<int> *indices) {
        for (const int child : absorbedChildren[groupIndex]) {
            *pixels += groups[child].pixels;
            *indices += groups[child].sourceRegionIndices;
            appendAbsorbed(child, pixels, indices);
        }
    };
    for (int parent = 0; parent < groups.size(); ++parent) {
        if (absorbedChildren[parent].isEmpty()) {
            continue;
        }
        QVector<int> mergedPixels = groups[parent].pixels;
        appendAbsorbed(parent, &mergedPixels, &groups[parent].absorbedRegionIndices);
        std::sort(groups[parent].absorbedRegionIndices.begin(),
                  groups[parent].absorbedRegionIndices.end());
        groups[parent].outline = tracePixels(mergedPixels, regions.imageSize,
                                             regions.raster->traceParams);
        groups[parent].outerPath = outerPath(groups[parent].outline);
        groups[parent].fillArea = mergedPixels.size();
        if (groups[parent].outerPath.isEmpty()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("an absorbed backdrop could not be traced"));
        }
    }

    QVector<int> groupByRegion(regions.regions.size(), -1);
    for (int source = 0; source < sources.size(); ++source) {
        groupByRegion[sources[source].resultIndex] = groupBySourceData[source];
    }
    const RegionLayerPlan baseline =
        fallbackPlan(regions, RegionLayerPlan{}, QStringLiteral("validation baseline"));
    const QImage baselineOwners =
        renderOwners(regions.imageSize, baseline.units, groupByRegion);
    QSet<quint64> dependencyKeys;
    for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        QImage coverage(regions.imageSize, QImage::Format_Grayscale8);
        if (coverage.isNull()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("layer coverage could not be allocated"));
        }
        coverage.fill(0);
        QPainter painter(&coverage);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillPath(groups[groupIndex].outerPath, Qt::white);
        painter.end();
        const QRect bounds = groups[groupIndex].outerPath.boundingRect().toAlignedRect()
            .intersected(QRect(QPoint(0, 0), regions.imageSize));
        for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
            const uchar *row = coverage.constScanLine(y);
            for (int x = bounds.left(); x <= bounds.right(); ++x) {
                if (row[x] == 0) {
                    continue;
                }
                const int owner = decodedOwner(baselineOwners.pixel(x, y));
                if (owner < 0 || owner == groupIndex
                    || sameColor(groups[owner].color, groups[groupIndex].color)) {
                    continue;
                }
                const quint64 key =
                    (static_cast<quint64>(static_cast<quint32>(groupIndex)) << 32)
                    | static_cast<quint32>(owner);
                dependencyKeys.insert(key);
            }
        }
    }
    QVector<quint64> sortedDependencies = dependencyKeys.values();
    std::sort(sortedDependencies.begin(), sortedDependencies.end());
    QVector<QVector<int>> outgoing(groups.size());
    QVector<int> indegree(groups.size(), 0);
    for (const quint64 key : sortedDependencies) {
        const int parent = static_cast<int>(key >> 32);
        const int child = static_cast<int>(key & 0xffffffffu);
        outgoing[parent].push_back(child);
        ++indegree[child];
    }
    plan.orderingEdgeCount = sortedDependencies.size();

    QVector<int> order;
    QVector<bool> scheduled(groups.size(), false);
    order.reserve(groups.size());
    while (order.size() < groups.size()) {
        int next = -1;
        for (int i = 0; i < groups.size(); ++i) {
            if (indegree[i] == 0
                && !scheduled[i]
                && (next < 0 || groups[i].firstSourceIndex < groups[next].firstSourceIndex)) {
                next = i;
            }
        }
        if (next < 0) {
            const QVector<int> cycleGroups = dependencyCycle(outgoing);
            QSet<int> cycleGroupSet(cycleGroups.cbegin(), cycleGroups.cend());
            QHash<int, int> bridgeCountByGroup;
            for (const NearbyMergeBridge &bridge : nearbyBridges) {
                const int group = groupBySourceData[bridge.leftSource];
                if (cycleGroupSet.contains(group)) {
                    ++bridgeCountByGroup[group];
                }
            }
            int splitGroup = -1;
            for (auto it = bridgeCountByGroup.cbegin();
                 it != bridgeCountByGroup.cend(); ++it) {
                if (splitGroup < 0 || it.value() < bridgeCountByGroup.value(splitGroup)
                    || (it.value() == bridgeCountByGroup.value(splitGroup)
                        && it.key() < splitGroup)) {
                    splitGroup = it.key();
                }
            }
            for (const NearbyMergeBridge &bridge : nearbyBridges) {
                if (groupBySourceData[bridge.leftSource] == splitGroup) {
                    cycleNearbyPairs.insert(bridge.pairKey);
                }
            }
            if (!cycleNearbyPairs.isEmpty()) {
                return plan;
            }
            int isolateGroup = -1;
            for (const int group : cycleGroups) {
                const bool modified = groups[group].sourceRegionIndices.size() > 1
                    || !absorbedChildren[group].isEmpty();
                if (!modified) {
                    continue;
                }
                if (isolateGroup < 0
                    || groups[group].sourceRegionIndices.size()
                        < groups[isolateGroup].sourceRegionIndices.size()
                    || (groups[group].sourceRegionIndices.size()
                            == groups[isolateGroup].sourceRegionIndices.size()
                        && group < isolateGroup)) {
                    isolateGroup = group;
                }
            }
            if (isolateGroup >= 0) {
                for (const int sourceIndex : groups[isolateGroup].sourceRegionIndices) {
                    if (!isolatedConflictSources.contains(sourceIndex)) {
                        cycleIsolationSources.insert(sourceIndex);
                    }
                }
            }
            if (!cycleIsolationSources.isEmpty()) {
                return plan;
            }
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("layer dependencies contain a cycle"));
        }
        order.push_back(next);
        scheduled[next] = true;
        indegree[next] = -1;
        for (const int child : outgoing[next]) {
            --indegree[child];
        }
    }

    for (int drawOrder = 0; drawOrder < order.size(); ++drawOrder) {
        const LayerGroup &group = groups[order[drawOrder]];
        RegionLayerUnit unit;
        unit.color = group.color;
        unit.outline = group.outline;
        unit.sourceRegionIndices = group.sourceRegionIndices;
        unit.absorbedRegionIndices = group.absorbedRegionIndices;
        unit.area = group.fillArea;
        plan.diagnostics << QStringLiteral("order=%1 color=%2 sources=%3 absorbed=%4")
                                .arg(drawOrder)
                                .arg(group.color.name(QColor::HexRgb))
                                .arg(indicesText(group.sourceRegionIndices))
                                .arg(indicesText(group.absorbedRegionIndices));
        plan.units.push_back(std::move(unit));
    }
    QSet<int> validationMismatchGroups;
    plan.validationMismatchPixels =
        validatePlan(regions.imageSize, baseline.units, plan.units,
                     baselineOwners, groupByRegion, validationMismatchGroups);
    plan.diagnostics << QStringLiteral("same_color_merges=%1 nearby_same_color_merges=%2 "
                                       "nearby_conflict_rejections=%3 "
                                       "conflict_isolated_sources=%4 absorbed_regions=%5 "
                                       "ordering_edges=%6 validation_mismatches=%7")
                            .arg(plan.sameColorMergeCount)
                            .arg(plan.nearbySameColorMergeCount)
                            .arg(plan.nearbyConflictRejectCount)
                            .arg(plan.conflictIsolatedSourceCount)
                            .arg(plan.absorbedRegionCount)
                            .arg(plan.orderingEdgeCount)
                            .arg(plan.validationMismatchPixels);
    if (plan.validationMismatchPixels > 0) {
        for (const int group : validationMismatchGroups) {
            if (groups[group].sourceRegionIndices.size() <= 1
                && absorbedChildren[group].isEmpty()) {
                continue;
            }
            for (const int sourceIndex : groups[group].sourceRegionIndices) {
                if (!isolatedConflictSources.contains(sourceIndex)) {
                    cycleIsolationSources.insert(sourceIndex);
                }
            }
        }
        if (!cycleIsolationSources.isEmpty()) {
            return plan;
        }
        return fallbackPlan(regions, std::move(plan),
                            QStringLiteral("planned layers changed %1 rendered pixels")
                                .arg(plan.validationMismatchPixels));
    }

    return plan;
}

} // namespace

RegionLayerPlan buildRegionLayerPlan(const RegionExtractionResult &regions) {
    QSet<quint64> disabledNearbyPairs;
    QSet<int> isolatedConflictSources;
    int dependencyCycleRebuilds = 0;
    while (true) {
        QSet<quint64> cycleNearbyPairs;
        QSet<int> cycleIsolationSources;
        RegionLayerPlan plan = buildRegionLayerPlanAttempt(
            regions, disabledNearbyPairs, isolatedConflictSources,
            cycleNearbyPairs, cycleIsolationSources);
        if (cycleNearbyPairs.isEmpty() && cycleIsolationSources.isEmpty()) {
            plan.diagnostics.prepend(
                QStringLiteral("dependency_cycle_rebuilds=%1 suppressed_bridges=%2 "
                               "isolated_sources=%3")
                    .arg(dependencyCycleRebuilds)
                    .arg(disabledNearbyPairs.size())
                    .arg(isolatedConflictSources.size()));
            return plan;
        }
        disabledNearbyPairs.unite(cycleNearbyPairs);
        isolatedConflictSources.unite(cycleIsolationSources);
        ++dependencyCycleRebuilds;
    }
}

} // namespace gui
