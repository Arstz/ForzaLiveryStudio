#include "image_region_plan.h"

#include "cubic_contour.h"
#include "region_shape_cost.h"

#include <algorithm>
#include <array>

namespace gui {
namespace {

constexpr double kContourSmoothing = 1.0;
constexpr double kMinimumUnderpaintGrowth = 1.5;
constexpr std::array<int, 4> kClosingRadii{1, 2, 4, 8};

using Mask = std::vector<std::uint8_t>;

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

Mask rasterMask(const QPainterPath &path, const QSize &size) {
    QImage image(size, QImage::Format_Grayscale8);
    image.fill(0);
    {
        QPainter painter(&image);
        painter.fillPath(path, Qt::white);
    }
    Mask mask(static_cast<size_t>(size.width()) * size.height());
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            mask[static_cast<size_t>(y) * size.width() + x] = image.constScanLine(y)[x] != 0;
        }
    }

    return mask;
}

bool preservesOwnership(const Mask &candidate, const Mask &required, const Mask &allowed) {
    for (size_t pixel = 0; pixel < candidate.size(); ++pixel) {
        if ((required[pixel] && !candidate[pixel]) || (candidate[pixel] && !allowed[pixel])) {
            return false;
        }
    }

    return true;
}

Mask fillHiddenHoles(const Mask &mask, const Mask &allowed, const QSize &size) {
    Mask filled = mask;
    Mask visited(mask.size(), 0);
    QVector<int> pending;
    const int width = size.width();
    const int height = size.height();
    for (int first = 0; first < static_cast<int>(mask.size()); ++first) {
        if (mask[first] || visited[first]) {
            continue;
        }
        pending = {first};
        visited[first] = 1;
        bool enclosed = true;
        bool hidden = true;
        for (int cursor = 0; cursor < pending.size(); ++cursor) {
            const int pixel = pending[cursor];
            const int x = pixel % width;
            const int y = pixel / width;
            enclosed &= x > 0 && y > 0 && x + 1 < width && y + 1 < height;
            hidden &= allowed[pixel] != 0;
            const int neighbors[] = {x ? pixel - 1 : -1, x + 1 < width ? pixel + 1 : -1,
                                     y ? pixel - width : -1, y + 1 < height ? pixel + width : -1};
            for (const int neighbor : neighbors) {
                if (neighbor >= 0 && !mask[neighbor] && !visited[neighbor]) {
                    visited[neighbor] = 1;
                    pending.push_back(neighbor);
                }
            }
        }
        if (enclosed && hidden) {
            for (const int pixel : pending) {
                filled[pixel] = 1;
            }
        }
    }

    return filled;
}

Mask closeMask(const Mask &mask, const QSize &size, int radius) {
    QPainterPathStroker stroker;
    const QPainterPath path = imageMaskContour(mask, size, 0.0);
    stroker.setWidth(2.0 * radius);
    stroker.setJoinStyle(Qt::RoundJoin);
    const Mask grown = rasterMask(path.united(stroker.createStroke(path)), size);
    Mask closed(mask.size(), 0);
    const int width = size.width();
    const int height = size.height();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool inside = true;
            for (int dy = -radius; dy <= radius && inside; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx * dx + dy * dy > radius * radius) {
                        continue;
                    }
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height
                        || !grown[ny * width + nx]) {
                        inside = false;
                        break;
                    }
                }
            }
            const int pixel = y * width + x;
            closed[pixel] = inside || mask[pixel];
        }
    }

    return closed;
}

QPolygonF convexHull(QPolygonF points) {
    std::sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) {
        return points;
    }
    auto cross = [](const QPointF &a, const QPointF &b, const QPointF &c) {
        return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
    };
    QPolygonF hull;
    for (const QPointF &point : points) {
        while (hull.size() > 1 && cross(hull[hull.size() - 2], hull.back(), point) <= 0.0) {
            hull.removeLast();
        }
        hull.push_back(point);
    }
    const int lower = hull.size();
    for (int i = points.size() - 2; i >= 0; --i) {
        while (hull.size() > lower && cross(hull[hull.size() - 2], hull.back(), points[i]) <= 0.0) {
            hull.removeLast();
        }
        hull.push_back(points[i]);
    }
    hull.removeLast();

    return hull;
}

Mask ownershipMask(const QVector<int> &labels, const QVector<int> &sources) {
    const QSet<int> owners(sources.cbegin(), sources.cend());
    Mask mask(labels.size(), 0);
    for (int pixel = 0; pixel < labels.size(); ++pixel) {
        mask[pixel] = owners.contains(labels[pixel]);
    }

    return mask;
}

QVector<QVector<int>> connectedPixels(const Mask &mask, const QSize &size) {
    Mask visited(mask.size(), 0);
    QVector<QVector<int>> components;
    QVector<int> pending;
    const int width = size.width();
    const int height = size.height();
    for (int first = 0; first < static_cast<int>(mask.size()); ++first) {
        if (!mask[first] || visited[first]) {
            continue;
        }
        pending = {first};
        visited[first] = 1;
        for (int cursor = 0; cursor < pending.size(); ++cursor) {
            const int pixel = pending[cursor];
            const int x = pixel % width;
            const int y = pixel / width;
            const int neighbors[] = {x ? pixel - 1 : -1, x + 1 < width ? pixel + 1 : -1,
                                     y ? pixel - width : -1, y + 1 < height ? pixel + width : -1};
            for (const int neighbor : neighbors) {
                if (neighbor >= 0 && mask[neighbor] && !visited[neighbor]) {
                    visited[neighbor] = 1;
                    pending.push_back(neighbor);
                }
            }
        }
        components.push_back(pending);
    }

    return components;
}

void buildBottomLining(const QVector<int> &labels, const QSize &size, int background,
                       double smoothing, RegionLayerPlan *plan) {
    QHash<QRgb, int> areas;
    for (const RegionLayerUnit &unit : plan->units) {
        if (unit.lining) {
            areas[unit.color.rgba()] += static_cast<int>(unit.area);
        }
    }
    if (areas.isEmpty()) {
        return;
    }
    QRgb color = 0;
    int largest = 0;
    for (auto it = areas.cbegin(); it != areas.cend(); ++it) {
        if (it.value() > largest || (it.value() == largest && it.key() < color)) {
            largest = it.value();
            color = it.key();
        }
    }
    QSet<int> colorSources;
    for (const RegionLayerUnit &unit : plan->units) {
        if (unit.color.rgba() == color) {
            for (const int source : unit.sourceRegionIndices) {
                colorSources.insert(source);
            }
        }
    }
    Mask foreground(labels.size(), 0);
    for (int pixel = 0; pixel < labels.size(); ++pixel) {
        foreground[pixel] = labels[pixel] >= 0 && labels[pixel] != background;
    }
    QVector<RegionLayerUnit> underpaints;
    QSet<int> absorbed;
    for (const QVector<int> &pixels : connectedPixels(foreground, size)) {
        Mask component(labels.size(), 0);
        QSet<int> sources;
        int sourceArea = 0;
        for (const int pixel : pixels) {
            component[pixel] = 1;
            if (colorSources.contains(labels[pixel])) {
                sources.insert(labels[pixel]);
                ++sourceArea;
            }
        }
        RegionLayerUnit underpaint;
        underpaint.color = QColor::fromRgba(color);
        underpaint.lining = true;
        underpaint.bottomLining = true;
        underpaint.area = pixels.size();
        if (sourceArea == 0 || underpaint.area < sourceArea * kMinimumUnderpaintGrowth) {
            continue;
        }
        underpaint.sourceRegionIndices = sources.values();
        std::sort(underpaint.sourceRegionIndices.begin(), underpaint.sourceRegionIndices.end());
        underpaint.outline = imageMaskContour(component, size, smoothing);
        if (underpaint.outline.isEmpty()) {
            continue;
        }
        absorbed.unite(sources);
        underpaints.push_back(std::move(underpaint));
    }
    if (underpaints.isEmpty()) {
        return;
    }
    QVector<RegionLayerUnit> upper;
    for (const RegionLayerUnit &unit : plan->units) {
        if (!absorbed.contains(unit.sourceRegionIndices.front())) {
            upper.push_back(unit);
        }
    }
    const int merged = plan->units.size() - upper.size() - underpaints.size();
    plan->units = std::move(upper);
    for (auto it = underpaints.crbegin(); it != underpaints.crend(); ++it) {
        plan->units.prepend(*it);
    }
    plan->topologyMergeCount += merged;
    ++plan->topologySimplificationCount;
    plan->diagnostics << QStringLiteral("bottom_lining source_components=%1 full_foreground=yes fit=compact")
        .arg(merged + underpaints.size());
}

} // namespace

QPainterPath imageMaskContour(const std::vector<std::uint8_t> &mask, const QSize &size,
                              double smoothing) {
    const auto polygons = pixelBoundaryLoops(mask, size, QRect(QPoint(), size));
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    CubicFitOptions options;
    options.tolerance = std::min(kContourSmoothing, smoothing);
    options.adaptToRasterNoise = false;
    for (const QPolygonF &polygon : polygons) {
        QVector<PenPoint> points;
        if (smoothing > 0.0) {
            points = fitCubicContour(polygon, options);
        }
        if (points.isEmpty() || !buildPenContour(points).valid()) {
            points.clear();
            for (const QPointF &point : polygon) {
                points.push_back({point, PenPointKind::Hard});
            }
        }
        path.addPath(penPath(points));
    }
    if (smoothing > 0.0 && cubicPathLoops(path).isEmpty()) {
        options.tolerance = std::min(kContourSmoothing, smoothing);
        const auto loops = fitMaskContours(mask, size, QRect(QPoint(), size), options);
        if (!loops.isEmpty()) {
            path = QPainterPath();
            path.setFillRule(Qt::OddEvenFill);
            for (const PenLoop &loop : loops) {
                path.addPath(penPath(loop.points));
            }
        }
    }

    return path;
}

void planImageRegionTopology(const QVector<int> &labels, const QSize &size,
                             int background, const ImageGeneratorOptions &options,
                             RegionLayerPlan *plan, const ImageGeneratorProgress &progress,
                             const std::function<bool()> &cancelled) {
    const double smoothing = std::min(kContourSmoothing, std::max(0.0, options.boundaryAllowance));
    if (options.liningMode != ImageLiningMode::Top) {
        buildBottomLining(labels, size, background, smoothing, plan);
    }
    Mask later(labels.size(), 0);
    for (int lower = plan->units.size() - 1; lower >= 0; --lower) {
        if (stopped(cancelled)) {
            plan->cancelled = true;
            return;
        }
        if (progress) {
            progress(QStringLiteral("Simplifying owned region topology"), plan->units.size() - lower,
                     plan->units.size());
        }
        RegionLayerUnit unit = plan->units[lower];
        if (unit.bottomLining) {
            continue;
        }
        Mask owned = ownershipMask(labels, unit.sourceRegionIndices);
        Mask allowed = owned;
        for (size_t pixel = 0; pixel < allowed.size(); ++pixel) {
            allowed[pixel] |= later[pixel];
        }
        QPainterPath best = imageMaskContour(owned, size, smoothing);
        QVector<int> bestPeers;
        int bestCost = estimateRegionShapeCount(best);
        int bestSaving = 0;
        auto consider = [&](const Mask &candidate, const Mask &required,
                            const QVector<int> &peers, int originalCost,
                            const QPainterPath &authored = QPainterPath()) {
            ++plan->topologyVariantCount;
            if (!preservesOwnership(candidate, required, allowed)) {
                return;
            }
            const QPainterPath contour = authored.isEmpty()
                ? imageMaskContour(candidate, size, smoothing) : authored;
            const int cost = estimateRegionShapeCount(contour);
            const int saving = originalCost - cost;
            if (!contour.isEmpty() && saving > bestSaving && !cubicPathLoops(contour).isEmpty()) {
                best = contour;
                bestCost = cost;
                bestSaving = saving;
                bestPeers = peers;
            }
        };
        const int initialCost = bestCost;
        consider(fillHiddenHoles(owned, allowed, size), owned, {}, initialCost);
        if (!unit.lining) {
            QPainterPath rectangle;
            rectangle.addRect(best.boundingRect());
            consider(rasterMask(rectangle, size), owned, {}, initialCost, rectangle);
            QPainterPath ellipse;
            ellipse.addEllipse(best.boundingRect());
            consider(rasterMask(ellipse, size), owned, {}, initialCost, ellipse);
            QPolygonF points;
            for (const QPolygonF &polygon : pixelBoundaryLoops(owned, size, QRect(QPoint(), size))) {
                points += polygon;
            }
            QPainterPath convex;
            convex.addPolygon(convexHull(std::move(points)));
            convex.closeSubpath();
            consider(rasterMask(convex, size), owned, {}, initialCost, convex);
            for (const int radius : kClosingRadii) {
                if (stopped(cancelled)) {
                    plan->cancelled = true;
                    return;
                }
                consider(fillHiddenHoles(closeMask(owned, size, radius), allowed, size),
                    owned, {}, initialCost);
            }
        }
        QVector<int> peers;
        QVector<int> sources = unit.sourceRegionIndices;
        int combinedCost = initialCost;
        for (int upper = lower + 1; upper < plan->units.size(); ++upper) {
            if (plan->units[upper].color == unit.color) {
                peers.push_back(upper);
                sources += plan->units[upper].sourceRegionIndices;
                combinedCost += estimateRegionShapeCount(plan->units[upper].outline);
            }
        }
        if (!peers.isEmpty()) {
            const Mask joined = ownershipMask(labels, sources);
            consider(fillHiddenHoles(joined, allowed, size), joined, peers, combinedCost);
            const QPainterPath joinedPath = imageMaskContour(joined, size, smoothing);
            QPainterPath rectangle;
            rectangle.addRect(joinedPath.boundingRect());
            consider(rasterMask(rectangle, size), joined, peers, combinedCost, rectangle);
        }
        if (bestCost < initialCost || !bestPeers.isEmpty()) {
            ++plan->topologySimplificationCount;
            plan->diagnostics << QStringLiteral("owned_topology lower=%1 estimated_shapes=%2->%3")
                .arg(lower).arg(initialCost).arg(bestCost);
        }
        unit.outline = best;
        for (const int peer : bestPeers) {
            unit.sourceRegionIndices += plan->units[peer].sourceRegionIndices;
        }
        for (auto peer = bestPeers.crbegin(); peer != bestPeers.crend(); ++peer) {
            plan->units.removeAt(*peer);
        }
        plan->topologyMergeCount += bestPeers.size();
        plan->units[lower] = std::move(unit);
        owned = ownershipMask(labels, plan->units[lower].sourceRegionIndices);
        for (size_t pixel = 0; pixel < later.size(); ++pixel) {
            later[pixel] |= owned[pixel];
        }
    }
}

} // namespace gui
