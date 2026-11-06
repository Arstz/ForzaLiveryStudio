#include "swatchbin.h"

#include <QCoreApplication>
#include <QtEndian>

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

QByteArray swatch(int width, int height, bool white) {
    constexpr int metadata = 44;
    constexpr int header = metadata + 8;
    constexpr int pixels = header + 68;
    const QByteArray block = solidBlock(white);
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
    u32(header + 64, 9);
    for (int offset = pixels; offset < bytes.size(); offset += block.size()) {
        bytes.replace(offset, block.size(), block);
    }
    return bytes;
}

void checkImage(int width, int height, bool white) {
    QString error;
    const fls::SwatchImage image = fls::decodeSwatchImage(swatch(width, height, white), &error);
    require(image.valid() && image.width == width && image.height == height, qPrintable(error));
    const uint8_t expected = white ? 255 : 0;
    for (uint8_t channel : image.rgba) {
        require(channel == expected, "BC7 pixel mismatch");
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        checkImage(4, 4, true);
        checkImage(8, 4, false);
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
        qInfo("BC7 pixels, dimension changes, and concurrent decoding checks passed");
        return 0;
    } catch (const std::exception &error) {
        qCritical("%s", error.what());
        return 1;
    }
}
