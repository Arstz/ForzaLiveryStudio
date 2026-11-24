#pragma once

#include "theme_manager.h"

#include <QPainter>

namespace gui {

struct BrowserTilePalette {
    QColor base;
    QColor hover;
    QColor selection;
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
        colors.selection,
        colors.border,
        colors.browserPreview,
        colors.text,
        colors.selectedText,
    };
}

inline void paintBrowserTileBackground(QPainter &painter, const QRect &rect,
                                       const BrowserTilePalette &colors, bool hovered, bool selected) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(hovered || selected ? Qt::NoPen : QPen(colors.stroke, kUiControlBorderWidth));
    painter.setBrush(selected ? colors.selection : hovered ? colors.hover : colors.base);
    painter.drawRoundedRect(rect.adjusted(0, 0, -1, -1), kUiCornerRadius, kUiCornerRadius);
}

} // namespace gui
