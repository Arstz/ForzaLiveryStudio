#pragma once

#include <QIcon>
#include <QString>

namespace gui {

// Resolves a bundled XPM asset file name next to the executable or in the source tree.
QString assetPath(const QString &fileName);
QIcon assetIcon(const QString &fileName);
QIcon mirroredAssetIcon(const QString &fileName);

} // namespace gui
