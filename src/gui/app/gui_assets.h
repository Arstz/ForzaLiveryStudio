#pragma once

#include <QIcon>
#include <QString>

namespace gui {

QString iconAssetPath(const QString &fileName);
QIcon assetIcon(const QString &fileName);
QIcon mirroredAssetIcon(const QString &fileName);

} // namespace gui
