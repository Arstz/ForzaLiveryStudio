#include "canvas_tools.h"

#include "editor_state.h"
#include "project_canvas.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include <optional>
#include <algorithm>

namespace gui {

bool CanvasTool::handlePress(QMouseEvent *event)
{
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::handleMove(QMouseEvent *event)
{
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::handleWheel(QWheelEvent *event)
{
    Q_UNUSED(event);
    return false;
}

void CanvasTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld)
{
    Q_UNUSED(screenPos);
    Q_UNUSED(boxCenterWorld);
}

bool CanvasTool::handleRelease(QMouseEvent *event)
{
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::handleDoubleClick(QMouseEvent *event)
{
    Q_UNUSED(event);
    return false;
}

bool CanvasTool::hoverCursor(const QPointF &point, QCursor *cursor) const
{
    Q_UNUSED(point);
    Q_UNUSED(cursor);
    return false;
}

Qt::CursorShape CanvasTool::idleCursorShape(const QPointF &point) const
{
    Q_UNUSED(point);
    return Qt::ArrowCursor;
}


QString SelectTool::name() const
{
    return QStringLiteral("select");
}

bool SelectTool::handlePress(QMouseEvent *event)
{
    event->accept();
    return true;
}

bool SelectTool::handleRelease(QMouseEvent *event)
{
    ProjectCanvas &c = canvas_;
    if (event->button() != Qt::LeftButton || c.dragMode_ != ProjectCanvas::DragMode::None) {
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


QString MoveTool::name() const
{
    return QStringLiteral("move");
}

void MoveTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld)
{
    Q_UNUSED(screenPos);
    Q_UNUSED(boxCenterWorld);
    canvas_.dragMode_ = ProjectCanvas::DragMode::Move;
}

Qt::CursorShape MoveTool::idleCursorShape(const QPointF &point) const
{
    Q_UNUSED(point);
    return Qt::SizeAllCursor;
}


QString MarqueeTool::name() const
{
    return QStringLiteral("marquee");
}

bool MarqueeTool::handlePress(QMouseEvent *event)
{
    ProjectCanvas &c = canvas_;
    c.dragMode_ = ProjectCanvas::DragMode::Marquee;
    c.marqueeRect_ = QRectF(c.dragStartScreen_, c.dragStartScreen_).normalized();
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool MarqueeTool::handleRelease(QMouseEvent *event)
{
    ProjectCanvas &c = canvas_;
    if (c.dragMode_ != ProjectCanvas::DragMode::Marquee || event->button() != Qt::LeftButton) {
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

Qt::CursorShape MarqueeTool::idleCursorShape(const QPointF &point) const
{
    Q_UNUSED(point);
    return Qt::CrossCursor;
}


QString TransformTool::name() const
{
    return QStringLiteral("transform");
}

void TransformTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld)
{
    ProjectCanvas &c = canvas_;
    c.activeHandle_ = c.transformHandleAt(screenPos, c.dragStartBox_);
    if (c.activeHandle_ == QStringLiteral("skew")) {
        c.dragMode_ = ProjectCanvas::DragMode::Skew;
    } else if (!c.activeHandle_.isEmpty()) {
        c.dragMode_ = ProjectCanvas::DragMode::Scale;
        c.captureScaleReference();
    } else if (c.rotateZoneAt(screenPos, c.dragStartBox_)) {
        c.beginRotateDrag(boxCenterWorld);
    } else if (c.boxContainsScreenPoint(c.dragStartBox_, screenPos)) {
        c.dragMode_ = ProjectCanvas::DragMode::TransformMove;
    }
}

bool TransformTool::hoverCursor(const QPointF &point, QCursor *cursor) const
{
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

Qt::CursorShape TransformTool::idleCursorShape(const QPointF &point) const
{
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


QString RotateTool::name() const
{
    return QStringLiteral("rotate");
}

void RotateTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld)
{
    Q_UNUSED(screenPos);
    canvas_.beginRotateDrag(boxCenterWorld);
}

bool RotateTool::hoverCursor(const QPointF &point, QCursor *cursor) const
{
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


QString PipetteTool::name() const
{
    return QStringLiteral("pipette");
}

bool PipetteTool::handlePress(QMouseEvent *event)
{
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

bool PipetteTool::hoverCursor(const QPointF &point, QCursor *cursor) const
{
    Q_UNUSED(point);
    if (cursor == nullptr) {
        return false;
    }
    *cursor = canvas_.pipetteCursor();
    return true;
}

Qt::CursorShape PipetteTool::idleCursorShape(const QPointF &point) const
{
    Q_UNUSED(point);
    return Qt::ArrowCursor;
}


QString PenTool::name() const
{
    return QStringLiteral("pen");
}

bool PenTool::handlePress(QMouseEvent *event)
{
    if ((event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        || canvas_.penFillRunning_) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    const QPointF world = c.screenToWorld(event->position());

    if (c.penLooped_) {
        c.refreshPenInteractionHint(event->position(), event->modifiers());
        const int pointIndex = c.penHoverPoint_;

        if (event->button() == Qt::RightButton) {
            if (pointIndex >= 0) {
                const bool removingOnlyHard =
                    c.penPoints_[pointIndex].kind == PenPointKind::Hard
                    && std::count_if(c.penPoints_.cbegin(),
                                     c.penPoints_.cend(),
                                     [](const PenPoint &point) {
                    return point.kind == PenPointKind::Hard;
                }) == 1;
                if (c.penPoints_.size() <= 3 || removingOnlyHard) {
                    const QString removalMessage = c.penPoints_.size() <= 3
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
                    c.penPoints_.removeAt(pointIndex);
                    c.normalizePenPointOrder();
                    c.validatePenInteraction();
                    c.refreshPenInteractionHint(event->position(), event->modifiers());
                }
            }
            event->accept();
            return true;
        }

        if ((event->modifiers() & Qt::AltModifier) && pointIndex >= 0) {
            c.penDragPoint_ = pointIndex;
            c.penDragOffsetWorld_ = c.penPoints_[pointIndex].position - world;
            c.updateCursorForPoint(event->position());
            event->accept();
            return true;
        }

        if (event->modifiers() & Qt::ControlModifier) {
            if (pointIndex >= 0) {
                if (c.penPoints_[pointIndex].kind == PenPointKind::Soft) {
                    c.penPoints_[pointIndex].kind = PenPointKind::Hard;
                    c.normalizePenPointOrder();
                    c.validatePenInteraction();
                }
            } else if (c.penHoverCurve_.valid()) {
                c.penPoints_.insert(c.penHoverCurve_.insertIndex,
                                    {c.penHoverCurve_.worldPosition, PenPointKind::Soft});
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
    if (c.penPoints_.size() >= 3
        && QLineF(event->position(), c.worldToScreen(c.penPoints_.front().position)).length()
               <= ProjectCanvas::PenCloseRadius) {
        c.penLooped_ = true;
        c.validatePenInteraction();
        c.refreshPenInteractionHint(event->position(), event->modifiers());
        event->accept();
        return true;
    }
    if (!c.penPoints_.isEmpty()
        && QLineF(world, c.penPoints_.back().position).length() <= 1e-8) {
        event->accept();
        return true;
    }
    c.penPoints_.push_back({world,
                            c.penPoints_.isEmpty() ? PenPointKind::Hard : PenPointKind::Soft});
    c.penHoverWorld_ = world;
    c.penError_.clear();
    c.penCrossings_.clear();
    c.update();
    event->accept();
    return true;
}

bool PenTool::handleMove(QMouseEvent *event)
{
    ProjectCanvas &c = canvas_;
    if (c.penDragPoint_ < 0 || c.penDragPoint_ >= c.penPoints_.size()) {
        return false;
    }
    c.penPoints_[c.penDragPoint_].position =
        c.screenToWorld(event->position()) + c.penDragOffsetWorld_;
    c.validatePenInteraction();
    c.refreshPenInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool PenTool::handleRelease(QMouseEvent *event)
{
    ProjectCanvas &c = canvas_;
    if (event->button() != Qt::LeftButton || c.penDragPoint_ < 0) {
        return false;
    }
    c.penDragPoint_ = -1;
    c.validatePenInteraction();
    c.refreshPenInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool PenTool::handleDoubleClick(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || canvas_.penFillRunning_) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    if (c.penLooped_) {
        event->accept();
        return true;
    }
    const QPointF world = c.screenToWorld(event->position());
    if (!c.penPoints_.isEmpty()
        && QLineF(world, c.penPoints_.back().position).length()
               <= std::max(1e-8, ProjectCanvas::PenCloseRadius / std::max(c.baseScale_ * c.zoom_, 1e-8))) {
        c.penPoints_.back().position = world;
        c.penPoints_.back().kind = PenPointKind::Hard;
    } else {
        c.penPoints_.push_back({world, PenPointKind::Hard});
    }
    c.penHoverWorld_ = world;
    c.penError_.clear();
    c.penCrossings_.clear();
    c.update();
    event->accept();
    return true;
}

Qt::CursorShape PenTool::idleCursorShape(const QPointF &point) const
{
    if (canvas_.penLooped_
        && (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
        && canvas_.penPointAtScreen(point) >= 0) {
        return Qt::SizeAllCursor;
    }
    return Qt::CrossCursor;
}

QString LiningTool::name() const
{
    return QStringLiteral("lining");
}

bool LiningTool::handlePress(QMouseEvent *event)
{
    if ((event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        || canvas_.liningFillRunning_) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    const QPointF world = c.screenToWorld(event->position());

    if (c.liningComplete_) {
        c.refreshLiningInteractionHint(event->position(), event->modifiers());
        const int pointIndex = c.liningHoverPoint_;
        if (event->button() == Qt::RightButton) {
            if (pointIndex >= 0) {
                if (c.liningPoints_.size() <= 2) {
                    c.liningError_ = QStringLiteral("A lining path needs at least two points");
                } else {
                    c.liningPoints_.removeAt(pointIndex);
                    c.liningPoints_.front().kind = PenPointKind::Hard;
                    c.liningPoints_.back().kind = PenPointKind::Hard;
                    c.validateLiningInteraction();
                }
                c.refreshLiningInteractionHint(event->position(), event->modifiers());
            }
            event->accept();
            return true;
        }
        if ((event->modifiers() & Qt::AltModifier) && pointIndex >= 0) {
            c.liningDragPoint_ = pointIndex;
            c.liningDragOffsetWorld_ = c.liningPoints_[pointIndex].position - world;
            c.updateCursorForPoint(event->position());
            event->accept();
            return true;
        }
        if (event->modifiers() & Qt::ControlModifier) {
            if (pointIndex >= 0) {
                if (c.liningPoints_[pointIndex].kind == PenPointKind::Soft) {
                    c.liningPoints_[pointIndex].kind = PenPointKind::Hard;
                    c.validateLiningInteraction();
                }
            } else if (c.liningHoverCurve_.valid()) {
                c.liningPoints_.insert(c.liningHoverCurve_.insertIndex,
                                       {c.liningHoverCurve_.worldPosition, PenPointKind::Soft});
                c.validateLiningInteraction();
            }
            c.refreshLiningInteractionHint(event->position(), event->modifiers());
        }
        event->accept();
        return true;
    }

    if (event->button() == Qt::RightButton) {
        if (c.liningPoints_.size() < 2) {
            c.liningError_ = QStringLiteral("A lining path needs at least two points");
        } else {
            c.liningPoints_.front().kind = PenPointKind::Hard;
            c.liningPoints_.back().kind = PenPointKind::Hard;
            c.liningComplete_ = true;
            c.validateLiningInteraction();
        }
        c.refreshLiningInteractionHint(event->position(), event->modifiers());
        event->accept();
        return true;
    }

    if (!c.liningPoints_.isEmpty()
        && QLineF(world, c.liningPoints_.back().position).length() <= 1e-8) {
        event->accept();
        return true;
    }
    c.liningPoints_.push_back({world,
                               c.liningPoints_.isEmpty() ? PenPointKind::Hard : PenPointKind::Soft});
    c.liningHoverWorld_ = world;
    c.liningError_.clear();
    c.update();
    event->accept();
    return true;
}

bool LiningTool::handleMove(QMouseEvent *event)
{
    ProjectCanvas &c = canvas_;
    if (c.liningDragPoint_ < 0 || c.liningDragPoint_ >= c.liningPoints_.size()) {
        return false;
    }
    c.liningPoints_[c.liningDragPoint_].position =
        c.screenToWorld(event->position()) + c.liningDragOffsetWorld_;
    c.validateLiningInteraction();
    c.refreshLiningInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool LiningTool::handleWheel(QWheelEvent *event)
{
    const QPoint angle = event->angleDelta();
    const QPoint pixels = event->pixelDelta();
    const int raw = angle.y() != 0 ? angle.y()
        : angle.x() != 0 ? angle.x()
        : pixels.y() != 0 ? pixels.y()
        : pixels.x();
    if (raw == 0 || canvas_.liningFillRunning_) {
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

bool LiningTool::handleRelease(QMouseEvent *event)
{
    ProjectCanvas &c = canvas_;
    if (event->button() != Qt::LeftButton || c.liningDragPoint_ < 0) {
        return false;
    }
    c.liningDragPoint_ = -1;
    c.validateLiningInteraction();
    c.refreshLiningInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool LiningTool::handleDoubleClick(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || canvas_.liningFillRunning_) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    if (c.liningComplete_) {
        event->accept();
        return true;
    }
    const QPointF world = c.screenToWorld(event->position());
    if (!c.liningPoints_.isEmpty()
        && QLineF(world, c.liningPoints_.back().position).length()
               <= std::max(1e-8, ProjectCanvas::PenEditRadius / std::max(c.baseScale_ * c.zoom_, 1e-8))) {
        c.liningPoints_.back().position = world;
        c.liningPoints_.back().kind = PenPointKind::Hard;
    } else {
        c.liningPoints_.push_back({world, PenPointKind::Hard});
    }
    c.liningHoverWorld_ = world;
    c.liningError_.clear();
    c.update();
    event->accept();
    return true;
}

Qt::CursorShape LiningTool::idleCursorShape(const QPointF &point) const
{
    if (canvas_.liningComplete_
        && (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
        && canvas_.liningPointAtScreen(point) >= 0) {
        return Qt::SizeAllCursor;
    }
    return Qt::CrossCursor;
}

QString BucketTool::name() const
{
    return QStringLiteral("bucket");
}

bool BucketTool::handlePress(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return false;
    }
    canvas_.commitBucketPreview(event->position());
    event->accept();
    return true;
}

bool BucketTool::handleMove(QMouseEvent *event)
{
    canvas_.updateBucketPreview(event->position());
    canvas_.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool BucketTool::handleWheel(QWheelEvent *event)
{
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

Qt::CursorShape BucketTool::idleCursorShape(const QPointF &point) const
{
    Q_UNUSED(point);
    return Qt::CrossCursor;
}

} // namespace gui
