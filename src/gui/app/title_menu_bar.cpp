#include "title_menu_bar.h"

#include "theme_manager.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QEvent>
#include <QAbstractButton>
#include <QFontMetrics>
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
#include <windowsx.h>
#endif

namespace gui {

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
namespace {

constexpr int kDefaultCaptionHeight = 32;
constexpr int kCaptionLogoExtent = 16;
constexpr int kCaptionMenuHorizontalPadding = 8;
constexpr double kCaptionButtonWidthRatio = 1.5;
constexpr double kRestoreOffset = 3.0;

enum class CaptionAction { Minimize, Maximize, Close };

class CaptionLogo final : public QWidget {
public:
    CaptionLogo(QMainWindow &window, QWidget *parent) : QWidget(parent), window_(&window) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedSize(kDefaultCaptionHeight, kDefaultCaptionHeight);
    }

    QSize sizeHint() const override {
        return minimumSize();
    }

    QSize minimumSizeHint() const override {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        QRect logo(QPoint(), QSize(kCaptionLogoExtent, kCaptionLogoExtent));

        logo.moveCenter(rect().center());
        window_->windowIcon().paint(&painter, logo);
    }

private:
    QPointer<QMainWindow> window_;
};

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
        const QRectF highlight = QRectF(rect()).adjusted(kUiHighlightMargin, kUiHighlightMargin,
            -kUiHighlightMargin, -kUiHighlightMargin);
        QRectF glyph(QPointF(), QSizeF(kUiActionGlyphExtent, kUiActionGlyphExtent));
        const bool hovered = underMouse();

        glyph.moveCenter(QRectF(rect()).center());
        painter.setRenderHint(QPainter::Antialiasing);
        if (isDown() || hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(isDown() ? colors.selection : uiSelectionHoverColor(colors.selection));
            painter.drawRoundedRect(highlight, kUiCornerRadius, kUiCornerRadius);
        }
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

class TitleMenuBar final : public QMenuBar, private QAbstractNativeEventFilter {
public:
    explicit TitleMenuBar(QMainWindow &window)
        : QMenuBar(&window)
        , window_(&window)
        , logo_(new CaptionLogo(window, this))
        , controls_(new CaptionControls(window, this)) {
        setObjectName(QStringLiteral("WindowTitleMenu"));
        setNativeMenuBar(false);
        setCornerWidget(logo_, Qt::TopLeftCorner);
        setCornerWidget(controls_, Qt::TopRightCorner);
        window.installEventFilter(this);
        QCoreApplication::instance()->installNativeEventFilter(this);
        updateCaptionMetrics();
    }

    ~TitleMenuBar() override {
        if (QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->removeNativeEventFilter(this);
        }
    }

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override {
        const auto *event = static_cast<const MSG *>(message);

        if (eventType != QByteArrayLiteral("windows_generic_MSG") || nativeWindow_ == nullptr
            || event->hwnd != nativeWindow_ || event->message != WM_NCHITTEST
            || window_ == nullptr || frameWindow_ == nullptr) {
            return false;
        }
        *result = hitTest(QPoint(GET_X_LPARAM(event->lParam), GET_Y_LPARAM(event->lParam)));

        return true;
    }

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
            case QEvent::WindowIconChange:
                logo_->update();
                break;
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
    LRESULT hitTest(const QPoint &screenPosition) const {
        POINT clientPosition{screenPosition.x(), screenPosition.y()};
        RECT clientRect{};

        if (ScreenToClient(nativeWindow_, &clientPosition) == FALSE
            || GetClientRect(nativeWindow_, &clientRect) == FALSE
            || PtInRect(&clientRect, clientPosition) == FALSE) {
            return HTNOWHERE;
        }
        const QPoint menuPosition = mapFrom(window_.data(),
            QPointF(clientPosition.x / frameWindow_->devicePixelRatio(),
                clientPosition.y / frameWindow_->devicePixelRatio()).toPoint());
        const UINT dpi = GetDpiForWindow(nativeWindow_);
        const int border = IsZoomed(nativeWindow_) || window_->isFullScreen() ? 0
            : GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const bool titleBar = isVisible() && rect().contains(menuPosition);
        const bool left = clientPosition.x < border;
        const bool right = clientPosition.x >= clientRect.right - border;
        const bool top = clientPosition.y < border;
        const bool bottom = clientPosition.y >= clientRect.bottom - border;

        if (QWidget::mouseGrabber() != nullptr || QApplication::activePopupWidget() != nullptr
            || (titleBar && (actionAt(menuPosition) != nullptr || childAt(menuPosition) != nullptr))) {
            return HTCLIENT;
        }
        if (top && left) {
            return HTTOPLEFT;
        }
        if (top && right) {
            return HTTOPRIGHT;
        }
        if (bottom && left) {
            return HTBOTTOMLEFT;
        }
        if (bottom && right) {
            return HTBOTTOMRIGHT;
        }
        if (left) {
            return HTLEFT;
        }
        if (right) {
            return HTRIGHT;
        }
        if (top) {
            return HTTOP;
        }
        if (bottom) {
            return HTBOTTOM;
        }

        return titleBar ? HTCAPTION : HTCLIENT;
    }

    void observeFrameWindow() {
        QWindow *handle = window_->windowHandle();
        if (frameWindow_ != handle) {
            if (frameWindow_ != nullptr) {
                disconnect(frameWindow_, nullptr, this, nullptr);
            }
            frameWindow_ = handle;
            nativeWindow_ = handle != nullptr ? reinterpret_cast<HWND>(handle->winId()) : nullptr;
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
        const QFontMetrics menuMetrics(menuFont, this);
        const QMargins textMargins = uiTextInsets(menuFont);
        const int safeCaptionHeight = frameWindow_ != nullptr ? frameWindow_->safeAreaMargins().top() : 0;
        const int captionHeight = std::max(kUiControlHeight,
            safeCaptionHeight > 0 ? safeCaptionHeight : kDefaultCaptionHeight);
        const int menuPadding = std::max(0, captionHeight - 2 * kUiHighlightMargin - menuMetrics.height());
        const int menuTopPadding = std::clamp(
            (menuPadding + textMargins.top() - textMargins.bottom()) / 2, 0, menuPadding);
        const int menuBottomPadding = menuPadding - menuTopPadding;
        const QString sheet = QStringLiteral(
            "QMenuBar#WindowTitleMenu { padding: 0; }"
            "QMenuBar#WindowTitleMenu::item { padding: %1px %2px %3px %2px;"
            " margin: %4px %4px %5px %4px; }")
            .arg(menuTopPadding).arg(kCaptionMenuHorizontalPadding).arg(menuBottomPadding)
            .arg(kUiHighlightMargin).arg(kUiHighlightMargin + kUiControlBorderWidth);

        if (font() != menuFont) {
            setFont(menuFont);
        }
        controls_->setCaptionHeight(captionHeight);
        logo_->setFixedSize(captionHeight, captionHeight);
        setFixedHeight(captionHeight + kUiControlBorderWidth);
        if (styleSheet() != sheet) {
            setStyleSheet(sheet);
        }
    }

    QPointer<QMainWindow> window_;
    QPointer<QWindow> frameWindow_;
    CaptionLogo *logo_;
    CaptionControls *controls_;
    HWND nativeWindow_ = nullptr;
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
