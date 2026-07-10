#pragma once

// Minimal ZIP extractor for car model archives. Supports the ordinary single-disk
// ZIP files used for car folders: stored and deflated entries, with a central
// directory. Extraction rejects absolute paths and parent-directory traversal.

#include <QString>

namespace fh6 {

bool extractZipArchive(const QString &zipPath, const QString &destinationDir, QString *error = nullptr);

} // namespace fh6
