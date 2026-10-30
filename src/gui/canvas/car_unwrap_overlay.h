#pragma once

#include "livery_masks.h"

#include <QHash>
#include <QPainterPath>
#include <QTransform>

#include <array>

namespace gui {

inline QTransform liveryMaskToSectionTransform(const fls::LiverySide &side, int sideIndex) {
    const bool transpose = sideIndex == 5 || sideIndex == 6 || sideIndex == 7;
    const double xSign = sideIndex == 4 || sideIndex == 6 || sideIndex == 7 || sideIndex == 10 ? -1.0 : 1.0;
    const double ySign = sideIndex == 4 || sideIndex == 7 || sideIndex == 10 ? -1.0 : 1.0;

    return transpose
        ? QTransform(0.0, ySign, xSign, 0.0, -xSign * side.yOrigin, -ySign * side.xOrigin)
        : QTransform(xSign, 0.0, 0.0, ySign, -xSign * side.xOrigin, -ySign * side.yOrigin);
}

struct CarUnwrapSide {
    QPainterPath path;
    QPainterPath wireframe;

    bool valid() const {
        return !path.isEmpty();
    }
};

struct CarUnwrapOverlay {
    std::array<CarUnwrapSide, fls::kLiverySideCount> sides;

    bool empty() const {
        for (const CarUnwrapSide &side : sides) {
            if (side.valid()) {
                return false;
            }
        }

        return true;
    }
};

struct CarUnwrapPartOverlays {
    CarUnwrapOverlay stock;
    QHash<int, CarUnwrapOverlay> options;
};

struct CarUnwrapOverlaySet {
    CarUnwrapOverlay base;
    QHash<int, CarUnwrapPartOverlays> parts;

    CarUnwrapOverlay selected(const QHash<int, int> &partSelections) const {
        CarUnwrapOverlay overlay = base;
        const auto append = [](CarUnwrapOverlay &target,
                               const CarUnwrapOverlay &source) {
            for (int sideIndex = 0;
                 sideIndex < fls::kLiverySideCount;
                 ++sideIndex) {
                CarUnwrapSide &targetSide = target.sides[sideIndex];
                const CarUnwrapSide &sourceSide = source.sides[sideIndex];
                targetSide.path.setFillRule(Qt::WindingFill);
                targetSide.path.addPath(sourceSide.path);
                targetSide.wireframe.addPath(sourceSide.wireframe);
            }
        };

        for (auto part = parts.cbegin(); part != parts.cend(); ++part) {
            const auto selection = partSelections.constFind(part.key());
            if (selection == partSelections.cend()) {
                append(overlay, part->stock);
                continue;
            }
            const auto option = part->options.constFind(selection.value());
            if (option != part->options.cend()) {
                append(overlay, option.value());
            }
        }
        return overlay;
    }
};

} // namespace gui
