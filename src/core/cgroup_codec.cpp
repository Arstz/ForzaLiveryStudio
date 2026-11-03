#include "cgroup_codec.h"

#include "binary_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <limits>
#include <stdexcept>

#include <zlib.h>

namespace fls {
namespace {

QString resolveCGroupPath(const QString &folderOrFile) {
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        const QDir directory(folderOrFile);

        return directory.filePath(directory.exists(QStringLiteral("C_group"))
            ? QStringLiteral("C_group") : QStringLiteral("data"));
    }
    return folderOrFile;
}

} // namespace

using detail::appendLeU32;
using detail::readLeU32;

QByteArray inflateFirstContainer(const QByteArray &wrapped) {
    if (wrapped.size() < 8) {
        throw std::runtime_error("container is shorter than wrapper header");
    }
    const quint32 compressedSize = readLeU32(wrapped, 0);
    const quint32 decompressedSize = readLeU32(wrapped, 4);
    const QByteArray compressed = wrapped.mid(8, compressedSize);

    QByteArray output;
    output.resize(static_cast<int>(decompressedSize));
    uLongf destinationSize = decompressedSize;
    const int status = uncompress(
        reinterpret_cast<Bytef *>(output.data()),
        &destinationSize,
        reinterpret_cast<const Bytef *>(compressed.constData()),
        static_cast<uLong>(compressed.size()));
    if (status != Z_OK || destinationSize != decompressedSize) {
        throw std::runtime_error("zlib decompression failed for container");
    }
    return output;
}

QByteArray inflateContainer(const QByteArray &wrapped) {
    if (wrapped.size() < 8) {
        throw std::runtime_error("container is shorter than wrapper header");
    }
    const quint32 compressedSize = readLeU32(wrapped, 0);
    if (compressedSize != static_cast<quint32>(wrapped.size()) - 8) {
        throw std::runtime_error("container compressed-size header does not match file size");
    }
    return inflateFirstContainer(wrapped);
}

QByteArray readCGroupPayload(const QString &folderOrFile) {
    QFile file(resolveCGroupPath(folderOrFile));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open C_group: " + file.fileName()).toStdString());
    }
    const QByteArray bytes = file.readAll();

    return bytes.startsWith("gyvl") ? bytes : inflateContainer(bytes);
}

QByteArray inflateContainerBlocks(const QByteArray &wrapped) {
    QByteArray payload;
    constexpr int kBlockHeaderBytes = 8;
    qsizetype offset = 0;
    while (offset < wrapped.size()) {
        if (wrapped.size() - offset < kBlockHeaderBytes) {
            throw std::runtime_error("container block header is truncated");
        }
        const quint32 compressedSize = readLeU32(wrapped, offset);
        const quint32 decompressedSize = readLeU32(wrapped, offset + 4);
        if (compressedSize == 0 || decompressedSize == 0
            || compressedSize > wrapped.size() - offset - kBlockHeaderBytes
            || decompressedSize > static_cast<quint32>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("container block size is invalid");
        }
        payload.append(inflateFirstContainer(wrapped.mid(offset, kBlockHeaderBytes + compressedSize)));
        offset += kBlockHeaderBytes + compressedSize;
    }
    if (payload.isEmpty()) {
        throw std::runtime_error("container has no blocks");
    }

    return payload;
}

QByteArray deflateContainerBlocks(const QByteArray &payload, int blockSize) {
    if (blockSize <= 0 || payload.isEmpty()) {
        throw std::runtime_error("invalid container block input");
    }
    QByteArray wrapped;
    for (qsizetype offset = 0; offset < payload.size(); offset += blockSize) {
        wrapped.append(deflateContainer(payload.mid(offset, blockSize)));
    }

    return wrapped;
}

QByteArray deflateContainer(const QByteArray &payload) {
    uLongf bound = compressBound(static_cast<uLong>(payload.size()));
    QByteArray compressed;
    compressed.resize(static_cast<int>(bound));
    int status = compress(
        reinterpret_cast<Bytef *>(compressed.data()),
        &bound,
        reinterpret_cast<const Bytef *>(payload.constData()),
        static_cast<uLong>(payload.size()));
    if (status != Z_OK) {
        throw std::runtime_error("zlib compression failed for C_group");
    }
    compressed.resize(static_cast<int>(bound));

    QByteArray wrapped;
    appendLeU32(wrapped, static_cast<quint32>(compressed.size()));
    appendLeU32(wrapped, static_cast<quint32>(payload.size()));
    wrapped.append(compressed);

    return wrapped;
}

void writeCGroupFile(const QString &path, const QByteArray &payload) {
    const QByteArray wrapped = deflateContainer(payload);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(("could not write C_group: " + path).toStdString());
    }
    if (file.write(wrapped) != wrapped.size()) {
        throw std::runtime_error("short write while writing C_group");
    }
}

} // namespace fls
