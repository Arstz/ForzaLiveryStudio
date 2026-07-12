#pragma once

#include <QByteArray>
#include <QString>

namespace fh6 {

// Inflate a `[u32 comp_len][u32 uncomp_len] + zlib` container blob (the wrapper
// shared by C_group and C_livery files). Throws on malformed input.
QByteArray inflateContainer(const QByteArray &wrapped);
// Inflate only the first container from a multi-container file, ignoring any
// trailing data after the first zlib stream. Throws on malformed input.
QByteArray inflateFirstContainer(const QByteArray &wrapped);
QByteArray readCGroupPayload(const QString &folderOrFile);
void writeCGroupFile(const QString &path, const QByteArray &payload);

} // namespace fh6
