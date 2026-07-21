#include "canvas_tools.h"

#include "editor_state.h"
#include "project_canvas.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include <optional>
#include <algorithm>

namespace gui {

bool CanvasTool::handlePress(QMouseEvent *event) {
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::handleMove(QMouseEvent *event) {
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::handleWheel(QWheelEvent *event) {
    Q_UNUSED(event);
    return false;
}

void CanvasTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    Q_UNUSED(screenPos);
    Q_UNUSED(boxCenterWorld);
}

bool CanvasTool::handleRelease(QMouseEvent *event) {
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::handleDoubleClick(QMouseEvent *event) {
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::hoverCursor(const QPointF &point, QCursor *cursor) const {
    Q_UNUSED(point);
    Q_UNUSED(cursor);
    return false;
}

Qt::CursorShape CanvasTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::ArrowCursor;
}


QString SelectTool::name() const {
    return QStringLiteral("select");
}

bool SelectTool::handlePress(QMouseEvent *event) {
    event->accept();
    return true;
}

bool SelectTool::handleRelease(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (event->button() != Qt::LeftButton || c.drag_.mode != ProjectCanvas::DragMode::None) {
        return false;
    }
    if (c.movedPastClickThreshold(event->position())) {
        c.hoverLayerId_.clear();
        c.hoverPolygon_ = {};
        c.update();
        event->accept();
        return true;
    }
    const QString target = c.selectTargetAtScreenPoint(event->position(), event->modifiers());
    const QString guideTarget = target.isEmpty() ? c.guideAtScreenPoint(event->position()) : QString();
    if (c.state_ != nullptr) {
        if (!target.isEmpty()) {
            c.state_->selectLayerAtPoint(target, event->modifiers());
        } else if (!guideTarget.isEmpty()) {
            QSet<QString> guides = c.state_->selectedGuideLayerIds();
            if (event->modifiers() & Qt::ControlModifier) {
                if (guides.contains(guideTarget)) {
                    guides.remove(guideTarget);
                } else {
                    guides.insert(guideTarget);
                }
                c.state_->setSelectionIds(c.state_->selectedLayerIds(), guides);
            } else {
                c.state_->setSelectionIds({}, {guideTarget});
            }
        } else {
            c.state_->clearSelection();
        }
    }
    c.refreshHover(event->position(), event->modifiers());
    event->accept();
    return true;
}


QString MoveTool::name() const {
    return QStringLiteral("move");
}

void MoveTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    Q_UNUSED(screenPos);
    Q_UNUSED(boxCenterWorld);
    canvas_.drag_.mode = ProjectCanvas::DragMode::Move;
}

Qt::CursorShape MoveTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::SizeAllCursor;
}


QString MarqueeTool::name() const {
    return QStringLiteral("marquee");
}

bool MarqueeTool::handlePress(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    c.drag_.mode = ProjectCanvas::DragMode::Marquee;
    c.drag_.marqueeRect = QRectF(c.drag_.startScreen, c.drag_.startScreen).normalized();
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool MarqueeTool::handleRelease(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (c.drag_.mode != ProjectCanvas::DragMode::Marquee || event->button() != Qt::LeftButton) {
        return false;
    }
    if (c.movedPastClickThreshold(event->position())) {
        c.selectByMarquee(event->modifiers());
    } else if (c.state_ != nullptr) {
        c.state_->clearSelection();
    }
    c.finishDrag();
    c.update();
    event->accept();
    return true;
}

Qt::CursorShape MarqueeTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::CrossCursor;
}


QString TransformTool::name() const {
    return QStringLiteral("transform");
}

void TransformTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    ProjectCanvas &c = canvas_;
    c.drag_.activeHandle = c.transformHandleAt(screenPos, c.drag_.startBox);
    if (c.drag_.activeHandle == QStringLiteral("skew")) {
        c.drag_.mode = ProjectCanvas::DragMode::Skew;
    } else if (!c.drag_.activeHandle.isEmpty()) {
        c.drag_.mode = ProjectCanvas::DragMode::Scale;
        c.captureScaleReference();
    } else if (c.rotateZoneAt(screenPos, c.drag_.startBox)) {
        c.beginRotateDrag(boxCenterWorld);
    } else if (c.boxContainsScreenPoint(c.drag_.startBox, screenPos)) {
        c.drag_.mode = ProjectCanvas::DragMode::TransformMove;
    }
}

bool TransformTool::hoverCursor(const QPointF &point, QCursor *cursor) const {
    ProjectCanvas &c = canvas_;
    if (c.state_ == nullptr
        || (c.state_->selectedLayerIds().isEmpty() && c.state_->selectedGuideLayerIds().isEmpty())) {
        return false;
    }
    c.updateViewTransform();
    const ProjectCanvas::SelectionBox box = c.currentSelectionBox();
    const QString handle = c.transformHandleAt(point, box);
    if (!handle.isEmpty()) {
        *cursor = c.cursorForTransformHandle(handle, &box);
        return true;
    }
    if (c.rotateZoneAt(point, box)) {
        *cursor = c.rotateCursorForPoint(point, box);
        return true;
    }
    return false;
}

Qt::CursorShape TransformTool::idleCursorShape(const QPointF &point) const {
    ProjectCanvas &c = canvas_;
    if (c.state_ != nullptr
        && (!c.state_->selectedLayerIds().isEmpty() || !c.state_->selectedGuideLayerIds().isEmpty())) {
        c.updateViewTransform();
        const ProjectCanvas::SelectionBox box = c.currentSelectionBox();
        const QString handle = c.transformHandleAt(point, box);
        if (!handle.isEmpty()) {
            return c.cursorForScaleHandle(handle, &box);
        }
        if (c.rotateZoneAt(point, box)) {
            return Qt::ArrowCursor;
        }
        if (c.boxContainsScreenPoint(box, point)) {
            return Qt::SizeAllCursor;
        }
    }
    return Qt::ArrowCursor;
}


QString RotateTool::name() const {
    return QStringLiteral("rotate");
}

void RotateTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    Q_UNUSED(screenPos);
    canvas_.beginRotateDrag(boxCenterWorld);
}

bool RotateTool::hoverCursor(const QPointF &point, QCursor *cursor) const {
    ProjectCanvas &c = canvas_;
    if (c.state_ != nullptr
        && (!c.state_->selectedLayerIds().isEmpty() || !c.state_->selectedGuideLayerIds().isEmpty())) {
        c.updateViewTransform();
        const ProjectCanvas::SelectionBox box = c.currentSelectionBox();
        if (c.rotateZoneAt(point, box)) {
            *cursor = c.rotateCursorForPoint(point, box);
            return true;
        }
    }
    *cursor = c.rotateCursor();
    return true;
}


QString PipetteTool::name() const {
    return QStringLiteral("pipette");
}

bool PipetteTool::handlePress(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        return false;
    }
    const std::optional<QColor> color = canvas_.colorAtScreenPoint(event->position());
    if (color.has_value() && canvas_.pipetteColorPickedCallback_ != nullptr) {
        canvas_.pipetteColorPickedCallback_(color.value());
    }
    event->accept();
    return true;
}

bool PipetteTool::hoverCursor(const QPointF &point, QCursor *cursor) const {
    Q_UNUSED(point);
    if (cursor == nullptr) {
        return false;
    }
    *cursor = canvas_.pipetteCursor();
    return true;
}

Qt::CursorShape PipetteTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::ArrowCursor;
}


QString PenTool::name() const {
    return QStringLiteral("pen");
}

bool PenTool::handlePress(QMouseEvent *event) {
    if ((event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        || canvas_.pen_.fillRunning) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    const QPointF world = c.screenToWorld(event->position());

    if (c.pen_.closed) {
        c.refreshPenInteractionHint(event->position(), event->modifiers());
        const int pointIndex = c.pen_.hoverPoint;

        if (event->button() == Qt::RightButton) {
            if (pointIndex >= 0) {
                const bool removingOnlyHard =
                    c.pen_.points[pointIndex].kind == PenPointKind::Hard
                    && std::count_if(c.pen_.points.cbegin(),
                                     c.pen_.points.cend(),
                                     [](const PenPoint &point) {
                    return point.kind == PenPointKind::Hard;
                }) == 1;
                if (c.pen_.points.size() <= 3 || removingOnlyHard) {
                    const QString removalMessage = c.pen_.points.size() <= 3
                        ? QStringLiteral("A closed Pen path needs at least three points")
                        : QStringLiteral("A closed Pen path needs at least one hard point");
                    c.validatePenInteraction();
                    c.refreshPenInteractionHint(event->position(), event->modifiers());
                    QStringList lines = c.cursorHintLines_;
                    if (!lines.contains(QString())) {
                        lines.push_back(QString());
                    }
                    lines.push_back(removalMessage);
                    c.setCursorHint(event->position(), lines);
                } else {
                    c.pen_.points.removeAt(pointIndex);
                    c.normalizePenPointOrder();
                    c.validatePenInteraction();
                    c.refreshPenInteractionHint(event->position(), event->modifiers());
                }
            }
            event->accept();
            return true;
        }

        if ((event->modifiers() & Qt::AltModifier) && pointIndex >= 0) {
            c.pen_.dragPoint = pointIndex;
            c.pen_.dragOffsetWorld = c.pen_.points[pointIndex].position - world;
            c.updateCursorForPoint(event->position());
            event->accept();
            return true;
        }

        if (event->modifiers() & Qt::ControlModifier) {
            if (pointIndex >= 0) {
                if (c.pen_.points[pointIndex].kind == PenPointKind::Soft) {
                    c.pen_.points[pointIndex].kind = PenPointKind::Hard;
                    c.normalizePenPointOrder();
                    c.validatePenInteraction();
                }
            } else if (c.pen_.hoverCurve.valid()) {
                c.pen_.points.insert(c.pen_.hoverCurve.insertIndex,
                                    {c.pen_.hoverCurve.worldPosition, PenPointKind::Soft});
                c.validatePenInteraction();
            }
            c.refreshPenInteractionHint(event->position(), event->modifiers());
        }
        event->accept();
        return true;
    }

    if (event->button() != Qt::LeftButton) {
        return false;
    }
    if (c.pen_.points.size() >= 3
        && QLineF(event->position(), c.worldToScreen(c.pen_.points.front().position)).length()
               <= ProjectCanvas::kPenCloseRadius) {
        c.pen_.closed = true;
        c.validatePenInteraction();
        c.refreshPenInteractionHint(event->position(), event->modifiers());
        event->accept();
        return true;
    }
    if (!c.pen_.points.isEmpty()
        && QLineF(world, c.pen_.points.back().position).length() <= 1e-8) {
        event->accept();
        return true;
    }
    c.pen_.points.push_back({world,
                            c.pen_.points.isEmpty() ? PenPointKind::Hard : PenPointKind::Soft});
    c.pen_.hoverWorld = world;
    c.pen_.error.clear();
    c.pen_.crossings.clear();
    c.update();
    event->accept();
    return true;
}

bool PenTool::handleMove(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (c.pen_.dragPoint < 0 || c.pen_.dragPoint >= c.pen_.points.size()) {
        return false;
    }
    c.pen_.points[c.pen_.dragPoint].position =
        c.screenToWorld(event->position()) + c.pen_.dragOffsetWorld;
    c.validatePenInteraction();
    c.refreshPenInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool PenTool::handleRelease(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (event->button() != Qt::LeftButton || c.pen_.dragPoint < 0) {
        return false;
    }
    c.pen_.dragPoint = -1;
    c.validatePenInteraction();
    c.refreshPenInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool PenTool::handleDoubleClick(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || canvas_.pen_.fillRunning) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    if (c.pen_.closed) {
        event->accept();
        return true;
    }
    const QPointF world = c.screenToWorld(event->position());
    if (!c.pen_.points.isEmpty()
        && QLineF(world, c.pen_.points.back().position).length()
               <= std::max(1e-8, ProjectCanvas::kPenCloseRadius / std::max(c.camera_.scale(), 1e-8))) {
        c.pen_.points.back().position = world;
        c.pen_.points.back().kind = PenPointKind::Hard;
    } else {
        c.pen_.points.push_back({world, PenPointKind::Hard});
    }
    c.pen_.hoverWorld = world;
    c.pen_.error.clear();
    c.pen_.crossings.clear();
    c.update();
    event->accept();
    return true;
}

Qt::CursorShape PenTool::idleCursorShape(const QPointF &point) const {
    if (canvas_.pen_.closed
        && (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
        && canvas_.pointAtScreen(canvas_.pen_.points, point) >= 0) {
        return Qt::SizeAllCursor;
    }
    return Qt::CrossCursor;
}

QString LiningTool::name() const {
    return QStringLiteral("lining");
}

bool LiningTool::handlePress(QMouseEvent *event) {
    if ((event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        || canvas_.lining_.fillRunning) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    const QPointF world = c.screenToWorld(event->position());

    if (c.lining_.closed) {
        c.refreshLiningInteractionHint(event->position(), event->modifiers());
        const int pointIndex = c.lining_.hoverPoint;
        if (event->button() == Qt::RightButton) {
            if (pointIndex >= 0) {
                if (c.lining_.points.size() <= 2) {
                    c.lining_.error = QStringLiteral("A lining path needs at least two points");
                } else {
                    c.lining_.points.removeAt(pointIndex);
                    c.lining_.points.front().kind = PenPointKind::Hard;
                    c.lining_.points.back().kind = PenPointKind::Hard;
                    c.validateLiningInteraction();
                }
                c.refreshLiningInteractionHint(event->position(), event->modifiers());
            }
            event->accept();
            return true;
        }
        if ((event->modifiers() & Qt::AltModifier) && pointIndex >= 0) {
            c.lining_.dragPoint = pointIndex;
            c.lining_.dragOffsetWorld = c.lining_.points[pointIndex].position - world;
            c.updateCursorForPoint(event->position());
            event->accept();
            return true;
        }
        if (event->modifiers() & Qt::ControlModifier) {
            if (pointIndex >= 0) {
                if (c.lining_.points[pointIndex].kind == PenPointKind::Soft) {
                    c.lining_.points[pointIndex].kind = PenPointKind::Hard;
                    c.validateLiningInteraction();
                }
            } else if (c.lining_.hoverCurve.valid()) {
                c.lining_.points.insert(c.lining_.hoverCurve.insertIndex,
                                       {c.lining_.hoverCurve.worldPosition, PenPointKind::Soft});
                c.validateLiningInteraction();
            }
            c.refreshLiningInteractionHint(event->position(), event->modifiers());
        }
        event->accept();
        return true;
    }

    if (event->button() == Qt::RightButton) {
        if (c.lining_.points.size() < 2) {
            c.lining_.error = QStringLiteral("A lining path needs at least two points");
        } else {
            c.lining_.points.front().kind = PenPointKind::Hard;
            c.lining_.points.back().kind = PenPointKind::Hard;
            c.lining_.closed = true;
            c.validateLiningInteraction();
        }
        c.refreshLiningInteractionHint(event->position(), event->modifiers());
        event->accept();
        return true;
    }

    if (!c.lining_.points.isEmpty()
        && QLineF(world, c.lining_.points.back().position).length() <= 1e-8) {
        event->accept();
        return true;
    }
    c.lining_.points.push_back({world,
                               c.lining_.points.isEmpty() ? PenPointKind::Hard : PenPointKind::Soft});
    c.lining_.hoverWorld = world;
    c.lining_.error.clear();
    c.update();
    event->accept();
    return true;
}

bool LiningTool::handleMove(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (c.lining_.dragPoint < 0 || c.lining_.dragPoint >= c.lining_.points.size()) {
        return false;
    }
    c.lining_.points[c.lining_.dragPoint].position =
        c.screenToWorld(event->position()) + c.lining_.dragOffsetWorld;
    c.validateLiningInteraction();
    c.refreshLiningInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool LiningTool::handleWheel(QWheelEvent *event) {
    const QPoint angle = event->angleDelta();
    const QPoint pixels = event->pixelDelta();
    const int raw = angle.y() != 0 ? angle.y()
        : angle.x() != 0 ? angle.x()
        : pixels.y() != 0 ? pixels.y()
        : pixels.x();
    if (raw == 0 || canvas_.lining_.fillRunning) {
        event->accept();
        return true;
    }
    int steps = raw / 120;
    if (steps == 0) {
        steps = raw > 0 ? 1 : -1;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        steps *= 5;
    }
    canvas_.adjustLiningWidth(steps * 0.5, event->position());
    event->accept();
    return true;
}

bool LiningTool::handleRelease(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (event->button() != Qt::LeftButton || c.lining_.dragPoint < 0) {
        return false;
    }
    c.lining_.dragPoint = -1;
    c.validateLiningInteraction();
    c.refreshLiningInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool LiningTool::handleDoubleClick(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || canvas_.lining_.fillRunning) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    if (c.lining_.closed) {
        event->accept();
        return true;
    }
    const QPointF world = c.screenToWorld(event->position());
    if (!c.lining_.points.isEmpty()
        && QLineF(world, c.lining_.points.back().position).length()
               <= std::max(1e-8, ProjectCanvas::kPenEditRadius / std::max(c.camera_.scale(), 1e-8))) {
        c.lining_.points.back().position = world;
        c.lining_.points.back().kind = PenPointKind::Hard;
    } else {
        c.lining_.points.push_back({world, PenPointKind::Hard});
    }
    c.lining_.hoverWorld = world;
    c.lining_.error.clear();
    c.update();
    event->accept();
    return true;
}

Qt::CursorShape LiningTool::idleCursorShape(const QPointF &point) const {
    if (canvas_.lining_.closed
        && (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
        && canvas_.pointAtScreen(canvas_.lining_.points, point) >= 0) {
        return Qt::SizeAllCursor;
    }
    return Qt::CrossCursor;
}

QString BucketTool::name() const {
    return QStringLiteral("bucket");
}

bool BucketTool::handlePress(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        return false;
    }
    canvas_.commitBucketPreview(event->position());
    event->accept();
    return true;
}

bool BucketTool::handleMove(QMouseEvent *event) {
    canvas_.updateBucketPreview(event->position());
    canvas_.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool BucketTool::handleWheel(QWheelEvent *event) {
    const QPoint angle = event->angleDelta();
    const QPoint pixels = event->pixelDelta();
    const int raw = angle.y() != 0 ? angle.y()
        : angle.x() != 0 ? angle.x()
        : pixels.y() != 0 ? pixels.y()
        : pixels.x();
    if (raw == 0) {
        event->accept();
        return true;
    }
    int steps = raw / 120;
    if (steps == 0) {
        steps = raw > 0 ? 1 : -1;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        steps *= 5;
    }
    canvas_.adjustBucketTolerance(steps, event->position());
    event->accept();
    return true;
}

Qt::CursorShape BucketTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::CrossCursor;
}

} // namespace gui
