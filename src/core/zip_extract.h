#pragma once


#include <QString>

namespace fh6 {

bool extractZipArchive(const QString &zipPath, const QString &destinationDir, QString *error = nullptr);

} // namespace fh6
