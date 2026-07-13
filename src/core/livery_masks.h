#pragma once

#include "swatchbin.h"

#include <QString>

#include <array>

namespace fh6 {

constexpr int kLiverySideCount = 11;

// Canvas-to-mask mapping is texel_x=x+1024 and texel_y=512-y.
constexpr float kLiveryCanvasHalfWidth = 1024.0f;
constexpr float kLiveryCanvasHalfHeight = 512.0f;

struct LiverySide {
    int slot = -1;
    QString maskName;
    bool valid = false;

    float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
    float xOrigin = 0.0f, yOrigin = 0.0f;

    int xAxis = 0;
    float xSign = 1.0f;
    int yAxis = 1;
    float ySign = 1.0f;
    float xScale = 1.0f, yScale = 1.0f;
    float rotationDeg = 0.0f;

    SwatchMask mask;
};

struct LiveryMaskSet {
    std::array<LiverySide, kLiverySideCount> sides{};
    bool loaded = false;
    int loadedMasks = 0;

    bool valid() const { return loaded; }
};

LiveryMaskSet loadLiveryMasks(const QString &dir, QString *error = nullptr);

} // namespace fh6
