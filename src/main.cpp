#include "main_window.h"
#include "theme_manager.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForzaTools"));
    QCoreApplication::setApplicationName(QStringLiteral("ForzaLiveryStudio"));
    gui::applyUiTheme(app, gui::loadUiTheme());

    gui::MainWindow window;
    window.newProject(nullptr);
    window.show();
    return QApplication::exec();
}
