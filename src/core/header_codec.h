#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace fh6 {

struct HeaderMetadata {
    quint32 formatVersion = 7;
    QString name;
    bool published = false;
    QString description;
    quint16 year = 0;
    quint8 month = 0;
    quint8 day = 0;
    QByteArray fieldBlock;
    QByteArray creatorTag;
    QString creatorName;
    quint32 typeValue = 0;
    QByteArray guid;
    QByteArray trailing;
    QByteArray publishedTail;
    bool parsedOk = false;
};

HeaderMetadata parseHeader(const QByteArray &bytes);
QByteArray buildHeader(const HeaderMetadata &meta);

HeaderMetadata defaultDraftHeader(const QString &name, const QString &creatorName);

} // namespace fh6
