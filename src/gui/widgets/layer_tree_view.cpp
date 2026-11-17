#include "layer_tree_view.h"

#include "drag_cursors.h"
#include "layer_state_delegate.h"
#include "layer_tree_model.h"
#include "theme_manager.h"

#include <algorithm>

namespace gui {

namespace {
constexpr int kDragHotspotClamp = 24;
constexpr int kDragScrollMargin = 32;
constexpr int kDragScrollIntervalMs = 40;
constexpr int kBranchGlyphExtent = 8;
constexpr int kLeewayFillAlpha = 35;
constexpr int kLeewayLineWidth = 2;
const QColor kLeewayAccentColor(45, 205, 185);

bool isBadgePoint(const QRect &rowRect, const QPoint &point, bool guide) {
    const int badgeCount = guide ? 2 : 3;
    const int left = rowRect.right() - kLayerBadgeRightMargin - badgeCount * kLayerBadgeSize
        - (badgeCount - 1) * kLayerBadgeGap + 1;
    const QRect badgesRect(left, rowRect.center().y() - kLayerBadgeSize / 2,
                           badgeCount * kLayerBadgeSize + (badgeCount - 1) * kLayerBadgeGap,
                           kLayerBadgeSize);
    return badgesRect.contains(point);
}
} // namespace

LayerTreeView::LayerTreeView(QWidget *parent)
    : QTreeView(parent) {
    setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_Hover, true);
    dragScrollTimer_.setInterval(kDragScrollIntervalMs);
    connect(&dragScrollTimer_, &QTimer::timeout, this, [this]() {
        QScrollBar *scrollBar = verticalScrollBar();
        if (scrollBar == nullptr || dragScrollDirection_ == 0) {
            stopDragAutoScroll();
            return;
        }
        const int delta = std::max(1, scrollBar->singleStep()) * dragScrollDirection_;
        scrollBar->setValue(scrollBar->value() + delta);
        QModelIndex parent;
        int row = -1;
        int indicatorY = -1;
        setDropIndicatorY(dropTargetForPosition(lastDragPosition_, &parent, &row, &indicatorY)
                              ? indicatorY
                              : -1);
    });
}

void LayerTreeView::setLeewayEntryId(const QString &entryId) {
    if (leewayEntryId_ == entryId) {
        return;
    }
    leewayEntryId_ = entryId;
    viewport()->update();
}

void LayerTreeView::setExternalSelection(const QItemSelection &selection, const QModelIndex &current) {
    if (selectionModel() == nullptr) {
        return;
    }
    const QScopedValueRollback<bool> selectionGuard(syncingExternalSelection_, true);
    selectionModel()->setCurrentIndex(QModelIndex(), QItemSelectionModel::NoUpdate);
    selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selectionModel()->select(QModelIndex(), QItemSelectionModel::Select);
    selectionModel()->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
}

QItemSelectionModel::SelectionFlags LayerTreeView::selectionCommand(const QModelIndex &index, const QEvent *event) const {
    if (syncingExternalSelection_) {
        return QItemSelectionModel::NoUpdate;
    }

    return QTreeView::selectionCommand(index, event);
}

void LayerTreeView::paintEvent(QPaintEvent *event) {
    QTreeView::paintEvent(event);
    if (dropIndicatorY_ < 0) {
        return;
    }
    QPainter painter(viewport());

    painter.setPen(QPen(dropIndicatorColor(), 2));
    painter.drawLine(QPoint(0, dropIndicatorY_), QPoint(viewport()->width(), dropIndicatorY_));
}

void LayerTreeView::drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const UiColors colors = uiColors(currentUiTheme());
    const QRect rowRect = fullRowRect(index);
    const QRect itemRect = visualRect(index);
    const QRect branchRect = isRightToLeft()
        ? QRect(itemRect.right() + 1, rowRect.top(), rowRect.right() - itemRect.right(), rowRect.height())
        : QRect(rowRect.left(), rowRect.top(), itemRect.left() - rowRect.left(), rowRect.height());
    QStyleOptionViewItem itemOption(option);
    const bool selected = selectionModel() != nullptr && selectionModel()->isSelected(index);
    const bool hovered = hoveredIndex_ == index;
    const bool focused = hasFocus() && currentIndex() == index;
    const bool guide = index.data(LayerTreeModel::IsGuideRole).toBool();

    itemOption.rect = itemRect;
    itemOption.state.setFlag(QStyle::State_Selected, selected);
    itemOption.state.setFlag(QStyle::State_MouseOver, hovered);
    itemOption.state.setFlag(QStyle::State_HasFocus, focused);
    painter->save();
    painter->fillRect(rowRect, hovered ? colors.hover : selected ? colors.selection
        : guide ? colors.guide : colors.navigationBackground);
    if (!leewayEntryId_.isEmpty() && index.data(LayerTreeModel::EntryIdRole).toString() == leewayEntryId_) {
        QColor fill = kLeewayAccentColor;

        fill.setAlpha(kLeewayFillAlpha);
        painter->fillRect(rowRect, fill);
        painter->setPen(QPen(kLeewayAccentColor, kLeewayLineWidth));
        painter->drawLine(rowRect.topLeft(), rowRect.bottomLeft());
    }
    drawBranches(painter, branchRect, index);
    itemDelegateForIndex(index)->paint(painter, itemOption, index);
    painter->setPen(QPen(colors.separator, kUiControlBorderWidth));
    painter->drawLine(rowRect.topLeft(), rowRect.topRight());
    painter->drawLine(QPoint(rowRect.left(), rowRect.bottom() + 1), QPoint(rowRect.right(), rowRect.bottom() + 1));
    if (focused && !hovered) {
        painter->setPen(QPen(colors.focus, kUiControlBorderWidth));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(rowRect.adjusted(0, 0, -1, -1));
    }
    painter->restore();
}

void LayerTreeView::drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const {
    const UiColors colors = uiColors(currentUiTheme());
    const QRect itemRect = visualRect(index);
    const bool highlighted = hoveredIndex_ == index
        || (selectionModel() != nullptr && selectionModel()->isSelected(index));
    const int indent = indentation();
    const int direction = isRightToLeft() ? 1 : -1;
    const int itemEdge = isRightToLeft() ? itemRect.right() : itemRect.left();
    int depth = 0;

    if (rect.isEmpty() || indent <= 0) {
        return;
    }
    for (QModelIndex ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
        ++depth;
    }
    painter->save();
    painter->setPen(QPen(colors.separator, kUiControlBorderWidth));
    for (int level = 0; level < depth; ++level) {
        const int x = itemEdge + direction * ((depth - level) * indent - indent / 2);

        painter->drawLine(QPoint(x, rect.top()), QPoint(x, rect.bottom()));
    }
    if (itemsExpandable() && model() != nullptr && model()->hasChildren(index)) {
        const qreal halfExtent = kBranchGlyphExtent / 2.0;
        const QPolygonF glyph{{-halfExtent / 2, -halfExtent},
            {halfExtent / 2, 0}, {-halfExtent / 2, halfExtent}};

        painter->translate(itemEdge + direction * indent / 2, rect.center().y());
        painter->rotate(isExpanded(index) ? 90 : isRightToLeft() ? 180 : 0);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(highlighted ? colors.selectedText : colors.icon);
        painter->drawPolygon(glyph);
    }
    painter->restore();
}

bool LayerTreeView::viewportEvent(QEvent *event) {
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove) {
        setHoveredIndex(indexAt(static_cast<QHoverEvent *>(event)->position().toPoint()));
    } else if (event->type() == QEvent::HoverLeave || event->type() == QEvent::Leave) {
        setHoveredIndex(QModelIndex());
    }

    return QTreeView::viewportEvent(event);
}

void LayerTreeView::scrollContentsBy(int dx, int dy) {
    QTreeView::scrollContentsBy(dx, dy);
    setHoveredIndex(viewport()->underMouse()
        ? indexAt(viewport()->mapFromGlobal(QCursor::pos())) : QModelIndex());
}

void LayerTreeView::selectionChanged(const QItemSelection &selected, const QItemSelection &deselected) {
    QTreeView::selectionChanged(selected, deselected);
    viewport()->update();
}

void LayerTreeView::currentChanged(const QModelIndex &current, const QModelIndex &previous) {
    QTreeView::currentChanged(current, previous);
    if (previous.isValid()) {
        viewport()->update(fullRowRect(previous).adjusted(0, 0, 0, 1));
    }
    if (current.isValid()) {
        viewport()->update(fullRowRect(current).adjusted(0, 0, 0, 1));
    }
}

void LayerTreeView::mousePressEvent(QMouseEvent *event) {
    if (event != nullptr
        && event->button() == Qt::LeftButton
        && event->modifiers().testFlag(Qt::AltModifier)) {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (index.isValid()
            && !index.data(LayerTreeModel::IsGuideRole).toBool()) {
            const QString entryId =
                index.data(LayerTreeModel::EntryIdRole).toString();
            if (!entryId.isEmpty()) {
                Q_EMIT leewayEntryRequested(entryId);
                event->accept();
                return;
            }
        }
    }
    const QModelIndex index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid()
        && isBadgePoint(visualRect(index), event->position().toPoint(),
                        index.data(LayerTreeModel::IsGuideRole).toBool() || index.data(LayerTreeModel::IsContourRole).toBool())) {
        pressedBadgeIndex_ = index;
        event->accept();
        return;
    }
    QTreeView::mousePressEvent(event);
}

void LayerTreeView::mouseReleaseEvent(QMouseEvent *event) {
    if (pressedBadgeIndex_.isValid() && event->button() == Qt::LeftButton) {
        const QModelIndex index = pressedBadgeIndex_;
        pressedBadgeIndex_ = QPersistentModelIndex();
        if (indexAt(event->position().toPoint()) == index) {
            QStyleOptionViewItem option;
            initViewItemOption(&option);
            option.rect = visualRect(index);
            if (selectionModel()->isSelected(index)) {
                option.state |= QStyle::State_Selected;
            }
            itemDelegateForIndex(index)->editorEvent(event, model(), option, index);
        }
        event->accept();
        return;
    }
    QTreeView::mouseReleaseEvent(event);
}

void LayerTreeView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event != nullptr) {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (index.isValid()
            && isBadgePoint(visualRect(index), event->position().toPoint(),
                            index.data(LayerTreeModel::IsGuideRole).toBool() || index.data(LayerTreeModel::IsContourRole).toBool())) {
            event->accept();
            return;
        }
    }
    QTreeView::mouseDoubleClickEvent(event);
}

void LayerTreeView::dragEnterEvent(QDragEnterEvent *event) {
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    if (event != nullptr) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }
}

void LayerTreeView::dragMoveEvent(QDragMoveEvent *event) {
    if (event == nullptr) {
        return;
    }
    if (event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        stopDragAutoScroll();
        setDropIndicatorY(-1);
        event->ignore();
        return;
    }
    updateDragAutoScroll(event->position().toPoint());
    QModelIndex parent;
    int row = -1;
    int indicatorY = -1;
    if (dropTargetForPosition(event->position().toPoint(), &parent, &row, &indicatorY)
        && model()->canDropMimeData(event->mimeData(), Qt::MoveAction, row, 0, parent)) {
        setDropIndicatorY(indicatorY);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    setDropIndicatorY(-1);
    event->setDropAction(Qt::IgnoreAction);
    event->ignore();
}

void LayerTreeView::dragLeaveEvent(QDragLeaveEvent *event) {
    stopDragAutoScroll();
    setDropIndicatorY(-1);
    QTreeView::dragLeaveEvent(event);
}

void LayerTreeView::dropEvent(QDropEvent *event) {
    stopDragAutoScroll();
    if (event == nullptr || model() == nullptr) {
        return;
    }
    if (event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        setDropIndicatorY(-1);
        event->ignore();
        return;
    }
    QModelIndex parent;
    int row = -1;
    int indicatorY = -1;
    if (!dropTargetForPosition(event->position().toPoint(), &parent, &row, &indicatorY)) {
        setDropIndicatorY(-1);
        event->setDropAction(Qt::IgnoreAction);
        event->ignore();
        return;
    }
    setDropIndicatorY(-1);
    if (model()->dropMimeData(event->mimeData(), Qt::MoveAction, row, 0, parent)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    event->setDropAction(Qt::IgnoreAction);
    event->ignore();
}

void LayerTreeView::startDrag(Qt::DropActions supportedActions) {
    if (model() == nullptr) {
        return;
    }
    const QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) {
        return;
    }
    QMimeData *mime = model()->mimeData(indexes);
    if (mime == nullptr) {
        return;
    }

    auto *drag = new QDrag(this);
    drag->setMimeData(mime);

    QPixmap rowPixmap;
    const QModelIndex current = currentIndex().isValid() ? currentIndex() : indexes.front();
    const QRect rowRect = visualRect(current).intersected(viewport()->rect());
    if (rowRect.isValid() && !rowRect.isEmpty()) {
        rowPixmap = viewport()->grab(rowRect);
        drag->setPixmap(rowPixmap);
        drag->setHotSpot(QPoint(std::min(rowPixmap.width() / 2, kDragHotspotClamp),
                                std::min(rowPixmap.height() / 2, kDragHotspotClamp)));
    }

    const QPixmap cursorPixmap = dropAllowedCursorPixmap();
    const QPixmap noCursorPixmap = dropForbiddenCursorPixmap();

    drag->setDragCursor(cursorPixmap, Qt::CopyAction);
    drag->setDragCursor(cursorPixmap, Qt::MoveAction);
    drag->setDragCursor(cursorPixmap, Qt::LinkAction);
    drag->setDragCursor(noCursorPixmap, Qt::IgnoreAction);
    drag->exec(supportedActions & Qt::MoveAction ? Qt::MoveAction : supportedActions, Qt::MoveAction);
    stopDragAutoScroll();
}

void LayerTreeView::updateDragAutoScroll(const QPoint &position) {
    lastDragPosition_ = position;
    const int height = viewport()->height();
    if (position.y() < kDragScrollMargin) {
        dragScrollDirection_ = -1;
    } else if (position.y() >= height - kDragScrollMargin) {
        dragScrollDirection_ = 1;
    } else {
        stopDragAutoScroll();
        return;
    }
    if (!dragScrollTimer_.isActive()) {
        dragScrollTimer_.start();
    }
}

void LayerTreeView::stopDragAutoScroll() {
    dragScrollDirection_ = 0;
    dragScrollTimer_.stop();
}

QRect LayerTreeView::fullRowRect(const QModelIndex &index) const {
    QRect rect = visualRect(index);

    rect.setLeft(viewport()->rect().left());
    rect.setRight(viewport()->rect().right());

    return rect;
}

void LayerTreeView::setHoveredIndex(const QModelIndex &index) {
    if (hoveredIndex_ == index) {
        return;
    }
    const QPersistentModelIndex previous = hoveredIndex_;

    hoveredIndex_ = index;
    if (previous.isValid()) {
        viewport()->update(fullRowRect(previous).adjusted(0, 0, 0, 1));
    }
    if (hoveredIndex_.isValid()) {
        viewport()->update(fullRowRect(hoveredIndex_).adjusted(0, 0, 0, 1));
    }
}

QModelIndexList LayerTreeView::normalizedSelectedRows() const {
    const QModelIndexList selected = selectionModel() == nullptr ? QModelIndexList{} : selectionModel()->selectedRows();
    QSet<QString> selectedIds;
    for (const QModelIndex &index : selected) {
        const QString entryId = index.data(LayerTreeModel::EntryIdRole).toString();
        if (!entryId.isEmpty()) {
            selectedIds.insert(entryId);
        }
    }

    QModelIndexList normalized;
    for (const QModelIndex &index : selected) {
        if (!index.isValid()) {
            continue;
        }
        bool hasSelectedAncestor = false;
        for (QModelIndex ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
            const QString ancestorId = ancestor.data(LayerTreeModel::EntryIdRole).toString();
            if (!ancestorId.isEmpty() && selectedIds.contains(ancestorId)) {
                hasSelectedAncestor = true;
                break;
            }
        }
        if (!hasSelectedAncestor) {
            normalized.push_back(index);
        }
    }
    return normalized;
}

QString LayerTreeView::parentIdForIndex(const QModelIndex &index) const {
    return index.parent().isValid()
        ? index.parent().data(LayerTreeModel::EntryIdRole).toString()
        : QString();
}

QRect LayerTreeView::visualSubtreeRect(const QModelIndex &index) const {
    QRect rect = visualRect(index);
    if (!index.isValid() || !isExpanded(index)) {
        return rect;
    }
    const int rows = model() == nullptr ? 0 : model()->rowCount(index);
    for (int row = 0; row < rows; ++row) {
        const QRect childRect = visualSubtreeRect(model()->index(row, 0, index));
        if (childRect.isValid()) {
            rect = rect.united(childRect);
        }
    }
    return rect;
}

void LayerTreeView::setDropIndicatorY(int y) {
    if (dropIndicatorY_ == y) {
        return;
    }
    dropIndicatorY_ = y;
    viewport()->update();
}

bool LayerTreeView::dropTargetForPosition(const QPoint &position, QModelIndex *dropParent, int *dropRow, int *indicatorY) const {
    if (model() == nullptr || selectionModel() == nullptr || normalizedSelectedRows().isEmpty()) {
        return false;
    }
    QModelIndex target = indexAt(position);
    if (!target.isValid()) {
        *dropParent = QModelIndex();
        *dropRow = model()->rowCount();
        *indicatorY = viewport()->height() - 2;
        return true;
    }
    QRect rect = visualRect(target);
    while (target.parent().isValid() && position.x() < rect.left() - indentation() / 2) {
        target = target.parent();
        rect = visualRect(target);
    }
    constexpr int kDropZoneDivisor = 4;
    const bool inside = target.data(LayerTreeModel::IsGroupRole).toBool()
        && position.x() >= rect.left()
        && position.y() >= rect.top() + rect.height() / kDropZoneDivisor
        && position.y() <= rect.bottom() - rect.height() / kDropZoneDivisor;
    if (inside) {
        *dropParent = target;
        *dropRow = 0;
        *indicatorY = rect.bottom();
    } else {
        const bool below = position.y() >= rect.center().y();
        *dropParent = target.parent();
        *dropRow = target.row() + (below ? 1 : 0);
        *indicatorY = below ? visualSubtreeRect(target).bottom() : rect.top();
    }

    return true;
}

} // namespace gui
