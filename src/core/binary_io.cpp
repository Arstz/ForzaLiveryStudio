#include "binary_io.h"

#include <QtEndian>

#include <cstring>
#include <stdexcept>

namespace fls::detail {

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

BinaryCursor::BinaryCursor(const QByteArray &bytes, const char *truncationMessage)
    : bytes_(bytes), truncationMessage_(truncationMessage) {}

quint8 BinaryCursor::u8() {
    require(1);

    return static_cast<quint8>(bytes_[position_++]);
}

quint16 BinaryCursor::u16() {
    require(2);
    const quint16 value = readLeU16(bytes_, position_);
    position_ += 2;

    return value;
}

quint32 BinaryCursor::u32() {
    require(4);
    const quint32 value = readLeU32(bytes_, position_);
    position_ += 4;

    return value;
}

qint32 BinaryCursor::i32() {
    return static_cast<qint32>(u32());
}

float BinaryCursor::f32() {
    require(4);
    const float value = readLeFloat(bytes_, position_);
    position_ += 4;

    return value;
}

void BinaryCursor::skip(int count) {
    require(count);
    position_ += count;
}

int BinaryCursor::position() const {
    return position_;
}

void BinaryCursor::require(int count) const {
    if (count < 0 || position_ < 0 || position_ + count > bytes_.size()) {
        throw std::runtime_error(truncationMessage_);
    }
}

} // namespace fls::detail
