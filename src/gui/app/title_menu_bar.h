#pragma once

class QMainWindow;
class QMenuBar;
class QApplication;

namespace gui {

void installWindowCaptionOverrides(QApplication &application);
QMenuBar *createTitleMenuBar(QMainWindow &window);

} // namespace gui
