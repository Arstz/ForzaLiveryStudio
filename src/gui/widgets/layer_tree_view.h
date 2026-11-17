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

class LayerTreeView final : public QTreeView {
    Q_OBJECT

public:
    explicit LayerTreeView(QWidget *parent = nullptr);
    void setLeewayEntryId(const QString &entryId);
    void setExternalSelection(const QItemSelection &selection, const QModelIndex &current);

Q_SIGNALS:
    void leewayEntryRequested(const QString &entryId);

protected:
    QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex &index, const QEvent *event = nullptr) const override;
    void paintEvent(QPaintEvent *event) override;
    void drawRow(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override;
    bool viewportEvent(QEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void selectionChanged(const QItemSelection &selected, const QItemSelection &deselected) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;

private:
    QRect fullRowRect(const QModelIndex &index) const;
    void setHoveredIndex(const QModelIndex &index);
    QModelIndexList normalizedSelectedRows() const;
    QString parentIdForIndex(const QModelIndex &index) const;
    QRect visualSubtreeRect(const QModelIndex &index) const;
    void setDropIndicatorY(int y);
    void updateDragAutoScroll(const QPoint &position);
    void stopDragAutoScroll();
    bool dropTargetForPosition(const QPoint &position, QModelIndex *dropParent, int *dropRow, int *indicatorY) const;

    QPersistentModelIndex pressedBadgeIndex_;
    QPersistentModelIndex hoveredIndex_;
    bool syncingExternalSelection_ = false;
    int dropIndicatorY_ = -1;
    int dragScrollDirection_ = 0;
    QPoint lastDragPosition_;
    QTimer dragScrollTimer_;
    QString leewayEntryId_;
};

} // namespace gui
