#pragma once

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMouseEvent;
class QPaintEvent;

namespace gui {

// QTreeView for the layer hierarchy: sibling-only internal drag/drop reordering
// with a custom drop-indicator line and drag cursors. All external drops (URLs)
// are refused so the main window can handle file drops instead.
class LayerTreeView final : public QTreeView {
public:
    using QTreeView::QTreeView;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;

private:
    QModelIndexList normalizedSelectedRows() const;
    QString parentIdForIndex(const QModelIndex &index) const;
    QRect visualSubtreeRect(const QModelIndex &index) const;
    void setDropIndicatorY(int y);
    bool dropTargetForPosition(const QPoint &position, QModelIndex *dropParent, int *dropRow, int *indicatorY) const;

    int dropIndicatorY_ = -1;
};

} // namespace gui
