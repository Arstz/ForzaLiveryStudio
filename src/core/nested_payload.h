#pragma once

#include "core_types.h"

#include <QByteArray>
#include <QSizeF>

#include <functional>

namespace fh6 {

// Provides a shape's raster (sprite) size in pixels, used to compute each
// group's bbox-center origin. If empty, a 128x128 default is assumed (origins
// are pure translations, so geometry is unaffected either way).
using SpriteSizeFn = std::function<QSizeF(quint16)>;

// Builds a nested (grouped) C_group payload the in-game editor accepts. Each
// group carries a translation-only transform to its bbox-center origin and packs
// its shapes relative to that origin (keeping child scale/rotation/skew clean),
// with position-dependent transform markers. Ported from the py-prototype
// build_grouped_payload (commit 0ab74ac).
QByteArray buildNestedPayload(const Project &project, const SpriteSizeFn &spriteSize = {});

} // namespace fh6
