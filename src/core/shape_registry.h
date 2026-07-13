#pragma once

#include <QString>
#include <QtGlobal>

namespace fh6::detail {

bool isKnownShapeId(quint16 shapeId);
QString shapeName(quint16 shapeId);

} // namespace fh6::detail
