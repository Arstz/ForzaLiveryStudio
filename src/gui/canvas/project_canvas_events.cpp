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

ProjectCanvas::GuidelineOrientation ProjectCanvas::rulerAt(const QPointF &point) const
{
    if (point.x() >= RulerExtent && point.x() < width()
        && point.y() >= 0.0 && point.y() < RulerExtent) {
        return GuidelineOrientation::Vertical;
    }
    if (point.x() >= 0.0 && point.x() < RulerExtent
        && point.y() >= RulerExtent && point.y() < height()) {
        return GuidelineOrientation::Horizontal;
    }
    return GuidelineOrientation::None;
}

int ProjectCanvas::guidelineAtRuler(const QPointF &point, GuidelineOrientation orientation) const
{
    if (!guidelinesVisible_ || project_ == nullptr || orientation == GuidelineOrientation::None) {
        return -1;
    }
    const QVector<double> &guidelines = orientation == GuidelineOrientation::Vertical
        ? project_->verticalGuidelines
        : project_->horizontalGuidelines;
    int closestIndex = -1;
    double closestDistance = GuidelineHitRadius + 1.0;
    for (int index = 0; index < guidelines.size(); ++index) {
        const QPointF screen = worldToScreen(QPointF(
            orientation == GuidelineOrientation::Vertical ? guidelines[index] : 0.0,
            orientation == GuidelineOrientation::Horizontal ? guidelines[index] : 0.0));
        const double distance = orientation == GuidelineOrientation::Vertical
            ? std::abs(screen.x() - point.x())
            : std::abs(screen.y() - point.y());
        if (distance <= GuidelineHitRadius && distance < closestDistance) {
            closestDistance = distance;
            closestIndex = index;
        }
    }
    return closestIndex;
}

double ProjectCanvas::guidelineCoordinateAt(const QPointF &point, GuidelineOrientation orientation) const
{
    const QPointF world = screenToWorld(point);
    return orientation == GuidelineOrientation::Vertical ? world.x() : world.y();
}

bool ProjectCanvas::handleRulerPress(QMouseEvent *event)
{
    const GuidelineOrientation orientation = rulerAt(event->position());
    if (project_ == nullptr) {
        return false;
    }
    if (orientation == GuidelineOrientation::None) {
        const bool corner = event->position().x() >= 0.0 && event->position().x() < RulerExtent
            && event->position().y() >= 0.0 && event->position().y() < RulerExtent;
        if (!corner || (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)) {
            return false;
        }
        rulerPressActive_ = event->button() == Qt::LeftButton;
        event->accept();
        return true;
    }

    const int guidelineIndex = guidelineAtRuler(event->position(), orientation);
    if (event->button() == Qt::RightButton) {
        if (!guidelinesLocked_ && guidelineIndex >= 0) {
            if (state_ != nullptr) {
                state_->beginProjectEdit();
            }
            QVector<double> &guidelines = orientation == GuidelineOrientation::Vertical
                ? project_->verticalGuidelines
                : project_->horizontalGuidelines;
            guidelines.removeAt(guidelineIndex);
            if (state_ != nullptr) {
                state_->commitProjectEdit();
            }
            update();
        }
        event->accept();
        return true;
    }
    if (event->button() != Qt::LeftButton) {
        return false;
    }
    rulerPressActive_ = true;

    if ((event->modifiers() & Qt::AltModifier) && guidelinesVisible_ && !guidelinesLocked_) {
        if (state_ != nullptr) {
            state_->beginProjectEdit();
        }
        QVector<double> &guidelines = orientation == GuidelineOrientation::Vertical
            ? project_->verticalGuidelines
            : project_->horizontalGuidelines;
        guidelines.push_back(guidelineCoordinateAt(event->position(), orientation));
        if (state_ != nullptr) {
            state_->commitProjectEdit();
        }
        update();
        event->accept();
        return true;
    }

    if (!guidelinesLocked_ && guidelineIndex >= 0) {
        if (state_ != nullptr) {
            state_->beginProjectEdit();
        }
        draggedGuidelineOrientation_ = orientation;
        draggedGuidelineIndex_ = guidelineIndex;
        updateCursorForPoint(event->position());
    }
    event->accept();
    return true;
}

void ProjectCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    updateViewTransform();
    if (handleRulerPress(event)) {
        return;
    }
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
            const QVector<QString> entries = state_->selectedTransformTargetIds();
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
    if (event->button() == Qt::LeftButton
        && draggedGuidelineOrientation_ != GuidelineOrientation::None) {
        if (state_ != nullptr) {
            state_->commitProjectEdit();
        }
        draggedGuidelineOrientation_ = GuidelineOrientation::None;
        draggedGuidelineIndex_ = -1;
        rulerPressActive_ = false;
        updateCursorForPoint(event->position());
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && rulerPressActive_) {
        rulerPressActive_ = false;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
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

void ProjectCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    updateViewTransform();
    if (activeTool_ != nullptr && activeTool_->handleDoubleClick(event)) {
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
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
    if (tool_ == QStringLiteral("pen") && event->key() == Qt::Key_Backspace && !penFillRunning_) {
        if (!penPoints_.isEmpty()) {
            penPoints_.removeLast();
            penCrossings_.clear();
            penError_.clear();
            clearCursorHint();
            update();
        }
        event->accept();
        return;
    }
    if (tool_ == QStringLiteral("polygon_lasso")
        && event->key() == Qt::Key_Backspace
        && !lassoFillRunning_) {
        if (!lassoPoints_.isEmpty()) {
            lassoPoints_.removeLast();
            lassoCrossings_.clear();
            lassoError_.clear();
            clearCursorHint();
            update();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && tool_ == QStringLiteral("pen")) {
        cancelPenInteraction();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && tool_ == QStringLiteral("polygon_lasso")) {
        cancelLassoInteraction();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (draggedGuidelineOrientation_ != GuidelineOrientation::None) {
            if (state_ != nullptr) {
                state_->cancelProjectEdit();
            }
            draggedGuidelineOrientation_ = GuidelineOrientation::None;
            draggedGuidelineIndex_ = -1;
            rulerPressActive_ = false;
            updateCursorForPoint(mapFromGlobal(QCursor::pos()));
            update();
            event->accept();
            return;
        }
        if (rulerPressActive_) {
            rulerPressActive_ = false;
            updateCursorForPoint(mapFromGlobal(QCursor::pos()));
            event->accept();
            return;
        }
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
