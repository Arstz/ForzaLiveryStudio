#include "binary_io.h"

#include <QtEndian>

#include <cstring>
#include <stdexcept>

namespace fh6::detail {

quint32 readLeU32(const QByteArray &bytes, int offset) {
    if (offset < 0 || offset + 4 > bytes.size()) {
        throw std::runtime_error("unexpected end of data while reading u32");
    }
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint16 readLeU16(const QByteArray &bytes, int offset) {
    if (offset < 0 || offset + 2 > bytes.size()) {
        throw std::runtime_error("unexpected end of data while reading u16");
    }
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

float readLeFloat(const QByteArray &bytes, int offset) {
    if (offset < 0 || offset + 4 > bytes.size()) {
        throw std::runtime_error("unexpected end of data while reading float");
    }
    quint32 raw = readLeU32(bytes, offset);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(float));
    return value;
}

void appendLeU16(QByteArray &out, quint16 value) {
    char bytes[2];
    qToLittleEndian<quint16>(value, bytes);
    out.append(bytes, 2);
}

void appendLeU32(QByteArray &out, quint32 value) {
    char bytes[4];
    qToLittleEndian<quint32>(value, bytes);
    out.append(bytes, 4);
}

void appendLeFloat(QByteArray &out, float value) {
    static_assert(sizeof(float) == sizeof(quint32));
    quint32 raw = 0;
    std::memcpy(&raw, &value, sizeof(float));
    appendLeU32(out, raw);
}

bool bytesAt(const QByteArray &data, int pos, std::initializer_list<quint8> bytes) {
    if (pos < 0 || pos + static_cast<int>(bytes.size()) > data.size()) {
        return false;
    }
    int offset = 0;
    for (quint8 byte : bytes) {
        if (static_cast<quint8>(data[pos + offset]) != byte) {
            return false;
        }
        ++offset;
    }
    return true;
}

} // namespace fh6::detail
