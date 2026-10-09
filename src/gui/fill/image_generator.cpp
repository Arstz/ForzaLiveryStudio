#include "image_generator.h"

#include "image_preprocessor.h"
#include "image_region_plan.h"
#include "cubic_contour.h"
#include "lining_extract.h"
#include "profile_fit.h"
#include "region_layer_plan.h"
#include "region_shape_cost.h"
#include "thin_fit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <limits>

namespace gui {
namespace {

constexpr int kSquareShapeId = 101;
constexpr double kBackgroundBorderFraction = 0.5;
constexpr int kBackgroundCornerCount = 3;
constexpr double kBackgroundPadding = 1.0;
constexpr double kGeometricSpillAreaTolerance = 0.01;
constexpr double kLiningRegionFraction = 0.5;
constexpr std::array<double, 3> kCoverageGrowthSteps{0.25, 0.5, 1.0};
constexpr std::array<double, 5> kCoveragePatchRadii{16.0, 8.0, 4.0, 2.0, 0.0};
constexpr double kCoveragePixelHalfSize = 0.51;
constexpr double kRoundedPatchScale = 1.5;
constexpr int kCompositeSamplingScale = 4;
constexpr double kContourGeometryTolerance = 0.05;
constexpr qint64 kMaximumCompositeSamples = 8LL * 1024LL * 1024LL;
constexpr qint64 kCompositePrefixBytes = 32LL * 1024LL * 1024LL;

struct ImageComponents {
    RegionLayerPlan plan;
    QString error;
    QVector<int> labels;
    QVector<QRect> bounds;
    QVector<QPainterPath> sourceOutlines;
    QPainterPath silhouette;
    QPainterPath protectedEmpty;
    QVector<bool> protectedEmptyPixels;
    int bucketRegionsBeforePalette = 0;
    int mergedFragmentCount = 0;
    int background = -1;
    bool backgroundSurrounds = false;
};

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

RegionExtractionParams tracingParameters() {
    RegionExtractionParams parameters;
    parameters.traceSpeckle = 0;
    parameters.minRegionArea = 1;
    parameters.smallRegionMergeArea = 0;

    return parameters;
}

QPainterPath pixelPath(const QVector<int> &labels, int label, const QSize &size,
                       const QRect &bounds) {
    std::vector<std::uint8_t> mask(static_cast<size_t>(bounds.width()) * bounds.height());
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const int pixel = y * size.width() + x;
            const int local = (y - bounds.top()) * bounds.width() + x - bounds.left();
            mask[static_cast<size_t>(local)] = labels[pixel] == label;
        }
    }
    const QSize localSize = bounds.size();
    const QRect localBounds(QPoint(), localSize);
    const auto loops = fitMaskContours(mask, localSize, localBounds);
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const PenLoop &loop : loops) {
        path.addPath(penPath(loop.points));
    }
    if (path.isEmpty()) {
        path = traceMaskToPath(mask, localSize.width(), localSize.height(),
                               localBounds, tracingParameters());
    }

    return QTransform::fromTranslate(bounds.left(), bounds.top()).map(path);
}

void applyRegionPalette(BucketRegionsResult *buckets, const QVector<QColor> &palette,
                        const QImage &preparedPixels, const QImage &lining, int fragmentTolerance) {
    QVector<QColor> colors;
    QVector<int> parents(buckets->regions.size());
    std::iota(parents.begin(), parents.end(), 0);
    for (const BucketRegion &region : buckets->regions) {
        QColor selected = region.color;
        int bestDistance = std::numeric_limits<int>::max();
        for (const QColor &color : palette) {
            const int red = color.red() - region.color.red();
            const int green = color.green() - region.color.green();
            const int blue = color.blue() - region.color.blue();
            const int distance = red * red + green * green + blue * blue;
            if (distance < bestDistance) {
                selected = color;
                bestDistance = distance;
            }
        }
        colors.push_back(selected);
    }
    QVector<bool> protectedDetails(buckets->regions.size(), false);
    for (int label = 0; label < buckets->regions.size(); ++label) {
        protectedDetails[label] = buckets->regions[label].area < kDefaultImageGeneratorFragmentArea;
    }
    const int sourceWidth = buckets->imageSize.width();
    for (int pixel = 0; pixel < buckets->labels.size(); ++pixel) {
        const int label = buckets->labels[pixel];
        if (label < 0) {
            continue;
        }
        const int neighbors[] = {pixel % sourceWidth ? pixel - 1 : -1, pixel - sourceWidth};
        for (const int neighbor : neighbors) {
            const int other = neighbor >= 0 ? buckets->labels[neighbor] : -1;
            if (other < 0 || label == other) {
                continue;
            }
            const QColor &a = buckets->regions[label].color;
            const QColor &b = buckets->regions[other].color;
            if (std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                          std::abs(a.blue() - b.blue())}) <= fragmentTolerance) {
                protectedDetails[label] = false;
                protectedDetails[other] = false;
            }
        }
    }
    if (!preparedPixels.isNull()) {
        QVector<QHash<QRgb, int>> votes(buckets->regions.size());
        QVector<QHash<QRgb, int>> strokeColors(buckets->regions.size());
        QVector<int> strokeVotes(buckets->regions.size(), 0);
        for (int y = 0; y < preparedPixels.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(preparedPixels.constScanLine(y));
            for (int x = 0; x < preparedPixels.width(); ++x) {
                const int label = buckets->labels[y * preparedPixels.width() + x];
                if (label >= 0 && qAlpha(row[x]) > 0) {
                    ++votes[label][row[x]];
                    if (!lining.isNull() && qAlpha(lining.pixel(x, y)) > 0) {
                        ++strokeVotes[label];
                        ++strokeColors[label][lining.pixel(x, y)];
                    }
                }
            }
        }
        for (int label = 0; label < votes.size(); ++label) {
            int most = 0;
            QRgb selected = colors[label].rgba();
            for (auto it = votes[label].cbegin(); it != votes[label].cend(); ++it) {
                if (it.value() > most || (it.value() == most && it.key() < selected)) {
                    most = it.value();
                    selected = it.key();
                }
            }
            const BucketRegion &region = buckets->regions[label];
            bool stroke = false;
            if (strokeVotes[label] >= region.area * kLiningRegionFraction) {
                int mostStroke = 0;
                QRgb core = colors[label].rgba();
                for (auto it = strokeColors[label].cbegin(); it != strokeColors[label].cend(); ++it) {
                    if (it.value() > mostStroke || (it.value() == mostStroke && it.key() < core)) {
                        mostStroke = it.value();
                        core = it.key();
                    }
                }
                const QColor coreColor = QColor::fromRgba(core);
                stroke = std::max({std::abs(coreColor.red() - region.color.red()),
                    std::abs(coreColor.green() - region.color.green()),
                    std::abs(coreColor.blue() - region.color.blue())}) <= fragmentTolerance;
                if (stroke) {
                    QColor selectedStroke = coreColor;
                    int bestDistance = std::numeric_limits<int>::max();
                    for (const QColor &color : palette) {
                        const int red = color.red() - coreColor.red();
                        const int green = color.green() - coreColor.green();
                        const int blue = color.blue() - coreColor.blue();
                        const int distance = red * red + green * green + blue * blue;
                        if (distance < bestDistance) {
                            selectedStroke = color;
                            bestDistance = distance;
                        }
                    }
                    colors[label] = selectedStroke;
                }
            }
            if (!protectedDetails[label] && !stroke) {
                colors[label] = QColor::fromRgba(selected);
            }
        }
    }
    auto root = [&](int label) {
        int current = label;
        while (parents[current] != current) {
            parents[current] = parents[parents[current]];
            current = parents[current];
        }

        return current;
    };
    const int width = buckets->imageSize.width();
    for (int pixel = 0; pixel < buckets->labels.size(); ++pixel) {
        const int label = buckets->labels[pixel];
        if (label < 0) {
            continue;
        }
        const int neighbors[] = {pixel % width ? pixel - 1 : -1, pixel - width};
        for (const int neighbor : neighbors) {
            const int above = neighbor >= 0 ? buckets->labels[neighbor] : -1;
            if (above >= 0 && colors[label] == colors[above]) {
                parents[root(above)] = root(label);
            }
        }
    }
    QHash<int, int> indexByRoot;
    QVector<BucketRegion> regions;
    QVector<int> mapped(buckets->regions.size());
    for (int label = 0; label < buckets->regions.size(); ++label) {
        const int owner = root(label);
        auto found = indexByRoot.constFind(owner);
        int index = regions.size();
        if (found == indexByRoot.cend()) {
            indexByRoot.insert(owner, index);
            BucketRegion region = buckets->regions[label];
            region.color = colors[label];
            regions.push_back(std::move(region));
        } else {
            index = *found;
            regions[index].bounds = regions[index].bounds.united(buckets->regions[label].bounds);
            regions[index].area += buckets->regions[label].area;
        }
        mapped[label] = index;
    }
    for (int &label : buckets->labels) {
        if (label >= 0) {
            label = mapped[label];
        }
    }
    buckets->regions = std::move(regions);
}

int mergeBucketFragments(BucketRegionsResult *buckets, const QImage &lining,
                         int maximumArea, int fragmentTolerance, const std::function<bool()> &cancelled) {
    const int count = buckets->regions.size();
    const int width = buckets->imageSize.width();
    QVector<QSet<int>> adjacent(count);
    QVector<int> parents(count);
    QVector<int> strokes(count, 0);
    QVector<int> order(count);
    std::iota(parents.begin(), parents.end(), 0);
    std::iota(order.begin(), order.end(), 0);
    auto root = [&](int label) {
        int current = label;
        while (parents[current] != current) {
            parents[current] = parents[parents[current]];
            current = parents[current];
        }

        return current;
    };
    if (maximumArea <= 0) {
        return 0;
    }
    for (int pixel = 0; pixel < buckets->labels.size(); ++pixel) {
        if ((pixel & 4095) == 0 && stopped(cancelled)) {
            buckets->cancelled = true;
            return 0;
        }
        const int label = buckets->labels[pixel];
        if (label < 0) {
            continue;
        }
        if (!lining.isNull() && qAlpha(lining.pixel(pixel % width, pixel / width)) > 0) {
            ++strokes[label];
        }
        const int neighbors[] = {pixel % width ? pixel - 1 : -1, pixel - width};
        for (const int neighbor : neighbors) {
            const int other = neighbor >= 0 ? buckets->labels[neighbor] : -1;
            if (other >= 0 && other != label) {
                adjacent[label].insert(other);
                adjacent[other].insert(label);
            }
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](int left, int right) {
        return buckets->regions[left].area < buckets->regions[right].area;
    });
    auto merge = [&](int source, int target) {
        parents[source] = target;
        buckets->regions[target].area += buckets->regions[source].area;
        buckets->regions[target].bounds = buckets->regions[target].bounds.united(buckets->regions[source].bounds);
        strokes[target] += strokes[source];
        adjacent[target].unite(adjacent[source]);
    };
    int merged = 0;
    for (const int source : order) {
        if (stopped(cancelled)) {
            buckets->cancelled = true;
            return merged;
        }
        const BucketRegion &region = buckets->regions[source];
        if (root(source) != source || region.area >= maximumArea
            || (region.area >= kDefaultLiningMinimumPixels
                && strokes[source] >= region.area * kLiningRegionFraction)) {
            continue;
        }
        int best = -1;
        int bestDistance = std::numeric_limits<int>::max();
        for (const int neighbor : adjacent[source]) {
            const int target = root(neighbor);
            if (target == source) {
                continue;
            }
            const BucketRegion &candidate = buckets->regions[target];
            if (candidate.area < region.area || (candidate.area == region.area && target > source)) {
                continue;
            }
            const int red = std::abs(region.color.red() - candidate.color.red());
            const int green = std::abs(region.color.green() - candidate.color.green());
            const int blue = std::abs(region.color.blue() - candidate.color.blue());
            if (std::max({red, green, blue}) > fragmentTolerance) {
                continue;
            }
            const int distance = red * red + green * green + blue * blue;
            if (distance < bestDistance
                || (distance == bestDistance && (best < 0
                    || candidate.area > buckets->regions[best].area
                    || (candidate.area == buckets->regions[best].area && target < best)))) {
                best = target;
                bestDistance = distance;
            }
        }
        if (best < 0) {
            continue;
        }
        merge(source, best);
        ++merged;
        bool joined = true;
        while (joined) {
            joined = false;
            const QSet<int> neighbors = adjacent[best];
            for (const int neighbor : neighbors) {
                const int target = root(neighbor);
                if (target != best && buckets->regions[target].color == buckets->regions[best].color) {
                    merge(target, best);
                    ++merged;
                    joined = true;
                }
            }
        }
    }
    for (int &label : buckets->labels) {
        if (label >= 0) {
            label = root(label);
        }
    }
    QHash<int, int> mapped;
    QVector<BucketRegion> regions;
    for (int label = 0; label < count; ++label) {
        if (root(label) == label) {
            mapped.insert(label, regions.size());
            regions.push_back(buckets->regions[label]);
        }
    }
    for (int &label : buckets->labels) {
        if (label >= 0) {
            label = mapped.value(label);
        }
    }
    buckets->regions = std::move(regions);

    return merged;
}

ImageComponents extractImageComponents(const QImage &image, const QImage &lining,
                                bool isolateBackground, int tolerance,
                                const QVector<QColor> &palette, const QImage &preparedPixels, int fragmentArea, int fragmentTolerance,
                                const ImageGeneratorProgress &progress,
                                const std::function<bool()> &cancelled) {
    ImageComponents result;
    QVector<int> queue;
    const QSize size = image.size();
    const int width = image.width();
    const int height = image.height();
    const int count = width * height;
    const QPoint offsets[] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
    BucketRegionsResult buckets = floodGuideRegions(image, tolerance, cancelled,
        [&](int completed, int total) {
            if (progress) {
                progress(QStringLiteral("Finding Bucket regions"), completed, total);
            }
        });
    result.error = buckets.error;
    result.plan.cancelled = buckets.cancelled;
    if (buckets.cancelled || !buckets.error.isEmpty()) {
        return result;
    }
    result.bucketRegionsBeforePalette = buckets.regions.size();
    applyRegionPalette(&buckets, palette, preparedPixels, lining, fragmentTolerance);
    result.mergedFragmentCount = mergeBucketFragments(&buckets, lining, fragmentArea, fragmentTolerance, cancelled);
    if (buckets.cancelled) {
        result.plan.cancelled = true;
        return result;
    }
    result.labels = std::move(buckets.labels);
    QVector<int> liningPixels(buckets.regions.size(), 0);
    if (!lining.isNull()) {
        for (int y = 0; y < height; ++y) {
            if (stopped(cancelled)) {
                result.plan.cancelled = true;
                return result;
            }
            const QRgb *row = reinterpret_cast<const QRgb *>(lining.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                const int label = result.labels[y * width + x];
                if (label >= 0 && qAlpha(row[x]) > 0) {
                    ++liningPixels[label];
                }
            }
        }
    }
    for (int label = 0; label < buckets.regions.size(); ++label) {
        const BucketRegion &bucket = buckets.regions[label];
        RegionLayerUnit unit;
        unit.color = bucket.color;
        unit.sourceRegionIndices = {label};
        unit.area = bucket.area;
        unit.lining = liningPixels[label] >= bucket.area * kLiningRegionFraction;
        result.bounds.push_back(bucket.bounds);
        result.plan.units.push_back(std::move(unit));
    }
    result.plan.inputRegionCount = result.plan.units.size();
    result.sourceOutlines.resize(result.plan.units.size());
    if (isolateBackground && !result.labels.contains(-1)) {
        QVector<int> border(result.plan.units.size(), 0);
        for (int x = 0; x < width; ++x) {
            ++border[result.labels[x]];
            ++border[result.labels[(height - 1) * width + x]];
        }
        for (int y = 0; y < height; ++y) {
            ++border[result.labels[y * width]];
            ++border[result.labels[y * width + width - 1]];
        }
        const auto largest = std::max_element(border.cbegin(), border.cend());
        if (largest != border.cend()) {
            const int label = static_cast<int>(largest - border.cbegin());
            const int corners[] = {0, width - 1, (height - 1) * width, count - 1};
            const int matchingCorners = std::count_if(std::begin(corners), std::end(corners),
                [&](int pixel) { return result.labels[pixel] == label; });
            if (*largest >= (2 * width + 2 * height) * kBackgroundBorderFraction
                && matchingCorners >= kBackgroundCornerCount) {
                result.background = label;
                result.backgroundSurrounds = *largest == 2 * width + 2 * height;
                result.plan.units[label].background = true;
                result.plan.units[label].lining = false;
            }
        }
    }
    std::vector<std::uint8_t> foreground(static_cast<size_t>(count), 0);
    std::vector<std::uint8_t> empty(static_cast<size_t>(count), 0);
    for (int pixel = 0; pixel < count; ++pixel) {
        foreground[static_cast<size_t>(pixel)] = result.labels[pixel] >= 0
            && result.labels[pixel] != result.background;
        empty[static_cast<size_t>(pixel)] = result.labels[pixel] < 0;
    }
    result.silhouette = traceMaskToPath(foreground, width, height, image.rect(), tracingParameters());
    QVector<bool> outside(count, false);
    queue.clear();
    auto enqueueEmpty = [&](int pixel) {
        if (empty[static_cast<size_t>(pixel)] && !outside[pixel]) {
            outside[pixel] = true;
            queue.push_back(pixel);
        }
    };
    for (int x = 0; x < width; ++x) {
        enqueueEmpty(x);
        enqueueEmpty((height - 1) * width + x);
    }
    for (int y = 0; y < height; ++y) {
        enqueueEmpty(y * width);
        enqueueEmpty(y * width + width - 1);
    }
    for (int cursor = 0; cursor < queue.size(); ++cursor) {
        const int pixel = queue[cursor];
        for (const QPoint &offset : offsets) {
            const int nx = pixel % width + offset.x();
            const int ny = pixel / width + offset.y();
            if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                enqueueEmpty(ny * width + nx);
            }
        }
    }
    for (int pixel = 0; pixel < count; ++pixel) {
        empty[static_cast<size_t>(pixel)] &= !outside[pixel];
    }
    result.protectedEmptyPixels.resize(count);
    for (int pixel = 0; pixel < count; ++pixel) {
        result.protectedEmptyPixels[pixel] = empty[static_cast<size_t>(pixel)] != 0;
    }
    result.protectedEmpty = traceMaskToPath(empty, width, height, image.rect(), tracingParameters());
    for (int label = 0; label < result.plan.units.size(); ++label) {
        if (stopped(cancelled)) {
            result.plan.cancelled = true;
            return result;
        }
        if (progress) {
            progress(QStringLiteral("Tracing source regions"), label, result.plan.units.size());
        }
        RegionLayerUnit &unit = result.plan.units[label];
        if (unit.background) {
            const QRectF bounds(image.rect());
            const double padding = result.backgroundSurrounds ? 0.0 : kBackgroundPadding;
            unit.outline.addRect(bounds.adjusted(-padding, -padding, padding, padding));
        } else if (unit.area == result.bounds[label].width() * result.bounds[label].height()) {
            unit.outline.addRect(QRectF(result.bounds[label]));
        } else {
            unit.outline = pixelPath(result.labels, label, size, result.bounds[label]);
        }
        result.sourceOutlines[label] = unit.outline;
    }

    return result;
}

QPainterPath expanded(const QPainterPath &path, double width) {
    if (width <= 0.0) {
        return path;
    }
    QPainterPathStroker stroker;
    stroker.setWidth(2.0 * width);
    stroker.setJoinStyle(Qt::RoundJoin);
    stroker.setCapStyle(Qt::RoundCap);

    return path.united(stroker.createStroke(path));
}

const PenPrimitive *squarePrimitive(const QVector<PenPrimitive> &primitives) {
    for (const PenPrimitive &primitive : primitives) {
        if (primitive.shapeId == kSquareShapeId && !primitive.bounds.isEmpty()) {
            return &primitive;
        }
    }

    return nullptr;
}

PenPlacement rectanglePlacement(const QRectF &rectangle, const PenPrimitive &square) {
    const QRectF bounds = square.bounds;
    const double sx = rectangle.width() / bounds.width();
    const double sy = rectangle.height() / bounds.height();

    return {square.shapeId, QTransform(sx, 0.0, 0.0, sy,
        rectangle.left() - sx * bounds.left(), rectangle.top() - sy * bounds.top()),
        rectangle.width() * rectangle.height()};
}

QVector<QRect> coverPixelRuns(const QVector<int> &labels, const QSize &size,
                             const QSet<int> &owners,
                             const std::function<bool(int)> &needed,
                             const std::function<bool()> &cancelled) {
    QVector<QRect> rectangles;
    QMap<QPair<int, int>, int> previous;
    for (int y = 0; y < size.height(); ++y) {
        if (stopped(cancelled)) {
            return {};
        }
        QMap<QPair<int, int>, int> current;
        for (int x = 0; x < size.width();) {
            const int pixel = y * size.width() + x;
            if (!owners.contains(labels[pixel]) || !needed(pixel)) {
                ++x;
                continue;
            }
            const int start = x++;
            while (x < size.width() && owners.contains(labels[y * size.width() + x])
                   && needed(y * size.width() + x)) {
                ++x;
            }
            const QPair<int, int> key(start, x - start);
            const auto prior = previous.constFind(key);
            int index = rectangles.size();
            if (prior != previous.cend()) {
                index = *prior;
                rectangles[index].setHeight(rectangles[index].height() + 1);
            } else {
                rectangles.push_back(QRect(start, y, x - start, 1));
            }
            current.insert(key, index);
        }
        previous = std::move(current);
    }

    return rectangles;
}

double filledArea(const QPainterPath &path) {
    double area = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        double twiceArea = 0.0;
        for (int i = 0; i < polygon.size(); ++i) {
            const QPointF &a = polygon[i];
            const QPointF &b = polygon[(i + 1) % polygon.size()];
            twiceArea += a.x() * b.y() - b.x() * a.y();
        }
        area += std::abs(twiceArea) * 0.5;
    }

    return area;
}

bool crossesProtectedPixels(const QPainterPath &path, const QVector<bool> &protectedPixels,
                            const QSize &size) {
    const QRect bounds = path.boundingRect().toAlignedRect().intersected(QRect(QPoint(), size));
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            if (protectedPixels[y * size.width() + x] && path.contains(QPointF(x + 0.5, y + 0.5))) {
                return true;
            }
        }
    }

    return false;
}

void paintFill(QImage *image, const RegionFillLayer &fill,
               const QHash<int, QPainterPath> &silhouettes) {
    QPainter painter(image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill.color);
    for (const PenPlacement &placement : fill.placements) {
        painter.drawPath(placement.transform.map(silhouettes.value(placement.shapeId)));
    }
}

bool needsCoverage(const QImage &target, const QImage &rendered,
                   const QImage &foreground, int pixel, int boundaryRadius) {
    const int x = pixel % target.width();
    const int y = pixel / target.width();
    if (qAlpha(foreground.pixel(x, y)) == 0) {
        return true;
    }
    const QRgb color = target.pixel(x, y);
    if (rendered.pixel(x, y) == color) {
        return false;
    }
    for (int dy = -boundaryRadius; dy <= boundaryRadius; ++dy) {
        for (int dx = -boundaryRadius; dx <= boundaryRadius; ++dx) {
            if (!target.rect().contains(x + dx, y + dy)
                || target.pixel(x + dx, y + dy) != color) {
                return false;
            }
        }
    }

    return true;
}

int adjustImageCoverage(const ImageComponents &components, const QImage &target,
                        const QHash<int, QPainterPath> &silhouettes,
                        const QVector<QPainterPath> &laterCoverage,
                        const QPainterPath &envelope, double allowance,
                        QVector<RegionFillLayer> *fills, QImage *rendered, QImage *foreground,
                        const std::function<bool()> &cancelled) {
    if (allowance <= 0.0) {
        return 0;
    }
    QImage above(target.size(), QImage::Format_ARGB32);
    above.fill(Qt::transparent);
    QVector<bool> needed(components.labels.size(), false);
    const int boundaryRadius = static_cast<int>(std::ceil(allowance));
    for (int pixel = 0; pixel < needed.size(); ++pixel) {
        needed[pixel] = components.labels[pixel] >= 0
            && components.labels[pixel] != components.background
            && needsCoverage(target, *rendered, *foreground, pixel, boundaryRadius);
    }
    int adjusted = 0;
    for (int layerIndex = fills->size() - 1; layerIndex >= 0; --layerIndex) {
        RegionFillLayer &fill = (*fills)[layerIndex];
        const RegionLayerUnit &unit = components.plan.units[layerIndex];
        if (unit.background) {
            continue;
        }
        if (!unit.lining || unit.bottomLining) {
            const QSet<int> owners(unit.sourceRegionIndices.begin(), unit.sourceRegionIndices.end());
            const QPainterPath legal = expanded(unit.outline, allowance)
                .united(laterCoverage[layerIndex]).intersected(envelope);
            for (PenPlacement &placement : fill.placements) {
                if (stopped(cancelled)) {
                    return adjusted;
                }
                const QPainterPath shape = silhouettes.value(placement.shapeId);
                const QRectF bounds = placement.transform.map(shape).boundingRect();
                const QRect search = bounds.adjusted(-allowance, -allowance, allowance, allowance)
                    .toAlignedRect().intersected(target.rect());
                QVector<QPointF> missing;
                for (int y = search.top(); y <= search.bottom(); ++y) {
                    for (int x = search.left(); x <= search.right(); ++x) {
                        const int pixel = y * target.width() + x;
                        const bool owned = unit.bottomLining
                            ? components.labels[pixel] >= 0 && components.labels[pixel] != components.background
                            : owners.contains(components.labels[pixel]);
                        if (needed[pixel] && owned
                            && qAlpha(above.pixel(x, y)) == 0) {
                            missing.push_back(QPointF(x + 0.5, y + 0.5));
                        }
                    }
                }
                if (missing.isEmpty() || bounds.isEmpty()) {
                    continue;
                }
                QTransform bestTransform;
                QPainterPath bestPath;
                int bestGain = 0;
                double bestAreaScale = 1.0;
                for (const double step : kCoverageGrowthSteps) {
                    if (step > allowance) {
                        continue;
                    }
                    const double scaleX = 1.0 + 2.0 * step / bounds.width();
                    const double scaleY = 1.0 + 2.0 * step / bounds.height();
                    const QPointF center = bounds.center();
                    const QTransform growth(scaleX, 0.0, 0.0, scaleY,
                        center.x() * (1.0 - scaleX), center.y() * (1.0 - scaleY));
                    const QTransform transform = placement.transform * growth;
                    const QPainterPath candidate = transform.map(shape);
                    const int gain = std::count_if(missing.cbegin(), missing.cend(),
                        [&](const QPointF &point) { return candidate.contains(point); });
                    if (gain <= bestGain
                        || filledArea(candidate.subtracted(legal)) > kGeometricSpillAreaTolerance
                        || crossesProtectedPixels(candidate, components.protectedEmptyPixels, target.size())) {
                        continue;
                    }
                    bestGain = gain;
                    bestTransform = transform;
                    bestPath = candidate;
                    bestAreaScale = scaleX * scaleY;
                }
                if (bestGain > 0) {
                    placement.transform = bestTransform;
                    placement.area *= bestAreaScale;
                    ++adjusted;
                    for (const QPointF &point : missing) {
                        if (bestPath.contains(point)) {
                            needed[static_cast<int>(point.y()) * target.width()
                                + static_cast<int>(point.x())] = false;
                        }
                    }
                }
            }
        }
        paintFill(&above, fill, silhouettes);
    }
    if (adjusted > 0) {
        rendered->fill(Qt::transparent);
        foreground->fill(Qt::transparent);
        for (const RegionFillLayer &fill : *fills) {
            paintFill(rendered, fill, silhouettes);
            if (!fill.background) {
                paintFill(foreground, fill, silhouettes);
            }
        }
    }

    return adjusted;
}

int repairImageLayers(const ImageComponents &components, const QImage &target,
                      const QHash<int, QPainterPath> &silhouettes,
                      const QVector<catalog::Primitive> &primitives,
                      const QVector<QPainterPath> &laterCoverage,
                      const QPainterPath &envelope, double allowance,
                      QVector<RegionFillLayer> *fills, QImage *rendered, QImage *foreground,
                      const std::function<bool()> &cancelled) {
    QImage above(target.size(), QImage::Format_ARGB32);
    above.fill(Qt::transparent);
    const QVector<int> patchIds = catalog::shapeIdsForTask(primitives, catalog::ShapeTask::GapPatches);
    const int radius = static_cast<int>(std::ceil(std::max(0.0, allowance)));
    int repairs = 0;
    for (int layer = fills->size() - 1; layer >= 0; --layer) {
        const RegionLayerUnit &unit = components.plan.units[layer];
        RegionFillLayer &fill = (*fills)[layer];
        if (unit.background) {
            continue;
        }
        const QSet<int> owners(unit.sourceRegionIndices.cbegin(), unit.sourceRegionIndices.cend());
        const double meanWidth = unit.outline.length() > 0.0
            ? 2.0 * filledArea(unit.outline) / unit.outline.length() : 0.0;
        const double localAllowance = unit.lining && !unit.bottomLining
            ? std::min(allowance, meanWidth * (thin::kQualityMaximumThicknessRatio - 1.0) * 0.5)
            : allowance;
        const QPainterPath legal = expanded(unit.outline, localAllowance)
            .united(laterCoverage[layer]).intersected(envelope);
        QVector<QPointF> missing;
        for (int pixel = 0; pixel < components.labels.size(); ++pixel) {
            const int x = pixel % target.width();
            const int y = pixel / target.width();
            const bool owned = unit.bottomLining
                ? components.labels[pixel] >= 0 && components.labels[pixel] != components.background
                    && qAlpha(foreground->pixel(x, y)) == 0
                : owners.contains(components.labels[pixel]);
            if (owned && qAlpha(above.pixel(x, y)) == 0
                && needsCoverage(target, *rendered, *foreground, pixel, radius)) {
                missing.push_back(QPointF(x + 0.5, y + 0.5));
            }
        }
        while (!missing.isEmpty() && !stopped(cancelled)) {
            PenPlacement best;
            QPainterPath bestPath;
            int bestGain = 0;
            double bestArea = std::numeric_limits<double>::max();
            bool bestRounded = false;
            const QPointF seed = missing.front();
            for (const double neighborhood : kCoveragePatchRadii) {
                QRectF bounds(seed, QSizeF());
                for (const QPointF &point : missing) {
                    if (std::abs(point.x() - seed.x()) <= neighborhood
                        && std::abs(point.y() - seed.y()) <= neighborhood) {
                        bounds.setLeft(std::min(bounds.left(), point.x()));
                        bounds.setRight(std::max(bounds.right(), point.x()));
                        bounds.setTop(std::min(bounds.top(), point.y()));
                        bounds.setBottom(std::max(bounds.bottom(), point.y()));
                    }
                }
                bounds.adjust(-kCoveragePixelHalfSize, -kCoveragePixelHalfSize,
                    kCoveragePixelHalfSize, kCoveragePixelHalfSize);
                for (const int id : patchIds) {
                    const QPainterPath shape = silhouettes.value(id);
                    const QRectF sourceBounds = shape.boundingRect();
                    if (sourceBounds.isEmpty()) {
                        continue;
                    }
                    const bool rounded = id != kSquareShapeId;
                    const double padding = rounded ? kRoundedPatchScale : 1.0;
                    const double scaleX = bounds.width() * padding / sourceBounds.width();
                    const double scaleY = bounds.height() * padding / sourceBounds.height();
                    const QTransform transform(scaleX, 0.0, 0.0, scaleY,
                        bounds.center().x() - sourceBounds.center().x() * scaleX,
                        bounds.center().y() - sourceBounds.center().y() * scaleY);
                    const QPainterPath candidate = transform.map(shape);
                    if (!candidate.contains(seed)
                        || filledArea(candidate.subtracted(legal)) > kGeometricSpillAreaTolerance
                        || crossesProtectedPixels(candidate, components.protectedEmptyPixels, target.size())) {
                        continue;
                    }
                    const int gain = std::count_if(missing.cbegin(), missing.cend(),
                        [&](const QPointF &point) { return candidate.contains(point); });
                    const double area = filledArea(candidate);
                    if (gain > bestGain || (gain == bestGain
                        && (rounded > bestRounded || (rounded == bestRounded && area < bestArea)))) {
                        best = {id, transform, area};
                        bestPath = candidate;
                        bestGain = gain;
                        bestArea = area;
                        bestRounded = rounded;
                    }
                }
            }
            if (bestGain == 0) {
                missing.removeFirst();
                continue;
            }
            fill.placements.push_back(best);
            ++repairs;
            missing.erase(std::remove_if(missing.begin(), missing.end(),
                [&](const QPointF &point) { return bestPath.contains(point); }), missing.end());
        }
        paintFill(&above, fill, silhouettes);
    }
    if (repairs > 0) {
        rendered->fill(Qt::transparent);
        foreground->fill(Qt::transparent);
        for (const RegionFillLayer &fill : *fills) {
            paintFill(rendered, fill, silhouettes);
            if (!fill.background) {
                paintFill(foreground, fill, silhouettes);
            }
        }
    }

    return repairs;
}

} // namespace

int pruneImageComposite(QVector<RegionFillLayer> *fills,
                        const QHash<int, QPainterPath> &silhouettes,
                        QImage *rendered, QImage *foreground,
                        const std::function<bool()> &cancelled) {
    if (!fills || !rendered || !foreground || rendered->isNull() || foreground->isNull()) {
        return 0;
    }
    struct Shape {
        QPainterPath path;
        QColor color;
        int layer = 0;
        int placement = 0;
        bool background = false;
        bool keep = true;
    };
    QVector<Shape> shapes;
    QVector<QImage> prefixes;
    QRectF worldBounds;
    for (int layer = 0; layer < fills->size(); ++layer) {
        const RegionFillLayer &fill = (*fills)[layer];
        for (int index = 0; index < fill.placements.size(); ++index) {
            const PenPlacement &placement = fill.placements[index];
            QPainterPath path = placement.transform.map(silhouettes.value(placement.shapeId));
            if (path.isEmpty()) {
                return 0;
            }
            worldBounds = worldBounds.united(path.boundingRect());
            shapes.push_back({std::move(path), fill.color, layer, index, fill.background});
        }
    }
    const QRect bounds = worldBounds.toAlignedRect();
    const QSize size = bounds.size() * kCompositeSamplingScale;
    const qint64 samples = static_cast<qint64>(size.width()) * size.height();
    if (shapes.size() < 2 || size.isEmpty() || samples > kMaximumCompositeSamples) {
        return 0;
    }
    const int prefixCount = std::max<qint64>(1, kCompositePrefixBytes / (samples * sizeof(QRgb)));
    const int blockSize = (shapes.size() + prefixCount - 1) / prefixCount;
    const QTransform worldToSample(kCompositeSamplingScale, 0.0, 0.0, kCompositeSamplingScale,
        -bounds.left() * kCompositeSamplingScale, -bounds.top() * kCompositeSamplingScale);
    QImage reference(size, QImage::Format_ARGB32);
    reference.fill(Qt::transparent);
    for (int index = 0; index < shapes.size(); ++index) {
        if (stopped(cancelled)) {
            return 0;
        }
        if (index % blockSize == 0) {
            prefixes.push_back(reference.copy());
        }
        if (!shapes[index].background) {
            QPainter painter(&reference);
            painter.setTransform(worldToSample);
            painter.fillPath(shapes[index].path, shapes[index].color);
        }
    }
    QImage above(size, QImage::Format_ARGB32);
    above.fill(Qt::transparent);
    int removed = 0;
    for (int index = shapes.size() - 1; index >= 0; --index) {
        if (stopped(cancelled)) {
            return 0;
        }
        Shape &shape = shapes[index];
        if (shape.background) {
            continue;
        }
        const QRect rectangle = worldToSample.mapRect(shape.path.boundingRect())
            .toAlignedRect().adjusted(-1, -1, 1, 1).intersected(reference.rect());
        QImage trial = prefixes[index / blockSize].copy(rectangle);
        {
            QPainter painter(&trial);
            painter.setTransform(worldToSample * QTransform::fromTranslate(-rectangle.left(), -rectangle.top()));
            for (int lower = index / blockSize * blockSize; lower < index; ++lower) {
                if (!shapes[lower].background && shapes[lower].path.boundingRect().intersects(shape.path.boundingRect())) {
                    painter.fillPath(shapes[lower].path, shapes[lower].color);
                }
            }
            painter.resetTransform();
            painter.drawImage(QPoint(-rectangle.left(), -rectangle.top()), above);
        }
        bool equal = !trial.isNull();
        for (int y = 0; y < trial.height() && equal; ++y) {
            const QRgb *actual = reinterpret_cast<const QRgb *>(trial.constScanLine(y));
            const QRgb *expected = reinterpret_cast<const QRgb *>(reference.constScanLine(rectangle.top() + y))
                + rectangle.left();
            equal = std::equal(actual, actual + trial.width(), expected);
        }
        if (equal) {
            shape.keep = false;
            ++removed;
        } else {
            QPainter painter(&above);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
            painter.setTransform(worldToSample);
            painter.fillPath(shape.path, shape.color);
        }
    }
    if (removed == 0) {
        return 0;
    }
    const QVector<RegionFillLayer> baseline = *fills;
    const QImage baselineRendered = *rendered;
    const QImage baselineForeground = *foreground;
    for (RegionFillLayer &fill : *fills) {
        fill.placements.clear();
    }
    for (const Shape &shape : shapes) {
        if (shape.keep) {
            (*fills)[shape.layer].placements.push_back(baseline[shape.layer].placements[shape.placement]);
        }
    }
    rendered->fill(Qt::transparent);
    foreground->fill(Qt::transparent);
    for (const RegionFillLayer &fill : *fills) {
        paintFill(rendered, fill, silhouettes);
        if (!fill.background) {
            paintFill(foreground, fill, silhouettes);
        }
    }
    if (*rendered != baselineRendered || *foreground != baselineForeground) {
        *fills = baseline;
        *rendered = baselineRendered;
        *foreground = baselineForeground;
        return 0;
    }

    return removed;
}

ImageGeneratorResult generateImage(const ImageGeneratorRequest &request,
                                   const ImageGeneratorProgress &progress,
                                   const std::function<bool()> &cancelled) {
    ImageGeneratorResult result;
    QElapsedTimer elapsed;
    QJsonArray fitDiagnostics;
    QVector<QColor> palette;
    QImage preparedPixels;
    QHash<int, QPainterPath> silhouettes;
    QImage image = request.source.convertToFormat(QImage::Format_ARGB32);
    const ImageGeneratorOptions &options = request.options;
    const PenPrimitive *square = squarePrimitive(request.primitives);
    elapsed.start();
    if (image.isNull() || square == nullptr || request.compactPrimitives.isEmpty()
        || (options.detectLining && request.liningPrimitives.isEmpty())) {
        result.error = QStringLiteral("Image Generator needs a source image and native shape catalogs");
        return result;
    }
    auto phase = [&](const QString &name, int done = 0, int total = 0) {
        if (progress) {
            progress(name, done, total);
        }
    };
    if (stopped(cancelled)) {
        result.cancelled = true;
        return result;
    }
    phase(QStringLiteral("Preparing palette"));
    if (options.reducePalette) {
        ImagePreprocessSettings settings;
        settings.colors = std::clamp(options.colors, 2, 256);
        if (!options.cleanRasterNoise) {
            settings.smoothingPasses = 0;
            settings.flattenStrength = 0.0;
            settings.detailRestore = 0.0;
            settings.saturationRestore = 0.0;
            settings.speckleSize = 0;
            settings.edgeCleanupPasses = 0;
        }
        settings.minimumColorFraction = 0.0;
        settings.lineMode = false;
        const auto prepared = preprocessImageDetailed(request.source, settings);
        palette = prepared.retainedPalette;
        if (options.cleanRasterNoise) {
            preparedPixels = prepared.image.convertToFormat(QImage::Format_ARGB32);
        }
        if (prepared.image.isNull()) {
            result.error = QStringLiteral("Image palette preparation failed");
            return result;
        }
    }
    if (image.isNull()) {
        result.error = QStringLiteral("Image palette preparation failed");
        return result;
    }
    for (int y = 0; y < image.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) > 0) {
                row[x] |= 0xff000000u;
            }
        }
    }
    const qint64 preparationMilliseconds = elapsed.elapsed();
    LiningExtractionResult lining;
    if (options.detectLining && !stopped(cancelled)) {
        phase(QStringLiteral("Detecting lining"));
        lining = extractLining(request.source, {}, cancelled, progress);
    }
    if (stopped(cancelled) || lining.cancelled) {
        result.cancelled = true;
        return result;
    }
    ImageComponents components = extractImageComponents(image, lining.pixels,
        options.isolateBackground, options.bucketTolerance, palette, preparedPixels,
        options.fragmentArea, options.fragmentTolerance, progress, cancelled);
    if (!components.error.isEmpty()) {
        result.error = components.error;
        return result;
    }
    if (components.plan.cancelled || stopped(cancelled)) {
        result.cancelled = true;
        return result;
    }
    if (components.plan.units.isEmpty()) {
        result.error = QStringLiteral("The selected source image has no visible pixels");
        return result;
    }
    const QImage preparedSource = image;
    int bucketColorChangedPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int label = components.labels[y * image.width() + x];
            if (label >= 0) {
                const QRgb color = components.plan.units[label].color.rgba();
                bucketColorChangedPixels += row[x] != color;
                row[x] = color;
            }
        }
    }
    RegionLayerUnit background;
    const bool hasBackground = components.background >= 0;
    if (hasBackground) {
        background = components.plan.units.takeAt(components.background);
    }
    QPainterPath exteriorBand;
    const double extension = std::clamp(options.outlineExtension, 0.0, kMaximumImageGeneratorExtension);
    if (extension > 0.0) {
        exteriorBand = expanded(components.silhouette, extension)
            .subtracted(components.silhouette).subtracted(components.protectedEmpty);
        for (RegionLayerUnit &unit : components.plan.units) {
            if (unit.lining) {
                unit.outline = unit.outline.united(
                    expanded(unit.outline, extension).intersected(exteriorBand));
            }
        }
    }
    std::stable_sort(components.plan.units.begin(), components.plan.units.end(),
        [&](const RegionLayerUnit &left, const RegionLayerUnit &right) {
            if (left.lining != right.lining && options.liningMode != ImageLiningMode::Mixed) {
                return options.liningMode == ImageLiningMode::Bottom ? left.lining : !left.lining;
            }
            return estimateRegionShapeCount(left.outline) > estimateRegionShapeCount(right.outline);
        });
    if (options.optimizeTopology) {
        planImageRegionTopology(components.labels, image.size(), components.background,
            options, &components.plan, progress, cancelled);
    }
    if (components.plan.cancelled || stopped(cancelled)) {
        result.cancelled = true;
        return result;
    }
    if (hasBackground) {
        components.plan.units.prepend(std::move(background));
    }
    const qint64 planningMilliseconds = elapsed.elapsed();
    for (const PenPrimitive &primitive : request.primitives) {
        silhouettes.insert(primitive.shapeId, primitive.silhouette);
    }
    for (const catalog::Primitive &primitive : request.compactPrimitives) {
        silhouettes.insert(primitive.shape.shapeId, primitive.shape.silhouette);
    }
    for (const catalog::Primitive &primitive : request.liningPrimitives) {
        silhouettes.insert(primitive.shape.shapeId, primitive.shape.silhouette);
    }
    QVector<QPainterPath> laterCoverage(components.plan.units.size());
    QPainterPath suffix;
    for (int i = components.plan.units.size() - 1; i >= 0; --i) {
        laterCoverage[i] = suffix;
        suffix = suffix.united(components.plan.units[i].outline);
    }
    QImage rendered(image.size(), QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QImage foregroundRendered = rendered.copy();
    int fallbackRegions = 0;
    int fallbackShapes = 0;
    int failedFits = 0;
    int rejectedPlacements = 0;
    const QPainterPath foregroundEnvelope = expanded(components.silhouette,
        std::max(extension, options.boundaryAllowance)).subtracted(components.protectedEmpty);
    for (int i = 0; i < components.plan.units.size(); ++i) {
        if (stopped(cancelled)) {
            result.cancelled = true;
            result.fills.clear();
            return result;
        }
        const RegionLayerUnit &unit = components.plan.units[i];
        phase(QStringLiteral("Fitting region %1 of %2").arg(i + 1).arg(components.plan.units.size()),
              i, components.plan.units.size());
        QElapsedTimer regionClock;
        regionClock.start();
        PenFillResult fill = fitSingleRegionPrimitive(unit.outline, request.primitives);
        QJsonObject diagnostics;
        const bool thin = unit.lining && !unit.bottomLining && options.liningMode != ImageLiningMode::Bottom
            && options.boundaryAllowance > 0.0;
        QPainterPath sourceOutline;
        if (thin) {
            for (const int source : unit.sourceRegionIndices) {
                sourceOutline = sourceOutline.united(components.sourceOutlines[source]);
            }
        }
        const QPainterPath required = (thin ? sourceOutline : unit.outline).subtracted(laterCoverage[i]);
        if (fill.placements.isEmpty() && !required.isEmpty()) {
            RegionPenLoopConversionOptions conversion;
            conversion.preserveInputCurves = true;
            const auto loops = regionOutlineToPenLoops(thin ? sourceOutline : unit.outline, conversion);
            if (loops.valid()) {
                PenFillRequest contour;
                contour.loops = loops.loops;
                contour.primitives = request.primitives;
                contour.boundaryTolerance = kContourGeometryTolerance;
                catalog::FillResult fitted;
                if (thin) {
                    thin::FillOptions settings;
                    settings.leeway = laterCoverage[i].united(unit.outline.subtracted(sourceOutline))
                        .toFillPolygons().toVector();
                    settings.protectedEmpty = components.protectedEmpty.toFillPolygons().toVector();
                    settings.thicknessReference = sourceOutline.toFillPolygons().toVector();
                    settings.maximumThicknessRatio = thin::kQualityMaximumThicknessRatio;
                    settings.boundaryAllowance = options.boundaryAllowance;
                    settings.qualityTimeBudgetMilliseconds = std::max(1, options.liningSeconds) * 1000;
                    settings.qualityEvaluationBudget = std::max(1, options.evaluationBudget);
                    settings.useGpu = options.useGpu;
                    fitted = thin::fillRegion(contour, request.liningPrimitives, settings, cancelled);
                } else {
                    compact::FillOptions settings;
                    settings.leeway = laterCoverage[i].subtracted(unit.outline)
                        .toFillPolygons().toVector();
                    settings.boundaryAllowance = options.boundaryAllowance;
                    settings.evaluationBudget = std::max(1, options.evaluationBudget);
                    settings.useGpu = options.useGpu;
                    settings.retainFailedFill = true;
                    fitted = profile::fillRegion(contour, request.compactPrimitives, settings, cancelled);
                }
                fill = std::move(fitted.fill);
                diagnostics = std::move(fitted.diagnostics);
            } else {
                fill.error = loops.error;
            }
        }
        if (stopped(cancelled) || fill.cancelled) {
            result.cancelled = true;
            result.fills.clear();
            return result;
        }
        const QPainterPath legal = expanded(unit.outline, options.boundaryAllowance)
            .united(laterCoverage[i]).intersected(foregroundEnvelope);
        QVector<PenPlacement> accepted;
        for (const PenPlacement &placement : fill.placements) {
            if (stopped(cancelled)) {
                result.cancelled = true;
                result.fills.clear();
                return result;
            }
            const QPainterPath path = placement.transform.map(silhouettes.value(placement.shapeId));
            if (!unit.background && (path.isEmpty()
                || filledArea(path.subtracted(legal)) > kGeometricSpillAreaTolerance
                || crossesProtectedPixels(path, components.protectedEmptyPixels, image.size()))) {
                ++rejectedPlacements;
            } else {
                accepted.push_back(placement);
            }
        }
        fill.placements = std::move(accepted);
        if (fill.placements.isEmpty() && !required.isEmpty()) {
            diagnostics.insert(QStringLiteral("fallbackReason"), fill.error);
            QSet<int> owners(unit.sourceRegionIndices.begin(), unit.sourceRegionIndices.end());
            const auto rectangles = coverPixelRuns(components.labels, image.size(), owners,
                [](int) { return true; }, cancelled);
            fill.placements.clear();
            for (const QRect &rectangle : rectangles) {
                fill.placements.push_back(rectanglePlacement(rectangle, *square));
            }
            ++fallbackRegions;
            fallbackShapes += fill.placements.size();
        }
        failedFits += !fill.error.isEmpty();
        RegionFillLayer layer;
        layer.color = unit.color;
        layer.background = unit.background;
        layer.drawOrder = i;
        layer.placements = std::move(fill.placements);
        layer.area = unit.area;
        result.fills.push_back(std::move(layer));
        result.groupNames.push_back(unit.background ? QStringLiteral("Background")
            : QStringLiteral("%1 %2").arg(unit.bottomLining ? QStringLiteral("Bottom lining")
                : unit.lining ? QStringLiteral("Top lining") : QStringLiteral("Color"))
                .arg(unit.color.name()));
        paintFill(&rendered, result.fills.back(), silhouettes);
        if (!unit.background) {
            paintFill(&foregroundRendered, result.fills.back(), silhouettes);
        }
        diagnostics.insert(QStringLiteral("shapes"), result.fills.back().placements.size());
        diagnostics.insert(QStringLiteral("fittedShapes"), result.fills.back().placements.size());
        diagnostics.insert(QStringLiteral("runtimeMs"), regionClock.elapsed());
        diagnostics.insert(QStringLiteral("estimatedShapes"), estimateRegionShapeCount(unit.outline));
        diagnostics.insert(QStringLiteral("lining"), unit.lining);
        diagnostics.insert(QStringLiteral("bottomLining"), unit.bottomLining);
        diagnostics.insert(QStringLiteral("fitter"), thin ? QStringLiteral("thin") : QStringLiteral("compact"));
        diagnostics.insert(QStringLiteral("color"), unit.color.name());
        diagnostics.insert(QStringLiteral("contourElements"), unit.outline.elementCount());
        diagnostics.insert(QStringLiteral("contourParts"), unit.outline.toSubpathPolygons().size());
        int contourLines = 0;
        int contourCurves = 0;
        for (int element = 0; element < unit.outline.elementCount(); ++element) {
            const auto type = unit.outline.elementAt(element).type;
            contourLines += type == QPainterPath::LineToElement;
            contourCurves += type == QPainterPath::CurveToElement;
        }
        diagnostics.insert(QStringLiteral("contourLines"), contourLines);
        diagnostics.insert(QStringLiteral("contourCurves"), contourCurves);
        diagnostics.insert(QStringLiteral("fitError"), fill.error);
        fitDiagnostics.push_back(diagnostics);
    }
    phase(QStringLiteral("Checking source coverage"));
    const int adjustedPlacements = adjustImageCoverage(components, image, silhouettes,
        laterCoverage, foregroundEnvelope, options.boundaryAllowance,
        &result.fills, &rendered, &foregroundRendered, cancelled);
    const int nativeRepairs = repairImageLayers(components, image, silhouettes,
        request.compactPrimitives, laterCoverage, foregroundEnvelope, options.boundaryAllowance,
        &result.fills, &rendered, &foregroundRendered, cancelled);
    QMap<QRgb, QSet<int>> colorOwners;
    for (int pixel = 0; pixel < components.labels.size(); ++pixel) {
        const int label = components.labels[pixel];
        if (label >= 0 && label != components.background) {
            colorOwners[image.pixel(pixel % image.width(), pixel / image.width())].insert(label);
        }
    }
    int repairs = 0;
    int uncovered = 0;
    for (auto it = colorOwners.cbegin(); it != colorOwners.cend(); ++it) {
        const auto rectangles = coverPixelRuns(components.labels, image.size(), it.value(),
            [&](int pixel) {
                const int x = pixel % image.width();
                const int y = pixel / image.width();
                const bool missing = qAlpha(foregroundRendered.pixel(x, y)) == 0;
                const int radius = static_cast<int>(std::ceil(std::max(0.0, options.boundaryAllowance)));
                uncovered += missing ? 1 : 0;

                return needsCoverage(image, rendered, foregroundRendered, pixel, radius);
            }, cancelled);
        if (rectangles.isEmpty()) {
            continue;
        }
        RegionFillLayer repair;
        repair.color = QColor::fromRgba(it.key());
        repair.drawOrder = result.fills.size();
        for (const QRect &rectangle : rectangles) {
            repair.placements.push_back(rectanglePlacement(rectangle, *square));
        }
        repairs += rectangles.size();
        result.fills.push_back(std::move(repair));
        result.groupNames.push_back(QStringLiteral("Coverage %1").arg(QColor::fromRgba(it.key()).name()));
        paintFill(&rendered, result.fills.back(), silhouettes);
        paintFill(&foregroundRendered, result.fills.back(), silhouettes);
    }
    if (stopped(cancelled)) {
        result.cancelled = true;
        result.fills.clear();
        return result;
    }
    QJsonObject shapeCounts;
    QVector<int> beforePruningCounts;
    for (const RegionFillLayer &fill : result.fills) {
        beforePruningCounts.push_back(fill.placements.size());
    }
    int remainingRepairs = 0;
    phase(QStringLiteral("Removing hidden placements"));
    const int prunedShapes = pruneImageComposite(&result.fills, silhouettes,
        &rendered, &foregroundRendered, cancelled);
    if (stopped(cancelled)) {
        result.cancelled = true;
        result.fills.clear();
        return result;
    }
    for (int index = 0; index < fitDiagnostics.size(); ++index) {
        QJsonObject diagnostics = fitDiagnostics[index].toObject();
        diagnostics.insert(QStringLiteral("preCompositeShapes"), beforePruningCounts[index]);
        diagnostics.insert(QStringLiteral("nativeRepairShapes"), beforePruningCounts[index]
            - diagnostics.value(QStringLiteral("fittedShapes")).toInt());
        diagnostics.insert(QStringLiteral("shapes"), result.fills[index].placements.size());
        fitDiagnostics[index] = diagnostics;
    }
    for (int index = fitDiagnostics.size(); index < result.fills.size(); ++index) {
        remainingRepairs += result.fills[index].placements.size();
    }
    int shapes = 0;
    int remaining = 0;
    int mismatched = 0;
    int sourceMismatched = 0;
    int protectedFilled = 0;
    for (const RegionFillLayer &fill : result.fills) {
        shapes += fill.placements.size();
        for (const PenPlacement &placement : fill.placements) {
            const QString key = QString::number(placement.shapeId);
            shapeCounts.insert(key, shapeCounts.value(key).toInt() + 1);
        }
    }
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int pixel = y * image.width() + x;
            const bool foreground = components.labels[pixel] >= 0
                && components.labels[pixel] != components.background;
            remaining += foreground && qAlpha(foregroundRendered.pixel(x, y)) == 0;
            mismatched += qAlpha(image.pixel(x, y)) > 0 && image.pixel(x, y) != rendered.pixel(x, y);
            sourceMismatched += qAlpha(preparedSource.pixel(x, y)) > 0
                && preparedSource.pixel(x, y) != rendered.pixel(x, y);
            protectedFilled += components.protectedEmptyPixels[pixel] && qAlpha(rendered.pixel(x, y)) > 0;
        }
    }
    result.diagnostics = {
        {QStringLiteral("runtimeMs"), elapsed.elapsed()},
        {QStringLiteral("preparationMs"), preparationMilliseconds},
        {QStringLiteral("planningMs"), planningMilliseconds - preparationMilliseconds},
        {QStringLiteral("sourceRegions"), components.plan.inputRegionCount},
        {QStringLiteral("bucketRegionsBeforePalette"), components.bucketRegionsBeforePalette},
        {QStringLiteral("mergedFragmentCount"), components.mergedFragmentCount},
        {QStringLiteral("fragmentArea"), options.fragmentArea},
        {QStringLiteral("fragmentTolerance"), options.fragmentTolerance},
        {QStringLiteral("plannedRegions"), components.plan.units.size()},
        {QStringLiteral("shapes"), shapes},
        {QStringLiteral("shapeCounts"), shapeCounts},
        {QStringLiteral("coverageRepairShapes"), remainingRepairs},
        {QStringLiteral("coverageRepairShapesBeforePruning"), repairs},
        {QStringLiteral("coverageAdjustedPlacements"), adjustedPlacements},
        {QStringLiteral("nativeCoverageRepairShapes"), nativeRepairs},
        {QStringLiteral("compositePrunedShapes"), prunedShapes},
        {QStringLiteral("uncoveredPixelsBeforeRepair"), uncovered},
        {QStringLiteral("uncoveredPixels"), remaining},
        {QStringLiteral("sourceColorMismatchPixels"), sourceMismatched},
        {QStringLiteral("bucketTargetColorMismatchPixels"), mismatched},
        {QStringLiteral("bucketColorChangedPixels"), bucketColorChangedPixels},
        {QStringLiteral("bucketTolerance"), options.bucketTolerance},
        {QStringLiteral("regionExtraction"), QStringLiteral("Source Bucket flood, region palette merging and cubic mask contours")},
        {QStringLiteral("protectedTransparencyFilledPixels"), protectedFilled},
        {QStringLiteral("rasterFallbackRegions"), fallbackRegions},
        {QStringLiteral("rasterFallbackShapes"), fallbackShapes},
        {QStringLiteral("failedFitRegions"), failedFits},
        {QStringLiteral("rejectedPlacements"), rejectedPlacements},
        {QStringLiteral("topology"), QJsonArray::fromStringList(components.plan.diagnostics)},
        {QStringLiteral("fits"), fitDiagnostics},
        {QStringLiteral("liningDetection"), lining.diagnostics},
        {QStringLiteral("liningMode"), static_cast<int>(options.liningMode)},
        {QStringLiteral("paletteReduction"), options.reducePalette},
        {QStringLiteral("cleanRasterNoise"), options.reducePalette && options.cleanRasterNoise},
        {QStringLiteral("colors"), options.colors},
        {QStringLiteral("useGpu"), options.useGpu},
        {QStringLiteral("evaluationBudget"), options.evaluationBudget},
        {QStringLiteral("liningSeconds"), options.liningSeconds},
        {QStringLiteral("isolateBackground"), options.isolateBackground},
        {QStringLiteral("optimizeTopology"), options.optimizeTopology},
        {QStringLiteral("detectLining"), options.detectLining},
        {QStringLiteral("boundaryAllowance"), options.boundaryAllowance},
        {QStringLiteral("outlineExtension"), extension}
    };
    if (remaining > 0 || protectedFilled > 0) {
        result.error = QStringLiteral("Image Generator could not preserve source coverage and transparency");
        result.fills.clear();
    }

    return result;
}

QVector<GeneratedRegionVariant> imageGeneratorWorldVariants(
    const ImageGeneratorResult &result, const QTransform &imageToWorld) {
    GeneratedRegionVariant variant;
    variant.name = QStringLiteral("Generated");
    for (int i = 0; i < result.fills.size(); ++i) {
        const RegionFillLayer &fill = result.fills[i];
        GeneratedRegionGroup group;
        group.name = result.groupNames.value(i);
        const std::array<std::uint8_t, 4> color = {
            static_cast<std::uint8_t>(fill.color.blue()),
            static_cast<std::uint8_t>(fill.color.green()),
            static_cast<std::uint8_t>(fill.color.red()), 255};
        for (const PenPlacement &placement : fill.placements) {
            group.shapes.push_back({placement.shapeId, placement.transform * imageToWorld, color});
        }
        if (!group.shapes.isEmpty()) {
            variant.regions.push_back(std::move(group));
        }
    }
    if (variant.regions.isEmpty()) {
        return {};
    }

    return {variant};
}

} // namespace gui
