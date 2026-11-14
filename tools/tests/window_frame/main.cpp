#include "title_menu_bar.h"
#include "project_canvas.h"
#include "car_preview_widget.h"
#include "main_window.h"

#include <QApplication>
#include <QBackingStore>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QOpenGLContext>
#include <QPaintEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QWindow>
#include <qpa/qplatformbackingstore.h>

#define NOMINMAX
#include <windows.h>

#include <array>
#include <stdexcept>

namespace {

constexpr LONG_PTR kWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

void require(bool condition, const char *message) {
    if (!condition) {
        QTextStream(stderr) << message << Qt::endl;
        throw std::runtime_error(message);
    }
}

RECT clientArea(HWND window, RECT frame, bool validRects) {
    WINDOWPOS position{window, nullptr, frame.left, frame.top,
        frame.right - frame.left, frame.bottom - frame.top, SWP_NOZORDER | SWP_NOACTIVATE};
    NCCALCSIZE_PARAMS parameters{{frame, frame, frame}, &position};

    SendMessageW(window, WM_NCCALCSIZE, validRects,
        reinterpret_cast<LPARAM>(validRects ? &parameters.rgrc[0] : &frame));

    return validRects ? parameters.rgrc[0] : frame;
}

void verifyDesktopBorder(HWND window, const RECT &desktop, bool maximized, bool validRects, bool openGLSurface) {
    const UINT dpi = GetDpiForWindow(window);
    const int border = maximized
        ? GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)
        : 0;
    const RECT frame{desktop.left - border, desktop.top - border,
        desktop.right + border, desktop.bottom + border};
    const LONG_PTR style = maximized ? kWindowStyle | WS_MAXIMIZE : WS_POPUP;

    SetWindowLongPtrW(window, GWL_STYLE, style);
    const RECT client = clientArea(window, frame, validRects);

    if (openGLSurface) {
        require(client.left > desktop.left && client.top > desktop.top
            && client.right < desktop.right && client.bottom < desktop.bottom,
            "The OpenGL client area must leave a visible native border inside the desktop bounds");
    } else {
        require(EqualRect(&client, &desktop), "The raster desktop window must fill the desktop bounds");
    }
    require(client.right > client.left && client.bottom > client.top,
        "Native frame calculation returned an empty client area");
}

void verifyRestoredCaption(HWND window, bool validRects) {
    const RECT frame{100, 100, 1100, 800};
    RECT nativeClient = frame;

    SetWindowLongPtrW(window, GWL_STYLE, kWindowStyle);
    DefWindowProcW(window, WM_NCCALCSIZE, FALSE, reinterpret_cast<LPARAM>(&nativeClient));
    const RECT client = clientArea(window, frame, validRects);

    require(client.top < nativeClient.top, "The integrated menu must occupy the native caption area");
    require(client.left == nativeClient.left && client.right == nativeClient.right
        && client.bottom == nativeClient.bottom && client.top > frame.top,
        "Restoring the integrated caption must retain the native resize border");
}

void verifyPreviewRendering(gui::CarPreviewWidget &preview) {
    QBackingStore *store = preview.backingStore();

    require(store != nullptr, "The preview must have a backing store");
    if (store->handle() == nullptr) {
        store->resize(preview.window()->size());
        store->beginPaint(QRegion(preview.window()->rect()));
        store->endPaint();
    }
    require(store->handle() != nullptr, "The preview backing store must have a platform handle");
    require(store->handle()->rhi(preview.windowHandle()) != nullptr,
        "The backing store must have a rendering device for the preview surface");
    QPaintEvent paint(preview.rect());
    QApplication::sendEvent(&preview, &paint);
    require(preview.isValid() && preview.context()->isValid() && preview.defaultFramebufferObject() != 0,
        "The preview must retain a working OpenGL context and framebuffer");
}

void verifyTabbedPreview() {
    gui::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.setDockOptions(window.dockOptions() & ~QMainWindow::AnimatedDocks);
    window.resize(1200, 800);
    auto *previewDock = window.findChild<QDockWidget *>("CarPreviewDock");
    auto *layersDock = window.findChild<QDockWidget *>("LayersDock");
    require(previewDock && layersDock, "The editor must provide preview and Layers docks");
    gui::CarPreviewWidget *preview = nullptr;
    for (auto *view : previewDock->findChildren<QOpenGLWidget *>()) {
        if (auto *candidate = dynamic_cast<gui::CarPreviewWidget *>(view)) {
            preview = candidate;
        }
    }
    require(preview, "The preview dock must contain the car preview");
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    require(previewDock->winId() != 0 && preview->winId() != 0, "The preview requires native dock and view surfaces");
    require(preview->nativeParentWidget() == previewDock,
        "The native preview must be clipped and stacked by its own dock host");
    require(previewDock->windowHandle()->surfaceType() == QSurface::RasterSurface,
        "The preview dock host must retain raster composition");
    previewDock->setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QApplication::processEvents();
    for (int iteration = 0; iteration < 3; ++iteration) {
        previewDock->raise();
        QApplication::processEvents();
        require(preview->nativeParentWidget() == previewDock,
            "Selecting the preview tab must retain its native clipping host");
        layersDock->raise();
        QApplication::processEvents();
        require(!layersDock->visibleRegion().isEmpty(), "The Layers tab must recover its exposed content region");
        require(previewDock->geometry().right() < 0 || previewDock->geometry().bottom() < 0,
            "The inactive preview dock must leave the shared tab content area");
        require(GetAncestor(reinterpret_cast<HWND>(preview->winId()), GA_PARENT)
                == reinterpret_cast<HWND>(previewDock->winId()),
            "The native preview window must remain inside its inactive dock window");
        require(window.windowHandle()->surfaceType() == QSurface::RasterSurface,
            "Dock tab switching must retain raster desktop composition");
    }
    previewDock->raise();
    QApplication::processEvents();
    verifyPreviewRendering(*preview);
    const QPointer<QOpenGLContext> graphicsContext = preview->context();
    for (int iteration = 0; iteration < 3; ++iteration) {
        previewDock->setFloating(true);
        QApplication::processEvents();
        require(preview->nativeParentWidget() == previewDock, "Floating the preview must retain its native dock host");
        verifyPreviewRendering(*preview);
        require(preview->context() == graphicsContext, "Floating must preserve the preview's OpenGL context");
        previewDock->resize(previewDock->size() + QSize(20, 20));
        QApplication::processEvents();
        verifyPreviewRendering(*preview);
        previewDock->setFloating(false);
        window.tabifyDockWidget(layersDock, previewDock);
        previewDock->raise();
        QApplication::processEvents();
        verifyPreviewRendering(*preview);
        require(preview->context() == graphicsContext, "Redocking must preserve the preview's OpenGL context");
        layersDock->raise();
        QApplication::processEvents();
        require(preview->nativeParentWidget() == previewDock && !layersDock->visibleRegion().isEmpty(),
            "Redocking the preview must preserve native clipping and expose the Layers tab");
    }
    require(!IsWindowVisible(handle), "The tab-switching regression test window must remain hidden");
    window.hide();
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("ForzaLiveryStudioTests");
    QCoreApplication::setApplicationName("WindowFrame");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    try {
        QMainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.setMenuBar(gui::createTitleMenuBar(window));
        window.menuBar()->addMenu(QStringLiteral("File"));
        auto *canvas = new gui::ProjectCanvas(&window);
        window.setCentralWidget(canvas);
        auto *dock = new QDockWidget(&window);
        dock->setAttribute(Qt::WA_DontShowOnScreen);
        auto *preview = new gui::CarPreviewWidget(dock);
        dock->setWidget(preview);
        window.addDockWidget(Qt::RightDockWidgetArea, dock);
        const HWND handle = reinterpret_cast<HWND>(window.winId());
        const LONG_PTR originalStyle = GetWindowLongPtrW(handle, GWL_STYLE);
        const std::array<RECT, 2> desktops{{{0, 0, 1920, 1080}, {-2560, -1440, 0, 0}}};

        require(handle != nullptr && !IsWindowVisible(handle), "Frame tests require a hidden native window");
        require(!window.windowFlags().testAnyFlags(Qt::ExpandedClientAreaHint | Qt::NoTitleBarBackgroundHint),
            "The integrated caption must use standard native window styles");
        require((originalStyle & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW,
            "Native caption, resize, and system commands must remain enabled");
        require(canvas->winId() != 0 && preview->winId() != 0, "Render widgets require native surfaces");
        require(window.windowHandle()->surfaceType() == QSurface::RasterSurface,
            "The main desktop window must remain a raster surface");
        require(canvas->windowHandle()->surfaceType() == QSurface::OpenGLSurface
            && preview->windowHandle()->surfaceType() == QSurface::OpenGLSurface,
            "The canvas and preview must retain GPU rendering on their own surfaces");
        dock->setFloating(true);
        require(dock->winId() != 0 && dock->windowHandle()->surfaceType() == QSurface::RasterSurface,
            "A floating preview dock must keep its own desktop frame on a raster surface");
        dock->setFloating(false);
        require(window.windowHandle()->surfaceType() == QSurface::RasterSurface,
            "Redocking the preview must retain raster composition for the main window");
        for (bool openGLSurface : {false, true}) {
            window.windowHandle()->setSurfaceType(openGLSurface ? QSurface::OpenGLSurface : QSurface::RasterSurface);
            for (bool validRects : {false, true}) {
                verifyRestoredCaption(handle, validRects);
                for (const RECT &desktop : desktops) {
                    verifyDesktopBorder(handle, desktop, true, validRects, openGLSurface);
                    verifyDesktopBorder(handle, desktop, false, validRects, openGLSurface);
                    verifyRestoredCaption(handle, validRects);
                }
            }
        }
        window.windowHandle()->setSurfaceType(QSurface::RasterSurface);
        SetWindowLongPtrW(handle, GWL_STYLE, originalStyle);
        verifyTabbedPreview();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << Qt::endl;

        return 1;
    }
#endif
    QTextStream(stdout) << "Native window frame passed" << Qt::endl;

    return 0;
}
