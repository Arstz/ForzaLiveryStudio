#pragma once

// Parser for a car's `LiveryMasks/` folder: the per-side planar-projection
// parameters (`Masks.xml`) plus the per-side BC4 coverage masks (`*.swatchbin`).
// Together these define how a C_livery's 11 sections wrap onto the car body:
//   - Each side projects a world position onto a 2D "paint canvas" via a pair of
//     signed world axes (xAxis/yAxis) and a rectangular region on that canvas.
//   - A swatchbin is a 2048x1024 coverage mask in that SAME paint-canvas space
//     (not in any mesh UV): its covered bounding box equals the side's Masks.xml
//     region rect, and its irregular interior is the authored clip (wheel-arch
//     cut-outs, trunk/hood seam boundaries). A fragment picks its side by planar-
//     projecting its world position onto the canvas and sampling the mask there.
//
// The swatch is NOT indexed by a baked mesh UV: Forza bakes no livery UV (channel
// 4 is empty) and none of channels 0-3 correlate with the coverage (verified with
// `fh6_model_dump --mask-hits`), so the projection must be computed live.
//
// Canvas <-> swatch texel: texel_x = canvasX + 1024, texel_y = 512 - canvasY (Y
// flipped); u = texel_x/2048, v = texel_y/1024.
//
// Side order follows the C_livery storage slots (see docs/CLIVERY.md /
// LiveryResearch/SECTIONS.md), which differ in name from the Masks.xml tags:
//   slot 5 Spoiler = mask "Wing"; slots 6-10 (window sections) = the "Glass_*"
//   masks. This lets a section slot index directly select its projection.

#include "swatchbin.h"

#include <QString>

#include <array>

namespace fh6 {

constexpr int kLiverySideCount = 11;

// The canvas coordinate extents (used to convert canvas coords to channel-0 UV).
constexpr float kLiveryCanvasHalfWidth = 1024.0f;  // canvasX in [-1024, 1024]
constexpr float kLiveryCanvasHalfHeight = 512.0f;  // canvasY in [-512, 512]

struct LiverySide {
    int slot = -1;         // C_livery storage slot 0..10
    QString maskName;      // Masks.xml tag (e.g. "Front", "Wing", "Glass_Front")
    bool valid = false;    // Masks.xml valid="true"

    // Region on the paint canvas (canvas units, matching shape px/py space).
    float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
    float xOrigin = 0.0f, yOrigin = 0.0f;

    // Planar projection: canvas x/y come from these signed world axes.
    int xAxis = 0;         // 0=X, 1=Y, 2=Z
    float xSign = 1.0f;
    int yAxis = 1;
    float ySign = 1.0f;
    float xScale = 1.0f, yScale = 1.0f;
    float rotationDeg = 0.0f;

    SwatchMask mask;       // per-side coverage in channel-0 UV space (may be empty)
};

struct LiveryMaskSet {
    std::array<LiverySide, kLiverySideCount> sides{};
    bool loaded = false;   // Masks.xml parsed
    int loadedMasks = 0;   // how many swatchbins decoded successfully

    bool valid() const { return loaded; }
};

// Loads `<dir>/Masks.xml` and the sibling `*.swatchbin` files. `dir` is a car's
// `LiveryMasks` folder. Returns loaded=false (and sets *error) if Masks.xml is
// missing/malformed; missing individual swatchbins are tolerated (that side just
// has an empty mask).
LiveryMaskSet loadLiveryMasks(const QString &dir, QString *error = nullptr);

} // namespace fh6
