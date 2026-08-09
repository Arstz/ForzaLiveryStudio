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
    void setLeewayGroupId(const QString &groupId);

Q_SIGNALS:
    void leewayGroupRequested(const QString &groupId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
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
    void updateDragAutoScroll(const QPoint &position);
    void stopDragAutoScroll();
    bool dropTargetForPosition(const QPoint &position, QModelIndex *dropParent, int *dropRow, int *indicatorY) const;

    int dropIndicatorY_ = -1;
    int dragScrollDirection_ = 0;
    QPoint lastDragPosition_;
    QTimer dragScrollTimer_;
    QString leewayGroupId_;
};

} // namespace gui
