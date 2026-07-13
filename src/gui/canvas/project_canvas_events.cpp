#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

using namespace pc_detail;


void ProjectCanvas::refreshHover(const QPointF &point, Qt::KeyboardModifiers modifiers)
{
    if (tool_ != QStringLiteral("select")) {
        return;
    }
    const QString id = selectTargetAtScreenPoint(point, modifiers);
    if (id == hoverLayerId_) {
        return;
    }
    hoverLayerId_ = id;
    hoverPolygon_ = {};
    for (const HitEntry &entry : hitEntries()) {
        if (entry.layerId == id) {
            hoverPolygon_ = entry.screenPolygon;
            break;
        }
    }
    update();
}

void ProjectCanvas::selectByMarquee(Qt::KeyboardModifiers modifiers)
{
    if (project_ == nullptr) {
        return;
    }
    QSet<QString> ids = (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) && state_ != nullptr
        ? state_->selectedLayerIds()
        : QSet<QString>{};
    for (const HitEntry &entry : hitEntries()) {
        if (marqueeRect_.contains(entry.screenBounds.center())) {
            ids.insert(entry.layerId);
        }
    }
    if (state_ != nullptr) {
        QSet<QString> guideIds;
        forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &) {
            if (guide.visible) {
                const QRectF bounds = screenQuad(world, sceneLocalRect(guide, geometry_)).boundingRect();
                if (marqueeRect_.contains(bounds.center())) {
                    guideIds.insert(guide.id);
                }
            }
            return true;
        }, /*reverse=*/false);
        state_->setSelectionIds(ids, guideIds);
    }
}

void ProjectCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    updateViewTransform();
    dragStartScreen_ = event->position();
    dragLastScreen_ = event->position();
    dragStartWorld_ = screenToWorld(dragStartScreen_);
    dragStartSelectionBounds_ = selectedScreenBounds();

    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && spaceDown_)) {
        dragMode_ = DragMode::Pan;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || project_ == nullptr) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    if (activeTool_ != nullptr && activeTool_->handlePress(event)) {
        return;
    }

    if (state_ == nullptr) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    const bool picksUnderCursor = activeTool_ != nullptr && activeTool_->picksUnderCursor();

    auto selectMoveAutoTarget = [this, event]() -> bool {
        const QString target = selectTargetAtScreenPoint(event->position(), {});
        if (!target.isEmpty()) {
            const QString groupId = state_->topmostGroupForEntry(target);
            if (!groupId.isEmpty()) {
                QSet<QString> leafIds;
                for (const QString &id : state_->leafLayerIdsForEntry(groupId)) {
                    leafIds.insert(id);
                }
                state_->setSelectionIds(leafIds, {});
            } else {
                state_->setSelectionIds({target}, {});
            }
            dragStartSelectionBounds_ = selectedScreenBounds();
            return true;
        }
        const QString guideTarget = guideAtScreenPoint(event->position());
        if (!guideTarget.isEmpty()) {
            state_->setSelectionIds({}, {guideTarget});
            dragStartSelectionBounds_ = selectedScreenBounds();
            return true;
        }
        state_->clearSelection();
        return false;
    };

    if (tool_ == QStringLiteral("move")
        && moveToolAutoSelect_
        && !selectedScreenBounds().contains(event->position())) {
        if (!selectMoveAutoTarget()) {
            event->accept();
            return;
        }
    }

    if (tool_ == QStringLiteral("move")
        && !moveToolAutoSelect_
        && (!state_->selectedLayerIds().isEmpty() || !state_->selectedGuideLayerIds().isEmpty())
        && !selectedScreenBounds().contains(event->position())) {
        event->accept();
        return;
    }

    if (picksUnderCursor
        && state_->selectedLayerIds().isEmpty()
        && !state_->selectedGuideLayerIds().isEmpty()) {
        const QString target = selectTargetAtScreenPoint(event->position(), {});
        if (!target.isEmpty()) {
            state_->selectLayerAtPoint(target, {});
            dragStartSelectionBounds_ = selectedScreenBounds();
        }
    }

    if (state_->selectedLayerIds().isEmpty() && state_->selectedGuideLayerIds().isEmpty()
        && picksUnderCursor) {
        const QString target = selectTargetAtScreenPoint(event->position(), {});
        const QString guideTarget = target.isEmpty() ? guideAtScreenPoint(event->position()) : QString();
        if (target.isEmpty() && guideTarget.isEmpty()) {
            QOpenGLWidget::mousePressEvent(event);
            return;
        }
        if (!target.isEmpty()) {
            state_->selectLayerAtPoint(target, {});
        } else {
            state_->setSelectionIds({}, {guideTarget});
        }
        dragStartSelectionBounds_ = selectedScreenBounds();
    }
    if (state_->selectedLayerIds().isEmpty() && state_->selectedGuideLayerIds().isEmpty()) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    dragStartBox_ = currentSelectionBox();
    const QPointF boxCenterWorld = dragStartBox_.valid
        ? dragStartBox_.localToWorld.map(dragStartBox_.localRect.center())
        : screenToWorld(dragStartSelectionBounds_.center());
    beginToolDrag(event->position(), boxCenterWorld);

    if (dragMode_ != DragMode::None) {
        dragDuplicated_ = false;
        dragUsesProjectEdit_ = (event->modifiers() & Qt::AltModifier)
            && (dragMode_ == DragMode::Move || dragMode_ == DragMode::TransformMove);
        if (dragUsesProjectEdit_) {
            state_->beginProjectEdit();
            QVector<QString> entries;
            for (const QString &id : state_->selectedLayerIds()) {
                entries.push_back(id);
            }
            for (const QString &id : state_->selectedGuideLayerIds()) {
                entries.push_back(id);
            }
            QSet<QString> newLayerSel;
            QSet<QString> newGuideSel;
            if (state_->duplicateEntriesInPlace(entries, &newLayerSel, &newGuideSel)
                && (!newLayerSel.isEmpty() || !newGuideSel.isEmpty())) {
                state_->setSelectionIds(newLayerSel, newGuideSel);
                dragDuplicated_ = true;
            }
        }
        captureDragStarts();
        if (!dragUsesProjectEdit_) {
            state_->beginTransformCommand(dragTransformTargetIds());
        }
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void ProjectCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (dragMode_ == DragMode::Marquee) {
        updateCursorForPoint(event->position());
        const QRectF nextRect = QRectF(dragStartScreen_, event->position()).normalized();
        if (nextRect != marqueeRect_) {
            marqueeRect_ = nextRect;
            update();
        }
        event->accept();
        return;
    }

    updateViewTransform();
    if (dragMode_ == DragMode::Pan) {
        updateCursorForPoint(event->position());
        const QPointF delta = event->position() - dragLastScreen_;
        pan_ += delta;
        dragLastScreen_ = event->position();
        invalidateSceneCache();
        update();
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Move || dragMode_ == DragMode::TransformMove) {
        applyMoveDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Scale) {
        applyScaleDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Skew) {
        applySkewDrag(event->position());
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Rotate) {
        applyRotateDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    updateCursorForPoint(event->position());
    refreshHover(event->position(), event->modifiers());
    QOpenGLWidget::mouseMoveEvent(event);
}

void ProjectCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (activeTool_ != nullptr && activeTool_->handleRelease(event)) {
        return;
    }

    updateViewTransform();
    if (dragMode_ == DragMode::Pan && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        dragMode_ = DragMode::None;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && dragMode_ != DragMode::None) {
        finishDrag();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void ProjectCanvas::wheelEvent(QWheelEvent *event)
{
    updateViewTransform();
    const QPointF anchorWorld = screenToWorld(event->position());
    const QPoint wheelDelta = event->angleDelta();
    const int notch = wheelDelta.y() != 0 ? wheelDelta.y() : wheelDelta.x();
    if (notch == 0) {
        event->accept();
        return;
    }
    const double factor = notch > 0 ? 1.15 : 1.0 / 1.15;
    zoom_ = std::clamp(zoom_ * factor, 0.1, 100.0);
    updateViewTransform();
    pan_ += event->position() - worldToScreen(anchorWorld);
    invalidateSceneCache();
    update();
    event->accept();
}

void ProjectCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceDown_ = true;
        updateCursorForPoint(mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        cancelDrag();
        if (tool_ == QStringLiteral("transform")) {
            setTool(QStringLiteral("select"));
        }
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && tool_ == QStringLiteral("transform")) {
        setTool(QStringLiteral("select"));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
        if (!event->isAutoRepeat()) {
            cycleFlipSelection();
        }
        event->accept();
        return;
    }
    const bool nudgeTool = tool_ == QStringLiteral("move") || tool_ == QStringLiteral("transform");
    if (nudgeTool && dragMode_ == DragMode::None) {
        QPointF delta;
        const double step = (event->modifiers() & Qt::ShiftModifier) ? nudgeShiftStep_ : nudgeStep_;
        switch (event->key()) {
        case Qt::Key_Left:
            delta = QPointF(-step, 0.0);
            break;
        case Qt::Key_Right:
            delta = QPointF(step, 0.0);
            break;
        case Qt::Key_Up:
            delta = QPointF(0.0, step);
            break;
        case Qt::Key_Down:
            delta = QPointF(0.0, -step);
            break;
        default:
            break;
        }
        if (!delta.isNull() && nudgeSelection(delta)) {
            event->accept();
            return;
        }
    }
    QOpenGLWidget::keyPressEvent(event);
}

bool ProjectCanvas::focusNextPrevChild(bool next)
{
    Q_UNUSED(next);
    return false;
}

void ProjectCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceDown_ = false;
        if (dragMode_ == DragMode::Pan) {
            dragMode_ = DragMode::None;
        }
        updateCursorForPoint(mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void ProjectCanvas::leaveEvent(QEvent *event)
{
    hoverLayerId_.clear();
    hoverPolygon_ = {};
    clearCursorHint();
    unsetCursor();
    update();
    QOpenGLWidget::leaveEvent(event);
}

} // namespace gui
