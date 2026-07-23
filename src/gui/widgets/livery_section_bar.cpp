#include "livery_section_bar.h"

#include "theme_manager.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QListWidgetItem>
#include <QPainter>
#include <QPalette>
#include <QStyledItemDelegate>
#include <QStyle>

namespace gui {

namespace {

constexpr int kSectionNameRole = Qt::UserRole + 1;
constexpr int kSectionCountRole = Qt::UserRole + 2;
constexpr int kOverLimitRole = Qt::UserRole + 3;

class SectionCountDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem itemOption(option);
        initStyleOption(&itemOption, index);
        const QString name = index.data(kSectionNameRole).toString();
        const int count = index.data(kSectionCountRole).toInt();
        const bool overLimit = index.data(kOverLimitRole).toBool();
        const QString suffix = count > 0 ? QStringLiteral("  (%1)").arg(count)
                                         : QStringLiteral("  (empty)");

        const QWidget *widget = itemOption.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &itemOption, widget);
        itemOption.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &itemOption, painter, widget);

        const bool selected = option.state.testFlag(QStyle::State_Selected);
        QColor textColor = selected ? option.palette.color(QPalette::HighlightedText)
                                    : option.palette.color(QPalette::Text);
        if (count == 0 && !selected) {
            textColor = QColor(140, 140, 140);
        }

        painter->save();
        painter->setClipRect(textRect);
        painter->setFont(itemOption.font);
        painter->setPen(textColor);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, name);
        const int suffixX = textRect.x() + QFontMetrics(itemOption.font).horizontalAdvance(name);
        painter->setPen(overLimit ? QColor(220, 45, 45) : textColor);
        painter->drawText(QRect(suffixX, textRect.y(), textRect.right() - suffixX + 1, textRect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, suffix);
        painter->restore();
    }
};

} // namespace

LiverySectionBar::LiverySectionBar(QWidget *parent)
    : QListWidget(parent) {
    setObjectName(QStringLiteral("LiverySectionBar"));
    setItemDelegate(new SectionCountDelegate(this));
    setVisible(false);
    refreshTheme();
    connect(this, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
                if (switching_ || current == nullptr) {
                    return;
                }
                Q_EMIT sectionActivated(current->data(Qt::UserRole).toString());
            });
}

void LiverySectionBar::refreshTheme() {
    const QPalette pal = paletteForTheme(currentUiTheme());
    setStyleSheet(QStringLiteral("QListWidget { background: %1; color: %2; }")
                      .arg(pal.color(QPalette::Base).name(), pal.color(QPalette::Text).name()));
}

void LiverySectionBar::setSections(const QVector<SectionInfo> &sections) {
    const QString previousSectionId = currentItem() != nullptr
        ? currentItem()->data(Qt::UserRole).toString()
        : QString();
    switching_ = true;
    clear();

    int firstPopulatedRow = -1;
    int previousRow = -1;
    for (int row = 0; row < sections.size(); ++row) {
        const SectionInfo &section = sections[row];
        auto *item = new QListWidgetItem(
            section.decalCount > 0 ? QStringLiteral("%1  (%2)").arg(section.name).arg(section.decalCount)
                                   : QStringLiteral("%1  (empty)").arg(section.name));
        item->setData(Qt::UserRole, section.id);
        item->setData(kSectionNameRole, section.name);
        item->setData(kSectionCountRole, section.decalCount);
        item->setData(kOverLimitRole, section.overShapeLimit);
        if (section.decalCount == 0) {
            item->setForeground(QBrush(QColor(140, 140, 140)));
        } else if (firstPopulatedRow < 0) {
            firstPopulatedRow = row;
        }
        if (!previousSectionId.isEmpty() && section.id == previousSectionId) {
            previousRow = row;
        }
        addItem(item);
    }
    setVisible(count() > 0);

    if (previousRow >= 0) {
        setCurrentRow(previousRow);
    }
    switching_ = false;
    if (count() > 0 && previousRow < 0) {
        setCurrentRow(firstPopulatedRow >= 0 ? firstPopulatedRow : 0);
    }
}

} // namespace gui
