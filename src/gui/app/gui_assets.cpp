#include "gui_assets.h"

#include "theme_manager.h"

#include <QtCore>
#include <QtGui>

namespace gui {
namespace {

QPixmap tintedPixmap(const QPixmap &source, const QColor &color) {
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

QIcon themedAssetIcon(const QString &fileName, bool mirrored) {
    const UiTheme theme = currentUiTheme();
    const QString key = QStringLiteral("%1|%2|%3")
                            .arg(themeSettingsValue(theme), fileName)
                            .arg(mirrored);
    static QHash<QString, QIcon> cache;
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) {
        return cached.value();
    }

    QPixmap pixmap(assetPath(fileName));
    if (pixmap.isNull()) {
        return {};
    }
    if (mirrored) {
        pixmap = pixmap.transformed(QTransform().scale(-1.0, 1.0));
    }
    const QIcon icon(tintedPixmap(pixmap, iconColorForTheme(theme)));
    cache.insert(key, icon);

    return icon;
}

} // namespace

QString assetPath(const QString &fileName) {
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

QIcon assetIcon(const QString &fileName) {
    return themedAssetIcon(fileName, false);
}

QIcon mirroredAssetIcon(const QString &fileName) {
    return themedAssetIcon(fileName, true);
}

} // namespace gui
