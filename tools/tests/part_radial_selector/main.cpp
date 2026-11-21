#include "part_radial_selector.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

QVector<gui::PartRadialChoice> choices(int count) {
    QVector<gui::PartRadialChoice> result;
    for (int index = 0; index < count; ++index) {
        result.push_back({QStringLiteral("Part %1").arg(index), {}, 100 + index});
    }
    return result;
}

void mouse(gui::PartRadialSelector &selector, QEvent::Type type, const QPoint &position) {
    QMouseEvent event(type, position, selector.mapToGlobal(position),
        type == QEvent::MouseButtonRelease ? Qt::LeftButton : Qt::NoButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&selector, &event);
}

void key(gui::PartRadialSelector &selector, int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&selector, &event);
}

void scroll(gui::PartRadialSelector &selector, int delta) {
    QWheelEvent event(QPointF(400, 180), selector.mapToGlobal(QPoint(400, 180)),
        {}, QPoint(0, delta), Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&selector, &event);
}

void testGestures() {
    QWidget parent;
    gui::PartRadialSelector selector(&parent);
    QVector<int> applied;
    int rendered = 0;
    const auto preview = [&rendered](int) {
        ++rendered;
        QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        return image;
    };
    const auto begin = [&](int count = 3, int selected = 100, QPoint origin = {400, 300}) {
        selector.beginSelection(origin, QStringLiteral("Body parts"), choices(count), selected, preview);
        require(selector.isSelecting(), "selector did not open on press");
    };
    QObject::connect(&selector, &gui::PartRadialSelector::optionSelected,
        [&applied](int optionId) { applied.push_back(optionId); });
    parent.resize(800, 600);
    selector.setGeometry(parent.rect());
    parent.show();
    QApplication::processEvents();

    begin();
    mouse(selector, QEvent::MouseMove, {400, 180});
    require(applied.isEmpty(), "hover committed before mouse release");
    mouse(selector, QEvent::MouseButtonRelease, {400, 180});
    require(applied == QVector<int>{100}, "release did not apply the hovered 3D choice");
    require(!selector.isSelecting(), "release left the selector open");

    begin();
    mouse(selector, QEvent::MouseMove, {400, 180});
    mouse(selector, QEvent::MouseButtonRelease, {400, 300});
    require(applied.size() == 1, "returning to center did not cancel");
    begin();
    mouse(selector, QEvent::MouseButtonRelease, {900, 300});
    require(applied.size() == 1, "release outside the selector applied a choice");
    begin();
    mouse(selector, QEvent::MouseMove, {400, 180});
    key(selector, Qt::Key_Escape);
    mouse(selector, QEvent::MouseButtonRelease, {400, 180});
    require(applied.size() == 1 && !selector.isSelecting(), "Escape did not cancel the held gesture");

    begin();
    mouse(selector, QEvent::MouseMove, {400, 180});
    QEvent lostGrab(QEvent::UngrabMouse);
    QApplication::sendEvent(&selector, &lostGrab);
    require(applied.size() == 1 && !selector.isSelecting(), "losing mouse capture committed a choice");
    begin();
    selector.hide();
    require(applied.size() == 1 && !selector.isSelecting(), "hiding the selector left a pending selection");

    begin(3, 100, {20, 20});
    mouse(selector, QEvent::MouseButtonRelease, {20, 20});
    require(applied.size() == 1, "a stationary edge click accidentally applied a relocated choice");

    rendered = 0;
    begin(10);
    require(rendered == 8, "opening rendered parts outside the visible page");
    scroll(selector, -120);
    require(rendered == 10 && applied.size() == 1, "scroll did not reveal previews without applying");
    scroll(selector, 120);
    require(rendered == 10, "returning to a page rendered cached previews again");
    scroll(selector, -120);
    mouse(selector, QEvent::MouseButtonRelease, {400, 180});
    require(applied.last() == 108, "release selected a part from the wrong page");

    rendered = 0;
    begin(10);
    selector.setChoicePreview(108, {});
    scroll(selector, -120);
    require(rendered == 9, "a completed preview was requested again when its page opened");
    selector.cancelSelection();

    begin(10, 109);
    key(selector, Qt::Key_Right);
    key(selector, Qt::Key_Return);
    require(applied.last() == 108, "current-part page or keyboard navigation selected the wrong part");
    begin();
    key(selector, Qt::Key_Left);
    key(selector, Qt::Key_Return);
    require(applied.last() == 102, "reverse keyboard selection failed");
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication application(argc, argv);
    try {
        testGestures();
        std::puts("Part radial selector gesture tests passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
