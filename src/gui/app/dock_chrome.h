#pragma once

#include <QString>

class QDockWidget;
class QSplitter;
class QToolButton;

namespace gui {

void setDockTitleIcon(QDockWidget *dock, const QString &iconName);
void refreshDockTitleIcon(QDockWidget *dock);
QToolButton *addDockAreaCollapseButton(QDockWidget *dock);

QString dockAreaCollapseText(Qt::DockWidgetArea area, bool collapsed);
void configureDockAreaCollapseButton(QToolButton *button, Qt::DockWidgetArea area, bool collapsed);

void installSplitterResizeCursor(QSplitter *splitter);

} // namespace gui
