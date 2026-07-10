#include "main_window.h"
#include "theme_manager.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    // Share GL resources across contexts so the 3D car preview and the 2D canvas
    // can interoperate; must be set before the QApplication is constructed.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForzaTools"));
    QCoreApplication::setApplicationName(QStringLiteral("ForzaLiveryStudio"));
    gui::applyUiTheme(app, gui::loadUiTheme());

    gui::MainWindow window;
    window.newProject();
    window.show();
    return QApplication::exec();
}
