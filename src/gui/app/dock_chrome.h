#pragma once

#include <QString>

class QDockWidget;
class QSplitter;
class QToolButton;

namespace gui {

// Installs a custom dock title bar with an asset icon plus float/close buttons.
void setDockTitleIcon(QDockWidget *dock, const QString &iconName);
void refreshDockTitleIcon(QDockWidget *dock);
QToolButton *addDockAreaCollapseButton(QDockWidget *dock);

// Arrow glyph for a dock-area collapse button given its area and collapsed state,
// and a helper that applies the glyph + tooltip to the button.
QString dockAreaCollapseText(Qt::DockWidgetArea area, bool collapsed);
void configureDockAreaCollapseButton(QToolButton *button, Qt::DockWidgetArea area, bool collapsed);

// Gives a splitter's first handle a hover/resize cursor that survives drags.
void installSplitterResizeCursor(QSplitter *splitter);

} // namespace gui
