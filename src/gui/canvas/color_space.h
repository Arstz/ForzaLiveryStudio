#pragma once

#include <QVector3D>

#include <algorithm>
#include <cmath>

namespace gui {

inline float srgbToLinear(float value) {
    const float srgb = std::clamp(value, 0.0f, 1.0f);

    return srgb <= 0.04045f
        ? srgb / 12.92f
        : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

inline QVector3D srgbToLinear(const QVector3D &color) {
    return QVector3D(
        srgbToLinear(color.x()),
        srgbToLinear(color.y()),
        srgbToLinear(color.z()));
}

} // namespace gui
