#include "swatchbin.h"

#include <QCoreApplication>
#include <QtEndian>

#include <algorithm>
#include <future>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QByteArray solidBlock(bool white) {
    QByteArray block(16, '\0');
    block[0] = '\x40';
    if (white) {
        for (int bit = 7; bit < 65; ++bit) {
            block[bit / 8] = static_cast<char>(
                static_cast<unsigned char>(block[bit / 8]) | (1u << (bit % 8)));
        }
    }
    return block;
}

QByteArray swatch(int width, int height, const QByteArray &block, int encoding) {
    constexpr int metadata = 44;
    constexpr int header = metadata + 8;
    constexpr int pixels = header + 68;
    const int size = ((width + 3) / 4) * ((height + 3) / 4) * block.size();
    QByteArray bytes(pixels + size, '\0');
    const auto u16 = [&](int offset, quint16 value) {
        qToLittleEndian(value, bytes.data() + offset);
    };
    const auto u32 = [&](int offset, quint32 value) {
        qToLittleEndian(value, bytes.data() + offset);
    };
    u32(0, 0x47727562);
    bytes[4] = 1;
    bytes[5] = 1;
    u32(16, 1);
    u32(20, 0x54584342);
    u16(26, 1);
    u32(28, metadata);
    u32(32, pixels);
    u32(36, size);
    u32(40, size);
    u32(metadata, 0x54584348);
    u16(metadata + 6, 8);
    u32(header + 0x18, width);
    u32(header + 0x1c, height);
    u32(header + 0x38, 64);
    u32(header + 64, encoding);
    for (int offset = pixels; offset < bytes.size(); offset += block.size()) {
        bytes.replace(offset, block.size(), block);
    }
    return bytes;
}

void checkImage(int width, int height, bool white) {
    QString error;
    const fls::SwatchImage image = fls::decodeSwatchImage(swatch(width, height, solidBlock(white), 9), &error);
    require(image.valid() && image.width == width && image.height == height, qPrintable(error));
    const uint8_t expected = white ? 255 : 0;
    for (uint8_t channel : image.rgba) {
        require(channel == expected, "BC7 pixel mismatch");
    }
}

void checkBc4Image() {
    QByteArray block(8, '\0');
    block[0] = 32;
    block[1] = static_cast<char>(224);
    block[2] = 8;
    const auto bytes = swatch(5, 3, block, 3);
    QString error;
    const auto image = fls::decodeSwatchImage(bytes, &error);
    const auto mask = fls::decodeSwatchMask(bytes, &error);
    require(image.valid() && mask.valid(), qPrintable(error));
    require(image.width == 5 && image.height == 3, "BC4 image dimensions changed");
    require(image.rgba[0] == 32 && image.rgba[4] == 224, "BC4 endpoints changed");
    for (size_t pixel = 0; pixel < mask.coverage.size(); ++pixel) {
        for (int channel = 0; channel < 3; ++channel)
            require(image.rgba[pixel * 4 + channel] == mask.coverage[pixel], "BC4 color/mask pixels differ");
        require(image.rgba[pixel * 4 + 3] == 255, "BC4 color image alpha is not opaque");
    }
    require(!fls::decodeSwatchImage(bytes.left(bytes.size() - 1)).valid(), "truncated BC4 color data accepted");
}

void checkSignedBlockChannels() {
    QByteArray block(8, '\0');
    block[0] = '\x7f';
    block[1] = static_cast<char>(0x81);
    block[2] = static_cast<char>(0x88);
    block[3] = '\x06';
    const QByteArray bytes = swatch(4, 4, block, 4);
    const auto mask = fls::decodeSwatchMask(bytes);
    const auto image = fls::decodeSwatchImage(bytes);
    require(mask.valid() && image.valid(), "signed BC4 surface did not decode");
    require(mask.at(0, 0) == 255 && mask.at(1, 0) == 0 && mask.at(2, 0) == 182,
            "signed BC4 endpoint or interpolation changed");
    require(image.rgba[0] == 255 && image.rgba[4] == 0 && image.rgba[8] == 182,
            "signed BC4 color and coverage differ");

    const auto normal = fls::decodeSwatchImage(swatch(4, 4, block + QByteArray(8, '\0'), 6));
    require(normal.valid() && normal.rgba[0] == 255 && normal.rgba[4] == 0
            && normal.rgba[1] == 128 && normal.rgba[5] == 128,
            "signed BC5 directions were not remapped for normal sampling");
}

void checkDurangoBc4Mask(int tileMode, int payloadSize) {
    constexpr int width = 301;
    constexpr int height = 141;
    constexpr int header = 52;
    constexpr int headerSize = 35;
    constexpr int payload = header + headerSize;
    struct Sample {
        int column;
        int row;
        int block1D;
        int block2D;
    };
    constexpr Sample samples[] = {
        {0, 0, 0, 0}, {2, 4, 36, 1028}, {8, 8, 704, 512},
        {31, 31, 2175, 1855}, {33, 32, 2817, 4353}, {64, 31, 2474, 3690},
        {75, 35, 3151, 6319},
    };
    QByteArray bytes = swatch(4, 4, QByteArray(8, '\0'), 3).left(header);
    bytes.append(QByteArray(headerSize + payloadSize, '\0'));
    const auto u16 = [&](int offset, quint16 value) { qToLittleEndian(value, bytes.data() + offset); };
    const auto u32 = [&](int offset, quint32 value) { qToLittleEndian(value, bytes.data() + offset); };
    bytes[24] = 2;
    u32(32, payload);
    u32(36, payloadSize);
    u32(40, payloadSize);
    u16(48, headerSize << 4);
    for (int offset : {16, 22}) {
        u16(header + offset, width);
        u16(header + offset + 2, height);
        u16(header + offset + 4, 1);
    }
    bytes[header + 28] = bytes[header + 29] = 1;
    u32(header + 31, static_cast<quint32>(tileMode) | (3u << 5));

    std::vector<uint8_t> expected(width * height);
    int coverage = 32;
    for (const Sample &sample : samples) {
        const int block = tileMode == 13 ? sample.block1D : sample.block2D;
        bytes[payload + block * 8] = static_cast<char>(coverage);
        const int x = sample.column * 4;
        const int y = sample.row * 4;
        for (int row = y; row < std::min(y + 4, height); ++row) {
            std::fill_n(expected.begin() + row * width + x, std::min(4, width - x), coverage);
        }
        coverage += 32;
    }
    QString error;
    const auto mask = fls::decodeSwatchMask(bytes, &error);
    require(mask.valid(), qPrintable(error));
    require(mask.width == width && mask.height == height && mask.coverage == expected,
            "Durango BC4 coverage differs from the reference tile layout");

    bytes.chop(8);
    u32(36, payloadSize - 8);
    u32(40, payloadSize - 8);
    require(!fls::decodeSwatchMask(bytes).valid(), "truncated Durango tiled surface accepted");
}

void checkAuthoredMips() {
    constexpr int header = 52;
    constexpr int payload = header + 100;
    QByteArray block(8, '\0');
    block[0] = block[1] = 32;
    QByteArray bytes = swatch(8, 4, block, 3);
    bytes.insert(header + 68, QByteArray(32, '\0'));
    bytes.append(block);
    bytes[payload] = bytes[payload + 1] = static_cast<char>(224);
    const auto u32 = [&](int offset, quint32 value) { qToLittleEndian(value, bytes.data() + offset); };
    u32(32, payload);
    u32(36, 24);
    u32(40, 24);
    u32(header + 32, 1);
    qToLittleEndian<quint16>(1, bytes.data() + header + 36);
    bytes[header + 38] = 2;
    u32(header + 68, 76);
    u32(header + 72, 4294967295u);
    u32(header + 76, 16);
    u32(header + 80, 8);
    u32(header + 84, 88);
    u32(header + 88, 8);
    u32(header + 92, 0);
    u32(header + 96, 4294967295u);
    QString error;
    const auto levels = fls::decodeSwatchMipImages(bytes, &error);
    require(levels.size() == 2, qPrintable(error));
    require(levels[0].width == 8 && levels[0].height == 4 && levels[0].rgba[0] == 32, "base mip descriptor ignored");
    require(levels[1].width == 4 && levels[1].height == 2 && levels[1].rgba[0] == 224, "lower mip descriptor ignored");
    require(fls::decodeSwatchImage(bytes).rgba == levels[0].rgba, "base image and mip image disagree");
    u32(header + 92, 24);
    require(fls::decodeSwatchMipImages(bytes).empty(), "invalid lower mip payload accepted");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        checkImage(4, 4, true);
        checkImage(8, 4, false);
        checkImage(2, 2, true);
        checkImage(1, 1, false);
        checkBc4Image();
        checkSignedBlockChannels();
        checkDurangoBc4Mask(13, 25600);
        checkDurangoBc4Mask(14, 65536);
        checkAuthoredMips();
        std::vector<std::future<void>> requests;
        for (int index = 0; index < 8; ++index) {
            requests.push_back(std::async(std::launch::async, [index]() {
                for (int repeat = 0; repeat < 4; ++repeat) {
                    checkImage(index % 2 ? 4 : 8, 4, (index + repeat) % 2);
                }
            }));
        }
        for (auto &request : requests) {
            request.get();
        }
        qInfo("BC4 color images, BC7 pixels, dimension changes, and concurrent decoding checks passed");
        return 0;
    } catch (const std::exception &error) {
        qCritical("%s", error.what());
        return 1;
    }
}
