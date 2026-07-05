#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace fh6 {

// Parsed view of the binary vinyl `header` file.
//
// Only the fields the editor reads or edits are decoded. Every opaque or
// variable-length region (fieldBlock, creatorTag, typeValue, guid, trailing) is
// stored verbatim so that buildHeader(parseHeader(bytes)) == bytes byte-for-byte
// for any draft header we round-trip. See HEADER.md for the layout.
struct HeaderMetadata {
    quint32 formatVersion = 7;
    QString name;
    bool published = false;        // false => draft (descLenOrNull == 0)
    QString description;           // populated only when published
    quint16 year = 0;
    quint8 month = 0;
    quint8 day = 0;
    QByteArray fieldBlock;         // 16 bytes verbatim (fieldA..pad)
    QByteArray creatorTag;         // 8 bytes verbatim (tag1[4]+tag2[2]+sep[2])
    QString creatorName;
    quint32 typeValue = 0;         // sec3 "type" u32
    QByteArray guid;               // 16 bytes
    QByteArray trailing;           // verbatim trailing blob; empty => synthesize 24B default
    QByteArray publishedTail;      // verbatim bytes after the description (published passthrough)
    bool parsedOk = false;         // structured parse succeeded
};

// Round-trip contract: buildHeader(parseHeader(b)) == b for draft fixtures.
HeaderMetadata parseHeader(const QByteArray &bytes);
QByteArray buildHeader(const HeaderMetadata &meta);

// Sensible defaults for a brand-new offline draft (random GUID, zeroed opaque
// fields, current year).
HeaderMetadata defaultDraftHeader(const QString &name, const QString &creatorName);

} // namespace fh6
