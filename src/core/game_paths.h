#pragma once

#include <QString>

namespace fh6 {

// Resolves the resource locations the editor needs from a single Forza game
// install folder. The folder may be the install root or its `media` directory.

QString gameMediaDir(const QString &gameFolder);
QString gameCarsDir(const QString &gameFolder);
QString gamePaintMaterialsArchive(const QString &gameFolder);
QString gamePaintTexturesArchive(const QString &gameFolder);

// Entry prefix of the customizable paint materials inside the materials archive.
QString gamePaintMaterialsPrefix();

} // namespace fh6
