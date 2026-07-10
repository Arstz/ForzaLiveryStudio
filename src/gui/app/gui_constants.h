#pragma once

#include <numbers>

// Cross-cutting UI constants shared by more than one gui component. Values used by
// a single component stay as named constants at the top of that component's file.
namespace gui {

// Pi for angle math; replaces literal 3.14159265358979323846 occurrences.
inline constexpr double kPi = std::numbers::pi;

// fh6::scene::Shape::color stores its bytes in BGRA order (matching the game
// payload). These indices name that layout  Edo not reorder them.
inline constexpr int ColorByteBlue = 0;
inline constexpr int ColorByteGreen = 1;
inline constexpr int ColorByteRed = 2;
inline constexpr int ColorByteAlpha = 3;

} // namespace gui
