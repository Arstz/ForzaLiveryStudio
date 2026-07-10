#include "layer_tree_view.h"

#include "drag_cursors.h"
#include "layer_tree_model.h"

#include <algorithm>

namespace gui {

namespace {
constexpr int DragHotspotClamp = 24;
constexpr int BadgeSize = 18;
constexpr int BadgeGap = 5;
constexpr int BadgeRightMargin = 8;

bool isBadgePoint(const QRect &rowRect, const QPoint &point, bool guide)
{
    const int badgeCount = guide ? 2 : 3;
    const int left = rowRect.right() - BadgeRightMargin - badgeCount * BadgeSize - (badgeCount - 1) * BadgeGap + 1;
    const QRect badgesRect(left, rowRect.center().y() - BadgeSize / 2,
                           badgeCount * BadgeSize + (badgeCount - 1) * BadgeGap,
                           BadgeSize);
    return badgesRect.contains(point);
}
} // namespace

void LayerTreeView::paintEvent(QPaintEvent *event)
{
    QTreeView::paintEvent(event);
    QPainter painter(viewport());
    const QColor text = palette().color(QPalette::Text);
    QColor lineColor = text;
    lineColor.setAlpha(70);
    painter.setPen(QPen(lineColor, 1));
    const int indent = indentation();
    if (indent > 0 && model() != nullptr) {
        for (int y = 0; y < viewport()->height();) {
            const QModelIndex index = indexAt(QPoint(0, y));
            if (!index.isValid()) {
                ++y;
                continue;
            }
            const QRect rowRect = visualRect(index);
            int depth = 0;
            for (QModelIndex ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
                ++depth;
            }
            for (int level = 0; level < depth; ++level) {
                const int x = rowRect.left() - (depth - level) * indent + indent / 2;
                painter.drawLine(QPoint(x, rowRect.top()), QPoint(x, rowRect.bottom()));
            }
            y = std::max(y + 1, rowRect.bottom() + 1);
        }
    }
    if (dropIndicatorY_ < 0) {
        return;
    }
    painter.setPen(QPen(dropIndicatorColor(), 2));
    painter.drawLine(QPoint(0, dropIndicatorY_), QPoint(viewport()->width(), dropIndicatorY_));
}

void LayerTreeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event != nullptr) {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (index.isValid()
            && isBadgePoint(visualRect(index), event->position().toPoint(), index.data(LayerTreeModel::IsGuideRole).toBool())) {
            event->accept();
            return;
        }
    }
    QTreeView::mouseDoubleClickEvent(event);
}

void LayerTreeView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    if (event != nullptr) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }
}

void LayerTreeView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event == nullptr) {
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
    if (dropTargetForPosition(event->position().toPoint(), &parent, &row, &indicatorY)) {
        setDropIndicatorY(indicatorY);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    setDropIndicatorY(-1);
    event->setDropAction(Qt::IgnoreAction);
    event->ignore();
}

void LayerTreeView::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropIndicatorY(-1);
    QTreeView::dragLeaveEvent(event);
}

void LayerTreeView::dropEvent(QDropEvent *event)
{
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

void LayerTreeView::startDrag(Qt::DropActions supportedActions)
{
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
        drag->setHotSpot(QPoint(std::min(rowPixmap.width() / 2, DragHotspotClamp),
                                std::min(rowPixmap.height() / 2, DragHotspotClamp)));
    }

    const QPixmap cursorPixmap = dropAllowedCursorPixmap();
    const QPixmap noCursorPixmap = dropForbiddenCursorPixmap();

    drag->setDragCursor(cursorPixmap, Qt::CopyAction);
    drag->setDragCursor(cursorPixmap, Qt::MoveAction);
    drag->setDragCursor(cursorPixmap, Qt::LinkAction);
    drag->setDragCursor(noCursorPixmap, Qt::IgnoreAction);
    drag->exec(supportedActions & Qt::MoveAction ? Qt::MoveAction : supportedActions, Qt::MoveAction);
}

QModelIndexList LayerTreeView::normalizedSelectedRows() const
{
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

QString LayerTreeView::parentIdForIndex(const QModelIndex &index) const
{
    return index.parent().isValid()
        ? index.parent().data(LayerTreeModel::EntryIdRole).toString()
        : QString();
}

QRect LayerTreeView::visualSubtreeRect(const QModelIndex &index) const
{
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

void LayerTreeView::setDropIndicatorY(int y)
{
    if (dropIndicatorY_ == y) {
        return;
    }
    dropIndicatorY_ = y;
    viewport()->update();
}

bool LayerTreeView::dropTargetForPosition(const QPoint &position, QModelIndex *dropParent, int *dropRow, int *indicatorY) const
{
    if (model() == nullptr || selectionModel() == nullptr || dropParent == nullptr || dropRow == nullptr || indicatorY == nullptr) {
        return false;
    }

    QString sourceParentId;
    QModelIndex sourceParentIndex;
    bool haveSourceParent = false;
    bool haveDraggedRow = false;
    QSet<QString> draggedIds;
    for (const QModelIndex &index : normalizedSelectedRows()) {
        if (!index.isValid()) {
            continue;
        }
        const QString entryId = index.data(LayerTreeModel::EntryIdRole).toString();
        if (entryId.isEmpty()) {
            continue;
        }
        const QString rowParentId = parentIdForIndex(index);
        if (!haveSourceParent) {
            sourceParentId = rowParentId;
            sourceParentIndex = index.parent();
            haveSourceParent = true;
        } else if (sourceParentId != rowParentId) {
            return false;
        }
        draggedIds.insert(entryId);
        haveDraggedRow = true;
    }
    if (!haveDraggedRow) {
        return false;
    }

    const QModelIndex target = indexAt(position);
    int targetRow = target.isValid() ? target.row() : model()->rowCount(QModelIndex());
    int y = viewport()->height() - 2;
    if (target.isValid()) {
        const QRect rect = visualRect(target);
        if (position.y() >= rect.center().y()) {
            ++targetRow;
            y = visualSubtreeRect(target).bottom();
        } else {
            y = rect.top();
        }
    }

    const QString targetParentId = target.isValid() ? parentIdForIndex(target) : QString();
    if (targetParentId != sourceParentId) {
        return false;
    }

    if (!target.isValid()) {
        if (!sourceParentId.isEmpty()) {
            return false;
        }
        *dropParent = QModelIndex();
        *dropRow = targetRow;
        *indicatorY = y;
        return true;
    }

    const QString targetId = target.data(LayerTreeModel::EntryIdRole).toString();
    if (draggedIds.contains(targetId)) {
        return false;
    }

    *dropParent = sourceParentIndex;
    *dropRow = targetRow;
    *indicatorY = y;
    return true;
}

} // namespace gui
