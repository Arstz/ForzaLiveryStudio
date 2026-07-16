#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

namespace gui {

QStringList supportedImageSuffixes();

QString imageDialogFilter();

QImage readGuideImage(const QString &path, QByteArray *format, QString *error);
QImage readThumbnailImage(const QString &path, QString *error = nullptr);

QByteArray encodeGuideImage(const QImage &image, QString *formatOut);

} // namespace gui
