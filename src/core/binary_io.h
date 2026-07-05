#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <initializer_list>

namespace fh6::detail {

quint32 readLeU32(const QByteArray &bytes, int offset);
quint16 readLeU16(const QByteArray &bytes, int offset);
float readLeFloat(const QByteArray &bytes, int offset);
void appendLeU16(QByteArray &out, quint16 value);
void appendLeU32(QByteArray &out, quint32 value);
void appendLeFloat(QByteArray &out, float value);
bool bytesAt(const QByteArray &data, int pos, std::initializer_list<quint8> bytes);

} // namespace fh6::detail
