#include "region_fill.h"
#include "curve_fill.h"
#include "region_layer_plan.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <limits>
#include <vector>

namespace gui {

namespace {

constexpr QRgb kSafeOnlyDifferenceColor = qRgba(255, 55, 55, 230);
constexpr QRgb kDangerousOnlyDifferenceColor = qRgba(30, 220, 255, 230);
constexpr QRgb kChangedColorDifferenceColor = qRgba(255, 215, 35, 230);
struct CurveRegionTrace {
    QPainterPath target;
    QPainterPath legalEnvelope;
};

CurveRegionTrace traceCurveRegion(
    const QPainterPath &outline,
    const RegionExtractionResult &regions) {
    CurveRegionTrace result;
    const QRect imageBounds(QPoint(0, 0), regions.imageSize);
    const QRect bounds = outline.boundingRect().toAlignedRect()
        .adjusted(-1, -1, 1, 1).intersected(imageBounds);
    if (bounds.isEmpty()) {
        return result;
    }
    const int width = bounds.width();
    const int height = bounds.height();
    std::vector<std::uint8_t> targetMask(
        static_cast<size_t>(width) * height, 0);
    std::vector<std::uint8_t> legalMask(targetMask.size(), 0);
    const auto localIndex = [width](int x, int y) {
        return static_cast<size_t>(y) * width + x;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const QPointF center(bounds.left() + x + 0.5,
                                 bounds.top() + y + 0.5);
            if (outline.contains(center)) {
                targetMask[localIndex(x, y)] = 1;
                legalMask[localIndex(x, y)] = 1;
            }
        }
    }
    const size_t imagePixelCount = static_cast<size_t>(
        regions.imageSize.width()) * regions.imageSize.height();
    if (regions.raster != nullptr
        && regions.raster->foreground.size() == imagePixelCount) {
        const int imageWidth = regions.imageSize.width();
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int imageX = bounds.left() + x;
                const int imageY = bounds.top() + y;
                const size_t imageIndex = static_cast<size_t>(imageY)
                    * imageWidth + imageX;
                if (regions.raster->foreground[imageIndex] == 0) {
                    continue;
                }
                bool adjacent = false;
                for (int offsetY = -1; offsetY <= 1 && !adjacent; ++offsetY) {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                        const int neighborX = x + offsetX;
                        const int neighborY = y + offsetY;
                        if (neighborX >= 0 && neighborY >= 0
                            && neighborX < width && neighborY < height
                            && targetMask[localIndex(neighborX, neighborY)] != 0) {
                            adjacent = true;
                            break;
                        }
                    }
                }
                if (adjacent) {
                    legalMask[localIndex(x, y)] = 1;
                }
            }
        }
    }
    RegionExtractionParams traceOptions = regions.raster != nullptr
        ? regions.raster->traceParams : RegionExtractionParams{};
    traceOptions.traceSpeckle = 0;
    traceOptions.traceOptTolerance = 0.0;
    result.target = traceMaskToPath(
        targetMask, width, height, QRect(0, 0, width, height), traceOptions);
    result.legalEnvelope = traceMaskToPath(
        legalMask, width, height, QRect(0, 0, width, height), traceOptions);
    result.target.translate(bounds.left(), bounds.top());
    result.legalEnvelope.translate(bounds.left(), bounds.top());

    return result;
}

QImage renderRegionFillVariant(
    const QSize &imageSize,
    const QVector<RegionFillLayer> &fills,
    const QHash<int, QPainterPath> &silhouettes,
    RegionFillVariant variant) {
    QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    for (const RegionFillLayer &fill : fills) {
        if (fill.variant != variant) {
            continue;
        }
        painter.setBrush(fill.color);
        for (const PenPlacement &placement : fill.placements) {
            const auto silhouette = silhouettes.constFind(placement.shapeId);
            if (silhouette != silhouettes.constEnd()) {
                painter.drawPath(placement.transform.map(silhouette.value()));
            }
        }
    }
    painter.end();

    return image;
}

QImage regionFillDifferenceHeatmap(
    const QImage &safe,
    const QImage &dangerous,
    int *differencePixelCount) {
    if (differencePixelCount != nullptr) {
        *differencePixelCount = 0;
    }
    if (safe.isNull() || dangerous.isNull() || safe.size() != dangerous.size()) {
        return {};
    }
    QImage heatmap(safe.size(), QImage::Format_ARGB32);
    heatmap.fill(Qt::transparent);
    int differences = 0;
    for (int y = 0; y < safe.height(); ++y) {
        const QRgb *safeRow =
            reinterpret_cast<const QRgb *>(safe.constScanLine(y));
        const QRgb *dangerousRow =
            reinterpret_cast<const QRgb *>(dangerous.constScanLine(y));
        QRgb *heatmapRow = reinterpret_cast<QRgb *>(heatmap.scanLine(y));
        for (int x = 0; x < safe.width(); ++x) {
            if (safeRow[x] == dangerousRow[x]) {
                continue;
            }
            ++differences;
            const bool safeVisible = qAlpha(safeRow[x]) > 0;
            const bool dangerousVisible = qAlpha(dangerousRow[x]) > 0;
            if (safeVisible && !dangerousVisible) {
                heatmapRow[x] = kSafeOnlyDifferenceColor;
            } else if (!safeVisible && dangerousVisible) {
                heatmapRow[x] = kDangerousOnlyDifferenceColor;
            } else {
                heatmapRow[x] = kChangedColorDifferenceColor;
            }
        }
    }
    if (differencePixelCount != nullptr) {
        *differencePixelCount = differences;
    }

    return heatmap;
}

} // namespace

RegionFillBatchResult computeRegionFills(
    const RegionFillBatchRequest &request,
    const std::function<void(const QString &, int, int)> &progress,
    const std::function<bool()> &cancelled) {
    RegionFillBatchResult result;
    result.overlayGuideId = request.overlayGuideId;
    result.overlayGeneration = request.overlayGeneration;
    const RegionExtractionResult &regionOverlay = request.regions;
    QVector<PenPrimitive> primitives = request.primitives;
    QVector<cover::ShapeMesh> curveMeshes;
    if (request.algorithm == FillAlgorithm::CurveBased) {
        const CurveFillCatalog catalog = cachedCurveFillCatalog(
            request.geometry, progress, cancelled);
        if (!catalog.valid()) {
            result.cancelled = cancelled && cancelled();
            result.error = catalog.error.isEmpty()
                ? QStringLiteral("Curve shape catalog is unavailable")
                : catalog.error;
            return result;
        }
        primitives = catalog.primitives;
        curveMeshes = catalog.meshes;
    }
    if (regionOverlay.regions.isEmpty() || primitives.isEmpty()) {
        result.error = QStringLiteral("Region fill input is empty");
        return result;
    }

    QHash<int, QPainterPath> silhouettes;
    for (const PenPrimitive &primitive : primitives) {
        silhouettes.insert(primitive.shapeId, primitive.silhouette);
    }
    const double tolerance = std::max(1.0,
                                      std::min(regionOverlay.imageSize.width(),
                                               regionOverlay.imageSize.height()) * 0.004);

    QVector<RegionFillLayer> fills;
    int filled = 0;
    int failed = 0;
    int timedOut = 0;
    int placementCount = 0;
    int meshFallbacks = 0;
    int sourceOutlineFallbacks = 0;
    int softRunRetries = 0;
    int baselineRetries = 0;
    int removedSoftPoints = 0;
    int coreEllipseCount = 0;
    int safeRdpFills = 0;
    int dangerousRdpFills = 0;
    QHash<QString, int> failureReasons;
    constexpr qint64 kRegionBudgetMs = 3000;
    constexpr qint64 kCurveRegionBudgetMs = 10000;
    const PolygonMeshSources &meshSources = request.meshSources;
    constexpr double kFallbackSimplifyEpsilon = 0.45;
    constexpr double kCyclicRdpEpsilon = 1.9;
    constexpr int kCyclicRdpCurveSamples = 32;
    const auto globallyCancelled = [&cancelled]() {
        return cancelled && cancelled();
    };
    struct FillUnit {
        QColor color;
        QPainterPath outline;
        QVector<int> sourceIndices;
        QVector<int> absorbedIndices;
        double area = 0.0;
        int drawOrder = -1;
        RegionFillVariant variant = RegionFillVariant::Safe;
    };
    QElapsedTimer planningClock;
    planningClock.start();
    const RegionLayerPlanVariants layerPlans =
        buildRegionLayerPlanVariants(regionOverlay, progress, globallyCancelled);
    const qint64 planningElapsedMs = planningClock.elapsed();
    if (layerPlans.safe.cancelled || layerPlans.dangerous.cancelled
        || globallyCancelled()) {
        result.cancelled = true;
        return result;
    }
    QVector<FillUnit> units;
    units.reserve(layerPlans.safe.units.size() + layerPlans.dangerous.units.size());
    const auto appendPlanUnits = [&units](const RegionLayerPlan &plan,
                                          RegionFillVariant variant) {
        for (int drawOrder = 0; drawOrder < plan.units.size(); ++drawOrder) {
            const RegionLayerUnit &unit = plan.units[drawOrder];
            units.push_back(FillUnit{unit.color, unit.outline,
                                     unit.sourceRegionIndices,
                                     unit.absorbedRegionIndices, unit.area,
                                     drawOrder, variant});
        }
    };
    appendPlanUnits(layerPlans.safe, RegionFillVariant::Safe);
    appendPlanUnits(layerPlans.dangerous, RegionFillVariant::Dangerous);
    QStringList log;
    log << QStringLiteral("Fill Regions diagnostic - image %1x%2, tolerance %3, "
                          "%4 safe + %5 dangerous planned units, planning %6 ms")
               .arg(regionOverlay.imageSize.width())
               .arg(regionOverlay.imageSize.height())
               .arg(tolerance, 0, 'f', 3)
               .arg(layerPlans.safe.units.size())
               .arg(layerPlans.dangerous.units.size())
               .arg(planningElapsedMs);
    if (request.algorithm == FillAlgorithm::CurveBased) {
        log << QStringLiteral(
            "Safe/Dangerous contour mode: high-precision Potrace, curve point "
            "optimization, one-pixel colored-region envelope, curve-template fill");
    } else {
        log << QStringLiteral("Safe/Dangerous contour mode: cyclic closed RDP epsilon=%1, "
                              "%2 curve samples, direct polygon mesh; Pen fitting is "
                              "retained as recovery")
                   .arg(kCyclicRdpEpsilon, 0, 'f', 1)
                   .arg(kCyclicRdpCurveSamples);
    }
    const QString layerLogPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/region_layer.log");
    QFile layerLogFile(layerLogPath);
    if (layerLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream stream(&layerLogFile);
        const auto writePlan = [&stream](const char *name,
                                         const RegionLayerPlan &plan) {
            stream << '[' << name << "]\n"
                   << "input_regions=" << plan.inputRegionCount << '\n'
                   << "planned_units=" << plan.units.size() << '\n'
                   << "same_color_merges=" << plan.sameColorMergeCount << '\n'
                   << "nearby_same_color_merges="
                   << plan.nearbySameColorMergeCount << '\n'
                   << "edge_component_merges="
                   << plan.edgeComponentMergeCount << '\n'
                   << "hierarchical_contour_merges="
                   << plan.hierarchicalContourMergeCount << '\n'
                   << "nearby_conflict_rejections="
                   << plan.nearbyConflictRejectCount << '\n'
                   << "nearby_foreign_owner_rejections="
                   << plan.nearbyForeignOwnerRejectCount << '\n'
                   << "adjacent_conflict_rejections="
                   << plan.adjacentConflictRejectCount << '\n'
                   << "containment_conflict_rejections="
                   << plan.containmentConflictRejectCount << '\n'
                   << "absorption_conflict_rejections="
                   << plan.absorptionConflictRejectCount << '\n'
                   << "suppressed_operations="
                   << plan.suppressedOperationCount << '\n'
                   << "dependency_cycle_rebuilds="
                   << plan.dependencyCycleRebuildCount << '\n'
                   << "conflict_isolated_sources="
                   << plan.conflictIsolatedSourceCount << '\n'
                   << "absorbed_regions=" << plan.absorbedRegionCount << '\n'
                   << "large_contained_absorptions="
                   << plan.largeContainedAbsorptionCount << '\n'
                   << "morphological_closings="
                   << plan.morphologicalClosingCount << '\n'
                   << "convex_simplifications="
                   << plan.convexSimplificationCount << '\n'
                   << "geometry_point_reductions="
                   << plan.geometryPointReductionCount << '\n'
                   << "dangerous_cycle_breaks="
                   << plan.dangerousCycleBreakCount << '\n'
                   << "ordering_edges=" << plan.orderingEdgeCount << '\n'
                   << "validation_mismatch_pixels="
                   << plan.validationMismatchPixels << '\n'
                   << "fallback=" << (plan.fallback ? "yes" : "no") << '\n';
            if (!plan.fallbackReason.isEmpty()) {
                stream << "fallback_reason=" << plan.fallbackReason << '\n';
            }
            stream << '\n' << plan.diagnostics.join(QLatin1Char('\n')) << "\n\n";
        };
        stream << "Region layer plan variants diagnostic\n\n";
        writePlan("safe", layerPlans.safe);
        writePlan("dangerous", layerPlans.dangerous);
        layerLogFile.close();
        qWarning().noquote() << "Region layer log written to" << layerLogPath;
    } else {
        qWarning().noquote() << "Could not write region layer log to" << layerLogPath;
    }
    const auto unitSourcesText = [&regionOverlay](const FillUnit &unit) {
        QStringList values;
        values.reserve(unit.sourceIndices.size());
        for (const int sourceIndex : unit.sourceIndices) {
            if (sourceIndex >= 0 && sourceIndex < regionOverlay.regions.size()) {
                values.push_back(QStringLiteral("%1/label-%2")
                                     .arg(sourceIndex)
                                     .arg(regionOverlay.regions[sourceIndex].id));
            }
        }

        return values.join(QLatin1Char(','));
    };
    const auto absorbedSourcesText = [&regionOverlay](const FillUnit &unit) {
        QStringList values;
        values.reserve(unit.absorbedIndices.size());
        for (const int sourceIndex : unit.absorbedIndices) {
            if (sourceIndex >= 0 && sourceIndex < regionOverlay.regions.size()) {
                values.push_back(QStringLiteral("%1/label-%2")
                                     .arg(sourceIndex)
                                     .arg(regionOverlay.regions[sourceIndex].id));
            }
        }

        return values.join(QLatin1Char(','));
    };
    for (int i = 0; i < units.size(); ++i) {
        const QString variantName = units[i].variant == RegionFillVariant::Safe
            ? QStringLiteral("safe") : QStringLiteral("dangerous");
        log << QStringLiteral("%1 plan #%2 color=%3 sources=%4 absorbed=%5 area=%6")
                   .arg(variantName)
                   .arg(units[i].drawOrder)
                   .arg(units[i].color.name(QColor::HexRgb))
                   .arg(unitSourcesText(units[i]))
                   .arg(absorbedSourcesText(units[i]))
                   .arg(units[i].area, 0, 'f', 0);
    }
    result.totalRegions = units.size();
    if (progress) {
        progress(QStringLiteral("Filling regions"), 0, result.totalRegions);
    }
    if (units.isEmpty()) {
        result.error = QStringLiteral("No colour regions are available to fill");
        return result;
    }
    int biggestUnitIndex = 0;
    for (int i = 1; i < units.size(); ++i) {
        if (units[i].area > units[biggestUnitIndex].area) {
            biggestUnitIndex = i;
        }
    }

    struct UnitWorkResult {
        PenFillResult fit;
        QString via = QStringLiteral("failed");
        QString cyclicRdpError;
        QString penError;
        RegionFillContourStats contourStats;
        QVector<PenPoint> optimizedPenPoints;
        QPolygonF optimizedContour;
        qint64 elapsedMs = 0;
        int fallbackInputPoints = 0;
        int fallbackMeshPoints = 0;
        int cyclicRdpInputPoints = 0;
        int cyclicRdpOutputPoints = 0;
        bool timedOut = false;
        bool meshFallback = false;
    };
    std::vector<UnitWorkResult> workResults(static_cast<size_t>(units.size()));
    const auto fitSucceeded = [](const PenFillResult &fit) {
        return fit.error.isEmpty() && !fit.placements.isEmpty() && !fit.cancelled;
    };
    std::atomic<int> nextUnit{0};
    std::atomic<int> completedUnits{0};
    const int availableThreads = std::max(1, QThread::idealThreadCount());
    const int requestedWorkers = std::max(1, availableThreads / 2);
    const int workerCount = std::min(requestedWorkers, static_cast<int>(units.size()));
    log << QStringLiteral("Workers: %1 of %2 available CPU threads")
               .arg(workerCount)
               .arg(availableThreads);

    QThreadPool fillPool;
    fillPool.setMaxThreadCount(workerCount);
    for (int worker = 0; worker < workerCount; ++worker) {
        fillPool.start([&, worker]() {
            Q_UNUSED(worker);
            while (true) {
                const int i = nextUnit.fetch_add(1, std::memory_order_relaxed);
                if (i >= units.size()) {
                    return;
                }
                UnitWorkResult &work = workResults[static_cast<size_t>(i)];
                if (!globallyCancelled()) {
                    const FillUnit &unit = units[i];
                    QElapsedTimer clock;
                    clock.start();
                    const auto unitCancelled = [
                        &clock, &globallyCancelled, &request]() {
                        const qint64 budget = request.algorithm
                                == FillAlgorithm::CurveBased
                            ? kCurveRegionBudgetMs : kRegionBudgetMs;
                        return globallyCancelled() || clock.elapsed() > budget;
                    };
                    QPolygonF optimizedContour;
                    if (request.algorithm == FillAlgorithm::CurveBased) {
                        const CurveRegionTrace trace = traceCurveRegion(
                            unit.outline, regionOverlay);
                        RegionPenLoopConversionOptions conversionOptions;
                        conversionOptions.curveBased = true;
                        conversionOptions.fallback.comparisonImageSize =
                            regionOverlay.imageSize;
                        conversionOptions.fallback.mergeTolerance =
                            kCurveContourMergeTolerance;
                        conversionOptions.fallback.maximumDeviationMultiplier = 1.0;
                        conversionOptions.fallback.maximumDssim =
                            kCurveContourMaximumDssim;
                        const RegionPenLoopConversionResult conversion =
                            regionOutlineToPenLoops(
                                trace.target, conversionOptions);
                        work.via = unit.variant == RegionFillVariant::Safe
                            ? QStringLiteral("safe-curve-based")
                            : QStringLiteral("dangerous-curve-based");
                        if (conversion.valid()) {
                            PenFillRequest fillRequest;
                            fillRequest.loops = conversion.loops;
                            fillRequest.primitives = primitives;
                            fillRequest.curveMeshes = curveMeshes;
                            fillRequest.targetPath = trace.target;
                            fillRequest.legalEnvelope = trace.legalEnvelope;
                            fillRequest.boundaryTolerance = tolerance;
                            fillRequest.shapeLimit = std::max(
                                6, trace.target.elementCount() * 2);
                            fillRequest.useGpu = request.useGpu;
                            work.fit = fillPenPath(fillRequest, unitCancelled);
                            const PenContour optimized = buildPenContour(
                                conversion.loops);
                            if (optimized.valid()) {
                                optimizedContour = regionOuterContour(
                                    optimized.path, kCyclicRdpCurveSamples);
                                work.contourStats.flattenedPointCount =
                                    optimizedContour.size();
                            }
                            const int flattenedPointCount =
                                work.contourStats.flattenedPointCount;
                            work.contourStats = conversion.contourStats;
                            work.contourStats.flattenedPointCount =
                                flattenedPointCount;
                            if (i == biggestUnitIndex) {
                                work.optimizedContour = optimizedContour;
                                work.optimizedPenPoints =
                                    conversion.loops.front().points;
                            }
                        } else {
                            work.fit.error = conversion.error.isEmpty()
                                ? QStringLiteral("Curve contour optimization failed")
                                : conversion.error;
                        }
                    } else {
                        const QPolygonF sourceContour = regionOuterContour(
                            unit.outline, kCyclicRdpCurveSamples);
                        const QPolygonF simplifiedContour =
                            simplifyClosedPolygonCyclic(
                                sourceContour, kCyclicRdpEpsilon);
                        work.cyclicRdpInputPoints = sourceContour.size();
                        work.cyclicRdpOutputPoints = simplifiedContour.size();
                        work.contourStats.originalPointCount =
                            regionOutlinePenPointCount(unit.outline);
                        work.contourStats.optimizedPointCount =
                            simplifiedContour.size();
                        work.contourStats.flattenedPointCount =
                            simplifiedContour.size();
                        work.contourStats.dssim =
                            std::numeric_limits<double>::quiet_NaN();
                        work.via = unit.variant == RegionFillVariant::Safe
                            ? QStringLiteral("safe-rdp-mesh")
                            : QStringLiteral("dangerous-rdp-mesh");
                        work.fit = fillPolygonMesh(
                            simplifiedContour, meshSources, unitCancelled);
                        if (i == biggestUnitIndex) {
                            work.optimizedContour = simplifiedContour;
                            work.optimizedPenPoints.reserve(
                                simplifiedContour.size());
                            for (const QPointF &point : simplifiedContour) {
                                work.optimizedPenPoints.push_back(
                                    PenPoint{point, PenPointKind::Hard});
                            }
                        }
                        if (!fitSucceeded(work.fit)) {
                            work.cyclicRdpError = work.fit.error;
                        }
                        if (!fitSucceeded(work.fit) && !globallyCancelled()) {
                            work.via = unit.variant == RegionFillVariant::Dangerous
                                ? QStringLiteral("dangerous-rdp-pen-fallback")
                                : QStringLiteral("safe-rdp-pen-fallback");
                            work.fit = fillRegionOutline(
                                unit.outline, primitives, tolerance, unitCancelled,
                                &optimizedContour, &work.contourStats,
                                i == biggestUnitIndex
                                    ? &work.optimizedPenPoints : nullptr,
                                regionOverlay.imageSize);
                            if (i == biggestUnitIndex) {
                                work.optimizedContour = optimizedContour;
                            }
                            if (work.contourStats.softRunRetry
                                && fitSucceeded(work.fit)) {
                                work.via = QStringLiteral("hard-only-pen-retry");
                            } else if (work.contourStats.baselineRetry
                                       && fitSucceeded(work.fit)) {
                                work.via = QStringLiteral("baseline-pen-retry");
                            }
                        }
                    }
                    if (work.fit.cancelled && !globallyCancelled()) {
                        work.timedOut = true;
                    }

                    if (!fitSucceeded(work.fit) && !globallyCancelled()) {
                        work.penError = work.fit.error;
                        work.meshFallback = true;
                        work.via = work.timedOut
                            ? QStringLiteral("optimized-mesh-timeout")
                            : QStringLiteral("optimized-mesh-fallback");
                        QPolygonF fallbackContour = optimizedContour.size() >= 3
                            ? optimizedContour : regionOuterContour(unit.outline);
                        if (fallbackContour.size() >= 3) {
                            if (optimizedContour.size() < 3) {
                                work.via = QStringLiteral("source-outline-mesh-fallback");
                            }
                            work.fallbackInputPoints = fallbackContour.size();
                            QPolygonF meshContour = simplifyClosedPolygon(
                                fallbackContour, kFallbackSimplifyEpsilon);
                            if (!buildPolygonContour(meshContour).valid()) {
                                meshContour = fallbackContour;
                            }
                            work.fallbackMeshPoints = meshContour.size();
                            work.fit = fillPolygonMesh(meshContour, meshSources,
                                                       globallyCancelled);
                            if (!fitSucceeded(work.fit)
                                && meshContour.size() != fallbackContour.size()
                                && !globallyCancelled()) {
                                work.fallbackMeshPoints = fallbackContour.size();
                                work.fit = fillPolygonMesh(fallbackContour, meshSources,
                                                            globallyCancelled);
                            }
                        } else if (work.fit.error.isEmpty()) {
                            work.fit = PenFillResult{};
                            work.fit.error = QStringLiteral("Optimized contour is unavailable");
                        }
                    }
                    if (!fitSucceeded(work.fit)) {
                        work.via = QStringLiteral("failed");
                    }
                    work.elapsedMs = clock.elapsed();
                }
                const int done = completedUnits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (progress) {
                    progress(QStringLiteral("Filling regions"),
                             done, result.totalRegions);
                }
            }
        });
    }
    fillPool.waitForDone();
    if (globallyCancelled()) {
        result.cancelled = true;
        return result;
    }
    result.completedRegions = completedUnits.load(std::memory_order_relaxed);

    const FillUnit &biggestUnit = units[biggestUnitIndex];
    const UnitWorkResult &biggestWork =
        workResults[static_cast<size_t>(biggestUnitIndex)];
    log << QStringLiteral("Biggest region points: region #%1 (sources %2) %3 area=%4, "
                          "original=%5 optimized=%6 flattened=%7 removed-hard=%8, "
                          "removed-soft=%9, hard-only-retry=%10, "
                          "optimization-skipped=%11, DSSIM=%12")
               .arg(biggestUnitIndex)
               .arg(unitSourcesText(biggestUnit))
               .arg(biggestUnit.color.name())
               .arg(biggestUnit.area, 0, 'f', 0)
               .arg(biggestWork.contourStats.originalPointCount)
               .arg(biggestWork.contourStats.optimizedPointCount)
               .arg(biggestWork.contourStats.flattenedPointCount)
               .arg(biggestWork.contourStats.removedHardPoints)
               .arg(biggestWork.contourStats.removedSoftPoints)
               .arg(biggestWork.contourStats.softRunRetry
                        ? QStringLiteral("yes") : QStringLiteral("no"))
               .arg(biggestWork.contourStats.optimizationSkipped
                         ? QStringLiteral("yes") : QStringLiteral("no"))
               .arg(biggestWork.contourStats.dssim, 0, 'g', 8);
    log << QStringLiteral(
               "Biggest region relocation: moved-hard=%1 maximum-movement=%2 "
               "area-error=%3")
               .arg(biggestWork.contourStats.movedHardPoints)
               .arg(biggestWork.contourStats.maximumHardPointMovement,
                    0, 'g', 8)
               .arg(biggestWork.contourStats.areaErrorRatio, 0, 'g', 8);

    const QString pointsLogPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/region_points.log");
    QFile pointsLogFile(pointsLogPath);
    if (pointsLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream stream(&pointsLogFile);
        stream.setRealNumberNotation(QTextStream::SmartNotation);
        stream.setRealNumberPrecision(17);
        stream << "Largest Fill Region path points\n"
               << "region_index=" << biggestUnitIndex << '\n'
               << "source_indices=" << unitSourcesText(biggestUnit) << '\n'
               << "color=" << biggestUnit.color.name() << '\n'
               << "area=" << biggestUnit.area << '\n'
               << "original_pen_point_count="
               << biggestWork.contourStats.originalPointCount << '\n'
               << "optimized_pen_point_count="
               << biggestWork.contourStats.optimizedPointCount << '\n'
               << "removed_hard_point_count="
               << biggestWork.contourStats.removedHardPoints << '\n'
               << "removed_soft_point_count="
               << biggestWork.contourStats.removedSoftPoints << '\n'
               << "moved_hard_point_count="
               << biggestWork.contourStats.movedHardPoints << '\n'
               << "maximum_hard_point_movement="
               << biggestWork.contourStats.maximumHardPointMovement << '\n'
               << "area_error_ratio="
               << biggestWork.contourStats.areaErrorRatio << '\n'
               << "hard_only_retry="
               << (biggestWork.contourStats.softRunRetry ? "yes" : "no") << '\n'
               << "flattened_point_count="
               << biggestWork.contourStats.flattenedPointCount << '\n'
               << "dssim=" << biggestWork.contourStats.dssim << "\n\n";

        const auto elementTypeName = [](QPainterPath::ElementType type) {
            switch (type) {
            case QPainterPath::MoveToElement:
                return QStringLiteral("move");
            case QPainterPath::LineToElement:
                return QStringLiteral("line");
            case QPainterPath::CurveToElement:
                return QStringLiteral("curve-control-1");
            case QPainterPath::CurveToDataElement:
                return QStringLiteral("curve-data");
            }
            return QStringLiteral("unknown");
        };

        stream << "[source_qpainter_path]\n"
               << "count=" << biggestUnit.outline.elementCount() << '\n'
               << "index,type,x,y\n";
        for (int i = 0; i < biggestUnit.outline.elementCount(); ++i) {
            const QPainterPath::Element element = biggestUnit.outline.elementAt(i);
            stream << i << ',' << elementTypeName(element.type) << ','
                   << element.x << ',' << element.y << '\n';
        }

        stream << "\n[optimized_pen_points]\n"
               << "count=" << biggestWork.optimizedPenPoints.size() << '\n'
               << "index,kind,x,y\n";
        for (int i = 0; i < biggestWork.optimizedPenPoints.size(); ++i) {
            const PenPoint &point = biggestWork.optimizedPenPoints[i];
            stream << i << ','
                   << (point.kind == PenPointKind::Hard ? "hard" : "soft") << ','
                   << point.position.x() << ',' << point.position.y() << '\n';
        }

        stream << "\n[flattened_optimized_contour]\n"
               << "count=" << biggestWork.optimizedContour.size() << '\n'
               << "index,x,y\n";
        for (int i = 0; i < biggestWork.optimizedContour.size(); ++i) {
            const QPointF &point = biggestWork.optimizedContour[i];
            stream << i << ',' << point.x() << ',' << point.y() << '\n';
        }
        pointsLogFile.close();
        qWarning().noquote() << "Largest region points log written to" << pointsLogPath;
    } else {
        qWarning().noquote() << "Could not write largest region points log to"
                             << pointsLogPath;
    }

    QVector<RegionFillLayer> unitFills(units.size());
    for (int i = 0; i < units.size(); ++i) {
        const FillUnit &unit = units[i];
        UnitWorkResult &work = workResults[static_cast<size_t>(i)];
        const int unitCoreEllipseCount = static_cast<int>(std::count_if(
            work.fit.placements.cbegin(), work.fit.placements.cend(),
            [](const PenPlacement &placement) {
                return placement.coreEllipse;
            }));
        coreEllipseCount += unitCoreEllipseCount;
        if (work.timedOut) {
            ++timedOut;
        }
        if (work.meshFallback && fitSucceeded(work.fit)) {
            ++meshFallbacks;
            if (work.via == QStringLiteral("source-outline-mesh-fallback")) {
                ++sourceOutlineFallbacks;
            }
        }
        if (work.contourStats.softRunRetry && fitSucceeded(work.fit)) {
            ++softRunRetries;
        }
        if (work.contourStats.baselineRetry && fitSucceeded(work.fit)) {
            ++baselineRetries;
        }
        if (fitSucceeded(work.fit)) {
            if (work.via == QStringLiteral("safe-rdp-mesh")) {
                ++safeRdpFills;
            } else if (work.via == QStringLiteral("dangerous-rdp-mesh")) {
                ++dangerousRdpFills;
            }
        }
        if (fitSucceeded(work.fit)) {
            RegionFillLayer &layer = unitFills[i];
            layer.color = unit.color;
            layer.area = unit.area;
            layer.placements = std::move(work.fit.placements);
            layer.drawOrder = unit.drawOrder;
            layer.variant = unit.variant;
            placementCount += layer.placements.size();
            removedSoftPoints += work.contourStats.removedSoftPoints;
            ++filled;
        } else {
            ++failed;
            const QString reason = work.fit.error.isEmpty()
                ? QStringLiteral("empty result") : work.fit.error;
            failureReasons[reason] += 1;
        }
        QString detail;
        if (!work.penError.isEmpty()) {
            detail += QStringLiteral(" [Pen: %1]").arg(work.penError);
        }
        if (!work.cyclicRdpError.isEmpty()) {
            detail += QStringLiteral(" [cyclic RDP: %1]")
                          .arg(work.cyclicRdpError);
        }
        if (!work.fit.error.isEmpty() && work.fit.error != work.penError) {
            detail += QStringLiteral(" [error: %1]").arg(work.fit.error);
        }
        if (work.meshFallback && work.fallbackInputPoints > 0) {
            detail += QStringLiteral(" [mesh points: %1 -> %2]")
                          .arg(work.fallbackInputPoints)
                          .arg(work.fallbackMeshPoints);
        }
        if (work.cyclicRdpInputPoints > 0) {
            detail += QStringLiteral(" [cyclic RDP %1: %2 -> %3]")
                          .arg(kCyclicRdpEpsilon, 0, 'f', 1)
                          .arg(work.cyclicRdpInputPoints)
                          .arg(work.cyclicRdpOutputPoints);
        }
        if (unitCoreEllipseCount > 0) {
            detail += QStringLiteral(" [core ellipses: %1]").arg(unitCoreEllipseCount);
        }
        if (work.via == QStringLiteral("safe-rdp-mesh")
            || work.via == QStringLiteral("dangerous-rdp-mesh")) {
            detail += QStringLiteral(" [points: %1 Pen controls -> %2 polygon vertices, "
                                     "DSSIM not evaluated during fill]")
                          .arg(work.contourStats.originalPointCount)
                          .arg(work.contourStats.optimizedPointCount);
        } else {
            detail += QStringLiteral(
                          " [points: %1 -> %2, hard -%3, soft -%4, DSSIM %5]")
                          .arg(work.contourStats.originalPointCount)
                          .arg(work.contourStats.optimizedPointCount)
                          .arg(work.contourStats.removedHardPoints)
                          .arg(work.contourStats.removedSoftPoints)
                          .arg(work.contourStats.dssim, 0, 'g', 8);
        }
        const QString variantName = unit.variant == RegionFillVariant::Safe
            ? QStringLiteral("safe") : QStringLiteral("dangerous");
        log << QStringLiteral("%1 region #%2 (sources %3) %4 %5: "
                              "area=%6 -> %7 shapes, %8 ms%9")
                   .arg(variantName)
                   .arg(unit.drawOrder)
                   .arg(unitSourcesText(unit))
                   .arg(unit.color.name())
                   .arg(work.via)
                   .arg(unit.area, 0, 'f', 0)
                   .arg(unitFills[i].placements.size())
                   .arg(work.elapsedMs)
                   .arg(detail);
    }

    for (RegionFillLayer &layer : unitFills) {
        if (!layer.placements.isEmpty()) {
            fills.push_back(std::move(layer));
        }
    }
    sortRegionFillLayersByDrawOrder(&fills);
    const auto fillVariantRank = [](RegionFillVariant variant) {
        switch (variant) {
        case RegionFillVariant::Safe:
            return 0;
        case RegionFillVariant::Dangerous:
            return 1;
        }
        return 2;
    };
    const bool drawOrderPreserved = std::is_sorted(
        fills.cbegin(), fills.cend(),
        [fillVariantRank](const RegionFillLayer &left,
                          const RegionFillLayer &right) {
            if (left.variant != right.variant) {
                return fillVariantRank(left.variant) < fillVariantRank(right.variant);
            }
            return left.drawOrder < right.drawOrder;
        });
    log << QStringLiteral("Parallel draw order preserved: %1")
               .arg(drawOrderPreserved ? QStringLiteral("yes") : QStringLiteral("no"));
    const QImage safeRendered = renderRegionFillVariant(
        regionOverlay.imageSize, fills, silhouettes, RegionFillVariant::Safe);
    const QImage dangerousRendered = renderRegionFillVariant(
        regionOverlay.imageSize, fills, silhouettes, RegionFillVariant::Dangerous);
    result.differenceHeatmap = regionFillDifferenceHeatmap(
        safeRendered, dangerousRendered, &result.differencePixelCount);
    log << QStringLiteral("Safe/Dangerous heatmap difference pixels: %1")
               .arg(result.differencePixelCount);
    log << QStringLiteral("Summary: %1 planned units filled (%2 Safe and %3 Dangerous "
                          "cyclic-RDP meshes, %4 hard-only Pen retries, %5 baseline Pen "
                          "retries, %6 mesh fallbacks including %7 source-outline "
                          "recoveries), %8 shapes including %9 core ellipses, %10 soft "
                          "controls removed, %11 failed, %12 timed out")
               .arg(filled)
               .arg(safeRdpFills)
               .arg(dangerousRdpFills)
               .arg(softRunRetries)
               .arg(baselineRetries)
               .arg(meshFallbacks)
               .arg(sourceOutlineFallbacks)
               .arg(placementCount)
               .arg(coreEllipseCount)
               .arg(removedSoftPoints)
               .arg(failed)
               .arg(timedOut);
    const QString logPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/region_fill.log");
    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream stream(&logFile);
        stream << log.join(QLatin1Char('\n')) << '\n';
        logFile.close();
    }
    qWarning().noquote() << "Fill Regions log written to" << logPath;

    QString reasonSuffix;
    if (!failureReasons.isEmpty()) {
        QString topReason;
        int topCount = 0;
        for (auto it = failureReasons.constBegin(); it != failureReasons.constEnd(); ++it) {
            if (it.value() > topCount) {
                topCount = it.value();
                topReason = it.key();
            }
        }
        reasonSuffix = QStringLiteral(" - top reason: %1 (x%2)").arg(topReason).arg(topCount);
    }

    if (fills.isEmpty()) {
        result.error = QStringLiteral("No regions could be filled%1").arg(reasonSuffix);
        return result;
    }
    const int safeFillCount = static_cast<int>(std::count_if(
        fills.cbegin(), fills.cend(), [](const RegionFillLayer &fill) {
            return fill.variant == RegionFillVariant::Safe;
        }));
    const int dangerousFillCount = static_cast<int>(std::count_if(
        fills.cbegin(), fills.cend(), [](const RegionFillLayer &fill) {
            return fill.variant == RegionFillVariant::Dangerous;
        }));
    result.summary = QStringLiteral(
        "Filled %1 safe and %2 dangerous layer units "
        "with %3 shapes (%4 failed, %5 timed out)%6")
        .arg(safeFillCount)
        .arg(dangerousFillCount)
        .arg(placementCount)
        .arg(failed)
        .arg(timedOut)
        .arg(reasonSuffix);
    result.fills = std::move(fills);
    result.silhouettes = std::move(silhouettes);
    return result;
}

} // namespace gui
