#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

using namespace pc_detail;


void ProjectCanvas::refreshHover(const QPointF &point, Qt::KeyboardModifiers modifiers) {
    if (tool_ == QStringLiteral("pen")) {
        if (pen_.fillRunning) {
            return;
        }
        refreshPenInteractionHint(point, modifiers);
        return;
    }
    if (tool_ == QStringLiteral("lining")) {
        if (lining_.fillRunning) {
            return;
        }
        refreshLiningInteractionHint(point, modifiers);
        return;
    }
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

void ProjectCanvas::selectByMarquee(Qt::KeyboardModifiers modifiers) {
    if (project_ == nullptr) {
        return;
    }
    QSet<QString> ids = (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) && state_ != nullptr
        ? state_->selectedLayerIds()
        : QSet<QString>{};
    for (const HitEntry &entry : hitEntries()) {
        if (drag_.marqueeRect.contains(entry.screenBounds.center())) {
            ids.insert(entry.layerId);
        }
    }
    if (state_ != nullptr) {
        QSet<QString> guideIds;
        forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &) {
            if (guide.visible) {
                const QRectF bounds = screenQuad(world, sceneLocalRect(guide, geometry_)).boundingRect();
                if (drag_.marqueeRect.contains(bounds.center())) {
                    guideIds.insert(guide.id);
                }
            }
            return true;
        }, /*reverse=*/false);
        state_->setSelectionIds(ids, guideIds);
    }
}

ProjectCanvas::GuidelineOrientation ProjectCanvas::rulerAt(const QPointF &point) const {
    if (point.x() >= kRulerExtent && point.x() < width()
        && point.y() >= 0.0 && point.y() < kRulerExtent) {
        return GuidelineOrientation::Vertical;
    }
    if (point.x() >= 0.0 && point.x() < kRulerExtent
        && point.y() >= kRulerExtent && point.y() < height()) {
        return GuidelineOrientation::Horizontal;
    }
    return GuidelineOrientation::None;
}

int ProjectCanvas::guidelineAtRuler(const QPointF &point, GuidelineOrientation orientation) const {
    if (!guidelines_.visible || project_ == nullptr || orientation == GuidelineOrientation::None) {
        return -1;
    }
    const QVector<double> &guidelines = orientation == GuidelineOrientation::Vertical
        ? project_->verticalGuidelines
        : project_->horizontalGuidelines;
    int closestIndex = -1;
    double closestDistance = kGuidelineHitRadius + 1.0;
    for (int index = 0; index < guidelines.size(); ++index) {
        const QPointF screen = worldToScreen(QPointF(
            orientation == GuidelineOrientation::Vertical ? guidelines[index] : 0.0,
            orientation == GuidelineOrientation::Horizontal ? guidelines[index] : 0.0));
        const double distance = orientation == GuidelineOrientation::Vertical
            ? std::abs(screen.x() - point.x())
            : std::abs(screen.y() - point.y());
        if (distance <= kGuidelineHitRadius && distance < closestDistance) {
            closestDistance = distance;
            closestIndex = index;
        }
    }
    return closestIndex;
}

double ProjectCanvas::guidelineCoordinateAt(const QPointF &point, GuidelineOrientation orientation) const {
    const QPointF world = screenToWorld(point);
    return orientation == GuidelineOrientation::Vertical ? world.x() : world.y();
}

bool ProjectCanvas::handleRulerPress(QMouseEvent *event) {
    const GuidelineOrientation orientation = rulerAt(event->position());
    if (project_ == nullptr) {
        return false;
    }
    if (orientation == GuidelineOrientation::None) {
        const bool corner = event->position().x() >= 0.0 && event->position().x() < kRulerExtent
            && event->position().y() >= 0.0 && event->position().y() < kRulerExtent;
        if (!corner || (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)) {
            return false;
        }
        guidelines_.rulerPressActive = event->button() == Qt::LeftButton;
        event->accept();
        return true;
    }

    const int guidelineIndex = guidelineAtRuler(event->position(), orientation);
    if (event->button() == Qt::RightButton) {
        if (!guidelines_.locked && guidelineIndex >= 0) {
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
    guidelines_.rulerPressActive = true;

    if ((event->modifiers() & Qt::AltModifier) && guidelines_.visible && !guidelines_.locked) {
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

    if (!guidelines_.locked && guidelineIndex >= 0) {
        if (state_ != nullptr) {
            state_->beginProjectEdit();
        }
        guidelines_.draggedOrientation = orientation;
        guidelines_.draggedIndex = guidelineIndex;
        updateCursorForPoint(event->position());
    }
    event->accept();
    return true;
}

void ProjectCanvas::mousePressEvent(QMouseEvent *event) {
    setFocus();
    updateViewTransform();
    if (handleRulerPress(event)) {
        return;
    }
    drag_.startScreen = event->position();
    drag_.lastScreen = event->position();
    drag_.startWorld = screenToWorld(drag_.startScreen);
    drag_.startSelectionBounds = selectedScreenBounds();

    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && spaceDown_)) {
        drag_.mode = DragMode::Pan;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (project_ != nullptr
        && (tool_ == QStringLiteral("pen") || tool_ == QStringLiteral("lining"))
        && event->button() != Qt::LeftButton
        && activeTool_ != nullptr
        && activeTool_->handlePress(event)) {
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
            drag_.startSelectionBounds = selectedScreenBounds();
            return true;
        }
        const QString guideTarget = guideAtScreenPoint(event->position());
        if (!guideTarget.isEmpty()) {
            state_->setSelectionIds({}, {guideTarget});
            drag_.startSelectionBounds = selectedScreenBounds();
            return true;
        }
        state_->clearSelection();
        return false;
    };

    if (tool_ == QStringLiteral("move")
        && options_.moveToolAutoSelect
        && !selectedScreenBounds().contains(event->position())) {
        if (!selectMoveAutoTarget()) {
            event->accept();
            return;
        }
    }

    if (tool_ == QStringLiteral("move")
        && !options_.moveToolAutoSelect
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
            drag_.startSelectionBounds = selectedScreenBounds();
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
        drag_.startSelectionBounds = selectedScreenBounds();
    }
    if (state_->selectedLayerIds().isEmpty() && state_->selectedGuideLayerIds().isEmpty()) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    drag_.startBox = currentSelectionBox();
    const QPointF boxCenterWorld = drag_.startBox.valid
        ? drag_.startBox.localToWorld.map(drag_.startBox.localRect.center())
        : screenToWorld(drag_.startSelectionBounds.center());
    beginToolDrag(event->position(), boxCenterWorld);

    if (drag_.mode != DragMode::None) {
        drag_.duplicated = false;
        drag_.usesProjectEdit = (event->modifiers() & Qt::AltModifier)
            && (drag_.mode == DragMode::Move || drag_.mode == DragMode::TransformMove);
        if (drag_.usesProjectEdit) {
            state_->beginProjectEdit();
            const QVector<QString> entries = state_->selectedTransformTargetIds();
            QSet<QString> newLayerSel;
            QSet<QString> newGuideSel;
            if (state_->duplicateEntriesInPlace(entries, &newLayerSel, &newGuideSel)
                && (!newLayerSel.isEmpty() || !newGuideSel.isEmpty())) {
                state_->setSelectionIds(newLayerSel, newGuideSel);
                drag_.duplicated = true;
            }
        }
        captureDragStarts();
        if (!drag_.usesProjectEdit) {
            state_->beginTransformCommand(dragTransformTargetIds());
        }
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void ProjectCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (guidelines_.draggedOrientation != GuidelineOrientation::None) {
        updateViewTransform();
        QVector<double> &guidelines = guidelines_.draggedOrientation == GuidelineOrientation::Vertical
            ? project_->verticalGuidelines
            : project_->horizontalGuidelines;
        if (guidelines_.draggedIndex >= 0 && guidelines_.draggedIndex < guidelines.size()) {
            guidelines[guidelines_.draggedIndex] =
                guidelineCoordinateAt(event->position(), guidelines_.draggedOrientation);
            update();
        }
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (drag_.mode == DragMode::Marquee) {
        updateCursorForPoint(event->position());
        const QRectF nextRect = QRectF(drag_.startScreen, event->position()).normalized();
        if (nextRect != drag_.marqueeRect) {
            drag_.marqueeRect = nextRect;
            update();
        }
        event->accept();
        return;
    }

    updateViewTransform();
    if (drag_.mode == DragMode::Pan) {
        updateCursorForPoint(event->position());
        const QPointF delta = event->position() - drag_.lastScreen;
        camera_.adjustPan(delta);
        drag_.lastScreen = event->position();
        invalidateSceneCache();
        update();
        event->accept();
        return;
    }
    if (drag_.mode == DragMode::Move || drag_.mode == DragMode::TransformMove) {
        applyMoveDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (drag_.mode == DragMode::Scale) {
        applyScaleDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (drag_.mode == DragMode::Skew) {
        applySkewDrag(event->position());
        event->accept();
        return;
    }
    if (drag_.mode == DragMode::Rotate) {
        applyRotateDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (activeTool_ != nullptr && activeTool_->handleMove(event)) {
        return;
    }
    updateCursorForPoint(event->position());
    refreshHover(event->position(), event->modifiers());
    QOpenGLWidget::mouseMoveEvent(event);
}

void ProjectCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton
        && guidelines_.draggedOrientation != GuidelineOrientation::None) {
        if (state_ != nullptr) {
            state_->commitProjectEdit();
        }
        guidelines_.draggedOrientation = GuidelineOrientation::None;
        guidelines_.draggedIndex = -1;
        guidelines_.rulerPressActive = false;
        updateCursorForPoint(event->position());
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && guidelines_.rulerPressActive) {
        guidelines_.rulerPressActive = false;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (activeTool_ != nullptr && activeTool_->handleRelease(event)) {
        return;
    }

    updateViewTransform();
    if (drag_.mode == DragMode::Pan && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        drag_.mode = DragMode::None;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && drag_.mode != DragMode::None) {
        finishDrag();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void ProjectCanvas::mouseDoubleClickEvent(QMouseEvent *event) {
    updateViewTransform();
    if (activeTool_ != nullptr && activeTool_->handleDoubleClick(event)) {
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void ProjectCanvas::wheelEvent(QWheelEvent *event) {
    updateViewTransform();
    const bool editToolZoom = (event->modifiers() & Qt::AltModifier)
        && (tool_ == QStringLiteral("bucket") || tool_ == QStringLiteral("lining"));
    if (!editToolZoom && activeTool_ != nullptr && activeTool_->handleWheel(event)) {
        return;
    }
    const QPointF anchorWorld = screenToWorld(event->position());
    const QPoint wheelDelta = event->angleDelta();
    const int notch = wheelDelta.y() != 0 ? wheelDelta.y() : wheelDelta.x();
    if (notch == 0) {
        event->accept();
        return;
    }
    const double factor = notch > 0 ? 1.15 : 1.0 / 1.15;
    camera_.setZoom(std::clamp(camera_.zoom() * factor, 0.1, 100.0));
    updateViewTransform();
    camera_.adjustPan(event->position() - worldToScreen(anchorWorld));
    invalidateSceneCache();
    update();
    event->accept();
}

bool ProjectCanvas::handleKeyBinding(KeyInteraction interaction, KeyEventPhase phase, bool autoRepeat) {
    if (interaction == KeyInteraction::CanvasPan) {
        if (autoRepeat) {
            return true;
        }
        spaceDown_ = phase == KeyEventPhase::Press;
        if (phase == KeyEventPhase::Release && drag_.mode == DragMode::Pan) {
            drag_.mode = DragMode::None;
        }
        updateCursorForPoint(mapFromGlobal(QCursor::pos()));

        return true;
    }
    if (phase != KeyEventPhase::Press) {
        return false;
    }
    if (interaction == KeyInteraction::CanvasRemovePathPoint
        && tool_ == QStringLiteral("pen") && !pen_.fillRunning && !pen_.closed) {
        if (!pen_.points.isEmpty()) {
            pen_.points.removeLast();
            pen_.crossings.clear();
            pen_.error.clear();
            clearCursorHint();
            update();
        }

        return true;
    }
    if (interaction == KeyInteraction::CanvasRemovePathPoint
        && tool_ == QStringLiteral("lining") && !lining_.fillRunning && !lining_.closed) {
        if (!lining_.points.isEmpty()) {
            lining_.points.removeLast();
            lining_.error.clear();
            clearCursorHint();
            update();
        }

        return true;
    }
    if (interaction == KeyInteraction::CanvasCommitInteraction
        && tool_ == QStringLiteral("lining") && lining_.closed && !lining_.fillRunning) {
        requestLiningFill();

        return true;
    }
    if (interaction == KeyInteraction::CanvasCommitInteraction
        && tool_ == QStringLiteral("pen") && pen_.closed && !pen_.fillRunning) {
        closePenPath();

        return true;
    }
    if (interaction == KeyInteraction::CanvasCommitInteraction
        && tool_ == QStringLiteral("transform")) {
        setTool(QStringLiteral("select"));

        return true;
    }
    if (interaction == KeyInteraction::CanvasCancelInteraction) {
        if (tool_ == QStringLiteral("pen")) {
            cancelPenInteraction();
            return true;
        }
        if (tool_ == QStringLiteral("lining")) {
            cancelLiningInteraction();
            return true;
        }
        if (tool_ == QStringLiteral("bucket")) {
            clearBucketPreview();
            return true;
        }
        if (tool_ == QStringLiteral("select")) {
            if (state_ != nullptr) {
                state_->clearSelection();
            }
            hoverLayerId_.clear();
            hoverPolygon_ = {};
            updateSelectionFlashState();
            update();
            return true;
        }
        if (guidelines_.draggedOrientation != GuidelineOrientation::None) {
            if (state_ != nullptr) {
                state_->cancelProjectEdit();
            }
            guidelines_.draggedOrientation = GuidelineOrientation::None;
            guidelines_.draggedIndex = -1;
            guidelines_.rulerPressActive = false;
            updateCursorForPoint(mapFromGlobal(QCursor::pos()));
            update();
            return true;
        }
        if (guidelines_.rulerPressActive) {
            guidelines_.rulerPressActive = false;
            updateCursorForPoint(mapFromGlobal(QCursor::pos()));
            return true;
        }
        cancelDrag();
        if (tool_ == QStringLiteral("transform")) {
            setTool(QStringLiteral("select"));
        }

        return true;
    }
    const bool nudgeTool = tool_ == QStringLiteral("move") || tool_ == QStringLiteral("transform");
    if (!nudgeTool || drag_.mode != DragMode::None) {
        return false;
    }
    const bool fast = interaction == KeyInteraction::CanvasNudgeLeftFast
        || interaction == KeyInteraction::CanvasNudgeRightFast
        || interaction == KeyInteraction::CanvasNudgeUpFast
        || interaction == KeyInteraction::CanvasNudgeDownFast;
    const double step = fast ? options_.nudgeShiftStep : options_.nudgeStep;
    QPointF delta;
    switch (interaction) {
    case KeyInteraction::CanvasNudgeLeft:
    case KeyInteraction::CanvasNudgeLeftFast:
        delta = QPointF(-step, 0.0);
        break;
    case KeyInteraction::CanvasNudgeRight:
    case KeyInteraction::CanvasNudgeRightFast:
        delta = QPointF(step, 0.0);
        break;
    case KeyInteraction::CanvasNudgeUp:
    case KeyInteraction::CanvasNudgeUpFast:
        delta = QPointF(0.0, step);
        break;
    case KeyInteraction::CanvasNudgeDown:
    case KeyInteraction::CanvasNudgeDownFast:
        delta = QPointF(0.0, -step);
        break;
    default:
        return false;
    }

    return nudgeSelection(delta);
}

bool ProjectCanvas::focusNextPrevChild(bool next) {
    Q_UNUSED(next);
    return false;
}

void ProjectCanvas::leaveEvent(QEvent *event) {
    hoverLayerId_.clear();
    hoverPolygon_ = {};
    clearCursorHint();
    if (tool_ == QStringLiteral("bucket")) {
        clearBucketPreview();
    }
    unsetCursor();
    update();
    QOpenGLWidget::leaveEvent(event);
}

} // namespace gui
