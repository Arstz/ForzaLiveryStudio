#include "title_menu_bar.h"

#include "theme_manager.h"

#include <QEvent>
#include <QAbstractButton>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScopedValueRollback>
#include <QWindow>

#include <algorithm>
#include <array>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace gui {

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
namespace {

constexpr int kDefaultCaptionHeight = 32;
constexpr double kCaptionButtonWidthRatio = 1.5;
constexpr double kRestoreOffset = 3.0;

enum class CaptionAction { Minimize, Maximize, Close };

void enableSystemCommands(QWindow &window) {
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    const LONG_PTR style = GetWindowLongPtrW(handle, GWL_STYLE);
    const LONG_PTR systemStyle = style | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

    if (style != systemStyle) {
        SetWindowLongPtrW(handle, GWL_STYLE, systemStyle);
        SetWindowPos(handle, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    EnableMenuItem(GetSystemMenu(handle, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
}

class CaptionButton final : public QAbstractButton {
public:
    CaptionButton(QMainWindow &window, CaptionAction action, QWidget *parent)
        : QAbstractButton(parent), window_(&window), action_(action) {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        updateWindowState();
        connect(this, &QAbstractButton::clicked, &window, [this]() {
            switch (action_) {
            case CaptionAction::Minimize:
                window_->showMinimized();
                break;
            case CaptionAction::Maximize:
                if (window_->isMaximized()) {
                    window_->showNormal();
                } else {
                    window_->showMaximized();
                }
                break;
            case CaptionAction::Close:
                window_->close();
                break;
            }
        });
    }

    void updateWindowState() {
        const QString label = action_ == CaptionAction::Minimize ? QStringLiteral("Minimize")
            : action_ == CaptionAction::Close ? QStringLiteral("Close")
            : window_->isMaximized() ? QStringLiteral("Restore") : QStringLiteral("Maximize");

        setToolTip(label);
        setAccessibleName(label);
        update();
    }

protected:
    bool event(QEvent *event) override {
        const bool result = QAbstractButton::event(event);

        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
            update();
        }

        return result;
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const UiColors colors = uiColors(currentUiTheme());
        QRectF glyph(QPointF(), QSizeF(kUiActionGlyphExtent, kUiActionGlyphExtent));
        const bool hovered = underMouse();

        glyph.moveCenter(QRectF(rect()).center());
        if (isDown() || hovered) {
            painter.fillRect(rect(), isDown() ? colors.selection : uiSelectionHoverColor(colors.selection));
        }
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(isDown() || hovered ? colors.selectedText : colors.text,
            kUiActionGlyphStrokeWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
        switch (action_) {
        case CaptionAction::Minimize:
            painter.drawLine(glyph.bottomLeft(), glyph.bottomRight());
            break;
        case CaptionAction::Maximize:
            if (window_->isMaximized()) {
                painter.drawRect(glyph.adjusted(kRestoreOffset, 0, 0, -kRestoreOffset));
                painter.fillRect(glyph.adjusted(0, kRestoreOffset, -kRestoreOffset, 0),
                    isDown() ? colors.selection : hovered ? uiSelectionHoverColor(colors.selection) : colors.main);
                painter.drawRect(glyph.adjusted(0, kRestoreOffset, -kRestoreOffset, 0));
            } else {
                painter.drawRect(glyph);
            }
            break;
        case CaptionAction::Close:
            painter.drawLine(glyph.topLeft(), glyph.bottomRight());
            painter.drawLine(glyph.bottomLeft(), glyph.topRight());
            break;
        }
    }

private:
    QPointer<QMainWindow> window_;
    CaptionAction action_;
};

class CaptionControls final : public QWidget {
public:
    explicit CaptionControls(QMainWindow &window, QWidget *parent)
        : QWidget(parent), buttons_{
            new CaptionButton(window, CaptionAction::Minimize, this),
            new CaptionButton(window, CaptionAction::Maximize, this),
            new CaptionButton(window, CaptionAction::Close, this)} {
        auto *layout = new QHBoxLayout(this);

        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        for (auto *button : buttons_) {
            layout->addWidget(button);
        }
    }

    void setCaptionHeight(int height) {
        const QSize buttonSize(qRound(height * kCaptionButtonWidthRatio), height);

        for (auto *button : buttons_) {
            button->setFixedSize(buttonSize);
        }
        setFixedSize(layout()->sizeHint());
        updateGeometry();
    }

    void updateWindowState() {
        for (auto *button : buttons_) {
            button->updateWindowState();
        }
    }

    QSize sizeHint() const override {
        return layout()->sizeHint();
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }

private:
    std::array<CaptionButton *, 3> buttons_;
};

class TitleMenuBar final : public QMenuBar {
public:
    explicit TitleMenuBar(QMainWindow &window)
        : QMenuBar(&window)
        , window_(&window)
        , controls_(new CaptionControls(window, this)) {
        setObjectName(QStringLiteral("WindowTitleMenu"));
        setNativeMenuBar(false);
        setCornerWidget(controls_, Qt::TopRightCorner);
        window.installEventFilter(this);
        updateCaptionMetrics();
    }

protected:
    bool event(QEvent *event) override {
        const bool result = QMenuBar::event(event);

        switch (event->type()) {
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::DevicePixelRatioChange:
            updateCaptionMetrics();
            break;
        default:
            break;
        }

        return result;
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == window_) {
            switch (event->type()) {
            case QEvent::WinIdChange:
            case QEvent::Show:
            case QEvent::WindowStateChange:
                updateCaptionMetrics();
                controls_->updateWindowState();
                break;
            default:
                break;
            }
        }

        return QMenuBar::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (actionAt(event->position().toPoint()) == nullptr) {
            event->ignore();
            return;
        }
        QMenuBar::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (actionAt(event->position().toPoint()) == nullptr) {
            event->ignore();
            return;
        }
        QMenuBar::mouseDoubleClickEvent(event);
    }

private:
    void observeFrameWindow() {
        QWindow *handle = window_->windowHandle();
        if (frameWindow_ != handle) {
            if (frameWindow_ != nullptr) {
                disconnect(frameWindow_, nullptr, this, nullptr);
            }
            frameWindow_ = handle;
            if (handle != nullptr) {
                enableSystemCommands(*handle);
                connect(handle, &QWindow::safeAreaMarginsChanged, this, [this]() { updateCaptionMetrics(); });
                connect(handle, &QWindow::screenChanged, this, [this]() { updateCaptionMetrics(); });
            }
        }
    }

    void updateCaptionMetrics() {
        if (updating_ || window_ == nullptr) {
            return;
        }
        const QScopedValueRollback<bool> guard(updating_, true);

        observeFrameWindow();
        const QFont menuFont = uiTextFont();
        const int safeCaptionHeight = frameWindow_ != nullptr ? frameWindow_->safeAreaMargins().top() : 0;
        const int captionHeight = std::max(kUiControlHeight,
            safeCaptionHeight > 0 ? safeCaptionHeight : kDefaultCaptionHeight);
        const int menuInset = std::max(0, (captionHeight - kUiControlHeight) / 2);
        const QString sheet = QStringLiteral(
            "QMenuBar#WindowTitleMenu { padding: 0; }"
            "QMenuBar#WindowTitleMenu::item { margin-top: %1px; margin-bottom: %2px; }")
            .arg(menuInset).arg(menuInset + kUiControlBorderWidth);

        if (font() != menuFont) {
            setFont(menuFont);
        }
        controls_->setCaptionHeight(captionHeight);
        setFixedHeight(captionHeight + kUiControlBorderWidth);
        if (styleSheet() != sheet) {
            setStyleSheet(sheet);
        }
    }

    QPointer<QMainWindow> window_;
    QPointer<QWindow> frameWindow_;
    CaptionControls *controls_;
    bool updating_ = false;
};

} // namespace
#endif

QMenuBar *createTitleMenuBar(QMainWindow &window) {
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    const Qt::WindowFlags nativeCaption = Qt::WindowTitleHint | Qt::WindowMinMaxButtonsHint
        | Qt::WindowCloseButtonHint | Qt::WindowContextHelpButtonHint;

    window.setWindowFlags((window.windowFlags() & ~nativeCaption) | Qt::CustomizeWindowHint
        | Qt::WindowSystemMenuHint | Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint);
    window.setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);

    return new TitleMenuBar(window);
#else
    return new QMenuBar(&window);
#endif
}

} // namespace gui
