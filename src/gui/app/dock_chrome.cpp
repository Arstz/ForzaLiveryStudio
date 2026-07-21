#include "dock_chrome.h"

#include "gui_assets.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

namespace gui {
namespace {

constexpr const char *kDockIconNameProperty = "fh6DockIconName";
constexpr const char *kDockIconLabelProperty = "fh6DockIconLabel";
constexpr const char *kDockTitleLayoutProperty = "fh6DockTitleLayout";

QToolButton *dockTitleButton(QWidget *parent, QStyle::StandardPixmap icon, const QString &toolTip) {
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setFixedSize(18, 18);
    button->setIcon(parent->style()->standardIcon(icon));
    button->setToolTip(toolTip);

    return button;
}

} // namespace

void setDockTitleIcon(QDockWidget *dock, const QString &iconName) {
    if (dock == nullptr) {
        return;
    }
    const QIcon icon = assetIcon(iconName);
    dock->setWindowIcon(icon);
    dock->setProperty(kDockIconNameProperty, iconName);

    auto *titleBar = new QWidget(dock);
    auto *layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(6, 2, 4, 2);
    layout->setSpacing(5);
    titleBar->setProperty(kDockTitleLayoutProperty, QVariant::fromValue<QObject *>(layout));

    auto *iconLabel = new QLabel(titleBar);
    iconLabel->setFixedSize(18, 18);
    iconLabel->setPixmap(icon.pixmap(16, 16));
    iconLabel->setObjectName(QStringLiteral("DockTitleIconLabel"));
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(iconLabel);
    dock->setProperty(kDockIconLabelProperty, QVariant::fromValue<QObject *>(iconLabel));

    auto *titleLabel = new QLabel(dock->windowTitle(), titleBar);
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(titleLabel, 1);

    auto *floatButton = dockTitleButton(titleBar, QStyle::SP_TitleBarNormalButton, QStringLiteral("Float"));
    QObject::connect(floatButton, &QToolButton::clicked, dock, [dock]() {
        dock->setFloating(!dock->isFloating());
    });
    layout->addWidget(floatButton);

    auto *closeButton = dockTitleButton(titleBar, QStyle::SP_TitleBarCloseButton, QStringLiteral("Close"));
    QObject::connect(closeButton, &QToolButton::clicked, dock, &QDockWidget::hide);
    layout->addWidget(closeButton);

    dock->setTitleBarWidget(titleBar);
}

QToolButton *addDockAreaCollapseButton(QDockWidget *dock) {
    if (dock == nullptr || dock->titleBarWidget() == nullptr) {
        return nullptr;
    }
    auto *titleBar = dock->titleBarWidget();
    auto *layout = qobject_cast<QHBoxLayout *>(titleBar->property(kDockTitleLayoutProperty).value<QObject *>());
    if (layout == nullptr) {
        layout = qobject_cast<QHBoxLayout *>(titleBar->layout());
    }
    if (layout == nullptr) {
        return nullptr;
    }

    auto *button = new QToolButton(titleBar);
    button->setAutoRaise(false);
    button->setFixedSize(22, 18);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(QStringLiteral("Collapse dock area"));
    button->setStyleSheet(QStringLiteral(
        "QToolButton {"
        " border: 1px solid palette(mid);"
        " border-radius: 3px;"
        " padding: 0px;"
        " font-weight: 700;"
        "}"
        "QToolButton:hover {"
        " background: palette(button);"
        "}"));
    layout->insertWidget(0, button);
    return button;
}

QString dockAreaCollapseText(Qt::DockWidgetArea area, bool collapsed) {
    switch (area) {
    case Qt::LeftDockWidgetArea:
        return collapsed ? QStringLiteral(">") : QStringLiteral("<");
    case Qt::RightDockWidgetArea:
        return collapsed ? QStringLiteral("<") : QStringLiteral(">");
    case Qt::TopDockWidgetArea:
        return collapsed ? QStringLiteral("v") : QStringLiteral("^");
    case Qt::BottomDockWidgetArea:
        return collapsed ? QStringLiteral("^") : QStringLiteral("v");
    default:
        return collapsed ? QStringLiteral("+") : QStringLiteral("-");
    }
}

void configureDockAreaCollapseButton(QToolButton *button, Qt::DockWidgetArea area, bool collapsed) {
    if (button == nullptr) {
        return;
    }
    button->setIcon(QIcon());
    button->setText(dockAreaCollapseText(area, collapsed));
    button->setToolTip(collapsed ? QStringLiteral("Restore dock area") : QStringLiteral("Collapse dock area"));
}

void refreshDockTitleIcon(QDockWidget *dock) {
    if (dock == nullptr) {
        return;
    }
    const QString iconName = dock->property(kDockIconNameProperty).toString();
    if (iconName.isEmpty()) {
        return;
    }
    const QIcon icon = assetIcon(iconName);
    dock->setWindowIcon(icon);
    QObject *stored = dock->property(kDockIconLabelProperty).value<QObject *>();
    auto *label = qobject_cast<QLabel *>(stored);
    if (label == nullptr && dock->titleBarWidget() != nullptr) {
        label = dock->titleBarWidget()->findChild<QLabel *>(QStringLiteral("DockTitleIconLabel"));
    }
    if (label != nullptr) {
        label->setPixmap(icon.pixmap(16, 16));
    }
}

namespace {

class SplitterResizeCursorFilter final : public QObject {
public:
    explicit SplitterResizeCursorFilter(Qt::Orientation orientation, QObject *parent = nullptr)
        : QObject(parent)
        , cursorShape_(orientation == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor) {
    }

    ~SplitterResizeCursorFilter() override {
        clearOverrideCursor();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
        case QEvent::MouseMove:
            setOverrideCursor();
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
        case QEvent::MouseButtonRelease:
            clearOverrideCursor();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void setOverrideCursor() {
        const QCursor cursor(cursorShape_);
        if (active_) {
            QApplication::changeOverrideCursor(cursor);
        } else {
            QApplication::setOverrideCursor(cursor);
            active_ = true;
        }
    }

    void clearOverrideCursor() {
        if (!active_) {
            return;
        }
        QApplication::restoreOverrideCursor();
        active_ = false;
    }

    Qt::CursorShape cursorShape_;
    bool active_ = false;
};

} // namespace

void installSplitterResizeCursor(QSplitter *splitter) {
    if (splitter == nullptr || splitter->count() < 2) {
        return;
    }
    QWidget *handle = splitter->handle(1);
    if (handle == nullptr) {
        return;
    }
    handle->setAttribute(Qt::WA_Hover, true);
    handle->setMouseTracking(true);
    handle->setCursor(splitter->orientation() == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
    handle->installEventFilter(new SplitterResizeCursorFilter(splitter->orientation(), handle));
}

} // namespace gui
