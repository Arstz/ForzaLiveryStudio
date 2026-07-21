#include "header_codec.h"

#include "binary_io.h"

#include <QDate>
#include <QUuid>

#include <stdexcept>

namespace fh6 {
namespace {

constexpr int CreatorTagSize = 8;   // tag1[4] + tag2[2] + sep[2]
constexpr int FieldBlockSize = 16;  // fieldA..pad
constexpr int GuidSize = 16;
constexpr int PaddingBeforeSec3 = 28;

QString readUtf16(const QByteArray &bytes, int offset, quint32 charCount) {
    const qsizetype byteCount = static_cast<qsizetype>(charCount) * 2;
    if (offset < 0 || offset + byteCount > bytes.size()) {
        throw std::runtime_error("unexpected end of data while reading UTF-16 string");
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(bytes.constData() + offset),
        static_cast<qsizetype>(charCount));
}

void appendUtf16(QByteArray &out, const QString &text) {
    out.append(reinterpret_cast<const char *>(text.utf16()),
               text.size() * static_cast<int>(sizeof(char16_t)));
}

} // namespace

HeaderMetadata parseHeader(const QByteArray &bytes) {
    HeaderMetadata meta;
    if (bytes.size() < 8) {
        throw std::runtime_error("header too small to parse");
    }

    int offset = 0;
    meta.formatVersion = detail::readLeU32(bytes, offset);
    offset += 4;
    const quint32 nameLen = detail::readLeU32(bytes, offset);
    offset += 4;
    meta.name = readUtf16(bytes, offset, nameLen);
    offset += static_cast<int>(nameLen) * 2;

    const quint32 descLenOrNull = detail::readLeU32(bytes, offset);
    offset += 4;
    if (descLenOrNull != 0) {
        meta.published = true;
        meta.description = readUtf16(bytes, offset, descLenOrNull);
        offset += static_cast<int>(descLenOrNull) * 2;
    } else {
        meta.published = false;
    }

    meta.year = detail::readLeU16(bytes, offset);
    meta.month = static_cast<quint8>(bytes.at(offset + 2));
    meta.day = static_cast<quint8>(bytes.at(offset + 3));
    offset += 4;

    meta.fieldBlock = bytes.mid(offset, FieldBlockSize);
    if (meta.fieldBlock.size() != FieldBlockSize) {
        throw std::runtime_error("header truncated in field block");
    }
    offset += FieldBlockSize;

    meta.creatorTag = bytes.mid(offset, CreatorTagSize);
    if (meta.creatorTag.size() != CreatorTagSize) {
        throw std::runtime_error("header truncated in creator tag");
    }
    offset += CreatorTagSize;

    const quint32 creatorLen = detail::readLeU32(bytes, offset);
    offset += 4;
    meta.creatorName = readUtf16(bytes, offset, creatorLen);
    offset += static_cast<int>(creatorLen) * 2;

    meta.sectionPrefix = bytes.mid(offset, PaddingBeforeSec3);
    if (meta.sectionPrefix.size() != PaddingBeforeSec3) {
        throw std::runtime_error("header truncated before sec3 marker");
    }
    offset += PaddingBeforeSec3;
    if (!detail::bytesAt(bytes, offset, {0x01, 0x02})) {
        throw std::runtime_error("header sec3 marker not found at expected offset");
    }
    offset += 9;
    meta.typeValue = detail::readLeU32(bytes, offset);
    offset += 4;
    meta.carId = detail::readLeU32(bytes, offset);
    offset += 4;

    meta.guid = bytes.mid(offset, GuidSize);
    if (meta.guid.size() != GuidSize) {
        throw std::runtime_error("header truncated in GUID");
    }
    offset += GuidSize;

    meta.trailing = bytes.mid(offset);
    meta.parsedOk = true;
    return meta;
}

QByteArray buildHeader(const HeaderMetadata &meta) {
    QByteArray out;
    detail::appendLeU32(out, meta.formatVersion);
    detail::appendLeU32(out, static_cast<quint32>(meta.name.size()));
    appendUtf16(out, meta.name);

    if (meta.published) {
        detail::appendLeU32(out, static_cast<quint32>(meta.description.size()));
        appendUtf16(out, meta.description);
        if (meta.sectionPrefix.isEmpty() && !meta.publishedTail.isEmpty()) {
            out.append(meta.publishedTail);
            return out;
        }
    } else {
        detail::appendLeU32(out, 0);
    }

    detail::appendLeU16(out, meta.year);
    out.append(static_cast<char>(meta.month));
    out.append(static_cast<char>(meta.day));

    QByteArray fieldBlock = meta.fieldBlock;
    if (fieldBlock.isEmpty()) {
        fieldBlock = QByteArray(FieldBlockSize, '\0');
    }
    fieldBlock.resize(FieldBlockSize);
    out.append(fieldBlock);

    QByteArray creatorTag = meta.creatorTag;
    if (creatorTag.isEmpty()) {
        creatorTag = QByteArray(CreatorTagSize, '\0');
    }
    creatorTag.resize(CreatorTagSize);
    out.append(creatorTag);

    detail::appendLeU32(out, static_cast<quint32>(meta.creatorName.size()));
    appendUtf16(out, meta.creatorName);

    QByteArray sectionPrefix = meta.sectionPrefix.left(PaddingBeforeSec3);
    sectionPrefix.append(QByteArray(PaddingBeforeSec3 - sectionPrefix.size(), '\0'));
    out.append(sectionPrefix);
    out.append('\x01');
    out.append('\x02');
    out.append(QByteArray(7, '\0'));
    detail::appendLeU32(out, meta.typeValue);
    detail::appendLeU32(out, meta.carId);

    QByteArray guid = meta.guid;
    if (guid.isEmpty()) {
        guid = QUuid::createUuid().toRfc4122();
    }
    guid.resize(GuidSize);
    out.append(guid);

    out.append(meta.trailing);
    return out;
}

HeaderMetadata defaultDraftHeader(const QString &name, const QString &creatorName, quint32 carId) {
    HeaderMetadata meta;
    meta.formatVersion = 7;
    meta.name = name;
    meta.published = false;
    const QDate today = QDate::currentDate();
    meta.year = static_cast<quint16>(today.year());
    meta.month = static_cast<quint8>(today.month());
    meta.day = 0;
    meta.fieldBlock = QByteArray(FieldBlockSize, '\0');
    meta.fieldBlock[12] = 2;
    meta.creatorTag = QByteArray(CreatorTagSize, '\0');
    meta.creatorName = creatorName;
    meta.sectionPrefix = QByteArray(PaddingBeforeSec3, '\0');
    meta.typeValue = 0;
    meta.carId = carId;
    meta.guid = QUuid::createUuid().toRfc4122();
    meta.trailing.clear();
    meta.parsedOk = true;
    return meta;
}

} // namespace fh6
