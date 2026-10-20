#include "canvas_tools.h"

#include "editor_state.h"
#include "project_canvas.h"

#include <QMouseEvent>
#include <QWheelEvent>

#include <optional>
#include <algorithm>

namespace gui {
namespace {

int wheelAdjustmentSteps(const QWheelEvent &event) {
    const QPoint angle = event.angleDelta();
    const QPoint pixels = event.pixelDelta();
    const int raw = angle.y() != 0 ? angle.y()
        : angle.x() != 0 ? angle.x()
        : pixels.y() != 0 ? pixels.y()
        : pixels.x();
    if (raw == 0) {
        return 0;
    }
    int steps = raw / 120;
    if (steps == 0) {
        steps = raw > 0 ? 1 : -1;
    }
    if (event.modifiers() & Qt::ShiftModifier) {
        steps *= 5;
    }

    return steps;
}

} // namespace

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
    ProjectCanvas &c = canvas_;
    if (!c.options_.separateSelectAndMarqueeTools) {
        c.drag_.mode = ProjectCanvas::DragMode::Marquee;
        c.drag_.marqueeRect = {};
        c.hoverLayerId_.clear();
        c.hoverBody_ = {};
        c.update();
    }
    event->accept();
    return true;
}

bool SelectTool::handleRelease(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    const bool marquee = c.drag_.mode == ProjectCanvas::DragMode::Marquee;
    if (event->button() != Qt::LeftButton
        || (c.drag_.mode != ProjectCanvas::DragMode::None && !marquee)) {
        return false;
    }
    if (c.movedPastClickThreshold(event->position())) {
        if (marquee) {
            c.drag_.marqueeRect = QRectF(c.drag_.startScreen, event->position()).normalized();
            c.selectByMarquee(event->modifiers());
            c.finishDrag();
        }
        c.hoverLayerId_.clear();
        c.hoverBody_ = {};
        c.update();
        event->accept();
        return true;
    }
    if (marquee) {
        c.finishDrag();
    }
    c.selectAtScreenPoint(event->position(), event->modifiers());
    c.refreshHover(event->position(), event->modifiers());
    event->accept();
    return true;
}

QString MoveTool::name() const {
    return QStringLiteral("move");
}

bool MoveTool::handlePress(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (!c.options_.moveToolAutoSelect) {
        return false;
    }
    c.drag_.startBox = c.currentSelectionBox();
    if (c.state_ != nullptr && !c.boxContainsScreenPoint(c.drag_.startBox, event->position())
        && c.layersAtScreenPoint(event->position()).isEmpty()
        && c.guideAtScreenPoint(event->position()).isEmpty()) {
        c.state_->clearSelection();
    }
    c.drag_.mode = ProjectCanvas::DragMode::PendingMove;
    c.updateCursorForPoint(event->position());
    event->accept();

    return true;
}

bool MoveTool::handleMove(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (c.drag_.mode != ProjectCanvas::DragMode::PendingMove) {
        return false;
    }
    if (c.movedPastClickThreshold(event->position()) && c.prepareMoveAutoSelection(event->modifiers())) {
        c.drag_.mode = ProjectCanvas::DragMode::None;
        if (c.startToolDrag(c.drag_.startScreen, event->modifiers())) {
            c.applyMoveDrag(event->position(), event->modifiers());
        }
    }
    event->accept();

    return true;
}

bool MoveTool::handleRelease(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (!c.options_.moveToolAutoSelect || event->button() != Qt::LeftButton
        || (c.drag_.mode != ProjectCanvas::DragMode::PendingMove
            && c.drag_.mode != ProjectCanvas::DragMode::Move)) {
        return false;
    }
    const bool click = c.drag_.mode == ProjectCanvas::DragMode::PendingMove
        && !c.movedPastClickThreshold(event->position());
    const bool selectOnClick = click && !c.boxContainsScreenPoint(c.drag_.startBox, c.drag_.startScreen);

    if (c.drag_.mode == ProjectCanvas::DragMode::PendingMove && !click) {
        handleMove(event);
    }
    if (c.drag_.mode == ProjectCanvas::DragMode::Move) {
        c.applyMoveDrag(event->position(), event->modifiers());
    }
    c.finishDrag();
    if (selectOnClick) {
        c.selectAtScreenPoint(event->position(), event->modifiers());
    }
    c.refreshHover(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();

    return true;
}

void MoveTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    Q_UNUSED(screenPos);
    Q_UNUSED(boxCenterWorld);
    canvas_.drag_.mode = ProjectCanvas::DragMode::Move;
}

Qt::CursorShape MoveTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);

    if (canvas_.options_.moveToolAutoSelect
        && (canvas_.state_ == nullptr
            || (canvas_.state_->selectedLayerIds().isEmpty() && canvas_.state_->selectedGuideLayerIds().isEmpty()))) {
        return Qt::ArrowCursor;
    }

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
        c.drag_.marqueeRect = QRectF(c.drag_.startScreen, event->position()).normalized();
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
    } else {
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

QString SkewTool::name() const {
    return QStringLiteral("skew");
}

void SkewTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    Q_UNUSED(screenPos);
    Q_UNUSED(boxCenterWorld);
    canvas_.drag_.mode = ProjectCanvas::DragMode::Skew;
}

Qt::CursorShape SkewTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::SizeHorCursor;
}

QString OpacityTool::name() const {
    return QStringLiteral("opacity");
}

void OpacityTool::beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    Q_UNUSED(screenPos);
    Q_UNUSED(boxCenterWorld);
    canvas_.drag_.mode = ProjectCanvas::DragMode::Opacity;
}

Qt::CursorShape OpacityTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::SizeHorCursor;
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
        if (canvas_.options_.pipetteAutoReturn) {
            canvas_.setTool(canvas_.lastNonPipetteTool_);
        }
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
        || canvas_.pen_.fillRunning
        || !canvas_.pathFigureEditable(canvas_.pen_)) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    const QPointF world =
        c.snappedPenPosition(
            c.screenToWorld(event->position()),
            event->modifiers());

    if (c.pen_.closed && c.pen_.cutoutClosed) {
        c.refreshPenInteractionHint(event->position(), event->modifiers());
        const int pointIndex = c.pen_.hoverPoint;
        const int loopIndex = c.pen_.hoverLoop;

        if (event->button() == Qt::RightButton) {
            if (pointIndex >= 0) {
                QVector<PenPoint> &points = c.penPointsForLoop(loopIndex);
                if (loopIndex >= 0 && points.size() <= 3) {
                    c.beginPathEdit(c.pen_);
                    c.pen_.cutouts.removeAt(loopIndex);
                    c.pen_.activeCutout = c.pen_.cutouts.isEmpty()
                        ? -1 : c.pen_.cutouts.size() - 1;
                    c.validatePenInteraction();
                    c.commitPathEdit(c.pen_);
                    c.refreshPenInteractionHint(event->position(), event->modifiers());
                } else if (points.size() <= 3) {
                    const QString removalMessage =
                        QStringLiteral("A closed Pen path needs at least three anchors");
                    c.validatePenInteraction();
                    c.refreshPenInteractionHint(event->position(), event->modifiers());
                    QStringList lines = c.cursorHintLines_;
                    if (!lines.contains(QString())) {
                        lines.push_back(QString());
                    }
                    lines.push_back(removalMessage);
                    c.setCursorHint(event->position(), lines);
                } else {
                    c.beginPathEdit(c.pen_);
                    points.removeAt(pointIndex);
                    c.normalizePenPointOrder(points);
                    c.validatePenInteraction();
                    c.commitPathEdit(c.pen_);
                    c.refreshPenInteractionHint(event->position(), event->modifiers());
                }
            }
            event->accept();
            return true;
        }

        if ((event->modifiers() & Qt::AltModifier) && pointIndex >= 0) {
            c.beginPathEdit(c.pen_);
            c.pen_.dragPoint = pointIndex;
            c.pen_.dragLoop = loopIndex;
            c.pen_.dragHandle = c.pen_.hoverHandle;
            const auto &point = c.penPointsForLoop(loopIndex)[pointIndex];
            c.pen_.dragOffsetWorld = point.position - world +
                                     (c.pen_.dragHandle < 0   ? point.incoming
                                      : c.pen_.dragHandle > 0 ? point.outgoing
                                                              : QPointF{});
            c.updateCursorForPoint(event->position());
            event->accept();
            return true;
        }

        if (event->modifiers() & Qt::ControlModifier) {
            c.beginPathEdit(c.pen_);
            if (pointIndex >= 0) {
                QVector<PenPoint> &points = c.penPointsForLoop(loopIndex);
                auto &point = points[pointIndex];
                point.kind =
                    point.kind == PenPointKind::Soft ? PenPointKind::Hard : PenPointKind::Soft;
                if (point.kind == PenPointKind::Soft && point.explicitHandles) {
                    QPointF direction = point.outgoing - point.incoming;
                    const double n = QLineF({}, direction).length();
                    if (n > 1e-9) {
                        direction /= n;
                        point.incoming = -direction * QLineF({}, point.incoming).length();
                        point.outgoing = direction * QLineF({}, point.outgoing).length();
                    }
                }
                c.validatePenInteraction();
            } else if (c.pen_.hoverCurve.valid()) {
                insertPenAnchor(c.penPointsForLoop(c.pen_.hoverCurve.loopIndex),
                                c.pen_.hoverCurve.insertIndex, c.pen_.hoverCurve.worldPosition);
                c.validatePenInteraction();
            } else if (buildPenContour(c.currentPenLoops()).path.contains(world)) {
                c.pen_.cutouts.push_back({{world, PenPointKind::Hard}});
                c.pen_.activeCutout = c.pen_.cutouts.size() - 1;
                c.pen_.cutoutClosed = false;
                c.pen_.hoverWorld = world;
                c.pen_.error.clear();
                c.pen_.crossings.clear();
            }
            c.commitPathEdit(c.pen_);
            c.refreshPenInteractionHint(event->position(), event->modifiers());
        }
        event->accept();
        return true;
    }

    if (event->button() != Qt::LeftButton) {
        return false;
    }
    QVector<PenPoint> &points = c.pen_.closed
        ? c.pen_.cutouts[c.pen_.activeCutout]
        : c.pen_.points;
    if (points.size() >= 3
        && QLineF(event->position(), c.worldToScreen(points.front().position)).length()
               <= ProjectCanvas::kPenCloseRadius) {
        c.beginPathEdit(c.pen_);
        if (c.pen_.closed) {
            c.pen_.cutoutClosed = true;
        } else {
            c.pen_.closed = true;
        }
        c.validatePenInteraction();
        c.commitPathEdit(c.pen_);
        c.refreshPenInteractionHint(event->position(), event->modifiers());
        event->accept();
        return true;
    }
    if (!points.isEmpty()
        && QLineF(world, points.back().position).length() <= 1e-8) {
        event->accept();
        return true;
    }
    c.beginPathEdit(c.pen_);
    points.push_back({world,
                      points.isEmpty() ? PenPointKind::Hard : PenPointKind::Soft});
    c.pen_.hoverWorld = world;
    c.pen_.error.clear();
    c.pen_.crossings.clear();
    c.commitPathEdit(c.pen_);
    c.update();
    event->accept();
    return true;
}

bool PenTool::handleMove(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (c.pen_.dragPoint < 0
        || c.pen_.dragPoint >= c.penPointsForLoop(c.pen_.dragLoop).size()) {
        return false;
    }
    auto &point = c.penPointsForLoop(c.pen_.dragLoop)[c.pen_.dragPoint];
    const QPointF destination = c.screenToWorld(event->position()) + c.pen_.dragOffsetWorld;
    if (!c.pen_.dragHandle)
        point.position = destination;
    else {
        QPointF &handle = c.pen_.dragHandle < 0 ? point.incoming : point.outgoing;
        QPointF &other = c.pen_.dragHandle < 0 ? point.outgoing : point.incoming;
        handle = destination - point.position;
        if (point.kind == PenPointKind::Soft) {
            const double n = QLineF({}, handle).length();
            if (n > 1e-9)
                other = -handle * (QLineF({}, other).length() / n);
        }
    }
    c.invalidatePenGeometryCache();
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
    c.pen_.dragLoop = -1;
    c.validatePenInteraction();
    c.commitPathEdit(c.pen_);
    c.refreshPenInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool PenTool::handleDoubleClick(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || canvas_.pen_.fillRunning
        || !canvas_.pathFigureEditable(canvas_.pen_)) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    if (c.pen_.closed && c.pen_.cutoutClosed) {
        event->accept();
        return true;
    }
    QVector<PenPoint> &points = c.pen_.closed
        ? c.pen_.cutouts[c.pen_.activeCutout]
        : c.pen_.points;
    const QPointF pointerWorld =
        c.screenToWorld(event->position());
    const bool completesSoftPoint =
        points.size() >= 2
        && points.back().kind
            == PenPointKind::Soft;
    const QPointF world =
        completesSoftPoint
            ? c.snappedPenPosition(
                  pointerWorld,
                  points[
                      points.size() - 2]
                      .position,
                  event->modifiers())
            : c.snappedPenPosition(
                  pointerWorld,
                  event->modifiers());
    c.beginPathEdit(c.pen_);
    if (!points.isEmpty()
        && QLineF(world, points.back().position).length()
               <= std::max(1e-8, ProjectCanvas::kPenCloseRadius / std::max(c.camera_.scale(), 1e-8))) {
        points.back().position = world;
        points.back().kind = PenPointKind::Hard;
    } else {
        points.push_back({world, PenPointKind::Hard});
    }
    c.pen_.hoverWorld = world;
    c.pen_.error.clear();
    c.pen_.crossings.clear();
    c.commitPathEdit(c.pen_);
    c.update();
    event->accept();
    return true;
}

Qt::CursorShape PenTool::idleCursorShape(const QPointF &point) const {
    if (canvas_.pen_.closed
        && (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
        && canvas_.pen_.cutoutClosed) {
        if (canvas_.penPointAtScreen(point).valid()) {
            return Qt::SizeAllCursor;
        }
    }
    return Qt::CrossCursor;
}

QString LiningTool::name() const {
    return QStringLiteral("lining");
}

bool LiningTool::handlePress(QMouseEvent *event) {
    if ((event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
        || canvas_.lining_.fillRunning || !canvas_.pathFigureEditable(canvas_.lining_)) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    const QPointF world = c.screenToWorld(event->position());
    const auto capHit = c.liningCapHandleAtScreen(event->position());
    if (capHit.valid()) {
        c.beginPathEdit(c.lining_);
        if (event->button() == Qt::RightButton) {
            c.lining_.points[capHit.pointIndex].endCap = 0.0;
            c.commitPathEdit(c.lining_);
        } else {
            c.lining_.dragPoint = capHit.pointIndex;
            c.lining_.dragHandle = 3;
        }
        c.refreshLiningInteractionHint(event->position(), event->modifiers());
        event->accept();
        return true;
    }
    const auto widthHit = c.liningWidthHandleAtScreen(event->position());

    if (widthHit.valid()) {
        c.beginPathEdit(c.lining_);
        if (event->button() == Qt::RightButton) {
            c.lining_.points[widthHit.pointIndex].strokeWidth = c.lining_.strokeWidth;
            c.commitPathEdit(c.lining_);
        } else {
            c.lining_.dragPoint = widthHit.pointIndex;
            c.lining_.dragHandle = widthHit.handle;
        }
        c.refreshLiningInteractionHint(event->position(), event->modifiers());
        c.updateCursorForPoint(event->position());
        event->accept();
        return true;
    }

    if (c.lining_.closed) {
        c.refreshLiningInteractionHint(event->position(), event->modifiers());
        const int pointIndex = c.lining_.hoverPoint;
        if (event->button() == Qt::RightButton) {
            if (pointIndex >= 0) {
                if (c.lining_.points.size() <= 2) {
                    c.lining_.error = QStringLiteral("A lining path needs at least two points");
                } else {
                    c.beginPathEdit(c.lining_);
                    c.lining_.points.removeAt(pointIndex);
                    c.lining_.points.front().kind = PenPointKind::Hard;
                    c.lining_.points.back().kind = PenPointKind::Hard;
                    c.validateLiningInteraction();
                    c.commitPathEdit(c.lining_);
                }
                c.refreshLiningInteractionHint(event->position(), event->modifiers());
            }
            event->accept();
            return true;
        }
        if ((event->modifiers() & Qt::AltModifier) && pointIndex >= 0) {
            c.beginPathEdit(c.lining_);
            c.lining_.dragPoint = pointIndex;
            c.lining_.dragHandle = 0;
            c.lining_.dragOffsetWorld = c.lining_.points[pointIndex].position - world;
            c.updateCursorForPoint(event->position());
            event->accept();
            return true;
        }
        if (event->modifiers() & Qt::ControlModifier) {
            c.beginPathEdit(c.lining_);
            if (pointIndex >= 0) {
                if (c.lining_.points[pointIndex].kind == PenPointKind::Soft) {
                    c.lining_.points[pointIndex].kind = PenPointKind::Hard;
                    c.validateLiningInteraction();
                }
            } else if (c.lining_.hoverCurve.valid()) {
                const int insertIndex = c.lining_.hoverCurve.insertIndex;
                const auto position = c.lining_.hoverCurve.worldPosition;
                const auto &before = c.lining_.points[insertIndex - 1];
                const auto &after = c.lining_.points[insertIndex];
                const double beforeDistance = QLineF(before.position, position).length();
                const double afterDistance = QLineF(after.position, position).length();
                const double fraction = beforeDistance / std::max(beforeDistance + afterDistance, 1e-8);
                const double width = liningPointWidth(before, c.lining_.strokeWidth) * (1.0 - fraction)
                    + liningPointWidth(after, c.lining_.strokeWidth) * fraction;
                c.lining_.points.insert(insertIndex, {position, PenPointKind::Soft, {}, {}, false, width});
                c.validateLiningInteraction();
            }
            c.commitPathEdit(c.lining_);
            c.refreshLiningInteractionHint(event->position(), event->modifiers());
        }
        event->accept();
        return true;
    }

    if (event->button() == Qt::RightButton) {
        if (c.lining_.points.size() < 2) {
            c.lining_.error = QStringLiteral("A lining path needs at least two points");
        } else {
            c.beginPathEdit(c.lining_);
            c.lining_.points.front().kind = PenPointKind::Hard;
            c.lining_.points.back().kind = PenPointKind::Hard;
            c.lining_.closed = true;
            c.validateLiningInteraction();
            c.commitPathEdit(c.lining_);
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
    c.beginPathEdit(c.lining_);
    c.lining_.points.push_back({world,
                               c.lining_.points.isEmpty() ? PenPointKind::Hard : PenPointKind::Soft,
                               {}, {}, false, c.lining_.strokeWidth});
    c.lining_.hoverWorld = world;
    c.lining_.error.clear();
    c.commitPathEdit(c.lining_);
    c.update();
    event->accept();
    return true;
}

bool LiningTool::handleMove(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (c.lining_.dragPoint < 0 || c.lining_.dragPoint >= c.lining_.points.size()) {
        return false;
    }
    auto &point = c.lining_.points[c.lining_.dragPoint];
    if (c.lining_.dragHandle == 3) {
        const QPointF outward = liningEndpointDirection(c.lining_.points, c.lining_.dragPoint);
        const double offset = c.camera_.worldPerPixel() * ProjectCanvas::kLiningWidthHandleOffsetPixels;
        const double distance = QPointF::dotProduct(c.screenToWorld(event->position()) - point.position, outward) - offset;
        const double radius = liningPointWidth(point, c.lining_.strokeWidth) * 0.5;
        point.endCap = std::clamp(distance / radius, -2.0, 2.0);
    } else if (c.lining_.dragHandle != 0) {
        const double distance = std::abs(QPointF::dotProduct(c.screenToWorld(event->position()) - point.position,
            c.liningWidthNormal(c.lining_.dragPoint))) - c.camera_.worldPerPixel() * ProjectCanvas::kLiningWidthHandleOffsetPixels;
        point.strokeWidth = std::clamp(distance * 2.0, ProjectCanvas::kMinimumLiningWidth, ProjectCanvas::kMaximumLiningWidth);
    } else {
        point.position = c.screenToWorld(event->position()) + c.lining_.dragOffsetWorld;
    }
    c.validateLiningInteraction();
    c.refreshLiningInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool LiningTool::handleWheel(QWheelEvent *event) {
    const int steps = wheelAdjustmentSteps(*event);
    if (steps == 0 || canvas_.lining_.fillRunning || !canvas_.pathFigureEditable(canvas_.lining_)) {
        event->accept();
        return true;
    }
    const auto cap = canvas_.liningCapHandleAtScreen(event->position());
    if (cap.valid()) {
        canvas_.beginPathEdit(canvas_.lining_);
        auto &point = canvas_.lining_.points[cap.pointIndex];
        point.endCap = std::clamp(point.endCap + steps * 0.1, -2.0, 2.0);
        canvas_.commitPathEdit(canvas_.lining_);
        canvas_.refreshLiningInteractionHint(event->position(), event->modifiers());
    } else {
        canvas_.adjustLiningWidth(steps * 0.5, event->position());
    }
    event->accept();
    return true;
}

bool LiningTool::handleRelease(QMouseEvent *event) {
    ProjectCanvas &c = canvas_;
    if (event->button() != Qt::LeftButton || c.lining_.dragPoint < 0) {
        return false;
    }
    c.lining_.dragPoint = -1;
    c.lining_.dragHandle = 0;
    c.validateLiningInteraction();
    c.commitPathEdit(c.lining_);
    c.refreshLiningInteractionHint(event->position(), event->modifiers());
    c.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool LiningTool::handleDoubleClick(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || canvas_.lining_.fillRunning || !canvas_.pathFigureEditable(canvas_.lining_)) {
        return false;
    }
    ProjectCanvas &c = canvas_;
    if (c.lining_.closed) {
        const auto cap = c.liningCapHandleAtScreen(event->position());
        if (cap.valid()) {
            c.beginPathEdit(c.lining_);
            c.lining_.points[cap.pointIndex].endCap = 0.0;
            c.commitPathEdit(c.lining_);
            c.refreshLiningInteractionHint(event->position(), event->modifiers());
        }
        event->accept();
        return true;
    }
    const QPointF world = c.screenToWorld(event->position());
    c.beginPathEdit(c.lining_);
    if (!c.lining_.points.isEmpty()
        && QLineF(world, c.lining_.points.back().position).length()
               <= std::max(1e-8, ProjectCanvas::kPenEditRadius / std::max(c.camera_.scale(), 1e-8))) {
        c.lining_.points.back().position = world;
        c.lining_.points.back().kind = PenPointKind::Hard;
    } else {
        c.lining_.points.push_back({world, PenPointKind::Hard, {}, {}, false, c.lining_.strokeWidth});
    }
    c.lining_.hoverWorld = world;
    c.lining_.error.clear();
    c.commitPathEdit(c.lining_);
    c.update();
    event->accept();
    return true;
}

Qt::CursorShape LiningTool::idleCursorShape(const QPointF &point) const {
    if (canvas_.liningWidthHandleAtScreen(point).valid() || canvas_.liningCapHandleAtScreen(point).valid())
        return Qt::SizeAllCursor;
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
    canvas_.commitBucketPreview(event->position(), event->modifiers());
    event->accept();
    return true;
}

bool BucketTool::handleMove(QMouseEvent *event) {
    canvas_.updateBucketPreview(
        event->position(), event->modifiers().testFlag(Qt::ShiftModifier));
    canvas_.updateCursorForPoint(event->position());
    event->accept();
    return true;
}

bool BucketTool::handleWheel(QWheelEvent *event) {
    const int steps = wheelAdjustmentSteps(*event);
    if (steps == 0) {
        event->accept();
        return true;
    }
    canvas_.adjustBucketTolerance(
        steps, event->position(), event->modifiers().testFlag(Qt::ShiftModifier));
    event->accept();
    return true;
}

Qt::CursorShape BucketTool::idleCursorShape(const QPointF &point) const {
    Q_UNUSED(point);
    return Qt::CrossCursor;
}

} // namespace gui
