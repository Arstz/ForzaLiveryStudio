#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

namespace gui {

// Lowercase image file suffixes the app accepts: everything Qt can decode plus the
// Windows WIC-backed formats. Unsorted (build order); callers sort if they need to.
QStringList supportedImageSuffixes();

// File-dialog name filter listing all decodable image suffixes.
QString imageDialogFilter();

// Decode a guide image: tries Qt's readers first, then a Windows WIC fallback.
// On success returns an ARGB32-premultiplied image and writes the detected format
// to *format (optional). On failure returns a null image and, if *error is empty,
// writes a descriptive message to *error (optional).
QImage readGuideImage(const QString &path, QByteArray *format, QString *error);

// Encode a decoded guide image to compressed bytes for embedding in project JSON.
// Prefers WEBP, falls back to PNG; writes the chosen format ("webp"/"png") to
// *formatOut. Returns empty on total failure.
QByteArray encodeGuideImage(const QImage &image, QString *formatOut);

} // namespace gui
