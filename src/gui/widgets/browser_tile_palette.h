#pragma once

#include <QColor>

namespace gui {

struct BrowserTilePalette {
    QColor base;
    QColor hover;
    QColor stroke;
    QColor activeStroke;
    QColor previewBase;
    QColor label;
};

inline BrowserTilePalette browserTilePalette(bool dark) {
    return {
        dark ? QColor(34, 34, 34) : QColor(232, 235, 240),
        dark ? QColor(42, 42, 42) : QColor(218, 223, 231),
        dark ? QColor(68, 68, 68) : QColor(145, 153, 166),
        dark ? QColor(119, 119, 119) : QColor(96, 107, 124),
        dark ? QColor(21, 21, 21) : QColor(248, 249, 251),
        dark ? QColor(238, 238, 238) : QColor(32, 34, 37),
    };
}

} // namespace gui
