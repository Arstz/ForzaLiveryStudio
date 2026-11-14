#pragma once

#include <QToolBar>

namespace gui {

class ThemeToolBar final : public QToolBar {
public:
    explicit ThemeToolBar(const QString &title, QWidget *parent = nullptr);

protected:
    bool event(QEvent *event) override;
};

} // namespace gui
