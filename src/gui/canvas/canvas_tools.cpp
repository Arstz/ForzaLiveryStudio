#include "canvas_tools.h"

#include "editor_state.h"
#include "project_canvas.h"

#include <QMouseEvent>

#include <optional>
#include <algorithm>

namespace gui {

bool CanvasTool::handlePress(QMouseEvent *event)
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
    if (event->button() != Qt::LeftButton || canvas_.penFillRunning_) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    const QPointF world = c.screenToWorld(event->position());
    if (c.penPoints_.size() >= 3
        && QLineF(event->position(), c.worldToScreen(c.penPoints_.front().position)).length()
               <= ProjectCanvas::PenCloseRadius) {
        c.closePenPath();
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

bool PenTool::handleDoubleClick(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || canvas_.penFillRunning_) {
        return false;
    }
    ProjectCanvas &c = canvas_;
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
    Q_UNUSED(point);
    return Qt::CrossCursor;
}

} // namespace gui
