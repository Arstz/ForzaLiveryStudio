#include "layer_state_delegate.h"

#include "editor_state.h"
#include "gui_assets.h"
#include "layer_tree_model.h"
#include "theme_manager.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <algorithm>

namespace gui {
namespace {

constexpr int kPositionGap = 6;
constexpr int kThumbnailPadding = 4;
constexpr int kPositionTextAlpha = 150;

QPixmap badgePixmap(const QString &fileName) {
    QPixmap pixmap(iconAssetPath(fileName));
    if (pixmap.isNull()) {
        return {};
    }
    return pixmap.scaled(kLayerBadgeSize, kLayerBadgeSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
    const UiColors colors = uiColors(currentUiTheme());
    QStyleOptionViewItem itemOption(option);
    initStyleOption(&itemOption, index);
    const QString positionText = index.data(LayerTreeModel::PositionTextRole).toString();
    const bool isGuide = index.data(LayerTreeModel::IsGuideRole).toBool();
    const bool isContour = index.data(LayerTreeModel::IsContourRole).toBool();
    const bool highlighted = option.state.testFlag(QStyle::State_Selected)
        || option.state.testFlag(QStyle::State_MouseOver);
    const bool visible = index.data(LayerTreeModel::VisibleRole).toBool();
    const bool mask = index.data(LayerTreeModel::MaskRole).toBool();
    const bool locked = index.data(LayerTreeModel::EffectiveLockedRole).toBool();
    const int badgeCount = isGuide || isContour ? 2 : 3;
    const int badgesBlock = kLayerBadgeRightMargin + kLayerBadgeSize * badgeCount
        + kLayerBadgeGap * (badgeCount - 1);
    const int positionWidth = positionText.isEmpty()
        ? 0
        : itemOption.fontMetrics.horizontalAdvance(positionText) + kPositionGap * 2;
    const int thumbnailExtent = std::max(0,
        std::min(itemOption.decorationSize.width(), option.rect.height() - 2 * kThumbnailPadding));
    const QRect thumbnailRect(option.rect.left() + kThumbnailPadding,
        option.rect.top() + (option.rect.height() - thumbnailExtent) / 2, thumbnailExtent, thumbnailExtent);
    const QRect labelRect(thumbnailRect.right() + 1 + kUiSpacing, option.rect.top(),
        std::max(0, option.rect.right() - badgesBlock - positionWidth - thumbnailRect.right() - kUiSpacing),
        option.rect.height());
    const QColor fg = !option.state.testFlag(QStyle::State_Enabled) ? colors.disabledText
        : highlighted ? colors.selectedText : itemOption.palette.color(QPalette::Text);

    painter->save();
    painter->setFont(itemOption.font);
    painter->fillRect(thumbnailRect, colors.preview);
    itemOption.icon.paint(painter,
        thumbnailRect.adjusted(kUiControlBorderWidth, kUiControlBorderWidth, -kUiControlBorderWidth, -kUiControlBorderWidth),
        Qt::AlignCenter, QIcon::Normal);
    painter->setPen(QPen(colors.border, kUiControlBorderWidth));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(thumbnailRect.adjusted(0, 0, -1, -1));
    painter->setPen(fg);
    painter->drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft,
        itemOption.fontMetrics.elidedText(itemOption.text, itemOption.textElideMode, labelRect.width()));
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
    if (!isGuide && !isContour) {
        drawBadge(Badge::Mask, maskOn_, mask ? 1.0 : 0.35);
    }
    drawBadge(Badge::Locked, locked ? locked_ : unlocked_, 1.0);
    painter->restore();

    if (!positionText.isEmpty()) {
        const int right = option.rect.right() - badgesBlock - kPositionGap;
        const QRect positionRect(right - positionWidth, option.rect.top(), positionWidth, option.rect.height());
        QColor positionColor = fg;
        positionColor.setAlpha(kPositionTextAlpha);
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
    if (badge == Badge::Mask && (index.data(LayerTreeModel::IsGuideRole).toBool()
        || index.data(LayerTreeModel::IsContourRole).toBool())) {
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    if (event->type() == QEvent::MouseButtonPress) {
        return true;
    }
    toggle(index, badge, option.state.testFlag(QStyle::State_Selected));
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
    const int right = rowRect.right() - kLayerBadgeRightMargin
        - slot * (kLayerBadgeSize + kLayerBadgeGap);
    const int top = rowRect.center().y() - kLayerBadgeSize / 2;
    return QRect(right - kLayerBadgeSize + 1, top, kLayerBadgeSize, kLayerBadgeSize);
}

LayerStateDelegate::Badge LayerStateDelegate::badgeAt(const QRect &rowRect, const QPoint &point) const {
    for (Badge badge : {Badge::Visible, Badge::Mask, Badge::Locked}) {
        if (badgeRect(rowRect, badge).contains(point)) {
            return badge;
        }
    }
    return Badge::None;
}

void LayerStateDelegate::toggle(const QModelIndex &index, Badge badge, bool selected) {
    const QString entryId = index.data(LayerTreeModel::EntryIdRole).toString();
    if (entryId.isEmpty() || state_->project() == nullptr) {
        return;
    }
    EditorCommand command;
    command.entries = selected ? state_->selectionEntries() : QVector<QString>{entryId};
    if (command.entries.isEmpty()) {
        command.entries = {entryId};
    }
    switch (badge) {
    case Badge::Visible: command.property = QStringLiteral("visible"); break;
    case Badge::Mask: command.property = QStringLiteral("mask"); break;
    case Badge::Locked: command.property = QStringLiteral("locked"); break;
    case Badge::None: return;
    }
    command.value = state_->propertyCheckState(command.entries, command.property) != Qt::Checked;
    command.deferRefresh = true;
    state_->executeCommand(command);
}

} // namespace gui
