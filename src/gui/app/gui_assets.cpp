#include "gui_assets.h"

#include "theme_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QTransform>

namespace gui {
namespace {

QPixmap tintedPixmap(const QPixmap &source, const QColor &color)
{
    QPixmap tinted(source.size());
    tinted.setDevicePixelRatio(source.devicePixelRatio());
    tinted.fill(Qt::transparent);
    QPainter painter(&tinted);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), color);
    return tinted;
}

} // namespace

QString assetPath(const QString &fileName)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    QStringList candidates;
    candidates << QDir(appDir).filePath(QStringLiteral("assets/%1").arg(fileName))
               << QDir(cwd).filePath(QStringLiteral("assets/%1").arg(fileName))
               << QDir(cwd).filePath(QStringLiteral("cpp-port/assets/%1").arg(fileName));
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return candidates.front();
}

QIcon assetIcon(const QString &fileName)
{
    const UiTheme theme = currentUiTheme();
    const QString key = QStringLiteral("%1|%2|0").arg(themeSettingsValue(theme), fileName);
    static QHash<QString, QIcon> cache;
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) {
        return cached.value();
    }
    QPixmap pixmap(assetPath(fileName));
    if (pixmap.isNull()) {
        return {};
    }
    const QIcon icon(tintedPixmap(pixmap, iconColorForTheme(theme)));
    cache.insert(key, icon);
    return icon;
}

QIcon mirroredAssetIcon(const QString &fileName)
{
    const UiTheme theme = currentUiTheme();
    const QString key = QStringLiteral("%1|%2|1").arg(themeSettingsValue(theme), fileName);
    static QHash<QString, QIcon> cache;
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) {
        return cached.value();
    }
    QPixmap pixmap(assetPath(fileName));
    if (pixmap.isNull()) {
        return {};
    }
    pixmap = pixmap.transformed(QTransform().scale(-1.0, 1.0));
    const QIcon icon(tintedPixmap(pixmap, iconColorForTheme(theme)));
    cache.insert(key, icon);
    return icon;
}

} // namespace gui
