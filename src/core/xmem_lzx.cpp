#include "xmem_lzx.h"

#include <mspack.h>
#include <lzx.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>

namespace fls::detail {
namespace {

constexpr quint32 kXmemFrameSize = 32768;
constexpr quint8 kExtendedFrameMarker = 255;
constexpr int kLzxInputBufferSize = 4096;
constexpr std::array kLzxWindowBits = {16, 17, 15, 18, 19, 20, 21};

struct LzxInput : mspack_file {
    const QByteArray *bytes = nullptr;
    qsizetype offset = 0;
};

struct LzxOutput : mspack_file {
    QByteArray *bytes = nullptr;
    qsizetype offset = 0;
};

bool readXmemPayload(const QByteArray &input, quint32 uncompressedSize, QByteArray &payload) {
    const auto readBigEndian16 = [&input](qsizetype position) {
        return (static_cast<quint8>(input[position]) << 8) | static_cast<quint8>(input[position + 1]);
    };
    qsizetype offset = 0;
    quint32 produced = 0;

    payload.reserve(input.size());
    while (produced < uncompressedSize) {
        if (offset >= input.size()) return false;
        const bool extended = static_cast<quint8>(input[offset]) == kExtendedFrameMarker;
        const int headerSize = extended ? 5 : 2;
        if (input.size() - offset < headerSize) return false;
        const quint32 remaining = std::min(kXmemFrameSize, uncompressedSize - produced);
        const quint32 expanded = extended ? readBigEndian16(offset + 1) : remaining;
        const int compressed = readBigEndian16(offset + (extended ? 3 : 0));
        offset += headerSize;
        if (expanded == 0 || expanded > remaining || compressed == 0 || compressed > input.size() - offset) return false;
        payload.append(input.constData() + offset, compressed);
        offset += compressed;
        produced += expanded;
    }
    const QByteArray trailing = input.mid(offset);

    return trailing.isEmpty() || trailing == QByteArray(2, '\0') || trailing == QByteArray(5, '\0')
        || trailing == QByteArray::fromHex("ff00000000");
}

mspack_system memorySystem() {
    mspack_system system{};
    system.read = [](mspack_file *file, void *buffer, int requested) {
        auto *input = static_cast<LzxInput *>(file);
        const int count = static_cast<int>(std::min<qsizetype>(requested, input->bytes->size() - input->offset));
        std::memcpy(buffer, input->bytes->constData() + input->offset, count);
        input->offset += count;

        return count;
    };
    system.write = [](mspack_file *file, void *buffer, int count) {
        auto *output = static_cast<LzxOutput *>(file);
        if (count < 0 || count > output->bytes->size() - output->offset) return -1;
        std::memcpy(output->bytes->data() + output->offset, buffer, count);
        output->offset += count;

        return count;
    };
    system.alloc = [](mspack_system *, size_t bytes) { return std::malloc(bytes); };
    system.free = std::free;
    system.copy = [](void *source, void *destination, size_t bytes) { std::memcpy(destination, source, bytes); };

    return system;
}

} // namespace

bool decompressXmemLzx(const QByteArray &input, quint32 uncompressedSize,
                       quint32 expectedCrc, QByteArray &output, QString *error) {
    QByteArray payload;
    mspack_system system = memorySystem();

    output.clear();
    if (uncompressedSize > static_cast<quint32>(std::numeric_limits<int>::max())
        || !readXmemPayload(input, uncompressedSize, payload)) {
        if (error != nullptr) *error = QStringLiteral("zip: invalid XMem frame data");
        return false;
    }
    if (uncompressedSize == 0 && expectedCrc == 0) return true;
    QByteArray decoded(static_cast<int>(uncompressedSize), Qt::Uninitialized);
    for (int windowBits : kLzxWindowBits) {
        LzxInput source;
        LzxOutput destination;
        source.bytes = &payload;
        destination.bytes = &decoded;
        std::unique_ptr<lzxd_stream, decltype(&lzxd_free)> decoder(
            lzxd_init(&system, &source, &destination, windowBits, 0, kLzxInputBufferSize, uncompressedSize, 0), lzxd_free);
        if (!decoder || lzxd_decompress(decoder.get(), uncompressedSize) != MSPACK_ERR_OK
            || destination.offset != uncompressedSize) continue;
        if (crc32(0, reinterpret_cast<const Bytef *>(decoded.constData()), uncompressedSize) != expectedCrc) continue;
        output = std::move(decoded);

        return true;
    }
    if (error != nullptr) *error = QStringLiteral("zip: LZX decoding or integrity check failed");

    return false;
}

} // namespace fls::detail
