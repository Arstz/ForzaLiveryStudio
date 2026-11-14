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

inline BrowserTilePalette browserTilePalette(UiTheme theme) {
    const UiColors colors = uiColors(theme);

    return {
        colors.browserBackground,
        colors.hover,
        colors.border,
        colors.focus,
        colors.browserPreview,
        colors.text,
    };
}

} // namespace gui
