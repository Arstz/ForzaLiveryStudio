#pragma once

#include <QString>

class QWidget;

namespace gui {

QString mostRecentContainersRoot();

QString importDialogStartDirectory(QWidget *parent);
QString importDialogStartDirectory(QWidget *parent, const QString &actionKey);

void rememberImportDirectory(const QString &path);
void rememberImportDirectory(const QString &path, const QString &actionKey);

} // namespace gui
