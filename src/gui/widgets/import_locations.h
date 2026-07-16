#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace gui {

QString mostRecentContainersRoot();

QString importDialogStartDirectory(QWidget *parent);
QString importDialogStartDirectory(QWidget *parent, const QString &actionKey);
QString importBrowserStartDirectory(const QString &actionKey, const QStringList &fallbackActionKeys = {});

void rememberImportDirectory(const QString &path);
void rememberImportDirectory(const QString &path, const QString &actionKey);

} // namespace gui
