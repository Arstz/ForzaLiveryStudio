#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace fh6 {

enum class SwatchEncoding : int {
    Bc1 = 0,
    Bc2 = 1,
    Bc3 = 2,
    UnsignedBc4 = 3,
    SignedBc4 = 4,
    UnsignedBc5 = 5,
    SignedBc5 = 6,
    UnsignedBc6H = 7,
    SignedBc6H = 8,
    Bc7 = 9,
    R32G32B32A32Float = 10,
    R16G16B16A16 = 11,
    R16G16B16A16Float = 12,
    R8G8B8A8 = 13,
    B5G6R5 = 14,
    B5G5R5A1 = 15,
    Dct = 16,
    IntegerDct = 17,
    Procedural = 18,
    R8 = 19,
    A8 = 20,
    R8G8 = 21,
    Bc7HighQuality = 22,
};

struct SwatchMipPayload {
    quint32 byteOffset = 0;
    quint32 byteLength = 0;
    quint32 nextDescriptorOffset = 0xFFFFFFFFu;
};

struct SwatchTextureSlice {
    SwatchEncoding encoding = SwatchEncoding::Bc1;
    quint32 encodedFormat = 0;
    quint32 descriptorOffset = 0;
    quint32 nextSliceOffset = 0xFFFFFFFFu;
    std::vector<SwatchMipPayload> mipLevels;
};

struct SwatchTexture {
    int width = 0;
    int height = 0;
    int arraySize = 0;
    int platform = 0;
    int sliceCount = 0;
    int mipCount = 0;
    quint8 textureType = 0;
    qint32 transcoding = 0;
    quint32 sliceTableOffset = 0;
    QByteArray payload;
    std::vector<SwatchTextureSlice> slices;

    bool valid() const {
        return width > 0 && height > 0 && arraySize > 0 && sliceCount > 0
            && mipCount > 0 && slices.size() == static_cast<size_t>(sliceCount);
    }

    QByteArray mipBytes(int sliceIndex, int mipIndex) const;
};

struct SwatchMask {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> coverage;

    bool valid() const { return width > 0 && height > 0 && !coverage.empty(); }
    uint8_t at(int x, int y) const { return coverage[static_cast<size_t>(y) * width + x]; }
};

struct SwatchImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const { return width > 0 && height > 0 && rgba.size() == static_cast<size_t>(width) * height * 4; }
};

struct SwatchHdrImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;

    bool valid() const {
        return width > 0 && height > 0
            && rgba.size() == static_cast<size_t>(width) * height * 4;
    }
};

SwatchMask loadSwatchMask(const QString &path, QString *error = nullptr);

SwatchMask decodeSwatchMask(const QByteArray &bytes, QString *error = nullptr);

SwatchImage loadSwatchImage(const QString &path, QString *error = nullptr);

SwatchImage decodeSwatchImage(const QByteArray &bytes, QString *error = nullptr);

std::optional<SwatchTexture> parseSwatchTexture(
    const QByteArray &bytes, QString *error = nullptr);

SwatchHdrImage decodeSwatchHdrImage(
    const SwatchTexture &texture, int sliceIndex = 0, int mipIndex = 0,
    QString *error = nullptr);

SwatchHdrImage decodeSwatchHdrImage(
    const QByteArray &bytes, int sliceIndex = 0, int mipIndex = 0,
    QString *error = nullptr);

} // namespace fh6
