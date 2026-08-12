#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

namespace gui {

QStringList supportedImageSuffixes();

QString imageDialogFilter();

bool isSvgGuideFormat(const QString &format);
QImage readGuideImage(const QString &path, QByteArray *format, QString *error);
QImage decodeGuideImage(const QByteArray &bytes, const QString &format,
                        QString *error = nullptr);
QImage readThumbnailImage(const QString &path, QString *error = nullptr);

QByteArray encodeGuideImage(const QImage &image, QString *formatOut);

} // namespace gui
