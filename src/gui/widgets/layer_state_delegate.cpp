#include "layer_state_delegate.h"

#include "editor_state.h"
#include "layer_tree_model.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int BadgeSize = 18;
constexpr int BadgeGap = 5;
constexpr int BadgeRightMargin = 8;
constexpr int PositionGap = 6;

QString assetPath(const QString &fileName) {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    QStringList candidates;
    candidates << QDir(appDir).filePath(QStringLiteral("assets/%1").arg(fileName))
               << QDir(cwd).filePath(QStringLiteral("assets/%1").arg(fileName))
               << QDir(cwd).filePath(QStringLiteral("cpp-port/assets/%1").arg(fileName));
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return candidates.front();
}

QPixmap badgePixmap(const QString &fileName) {
    QPixmap pixmap(assetPath(fileName));
    if (pixmap.isNull()) {
        return {};
    }
    return pixmap.scaled(BadgeSize, BadgeSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QPixmap tinted(const QPixmap &source, const QColor &color) {
    if (source.isNull()) {
        return {};
    }
    QPixmap out(source.size());
    out.setDevicePixelRatio(source.devicePixelRatio());
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(out.rect(), color);
    painter.end();
    return out;
}

double relativeLuminance(const QColor &color) {
    const auto channel = [](int value) {
        const double v = value / 255.0;
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green()) + 0.0722 * channel(color.blue());
}

double contrastRatio(const QColor &a, const QColor &b) {
    const double l1 = relativeLuminance(a);
    const double l2 = relativeLuminance(b);
    return (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
}

QColor badgeForeground(const QStyleOptionViewItem &option) {
    if (!(option.state & QStyle::State_Selected)) {
        return option.palette.color(QPalette::Text);
    }
    const QColor highlight = option.palette.color(QPalette::Highlight);
    const QColor highlightedText = option.palette.color(QPalette::HighlightedText);
    const QColor text = option.palette.color(QPalette::Text);
    return contrastRatio(highlightedText, highlight) >= contrastRatio(text, highlight)
        ? highlightedText
        : text;
}

} // namespace

LayerStateDelegate::LayerStateDelegate(EditorState *state, QObject *parent)
    : QStyledItemDelegate(parent)
    , state_(state)
    , visible_(badgePixmap(QStringLiteral("PropertyVisible.xpm")))
    , invisible_(badgePixmap(QStringLiteral("PropertyInvisible.xpm")))
    , maskOn_(badgePixmap(QStringLiteral("PropertyMask.xpm")))
    , locked_(badgePixmap(QStringLiteral("PropertyLocked.xpm")))
    , unlocked_(badgePixmap(QStringLiteral("PropertyUnlocked.xpm"))) {
}

void LayerStateDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const bool isGuide = index.data(LayerTreeModel::IsGuideRole).toBool();
    if (isGuide && !(option.state & QStyle::State_Selected)) {
        painter->fillRect(option.rect, QColor(0x9c, 0x85, 0x5a));
    }
    QStyleOptionViewItem itemOption(option);
    const int badgeCount = isGuide ? 2 : 3;
    const int badgesBlock = BadgeRightMargin + BadgeSize * badgeCount + BadgeGap * (badgeCount - 1);
    const QString positionText = index.data(LayerTreeModel::PositionTextRole).toString();
    const int positionWidth = positionText.isEmpty()
        ? 0
        : option.fontMetrics.horizontalAdvance(positionText) + PositionGap * 2;
    itemOption.rect.adjust(0, 0, -(badgesBlock + positionWidth), 0);
    QStyledItemDelegate::paint(painter, itemOption, index);

    const bool visible = index.data(LayerTreeModel::VisibleRole).toBool();
    const bool mask = index.data(LayerTreeModel::MaskRole).toBool();
    const bool locked = index.data(LayerTreeModel::EffectiveLockedRole).toBool();

    const QColor fg = badgeForeground(option);

    painter->save();
    const auto drawBadge = [&](Badge badge, const QPixmap &glyph, double opacity) {
        if (glyph.isNull()) {
            return;
        }
        const QPixmap colored = tinted(glyph, fg);
        const QRect rect = badgeRect(option.rect, badge);
        const QPoint topLeft(rect.center().x() - colored.width() / 2,
                             rect.center().y() - colored.height() / 2);
        painter->setOpacity(opacity);
        painter->drawPixmap(topLeft, colored);
    };
    drawBadge(Badge::Visible, visible ? visible_ : invisible_, 1.0);
    if (!isGuide) {
        drawBadge(Badge::Mask, maskOn_, mask ? 1.0 : 0.35);
    }
    drawBadge(Badge::Locked, locked ? locked_ : unlocked_, 1.0);
    painter->restore();

    if (!positionText.isEmpty()) {
        const int right = option.rect.right() - badgesBlock - PositionGap;
        const QRect positionRect(right - positionWidth, option.rect.top(), positionWidth, option.rect.height());
        QColor positionColor = fg;
        positionColor.setAlpha(150);
        painter->save();
        painter->setPen(positionColor);
        painter->drawText(positionRect, Qt::AlignVCenter | Qt::AlignRight, positionText);
        painter->restore();
    }
}

bool LayerStateDelegate::editorEvent(QEvent *event,
                                     QAbstractItemModel *model,
                                     const QStyleOptionViewItem &option,
                                     const QModelIndex &index) {
    if ((event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease)
        || state_ == nullptr || !index.isValid()) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() != Qt::LeftButton) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    const Badge badge = badgeAt(option.rect, mouse->position().toPoint());
    if (badge == Badge::None) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    if (badge == Badge::Mask && index.data(LayerTreeModel::IsGuideRole).toBool()) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    if (event->type() == QEvent::MouseButtonPress) {
        return true;
    }
    toggle(index, badge);
    return true;
}

QRect LayerStateDelegate::badgeRect(const QRect &rowRect, Badge badge) const {
    int slot = 0;
    switch (badge) {
    case Badge::Visible:
        slot = 2;
        break;
    case Badge::Mask:
        slot = 1;
        break;
    case Badge::Locked:
        slot = 0;
        break;
    case Badge::None:
        return {};
    }
    const int right = rowRect.right() - BadgeRightMargin - slot * (BadgeSize + BadgeGap);
    const int top = rowRect.center().y() - BadgeSize / 2;
    return QRect(right - BadgeSize + 1, top, BadgeSize, BadgeSize);
}

LayerStateDelegate::Badge LayerStateDelegate::badgeAt(const QRect &rowRect, const QPoint &point) const {
    for (Badge badge : {Badge::Visible, Badge::Mask, Badge::Locked}) {
        if (badgeRect(rowRect, badge).contains(point)) {
            return badge;
        }
    }
    return Badge::None;
}

void LayerStateDelegate::toggle(const QModelIndex &index, Badge badge) {
    const QString entryId = index.data(LayerTreeModel::EntryIdRole).toString();
    if (entryId.isEmpty() || state_->project() == nullptr) {
        return;
    }
    const bool isGroup = index.data(LayerTreeModel::IsGroupRole).toBool();
    const bool isGuide = index.data(LayerTreeModel::IsGuideRole).toBool();
    const bool nextVisible = !index.data(LayerTreeModel::VisibleRole).toBool();
    const bool nextMask = !index.data(LayerTreeModel::MaskRole).toBool();
    const bool nextLocked = !index.data(LayerTreeModel::EffectiveLockedRole).toBool();

    state_->beginProjectEdit();
    if (badge == Badge::Visible) {
        if (isGuide) {
            state_->setGuideLayerVisible(entryId, nextVisible);
        } else if (isGroup) {
            state_->setGroupDescendantVisible(entryId, nextVisible);
        } else {
            state_->setLayerVisible(entryId, nextVisible);
        }
        state_->commitProjectEdit();
        state_->noteProjectGeometryChanged(true, {entryId});
        return;
    }
    if (badge == Badge::Mask) {
        if (isGuide) {
            state_->cancelProjectEdit();
            return;
        }
        if (isGroup) {
            state_->setGroupDescendantMask(entryId, nextMask);
        } else {
            state_->setLayerMask(entryId, nextMask);
        }
        state_->commitProjectEdit();
        state_->noteProjectGeometryChanged(true, {entryId});
        return;
    }

    if (isGuide) {
        state_->setGuideLayerLocked(entryId, nextLocked);
    } else if (isGroup) {
        state_->setGroupAndDescendantLocked(entryId, nextLocked);
    } else {
        state_->setLayerLockScope(entryId, nextLocked);
    }
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
}

} // namespace gui
