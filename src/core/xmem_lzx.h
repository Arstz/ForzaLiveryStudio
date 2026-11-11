#pragma once

#include <QByteArray>
#include <QString>

namespace fls::detail {

bool decompressXmemLzx(const QByteArray &input, quint32 uncompressedSize,
                       quint32 expectedCrc, QByteArray &output, QString *error);

} // namespace fls::detail
