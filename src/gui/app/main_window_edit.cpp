#include "main_window.h"

#include "main_window_internal.h"

#include "clipboard_buffer_widget.h"
#include "color_palette_widget.h"
#include "compact_fit_catalog.h"
#include "compact_fit.h"
#include "profile_fit.h"
#include "thin_fit.h"
#include "image_io.h"
#include "layer_tree_view.h"
#include "perf_utils.h"
#include "property_panel.h"
#include "cubic_contour.h"
#include "tool_settings_widget.h"
#include "shape_registry.h"
#include "shapes_browser_widget.h"
#include "matrix_math.h"

#include <QtGui>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

namespace gui {

using namespace mw_detail;

namespace {

bool usesWorkProgress(const QString &tool) {

    return tool == QStringLiteral("compact") || tool == QStringLiteral("lining");
}

int stripRasterLayers(std::vector<std::unique_ptr<fls::scene::Layer>> &nodes) {
    int removed = 0;
    for (auto it = nodes.begin(); it != nodes.end();) {
        fls::scene::Layer *node = it->get();
        if (node != nullptr && node->kind() == fls::scene::LayerKind::Shape
            && static_cast<fls::scene::Shape *>(node)->raster) {
            it = nodes.erase(it);
            ++removed;
            continue;
        }
        if (node != nullptr && node->kind() == fls::scene::LayerKind::Group) {
            removed += stripRasterLayers(static_cast<fls::scene::Group *>(node)->children);
        }
        ++it;
    }
    return removed;
}

fls::Matrix3 generatedShapeMatrix(const QTransform &transform) {
    fls::Matrix3 matrix;
    matrix.m[0][0] = transform.m11();
    matrix.m[0][1] = transform.m21();
    matrix.m[0][2] = transform.dx();
    matrix.m[1][0] = transform.m12();
    matrix.m[1][1] = transform.m22();
    matrix.m[1][2] = transform.dy();
    return matrix;
}

std::array<quint8, 4> colorBytes(const QColor &color) {
    return {
        static_cast<quint8>(color.blue()),
        static_cast<quint8>(color.green()),
        static_cast<quint8>(color.red()),
        static_cast<quint8>(color.alpha()),
    };
}

QString elapsedDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return {};
    }
    const qint64 rounded = static_cast<qint64>(std::floor(seconds));
    if (rounded < 60) {
        return QStringLiteral("%1s").arg(rounded);
    }
    if (rounded < 3600) {
        return QStringLiteral("%1m %2s")
            .arg(rounded / 60)
            .arg(rounded % 60, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1h %2m")
        .arg(rounded / 3600)
        .arg((rounded % 3600) / 60, 2, 10, QLatin1Char('0'));
}

QJsonObject writePenFillLog(const PenFillRequest &request,
                     const PenFillResult &result,
                     const QString &strategy) {
    QJsonArray loops;
    const QVector<PenLoop> requestLoops = request.loops.isEmpty()
        ? QVector<PenLoop>{{request.points, PenLoopKind::Outer}}
        : request.loops;
    for (const PenLoop &loop : requestLoops) {
        QJsonArray points;
        for (const PenPoint &point : loop.points) {
            QJsonObject pointObject;
            pointObject.insert(QStringLiteral("kind"),
                               point.kind == PenPointKind::Hard
                                   ? QStringLiteral("hard")
                                   : QStringLiteral("soft"));
            pointObject.insert(QStringLiteral("position"),
                               QJsonArray{point.position.x(), point.position.y()});
            pointObject.insert(QStringLiteral("incoming"),
                               QJsonArray{point.incoming.x(), point.incoming.y()});
            pointObject.insert(QStringLiteral("outgoing"),
                               QJsonArray{point.outgoing.x(), point.outgoing.y()});
            pointObject.insert(QStringLiteral("explicitHandles"), point.explicitHandles);
            pointObject.insert(QStringLiteral("strokeWidth"), point.strokeWidth);
            points.push_back(pointObject);
        }
        QJsonObject loopObject;
        loopObject.insert(QStringLiteral("kind"),
                          loop.kind == PenLoopKind::Outer
                              ? QStringLiteral("outer")
                              : QStringLiteral("cutout"));
        loopObject.insert(QStringLiteral("points"), points);
        loops.push_back(loopObject);
    }

    QJsonObject requestObject;
    requestObject.insert(QStringLiteral("curveModel"), QStringLiteral("cubic-anchors-v1"));
    requestObject.insert(QStringLiteral("strategy"), strategy);
    requestObject.insert(QStringLiteral("boundaryTolerance"), request.boundaryTolerance);
    requestObject.insert(QStringLiteral("discardNegligiblePlacements"),
                         request.discardNegligiblePlacements);
    requestObject.insert(QStringLiteral("loops"), loops);

    QJsonObject resultObject;
    resultObject.insert(QStringLiteral("shapeCount"), result.placements.size());
    resultObject.insert(QStringLiteral("shapeLimit"), result.shapeLimit);
    resultObject.insert(QStringLiteral("targetArea"), result.targetArea);
    resultObject.insert(QStringLiteral("coveredArea"), result.coveredArea);
    resultObject.insert(
        QStringLiteral("residualArea"),
        std::max(0.0, result.targetArea - result.coveredArea));
    resultObject.insert(QStringLiteral("outsideArea"), result.outsideArea);
    resultObject.insert(QStringLiteral("cancelled"), result.cancelled);
    resultObject.insert(QStringLiteral("timedOut"), result.timedOut);
    resultObject.insert(QStringLiteral("error"), result.error);
    resultObject.insert(QStringLiteral("retainedAfterError"),
        !result.cancelled && !result.error.isEmpty() && !result.placements.isEmpty());
    QJsonArray placements;
    for (const PenPlacement &placement :
         result.placements) {
        QJsonObject placementObject;
        placementObject.insert(
            QStringLiteral("shapeId"),
            placement.shapeId);
        placementObject.insert(
            QStringLiteral("coveredArea"),
            placement.area);
        placementObject.insert(
            QStringLiteral("transform"),
            QJsonArray{
                placement.transform.m11(),
                placement.transform.m12(),
                placement.transform.m21(),
                placement.transform.m22(),
                placement.transform.dx(),
                placement.transform.dy(),
            });
        placements.push_back(
            placementObject);
    }
    resultObject.insert(
        QStringLiteral("placements"),
        placements);

    QJsonObject root;
    root.insert(QStringLiteral("request"), requestObject);
    root.insert(QStringLiteral("result"), resultObject);

    const QString path = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("pen_fill.log"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        qWarning().noquote() << "Could not write Pen fill log to" << path;
        return root;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    qWarning().noquote() << "Pen fill log written to" << path;

    return root;
}

} // namespace

void MainWindow::startPenFill(const QVector<PenLoop> &loops,
                              const std::optional<QColor> &fillColor,
                              bool fillMask) {
    if (!ensureProjectForInsertion() || canvas_ == nullptr) {
        if (canvas_ != nullptr) {
            canvas_->setPenFillRunning(false);
        }
        return;
    }
    const ContourFillMode fillMode = loadBehaviorSettings().contourFillMode;
    const bool compactFit = fillMode == ContourFillMode::CompactFit;
    const QString fillLabel = compactFit ? QStringLiteral("Compact fit") : QStringLiteral("Analytic contour fill");
    const QString fillTool = compactFit ? QStringLiteral("compact") : QStringLiteral("analytic");
    compact::FillOptions compactOptions;
    compactOptions.retainFailedFill = true;
    if (compactFit && !fillMask) {
        bool accepted = false;
        compactOptions.boundaryAllowance = QInputDialog::getDouble(this,
            QStringLiteral("Compact Fit"),
            QStringLiteral("Outward allowance (world units, per axis):\nInward gap target: %1 world units. Total area error target: %2%.\nSmooth boundaries and sharp corners are checked separately.\nGenerated shapes are kept with a warning if checks fail.")
                .arg(compactOptions.inwardAllowance, 0, 'g', 3)
                .arg(compactOptions.areaErrorRatio * 100.0, 0, 'g', 3),
            compactOptions.boundaryAllowance, 0.01, 1000.0, 2, &accepted);
        if (!accepted) {
            canvas_->setPenFillRunning(false);
            return;
        }
    }
    prepareGeneratedFill(
        fillColor, fillLabel, fillTool,
        fillMask);
    if (fillColor.has_value()) {
        generatedFillGuideSelection_ = state_->selectedGuideLayerIds();
    }
    PenFillRequest request;
    request.loops = loops;
    if (compactFit) {
        QString catalogError;
        generatedFillColor_[3] = 255;
        const QVector<catalog::Primitive> primitives = canvas_->compactFillPrimitives(&catalogError);
        if (primitives.isEmpty() || fillMask) {
            canvas_->setPenFillRunning(false);
            clearGeneratedFillState();
            statusBar()->showMessage(fillMask
                ? QStringLiteral("%1 requires an opaque color fill").arg(fillLabel)
                : QStringLiteral("%1 failed: %2").arg(fillLabel, catalogError), 5000);
            return;
        }
        if (!contourLeewayEntryId_.isEmpty()) {
            QString leewayError;
            const QPainterPath leewayPath = canvas_->contourLeewayPath(&leewayError);
            if (leewayPath.isEmpty()) {
                canvas_->setPenFillRunning(false);
                clearGeneratedFillState();
                statusBar()->showMessage(
                    QStringLiteral("Compact Fit failed: %1").arg(leewayError), 5000);
                return;
            }
            compactOptions.leeway = leewayPath.toSubpathPolygons();
            generatedFillLeewayEntryId_ = contourLeewayEntryId_;
        }
        generatedFillProgress_->setRange(0, 0);
        generatedFillProgress_->setFormat(QStringLiteral("%1 | Preparing region").arg(fillLabel));
        generatedFillProgress_->show();
        canvas_->setPenFillRunning(true, QStringLiteral("Optimizing %1…").arg(fillLabel));
        statusBar()->showMessage(QStringLiteral("Optimizing %1… Press %2 to cancel")
            .arg(fillLabel, interactionShortcutText(KeyInteraction::CanvasCancelInteraction)));
        const QString strategy = fillTool + (fillColor.has_value()
            ? QStringLiteral("-bucket") : QStringLiteral("-pen"));
        startGeneratedFillTask([request = std::move(request), primitives, strategy, compactOptions](
            const std::function<bool()> &cancelled, const GeneratedFillProgress &progress) {
            auto options = compactOptions;
            QElapsedTimer timer;
            timer.start();
            options.workProgress = progress;
            catalog::FillResult result = profile::fillRegion(request, primitives, options, cancelled);
            const auto replay = writePenFillLog(request, result.fill, strategy);
            QFile log(QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("compact_fit.log")));
            result.diagnostics.insert(QStringLiteral("error"), result.fill.error);
            result.diagnostics.insert(QStringLiteral("cancelled"), result.fill.cancelled);
            result.diagnostics.insert(QStringLiteral("elapsedMilliseconds"), timer.elapsed());
            result.diagnostics.insert(QStringLiteral("request"), replay.value(QStringLiteral("request")));
            result.diagnostics.insert(QStringLiteral("result"), replay.value(QStringLiteral("result")));
            if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                log.write(QJsonDocument(result.diagnostics).toJson(QJsonDocument::Indented));
            }

            return result.fill;
        });
        return;
    }

    request.primitives = canvas_->penPrimitiveCatalog();
    if (request.primitives.isEmpty()) {
        canvas_->setPenFillRunning(false);
        clearGeneratedFillState();
        statusBar()->showMessage(QStringLiteral("Pen fill failed: Primitive geometry is unavailable"), 4000);
        return;
    }

    canvas_->setPenFillRunning(true, QStringLiteral("Running analytic contour fill…"));
    statusBar()->showMessage(QStringLiteral("Running analytic contour fill… Press %1 to cancel")
                                 .arg(interactionShortcutText(KeyInteraction::CanvasCancelInteraction)));

    const QString strategy = fillColor.has_value()
        ? QStringLiteral("analytic-bucket-hybrid-quadratic-rdp")
        : QStringLiteral("analytic-pen");
    startGeneratedFillTask([request = std::move(request), strategy](
                               const std::function<bool()> &cancelled,
                               const GeneratedFillProgress &) {
        PenFillResult result = fillPenPath(request, cancelled);
        writePenFillLog(request, result, strategy);

        return result;
    });
}

void MainWindow::startLiningFill(const QVector<PenPoint> &points,
                                 double width,
                                 const std::optional<QColor> &fillColor) {
    if (!ensureProjectForInsertion() || canvas_ == nullptr) {
        if (canvas_ != nullptr) {
            canvas_->setLiningFillRunning(false);
        }
        return;
    }
    prepareGeneratedFill(fillColor.has_value() && fillColor->isValid()
                             ? fillColor : std::optional<QColor>(fls::scene::kDefaultLiningFillColor),
                         QStringLiteral("Lining fill"), QStringLiteral("lining"));

    QString catalogError;
    const auto primitives = canvas_->thinFillPrimitives(&catalogError);
    if (primitives.isEmpty()) {
        canvas_->setLiningFillRunning(false);
        clearGeneratedFillState();
        statusBar()->showMessage(QStringLiteral("Lining fill failed: %1").arg(catalogError), 4000);
        return;
    }

    canvas_->setLiningFillRunning(true, QStringLiteral("Tracing lining path…"));
    statusBar()->showMessage(QStringLiteral("Tracing lining path… Press %1 to cancel")
                                 .arg(interactionShortcutText(KeyInteraction::CanvasCancelInteraction)));

    const double outsideMargin = loadBehaviorSettings().liningOutsideMargin;
    startGeneratedFillTask([points, width, primitives, outsideMargin](
                               const std::function<bool()> &cancelled,
                               const GeneratedFillProgress &progress) {
        auto options = thin::qualityOptions();
        options.liningOutsideMarginWidthFraction = outsideMargin;
        options.workProgress = progress;
        QElapsedTimer timer;
        timer.start();
        auto result = thin::fillLiningPath(points, width, primitives, options, cancelled);
        PenFillRequest request;
        request.points = points;
        const auto replay = writePenFillLog(request, result.fill, QStringLiteral("thin-lining"));
        result.diagnostics.insert(QStringLiteral("request"), replay.value(QStringLiteral("request")));
        result.diagnostics.insert(QStringLiteral("elapsedMilliseconds"), timer.elapsed());
        result.diagnostics.insert(QStringLiteral("error"), result.fill.error);
        QFile log(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("lining_fill.log")));
        if (log.open(QIODevice::WriteOnly | QIODevice::Truncate))
            log.write(QJsonDocument(result.diagnostics).toJson(QJsonDocument::Indented));

        return result.fill;
    });
}

void MainWindow::prepareGeneratedFill(const std::optional<QColor> &fillColor,
                                      const QString &label,
                                      const QString &tool,
                                      bool fillMask) {
    cancelActiveFills();
    updateLastSelectedShapeDefaults();
    generatedFillColor_ = fillColor.has_value() && fillColor->isValid()
        ? colorBytes(*fillColor)
        : (haveLastSelectedShapeDefaults_
               ? lastSelectedShapeColor_
               : std::array<quint8, 4>{255, 255, 255, 255});
    generatedFillInsertionEntries_ = selectedEntryIds();
    generatedFillLabel_ = label;
    generatedFillTool_ = tool;
    generatedFillMask_ = fillMask;
}

void MainWindow::startGeneratedFillTask(GeneratedFillFunction fill) {
    const quint64 generation = ++generatedFillGeneration_;
    const auto token = std::make_shared<std::atomic_bool>(false);
    generatedFillCancel_ = token;
    if (generatedFillProgress_ != nullptr) {
        generatedFillProgress_->setToolTip(generatedFillTool_ == QStringLiteral("compact")
            ? QStringLiteral("Percentage of the search score budget used, not a time estimate. Preparation and verification depend on contour complexity.")
            : QString());
    }
    if (usesWorkProgress(generatedFillTool_)) {
        generatedFillPlacementCount_ = 0;
        generatedFillWorkCompleted_ = 0;
        generatedFillWorkTotal_ = 0;
        generatedFillElapsed_.restart();
        generatedFillElapsedTimer_->start();
    }

    QPointer<MainWindow> guard(this);
    auto *task = QRunnable::create([guard, generation, fill = std::move(fill), token]() mutable {
        PenFillResult result = fill(
            [token]() {
                return token->load(std::memory_order_relaxed);
            },
            [guard, generation](int placementCount, int workCompleted, int workTotal) {
                queueMainWindowCallback(guard,
                    [generation, placementCount, workCompleted, workTotal](MainWindow &window) {
                        window.updateGeneratedFillProgress(
                            generation, placementCount, workCompleted, workTotal);
                    });
            });
        queueMainWindowCallback(guard,
            [generation, result = std::move(result)](MainWindow &window) mutable {
                window.finishGeneratedFill(generation, std::move(result));
            });
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void MainWindow::clearGeneratedFillState() {
    if (generatedFillElapsedTimer_ != nullptr) {
        generatedFillElapsedTimer_->stop();
    }
    generatedFillElapsed_.invalidate();
    if (generatedFillProgress_ != nullptr) {
        generatedFillProgress_->hide();
    }
    generatedFillPlacementCount_ = 0;
    generatedFillWorkCompleted_ = 0;
    generatedFillWorkTotal_ = 0;
    generatedFillInsertionEntries_.clear();
    generatedFillLabel_.clear();
    generatedFillGuideSelection_.clear();
    generatedFillTool_.clear();
    generatedFillLeewayEntryId_.clear();
    generatedFillMask_ = false;
}

void MainWindow::cancelActiveFills() {
    cancelGeneratedFill();
    cancelRegionFill();
}

void MainWindow::cancelGeneratedFill() {
    if (generatedFillCancel_ == nullptr) {
        return;
    }
    generatedFillCancel_->store(true, std::memory_order_relaxed);
    generatedFillCancel_.reset();
    ++generatedFillGeneration_;
    if (canvas_ != nullptr) {
        canvas_->setPenFillRunning(false);
        canvas_->setLiningFillRunning(false);
    }
    statusBar()->showMessage(QStringLiteral("%1 cancelled").arg(generatedFillLabel_), 1500);
    clearGeneratedFillState();
}

void MainWindow::refreshGeneratedFillElapsedTime() {
    if (generatedFillCancel_ == nullptr
        || !usesWorkProgress(generatedFillTool_)
        || generatedFillProgress_ == nullptr
        || !generatedFillElapsed_.isValid()) {
        return;
    }

    const QString duration = elapsedDuration(static_cast<double>(generatedFillElapsed_.elapsed()) / 1000.0);
    constexpr int kProgressResolution = 1000;
    const double fraction = generatedFillWorkTotal_ > 0
        ? std::clamp(static_cast<double>(generatedFillWorkCompleted_) / generatedFillWorkTotal_, 0.0, 1.0) : 0.0;
    generatedFillProgress_->setRange(0, generatedFillWorkTotal_ > 0 ? kProgressResolution : 0);
    generatedFillProgress_->setValue(static_cast<int>(fraction * kProgressResolution));
    generatedFillProgress_->setFormat(generatedFillWorkTotal_ > 0
        ? QStringLiteral("%1 | Work %2% | Elapsed %3 | %4 shapes")
            .arg(generatedFillLabel_).arg(fraction * 100.0, 0, 'f', 1).arg(duration).arg(generatedFillPlacementCount_)
        : QStringLiteral("%1 | Elapsed %2 | %3 shapes")
            .arg(generatedFillLabel_, duration).arg(generatedFillPlacementCount_));
    generatedFillProgress_->show();
}

void MainWindow::updateGeneratedFillProgress(quint64 generation,
                                             int placementCount,
                                             int workCompleted,
                                             int workTotal) {
    if (generation != generatedFillGeneration_
        || generatedFillCancel_ == nullptr
        || !usesWorkProgress(generatedFillTool_)
        || generatedFillProgress_ == nullptr
        || workCompleted < 0 || workTotal < 0) {
        return;
    }

    generatedFillPlacementCount_ = placementCount;
    generatedFillWorkTotal_ = workTotal;
    generatedFillWorkCompleted_ = workCompleted;
    refreshGeneratedFillElapsedTime();
}

void MainWindow::finishGeneratedFill(quint64 generation, PenFillResult result) {
    if (generation != generatedFillGeneration_ || generatedFillCancel_ == nullptr) {
        return;
    }
    generatedFillCancel_.reset();
    if (canvas_ != nullptr) {
        if (generatedFillTool_ == QStringLiteral("lining")) {
            canvas_->setLiningFillRunning(false);
        } else {
            canvas_->setPenFillRunning(false);
        }
    }
    if (result.cancelled) {
        statusBar()->showMessage(QStringLiteral("%1 cancelled").arg(generatedFillLabel_), 1500);
        clearGeneratedFillState();
        return;
    }
    const bool lining = generatedFillTool_ == QStringLiteral("lining");
    if ((lining && !result.error.isEmpty()) || result.placements.isEmpty() || !state_->hasProject()) {
        statusBar()->showMessage(QStringLiteral("%1 failed: %2")
                                     .arg(generatedFillLabel_)
                                     .arg(result.error.isEmpty() ? QStringLiteral("no shapes generated") : result.error),
                                 5000);
        clearGeneratedFillState();
        return;
    }

    QVector<QPair<int, QTransform>> placements;
    placements.reserve(result.placements.size());
    for (const PenPlacement &placement : result.placements) {
        placements.push_back({placement.shapeId, placement.transform});
    }
    const bool compactFit = generatedFillTool_ == QStringLiteral("compact");
    const QString groupName = lining
        ? QStringLiteral("Lining")
        : (compactFit ? QStringLiteral("Compact Fit") : QStringLiteral("Analytic Contour Fill"));
    insertGeneratedFill(groupName, groupName, placements);
    if (compactFit) {
        statusBar()->showMessage(QStringLiteral("Created Compact Fit with %1 shapes; missing %2, spill %3 world²")
            .arg(placements.size()).arg(result.targetArea - result.coveredArea, 0, 'g', 6)
            .arg(result.outsideArea, 0, 'g', 6), 8000);
    }
    if (!result.error.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Created %1 with %2 shapes. Warning: %3")
            .arg(groupName).arg(placements.size()).arg(result.error), 15000);
    }
    if (canvas_ != nullptr) {
        if (lining) {
            canvas_->cancelLiningInteraction();
        } else {
            canvas_->cancelPenInteraction();
        }
    }
    clearGeneratedFillState();
}

void MainWindow::insertGeneratedFill(const QString &groupName,
                                     const QString &displayName,
                                     const QVector<QPair<int, QTransform>> &placements) {
    if (placements.isEmpty() || !state_->hasProject()) {
        generatedFillInsertionEntries_.clear();
        return;
    }

    auto group = std::make_unique<fls::scene::Group>();
    group->id = state_->uniqueGroupId();
    const QString groupId = group->id;
    group->name = groupName;
    QSet<QString> generatedIds;
    generatedIds.reserve(placements.size());
    for (const auto &placement : placements) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->id = QStringLiteral("layer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        shape->name = fls::detail::shapeName(static_cast<quint16>(placement.first));
        shape->setVectorShape(static_cast<quint16>(placement.first));
        shape->transform = fls::decomposeTransform2D(generatedShapeMatrix(placement.second));
        shape->color = generatedFillColor_;
        shape->mask = generatedFillMask_;
        generatedIds.insert(shape->id);
        group->append(std::move(shape));
    }

    QString insertedEntryId = groupId;
    std::unique_ptr<fls::scene::Layer> insertedNode = std::move(group);
    if (placements.size() == 1) {
        auto &insertedGroup = static_cast<fls::scene::Group &>(*insertedNode);
        insertedNode = insertedGroup.takeAt(0);
        insertedEntryId = insertedNode->id;
    }

    state_->beginProjectEdit();
    const bool insertedBelowLeeway =
        !generatedFillLeewayEntryId_.isEmpty()
        && state_->insertLayerBelowEntry(
            insertedNode,
            generatedFillLeewayEntryId_);
    if (!insertedBelowLeeway && insertedNode != nullptr) {
        state_->insertLayerAboveSelection(
            std::move(insertedNode),
            generatedFillInsertionEntries_);
    }
    if (fls::scene::Layer *inserted = state_->sceneNode(insertedEntryId); inserted != nullptr) {
        const QString parentId = state_->parentGroupForEntry(insertedEntryId);
        if (const fls::scene::Group *parent = state_->groupForId(parentId); parent != nullptr) {
            const fls::Matrix3 parentInverse = fls::invertAffine(parent->worldMatrix());
            if (inserted->kind() == fls::scene::LayerKind::Group) {
                for (const auto &child : static_cast<fls::scene::Group &>(*inserted).children) {
                    child->transform = fls::decomposeTransform2D(
                        fls::detail::multiply(parentInverse, child->transform.matrix()));
                }
            } else {
                inserted->transform = fls::decomposeTransform2D(
                    fls::detail::multiply(parentInverse, inserted->transform.matrix()));
            }
        }
    }
    state_->selectedGuideLayerIds_ =
        state_->existingGuideLayerIds(generatedFillGuideSelection_);
    state_->selectedLayerIds_ = state_->selectedGuideLayerIds_.isEmpty()
        ? generatedIds : QSet<QString>{};
    state_->selectedEntryIds_.clear();
    state_->commitProjectEdit();
    generatedFillInsertionEntries_.clear();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Created %1 with %2 shapes")
                                 .arg(displayName)
                                 .arg(placements.size()),
                             3500);
    clearContourLeewayEntry();
}

void MainWindow::toggleContourLeewayEntry(
    const QString &entryId) {
    if (canvas_ == nullptr
        || loadBehaviorSettings().contourFillMode != ContourFillMode::CompactFit) {
        statusBar()->showMessage(
            QStringLiteral("Contour leeway requires Compact Fit Contour Fill"),
            3000);
        return;
    }
    if (!canvas_->canAssignContourLeeway()) {
        statusBar()->showMessage(
            QStringLiteral("Close the Pen contour before assigning leeway"),
            3000);
        return;
    }
    const QString nextEntryId =
        contourLeewayEntryId_ == entryId
        ? QString() : entryId;
    const QString previousEntryId = contourLeewayEntryId_;
    canvas_->setContourLeewayEntryId(nextEntryId);
    if (!nextEntryId.isEmpty()) {
        QString error;
        if (canvas_->contourLeewayPath(&error).isEmpty()) {
            canvas_->setContourLeewayEntryId(previousEntryId);
            statusBar()->showMessage(error, 4000);
            return;
        }
    }
    contourLeewayEntryId_ = nextEntryId;
    tree_->setLeewayEntryId(nextEntryId);
    const fls::scene::Layer *entry =
        state_->sceneNode(nextEntryId);
    statusBar()->showMessage(
        nextEntryId.isEmpty()
        ? QStringLiteral("Contour leeway cleared")
        : QStringLiteral("Contour leeway: %1; fill will be placed below it")
              .arg(entry != nullptr ? entry->name : nextEntryId),
        3500);
}

void MainWindow::clearContourLeewayEntry() {
    if (contourLeewayEntryId_.isEmpty()) {
        return;
    }
    contourLeewayEntryId_.clear();
    if (canvas_ != nullptr) {
        canvas_->setContourLeewayEntryId({});
    }
    if (tree_ != nullptr) {
        tree_->setLeewayEntryId({});
    }
}

void MainWindow::insertGeneratedRegionVariants(
    const QString &groupName,
    const QString &displayName,
    const QVector<GeneratedRegionVariant> &variants,
    const QVector<QString> &insertionEntries,
    const QImage &differenceHeatmap,
    const QString &sourceGuideId) {
    if (variants.isEmpty() || !state_->hasProject()) {
        return;
    }

    auto group = std::make_unique<fls::scene::Group>();
    group->id = state_->uniqueGroupId();
    const QString groupId = group->id;
    group->name = groupName;
    QSet<QString> generatedIds;
    int shapeCount = 0;
    int regionCount = 0;
    for (const GeneratedRegionVariant &variant : variants) {
        regionCount += variant.regions.size();
        for (const GeneratedRegionGroup &region : variant.regions) {
            shapeCount += region.shapes.size();
        }
    }
    if (shapeCount == 0) {
        return;
    }
    std::unique_ptr<fls::scene::GuideLayer> differenceGuide;
    QString differenceGuideParentId;
    if (!differenceHeatmap.isNull()) {
        const fls::scene::Layer *sourceNode = state_->sceneNode(sourceGuideId);
        if (sourceNode != nullptr
            && sourceNode->kind() == fls::scene::LayerKind::Guide) {
            const auto *sourceGuide =
                static_cast<const fls::scene::GuideLayer *>(sourceNode);
            const QImage storedHeatmap = differenceHeatmap.convertToFormat(
                QImage::Format_ARGB32_Premultiplied);
            QString heatmapFormat;
            const QByteArray encodedHeatmap =
                encodeGuideImage(storedHeatmap, &heatmapFormat);
            if (!encodedHeatmap.isEmpty()) {
                differenceGuide = std::make_unique<fls::scene::GuideLayer>();
                differenceGuide->id = state_->uniqueGuideLayerId();
                differenceGuide->name = QStringLiteral("Dangerous Differences");
                differenceGuide->transform = sourceGuide->transform;
                differenceGuide->opacity = 1.0;
                differenceGuide->image =
                    std::make_unique<fls::scene::RasterContainer>();
                differenceGuide->image->encoded = encodedHeatmap;
                differenceGuide->image->pixels = QByteArray(
                    reinterpret_cast<const char *>(storedHeatmap.constBits()),
                    storedHeatmap.sizeInBytes());
                differenceGuide->image->format = heatmapFormat;
                differenceGuide->image->width = storedHeatmap.width();
                differenceGuide->image->height = storedHeatmap.height();
                differenceGuideParentId =
                    state_->parentGroupForEntry(sourceGuideId);
            }
        }
    }
    const bool differenceGuideCreated = differenceGuide != nullptr;
    generatedIds.reserve(shapeCount);
    for (const GeneratedRegionVariant &variant : variants) {
        auto variantGroup = std::make_unique<fls::scene::Group>();
        variantGroup->id = QStringLiteral("group_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        variantGroup->name = variant.name;
        variantGroup->visible = variant.visible;
        int variantRegionCount = 0;
        for (const GeneratedRegionGroup &region : variant.regions) {
            if (region.shapes.isEmpty()) {
                continue;
            }
            auto regionGroup = std::make_unique<fls::scene::Group>();
            regionGroup->id = QStringLiteral("group_%1").arg(
                QUuid::createUuid().toString(QUuid::WithoutBraces));
            regionGroup->name = QStringLiteral("Region %1").arg(++variantRegionCount);
            for (const GeneratedRegionShape &placement : region.shapes) {
                auto shape = std::make_unique<fls::scene::Shape>();
                shape->id = QStringLiteral("layer_%1").arg(
                    QUuid::createUuid().toString(QUuid::WithoutBraces));
                shape->name = fls::detail::shapeName(
                    static_cast<quint16>(placement.shapeId));
                shape->setVectorShape(static_cast<quint16>(placement.shapeId));
                shape->transform = fls::decomposeTransform2D(
                    generatedShapeMatrix(placement.transform));
                shape->color = {placement.color[0], placement.color[1],
                                placement.color[2], placement.color[3]};
                if (variant.visible) {
                    generatedIds.insert(shape->id);
                }
                regionGroup->append(std::move(shape));
            }
            variantGroup->append(std::move(regionGroup));
        }
        group->append(std::move(variantGroup));
    }

    state_->beginProjectEdit();
    state_->insertLayerAboveSelection(std::move(group), insertionEntries);
    if (fls::scene::Group *inserted = state_->groupForId(groupId); inserted != nullptr) {
        const QString parentId = state_->parentGroupForEntry(groupId);
        if (const fls::scene::Group *parent = state_->groupForId(parentId); parent != nullptr) {
            const fls::Matrix3 parentInverse = fls::invertAffine(parent->worldMatrix());
            for (const auto &variantNode : inserted->children) {
                if (variantNode->kind() != fls::scene::LayerKind::Group) {
                    continue;
                }
                auto *variantGroup = static_cast<fls::scene::Group *>(variantNode.get());
                for (const auto &regionNode : variantGroup->children) {
                    if (regionNode->kind() != fls::scene::LayerKind::Group) {
                        continue;
                    }
                    auto *regionGroup = static_cast<fls::scene::Group *>(regionNode.get());
                    for (const auto &shape : regionGroup->children) {
                        shape->transform = fls::decomposeTransform2D(
                            fls::detail::multiply(parentInverse,
                                                  shape->transform.matrix()));
                    }
                }
            }
        }
    }
    if (differenceGuide != nullptr) {
        fls::scene::Group *differenceParent = differenceGuideParentId.isEmpty()
            ? state_->project_.root.get()
            : state_->groupForId(differenceGuideParentId);
        if (differenceParent == nullptr) {
            differenceParent = state_->project_.root.get();
        }
        differenceParent->append(std::move(differenceGuide));
    }
    state_->selectedLayerIds_ = generatedIds;
    state_->selectedGuideLayerIds_.clear();
    state_->selectedEntryIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    const QString differenceText = differenceGuideCreated
        ? QStringLiteral(", and a difference heatmap") : QString();
    statusBar()->showMessage(QStringLiteral(
                                 "Created %1 with %2 variants, %3 region groups, %4 shapes%5")
                                 .arg(displayName)
                                 .arg(variants.size())
                                 .arg(regionCount)
                                 .arg(shapeCount)
                                 .arg(differenceText),
                             3500);
}

fls::Project *MainWindow::project() {
    return state_->project();
}

QVector<fls::scene::Group *> MainWindow::selectedGroups() {
    return state_->selectedGroups(state_->fullySelectedTopGroupIds());
}

void MainWindow::refreshSelectionProperties() {
    if (properties_ == nullptr) {
        return;
    }
    ScopedPerf perf("refreshSelectionProperties");
    properties_->setSelection(state_->selectedLayers(), state_->selectedGuideLayers(), selectedGroups(), state_->selectedContours());
    refreshPropertyBoxFieldsFromCanvas();
}

void MainWindow::refreshPropertyBoxFieldsFromCanvas() {
    if (properties_ == nullptr || canvas_ == nullptr) {
        return;
    }
    QPointF center;
    double width = 0.0;
    double height = 0.0;
    QTransform boxFrame;
    const bool valid = canvas_->currentTransformBox(&center, &width, &height, &boxFrame);
    properties_->refreshTransformFieldsFromBox(center, width, height, boxFrame, valid);
}

void MainWindow::deleteSelectedLayers() {
    executeSelectionCommand(EditorCommandType::Remove);
}

void MainWindow::copySelection() {
    if (copySelectionToClipboard()) {
        statusBar()->showMessage(QStringLiteral("Copied selection"), 1500);
    }
}

void MainWindow::cutSelection() {
    executeSelectionCommand(EditorCommandType::Cut);
}

void MainWindow::executeSelectionCommand(EditorCommandType type, bool flatten) {
    EditorCommand command;
    command.type = type;
    command.entries = state_->selectionEntries();
    command.flatten = flatten;
    QString error;
    if (!state_->executeCommand(command, &error)) {
        statusBar()->showMessage(error, 3000);
    }
}

void MainWindow::pasteClipboard() {
    if (!state_->hasProject() || state_->clipboard() == nullptr) {
        statusBar()->showMessage(QStringLiteral("Clipboard is empty"), 2500);
        return;
    }

    ProjectClipboard pasteBuffer = *state_->clipboard();
    int strippedLogos = 0;
    if (!state_->project_.isLivery) {
        strippedLogos = stripRasterLayers(pasteBuffer.nodes);
    }
    if (pasteBuffer.nodes.empty()) {
        statusBar()->showMessage(
            QStringLiteral("Logos can only be used in livery projects; nothing to paste"), 3000);
        return;
    }

    const bool pasteInPlace = loadBehaviorSettings().pasteInPlace;
    const QPointF pasteCenter = canvas_ == nullptr || pasteInPlace
        ? QPointF() : canvas_->viewCenterWorld();
    state_->beginProjectEdit();
    EditorCommand command;
    command.type = EditorCommandType::Paste;
    command.entries = state_->selectionEntries();
    command.clipboard = pasteBuffer;
    if (!state_->executeCommand(command)) {
        state_->cancelProjectEdit();
        return;
    }
    if (canvas_ != nullptr && !pasteInPlace) {
        canvas_->invalidateSelectionCache();
        const QRectF pastedBounds = canvas_->selectionWorldBounds();
        if (pastedBounds.isValid() && !pastedBounds.isEmpty()) {
            const QPointF delta = pasteCenter - pastedBounds.center();
            EditorCommand transform;
            transform.type = EditorCommandType::WorldTransform;
            transform.entries = state_->selectionEntries();
            transform.worldTransform = QTransform::fromTranslate(delta.x(), delta.y());
            state_->executeCommand(transform);
        }
    }
    state_->commitProjectEdit();
    if (strippedLogos > 0) {
        statusBar()->showMessage(
            QStringLiteral("Pasted selection (%1 logo%2 removed — not allowed in Group projects)")
                .arg(strippedLogos)
                .arg(strippedLogos == 1 ? QString() : QStringLiteral("s")),
            3000);
    } else {
        statusBar()->showMessage(QStringLiteral("Pasted selection"), 1500);
    }
}

void MainWindow::stampSelection() {
    executeSelectionCommand(EditorCommandType::Duplicate);
}

void MainWindow::sampleGuideColorToSelection() {
    if (properties_ == nullptr || !properties_->sampleGuideColorToSelection()) {
        statusBar()->showMessage(QStringLiteral("No guide color under cursor or no colorable selection"), 2500);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Picked guide color"), 1500);
}

void MainWindow::insertShape(int shapeId) {
    if (!ensureProjectForInsertion()) {
        return;
    }

    auto layer = std::make_unique<fls::scene::Shape>();
    layer->id = state_->uniqueLayerId();
    layer->name = fls::detail::shapeName(static_cast<quint16>(shapeId));
    layer->setVectorShape(static_cast<quint16>(shapeId));
    insertShapeLayer(std::move(layer));
}

void MainWindow::replaceSelectedShape(int shapeId) {
    const QVector<fls::scene::Shape *> selected = state_->selectedLayers();
    if (selected.size() != 1 || !state_->selectedGuideLayerIds().isEmpty()
        || selected.front() == nullptr) {
        statusBar()->showMessage(QStringLiteral("Select one shape to replace"), 2000);
        return;
    }

    fls::scene::Shape *layer = selected.front();
    if (!layer->isRaster() && layer->shapeId == shapeId) {
        statusBar()->showMessage(QStringLiteral("Selected shape already uses this geometry"), 1500);
        return;
    }

    state_->beginProjectEdit();
    layer->setVectorShape(static_cast<quint16>(shapeId));
    layer->sourceLogoId = 0;
    state_->commitProjectEdit();
    state_->noteProjectGeometryChanged(true, {layer->id});
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Replaced selected shape"), 1500);
}

void MainWindow::insertLogo(quint32 rasterId, int width, int height) {
    if (!ensureProjectForInsertion()) {
        return;
    }

    auto layer = std::make_unique<fls::scene::Shape>();
    layer->id = state_->uniqueLayerId();
    layer->name = QStringLiteral("Logo %1").arg(rasterId);
    layer->setRasterShape(rasterId, width, height);
    layer->sourceLogoId = static_cast<quint16>(0x8000u | (rasterId & 0x7fffu));
    insertShapeLayer(std::move(layer));
}

void MainWindow::insertShapeLayer(std::unique_ptr<fls::scene::Shape> layer) {
    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();

    layer->x = center.x();
    layer->y = center.y();
    updateLastSelectedShapeDefaults();
    const BehaviorSettings behavior = loadBehaviorSettings();
    if (behavior.insertShapeWithLastSelectedColor && haveLastSelectedShapeDefaults_) {
        layer->color = lastSelectedShapeColor_;
    }
    if (behavior.insertShapeWithLastSelectedScale && haveLastSelectedShapeDefaults_) {
        layer->scaleX = lastSelectedShapeScaleX_;
        layer->scaleY = lastSelectedShapeScaleY_;
    }

    state_->beginProjectEdit();
    const QString insertedId = layer->id;
    const QString insertedName = layer->name;
    state_->insertLayerAboveSelection(std::move(layer), selectedEntryIds());
    state_->selectedLayerIds_ = {insertedId};
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Inserted %1").arg(insertedName), 1500);
}

void MainWindow::placeTextDialog() {
    if (!ensureProjectForInsertion()) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Place Text"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *fontCombo = new QComboBox(&dialog);
    fontCombo->addItems(gui::fontglyphs::fontNames());
    const QString lastFont = QSettings().value(QStringLiteral("placeText/font")).toString();
    const int lastFontIndex = fontCombo->findText(lastFont);
    if (lastFontIndex >= 0) {
        fontCombo->setCurrentIndex(lastFontIndex);
    }
    auto *textEdit = new QLineEdit(&dialog);
    textEdit->setPlaceholderText(QStringLiteral("Type text to place..."));
    auto *monoCheck = new QCheckBox(QStringLiteral("Monospace"), &dialog);
    monoCheck->setToolTip(QStringLiteral(
        "Advance every glyph by a fixed cell (the font's average glyph width, "
        "computed separately for upper- and lower-case) instead of each glyph's "
        "own width."));
    monoCheck->setChecked(QSettings().value(QStringLiteral("placeText/monospace"), false).toBool());
    form->addRow(QStringLiteral("Font"), fontCombo);
    form->addRow(QStringLiteral("Text"), textEdit);
    form->addRow(QString(), monoCheck);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    textEdit->setFocus();

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString fontName = fontCombo->currentText();
    const QString text = textEdit->text();
    const bool monospace = monoCheck->isChecked();
    if (text.trimmed().isEmpty()) {
        return;
    }
    QSettings().setValue(QStringLiteral("placeText/font"), fontName);
    QSettings().setValue(QStringLiteral("placeText/monospace"), monospace);

    const PlacedTextLine line = layoutTextGlyphs(fontName, text, monospace, [this](int shapeId) {
        return canvas_ != nullptr ? canvas_->shapeInkBounds(shapeId)
                                  : QRectF(-64.0, -64.0, 128.0, 128.0);
    });

    if (line.glyphs.isEmpty()) {
        QMessageBox::information(this,
                                QStringLiteral("Place Text"),
                                QStringLiteral("None of the typed characters have a glyph in %1.").arg(fontName));
        return;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    const double startX = center.x() - line.width * 0.5;

    state_->beginProjectEdit();
    QVector<QString> newIds;
    newIds.reserve(line.glyphs.size());
    for (const PlacedTextGlyph &glyph : line.glyphs) {
        auto layer = std::make_unique<fls::scene::Shape>();
        layer->id = state_->uniqueLayerId();
        layer->name = fls::detail::shapeName(static_cast<quint16>(glyph.shapeId));
        layer->setVectorShape(static_cast<quint16>(glyph.shapeId));
        layer->x = startX + glyph.originX;
        layer->y = center.y();
        const QString id = layer->id;
        state_->insertLayerAboveSelection(std::move(layer), {});
        newIds.push_back(id);
    }

    QString groupName = text.trimmed();
    if (newIds.size() > 1) {
        state_->groupEntries(newIds);
        const QString parent = state_->parentGroupForEntry(newIds.front());
        if (fls::scene::Group *group = state_->groupForId(parent); group != nullptr) {
            group->name = groupName;
        }
    } else {
        state_->selectedLayerIds_ = {newIds.front()};
        state_->selectedGuideLayerIds_.clear();
    }
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }

    if (!line.skipped.isEmpty()) {
        QMessageBox::information(this,
                                QStringLiteral("Place Text"),
                                QStringLiteral("Placed \"%1\" in %2.\n\nSkipped characters with no glyph in this font: %3")
                                    .arg(groupName, fontName, line.skipped));
    } else {
        statusBar()->showMessage(QStringLiteral("Placed \"%1\" (%2)").arg(groupName, fontName), 2000);
    }
}

void MainWindow::saveCurrentSelectionAsCustomGroup() {
    if (!state_->hasProject()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("Open a project before saving a custom group."));
        return;
    }
    QVector<QString> entries;
    for (const QString &id : selectedEntryIds()) {
        if (!state_->entryIsGuide(id)) {
            entries.push_back(id);
        }
    }
    entries = state_->normalizeEntrySelection(entries);
    if (entries.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("Select one or more shape layers or groups first."));
        return;
    }

    ProjectClipboard clipboard;
    if (!state_->buildEntryClipboard(entries, clipboard) || clipboard.nodes.empty()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("The selected layers cannot be saved as a custom group."));
        return;
    }

    QString defaultName = entryNameForId(state_->project_, entries.front()).trimmed();
    if (defaultName.isEmpty()) {
        defaultName = QStringLiteral("Custom Group");
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Add Custom Group"),
                                               QStringLiteral("Name"),
                                               QLineEdit::Normal,
                                               defaultName,
                                               &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    shapesBrowser_->addCustomGroup(name, clipboard);
    statusBar()->showMessage(QStringLiteral("Saved custom group %1").arg(name), 2000);
}

void MainWindow::insertCustomGroup(const QString &name, const ProjectClipboard &clipboard) {
    if (!ensureProjectForInsertion()) {
        return;
    }
    if (clipboard.nodes.empty()) {
        return;
    }
    ProjectClipboard placement;
    if (clipboard.nodes.size() == 1 && clipboard.nodes.front()
        && clipboard.nodes.front()->kind() == fls::scene::LayerKind::Group) {
        std::unique_ptr<fls::scene::Layer> root = clipboard.nodes.front()->clone();
        root->name = name;
        placement.rootIds = {root->id};
        placement.nodes.push_back(std::move(root));
    } else {
        auto root = std::make_unique<fls::scene::Group>();
        root->id = QStringLiteral("custom_group_root");
        root->name = name;
        for (const auto &node : clipboard.nodes) {
            if (node) {
                root->append(node->clone());
            }
        }
        placement.rootIds = {root->id};
        placement.nodes.push_back(std::move(root));
    }

    QSet<QString> layerSelection;
    QSet<QString> guideSelection;
    QVector<QString> insertedRoots;
    state_->beginProjectEdit();
    state_->insertClipboardAboveSelection(
        placement, selectedEntryIds(), &layerSelection, &guideSelection, false, &insertedRoots);
    state_->selectedLayerIds_ = layerSelection;
    state_->selectedGuideLayerIds_ = guideSelection;
    state_->selectedEntryIds_.clear();
    QRectF insertedBounds;
    bool hasInsertedBounds = false;
    for (const QString &id : layerSelection) {
        auto *layer = dynamic_cast<fls::scene::Shape *>(state_->sceneNode(id));
        if (layer == nullptr) {
            continue;
        }
        const QSizeF size = canvas_ == nullptr ? QSizeF(128.0, 128.0) : canvas_->shapeSize(layer->shapeId);
        const QRectF bounds = shapeWorldBounds(*layer, size);
        insertedBounds = hasInsertedBounds ? insertedBounds.united(bounds) : bounds;
        hasInsertedBounds = true;
    }
    if (hasInsertedBounds) {
        const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
        const QPointF delta = center - insertedBounds.center();
        if (!insertedRoots.isEmpty()) {
            state_->transformGroupFrames(insertedRoots, QTransform::fromTranslate(delta.x(), delta.y()));
        }
    }
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Inserted custom group"), 1500);
}

bool MainWindow::importGuideLayer(const QString &path, QString *error) {
    if (!ensureProjectForInsertion()) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    QString decodeError;
    QByteArray decodedFormat;
    const QImage image = readGuideImage(path, &decodedFormat, &decodeError);
    if (image.isNull()) {
        if (error != nullptr) {
            *error = decodeError.isEmpty()
                ? QStringLiteral("could not decode image: %1").arg(path)
                : decodeError;
        }
        return false;
    }

    QString embedFormat;
    QProgressDialog progress(decodedFormat == QByteArrayLiteral("svg")
                                 ? QStringLiteral("Preparing SVG guide for project storage...")
                                 : QStringLiteral("Converting guide image for project storage..."),
                             QString(),
                             0,
                             0,
                             this);
    progress.setWindowTitle(QStringLiteral("Converting Image"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    progress.show();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QByteArray embedBytes;
    if (decodedFormat == QByteArrayLiteral("svg")) {
        QFile source(path);
        if (source.open(QIODevice::ReadOnly)) {
            embedBytes = source.readAll();
            embedFormat = QStringLiteral("svg");
        }
    } else {
        embedBytes = encodeGuideImage(image, &embedFormat);
    }
    progress.close();
    if (embedBytes.isEmpty()) {
        if (error != nullptr) {
            *error = decodedFormat == QByteArrayLiteral("svg")
                ? QStringLiteral("could not read SVG guide image: %1").arg(path)
                : QStringLiteral("could not encode guide image: %1").arg(path);
        }
        return false;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    auto guide = std::make_unique<fls::scene::GuideLayer>();
    guide->id = state_->uniqueGuideLayerId();
    guide->name = QFileInfo(path).completeBaseName();
    if (guide->name.isEmpty()) {
        guide->name = QStringLiteral("Guide");
    }
    guide->sourcePath = QFileInfo(path).absoluteFilePath();
    guide->image = std::make_unique<fls::scene::RasterContainer>();
    guide->image->encoded = embedBytes;
    if (decodedFormat != QByteArrayLiteral("svg")) {
        guide->image->pixels = QByteArray(reinterpret_cast<const char *>(image.constBits()),
                                          image.sizeInBytes());
    }
    guide->image->format = embedFormat;
    guide->image->width = image.width();
    guide->image->height = image.height();
    guide->x = center.x();
    guide->y = center.y();
    guide->opacity = 0.5;

    state_->beginProjectEdit();
    const QString guideId = guide->id;
    const QString guideName = guide->name;
    const QString sectionId = state_->project_.isLivery ? activeLiverySectionId() : QString();
    fls::scene::Group *parent = sectionId.isEmpty() ? nullptr : state_->groupForId(sectionId);
    if (parent == nullptr) {
        parent = state_->project_.root.get();
    }
    parent->append(std::move(guide));
    state_->selectedLayerIds_.clear();
    state_->selectedGuideLayerIds_ = {guideId};
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setTool(QStringLiteral("transform"));
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Added guide layer %1").arg(guideName), 2500);
    return true;
}

void MainWindow::groupOrUngroupSelection() {
    EditorCommand command;
    command.entries = state_->selectionEntries();
    command.type = command.entries.size() == 1 && state_->entryIsGroup(command.entries.front())
        ? EditorCommandType::Ungroup : EditorCommandType::Group;
    QString error;
    if (!state_->executeCommand(command, &error)) {
        statusBar()->showMessage(error, 3000);
    }
}

void MainWindow::ungroupSelectionFlat() {
    executeSelectionCommand(EditorCommandType::Ungroup, true);
}

void MainWindow::collapseAllGroups() {
    if (tree_ == nullptr) {
        return;
    }
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    tree_->collapseAll();
}

void MainWindow::undo() {
    if (canvas_ != nullptr && canvas_->undoContourEdit()) {
        return;
    }
    EditorCommand command;
    command.type = EditorCommandType::Undo;
    state_->executeCommand(command);
    canvas_->resetRelativeSelectionFrame();
}

void MainWindow::redo() {
    if (canvas_ != nullptr && canvas_->redoContourEdit()) {
        return;
    }
    EditorCommand command;
    command.type = EditorCommandType::Redo;
    state_->executeCommand(command);
    canvas_->resetRelativeSelectionFrame();
}

void MainWindow::centerViewOnSelection() {
    if (canvas_ == nullptr || !canvas_->centerViewOnSelection()) {
        statusBar()->showMessage(QStringLiteral("No selection to center"), 1500);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Centered view on selection"), 1500);
}

void MainWindow::fitViewToVisibleArea() {
    if (canvas_ == nullptr || !canvas_->fitViewToVisibleArea()) {
        statusBar()->showMessage(QStringLiteral("Open a project to fit the visible area"), 1500);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Fit view to visible area"), 1500);
}

void MainWindow::noteProjectGeometryChanged(bool refreshPreviews) {
    const bool updateLayerPreviews = refreshPreviews
        || (treeModel_ != nullptr && treeModel_->generatePreviewsWithTransformations());
    ScopedPerf perf(updateLayerPreviews ? "noteProjectGeometryChanged(previews)" : "noteProjectGeometryChanged");
    if (canvas_ != nullptr) {
        if (refreshPreviews) {
            canvas_->clearRegionOverlay();
            canvas_->invalidateGuideImageCache();
        }
        canvas_->invalidateSelectionCache();
        canvas_->invalidateSceneCache();
        canvas_->invalidateContourLeewayCache();
        canvas_->update();
    }
    if (treeModel_ != nullptr) {
        ScopedPerf perfPrev("  refreshPreviews");
        treeModel_->refreshStateRoles(&state_->project_);
        if (updateLayerPreviews) {
            treeModel_->refreshPreviews(&state_->project_);
        }
    }
    updateColorPaletteWidget();
    refreshSelectionProperties();
    updateStatus();
}

void MainWindow::noteProjectStructureChanged() {
    ScopedPerf perf("noteProjectStructureChanged");
    const QSignalBlocker selectionBlocker(tree_->selectionModel());
    state_->selectedLayerIds_ = state_->existingLayerIds(state_->selectedLayerIds_);
    state_->selectedGuideLayerIds_ = state_->existingGuideLayerIds(state_->selectedGuideLayerIds_);
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    if (state_->project_.isLivery) {
        const QString sectionId = activeLiverySectionId();
        if (!sectionId.isEmpty()) {
            treeModel_->clearSectionCache();
            treeModel_->clear();
            ScopedPerf perfTree("  treeModel_->setProjectSection");
            treeModel_->setProjectSection(&state_->project_, sectionId);
        } else {
            treeModel_->setProject(&state_->project_);
        }
        rebuildSectionBar();
    } else {
        ScopedPerf perfTree("  treeModel_->setProject");
        treeModel_->setProject(&state_->project_);
    }
    syncTreeSelectionFromIds();
    noteProjectGeometryChanged();
}

void MainWindow::setToolName(const QString &name) {
    if (name != QStringLiteral("pen")) {
        clearContourLeewayEntry();
    }
    for (QAction *action : actions()) {
        const QVariant toolName = action->property("canvasToolName");
        if (toolName.isValid()) {
            action->setChecked(toolName.toString() == name);
        }
    }
    if (toolSettings_ != nullptr) {
        toolSettings_->setTool(name);
        refreshToolSettings();
    }
    statusBar()->showMessage(QStringLiteral("Tool: %1").arg(name), 1500);
}

} // namespace gui
