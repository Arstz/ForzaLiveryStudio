#pragma once

#include "core_types.h"

#include <QByteArray>

namespace fh6 {

QByteArray packShape(const ShapeLayer &layer, bool maskRecord);
QByteArray buildFlatPayload(const Project &project);

} // namespace fh6
