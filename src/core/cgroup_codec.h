#pragma once

#include <QByteArray>
#include <QString>

namespace fh6 {

QByteArray inflateContainer(const QByteArray &wrapped);
QByteArray inflateFirstContainer(const QByteArray &wrapped);
QByteArray readCGroupPayload(const QString &folderOrFile);
void writeCGroupFile(const QString &path, const QByteArray &payload);

} // namespace fh6
