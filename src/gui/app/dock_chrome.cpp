#include "dock_chrome.h"

#include "gui_assets.h"
#include "theme_manager.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

namespace gui {
namespace {

constexpr const char *kDockIconNameProperty = "flsDockIconName";
constexpr const char *kDockIconLabelProperty = "flsDockIconLabel";
constexpr const char *kDockTitleLayoutProperty = "flsDockTitleLayout";
constexpr int kDockIconExtent = 16;

enum class DockGlyph { Close, Left, Right, Up, Down };

class PanelDock final : public QDockWidget {
public:
    using QDockWidget::QDockWidget;

protected:
    void paintEvent(QPaintEvent *event) override {
        {
            QPainter painter(this);

            painter.fillRect(rect(), uiColors(currentUiTheme()).panelBackground);
        }
        QDockWidget::paintEvent(event);
    }
};

class DockTitleButton final : public QToolButton {
public:
    DockTitleButton(QWidget *parent, DockGlyph glyph, const QString &toolTip)
        : QToolButton(parent), glyph_(glyph) {
        setAutoRaise(false);
        setProperty("uiDockButton", true);
        setFixedSize(kUiControlHeight, kUiControlHeight);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(toolTip);
        setAccessibleName(toolTip);
    }

    QSize sizeHint() const override {
        return QSize(kUiControlHeight, kUiControlHeight);
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }

    void setGlyph(DockGlyph glyph) {
        if (glyph_ == glyph) {
            return;
        }
        glyph_ = glyph;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionToolButton option;
        QPainter painter(this);
        const UiColors colors = uiColors(currentUiTheme());
        QRectF glyphArea(QPointF(), QSizeF(kUiActionGlyphExtent, kUiActionGlyphExtent));

        glyphArea.moveCenter(QRectF(rect()).center());
        initStyleOption(&option);
        option.icon = QIcon();
        option.text.clear();
        style()->drawComplexControl(QStyle::CC_ToolButton, &option, &painter, this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(!isEnabled() ? colors.disabledText : underMouse() || isDown() ? colors.selectedText : colors.text,
                            kUiActionGlyphStrokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (glyph_ == DockGlyph::Close) {
            painter.drawLine(glyphArea.topLeft(), glyphArea.bottomRight());
            painter.drawLine(glyphArea.bottomLeft(), glyphArea.topRight());
        } else {
            QTransform transform;
            const QPolygonF chevron{QPointF(-0.25, -0.4), QPointF(0.25, 0.0), QPointF(-0.25, 0.4)};
            const double rotation = glyph_ == DockGlyph::Down ? 90.0
                : glyph_ == DockGlyph::Left ? 180.0 : glyph_ == DockGlyph::Up ? 270.0 : 0.0;
            transform.translate(glyphArea.center().x(), glyphArea.center().y());
            transform.rotate(rotation);
            transform.scale(glyphArea.width(), glyphArea.height());
            painter.drawPolyline(transform.map(chevron));
        }
    }

private:
    DockGlyph glyph_;
};

} // namespace

QDockWidget *createPanelDock(const QString &title, QWidget *parent) {
    return new PanelDock(title, parent);
}

void setDockTitleIcon(QDockWidget *dock, const QString &iconName) {
    if (dock == nullptr) {
        return;
    }
    const QIcon icon = assetIcon(iconName);
    dock->setWindowIcon(icon);
    dock->setProperty(kDockIconNameProperty, iconName);

    auto *titleBar = new QWidget(dock);
    titleBar->setObjectName(QStringLiteral("dockHeader"));
    titleBar->setAttribute(Qt::WA_StyledBackground, true);
    titleBar->setProperty("panelFocused", false);
    titleBar->setMinimumHeight(kUiHeaderHeight);
    titleBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(kUiHighlightMargin, kUiHighlightMargin,
                               kUiHighlightMargin, kUiHighlightMargin + kUiControlBorderWidth);
    layout->setSpacing(kUiSpacing);
    titleBar->setProperty(kDockTitleLayoutProperty, QVariant::fromValue<QObject *>(layout));

    auto *iconLabel = new QLabel(titleBar);
    iconLabel->setFixedSize(kUiCompactControlExtent, kUiCompactControlExtent);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setPixmap(icon.pixmap(kDockIconExtent, kDockIconExtent));
    iconLabel->setObjectName(QStringLiteral("DockTitleIconLabel"));
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(iconLabel);
    dock->setProperty(kDockIconLabelProperty, QVariant::fromValue<QObject *>(iconLabel));

    auto *titleLabel = new QLabel(dock->windowTitle(), titleBar);
    titleLabel->setObjectName(QStringLiteral("dockTitle"));
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    QObject::connect(dock, &QDockWidget::windowTitleChanged, titleLabel, &QLabel::setText);
    layout->addWidget(titleLabel, 1);
    QObject::connect(qApp, &QApplication::focusChanged, titleBar,
                     [dock, titleBar](QWidget *, QWidget *focused) {
        const bool active = focused != nullptr && (focused == dock || dock->isAncestorOf(focused));
        if (titleBar->property("panelFocused").toBool() == active) {
            return;
        }
        titleBar->setProperty("panelFocused", active);
        titleBar->style()->unpolish(titleBar);
        titleBar->style()->polish(titleBar);
        titleBar->update();
    });

    auto *closeButton = new DockTitleButton(titleBar, DockGlyph::Close, QStringLiteral("Close"));
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

    auto *button = new DockTitleButton(titleBar, DockGlyph::Left, QStringLiteral("Collapse dock area"));
    layout->insertWidget(0, button, 0, Qt::AlignLeft);
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
    button->setAccessibleName(button->toolTip());
    if (auto *titleButton = dynamic_cast<DockTitleButton *>(button)) {
        const QString text = dockAreaCollapseText(area, collapsed);
        const DockGlyph glyph = text == QStringLiteral(">") ? DockGlyph::Right
            : text == QStringLiteral("^") ? DockGlyph::Up
            : text == QStringLiteral("v") ? DockGlyph::Down : DockGlyph::Left;
        titleButton->setGlyph(glyph);
    }
}

void setHorizontalDockTitleFolded(QDockWidget &dock, bool folded) {
    QWidget *titleBar = dock.titleBarWidget();
    if (titleBar == nullptr) {
        return;
    }
    titleBar->setProperty("horizontalDockFolded", folded);
    titleBar->style()->unpolish(titleBar);
    titleBar->style()->polish(titleBar);
    titleBar->update();
}

void refreshDockTitleIcon(QDockWidget *dock) {
    if (dock == nullptr) {
        return;
    }
    const QString iconName = dock->property(kDockIconNameProperty).toString();
    if (iconName.isEmpty()) {
        return;
    }
    const QIcon previousIcon = dock->windowIcon();
    const QIcon icon = assetIcon(iconName);
    dock->setWindowIcon(icon);
    if (QWidget *owner = dock->parentWidget()) {
        for (QTabBar *tabs : owner->findChildren<QTabBar *>()) {
            for (int index = 0; index < tabs->count(); ++index) {
                if (tabs->tabIcon(index).cacheKey() == previousIcon.cacheKey()) {
                    tabs->setTabIcon(index, icon);
                }
            }
        }
    }
    QObject *stored = dock->property(kDockIconLabelProperty).value<QObject *>();
    auto *label = qobject_cast<QLabel *>(stored);
    if (label == nullptr && dock->titleBarWidget() != nullptr) {
        label = dock->titleBarWidget()->findChild<QLabel *>(QStringLiteral("DockTitleIconLabel"));
    }
    if (label != nullptr) {
        label->setPixmap(icon.pixmap(kDockIconExtent, kDockIconExtent));
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
