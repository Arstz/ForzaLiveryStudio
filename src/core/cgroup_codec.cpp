#include "cgroup_codec.h"

#include "binary_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <stdexcept>

#include <zlib.h>

namespace fh6 {
namespace {

QString resolveCGroupPath(const QString &folderOrFile)
{
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        return QDir(folderOrFile).filePath(QStringLiteral("C_group"));
    }
    return folderOrFile;
}

} // namespace

using detail::appendLeU32;
using detail::readLeU32;

QByteArray inflateContainer(const QByteArray &wrapped)
{
    if (wrapped.size() < 8) {
        throw std::runtime_error("container is shorter than wrapper header");
    }

    const quint32 compressedSize = readLeU32(wrapped, 0);
    const quint32 decompressedSize = readLeU32(wrapped, 4);
    const QByteArray compressed = wrapped.mid(8);
    if (compressedSize != static_cast<quint32>(compressed.size())) {
        throw std::runtime_error("container compressed-size header does not match file size");
    }

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

QByteArray readCGroupPayload(const QString &folderOrFile)
{
    QFile file(resolveCGroupPath(folderOrFile));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open C_group: " + file.fileName()).toStdString());
    }
    return inflateContainer(file.readAll());
}

void writeCGroupFile(const QString &path, const QByteArray &payload)
{
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

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(("could not write C_group: " + path).toStdString());
    }
    if (file.write(wrapped) != wrapped.size()) {
        throw std::runtime_error("short write while writing C_group");
    }
}

} // namespace fh6
