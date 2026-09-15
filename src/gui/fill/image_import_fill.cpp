#include "image_import_fill.h"

#include "region_extract.h"
#include "region_fill.h"

#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

namespace gui {
namespace {

constexpr double kLoopAreaEpsilon = 1e-6;

QString authoredCurvesVia() {
    return QStringLiteral("authored-curves");
}

QString sampledVia() {
    return QStringLiteral("sampled-rdp");
}

QString simplifiedVia() {
    return QStringLiteral("simplified-rdp");
}

RegionPenLoopConversionResult convertPreservingCurves(const QPainterPath &outline) {
    RegionPenLoopConversionOptions options;
    options.preserveInputCurves = true;

    return regionOutlineToPenLoops(outline, options);
}

RegionPenLoopConversionResult convertSampled(const QPainterPath &outline,
                                             double simplifyEpsilon) {
    RegionPenLoopConversionOptions options;
    if (simplifyEpsilon > 0.0) {
        options.simplifyEpsilon = simplifyEpsilon;
    }

    return regionOutlineToPenLoops(outline, options);
}

double polygonArea(const QPolygonF &polygon) {
    double twiceArea = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon[i];
        const QPointF &b = polygon[(i + 1) % polygon.size()];
        twiceArea += a.x() * b.y() - a.y() * b.x();
    }

    return std::abs(twiceArea * 0.5);
}

struct OutlineLoop {
    QPainterPath path;
    QPointF probe;
    double area = 0.0;
    int depth = 0;
    int parent = -1;
};

QVector<QPainterPath> closedSubpaths(const QPainterPath &outline) {
    QVector<QPainterPath> result;
    QPainterPath current;
    const auto flush = [&]() {
        if (!current.isEmpty()) {
            current.closeSubpath();
            result.push_back(current);
        }
        current = QPainterPath();
    };
    const int count = outline.elementCount();
    for (int i = 0; i < count; ++i) {
        const QPainterPath::Element element = outline.elementAt(i);
        switch (element.type) {
        case QPainterPath::MoveToElement:
            flush();
            current.moveTo(element);
            break;
        case QPainterPath::LineToElement:
            current.lineTo(element);
            break;
        case QPainterPath::CurveToElement:
            if (i + 2 < count) {
                current.cubicTo(element, outline.elementAt(i + 1), outline.elementAt(i + 2));
            }
            i += 2;
            break;
        case QPainterPath::CurveToDataElement:
            break;
        }
    }
    flush();

    return result;
}

QVector<OutlineLoop> outlineLoops(const QPainterPath &outline) {
    QVector<OutlineLoop> loops;
    for (const QPainterPath &subpath : closedSubpaths(outline)) {
        const QList<QPolygonF> polygons = subpath.toSubpathPolygons();
        if (polygons.isEmpty() || polygons.front().size() < 3) {
            continue;
        }
        OutlineLoop loop;
        loop.path = subpath;
        loop.path.setFillRule(Qt::WindingFill);
        loop.probe = polygons.front().front();
        loop.area = polygonArea(polygons.front());
        if (loop.area <= kLoopAreaEpsilon) {
            continue;
        }
        loops.push_back(std::move(loop));
    }
    for (int i = 0; i < loops.size(); ++i) {
        OutlineLoop &loop = loops[i];
        for (int j = 0; j < loops.size(); ++j) {
            const OutlineLoop &other = loops[j];
            if (j == i
                || other.area <= loop.area
                || !other.path.controlPointRect().contains(loop.probe)
                || !other.path.contains(loop.probe)) {
                continue;
            }
            ++loop.depth;
            if (loop.parent < 0 || other.area < loops[loop.parent].area) {
                loop.parent = j;
            }
        }
    }

    return loops;
}

bool convertComponents(const QVector<QPainterPath> &components,
                       bool preferSampled,
                       double simplifyEpsilon,
                       ImageImportUnitLoops *result) {
    result->components.clear();
    result->outlines.clear();
    result->components.reserve(components.size());
    result->outlines.reserve(components.size());
    bool anySampled = false;
    for (const QPainterPath &component : components) {
        const ImageImportPenLoops loops =
            imageImportPenLoops(component, preferSampled, simplifyEpsilon);
        if (!loops.valid()) {
            result->error = loops.error;
            result->components.clear();
            result->outlines.clear();
            return false;
        }
        anySampled = anySampled || loops.via != authoredCurvesVia();
        result->components.push_back(loops.loops);
        result->outlines.push_back(component);
    }
    result->error.clear();
    result->via = anySampled ? sampledVia() : authoredCurvesVia();

    return !result->components.isEmpty();
}

// Fill errors may carry measured magnitudes in parentheses; the histogram
// buckets on the message alone.
QString failureReasonKey(const QString &error) {
    if (error.isEmpty()) {
        return QStringLiteral("empty result");
    }
    const int detail = error.indexOf(QLatin1String(" ("));

    return detail > 0 ? error.left(detail) : error;
}

QString topFailureReason(const QHash<QString, int> &reasons) {
    QString topReason;
    int topCount = 0;
    for (auto it = reasons.constBegin(); it != reasons.constEnd(); ++it) {
        if (it.value() > topCount) {
            topCount = it.value();
            topReason = it.key();
        }
    }

    return topReason.isEmpty()
        ? QString()
        : QStringLiteral(" - top reason: %1 (x%2)").arg(topReason).arg(topCount);
}

} // namespace

QVector<ImageImportFillUnit> svgImageImportUnits(const SvgVectorDocument &document) {
    QVector<ImageImportFillUnit> units;
    if (!document.supportsObjectSelection()) {
        return units;
    }
    units.reserve(document.objects.size());
    for (const SvgVectorObject &object : document.objects) {
        if (object.path.isEmpty() || object.color.alpha() == 0) {
            continue;
        }
        units.push_back({object.path, object.color});
    }

    return units;
}

namespace {

// Traces a region again after growing it under the neighbours that are drawn
// on top of it, one ring per requested pixel of overlap. A pixel is claimed
// only when its region has a later draw rank than this one, so the extension
// is always hidden: the region on top keeps its exact outline, a thin region
// such as an outline effect is never eaten from either side, and empty pixels
// are never claimed, so the silhouette is unchanged. Thin-line regions are
// traced from a separate label space and keep their extracted outline.
QPainterPath overlappingRegionOutline(const RegionRasterData &raster,
                                      const QSize &size,
                                      const ExtractedRegion &region,
                                      const std::vector<int> &drawRankOfLabel,
                                      int overlap) {
    const int width = size.width();
    const int height = size.height();
    if (width <= 0 || height <= 0
        || raster.labels.size() != static_cast<size_t>(width) * height) {
        return {};
    }
    QRect bounds = region.bounds.adjusted(-overlap, -overlap, overlap, overlap)
        .intersected(QRect(0, 0, width, height));
    if (bounds.isEmpty()) {
        return {};
    }
    const auto rankOf = [&](int label) {
        return label >= 0 && label < static_cast<int>(drawRankOfLabel.size())
            ? drawRankOfLabel[static_cast<size_t>(label)] : -1;
    };
    const int ownRank = rankOf(region.id);
    if (ownRank < 0) {
        return {};
    }
    std::vector<std::uint8_t> mask(static_cast<size_t>(width) * height, 0);
    const auto at = [&](int x, int y) { return static_cast<size_t>(y) * width + x; };
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            mask[at(x, y)] = raster.labels[at(x, y)] == region.id ? 1 : 0;
        }
    }
    for (int ring = 0; ring < overlap; ++ring) {
        std::vector<std::uint8_t> next = mask;
        for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
            for (int x = bounds.left(); x <= bounds.right(); ++x) {
                if (mask[at(x, y)] != 0 || rankOf(raster.labels[at(x, y)]) <= ownRank) {
                    continue;
                }
                const bool touches = (x > 0 && mask[at(x - 1, y)] != 0)
                    || (x + 1 < width && mask[at(x + 1, y)] != 0)
                    || (y > 0 && mask[at(x, y - 1)] != 0)
                    || (y + 1 < height && mask[at(x, y + 1)] != 0);
                if (touches) {
                    next[at(x, y)] = 1;
                }
            }
        }
        mask.swap(next);
    }
    return traceMaskToPath(mask, width, height, bounds, raster.traceParams);
}

} // namespace

// Regions come out of the extractor traced in the space it processed, which
// is the source image downscaled when a dimension cap applied; the units are
// scaled back so one unit always means one source pixel. Larger regions come
// first so they sit behind the smaller ones they surround, and anything a
// fitted shape spills lands on a neighbour it would have overlapped anyway.
RasterImportUnits rasterImageImportUnits(const QImage &image,
                                         const RasterImportOptions &options) {
    RasterImportUnits result;
    if (image.isNull()) {
        result.error = QStringLiteral("The image is empty");
        return result;
    }
    RegionExtractionParams params;
    params.alphaThreshold = std::clamp(options.alphaThreshold, 0.0, 1.0);
    params.maxColorCount = std::max(2, options.maximumColors);
    params.minRegionArea = std::max(1, options.minimumRegionArea);
    params.smallRegionMergeArea = params.minRegionArea;
    params.traceSpeckle = std::max(0, options.speckleSize);
    params.traceAlphaMax = std::clamp(options.traceSmoothing, 0.0, 1.3334);
    params.maxDimension = std::max(0, options.maximumDimension);
    // Blurring would feather crisp alpha edges before the threshold sees them.
    params.blurPasses = 0;
    if (!options.separateThinLines) {
        params.lineWidthCapFraction = 0.0;
        params.lineWidthCapFloor = 0.0;
    }
    const RegionExtractionResult regions =
        extractRegions(image.convertToFormat(QImage::Format_ARGB32), params);
    if (!regions.valid()) {
        result.error = regions.error.isEmpty()
            ? QStringLiteral("No colour regions were found above the alpha threshold")
            : regions.error;
        return result;
    }
    result.processedSize = regions.imageSize;
    const double scaleX = regions.imageSize.width() > 0
        ? static_cast<double>(image.width()) / regions.imageSize.width() : 1.0;
    const double scaleY = regions.imageSize.height() > 0
        ? static_cast<double>(image.height()) / regions.imageSize.height() : 1.0;
    const QTransform toSource = QTransform::fromScale(scaleX, scaleY);

    // Draw order first: largest region first, extraction order on ties. The
    // overlap needs it to know which neighbour ends up on top.
    QVector<int> order;
    order.reserve(regions.regions.size());
    for (int index = 0; index < regions.regions.size(); ++index) {
        if (!regions.regions[index].outline.isEmpty()) {
            order.push_back(index);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const int areaA = regions.regions[a].area;
        const int areaB = regions.regions[b].area;
        if (areaA != areaB) {
            return areaA > areaB;
        }
        return a < b;
    });
    std::vector<int> drawRankOfLabel;
    for (int rank = 0; rank < order.size(); ++rank) {
        const ExtractedRegion &region = regions.regions[order[rank]];
        if (region.lineart || region.id < 0) {
            continue;
        }
        if (region.id >= static_cast<int>(drawRankOfLabel.size())) {
            drawRankOfLabel.resize(static_cast<size_t>(region.id) + 1, -1);
        }
        drawRankOfLabel[static_cast<size_t>(region.id)] = rank;
    }

    result.units.reserve(order.size());
    for (const int index : order) {
        const ExtractedRegion &region = regions.regions[index];
        QColor color = region.color;
        color.setAlpha(255);
        QPainterPath outline = region.outline;
        if (options.neighbourOverlap > 0 && !region.lineart && regions.raster) {
            const QPainterPath grown = overlappingRegionOutline(
                *regions.raster, regions.imageSize, region, drawRankOfLabel,
                options.neighbourOverlap);
            if (!grown.isEmpty()) {
                outline = grown;
            }
        }
        result.units.push_back({toSource.map(outline), color});
        result.thinLineCount += region.lineart ? 1 : 0;
    }
    if (result.units.isEmpty()) {
        result.error = QStringLiteral("No colour regions were found above the alpha threshold");
    }

    return result;
}

// Loops nested at an even depth are islands and start a component; loops at
// an odd depth are cutouts of the innermost island containing them. A single
// SVG path can therefore hold several letters, each with its own holes.
QVector<QPainterPath> imageImportOutlineComponents(const QPainterPath &outline) {
    const QVector<OutlineLoop> loops = outlineLoops(outline);

    QVector<QPainterPath> components;
    QVector<int> componentOfLoop(loops.size(), -1);
    for (int i = 0; i < loops.size(); ++i) {
        if (loops[i].depth % 2 != 0) {
            continue;
        }
        QPainterPath component;
        component.setFillRule(Qt::WindingFill);
        component.addPath(loops[i].path);
        componentOfLoop[i] = components.size();
        components.push_back(component);
    }
    for (int i = 0; i < loops.size(); ++i) {
        const OutlineLoop &loop = loops[i];
        if (loop.depth % 2 == 0 || loop.parent < 0) {
            continue;
        }
        const int componentIndex = componentOfLoop[loop.parent];
        if (componentIndex >= 0) {
            components[componentIndex].addPath(loop.path);
        }
    }

    return components;
}

// Authored curves keep the source's own control points, so a clean SVG converts
// with no resampling; a boundary the Pen rules reject falls back to the same
// sampled reconstruction the raster Bucket uses. A simplified outline has had
// its curves flattened, so sampling is preferred there.
ImageImportPenLoops imageImportPenLoops(const QPainterPath &component,
                                        bool preferSampled,
                                        double simplifyEpsilon) {
    ImageImportPenLoops result;
    if (component.isEmpty()) {
        result.error = QStringLiteral("The object has no outline");
        return result;
    }
    const RegionPenLoopConversionResult first = preferSampled
        ? convertSampled(component, simplifyEpsilon) : convertPreservingCurves(component);
    if (first.valid()) {
        result.loops = first.loops;
        result.via = preferSampled ? sampledVia() : authoredCurvesVia();
        return result;
    }
    const RegionPenLoopConversionResult second = preferSampled
        ? convertPreservingCurves(component) : convertSampled(component, simplifyEpsilon);
    if (second.valid()) {
        result.loops = second.loops;
        result.via = preferSampled ? authoredCurvesVia() : sampledVia();
        return result;
    }
    result.error = second.error.isEmpty()
        ? (first.error.isEmpty()
               ? QStringLiteral("The object is not a valid Pen contour")
               : first.error)
        : second.error;

    return result;
}

// Stroked outlines and other self-overlapping paths only become valid Pen
// contours after Qt merges their overlaps into one non-crossing outline.
ImageImportUnitLoops imageImportUnitLoops(const QPainterPath &outline,
                                          double outlineSimplification) {
    ImageImportUnitLoops result;
    if (outline.isEmpty()) {
        result.error = QStringLiteral("The object has no outline");
        return result;
    }
    const QVector<QPainterPath> components = imageImportOutlineComponents(outline);
    if (components.isEmpty()) {
        result.error = QStringLiteral("The object has no fillable area");
        return result;
    }
    const bool sampleFirst = outlineSimplification > 0.0;
    if (convertComponents(components, sampleFirst, outlineSimplification, &result)) {
        return result;
    }
    const QString directError = result.error;
    const QVector<QPainterPath> simplifiedComponents =
        imageImportOutlineComponents(outline.simplified());
    if (!simplifiedComponents.isEmpty()
        && convertComponents(simplifiedComponents, true, outlineSimplification, &result)) {
        result.via = simplifiedVia();
        return result;
    }
    result.components.clear();
    result.error = directError.isEmpty()
        ? QStringLiteral("The object is not a valid Pen contour") : directError;

    return result;
}

ImageImportFillResult computeImageImportFills(
    const ImageImportFillRequest &request,
    const ImageImportFillProgress &progress,
    const std::function<bool()> &cancelled) {
    ImageImportFillResult result;
    if (request.units.isEmpty()) {
        result.error = QStringLiteral("The image has no fillable vector objects");
        return result;
    }
    if (request.primitives.isEmpty()) {
        result.error = QStringLiteral("Pen primitive geometry is unavailable");
        return result;
    }

    const int total = request.units.size();
    const auto globallyCancelled = [&cancelled]() {
        return cancelled && cancelled();
    };
    const int availableThreads = std::max(1, QThread::idealThreadCount());
    const int workerCount = std::min(std::max(1, availableThreads / 2), total);

    result.units.resize(total);
    if (progress) {
        progress(0, total);
    }
    std::atomic<int> nextUnit{0};
    std::atomic<int> completedUnits{0};
    QThreadPool fillPool;
    fillPool.setMaxThreadCount(workerCount);
    for (int worker = 0; worker < workerCount; ++worker) {
        fillPool.start([&]() {
            while (true) {
                const int index = nextUnit.fetch_add(1, std::memory_order_relaxed);
                if (index >= total) {
                    return;
                }
                const ImageImportFillUnit &unit = request.units[index];
                ImageImportFilledUnit &work = result.units[index];
                work.color = unit.color;
                if (!globallyCancelled()) {
                    QElapsedTimer unitClock;
                    unitClock.start();
                    const ImageImportUnitLoops unitLoops =
                        imageImportUnitLoops(unit.outline, request.outlineSimplification);
                    QString firstError;
                    if (!unitLoops.valid()) {
                        firstError = unitLoops.error;
                    } else {
                        work.via = unitLoops.via;
                        work.componentCount = unitLoops.components.size();
                        work.components.reserve(work.componentCount);
                        for (int componentIndex = 0;
                             componentIndex < unitLoops.components.size(); ++componentIndex) {
                            const QVector<PenLoop> &loops = unitLoops.components[componentIndex];
                            if (globallyCancelled()) {
                                break;
                            }
                            ImageImportComponentResult component;
                            component.loopCount = loops.size();
                            for (const PenLoop &loop : loops) {
                                component.pointCount += loop.points.size();
                            }
                            work.loopCount += loops.size();
                            // The curve fitting search grows with the point
                            // count, so a large component earns more time.
                            const qint64 componentBudgetMs = request.componentBudgetMs
                                + request.componentBudgetMsPerPoint * component.pointCount;
                            QElapsedTimer componentClock;
                            componentClock.start();
                            const auto componentCancelled = [&]() {
                                return globallyCancelled()
                                    || componentClock.elapsed() > componentBudgetMs;
                            };
                            PenFillRequest fill;
                            fill.loops = loops;
                            fill.primitives = request.primitives;
                            fill.boundaryTolerance = request.boundaryTolerance;
                            fill.shapeLimitPerPoint = request.shapeLimitPerPoint;
                            fill.spillWithinTolerance = true;
                            PenFillResult fit = fillPenPath(fill, componentCancelled);
                            // A traced or authored outline can pass the Pen
                            // rules and still defeat the fill (a tiny loop at a
                            // corner, say); the sampled reconstruction smooths
                            // such detail away, so it gets one try within the
                            // same budget before the component is given up.
                            if (!fit.error.isEmpty() && !fit.cancelled
                                && componentIndex < unitLoops.outlines.size()) {
                                const ImageImportPenLoops resampled = imageImportPenLoops(
                                    unitLoops.outlines[componentIndex], true,
                                    request.outlineSimplification);
                                if (resampled.valid()) {
                                    fill.loops = resampled.loops;
                                    PenFillResult retry = fillPenPath(fill, componentCancelled);
                                    if (retry.error.isEmpty() && !retry.placements.isEmpty()) {
                                        fit = std::move(retry);
                                        component.via = QStringLiteral("sampled-retry");
                                    } else if (retry.cancelled) {
                                        fit = std::move(retry);
                                    }
                                }
                            }
                            component.elapsedMs = componentClock.elapsed();
                            if (fit.cancelled && !globallyCancelled()) {
                                component.timedOut = true;
                                component.error = QStringLiteral("Timed out");
                            } else if (!fit.error.isEmpty()) {
                                component.error = fit.error;
                            } else if (fit.placements.isEmpty()) {
                                component.error = QStringLiteral("No shapes generated");
                            }
                            if (component.error.isEmpty()) {
                                component.placementCount = fit.placements.size();
                                work.placements += fit.placements;
                            } else {
                                ++work.failedComponentCount;
                                if (component.timedOut) {
                                    work.timedOut = true;
                                    ++work.timedOutComponentCount;
                                }
                                if (firstError.isEmpty()) {
                                    firstError = component.error;
                                }
                            }
                            work.components.push_back(std::move(component));
                        }
                    }
                    if (work.placements.isEmpty()) {
                        work.error = firstError.isEmpty()
                            ? QStringLiteral("No shapes generated") : firstError;
                    }
                    work.elapsedMs = unitClock.elapsed();
                }
                const int done = completedUnits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (progress) {
                    progress(done, total);
                }
            }
        });
    }
    fillPool.waitForDone();
    if (globallyCancelled()) {
        result.cancelled = true;
        return result;
    }

    for (const ImageImportFilledUnit &unit : std::as_const(result.units)) {
        if (unit.timedOut) {
            ++result.timedOutCount;
        }
        result.componentCount += unit.componentCount;
        result.failedComponentCount += unit.failedComponentCount;
        result.timedOutComponentCount += unit.timedOutComponentCount;
        for (const ImageImportComponentResult &component : unit.components) {
            if (component.filled()) {
                ++result.filledComponentCount;
            } else {
                result.componentFailureReasons[failureReasonKey(component.error)] += 1;
            }
        }
        if (unit.filled()) {
            ++result.filledCount;
            result.placementCount += unit.placements.size();
            if (unit.failedComponentCount > 0) {
                ++result.partialCount;
            }
            continue;
        }
        ++result.failedCount;
        if (unit.components.isEmpty()) {
            result.componentFailureReasons[failureReasonKey(unit.error)] += 1;
        }
    }
    const QString reasonSuffix = topFailureReason(result.componentFailureReasons);
    if (result.filledCount == 0) {
        result.error = QStringLiteral("No objects could be filled%1").arg(reasonSuffix);
        return result;
    }
    result.summary = QStringLiteral(
        "Filled %1 of %2 objects, %3 of %4 components, with %5 shapes; "
        "%6 components failed, %7 timed out%8")
        .arg(result.filledCount)
        .arg(total)
        .arg(result.filledComponentCount)
        .arg(result.componentCount)
        .arg(result.placementCount)
        .arg(result.failedComponentCount)
        .arg(result.timedOutComponentCount)
        .arg(reasonSuffix);

    return result;
}

} // namespace gui
