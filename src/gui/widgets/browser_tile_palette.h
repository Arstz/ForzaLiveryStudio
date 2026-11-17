#pragma once

#include "theme_manager.h"

namespace gui {

struct BrowserTilePalette {
    QColor base;
    QColor hover;
    QColor stroke;
    QColor previewBase;
    QColor label;
    QColor hoverLabel;
};

inline BrowserTilePalette browserTilePalette(UiTheme theme) {
    const UiColors colors = uiColors(theme);

    return {
        colors.browserBackground,
        colors.hover,
        colors.border,
        colors.browserPreview,
        colors.text,
        colors.selectedText,
    };
}

} // namespace gui
