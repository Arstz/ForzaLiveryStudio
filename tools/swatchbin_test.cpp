#include "swatchbin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

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

QByteArray makeSingleSurfaceFixture() {
    constexpr int dataOffset = 0x8C;
    constexpr int payloadSize = 4 * 4 * 4;
    QByteArray bytes(dataOffset + payloadSize, '\0');
    writeLeU32(bytes, 0x00, 0x47727562u);
    bytes[0x04] = 1;
    bytes[0x05] = 1;
    writeLeU32(bytes, 0x08, dataOffset);
    writeLeU32(bytes, 0x0C, bytes.size());
    writeLeU32(bytes, 0x10, 1);
    writeLeU32(bytes, 0x14, 0x54584342u);
    writeLeU16(bytes, 0x1A, 1);
    writeLeU32(bytes, 0x1C, 0x2C);
    writeLeU32(bytes, 0x20, dataOffset);
    writeLeU32(bytes, 0x24, payloadSize);
    writeLeU32(bytes, 0x28, payloadSize);
    writeLeU32(bytes, 0x2C, 0x54584348u);
    writeLeU16(bytes, 0x32, 8);
    constexpr int txch = 0x34;
    writeLeU32(bytes, txch + 0x18, 4);
    writeLeU32(bytes, txch + 0x1C, 4);
    writeLeU32(bytes, txch + 0x20, 1);
    writeLeU16(bytes, txch + 0x24, 1);
    bytes[txch + 0x26] = 1;
    writeLeU32(bytes, txch + 0x38, 0x40);
    writeLeU32(bytes, txch + 0x40, 13);
    writeLeU32(bytes, txch + 0x44, 0x4C);
    writeLeU32(bytes, txch + 0x48, 0xFFFFFFFFu);
    writeLeU32(bytes, txch + 0x4C, payloadSize);
    writeLeU32(bytes, txch + 0x50, 0);
    writeLeU32(bytes, txch + 0x54, 0xFFFFFFFFu);
    for (int i = 0; i < payloadSize; ++i) {
        bytes[dataOffset + i] = static_cast<char>(i);
    }
    return bytes;
}

QByteArray makeHdrFixture() {
    constexpr int dataOffset = 0x8C;
    constexpr int payloadSize = 4 * 4 * 8;
    QByteArray bytes = makeSingleSurfaceFixture();
    bytes.resize(dataOffset + payloadSize);
    writeLeU32(bytes, 0x0C, bytes.size());
    writeLeU32(bytes, 0x24, payloadSize);
    writeLeU32(bytes, 0x28, payloadSize);
    writeLeU32(bytes, 0x74, 12);
    writeLeU32(bytes, 0x80, payloadSize);
    const quint16 texel[] = {0x3C00u, 0x3800u, 0x0000u, 0x3C00u};
    for (int pixel = 0; pixel < 16; ++pixel) {
        for (int channel = 0; channel < 4; ++channel) {
            writeLeU16(bytes, dataOffset + (pixel * 4 + channel) * 2, texel[channel]);
        }
    }
    return bytes;
}

void testSyntheticContainer() {
    const QByteArray bytes = makeSingleSurfaceFixture();
    QString error;
    const auto texture = fh6::parseSwatchTexture(bytes, &error);
    expect(texture.has_value(), "valid single-surface fixture parses");
    if (!texture) {
        return;
    }
    expect(texture->width == 4 && texture->height == 4, "fixture dimensions are exposed");
    expect(texture->arraySize == 1 && texture->sliceCount == 1 && texture->mipCount == 1,
           "fixture topology is exposed");
    expect(texture->platform == 0 && texture->textureType == 0,
           "fixture platform and texture type are exposed");
    expect(texture->slices[0].encoding == fh6::SwatchEncoding::R8G8B8A8,
           "fixture encoding is exposed");
    expect(texture->mipBytes(0, 0).size() == 64, "fixture mip span is exposed");
    expect(texture->mipBytes(1, 0).isEmpty() && texture->mipBytes(0, 1).isEmpty(),
           "invalid mip span indices are rejected");
    const fh6::SwatchImage decoded = fh6::decodeSwatchImage(*texture, 0, 0, &error);
    expect(decoded.valid() && decoded.width == 4 && decoded.height == 4,
           "parsed swatch mip can be decoded without reparsing the container");
    expect(!fh6::decodeSwatchImage(*texture, 0, 1, &error).valid(),
           "parsed swatch decoder rejects an unavailable mip level");

    QByteArray truncatedSlice = bytes.left(0x7F);
    expect(!fh6::parseSwatchTexture(truncatedSlice, &error), "truncated slice table is rejected");

    QByteArray truncatedMip = bytes;
    writeLeU32(truncatedMip, 0x78, 0x1000);
    expect(!fh6::parseSwatchTexture(truncatedMip, &error), "truncated mip table is rejected");

    QByteArray oversizedPayload = bytes;
    writeLeU32(oversizedPayload, 0x80, 65);
    expect(!fh6::parseSwatchTexture(oversizedPayload, &error),
           "out-of-range mip payload is rejected");

    const fh6::SwatchHdrImage hdr = fh6::decodeSwatchHdrImage(makeHdrFixture(), 0, 0, &error);
    expect(hdr.valid(), "RGBA16F fixture decodes to a separate HDR image");
    if (hdr.valid()) {
        expect(hdr.rgba[0] == 1.0f && hdr.rgba[1] == 0.5f
                   && hdr.rgba[2] == 0.0f && hdr.rgba[3] == 1.0f,
               "RGBA16F half-float values remain linear floating point");
    }
}

std::optional<fh6::SwatchTexture> loadTexture(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "FAIL: cannot open %s\n", qPrintable(path));
        ++failures;
        return std::nullopt;
    }
    QString error;
    auto texture = fh6::parseSwatchTexture(file.readAll(), &error);
    if (!texture) {
        std::fprintf(stderr, "FAIL: cannot parse %s: %s\n", qPrintable(path), qPrintable(error));
        ++failures;
    }
    return texture;
}

void expectPackedPayload(const fh6::SwatchTexture &texture, const char *resource) {
    std::vector<fh6::SwatchMipPayload> spans;
    for (const fh6::SwatchTextureSlice &slice : texture.slices) {
        spans.insert(spans.end(), slice.mipLevels.begin(), slice.mipLevels.end());
    }
    std::sort(spans.begin(), spans.end(), [](const auto &left, const auto &right) {
        return left.byteOffset < right.byteOffset;
    });
    quint64 nextOffset = 0;
    for (const fh6::SwatchMipPayload &span : spans) {
        if (span.byteOffset != nextOffset) {
            std::fprintf(stderr, "FAIL: %s payload spans are not contiguous\n", resource);
            ++failures;
            return;
        }
        nextOffset += span.byteLength;
    }
    if (nextOffset != static_cast<quint64>(texture.payload.size())) {
        std::fprintf(stderr, "FAIL: %s payload spans do not cover the payload\n", resource);
        ++failures;
    }
}

void expectOpenGlCubemapOrientation(const fh6::SwatchTexture &texture) {
    double maximumError = 0.0;
    for (int face = 0; face < 6; ++face) {
        QString error;
        const fh6::SwatchHdrImage image = fh6::decodeSwatchHdrImage(texture, face, 0, &error);
        expect(image.valid(), "ground-lighting face decodes for orientation test");
        if (!image.valid()) {
            continue;
        }
        for (int y = 0; y < image.height; ++y) {
            const double v = 2.0 * (y + 0.5) / image.height - 1.0;
            for (int x = 0; x < image.width; ++x) {
                const double u = 2.0 * (x + 0.5) / image.width - 1.0;
                double directionY = 0.0;
                switch (face) {
                case 0:
                case 1:
                case 4:
                case 5:
                    directionY = -v;
                    break;
                case 2:
                    directionY = 1.0;
                    break;
                case 3:
                    directionY = -1.0;
                    break;
                }
                const double length = std::sqrt(u * u + v * v + 1.0);
                directionY /= length;
                const double expected = (1.0 - directionY) * 0.5;
                const double actual = image.rgba[
                    (static_cast<size_t>(y) * image.width + x) * 4];
                maximumError = (std::max)(maximumError, std::abs(actual - expected));
            }
        }
    }
    expect(maximumError < 0.001,
           "ground-lighting data matches OpenGL +X,-X,+Y,-Y,+Z,-Z face orientation");
}

void testGameEvidence(const QString &directory) {
    const QDir evidence(directory);
    auto panorama = loadTexture(evidence.filePath(QStringLiteral("SphericalFrames_0001.swatchbin")));
    if (panorama) {
        expect(panorama->width == 8192 && panorama->height == 4096,
               "garage panorama dimensions match evidence");
        expect(panorama->arraySize == 1 && panorama->sliceCount == 1 && panorama->mipCount == 1,
               "garage panorama topology matches evidence");
        expect(panorama->platform == 0 && panorama->slices[0].encoding == fh6::SwatchEncoding::UnsignedBc6H,
               "garage panorama platform and encoding match evidence");
        expectPackedPayload(*panorama, "garage panorama");
    }

    for (const QString &name : {QStringLiteral("Lighting_0001.swatchbin"),
                                QStringLiteral("GroundLighting_base.swatchbin")}) {
        auto lighting = loadTexture(evidence.filePath(name));
        if (!lighting) {
            continue;
        }
        expect(lighting->width == 64 && lighting->height == 64,
               "garage lighting dimensions match evidence");
        expect(lighting->arraySize == 1 && lighting->sliceCount == 6 && lighting->mipCount == 1,
               "garage lighting topology matches evidence");
        expect(lighting->platform == 0 && lighting->textureType == 1,
               "garage lighting platform and texture type match evidence");
        expect(lighting->slices[0].encoding == fh6::SwatchEncoding::R16G16B16A16Float,
               "garage lighting encoding matches evidence");
        expectPackedPayload(*lighting, "garage lighting");
        if (name == QStringLiteral("GroundLighting_base.swatchbin")) {
            expectOpenGlCubemapOrientation(*lighting);
        }
    }

    auto diffuse = loadTexture(evidence.filePath(QStringLiteral("CubemapProbeDiffuse_Thumbnail.swatchbin")));
    if (diffuse) {
        expect(diffuse->width == 32 && diffuse->height == 32,
               "static diffuse probe dimensions match evidence");
        expect(diffuse->sliceCount == 6 && diffuse->mipCount == 6 && diffuse->textureType == 5,
               "static diffuse probe topology matches evidence");
        expect(diffuse->slices[0].encoding == fh6::SwatchEncoding::R8G8B8A8,
               "static diffuse probe encoding matches evidence");
        expectPackedPayload(*diffuse, "static diffuse probe");
    }

    auto specular = loadTexture(evidence.filePath(QStringLiteral("CubemapProbeSpecular_Thumbnail.swatchbin")));
    if (specular) {
        expect(specular->width == 512 && specular->height == 512,
               "static specular probe dimensions match evidence");
        expect(specular->sliceCount == 6 && specular->mipCount == 10 && specular->textureType == 5,
               "static specular probe topology matches evidence");
        expect(specular->slices[0].encoding == fh6::SwatchEncoding::UnsignedBc6H,
               "static specular probe encoding matches evidence");
        expectPackedPayload(*specular, "static specular probe");
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    testSyntheticContainer();
    if (app.arguments().size() > 1) {
        testGameEvidence(app.arguments()[1]);
    }
    if (failures == 0) {
        std::puts("swatchbin tests passed");
    }
    return failures == 0 ? 0 : 1;
}
