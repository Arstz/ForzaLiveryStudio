#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {
namespace {

constexpr double kMeshAnchorRadius = 4.5;
constexpr double kMeshHandleRadius = 3.0;
constexpr double kMeshHandleSeparation = 10.0;
constexpr int kMeshCurveHitSamples = 48;

PenBoundarySegment mapCurve(PenBoundarySegment curve, const QTransform &matrix) {
    curve.start = matrix.map(curve.start);
    curve.control = matrix.map(curve.control);
    curve.control2 = matrix.map(curve.control2);
    curve.end = matrix.map(curve.end);

    return curve;
}

} // namespace

void ProjectCanvas::setMeshFillRequestedCallback(std::function<void(const QString &)> callback) {
    meshFillRequestedCallback_ = std::move(callback);
}

void ProjectCanvas::setMeshFillCancelCallback(std::function<void()> callback) {
    meshFillCancelCallback_ = std::move(callback);
}

void ProjectCanvas::setMeshFillSourceId(const QString &id) {
    if (meshFillSourceId_ == id) return;
    meshFillSourceId_ = id;
    update();
}

const fls::scene::Mesh *ProjectCanvas::selectedMesh() const {

    return state_ != nullptr ? dynamic_cast<const fls::scene::Mesh *>(state_->sceneNode(pathFigureId_)) : nullptr;
}

bool ProjectCanvas::meshEditActive() const {

    return selectedMesh() != nullptr && (tool_ == QStringLiteral("pen") || tool_ == QStringLiteral("select") || tool_ == QStringLiteral("lining"));
}

bool ProjectCanvas::meshVisible() const {
    const auto *mesh = selectedMesh();

    if (mesh == nullptr) return false;
    for (const fls::scene::Layer *node = mesh; node != nullptr; node = node->parent()) if (!node->visible) return false;
    for (const auto &entry : state_->renderEntries()) if (entry.nodeId == mesh->id) return isSectionActive(entry.sectionGroupId);

    return false;
}

bool ProjectCanvas::meshEditable() const {
    const auto *mesh = selectedMesh();

    return meshVisible() && meshFillSourceId_ != mesh->id && !state_->isLayerLocked(mesh->id) && sceneWorldTransform(*mesh).isInvertible();
}

ProjectCanvas::MeshEditHit ProjectCanvas::meshPointAtScreen(const QPointF &point, bool includeEdges) const {
    MeshEditHit result;
    const auto *mesh = selectedMesh();
    double distance = kPenEditRadius;

    if (!meshVisible()) return result;
    const auto matrix = sceneWorldTransform(*mesh) * camera_.matrix();
    const auto edges = meshEditEdges(mesh->data);
    for (int vertex = 0; vertex < mesh->data.vertices.size(); ++vertex) {
        const double candidate = QLineF(point, matrix.map(mesh->data.vertices[vertex])).length();
        if (candidate < distance) {
            result.vertex = vertex;
            distance = candidate;
        }
    }
    if (result.valid()) return result;
    for (int index = 0; index < edges.size(); ++index) {
        const auto &edge = edges[index];
        for (int handle = 0; handle < 2; ++handle) {
            const int vertex = handle == 0 ? edge.first : edge.last;
            const QPointF control = matrix.map(handle == 0 ? edge.curve.control : edge.curve.control2);
            if (!meshEdit_.vertices.contains(vertex) || QLineF(control, matrix.map(mesh->data.vertices[vertex])).length() < kMeshHandleSeparation) continue;
            const double candidate = QLineF(point, control).length();
            if (candidate < distance) {
                result = {vertex, index, handle};
                distance = candidate;
            }
        }
    }
    if (result.valid() || !includeEdges) return result;
    for (int index = 0; index < edges.size(); ++index) {
        const auto curve = mapCurve(edges[index].curve, matrix);
        QPointF previous = curve.start;
        for (int sample = 1; sample <= kMeshCurveHitSamples; ++sample) {
            const QPointF next = curve.point(double(sample) / kMeshCurveHitSamples);
            const QPointF direction = next - previous;
            const double squared = QPointF::dotProduct(direction, direction);
            const double parameter = squared > 0.0 ? std::clamp(QPointF::dotProduct(point - previous, direction) / squared, 0.0, 1.0) : 0.0;
            const double candidate = QLineF(point, previous + direction * parameter).length();
            if (candidate < distance) {
                result = {-1, index, -1, (sample - 1 + parameter) / kMeshCurveHitSamples};
                distance = candidate;
            }
            previous = next;
        }
    }

    return result;
}

void ProjectCanvas::refreshMeshHover(const QPointF &point) {
    meshEdit_.hover = meshPointAtScreen(point);
    if (!meshEditable()) {
        clearCursorHint();
    } else if (meshEdit_.hover.vertex >= 0) {
        setCursorHint(point, {QStringLiteral("Drag: Move point"), QStringLiteral("Shift+click: Select points"), QStringLiteral("RMB: Remove point")});
    } else if (meshEdit_.hover.edge >= 0) {
        setCursorHint(point, {QStringLiteral("Ctrl+click: Insert shared point")});
    } else {
        clearCursorHint();
    }
    update();
}

bool ProjectCanvas::storeMeshData(fls::scene::MeshData data) {
    EditorCommand command;
    QString error;

    command.entries = {pathFigureId_};
    command.property = QStringLiteral("meshData");
    command.meshData = std::move(data);
    command.deferRefresh = meshEdit_.before.has_value();
    syncingPathFigure_ = true;
    const bool accepted = state_->executeCommand(command, &error);
    syncingPathFigure_ = false;
    if (!accepted) setCursorHint(mapFromGlobal(QCursor::pos()), {error});
    invalidateSceneCache();
    update();

    return accepted;
}

bool ProjectCanvas::handleMeshPress(QMouseEvent *event) {
    const auto *mesh = selectedMesh();

    if (!meshEditActive() || (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)) return false;
    if (!meshEditable()) return true;
    const auto hit = meshPointAtScreen(event->position());
    if (event->button() == Qt::RightButton) {
        if (hit.vertex >= 0) {
            auto data = mesh->data;
            QString error;
            if (removeMeshVertices(data, {hit.vertex}, &error)) {
                meshEdit_.vertices.clear();
                storeMeshData(std::move(data));
            } else setCursorHint(event->position(), {error});
        }

        return true;
    }
    if (hit.vertex < 0) {
        if (hit.edge >= 0 && (event->modifiers() & Qt::ControlModifier)) {
            auto data = mesh->data;
            const auto edges = meshEditEdges(data);
            materializeMeshHandles(data);
            const int vertex = splitMeshEdge(data, edges[hit.edge], hit.parameter);
            if (vertex >= 0 && storeMeshData(std::move(data))) meshEdit_.vertices = {vertex};
        } else meshEdit_.vertices.clear();
        update();

        return tool_ == QStringLiteral("pen") || hit.valid();
    }
    if (hit.handle < 0 && (event->modifiers() & Qt::ShiftModifier) && meshEdit_.vertices.contains(hit.vertex)) {
        meshEdit_.vertices.remove(hit.vertex);
        update();

        return true;
    }
    if (!meshEdit_.vertices.contains(hit.vertex)) {
        if (!(event->modifiers() & Qt::ShiftModifier)) meshEdit_.vertices.clear();
        meshEdit_.vertices.insert(hit.vertex);
    }
    meshEdit_.before = mesh->data;
    meshEdit_.edges = meshEditEdges(*meshEdit_.before);
    meshEdit_.drag = hit;
    meshEdit_.startWorld = screenToWorld(event->position());
    meshEdit_.inverse = sceneWorldTransform(*mesh).inverted();
    state_->beginProjectEdit();
    updateCursorForPoint(event->position());
    update();

    return true;
}

bool ProjectCanvas::handleMeshMove(QMouseEvent *event) {
    if (!meshEdit_.before.has_value()) return false;
    if (!meshEditable()) {
        finishMeshEdit(true);

        return true;
    }
    auto data = *meshEdit_.before;
    QPointF delta = screenToWorld(event->position()) - meshEdit_.startWorld;
    if (event->modifiers() & Qt::ShiftModifier) {
        if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0.0);
        else delta.setX(0.0);
    }
    const QPointF localDelta = meshEdit_.inverse.map(meshEdit_.startWorld + delta) - meshEdit_.inverse.map(meshEdit_.startWorld);
    if (localDelta.isNull()) {
        if (selectedMesh()->data != data) storeMeshData(std::move(data));

        return true;
    }
    const auto &hit = meshEdit_.drag;
    if (hit.handle >= 0) {
        const auto &edge = meshEdit_.edges[hit.edge];
        const auto control = (hit.handle == 0 ? edge.curve.control : edge.curve.control2) + localDelta;
        setMeshEdgeHandle(data, edge.first, edge.last, hit.vertex, control);
    } else {
        translateMeshVertices(data, meshEdit_.vertices, localDelta);
    }
    storeMeshData(std::move(data));
    updateCursorForPoint(event->position());

    return true;
}

bool ProjectCanvas::handleMeshRelease(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !meshEdit_.before.has_value()) return false;
    handleMeshMove(event);
    finishMeshEdit();
    refreshMeshHover(event->position());
    updateCursorForPoint(event->position());

    return true;
}

void ProjectCanvas::finishMeshEdit(bool cancel) {
    if (!meshEdit_.before.has_value()) return;
    meshEdit_.before.reset();
    meshEdit_.drag = {};
    meshEdit_.edges.clear();
    if (state_ != nullptr) {
        if (cancel) state_->cancelProjectEdit();
        else state_->commitProjectEdit();
    }
    invalidateSceneCache();
    update();
}

bool ProjectCanvas::handleMeshKey(KeyInteraction interaction) {
    const auto *mesh = selectedMesh();

    if (mesh == nullptr) return false;
    if (interaction == KeyInteraction::CanvasCommitInteraction) {
        if (meshFillSourceId_.isEmpty() && meshEditable()) {
            finishMeshEdit();
            if (drag_.mode != DragMode::None) finishDrag();
            if (meshFillRequestedCallback_) meshFillRequestedCallback_(pathFigureId_);
        }

        return true;
    }
    if (interaction == KeyInteraction::CanvasCancelInteraction && !meshFillSourceId_.isEmpty()) {
        if (meshFillCancelCallback_) meshFillCancelCallback_();

        return true;
    }
    if (!meshEditActive()) return false;
    if (interaction == KeyInteraction::CanvasCancelInteraction) {
        if (meshEdit_.before.has_value()) finishMeshEdit(true);
        else state_->clearSelection();

        return true;
    }
    if (interaction == KeyInteraction::CanvasRemovePathPoint) {
        if (meshEditable() && !meshEdit_.vertices.isEmpty()) {
            auto data = selectedMesh()->data;
            QString error;
            if (removeMeshVertices(data, meshEdit_.vertices, &error)) {
                meshEdit_.vertices.clear();
                storeMeshData(std::move(data));
            } else setCursorHint(mapFromGlobal(QCursor::pos()), {error});
        }

        return true;
    }

    return false;
}

void ProjectCanvas::drawMeshOverlay(QPainter &painter) {
    const auto *mesh = selectedMesh();

    if (!meshVisible()) return;
    const auto matrix = sceneWorldTransform(*mesh) * camera_.matrix();
    const auto edges = meshEditEdges(mesh->data);
    const QColor color = meshEditable() ? QColor(83, 164, 255) : QColor(160, 165, 175);
    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (meshEditActive()) {
        for (int index = 0; index < mesh->data.contours.size(); ++index) {
            if (!mesh->data.contours[index].visible) continue;
            auto fill = fls::scene::contourFillColor(mesh->data.contours[index].style);
            fill.setAlpha(50);
            painter.fillPath(matrix.map(contourDataPath(mesh->data.contourData(index))), fill);
        }
    }
    QPainterPath boundaries;
    for (const auto &edge : edges) {
        const auto curve = mapCurve(edge.curve, matrix);
        boundaries.moveTo(curve.start);
        curve.appendTo(boundaries);
    }
    painter.setBrush(Qt::NoBrush);
    painter.strokePath(boundaries, QPen(QColor(18, 20, 24), 4.0));
    painter.strokePath(boundaries, QPen(color, 2.0));
    if (meshEditActive()) {
        for (const auto &edge : edges) for (int handle = 0; handle < 2; ++handle) {
            const int vertex = handle == 0 ? edge.first : edge.last;
            if (!meshEdit_.vertices.contains(vertex)) continue;
            const auto anchor = matrix.map(mesh->data.vertices[vertex]);
            const auto control = matrix.map(handle == 0 ? edge.curve.control : edge.curve.control2);
            if (QLineF(anchor, control).length() < kMeshHandleSeparation) continue;
            painter.setPen(QPen(QColor(185, 190, 200, 180), 1.0));
            painter.drawLine(anchor, control);
            painter.setBrush(QColor(145, 185, 225));
            painter.drawRect(QRectF(control - QPointF(kMeshHandleRadius, kMeshHandleRadius), QSizeF(kMeshHandleRadius * 2.0, kMeshHandleRadius * 2.0)));
        }
    }
    for (int vertex = 0; vertex < mesh->data.vertices.size(); ++vertex) {
        const bool selected = meshEdit_.vertices.contains(vertex);
        const bool hovered = meshEdit_.hover.vertex == vertex;
        painter.setPen(QPen(QColor(18, 20, 24), 2.0));
        painter.setBrush(selected ? color : hovered ? QColor(120, 220, 135) : QColor(238, 240, 244));
        painter.drawEllipse(matrix.map(mesh->data.vertices[vertex]), kMeshAnchorRadius, kMeshAnchorRadius);
    }
    painter.restore();
}

} // namespace gui
