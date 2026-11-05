#pragma once

#include "core_types.h"

namespace fls {

inline constexpr int kPositionalPaintRecordSize = 40;
inline constexpr int kPositionalPaintBlockOverhead = 3;

QByteArray positionalPaintBlock(const QByteArray &payload);
QString positionalPaintGame(const QByteArray &payload, int recordCount);
LiveryPaintState decodePositionalPaint(const QByteArray &block, const QString &game);
QByteArray encodePositionalPaint(const Project &project, int recordCount, const QString &game);

} // namespace fls
