#include "pgzp_extract.h"

#include <QFile>
#include <QHash>
#include <QtEndian>

#include <algorithm>
#include <limits>

namespace fh6 {

namespace {

constexpr quint32 kPgzpMagic = 0x505a4750U;
constexpr quint32 kSupportedVersion = 101;
constexpr quint16 kStoredMethod = 0;
constexpr quint16 kRawLz4Method = 31;

struct DirectoryEntry {
    quint64 offset = 0;
    quint32 unpackedSize = 0;
    quint16 flags = 0;
    quint16 parent = 0;
};

quint32 readU32(const char *data) {
    return qFromLittleEndian<quint32>(data);
}

quint64 readU64(const char *data) {
    return qFromLittleEndian<quint64>(data);
}

quint16 readU16(const char *data) {
    return qFromLittleEndian<quint16>(data);
}

bool readExact(QFile *file, qint64 size, QByteArray *bytes, QString *error) {
    *bytes = file->read(size);
    if (bytes->size() == size) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral("truncated PGZP archive at offset %1")
                     .arg(file->pos());
    }
    return false;
}

bool readExtendedLength(
    const QByteArray &input, qsizetype *cursor, qsizetype *length,
    QString *error) {
    if (*length != 15) {
        return true;
    }
    while (true) {
        if (*cursor >= input.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("truncated LZ4 extended length");
            }
            return false;
        }
        const quint8 value = static_cast<quint8>(input.at((*cursor)++));
        if (*length > std::numeric_limits<qsizetype>::max() - value) {
            if (error != nullptr) {
                *error = QStringLiteral("LZ4 length overflow");
            }
            return false;
        }
        *length += value;
        if (value != 255) {
            return true;
        }
    }
}

QByteArray decompressRawLz4(
    const QByteArray &input, qsizetype expectedSize, QString *error) {
    QByteArray output;
    output.reserve(expectedSize);
    qsizetype cursor = 0;
    while (cursor < input.size()) {
        const quint8 token = static_cast<quint8>(input.at(cursor++));
        qsizetype literalLength = token >> 4;
        if (!readExtendedLength(input, &cursor, &literalLength, error)
            || literalLength > input.size() - cursor) {
            if (error != nullptr && error->isEmpty()) {
                *error = QStringLiteral("truncated LZ4 literal run");
            }
            return {};
        }
        output.append(input.constData() + cursor, literalLength);
        cursor += literalLength;
        if (cursor == input.size()) {
            break;
        }
        if (input.size() - cursor < 2) {
            if (error != nullptr) {
                *error = QStringLiteral("truncated LZ4 match offset");
            }
            return {};
        }
        const quint16 matchOffset =
            static_cast<quint8>(input.at(cursor))
            | (static_cast<quint16>(static_cast<quint8>(input.at(cursor + 1))) << 8);
        cursor += 2;
        if (matchOffset == 0 || matchOffset > output.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("invalid LZ4 match offset %1").arg(matchOffset);
            }
            return {};
        }
        qsizetype matchLength = token & 0x0f;
        if (!readExtendedLength(input, &cursor, &matchLength, error)) {
            return {};
        }
        matchLength += 4;
        if (matchLength > expectedSize - output.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("LZ4 output exceeds declared size");
            }
            return {};
        }
        for (qsizetype index = 0; index < matchLength; ++index) {
            output.append(output.at(output.size() - matchOffset));
        }
    }
    if (output.size() != expectedSize) {
        if (error != nullptr) {
            *error = QStringLiteral("LZ4 size mismatch: expected %1, decoded %2")
                         .arg(expectedSize)
                         .arg(output.size());
        }
        return {};
    }
    return output;
}

QString manifestLeaf(const QString &path) {
    const qsizetype slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return path.mid(slash + 1);
}

} // namespace

std::vector<PgzpExtractedEntry> extractPgzpEntries(
    const QString &archivePath, const QString &manifestPath,
    const QStringList &requestedLeafNames, QString *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (requestedLeafNames.isEmpty()) {
        return {};
    }

    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1: %2")
                         .arg(archivePath, archive.errorString());
        }
        return {};
    }
    QByteArray header;
    if (!readExact(&archive, 32, &header, error)) {
        return {};
    }
    const quint32 magic = readU32(header.constData());
    const quint32 version = readU32(header.constData() + 4);
    const quint32 folderOffset = readU32(header.constData() + 8);
    const quint32 count = readU32(header.constData() + 12);
    const quint32 folders = readU32(header.constData() + 16);
    const quint32 entriesPerChunk = readU32(header.constData() + 20);
    const quint32 chunks = readU32(header.constData() + 24);
    if (magic != kPgzpMagic || version != kSupportedVersion
        || count == 0 || entriesPerChunk == 0 || chunks == 0) {
        if (error != nullptr) {
            *error = QStringLiteral("unsupported PGZP header (magic %1, version %2)")
                         .arg(magic, 8, 16, QLatin1Char('0'))
                         .arg(version);
        }
        return {};
    }
    const quint64 folderBytes = (static_cast<quint64>(folders) * 4 + 4 + 7) & ~quint64(7);
    const quint64 directoryOffset = folderOffset + folderBytes;
    if (directoryOffset > static_cast<quint64>(archive.size())
        || !archive.seek(static_cast<qint64>(directoryOffset))) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid PGZP directory offset");
        }
        return {};
    }

    std::vector<DirectoryEntry> entries;
    entries.reserve(count);
    quint32 remaining = count;
    for (quint32 chunk = 0; chunk < chunks && remaining > 0; ++chunk) {
        QByteArray chunkHeader;
        if (!readExact(&archive, 8, &chunkHeader, error)) {
            return {};
        }
        const quint64 dataStart = readU64(chunkHeader.constData());
        const quint32 inChunk = std::min(entriesPerChunk, remaining);
        QByteArray directoryBytes;
        if (!readExact(&archive, static_cast<qint64>(inChunk) * 12, &directoryBytes, error)) {
            return {};
        }
        for (quint32 index = 0; index < inChunk; ++index) {
            const char *record = directoryBytes.constData() + index * 12;
            entries.push_back({
                dataStart + readU32(record), readU32(record + 4),
                readU16(record + 8), readU16(record + 10)});
        }
        remaining -= inChunk;
    }
    if (entries.size() != count) {
        if (error != nullptr) {
            *error = QStringLiteral("PGZP directory count mismatch");
        }
        return {};
    }

    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1: %2")
                         .arg(manifestPath, manifest.errorString());
        }
        return {};
    }
    QHash<QString, int> requested;
    for (int index = 0; index < requestedLeafNames.size(); ++index) {
        requested.insert(requestedLeafNames.at(index).toLower(), index);
    }
    std::vector<QString> paths(requestedLeafNames.size());
    std::vector<qsizetype> indices(requestedLeafNames.size(), -1);
    qsizetype manifestIndex = 0;
    while (!manifest.atEnd()) {
        QString line = QString::fromUtf8(manifest.readLine()).trimmed();
        if (manifestIndex == 0 && line.startsWith(QChar(0xfeff))) {
            line.remove(0, 1);
        }
        if (line.isEmpty()) {
            continue;
        }
        line.remove(QStringLiteral("<PREZIPPED>"), Qt::CaseInsensitive);
        const qsizetype separator = line.indexOf('|');
        const QString path = (separator < 0 ? line : line.left(separator)).trimmed();
        const auto found = requested.constFind(manifestLeaf(path).toLower());
        if (found != requested.cend()) {
            paths[found.value()] = path;
            indices[found.value()] = manifestIndex;
        }
        ++manifestIndex;
    }
    if (manifestIndex != static_cast<qsizetype>(entries.size())) {
        if (error != nullptr) {
            *error = QStringLiteral("manifest entries %1 do not match PGZP entries %2")
                         .arg(manifestIndex)
                         .arg(entries.size());
        }
        return {};
    }

    std::vector<PgzpExtractedEntry> result;
    result.reserve(requestedLeafNames.size());
    for (int requestedIndex = 0; requestedIndex < requestedLeafNames.size(); ++requestedIndex) {
        const qsizetype entryIndex = indices[requestedIndex];
        if (entryIndex < 0 || entryIndex + 1 >= static_cast<qsizetype>(entries.size())) {
            if (error != nullptr) {
                *error = QStringLiteral("PGZP entry not found: %1")
                             .arg(requestedLeafNames.at(requestedIndex));
            }
            return {};
        }
        const DirectoryEntry &entry = entries[entryIndex];
        const DirectoryEntry &next = entries[entryIndex + 1];
        const quint16 method = entry.flags & 0x0fff;
        const quint16 padding = entry.flags >> 12;
        if ((method != kStoredMethod && method != kRawLz4Method)
            || next.offset <= entry.offset + padding) {
            if (error != nullptr) {
                *error = QStringLiteral("unsupported PGZP payload contract for %1")
                             .arg(requestedLeafNames.at(requestedIndex));
            }
            return {};
        }
        const quint64 packedSize = next.offset - entry.offset - padding;
        if (packedSize > static_cast<quint64>(std::numeric_limits<qsizetype>::max())
            || entry.offset > static_cast<quint64>(archive.size())
            || packedSize > static_cast<quint64>(archive.size()) - entry.offset
            || !archive.seek(static_cast<qint64>(entry.offset))) {
            if (error != nullptr) {
                *error = QStringLiteral("invalid PGZP payload range for %1")
                             .arg(requestedLeafNames.at(requestedIndex));
            }
            return {};
        }
        QByteArray packed;
        if (!readExact(&archive, static_cast<qint64>(packedSize), &packed, error)) {
            return {};
        }
        QString decodeError;
        QByteArray decoded = method == kStoredMethod
            ? std::move(packed)
            : decompressRawLz4(
                  packed, static_cast<qsizetype>(entry.unpackedSize), &decodeError);
        if (decoded.size() != entry.unpackedSize || !decoded.startsWith("burG")) {
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2")
                             .arg(requestedLeafNames.at(requestedIndex),
                                  decodeError.isEmpty()
                                      ? QStringLiteral("decoded payload has invalid magic")
                                      : decodeError);
            }
            return {};
        }
        result.push_back({
            requestedLeafNames.at(requestedIndex), paths[requestedIndex],
            std::move(decoded), entry.parent});
    }
    return result;
}

} // namespace fh6
