#pragma once

class QApplication;
class QWidget;

namespace gui {

void configureSystemIntegration(QApplication &app);
void applySystemWindowIcon(QWidget &window);

} // namespace gui
