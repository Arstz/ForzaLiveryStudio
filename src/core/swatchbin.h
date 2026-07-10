#pragma once

// Decoder for ForzaTech `.swatchbin` livery-mask textures (a "Grub" bundle
// wrapping a single TXCB texture blob). For the 3D preview these files are the
// per-side coverage masks in `LiveryMasks/` next to a car's `.carbin`: each is a
// single-channel BC4 texture in the base (UV channel 0) atlas space that marks
// which body texels belong to that livery side (front/back/top/...).
//
// Mask loading handles the subset needed by car livery coverage masks: PC-format
// (linear) TXCB blobs, mip 0, single-channel encodings (BC4 / R8). Image loading
// additionally handles the PC BC7 decal/logo swatches used by Forza's built-in
// vinyl logos. Xbox/Durango tiled textures (blob "platform" != 0, needing xg.dll
// detiling) are out of scope and reported as an error. See docs/DEV.md and
// ForzaTechStudio's SwatchbinService for the container/format reference.

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

namespace fh6 {

// A decoded single-channel mask: `coverage` holds width*height bytes, row-major
// from the top-left texel (matching DDS/D3D texture order), 0 = not covered,
// 255 = fully covered.
struct SwatchMask {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> coverage;

    bool valid() const { return width > 0 && height > 0 && !coverage.empty(); }
    uint8_t at(int x, int y) const { return coverage[static_cast<size_t>(y) * width + x]; }
};

// A decoded color texture. `rgba` holds width*height*4 bytes, row-major from the
// top-left texel, in RGBA byte order.
struct SwatchImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const { return width > 0 && height > 0 && rgba.size() == static_cast<size_t>(width) * height * 4; }
};

// Decodes a `.swatchbin` from disk. Returns an invalid mask (and sets *error) on
// a bad container, an unsupported/Xbox format, or I/O failure.
SwatchMask loadSwatchMask(const QString &path, QString *error = nullptr);

// Decodes a swatchbin already loaded into memory. Same contract as above.
SwatchMask decodeSwatchMask(const QByteArray &bytes, QString *error = nullptr);

// Decodes a color `.swatchbin` from disk. Currently intended for PC-format BC7
// decal/logo swatches and plain RGBA swatches.
SwatchImage loadSwatchImage(const QString &path, QString *error = nullptr);

// Decodes a color swatchbin already loaded into memory. Same contract as above.
SwatchImage decodeSwatchImage(const QByteArray &bytes, QString *error = nullptr);

} // namespace fh6
