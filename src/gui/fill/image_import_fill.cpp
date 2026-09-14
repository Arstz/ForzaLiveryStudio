#include "image_import_fill.h"

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

RegionPenLoopConversionResult convertSampled(const QPainterPath &outline) {
    return regionOutlineToPenLoops(outline, RegionPenLoopConversionOptions{});
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
                       ImageImportUnitLoops *result) {
    result->components.clear();
    result->components.reserve(components.size());
    bool anySampled = false;
    for (const QPainterPath &component : components) {
        const ImageImportPenLoops loops = imageImportPenLoops(component, preferSampled);
        if (!loops.valid()) {
            result->error = loops.error;
            result->components.clear();
            return false;
        }
        anySampled = anySampled || loops.via != authoredCurvesVia();
        result->components.push_back(loops.loops);
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
ImageImportPenLoops imageImportPenLoops(const QPainterPath &component, bool preferSampled) {
    ImageImportPenLoops result;
    if (component.isEmpty()) {
        result.error = QStringLiteral("The object has no outline");
        return result;
    }
    const RegionPenLoopConversionResult first = preferSampled
        ? convertSampled(component) : convertPreservingCurves(component);
    if (first.valid()) {
        result.loops = first.loops;
        result.via = preferSampled ? sampledVia() : authoredCurvesVia();
        return result;
    }
    const RegionPenLoopConversionResult second = preferSampled
        ? convertPreservingCurves(component) : convertSampled(component);
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
ImageImportUnitLoops imageImportUnitLoops(const QPainterPath &outline) {
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
    if (convertComponents(components, false, &result)) {
        return result;
    }
    const QString directError = result.error;
    const QVector<QPainterPath> simplifiedComponents =
        imageImportOutlineComponents(outline.simplified());
    if (!simplifiedComponents.isEmpty()
        && convertComponents(simplifiedComponents, true, &result)) {
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
                    const ImageImportUnitLoops unitLoops = imageImportUnitLoops(unit.outline);
                    QString firstError;
                    if (!unitLoops.valid()) {
                        firstError = unitLoops.error;
                    } else {
                        work.via = unitLoops.via;
                        work.componentCount = unitLoops.components.size();
                        work.components.reserve(work.componentCount);
                        for (const QVector<PenLoop> &loops : unitLoops.components) {
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
                            PenFillResult fit = fillPenPath(fill, componentCancelled);
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
