#pragma once

#include <numbers>

namespace gui {

inline constexpr double kPi = std::numbers::pi;

// Shape colors use BGRA byte order.
inline constexpr int ColorByteBlue = 0;
inline constexpr int ColorByteGreen = 1;
inline constexpr int ColorByteRed = 2;
inline constexpr int ColorByteAlpha = 3;

} // namespace gui
