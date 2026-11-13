#pragma once

#include "theme_manager.h"

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
    const UiColors colors = uiColors(dark ? UiTheme::Dark : UiTheme::Light);

    return {
        colors.secondary,
        colors.hover,
        colors.border,
        colors.focus,
        colors.contrast,
        colors.text,
    };
}

} // namespace gui
