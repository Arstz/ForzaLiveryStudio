#pragma once

#include <QString>
#include <QtGlobal>

namespace fls::detail {

quint16 canonicalShapeId(quint16 encodedShapeId);
bool isKnownShapeId(quint16 shapeId);
QString shapeName(quint16 shapeId);

} // namespace fls::detail
