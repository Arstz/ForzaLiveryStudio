#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace fls {

QJsonObject gamePaintDatabase(const QString &game);
int matchingPaintFinish(quint32 code, const QString &sourceGame, const QString &targetGame);
quint32 exportedPaintFinish(quint32 code, const QString &sourceGame, const QString &targetGame);

} // namespace fls
