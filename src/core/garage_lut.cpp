#include "garage_lut.h"

#include "game_paths.h"
#include "zip_extract.h"

#include <QDir>

#include <cmath>
#include <cstring>
#include <limits>

namespace fh6 {
namespace {

constexpr int kHeaderSize = 12;
constexpr int kChannelsPerTexel = 4;
constexpr int kBytesPerChannel = 2;
constexpr quint32 kReservedHeaderValue = 0;
constexpr quint32 kMinimumDimension = 2;
constexpr quint32 kMaximumDimension = 256;
constexpr char kPaintCarColorLutEntry[] = "Forte_BaseFilmStock_Corrected.lut";

void setError(QString *error, const QString &message) {
    if (error != nullptr) {
        *error = message;
    }
}

quint16 readLeU16(const char *bytes) {
    const auto *data = reinterpret_cast<const unsigned char *>(bytes);

    return static_cast<quint16>(data[0] | (static_cast<quint16>(data[1]) << 8));
}

quint32 readLeU32(const char *bytes) {
    const auto *data = reinterpret_cast<const unsigned char *>(bytes);

    return static_cast<quint32>(data[0])
        | (static_cast<quint32>(data[1]) << 8)
        | (static_cast<quint32>(data[2]) << 16)
        | (static_cast<quint32>(data[3]) << 24);
}

float readLeFloat(const char *bytes) {
    const quint32 bits = readLeU32(bytes);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));

    return value;
}

float halfToFloat(quint16 half) {
    const quint32 sign = static_cast<quint32>(half & 0x8000u) << 16;
    quint32 exponent = (half >> 10) & 0x1Fu;
    quint32 mantissa = half & 0x03FFu;
    quint32 bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));

    return result;
}

} // namespace

bool GarageColorLut::valid() const {
    if (dimension < static_cast<int>(kMinimumDimension)
        || dimension > static_cast<int>(kMaximumDimension)) {
        return false;
    }
    const size_t edge = static_cast<size_t>(dimension);

    return std::isfinite(scale) && scale > 0.0f
        && rgba.size() == edge * edge * edge * kChannelsPerTexel;
}

std::array<float, 4> GarageColorLut::texel(int red, int green, int blue) const {
    if (!valid() || red < 0 || red >= dimension || green < 0 || green >= dimension
        || blue < 0 || blue >= dimension) {
        return {};
    }
    const size_t edge = static_cast<size_t>(dimension);
    const size_t offset = ((static_cast<size_t>(blue) * edge + green) * edge + red)
        * kChannelsPerTexel;

    return {rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3]};
}

std::optional<GarageColorLut> parseGarageColorLut(const QByteArray &bytes, QString *error) {
    if (bytes.size() < kHeaderSize) {
        setError(error, QStringLiteral("colour LUT header is truncated"));
        return std::nullopt;
    }
    const quint32 reserved = readLeU32(bytes.constData());
    const quint32 dimension = readLeU32(bytes.constData() + 4);
    const float scale = readLeFloat(bytes.constData() + 8);
    if (reserved != kReservedHeaderValue) {
        setError(error, QStringLiteral("colour LUT reserved header field is unsupported"));
        return std::nullopt;
    }
    if (dimension < kMinimumDimension || dimension > kMaximumDimension) {
        setError(error, QStringLiteral("colour LUT dimension is outside the supported range"));
        return std::nullopt;
    }
    if (!std::isfinite(scale) || scale <= 0.0f) {
        setError(error, QStringLiteral("colour LUT scale is invalid"));
        return std::nullopt;
    }
    const quint64 edge = dimension;
    const quint64 channelCount = edge * edge * edge * kChannelsPerTexel;
    const quint64 payloadLength = channelCount * kBytesPerChannel;
    const quint64 expectedLength = kHeaderSize + payloadLength;
    if (expectedLength != static_cast<quint64>(bytes.size())) {
        setError(error, QStringLiteral("colour LUT payload length does not match its dimension"));
        return std::nullopt;
    }

    GarageColorLut lut;
    lut.dimension = static_cast<int>(dimension);
    lut.scale = scale;
    lut.rgba.resize(static_cast<size_t>(channelCount));
    const char *payload = bytes.constData() + kHeaderSize;
    for (size_t channel = 0; channel < lut.rgba.size(); ++channel) {
        lut.rgba[channel] = halfToFloat(readLeU16(payload + channel * kBytesPerChannel));
        if (!std::isfinite(lut.rgba[channel])) {
            setError(error, QStringLiteral("colour LUT payload contains a non-finite channel"));
            return std::nullopt;
        }
    }
    if (error != nullptr) {
        error->clear();
    }

    return lut;
}

std::optional<GarageColorLut> loadGarageColorLut(const QString &gameFolder, QString *error) {
    const QString mediaDir = gameMediaDir(gameFolder);
    if (mediaDir.isEmpty()) {
        setError(error, QStringLiteral("game folder is not configured"));
        return std::nullopt;
    }
    const QString archive = QDir(mediaDir).filePath(QStringLiteral("colourgrades.zip"));
    QString zipError;
    const QByteArray bytes = readZipEntry(
        archive, QString::fromLatin1(kPaintCarColorLutEntry), &zipError);
    if (bytes.isEmpty()) {
        setError(error, QStringLiteral("Paint Car colour LUT: %1").arg(zipError));
        return std::nullopt;
    }

    return parseGarageColorLut(bytes, error);
}

} // namespace fh6
