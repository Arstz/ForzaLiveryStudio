#include "system_integration.h"

#include "gui_assets.h"
#include "image_io.h"
#include "theme_manager.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <array>
#include <optional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace gui {
namespace {

constexpr auto kIconSetSettingsKey = "system/iconSet";
constexpr auto kProjectProgId = "ForzaLiveryStudio.Project";
constexpr auto kApplicationFileName = "ForzaLiveryStudio.exe";
constexpr int kSmallIconMaximumExtent = 32;
constexpr std::array<int, 10> kIconExtents = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};

QString selectedApplicationIconPath;

enum class SystemIconSet {
    Light,
    Dark,
};

struct IconFrame {
    QByteArray data;
    int extent = 0;
};

QString iconSetSettingsValue(SystemIconSet iconSet) {
    return iconSet == SystemIconSet::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

QString systemAssetName(const QString &baseName, SystemIconSet iconSet) {
    return iconSet == SystemIconSet::Dark
        ? QStringLiteral("system/%1 dark.png").arg(baseName)
        : QStringLiteral("system/%1.png").arg(baseName);
}

QImage loadSystemImage(const QString &assetName) {
    const QString path = assetPath(assetName);
    QString error;
    const QImage image = readGuideImage(path, nullptr, &error);

    if (image.isNull()) {
        qWarning().noquote() << QStringLiteral("Could not load system icon %1: %2").arg(path, error);
    }

    return image;
}

void setApplicationIcon(QApplication &app, SystemIconSet iconSet) {
    const QImage image = loadSystemImage(systemAssetName(QStringLiteral("Logo"), iconSet));
    const QIcon icon(QPixmap::fromImage(image));

    if (icon.isNull()) {
        return;
    }
    app.setWindowIcon(icon);
}

QByteArray iconFrameData(const QImage &source, int extent) {
    QImage canvas(QSize(extent, extent), QImage::Format_ARGB32_Premultiplied);
    const QImage scaled = source.scaled(
        QSize(extent, extent), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QPoint origin((extent - scaled.width()) / 2, (extent - scaled.height()) / 2);
    const int maskStride = ((extent + 31) / 32) * 4;
    QByteArray data;

    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.drawImage(origin, scaled);
    painter.end();

    const QImage image = canvas.convertToFormat(QImage::Format_ARGB32);
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(40) << qint32(extent) << qint32(extent * 2);
    stream << quint16(1) << quint16(32) << quint32(0);
    stream << quint32(extent * extent * 4) << qint32(0) << qint32(0);
    stream << quint32(0) << quint32(0);

    for (int y = extent - 1; y >= 0; --y) {
        for (int x = 0; x < extent; ++x) {
            const QRgb pixel = image.pixel(x, y);
            stream << quint8(qBlue(pixel)) << quint8(qGreen(pixel));
            stream << quint8(qRed(pixel)) << quint8(qAlpha(pixel));
        }
    }
    for (int y = extent - 1; y >= 0; --y) {
        QByteArray mask(maskStride, '\0');
        for (int x = 0; x < extent; ++x) {
            if (qAlpha(image.pixel(x, y)) == 0) {
                const int byteIndex = x / 8;
                const int bitIndex = 7 - (x % 8);
                mask[byteIndex] = static_cast<char>(
                    static_cast<unsigned char>(mask[byteIndex]) | (1U << bitIndex));
            }
        }
        if (stream.writeRawData(mask.constData(), mask.size()) != mask.size()) {
            return {};
        }
    }

    return stream.status() == QDataStream::Ok ? data : QByteArray();
}

bool writeWindowsIcon(const QString &path, const QImage &smallSource, const QImage &largeSource) {
    QList<IconFrame> frames;

    if (smallSource.isNull() || largeSource.isNull()) {
        return false;
    }
    frames.reserve(static_cast<qsizetype>(kIconExtents.size()));
    for (const int extent : kIconExtents) {
        const QImage &source = extent <= kSmallIconMaximumExtent ? smallSource : largeSource;
        const QByteArray data = iconFrameData(source, extent);
        if (data.isEmpty()) {
            return false;
        }
        frames.push_back({data, extent});
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(frames.size());

    quint32 imageOffset = 6 + static_cast<quint32>(frames.size()) * 16;
    for (const IconFrame &frame : frames) {
        const quint8 encodedExtent = frame.extent == 256 ? 0 : static_cast<quint8>(frame.extent);
        stream << encodedExtent << encodedExtent << quint8(0) << quint8(0);
        stream << quint16(1) << quint16(32);
        stream << quint32(frame.data.size()) << imageOffset;
        imageOffset += static_cast<quint32>(frame.data.size());
    }
    for (const IconFrame &frame : frames) {
        if (stream.writeRawData(frame.data.constData(), frame.data.size()) != frame.data.size()) {
            return false;
        }
    }

    return stream.status() == QDataStream::Ok && file.commit();
}

QString createCachedIcon(const QString &fileName,
                         const QString &smallAsset,
                         const QString &largeAsset) {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/system-icons");
    const QString path = QDir(directory).filePath(fileName);
    const QImage smallSource = loadSystemImage(smallAsset);
    const QImage largeSource = smallAsset == largeAsset ? smallSource : loadSystemImage(largeAsset);

    if (!QDir().mkpath(directory)
        || !writeWindowsIcon(path, smallSource, largeSource)) {
        qWarning().noquote() << QStringLiteral("Could not create Windows icon: %1").arg(path);
        return {};
    }

    return QDir::toNativeSeparators(path);
}

QString applicationIconPath(SystemIconSet iconSet) {
    const QString value = iconSetSettingsValue(iconSet);
    const QString logoAsset = systemAssetName(QStringLiteral("Logo"), iconSet);

    return createCachedIcon(QStringLiteral("application-%1.ico").arg(value), logoAsset, logoAsset);
}

QString projectIconPath(SystemIconSet iconSet) {
    const QString value = iconSetSettingsValue(iconSet);

    return createCachedIcon(
        QStringLiteral("project-%1.ico").arg(value),
        systemAssetName(QStringLiteral("File small"), iconSet),
        systemAssetName(QStringLiteral("File big"), iconSet));
}

std::optional<SystemIconSet> storedIconSet() {
    const QSettings settings;
    if (!settings.contains(QLatin1String(kIconSetSettingsKey))) {
        return std::nullopt;
    }

    const QString value = settings.value(QLatin1String(kIconSetSettingsKey)).toString();
    if (value.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0) {
        return SystemIconSet::Light;
    }
    if (value.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0) {
        return SystemIconSet::Dark;
    }

    return std::nullopt;
}

std::optional<SystemIconSet> chooseIconSet() {
    const QStringList choices = {
        QCoreApplication::translate("SystemIntegration", "Light"),
        QCoreApplication::translate("SystemIntegration", "Dark"),
    };
    const int current = isDarkTheme(currentUiTheme()) ? 1 : 0;
    bool accepted = false;
    const QString choice = QInputDialog::getItem(
        nullptr,
        QCoreApplication::translate("SystemIntegration", "Choose system icons"),
        QCoreApplication::translate(
            "SystemIntegration",
            "Choose the icon set for Forza Livery Studio and .3so project files:"),
        choices,
        current,
        false,
        &accepted);

    if (!accepted) {
        return std::nullopt;
    }

    return choice == choices.front() ? SystemIconSet::Light : SystemIconSet::Dark;
}

#ifdef Q_OS_WIN
bool registerFileAssociation(const QString &applicationIcon, const QString &projectIcon) {
    const QString executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString command = QStringLiteral("\"") + executable + QStringLiteral("\" \"%1\"");
    const QString applicationIconValue = QStringLiteral("\"%1\",0").arg(applicationIcon);
    const QString projectIconValue = QStringLiteral("\"%1\",0").arg(projectIcon);
    QSettings extension(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.3so"),
        QSettings::NativeFormat);
    QSettings project(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1").arg(QLatin1String(kProjectProgId)),
        QSettings::NativeFormat);
    QSettings application(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Applications\\%1")
            .arg(QLatin1String(kApplicationFileName)),
        QSettings::NativeFormat);

    extension.setValue(QStringLiteral("."), QLatin1String(kProjectProgId));
    extension.setValue(QStringLiteral("OpenWithProgids/%1").arg(QLatin1String(kProjectProgId)), QString());
    project.setValue(QStringLiteral("."), QStringLiteral("Forza Livery Studio Project"));
    project.setValue(QStringLiteral("DefaultIcon/."), projectIconValue);
    project.setValue(QStringLiteral("shell/open/command/."), command);
    application.setValue(QStringLiteral("FriendlyAppName"), QStringLiteral("Forza Livery Studio"));
    application.setValue(QStringLiteral("DefaultIcon/."), applicationIconValue);
    application.setValue(QStringLiteral("shell/open/command/."), command);
    application.setValue(QStringLiteral("SupportedTypes/.3so"), QString());

    extension.sync();
    project.sync();
    application.sync();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    return extension.status() == QSettings::NoError
        && project.status() == QSettings::NoError
        && application.status() == QSettings::NoError;
}
#endif

} // namespace

void configureSystemIntegration(QApplication &app) {
    std::optional<SystemIconSet> iconSet = storedIconSet();

#ifdef Q_OS_WIN
    if (!iconSet.has_value()) {
        iconSet = chooseIconSet();
        if (!iconSet.has_value()) {
            setApplicationIcon(
                app, isDarkTheme(currentUiTheme()) ? SystemIconSet::Dark : SystemIconSet::Light);
            return;
        }
        QSettings().setValue(
            QLatin1String(kIconSetSettingsKey), iconSetSettingsValue(iconSet.value()));
    }

    setApplicationIcon(app, iconSet.value());
    const QString applicationIcon = applicationIconPath(iconSet.value());
    const QString projectIcon = projectIconPath(iconSet.value());
    selectedApplicationIconPath = applicationIcon;
    if (!applicationIcon.isEmpty() && !projectIcon.isEmpty()
        && !registerFileAssociation(applicationIcon, projectIcon)) {
        qWarning() << "Could not register the .3so file association";
    }
#else
    if (!iconSet.has_value()) {
        iconSet = isDarkTheme(currentUiTheme()) ? SystemIconSet::Dark : SystemIconSet::Light;
    }
    setApplicationIcon(app, iconSet.value());
#endif
}

void applySystemWindowIcon(QWidget &window) {
    window.setWindowIcon(QApplication::windowIcon());

#ifdef Q_OS_WIN
    if (selectedApplicationIconPath.isEmpty()) {
        return;
    }

    const std::wstring path = selectedApplicationIconPath.toStdWString();
    const HICON largeIcon = static_cast<HICON>(LoadImageW(
        nullptr,
        path.c_str(),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_LOADFROMFILE));
    const HICON smallIcon = static_cast<HICON>(LoadImageW(
        nullptr,
        path.c_str(),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_LOADFROMFILE));
    const HWND handle = reinterpret_cast<HWND>(window.winId());

    if (largeIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    }
    if (smallIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }
#endif
}

} // namespace gui
