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
constexpr int Sec3HeaderSize = 13;  // 01 02 + 7 zero bytes + u32 typeValue

QString readUtf16(const QByteArray &bytes, int offset, quint32 charCount)
{
    const qsizetype byteCount = static_cast<qsizetype>(charCount) * 2;
    if (offset < 0 || offset + byteCount > bytes.size()) {
        throw std::runtime_error("unexpected end of data while reading UTF-16 string");
    }
    return QString::fromUtf16(
        reinterpret_cast<const char16_t *>(bytes.constData() + offset),
        static_cast<qsizetype>(charCount));
}

void appendUtf16(QByteArray &out, const QString &text)
{
    out.append(reinterpret_cast<const char *>(text.utf16()),
               text.size() * static_cast<int>(sizeof(char16_t)));
}

QByteArray defaultTrailing(quint32 typeValue)
{
    // The 24-byte draft trailing observed in fixtures: u32 0, u32 typeValue copy,
    // then 16 zero bytes. Keeping the copy equal to typeValue preserves the
    // type/trailing correlation seen in the originals.
    QByteArray trailing;
    detail::appendLeU32(trailing, 0);
    detail::appendLeU32(trailing, typeValue);
    trailing.append(QByteArray(16, '\0'));
    return trailing;
}

} // namespace

HeaderMetadata parseHeader(const QByteArray &bytes)
{
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
        meta.publishedTail = bytes.mid(offset);
        meta.parsedOk = true;
        return meta;
    }

    meta.published = false;
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

    // 28 bytes of zero padding then the sec3 header at a deterministic offset.
    const int sec3Offset = offset + PaddingBeforeSec3;
    if (!detail::bytesAt(bytes, sec3Offset, {0x01, 0x02})) {
        throw std::runtime_error("header sec3 marker not found at expected offset");
    }
    offset = sec3Offset + 9; // skip 01 02 + 7 zero bytes
    meta.typeValue = detail::readLeU32(bytes, offset);
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

QByteArray buildHeader(const HeaderMetadata &meta)
{
    QByteArray out;
    detail::appendLeU32(out, meta.formatVersion);
    detail::appendLeU32(out, static_cast<quint32>(meta.name.size()));
    appendUtf16(out, meta.name);

    if (meta.published) {
        detail::appendLeU32(out, static_cast<quint32>(meta.description.size()));
        appendUtf16(out, meta.description);
        out.append(meta.publishedTail);
        return out;
    }

    detail::appendLeU32(out, 0); // descLenOrNull (draft)
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

    out.append(QByteArray(PaddingBeforeSec3, '\0'));
    out.append('\x01');
    out.append('\x02');
    out.append(QByteArray(7, '\0'));
    detail::appendLeU32(out, meta.typeValue);
    Q_UNUSED(Sec3HeaderSize);

    QByteArray guid = meta.guid;
    if (guid.isEmpty()) {
        guid = QUuid::createUuid().toRfc4122();
    }
    guid.resize(GuidSize);
    out.append(guid);

    out.append(meta.trailing.isEmpty() ? defaultTrailing(meta.typeValue) : meta.trailing);
    return out;
}

HeaderMetadata defaultDraftHeader(const QString &name, const QString &creatorName)
{
    HeaderMetadata meta;
    meta.formatVersion = 7;
    meta.name = name;
    meta.published = false;
    const QDate today = QDate::currentDate();
    meta.year = static_cast<quint16>(today.year());
    meta.month = static_cast<quint8>(today.month());
    meta.day = 0; // fixtures store day == 0
    meta.fieldBlock = QByteArray(FieldBlockSize, '\0');
    meta.fieldBlock[12] = 2; // only reliably-constant byte in the block
    meta.creatorTag = QByteArray(CreatorTagSize, '\0');
    meta.creatorName = creatorName;
    meta.typeValue = 0;
    meta.guid = QUuid::createUuid().toRfc4122();
    meta.trailing.clear(); // buildHeader synthesizes the 24-byte default
    meta.parsedOk = true;
    return meta;
}

} // namespace fh6
