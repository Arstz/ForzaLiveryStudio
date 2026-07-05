#pragma once

#include <QString>

class QWidget;

namespace gui {

// Locates the most recently modified Xbox game-save ContainersRoot folder.
QString mostRecentContainersRoot();

// Resolves the directory an import dialog should open in (remembered setting,
// then auto-detected ContainersRoot, then a one-time prompt).
QString importDialogStartDirectory(QWidget *parent);
QString importDialogStartDirectory(QWidget *parent, const QString &actionKey);

// Persists the folder of an imported path so future imports start there.
void rememberImportDirectory(const QString &path);
void rememberImportDirectory(const QString &path, const QString &actionKey);

} // namespace gui
