#include "theme_toolbar.h"

#include "theme_manager.h"

#include <QEvent>

namespace gui {

ThemeToolBar::ThemeToolBar(const QString &title, QWidget *parent) : QToolBar(title, parent) {
    connect(this, &QToolBar::orientationChanged, this, [this]() { applyUiToolbarMetrics(*this); });
}

bool ThemeToolBar::event(QEvent *event) {
    const bool result = QToolBar::event(event);

    switch (event->type()) {
    case QEvent::Show:
    case QEvent::StyleChange:
    case QEvent::FontChange:
        applyUiToolbarMetrics(*this);
        break;
    default:
        break;
    }

    return result;
}

} // namespace gui
