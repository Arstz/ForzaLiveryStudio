#pragma once

#include <QByteArray>
#include <QHash>

#include <QString>
#include <QStringList>

namespace fls {

bool extractZipArchive(const QString &zipPath, const QString &destinationDir, QString *error = nullptr);

QByteArray readZipEntry(const QString &zipPath, const QString &entryPath, QString *error = nullptr);

QHash<QString, QByteArray> readZipEntries(
    const QString &zipPath, const QStringList &entryPaths, QString *error = nullptr);

QStringList listZipEntries(const QString &zipPath, QString *error = nullptr);

} // namespace fls
