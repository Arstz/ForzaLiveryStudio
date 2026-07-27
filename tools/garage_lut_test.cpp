#include "garage_lut.h"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

constexpr int kLutHeaderSize = 12;

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void writeLeU16(QByteArray &bytes, int offset, quint16 value) {
    bytes[offset] = static_cast<char>(value & 0xFF);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void writeLeU32(QByteArray &bytes, int offset, quint32 value) {
    for (int byte = 0; byte < 4; ++byte) {
        bytes[offset + byte] = static_cast<char>((value >> (byte * 8)) & 0xFF);
    }
}

void writeLeFloat(QByteArray &bytes, int offset, float value) {
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeLeU32(bytes, offset, bits);
}

QByteArray makeFixture() {
    constexpr int dimension = 2;
    constexpr int channelCount = dimension * dimension * dimension * 4;
    QByteArray bytes(kLutHeaderSize + channelCount * 2, '\0');
    writeLeU32(bytes, 0, 0);
    writeLeU32(bytes, 4, dimension);
    writeLeFloat(bytes, 8, 100.0f);
    for (int blue = 0; blue < dimension; ++blue) {
        for (int green = 0; green < dimension; ++green) {
            for (int red = 0; red < dimension; ++red) {
                const int texel = (blue * dimension + green) * dimension + red;
                const int offset = kLutHeaderSize + texel * 8;
                writeLeU16(bytes, offset, red == 0 ? 0x0000u : 0x3C00u);
                writeLeU16(bytes, offset + 2, green == 0 ? 0x0000u : 0x3800u);
                writeLeU16(bytes, offset + 4, blue == 0 ? 0x0000u : 0x3400u);
                writeLeU16(bytes, offset + 6, 0xBC00u);
            }
        }
    }

    return bytes;
}

void testSyntheticLut() {
    const QByteArray bytes = makeFixture();
    QString error;
    const auto lut = fh6::parseGarageColorLut(bytes, &error);
    expect(lut.has_value(), "valid LUT fixture parses");
    if (!lut) {
        return;
    }
    expect(lut->dimension == 2 && lut->scale == 100.0f,
           "LUT header dimension and scale are retained");
    expect(lut->rgba.size() == 32, "LUT exposes four float channels per texel");
    expect(lut->texel(1, 0, 0) == std::array<float, 4>{1.0f, 0.0f, 0.0f, -1.0f},
           "red is the fastest axis and RGBA channel order is retained");
    expect(lut->texel(0, 1, 0) == std::array<float, 4>{0.0f, 0.5f, 0.0f, -1.0f},
           "green is the second LUT axis");
    expect(lut->texel(0, 0, 1) == std::array<float, 4>{0.0f, 0.0f, 0.25f, -1.0f},
           "blue is the third LUT axis");
    expect(lut->texel(-1, 0, 0) == std::array<float, 4>{},
           "out-of-range LUT coordinates are rejected");

    QByteArray truncated = bytes.left(bytes.size() - 2);
    expect(!fh6::parseGarageColorLut(truncated, &error),
           "truncated LUT payload is rejected");
    QByteArray trailing = bytes + QByteArray(2, '\0');
    expect(!fh6::parseGarageColorLut(trailing, &error),
           "trailing LUT payload is rejected");
    QByteArray reserved = bytes;
    writeLeU32(reserved, 0, 1);
    expect(!fh6::parseGarageColorLut(reserved, &error),
           "unknown LUT header fields are rejected");
    QByteArray dimension = bytes;
    writeLeU32(dimension, 4, 1);
    expect(!fh6::parseGarageColorLut(dimension, &error),
           "invalid LUT dimensions are rejected");
    QByteArray invalidScale = bytes;
    writeLeFloat(invalidScale, 8, std::numeric_limits<float>::infinity());
    expect(!fh6::parseGarageColorLut(invalidScale, &error),
           "non-finite LUT scale is rejected");
    QByteArray invalidChannel = bytes;
    writeLeU16(invalidChannel, kLutHeaderSize, 0x7C00u);
    expect(!fh6::parseGarageColorLut(invalidChannel, &error),
           "non-finite LUT channels are rejected");
}

void testGameEvidence(const QString &path) {
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "game LUT evidence opens");
    if (!file.isOpen()) {
        return;
    }
    QString error;
    const auto lut = fh6::parseGarageColorLut(file.readAll(), &error);
    expect(lut.has_value(), "game LUT evidence parses");
    if (!lut) {
        return;
    }
    expect(lut->dimension == 32, "Paint Car LUT dimension matches evidence");
    expect(lut->scale == 100.0f, "Paint Car LUT scale matches evidence");
    expect(lut->rgba.size() == 32u * 32u * 32u * 4u,
           "Paint Car LUT payload topology matches evidence");
    bool finite = true;
    bool zeroAlpha = true;
    float maximumRgb = 0.0f;
    for (size_t channel = 0; channel < lut->rgba.size(); ++channel) {
        const float value = lut->rgba[channel];
        finite = finite && std::isfinite(value);
        if (channel % 4 == 3) {
            zeroAlpha = zeroAlpha && value == 0.0f;
        } else {
            maximumRgb = std::max(maximumRgb, value);
        }
    }
    expect(finite, "Paint Car LUT half-float payload is finite");
    expect(zeroAlpha, "Paint Car LUT fourth channel is zero throughout");
    expect(maximumRgb > 1.0f, "Paint Car LUT retains HDR-range RGB values");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    testSyntheticLut();
    if (app.arguments().size() > 1) {
        testGameEvidence(app.arguments()[1]);
    }
    if (failures == 0) {
        std::puts("garage LUT tests passed");
    }

    return failures == 0 ? 0 : 1;
}
