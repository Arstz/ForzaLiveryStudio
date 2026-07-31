#pragma once

#include "core_types.h"

#include <QByteArray>

namespace fls {

QByteArray buildFlatPayload(const Project &project);

} // namespace fls
