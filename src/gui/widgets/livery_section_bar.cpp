#include "livery_section_bar.h"

#include "theme_manager.h"

#include <QBrush>
#include <QColor>
#include <QListWidgetItem>
#include <QPalette>

namespace gui {

LiverySectionBar::LiverySectionBar(QWidget *parent)
    : QListWidget(parent)
{
    setObjectName(QStringLiteral("LiverySectionBar"));
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

void LiverySectionBar::refreshTheme()
{
    const QPalette pal = paletteForTheme(currentUiTheme());
    setStyleSheet(QStringLiteral("QListWidget { background: %1; color: %2; }")
                      .arg(pal.color(QPalette::Base).name(), pal.color(QPalette::Text).name()));
}

void LiverySectionBar::setSections(const QVector<SectionInfo> &sections)
{
    switching_ = true;
    clear();

    int firstPopulatedRow = -1;
    for (int row = 0; row < sections.size(); ++row) {
        const SectionInfo &section = sections[row];
        auto *item = new QListWidgetItem(
            section.decalCount > 0 ? QStringLiteral("%1  (%2)").arg(section.name).arg(section.decalCount)
                                   : QStringLiteral("%1  (empty)").arg(section.name));
        item->setData(Qt::UserRole, section.id);
        if (section.decalCount == 0) {
            item->setForeground(QBrush(QColor(140, 140, 140)));
        } else if (firstPopulatedRow < 0) {
            firstPopulatedRow = row;
        }
        addItem(item);
    }
    setVisible(count() > 0);
    switching_ = false;

    if (count() > 0) {
        setCurrentRow(firstPopulatedRow >= 0 ? firstPopulatedRow : 0);
    }
}

} // namespace gui
