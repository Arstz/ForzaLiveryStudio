#pragma once

#include <QString>
#include <QtGlobal>

namespace fls::detail {

inline constexpr quint16 kFirstXbox360ExclusiveShapeId = 1001;
inline constexpr quint16 kLastXbox360ExclusiveShapeId = 1240;

quint16 canonicalShapeId(quint16 encodedShapeId);
bool isKnownShapeId(quint16 shapeId);
QString xbox360ExclusiveShapeModelName(quint16 shapeId);
bool isXbox360ExclusiveShapeId(quint16 shapeId);
QString shapeName(quint16 shapeId);

} // namespace fls::detail
