#pragma once

#include "livery_masks.h"

#include <QHash>
#include <QPainterPath>

#include <array>

namespace gui {

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
