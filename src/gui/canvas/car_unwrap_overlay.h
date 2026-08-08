#pragma once

#include "livery_masks.h"

#include <QPainterPath>

#include <array>

namespace gui {

struct CarUnwrapSide {
    QPainterPath path;

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

} // namespace gui
