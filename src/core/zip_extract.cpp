#include "zip_extract.h"

#include "binary_io.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <zlib.h>

#include <algorithm>
#include <limits>

namespace fh6 {
namespace {

using fh6::detail::readLeU16;
using fh6::detail::readLeU32;

constexpr quint32 kEndOfCentralDirectory = 0x06054b50;
constexpr quint32 kCentralDirectoryHeader = 0x02014b50;
constexpr quint32 kLocalFileHeader = 0x04034b50;
constexpr quint16 kMethodStored = 0;
constexpr quint16 kMethodDeflated = 8;
constexpr quint16 kFlagEncrypted = 1 << 0;
constexpr quint16 kFlagUtf8Name = 1 << 11;

bool hasBytes(const QByteArray &bytes, qsizetype offset, qsizetype count) {
    return offset >= 0 && count >= 0 && offset + count <= bytes.size();
}

int findEndOfCentralDirectory(const QByteArray &bytes) {
    const qsizetype maxComment = 0xffff;
    const qsizetype minSize = 22;
    const qsizetype start = std::max<qsizetype>(0, bytes.size() - (maxComment + minSize));
    for (int pos = bytes.size() - minSize; pos >= start; --pos) {
        if (readLeU32(bytes, pos) == kEndOfCentralDirectory) {
            return pos;
        }
    }
    return -1;
}

QString decodeEntryName(const QByteArray &raw, quint16 flags) {
    return (flags & kFlagUtf8Name) != 0 ? QString::fromUtf8(raw) : QString::fromLatin1(raw);
}

bool isSafeEntryPath(QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path)) {
        return false;
    }
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }
    for (const QString &part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..") || part.contains(QLatin1Char(':'))) {
            return false;
        }
    }
    return true;
}

bool inflateRaw(const QByteArray &input, quint32 uncompressedSize, QByteArray &output, QString *error) {
    if (uncompressedSize > static_cast<quint32>(std::numeric_limits<int>::max())) {
        if (error != nullptr) {
            *error = QStringLiteral("zip entry is too large to extract");
        }
        return false;
    }
    output.resize(static_cast<int>(uncompressedSize));

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to initialize zip inflater");
        }
        return false;
    }
    const int rc = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (rc != Z_STREAM_END || stream.total_out != uncompressedSize) {
        if (error != nullptr) {
            *error = QStringLiteral("failed to inflate zip entry");
        }
        return false;
    }
    return true;
}

bool writeEntry(const QString &path, const QByteArray &data, QString *error) {
    const QFileInfo info(path);
    QDir dir;
    if (!dir.mkpath(info.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot create %1").arg(info.absolutePath());
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot write %1").arg(path);
        }
        return false;
    }
    if (file.write(data) != data.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("short write extracting %1").arg(path);
        }
        return false;
    }
    return true;
}

QByteArray readRange(QFile &file, quint64 offset, quint64 size) {
    if (offset > static_cast<quint64>(file.size())
        || size > static_cast<quint64>(std::numeric_limits<int>::max())
        || offset + size > static_cast<quint64>(file.size())
        || !file.seek(static_cast<qint64>(offset))) {
        return {};
    }
    return file.read(static_cast<qint64>(size));
}

} // namespace

bool extractZipArchive(const QString &zipPath, const QString &destinationDir, QString *error) {
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1").arg(zipPath);
        }
        return false;
    }
    const QByteArray bytes = file.readAll();

    const int eocd = findEndOfCentralDirectory(bytes);
    if (eocd < 0 || !hasBytes(bytes, eocd, 22)) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: central directory not found");
        }
        return false;
    }

    const quint16 diskNumber = readLeU16(bytes, eocd + 4);
    const quint16 centralDisk = readLeU16(bytes, eocd + 6);
    const quint16 entryCount = readLeU16(bytes, eocd + 10);
    const quint32 centralSize = readLeU32(bytes, eocd + 12);
    const quint32 centralOffset = readLeU32(bytes, eocd + 16);
    if (diskNumber != 0 || centralDisk != 0
        || centralOffset == 0xffffffffu || centralSize == 0xffffffffu || entryCount == 0xffffu) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: unsupported multi-disk or ZIP64 archive");
        }
        return false;
    }
    if (!hasBytes(bytes, centralOffset, centralSize)) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: central directory is out of range");
        }
        return false;
    }

    QDir destination(destinationDir);
    if (!destination.exists() && !QDir().mkpath(destinationDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot create %1").arg(destinationDir);
        }
        return false;
    }

    qsizetype pos = centralOffset;
    for (quint16 i = 0; i < entryCount; ++i) {
        if (!hasBytes(bytes, pos, 46) || readLeU32(bytes, static_cast<int>(pos)) != kCentralDirectoryHeader) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: malformed central directory");
            }
            return false;
        }

        const quint16 flags = readLeU16(bytes, static_cast<int>(pos + 8));
        const quint16 method = readLeU16(bytes, static_cast<int>(pos + 10));
        const quint32 expectedCrc = readLeU32(bytes, static_cast<int>(pos + 16));
        const quint32 compressedSize = readLeU32(bytes, static_cast<int>(pos + 20));
        const quint32 uncompressedSize = readLeU32(bytes, static_cast<int>(pos + 24));
        const quint16 nameLen = readLeU16(bytes, static_cast<int>(pos + 28));
        const quint16 extraLen = readLeU16(bytes, static_cast<int>(pos + 30));
        const quint16 commentLen = readLeU16(bytes, static_cast<int>(pos + 32));
        const quint32 localOffset = readLeU32(bytes, static_cast<int>(pos + 42));
        const qsizetype nameOffset = pos + 46;
        if (compressedSize == 0xffffffffu || uncompressedSize == 0xffffffffu || localOffset == 0xffffffffu
            || !hasBytes(bytes, nameOffset, nameLen)) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: unsupported ZIP64 entry");
            }
            return false;
        }

        QString entryName = decodeEntryName(bytes.mid(nameOffset, nameLen), flags);
        entryName.replace(QLatin1Char('\\'), QLatin1Char('/'));
        pos = nameOffset + nameLen + extraLen + commentLen;

        if (!isSafeEntryPath(entryName)) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: unsafe entry path %1").arg(entryName);
            }
            return false;
        }
        const bool isDirectory = entryName.endsWith(QLatin1Char('/'));
        const QString outputPath = destination.filePath(entryName);
        if (isDirectory) {
            if (!QDir().mkpath(outputPath)) {
                if (error != nullptr) {
                    *error = QStringLiteral("cannot create %1").arg(outputPath);
                }
                return false;
            }
            continue;
        }

        if ((flags & kFlagEncrypted) != 0) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: encrypted entry is unsupported: %1").arg(entryName);
            }
            return false;
        }
        if (method != kMethodStored && method != kMethodDeflated) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: unsupported compression method %1 for %2").arg(method).arg(entryName);
            }
            return false;
        }
        if (!hasBytes(bytes, localOffset, 30) || readLeU32(bytes, localOffset) != kLocalFileHeader) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: malformed local header for %1").arg(entryName);
            }
            return false;
        }
        const quint16 localNameLen = readLeU16(bytes, localOffset + 26);
        const quint16 localExtraLen = readLeU16(bytes, localOffset + 28);
        const qsizetype dataOffset = static_cast<qsizetype>(localOffset) + 30 + localNameLen + localExtraLen;
        if (!hasBytes(bytes, dataOffset, compressedSize)) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: compressed data is out of range for %1").arg(entryName);
            }
            return false;
        }

        QByteArray data;
        const QByteArray compressed = bytes.mid(dataOffset, compressedSize);
        if (method == kMethodStored) {
            data = compressed;
            if (data.size() != static_cast<int>(uncompressedSize)) {
                if (error != nullptr) {
                    *error = QStringLiteral("zip: stored size mismatch for %1").arg(entryName);
                }
                return false;
            }
        } else if (!inflateRaw(compressed, uncompressedSize, data, error)) {
            if (error != nullptr && !error->contains(entryName)) {
                *error = QStringLiteral("%1: %2").arg(entryName, *error);
            }
            return false;
        }

        const quint32 actualCrc = crc32(0L, reinterpret_cast<const Bytef *>(data.constData()), static_cast<uInt>(data.size()));
        if (actualCrc != expectedCrc) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: CRC mismatch for %1").arg(entryName);
            }
            return false;
        }
        if (!writeEntry(outputPath, data, error)) {
            return false;
        }
    }

    return true;
}

QHash<QString, QByteArray> readZipEntries(
    const QString &zipPath, const QStringList &entryPaths, QString *error) {
    QHash<QString, QString> wanted;
    for (QString path : entryPaths) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (path.startsWith(QLatin1Char('/'))) {
            path.remove(0, 1);
        }
        if (!path.isEmpty()) {
            wanted.insert(path.toLower(), path);
        }
    }
    if (wanted.isEmpty()) {
        return {};
    }

    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1").arg(zipPath);
        }
        return {};
    }

    const quint64 tailSize = std::min<quint64>(static_cast<quint64>(file.size()), 0xffffu + 22u);
    const QByteArray tail = readRange(file, static_cast<quint64>(file.size()) - tailSize, tailSize);
    const int relativeEocd = findEndOfCentralDirectory(tail);
    if (relativeEocd < 0 || !hasBytes(tail, relativeEocd, 22)) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: central directory not found");
        }
        return {};
    }
    const quint16 entryCount = readLeU16(tail, relativeEocd + 10);
    const quint32 centralSize = readLeU32(tail, relativeEocd + 12);
    const quint32 centralOffset = readLeU32(tail, relativeEocd + 16);
    if (entryCount == 0xffffu || centralSize == 0xffffffffu || centralOffset == 0xffffffffu) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: ZIP64 archive is unsupported");
        }
        return {};
    }
    const QByteArray central = readRange(file, centralOffset, centralSize);
    if (central.size() != static_cast<int>(centralSize)) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: central directory is out of range");
        }
        return {};
    }

    QHash<QString, QByteArray> result;
    qsizetype pos = 0;
    for (quint16 i = 0; i < entryCount; ++i) {
        if (!hasBytes(central, pos, 46)
            || readLeU32(central, static_cast<int>(pos)) != kCentralDirectoryHeader) {
            break;
        }
        const quint16 flags = readLeU16(central, static_cast<int>(pos + 8));
        const quint16 method = readLeU16(central, static_cast<int>(pos + 10));
        const quint32 expectedCrc = readLeU32(central, static_cast<int>(pos + 16));
        const quint32 compressedSize = readLeU32(central, static_cast<int>(pos + 20));
        const quint32 uncompressedSize = readLeU32(central, static_cast<int>(pos + 24));
        const quint16 nameLen = readLeU16(central, static_cast<int>(pos + 28));
        const quint16 extraLen = readLeU16(central, static_cast<int>(pos + 30));
        const quint16 commentLen = readLeU16(central, static_cast<int>(pos + 32));
        const quint32 localOffset = readLeU32(central, static_cast<int>(pos + 42));
        const qsizetype nameOffset = pos + 46;
        if (!hasBytes(central, nameOffset, nameLen)) {
            break;
        }
        QString name = decodeEntryName(central.mid(nameOffset, nameLen), flags);
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        pos = nameOffset + nameLen + extraLen + commentLen;
        const QString key = name.toLower();
        if (!wanted.contains(key)) {
            continue;
        }
        if ((flags & kFlagEncrypted) != 0
            || (method != kMethodStored && method != kMethodDeflated)
            || compressedSize == 0xffffffffu || uncompressedSize == 0xffffffffu
            || localOffset == 0xffffffffu) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: unsupported entry %1").arg(name);
            }
            return {};
        }
        const QByteArray localHeader = readRange(file, localOffset, 30);
        if (localHeader.size() != 30 || readLeU32(localHeader, 0) != kLocalFileHeader) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: malformed local header for %1").arg(name);
            }
            return {};
        }
        const quint16 localNameLen = readLeU16(localHeader, 26);
        const quint16 localExtraLen = readLeU16(localHeader, 28);
        const quint64 dataOffset = static_cast<quint64>(localOffset) + 30 + localNameLen + localExtraLen;
        const QByteArray compressed = readRange(file, dataOffset, compressedSize);
        if (compressed.size() != static_cast<int>(compressedSize)) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: compressed data is out of range for %1").arg(name);
            }
            return {};
        }
        QByteArray data;
        if (method == kMethodStored) {
            data = compressed;
        } else if (!inflateRaw(compressed, uncompressedSize, data, error)) {
            return {};
        }
        if (data.size() != static_cast<int>(uncompressedSize)
            || crc32(0L, reinterpret_cast<const Bytef *>(data.constData()),
                     static_cast<uInt>(data.size())) != expectedCrc) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: entry integrity check failed for %1").arg(name);
            }
            return {};
        }
        result.insert(key, data);
        if (result.size() == wanted.size()) {
            return result;
        }
    }

    if (error != nullptr) {
        QStringList missing;
        for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
            if (!result.contains(it.key())) {
                missing.push_back(it.value());
            }
        }
        if (!missing.isEmpty()) {
            *error = QStringLiteral("zip: entries not found: %1").arg(missing.join(QStringLiteral(", ")));
        }
    }
    return result;
}

QStringList listZipEntries(const QString &zipPath, QString *error) {
    QFile file(zipPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1").arg(zipPath);
        }
        return {};
    }

    const quint64 tailSize = std::min<quint64>(static_cast<quint64>(file.size()), 0xffffu + 22u);
    const QByteArray tail = readRange(file, static_cast<quint64>(file.size()) - tailSize, tailSize);
    const int relativeEocd = findEndOfCentralDirectory(tail);
    if (relativeEocd < 0 || !hasBytes(tail, relativeEocd, 22)) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: central directory not found");
        }
        return {};
    }
    const quint16 entryCount = readLeU16(tail, relativeEocd + 10);
    const quint32 centralSize = readLeU32(tail, relativeEocd + 12);
    const quint32 centralOffset = readLeU32(tail, relativeEocd + 16);
    if (entryCount == 0xffffu || centralSize == 0xffffffffu || centralOffset == 0xffffffffu) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: ZIP64 archive is unsupported");
        }
        return {};
    }
    const QByteArray central = readRange(file, centralOffset, centralSize);
    if (central.size() != static_cast<int>(centralSize)) {
        if (error != nullptr) {
            *error = QStringLiteral("zip: central directory is out of range");
        }
        return {};
    }

    QStringList names;
    names.reserve(entryCount);
    qsizetype pos = 0;
    for (quint16 i = 0; i < entryCount; ++i) {
        if (!hasBytes(central, pos, 46)
            || readLeU32(central, static_cast<int>(pos)) != kCentralDirectoryHeader) {
            break;
        }
        const quint16 flags = readLeU16(central, static_cast<int>(pos + 8));
        const quint16 nameLen = readLeU16(central, static_cast<int>(pos + 28));
        const quint16 extraLen = readLeU16(central, static_cast<int>(pos + 30));
        const quint16 commentLen = readLeU16(central, static_cast<int>(pos + 32));
        const qsizetype nameOffset = pos + 46;
        if (!hasBytes(central, nameOffset, nameLen)) {
            break;
        }
        QString name = decodeEntryName(central.mid(nameOffset, nameLen), flags);
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        pos = nameOffset + nameLen + extraLen + commentLen;
        if (!name.endsWith(QLatin1Char('/'))) {
            names.push_back(name);
        }
    }

    return names;
}

QByteArray readZipEntry(const QString &zipPath, const QString &entryPath, QString *error) {
    QString key = entryPath;
    key.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (key.startsWith(QLatin1Char('/'))) {
        key.remove(0, 1);
    }
    return readZipEntries(zipPath, QStringList{key}, error).value(key.toLower());
}

} // namespace fh6
