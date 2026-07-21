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

int validatePlan(const RegionExtractionResult &regions,
                 const QVector<RegionLayerUnit> &units,
                 const QVector<int> &sourceIndexByLabel) {
    QImage rendered(regions.imageSize, QImage::Format_ARGB32);
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

    const std::vector<int> &labels = regions.raster->labels;
    const int width = regions.imageSize.width();
    int mismatches = 0;
    for (int pixel = 0; pixel < static_cast<int>(labels.size()); ++pixel) {
        const int label = labels[static_cast<size_t>(pixel)];
        if (label < 0 || label >= sourceIndexByLabel.size()) {
            continue;
        }
        const int sourceIndex = sourceIndexByLabel[label];
        if (sourceIndex < 0) {
            continue;
        }
        const QColor expected = regions.regions[sourceIndex].color;
        const QColor actual = rendered.pixelColor(pixel % width, pixel / width);
        if (expected.rgb() != actual.rgb() || actual.alpha() == 0) {
            ++mismatches;
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

} // namespace

RegionLayerPlan buildRegionLayerPlan(const RegionExtractionResult &regions) {
    RegionLayerPlan plan;
    QVector<SourceRegion> sources;
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
    QVector<int> sourceIndexByLabel(maximumLabel + 1, -1);
    for (int i = 0; i < sources.size(); ++i) {
        const int label = sources[i].label;
        if (label < 0 || sourceDataByLabel[label] >= 0 || sources[i].outerPath.isEmpty()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("source labels or contours are invalid"));
        }
        sourceDataByLabel[label] = i;
        sourceIndexByLabel[label] = sources[i].resultIndex;
    }
    const std::vector<int> &labels = regions.raster->labels;
    for (int pixel = 0; pixel < static_cast<int>(labels.size()); ++pixel) {
        const int label = labels[static_cast<size_t>(pixel)];
        if (label >= 0 && label < sourceDataByLabel.size()) {
            const int sourceDataIndex = sourceDataByLabel[label];
            if (sourceDataIndex >= 0) {
                sources[sourceDataIndex].pixels.push_back(pixel);
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
                    && sameColor(regions.regions[sources[source].resultIndex].color,
                                 regions.regions[sources[neighbor].resultIndex].color)
                    && sets.unite(source, neighbor)) {
                    ++plan.sameColorMergeCount;
                }
            }
        }
    }
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
            && sameColor(regions.regions[sources[child].resultIndex].color,
                         regions.regions[sources[parent].resultIndex].color)
            && sets.unite(child, parent)) {
            ++plan.sameColorMergeCount;
        }
    }

    QHash<int, int> groupByRoot;
    QVector<LayerGroup> groups;
    for (int i = 0; i < sources.size(); ++i) {
        const int root = sets.root(i);
        int groupIndex = groupByRoot.value(root, -1);
        if (groupIndex < 0) {
            groupIndex = groups.size();
            groupByRoot.insert(root, groupIndex);
            groups.push_back(LayerGroup{});
        }
        LayerGroup &group = groups[groupIndex];
        const ExtractedRegion &region = regions.regions[sources[i].resultIndex];
        if (!group.color.isValid()) {
            group.color = region.color;
        }
        group.sourceRegionIndices.push_back(sources[i].resultIndex);
        group.pixels += sources[i].pixels;
        group.sourceArea += sources[i].pixels.size();
        group.fillArea += sources[i].pixels.size();
        if (group.firstSourceIndex < 0 || sources[i].resultIndex < group.firstSourceIndex) {
            group.firstSourceIndex = sources[i].resultIndex;
        }
    }
    for (LayerGroup &group : groups) {
        std::sort(group.sourceRegionIndices.begin(), group.sourceRegionIndices.end());
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
        if (parent < 0 || sameColor(groups[parent].color, groups[child].color)) {
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

    QVector<QVector<int>> outgoing(groups.size());
    QVector<int> indegree(groups.size(), 0);
    for (int child = 0; child < groups.size(); ++child) {
        for (int parent = 0; parent < groups.size(); ++parent) {
            if (child == parent
                || !strictlyContains(groups[parent].outerPath, groups[child].outerPath)) {
                continue;
            }
            outgoing[parent].push_back(child);
            ++indegree[child];
            ++plan.orderingEdgeCount;
        }
    }

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
    plan.validationMismatchPixels = validatePlan(regions, plan.units, sourceIndexByLabel);
    plan.diagnostics << QStringLiteral("same_color_merges=%1 absorbed_regions=%2 "
                                       "ordering_edges=%3 validation_mismatches=%4")
                            .arg(plan.sameColorMergeCount)
                            .arg(plan.absorbedRegionCount)
                            .arg(plan.orderingEdgeCount)
                            .arg(plan.validationMismatchPixels);
    if (plan.validationMismatchPixels > 0) {
        return fallbackPlan(regions, std::move(plan),
                            QStringLiteral("planned layers changed %1 owned pixels")
                                .arg(plan.validationMismatchPixels));
    }

    return plan;
}

} // namespace gui
