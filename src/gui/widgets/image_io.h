#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

namespace gui {

QStringList supportedImageSuffixes();

QString imageDialogFilter();

QImage readGuideImage(const QString &path, QByteArray *format, QString *error);

QByteArray encodeGuideImage(const QImage &image, QString *formatOut);

} // namespace gui
