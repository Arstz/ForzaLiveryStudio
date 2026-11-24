#include "browser_scroll.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int kWheelAnimationDuration = 120;
constexpr double kWheelUnitsPerNotch = 120.0;

class BrowserWheelScroll final : public QObject {
public:
    explicit BrowserWheelScroll(QAbstractItemView &view)
        : QObject(&view),
          view_(view),
          horizontal_(view.horizontalScrollBar(), QByteArrayLiteral("value")),
          vertical_(view.verticalScrollBar(), QByteArrayLiteral("value")) {
        configureAnimation(horizontal_, *view_.horizontalScrollBar());
        configureAnimation(vertical_, *view_.verticalScrollBar());
        view_.viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Hide) {
            horizontal_.stop();
            vertical_.stop();
        }
        if (event->type() == QEvent::Wheel && scroll(*static_cast<QWheelEvent *>(event))) {
            event->accept();
            return true;
        }

        return QObject::eventFilter(watched, event);
    }

private:
    static void configureAnimation(QPropertyAnimation &animation, QScrollBar &bar) {
        animation.setDuration(kWheelAnimationDuration);
        animation.setEasingCurve(QEasingCurve::OutCubic);
        connect(&bar, &QScrollBar::sliderPressed, &animation, &QPropertyAnimation::stop);
        connect(&bar, &QScrollBar::actionTriggered, &animation, &QPropertyAnimation::stop);
        connect(&bar, &QScrollBar::rangeChanged, &animation, &QPropertyAnimation::stop);
        connect(&bar, &QScrollBar::valueChanged, &animation, [&animation](int value) {
            if (animation.state() == QAbstractAnimation::Running && value != animation.currentValue().toInt()) {
                animation.stop();
            }
        });
    }

    bool scroll(const QWheelEvent &event) {
        const bool pixelScroll = !event.pixelDelta().isNull();
        const QPoint delta = pixelScroll ? event.pixelDelta() : event.angleDelta();
        if (delta.isNull()) {
            return false;
        }
        const bool horizontal = std::abs(delta.x()) > std::abs(delta.y());
        QScrollBar &bar = horizontal ? *view_.horizontalScrollBar() : *view_.verticalScrollBar();
        QPropertyAnimation &animation = horizontal ? horizontal_ : vertical_;
        const int value = bar.value();
        const int units = horizontal ? delta.x() : delta.y();
        const double scrollUnits = bar.invertedControls() ? -static_cast<double>(units) : units;
        const auto boundedValue = [&bar](double target) {
            return qRound(std::clamp(target, static_cast<double>(bar.minimum()), static_cast<double>(bar.maximum())));
        };
        if (pixelScroll) {
            animation.stop();
            bar.setValue(boundedValue(value + scrollUnits));
            return bar.value() != value;
        }

        const int page = bar.pageStep();
        const double step = (event.modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            ? page : static_cast<double>(QApplication::wheelScrollLines()) * bar.singleStep();
        const double distance = std::clamp(scrollUnits * step / kWheelUnitsPerNotch,
                                           -static_cast<double>(page), static_cast<double>(page));
        const bool running = animation.state() == QAbstractAnimation::Running;
        const int pending = running ? animation.endValue().toInt() : value;
        const int start = (pending - value) * distance > 0 ? pending : value;
        const int target = boundedValue(start + distance);
        if (running && target == pending) {
            return true;
        }
        animation.stop();
        if (target == value) {
            return false;
        }
        animation.setStartValue(value);
        animation.setEndValue(target);
        animation.start();

        return true;
    }

    QAbstractItemView &view_;
    QPropertyAnimation horizontal_;
    QPropertyAnimation vertical_;
};

} // namespace

void enableBrowserScrolling(QAbstractItemView &view) {
    view.setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    new BrowserWheelScroll(view);
}

} // namespace gui
