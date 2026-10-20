#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <initializer_list>
#include <string>

namespace fls::detail {

quint32 readLeU32(const QByteArray &bytes, int offset);
quint16 readLeU16(const QByteArray &bytes, int offset);
float readLeFloat(const QByteArray &bytes, int offset);
void appendLeU16(QByteArray &out, quint16 value);
void appendLeU32(QByteArray &out, quint32 value);
void appendLeFloat(QByteArray &out, float value);
bool bytesAt(const QByteArray &data, int pos, std::initializer_list<quint8> bytes);

class BinaryCursor {
public:
    BinaryCursor(const QByteArray &bytes, const char *truncationMessage);

    quint8 u8();
    quint16 u16();
    quint32 u32();
    qint32 i32();
    float f32();
    void skip(int count);
    int position() const;

protected:
    void require(int count) const;

    const QByteArray &bytes_;
    int position_ = 0;

private:
    std::string truncationMessage_;
};

} // namespace fls::detail
