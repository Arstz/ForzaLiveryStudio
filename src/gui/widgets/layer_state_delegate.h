#pragma once

#include <QPixmap>
#include <QStyledItemDelegate>

namespace gui {

class EditorState;

class LayerStateDelegate final : public QStyledItemDelegate {
public:
    explicit LayerStateDelegate(EditorState *state, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event,
                     QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

private:
    enum class Badge {
        None,
        Visible,
        Mask,
        Locked,
    };

    QRect badgeRect(const QRect &rowRect, Badge badge) const;
    Badge badgeAt(const QRect &rowRect, const QPoint &point) const;
    void toggle(const QModelIndex &index, Badge badge);

    EditorState *state_ = nullptr;
    QPixmap visible_;
    QPixmap invisible_;
    QPixmap maskOn_;
    QPixmap locked_;
    QPixmap unlocked_;
};

} // namespace gui
