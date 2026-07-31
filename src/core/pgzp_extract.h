#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace fh6 {

struct PgzpExtractedEntry {
    QString requestedName;
    QString manifestPath;
    QByteArray bytes;
    quint16 parent = 0;
};

// Extracts named payloads from a ForzaTech PGZP v101 archive. The companion
// manifest's non-empty line order is the archive directory order.
std::vector<PgzpExtractedEntry> extractPgzpEntries(
    const QString &archivePath, const QString &manifestPath,
    const QStringList &requestedLeafNames, QString *error = nullptr);

} // namespace fh6
