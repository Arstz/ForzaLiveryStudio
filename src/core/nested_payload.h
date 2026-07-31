#pragma once

#include "core_types.h"

#include <QByteArray>
#include <QSizeF>

#include <functional>

namespace fls {

using SpriteSizeFn = std::function<QSizeF(quint16)>;

QByteArray buildNestedPayload(const Project &project, const SpriteSizeFn &spriteSize = {});

} // namespace fls
