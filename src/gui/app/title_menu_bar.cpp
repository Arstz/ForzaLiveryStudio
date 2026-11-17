#include "title_menu_bar.h"

#include "theme_manager.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QEvent>
#include <QAbstractButton>
#include <QDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLayout>
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
#include <dwmapi.h>
#endif

namespace gui {

#ifdef Q_OS_WIN
namespace {

constexpr int kDefaultCaptionHeight = 32;
constexpr int kCaptionLogoExtent = 16;
constexpr int kCaptionMenuHorizontalPadding = 8;
constexpr LPARAM kSuppressNonClientRepaint = -1;
constexpr double kCaptionButtonWidthRatio = 1.5;
constexpr double kRestoreOffset = 3.0;
constexpr const char *kCaptionManagerName = "WindowCaptionManager";
const auto kNativeStateUpdateEvent = static_cast<QEvent::Type>(QEvent::registerEventType());

enum class CaptionAction { Minimize, Maximize, Close };

bool nativeWindowMaximized(const QWidget &window) {
    const HWND handle = reinterpret_cast<HWND>(window.internalWinId());

    return handle != nullptr && IsWindowVisible(handle) ? IsZoomed(handle) != FALSE : window.isMaximized();
}

bool nativeClientCoversMonitor(HWND window, POINT *origin) {
    RECT client{};
    MONITORINFO monitor{};

    monitor.cbSize = sizeof(monitor);
    if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION
        || !GetClientRect(window, &client) || !ClientToScreen(window, origin)
        || !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        return false;
    }
    OffsetRect(&client, origin->x, origin->y);

    return EqualRect(&client, &monitor.rcMonitor) != FALSE;
}

class CaptionLogo final : public QWidget {
public:
    CaptionLogo(QWidget &window, QWidget *parent) : QWidget(parent), window_(&window) {
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
    QPointer<QWidget> window_;
};

void applyNativeFrameTheme(HWND window) {
    if (window == nullptr) {
        return;
    }
    const UiTheme theme = currentUiTheme();
    const UiColors colors = uiColors(theme);
    const COLORREF surface = RGB(colors.main.red(), colors.main.green(), colors.main.blue());
    const COLORREF text = RGB(colors.text.red(), colors.text.green(), colors.text.blue());
    const BOOL dark = isDarkTheme(theme);

    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &surface, sizeof(surface));
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &surface, sizeof(surface));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

void configureNativeFrame(HWND window, bool mainWindow) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR frameStyle = mainWindow
        ? style | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX
        : style | WS_CAPTION;
    const DWMNCRENDERINGPOLICY renderingPolicy = DWMNCRP_ENABLED;
    const MARGINS margins{};

    if (style != frameStyle) {
        SetWindowLongPtrW(window, GWL_STYLE, frameStyle);
    }
    DwmSetWindowAttribute(window, DWMWA_NCRENDERING_POLICY, &renderingPolicy, sizeof(renderingPolicy));
    DwmExtendFrameIntoClientArea(window, &margins);
    applyNativeFrameTheme(window);
    if (mainWindow) {
        EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
    }
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void calculateNativeClientArea(const MSG &event, qintptr *result) {
    RECT *client = event.wParam != FALSE
        ? &reinterpret_cast<NCCALCSIZE_PARAMS *>(event.lParam)->rgrc[0]
        : reinterpret_cast<RECT *>(event.lParam);
    const RECT frame = *client;
    const LONG_PTR style = GetWindowLongPtrW(event.hwnd, GWL_STYLE);
    const bool caption = (style & WS_CAPTION) == WS_CAPTION;

    DefWindowProcW(event.hwnd, event.message, event.wParam, event.lParam);
    if (caption) {
        client->top = frame.top + (IsZoomed(event.hwnd) ? client->left - frame.left : 0);
    }
    *result = event.wParam != FALSE ? WVR_REDRAW : 0;
}

class CaptionButton final : public QAbstractButton {
public:
    CaptionButton(QWidget &window, CaptionAction action, QWidget *parent)
        : QAbstractButton(parent), window_(&window), action_(action) {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        updateWindowState();
        connect(this, &QAbstractButton::clicked, &window, [this]() {
            const HWND handle = reinterpret_cast<HWND>(window_->internalWinId());

            switch (action_) {
            case CaptionAction::Minimize:
                PostMessageW(handle, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                break;
            case CaptionAction::Maximize:
                PostMessageW(handle, WM_SYSCOMMAND,
                    nativeWindowMaximized(*window_) ? SC_RESTORE : SC_MAXIMIZE, 0);
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
            : nativeWindowMaximized(*window_) ? QStringLiteral("Restore") : QStringLiteral("Maximize");

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
            painter.setBrush(isDown() ? colors.selection : colors.hover);
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
            if (nativeWindowMaximized(*window_)) {
                painter.drawRect(glyph.adjusted(kRestoreOffset, 0, 0, -kRestoreOffset));
                painter.fillRect(glyph.adjusted(0, kRestoreOffset, -kRestoreOffset, 0),
                    isDown() ? colors.selection : hovered ? colors.hover : colors.main);
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
    QPointer<QWidget> window_;
    CaptionAction action_;
};

class CaptionControls final : public QWidget {
public:
    explicit CaptionControls(QWidget &window, QWidget *parent)
        : QWidget(parent), window_(&window), buttons_{
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

    void updateWindowFlags() {
        const Qt::WindowFlags flags = window_->windowFlags();

        buttons_[0]->setVisible(flags.testFlag(Qt::WindowMinimizeButtonHint));
        buttons_[1]->setVisible(flags.testFlag(Qt::WindowMaximizeButtonHint));
        buttons_[2]->setVisible(flags.testFlag(Qt::WindowCloseButtonHint));
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
    QPointer<QWidget> window_;
    std::array<CaptionButton *, 3> buttons_;
};

class TitleMenuBar final : public QMenuBar, private QAbstractNativeEventFilter {
public:
    explicit TitleMenuBar(QWidget &window)
        : QMenuBar(&window)
        , window_(&window)
        , logo_(new CaptionLogo(window, this))
        , controls_(new CaptionControls(window, this)) {
        setObjectName(QStringLiteral("WindowTitleMenu"));
        setCornerWidget(logo_, Qt::TopLeftCorner);
        setCornerWidget(controls_, Qt::TopRightCorner);
        window.installEventFilter(this);
        QCoreApplication::instance()->installNativeEventFilter(this);
        controls_->updateWindowFlags();
        controls_->updateWindowState();
        updateCaptionMetrics();
        initializeNativeFrame();
    }

    ~TitleMenuBar() override {
        if (QCoreApplication::instance() != nullptr) {
            QCoreApplication::instance()->removeNativeEventFilter(this);
        }
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QMenuBar::paintEvent(event);
        if (qobject_cast<QMainWindow *>(window_) != nullptr) {
            return;
        }
        QPainter painter(this);
        const QRect titleBounds = rect().adjusted(logo_->width() + kUiSpacing, 0,
            -controls_->width() - kUiSpacing, -kUiControlBorderWidth);
        const QString title = fontMetrics().elidedText(window_->windowTitle(), Qt::ElideRight,
            std::max(0, titleBounds.width()));

        painter.setPen(uiColors(currentUiTheme()).text);
        painter.drawText(titleBounds, Qt::AlignLeft | Qt::AlignVCenter, title);
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override {
        const auto *event = static_cast<const MSG *>(message);

        if (eventType != QByteArrayLiteral("windows_generic_MSG") || nativeWindow_ == nullptr
            || event->hwnd != nativeWindow_
            || window_ == nullptr || frameWindow_ == nullptr) {
            return false;
        }
        switch (event->message) {
        case WM_ENTERSIZEMOVE:
            nativeMoveActive_ = true;
            return false;
        case WM_EXITSIZEMOVE:
            nativeMoveActive_ = false;
            scheduleNativeStateUpdate();
            return false;
        case WM_SIZE: {
            POINT origin{};

            scheduleNativeStateUpdate();
            if ((event->wParam == SIZE_MAXIMIZED || event->wParam == SIZE_RESTORED)
                && nativeClientCoversMonitor(nativeWindow_, &origin)) {
                PostMessageW(nativeWindow_, WM_MOVE, 0, MAKELPARAM(origin.x, origin.y));
                *result = 0;

                return true;
            }

            return false;
        }
        case WM_WINDOWPOSCHANGED:
        case WM_STYLECHANGED:
            scheduleNativeStateUpdate();
            return false;
        case WM_NCCALCSIZE:
            calculateNativeClientArea(*event, result);
            break;
        case WM_NCACTIVATE:
            *result = DefWindowProcW(event->hwnd, event->message, event->wParam, kSuppressNonClientRepaint);
            break;
        case WM_NCHITTEST:
            *result = hitTest(QPoint(GET_X_LPARAM(event->lParam), GET_Y_LPARAM(event->lParam)));
            break;
        default:
            return false;
        }

        return true;
    }

    bool event(QEvent *event) override {
        if (event->type() == kNativeStateUpdateEvent) {
            nativeStateUpdatePending_ = false;
            synchronizeNativeState();

            return true;
        }
        const bool result = QMenuBar::event(event);

        switch (event->type()) {
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::DevicePixelRatioChange:
            updateCaptionMetrics();
            break;
        case QEvent::PaletteChange:
            applyNativeFrameTheme(nativeWindow_);
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
            case QEvent::WindowTitleChange:
                update();
                break;
            case QEvent::WindowActivate:
            case QEvent::WindowDeactivate:
                applyNativeFrameTheme(nativeWindow_);
                break;
            case QEvent::WinIdChange:
            case QEvent::Show:
                controls_->updateWindowFlags();
                controls_->updateWindowState();
                updateCaptionMetrics();
                initializeNativeFrame();
                scheduleNativeStateUpdate();
                break;
            case QEvent::WindowStateChange:
                controls_->updateWindowState();
                scheduleNativeStateUpdate();
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

    void scheduleNativeStateUpdate() {
        if (nativeStateUpdatePending_ || synchronizingNativeState_ || nativeMoveActive_) {
            return;
        }
        nativeStateUpdatePending_ = true;
        QCoreApplication::postEvent(this, new QEvent(kNativeStateUpdateEvent), Qt::LowEventPriority);
    }

    void synchronizeNativeState() {
        if (window_ == nullptr || frameWindow_ == nullptr || nativeWindow_ == nullptr
            || !IsWindowVisible(nativeWindow_) || nativeMoveActive_ || IsIconic(nativeWindow_)) {
            return;
        }
        const QScopedValueRollback<bool> guard(synchronizingNativeState_, true);
        const LONG_PTR style = GetWindowLongPtrW(nativeWindow_, GWL_STYLE);
        const Qt::WindowStates nativeState = IsZoomed(nativeWindow_)
            ? Qt::WindowMaximized : Qt::WindowNoState;
        const Qt::WindowStates stateMask = Qt::WindowMinimized | Qt::WindowMaximized | Qt::WindowFullScreen;

        if ((style & WS_CAPTION) != WS_CAPTION) {
            return;
        }
        if ((frameWindow_->windowStates() & stateMask) != nativeState) {
            frameWindow_->setWindowStates(nativeState);
        }
        if ((window_->windowState() & stateMask) != nativeState) {
            QWindowStateChangeEvent event(window_->windowState());

            QCoreApplication::sendEvent(frameWindow_, &event);
        }
        controls_->updateWindowState();
    }

    LRESULT hitTest(const QPoint &screenPosition) const {
        POINT clientPosition{screenPosition.x(), screenPosition.y()};
        const POINT position = clientPosition;
        RECT frame{};

        if (GetWindowRect(nativeWindow_, &frame) == FALSE || PtInRect(&frame, position) == FALSE
            || ScreenToClient(nativeWindow_, &clientPosition) == FALSE) {
            return HTNOWHERE;
        }
        const QPoint menuPosition = mapFrom(window_.data(),
            QPointF(clientPosition.x / frameWindow_->devicePixelRatio(),
                clientPosition.y / frameWindow_->devicePixelRatio()).toPoint());
        const UINT dpi = GetDpiForWindow(nativeWindow_);
        const bool resizable = !IsZoomed(nativeWindow_)
            && (GetWindowLongPtrW(nativeWindow_, GWL_STYLE) & WS_THICKFRAME) != 0;
        const int borderWidth = !resizable ? 0
            : GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int borderHeight = !resizable ? 0
            : GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const bool titleBar = isVisible() && rect().contains(menuPosition);
        const bool left = screenPosition.x() < frame.left + borderWidth;
        const bool right = screenPosition.x() >= frame.right - borderWidth;
        const bool top = screenPosition.y() < frame.top + borderHeight;
        const bool bottom = screenPosition.y() >= frame.bottom - borderHeight;

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

        if (QWidget::mouseGrabber() != nullptr || QApplication::activePopupWidget() != nullptr) {
            return HTCLIENT;
        }
        if (titleBar && (actionAt(menuPosition) != nullptr || childAt(menuPosition) != nullptr)) {
            return HTCLIENT;
        }

        return titleBar ? HTCAPTION : HTCLIENT;
    }

    void observeFrameWindow() {
        QWindow *handle = window_->windowHandle();
        const HWND nativeWindow = handle != nullptr && handle->handle() != nullptr
            ? reinterpret_cast<HWND>(window_->internalWinId()) : nullptr;

        if (frameWindow_ != handle) {
            if (frameWindow_ != nullptr) {
                disconnect(frameWindow_, nullptr, this, nullptr);
            }
            frameWindow_ = handle;
            if (handle != nullptr) {
                connect(handle, &QWindow::screenChanged, this, [this]() { updateCaptionMetrics(); });
            }
        }
        if (nativeWindow_ != nativeWindow) {
            nativeWindow_ = nativeWindow;
            configuredWindow_ = nullptr;
        }
    }

    void initializeNativeFrame() {
        if (nativeWindow_ == nullptr || configuredWindow_ == nativeWindow_ || window_ == nullptr
            || window_->isMinimized() || window_->isFullScreen()) {
            return;
        }
        configuredWindow_ = nativeWindow_;
        configureNativeFrame(nativeWindow_, qobject_cast<QMainWindow *>(window_) != nullptr);
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
        const int captionHeight = std::max(kUiControlHeight, kDefaultCaptionHeight);
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
        applyNativeFrameTheme(nativeWindow_);
    }

    QPointer<QWidget> window_;
    QPointer<QWindow> frameWindow_;
    CaptionLogo *logo_;
    CaptionControls *controls_;
    HWND nativeWindow_ = nullptr;
    HWND configuredWindow_ = nullptr;
    bool updating_ = false;
    bool nativeStateUpdatePending_ = false;
    bool synchronizingNativeState_ = false;
    bool nativeMoveActive_ = false;
};

class WindowCaptionManager final : public QObject {
public:
    explicit WindowCaptionManager(QApplication &application) : QObject(&application) {
        setObjectName(QString::fromLatin1(kCaptionManagerName));
        application.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (installing_ || (event->type() != QEvent::Polish && event->type() != QEvent::Show
            && event->type() != QEvent::LayoutRequest
            && event->type() != QEvent::WinIdChange)) {
            return false;
        }
        auto *dialog = qobject_cast<QDialog *>(watched);

        if (dialog == nullptr || !dialog->isWindow() || dialog->layout() == nullptr
            || dialog->windowType() != Qt::Dialog) {
            return false;
        }
        auto *caption = dialog->findChild<QMenuBar *>(QStringLiteral("WindowTitleMenu"), Qt::FindDirectChildrenOnly);

        if (caption != nullptr && dialog->layout()->menuBar() == caption) {
            return false;
        }
        const QScopedValueRollback<bool> guard(installing_, true);

        if (caption == nullptr) {
            caption = new TitleMenuBar(*dialog);
        }
        dialog->layout()->setMenuBar(caption);

        return false;
    }

private:
    bool installing_ = false;
};

} // namespace
#endif

void installWindowCaptionOverrides(QApplication &application) {
#ifdef Q_OS_WIN
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    if (application.findChild<QObject *>(QString::fromLatin1(kCaptionManagerName), Qt::FindDirectChildrenOnly) == nullptr) {
        new WindowCaptionManager(application);
    }
#else
    Q_UNUSED(application);
#endif
}

QMenuBar *createTitleMenuBar(QMainWindow &window) {
#ifdef Q_OS_WIN
    installWindowCaptionOverrides(*qApp);
    return new TitleMenuBar(window);
#else
    return new QMenuBar(&window);
#endif
}

} // namespace gui
