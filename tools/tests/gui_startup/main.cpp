#include "main_window.h"
#include "theme_manager.h"

#include <QApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        QTextStream(stderr) << message << Qt::endl;
        throw std::runtime_error(message);
    }
}

void click(QWidget *widget, QPoint point, Qt::MouseButton button = Qt::LeftButton) {
    QMouseEvent press(QEvent::MouseButtonPress, point, widget->mapToGlobal(point), button, button, {});
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, widget->mapToGlobal(point), button, Qt::NoButton, {});
    QApplication::sendEvent(widget, &release);
    QApplication::processEvents();
}

void enter(QWidget *widget) {
    require(widget != nullptr, "No widget has keyboard focus");
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, {});
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, {});
    QApplication::sendEvent(widget, &release);
    QApplication::processEvents();
}

void verifyToolSettings(gui::MainWindow &window) {
    auto *canvas = dynamic_cast<gui::ProjectCanvas *>(window.centralWidget());
    auto *toolbar = window.findChild<QToolBar *>("ToolSettingsBar");
    require(canvas && toolbar, "Canvas or Tool Settings is missing");
    const QSize rowSize = toolbar->size();
    for (const QString &tool : {"pen", "lining", "bucket", "move", "select", "transform", "pipette", "rotate"}) {
        window.findChild<QAction *>("tool_" + tool)->trigger();
        QApplication::processEvents();
        require(toolbar->size() == rowSize, "Tool Settings changes dimensions between tools");
        require(toolbar->width() == window.contentsRect().width(), "Tool Settings does not span the window");
    }
    QApplication::setActiveWindow(&window);
    window.findChild<QAction *>("tool_pen")->trigger();
    canvas->setFocus();
    bool penFill = false;
    canvas->setPenFillRequestedCallback([&](const auto &, const auto &, bool) { penFill = true; });
    const QPoint start(canvas->width() / 4, canvas->height() / 3);
    const QPoint second(canvas->width() * 3 / 4, start.y());
    const QPoint third(second.x(), canvas->height() * 2 / 3);
    click(canvas, start);
    click(canvas, second);
    click(canvas, third);
    click(canvas, start);
    auto *mode = window.findChild<QComboBox *>("toolSettings.contourFillMode");
    mode->setFocus();
    mode->setCurrentIndex(1);
    QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 1));
    require(QApplication::focusWidget() == canvas, "Pen mode did not return focus to the canvas");
    enter(QApplication::focusWidget());
    require(penFill, "Enter did not request Pen fill after changing its mode");
    canvas->setPenFillRunning(false);
    window.findChild<QAction *>("tool_lining")->trigger();
    bool liningFill = false;
    canvas->setLiningFillRequestedCallback([&](const auto &, double, const auto &) { liningFill = true; });
    click(canvas, start);
    click(canvas, second);
    click(canvas, second, Qt::RightButton);
    auto *width = window.findChild<QDoubleSpinBox *>("toolSettings.liningWidth");
    auto *editor = width->findChild<QLineEdit *>();
    width->setFocus();
    require(width->hasFocus() || editor->hasFocus(), "Numeric field cannot receive focus");
    click(editor, editor->rect().center());
    require(width->hasFocus() || editor->hasFocus(), "Numeric field cannot retain focus for typing");
    editor->setText(width->locale().toString(12.5, 'f', 2));
    enter(QApplication::focusWidget());
    require(QApplication::focusWidget() == canvas && canvas->liningWidth() == 12.5,
            "Committing a typed width did not apply it and return canvas focus");
    require(!liningFill, "Committing a typed width also requested a fill");
    enter(QApplication::focusWidget());
    require(liningFill, "Enter did not request Lining fill after changing width");
    canvas->setLiningFillRunning(false);
    auto *slider = window.findChild<QSlider *>("toolSettings.liningWidth.slider");
    slider->setFocus();
    click(slider, QPoint(slider->width() / 2, slider->height() / 2));
    require(QApplication::focusWidget() == canvas, "Slider edit did not return canvas focus");
    window.findChild<QAction *>("tool_move")->trigger();
    auto *check = window.findChild<QCheckBox *>("toolSettings.moveToolAutoSelect");
    check->setFocus();
    click(check, QPoint(8, check->height() / 2));
    require(QApplication::focusWidget() == canvas, "Checkbox edit did not return canvas focus");
}

void saveLegacyMaximizedLayout() {
    QMainWindow legacy;
    legacy.setAttribute(Qt::WA_DontShowOnScreen);
    legacy.resize(3000, 1800);
    auto *tools = legacy.addToolBar("Tools");
    tools->setObjectName("MainToolBar");
    auto *settings = legacy.addToolBar("Tool Settings");
    settings->setObjectName("ToolSettingsBar");
    settings->setMinimumHeight(4096);
    legacy.layout()->setGeometry(legacy.rect());
    legacy.setWindowState(Qt::WindowMaximized);
    QSettings().setValue("layout/state", legacy.saveState());
    QSettings().setValue("layout/geometry", legacy.saveGeometry());
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("ForzaLiveryStudioTests");
    QCoreApplication::setApplicationName("GuiStartup-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    const bool useSavedSettings = application.arguments().contains("--saved-settings");
    if (useSavedSettings) {
        QSettings saved(QSettings::NativeFormat, QSettings::UserScope, "ForzaTools", "ForzaLiveryStudio");
        for (const auto &key : saved.allKeys()) {
            if (key.startsWith("layout/") || key.startsWith("ui/")) QSettings().setValue(key, saved.value(key));
        }
    }
    QSettings().setValue("ui/behavior/autosaveIntervalMinutes", 0);
    QSettings().setValue("ui/behavior/autoSavePenPaths", false);
    QSettings().setValue("ui/behavior/autoSaveLiningPaths", false);
    try {
    for (auto theme : {gui::UiTheme::Dark, gui::UiTheme::Light}) {
        gui::applyUiTheme(application, theme);
        gui::saveUiTheme(theme);
        for (int iteration = 0; iteration < 3; ++iteration) {
            gui::MainWindow window;
            const auto hideBeforeDestruction = qScopeGuard([&]() { window.hide(); });
            window.setAttribute(Qt::WA_DontShowOnScreen);
            QEventLoop loop;
            QTimer::singleShot(0, &window, [&]() {
                window.newProject();
                window.findChild<QAction *>("tool_lining")->trigger();
                window.resize(1000, 700);
                window.findChild<QAction *>("tool_bucket")->trigger();
                window.findChild<QAction *>("tool_select")->trigger();
                QTimer::singleShot(200, &loop, &QEventLoop::quit);
            });
            window.show();
            loop.exec();
            auto *toolbar = window.findChild<QToolBar *>("ToolSettingsBar");
            auto *panel = window.findChild<QScrollArea *>("ToolSettingsPanel");
            if (!toolbar || !panel || toolbar->isHidden() || panel->height() < panel->widget()->sizeHint().height()) {
                QTextStream(stderr) << "Tool Settings did not survive startup\n";
                return 1;
            }
            if (iteration == 0) verifyToolSettings(window);
            window.close();
            if (!useSavedSettings) saveLegacyMaximizedLayout();
        }
    }
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "GUI startup passed\n";
    return 0;
}
