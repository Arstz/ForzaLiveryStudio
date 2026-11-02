#include "main_window.h"

#include "main_window_internal.h"
#include "mesh_fill.h"
#include "matrix_math.h"
#include "shape_registry.h"

namespace gui {

using namespace mw_detail;

namespace {

fls::scene::Transform2D placementTransform(const QTransform &transform) {
    fls::Matrix3 matrix;

    matrix.m[0][0] = transform.m11();
    matrix.m[1][0] = transform.m12();
    matrix.m[0][1] = transform.m21();
    matrix.m[1][1] = transform.m22();
    matrix.m[0][2] = transform.dx();
    matrix.m[1][2] = transform.dy();

    return fls::decomposeTransform2D(matrix);
}

} // namespace

void MainWindow::startMeshFill(const QString &meshId) {
    const auto *mesh = dynamic_cast<const fls::scene::Mesh *>(state_->sceneNode(meshId));
    QString error;

    if (mesh == nullptr || canvas_ == nullptr || state_->isLayerLocked(meshId)
        || (generatedFillCancel_ != nullptr && generatedFillTool_ == QStringLiteral("mesh"))) return;
    for (const fls::scene::Layer *node = mesh; node != nullptr; node = node->parent()) {
        if (!node->visible || node->opacity < 1.0) {
            statusBar()->showMessage(QStringLiteral("Mesh filling requires opaque visible mesh and parent layers"), 5000);

            return;
        }
    }
    MeshFillRequest request;
    request.mesh = mesh->data;
    request.primitives = canvas_->compactFillPrimitives(&error);
    if (request.primitives.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Mesh fill failed: %1").arg(error), 5000);

        return;
    }
    prepareGeneratedFill({}, QStringLiteral("Mesh Fill"), QStringLiteral("mesh"));
    const quint64 generation = ++generatedFillGeneration_;
    const auto token = std::make_shared<std::atomic_bool>(false);
    const QPointer<MainWindow> guard(this);
    generatedFillCancel_ = token;
    generatedFillPlacementCount_ = generatedFillWorkCompleted_ = 0;
    generatedFillWorkTotal_ = std::count_if(request.mesh.contours.cbegin(), request.mesh.contours.cend(), [](const auto &face) { return face.visible; });
    generatedFillElapsed_.restart();
    generatedFillElapsedTimer_->start();
    generatedFillProgress_->setToolTip(QStringLiteral("Escape stops filling and keeps the generated shapes."));
    refreshGeneratedFillElapsedTime();
    canvas_->setMeshFillSourceId(meshId);
    statusBar()->showMessage(QStringLiteral("Filling mesh. Press Escape to stop and keep generated shapes."));
    auto *task = QRunnable::create([guard, generation, meshId, token, request = std::move(request)]() {
        auto result = fillMesh(request, [token]() { return token->load(std::memory_order_relaxed); },
            [guard, generation](const MeshFillProgress &progress) {
                queueMainWindowCallback(guard, [generation, progress](MainWindow &window) {
                    window.updateGeneratedFillProgress(generation, progress.shapeCount, progress.completedFaces, progress.totalFaces);
                });
            });
        queueMainWindowCallback(guard, [generation, meshId, result = std::move(result)](MainWindow &window) mutable {
            window.finishMeshFill(generation, meshId, std::move(result));
        });
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void MainWindow::stopMeshFill() {
    if (generatedFillTool_ != QStringLiteral("mesh") || generatedFillCancel_ == nullptr) return;
    if (!generatedFillCancel_->exchange(true, std::memory_order_relaxed)) {
        statusBar()->showMessage(QStringLiteral("Stopping mesh fill; keeping generated shapes…"));
    }
}

void MainWindow::finishMeshFill(quint64 generation, const QString &meshId, MeshFillResult result) {
    if (generation != generatedFillGeneration_ || generatedFillCancel_ == nullptr) return;
    result.stopped = result.stopped || generatedFillCancel_->load(std::memory_order_relaxed);
    generatedFillCancel_.reset();
    canvas_->setMeshFillSourceId({});
    const auto *source = dynamic_cast<const fls::scene::Mesh *>(state_->sceneNode(meshId));
    result.diagnostics.insert(QStringLiteral("sourceMesh"), meshId);
    result.diagnostics.insert(QStringLiteral("stopped"), result.stopped);
    QFile log(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mesh_fill_latest.json")));
    if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) log.write(QJsonDocument(result.diagnostics).toJson(QJsonDocument::Indented));
    if (source == nullptr || result.shapeCount == 0) {
        statusBar()->showMessage(result.stopped ? QStringLiteral("Mesh fill stopped; no shapes generated")
            : QStringLiteral("Mesh fill failed: %1").arg(result.error.isEmpty() ? QStringLiteral("no shapes generated") : result.error), 6000);
        clearGeneratedFillState();

        return;
    }
    auto group = std::make_unique<fls::scene::Group>();
    group->id = state_->uniqueGroupId();
    group->name = QStringLiteral("Mesh Fill");
    group->transform = source->transform;
    for (auto layer = result.layers.crbegin(); layer != result.layers.crend(); ++layer) {
        if (layer->placements.isEmpty()) continue;
        auto face = std::make_unique<fls::scene::Group>();
        face->id = QStringLiteral("group_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        face->name = layer->name.isEmpty() ? QStringLiteral("Face %1").arg(layer->face + 1) : layer->name;
        for (const auto &placement : layer->placements) {
            auto shape = std::make_unique<fls::scene::Shape>();
            shape->id = QStringLiteral("layer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            shape->name = fls::detail::shapeName(static_cast<quint16>(placement.shapeId));
            shape->setVectorShape(static_cast<quint16>(placement.shapeId));
            shape->transform = placementTransform(placement.transform);
            shape->color = {static_cast<quint8>(layer->color.blue()), static_cast<quint8>(layer->color.green()),
                static_cast<quint8>(layer->color.red()), static_cast<quint8>(layer->color.alpha())};
            face->append(std::move(shape));
        }
        group->append(std::move(face));
    }
    EditorCommand insert;
    insert.type = EditorCommandType::Insert;
    insert.entries = {meshId};
    insert.clipboard = ProjectClipboard{};
    insert.clipboard->nodes.push_back(std::move(group));
    QString error;
    const bool inserted = state_->executeCommand(insert, &error);
    const QString message = !inserted ? QStringLiteral("Mesh fill insertion failed: %1").arg(error)
        : !result.error.isEmpty() ? QStringLiteral("Kept %1 mesh fill shapes: %2").arg(result.shapeCount).arg(result.error)
        : result.stopped ? QStringLiteral("Mesh fill stopped; kept %1 shapes").arg(result.shapeCount)
        : QStringLiteral("Filled mesh with %1 shapes").arg(result.shapeCount);
    clearGeneratedFillState();
    statusBar()->showMessage(message, 8000);
    canvas_->setFocus();
}

} // namespace gui
