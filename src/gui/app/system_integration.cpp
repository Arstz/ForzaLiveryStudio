#include "system_integration.h"

#include "gui_assets.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <array>
#include <cstring>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#endif

namespace gui {
namespace {

constexpr auto kIconSetSettingsKey = "system/iconSet";
constexpr auto kAssociationPromptedSettingsKey = "system/associationPrompted";
constexpr auto kProjectProgId = "ForzaLiveryStudio.Project";
constexpr auto kApplicationFileName = "ForzaLiveryStudio.exe";
constexpr int kApplicationLightIconResource = 101;
constexpr int kApplicationDarkIconResource = 102;
constexpr int kProjectLightIconResource = 201;
constexpr int kProjectDarkIconResource = 202;
constexpr std::array<int, 10> kIconExtents = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};

SystemIconSet selectedIconSet = SystemIconSet::Light;

int applicationIconResource(SystemIconSet iconSet) {
    return iconSet == SystemIconSet::Dark
        ? kApplicationDarkIconResource : kApplicationLightIconResource;
}

int projectIconResource(SystemIconSet iconSet) {
    return iconSet == SystemIconSet::Dark
        ? kProjectDarkIconResource : kProjectLightIconResource;
}

#ifdef Q_OS_WIN
QString executablePath() {
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

QString iconRegistryValue(int resourceId) {
    return QStringLiteral("\"%1\",-%2").arg(executablePath()).arg(resourceId);
}

QString openCommandRegistryValue() {
    return QStringLiteral("\"") + executablePath() + QStringLiteral("\" \"%1\"");
}

bool setRegistryValue(QSettings &settings,
                      const QString &key,
                      const QVariant &value) {
    if (settings.contains(key) && settings.value(key) == value) {
        return false;
    }
    settings.setValue(key, value);

    return true;
}

bool removeRegistryValue(QSettings &settings, const QString &key) {
    if (!settings.contains(key)) {
        return false;
    }
    settings.remove(key);

    return true;
}

QImage imageFromIconResource(int resourceId, int extent) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HICON icon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(resourceId),
        IMAGE_ICON,
        extent,
        extent,
        LR_DEFAULTCOLOR | LR_SHARED));
    if (icon == nullptr) {
        return {};
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = extent;
    bitmapInfo.bmiHeader.biHeight = -extent;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void *pixels = nullptr;
    HDC deviceContext = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(
        deviceContext, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (deviceContext == nullptr || bitmap == nullptr || pixels == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (deviceContext != nullptr) {
            DeleteDC(deviceContext);
        }
        return {};
    }

    HGDIOBJ previous = SelectObject(deviceContext, bitmap);
    std::memset(pixels, 0, static_cast<size_t>(extent * extent * 4));
    const BOOL drawn = DrawIconEx(
        deviceContext, 0, 0, icon, extent, extent, 0, nullptr, DI_NORMAL);
    QImage image;
    if (drawn) {
        image = QImage(
            static_cast<uchar *>(pixels), extent, extent, QImage::Format_ARGB32).copy();
    }
    SelectObject(deviceContext, previous);
    DeleteObject(bitmap);
    DeleteDC(deviceContext);

    return image;
}

QIcon applicationIcon(SystemIconSet iconSet) {
    QIcon icon;
    const int resourceId = applicationIconResource(iconSet);
    for (const int extent : kIconExtents) {
        const QImage image = imageFromIconResource(resourceId, extent);
        if (!image.isNull()) {
            icon.addPixmap(QPixmap::fromImage(image));
        }
    }

    return icon;
}

bool associationOwnedByApplication() {
    QSettings extension(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.3so"),
        QSettings::NativeFormat);
    return extension.value(QStringLiteral(".")).toString()
        == QLatin1String(kProjectProgId);
}

bool applicationUsesPowerSavingGpuPreference() {
    constexpr wchar_t kGpuPreferencesRegistryPath[] =
        L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
    const QString path = executablePath();
    const wchar_t *valueName = reinterpret_cast<const wchar_t *>(path.utf16());

    DWORD valueType = 0;
    DWORD valueSize = 0;
    LONG result = RegGetValueW(
        HKEY_CURRENT_USER,
        kGpuPreferencesRegistryPath,
        valueName,
        RRF_RT_REG_SZ,
        &valueType,
        nullptr,
        &valueSize);
    if (result != ERROR_SUCCESS || valueSize < sizeof(wchar_t)) {
        return false;
    }

    std::vector<wchar_t> value(valueSize / sizeof(wchar_t) + 1, L'\0');
    result = RegGetValueW(
        HKEY_CURRENT_USER,
        kGpuPreferencesRegistryPath,
        valueName,
        RRF_RT_REG_SZ,
        &valueType,
        value.data(),
        &valueSize);
    if (result != ERROR_SUCCESS) {
        return false;
    }

    const QString preference = QString::fromWCharArray(value.data());
    static const QRegularExpression powerSavingPreference(
        QStringLiteral(R"((?:^|;)\s*GpuPreference\s*=\s*1\s*(?:;|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return powerSavingPreference.match(preference).hasMatch();
}
#else
QIcon applicationIcon(SystemIconSet iconSet) {
    const QString name = iconSet == SystemIconSet::Dark
        ? QStringLiteral("system/Logo dark.png") : QStringLiteral("system/Logo.png");
    return QIcon(assetPath(name));
}
#endif

} // namespace

QString systemIconSetSettingsValue(SystemIconSet iconSet) {
    return iconSet == SystemIconSet::Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

SystemIconSet systemIconSetFromSettingsValue(const QString &value) {
    return value.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0
        ? SystemIconSet::Dark : SystemIconSet::Light;
}

SystemIconSet loadSystemIconSet() {
    const QSettings settings;
    return systemIconSetFromSettingsValue(
        settings.value(QLatin1String(kIconSetSettingsKey), QStringLiteral("light")).toString());
}

void saveSystemIconSet(SystemIconSet iconSet) {
    QSettings().setValue(
        QLatin1String(kIconSetSettingsKey), systemIconSetSettingsValue(iconSet));
}

ProjectFileAssociationState projectFileAssociationState(SystemIconSet iconSet) {
#ifdef Q_OS_WIN
    QSettings extension(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.3so"),
        QSettings::NativeFormat);
    if (extension.value(QStringLiteral(".")).toString()
        != QLatin1String(kProjectProgId)) {
        return ProjectFileAssociationState::NotRegistered;
    }

    QSettings project(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1")
            .arg(QLatin1String(kProjectProgId)),
        QSettings::NativeFormat);
    QSettings application(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Applications\\%1")
            .arg(QLatin1String(kApplicationFileName)),
        QSettings::NativeFormat);
    const bool current = project.value(QStringLiteral("DefaultIcon/.")).toString()
            == iconRegistryValue(projectIconResource(iconSet))
        && project.value(QStringLiteral("shell/open/command/.")).toString()
            == openCommandRegistryValue()
        && application.value(QStringLiteral("DefaultIcon/.")).toString()
            == iconRegistryValue(applicationIconResource(iconSet))
        && application.value(QStringLiteral("shell/open/command/.")).toString()
            == openCommandRegistryValue();
    return current ? ProjectFileAssociationState::Registered
                   : ProjectFileAssociationState::NeedsRepair;
#else
    Q_UNUSED(iconSet);
    return ProjectFileAssociationState::NotRegistered;
#endif
}

bool setProjectFileAssociationEnabled(bool enabled,
                                      SystemIconSet iconSet,
                                      QString *error) {
#ifdef Q_OS_WIN
    QSettings extension(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\.3so"),
        QSettings::NativeFormat);
    QSettings project(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\%1")
            .arg(QLatin1String(kProjectProgId)),
        QSettings::NativeFormat);
    QSettings application(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\Applications\\%1")
            .arg(QLatin1String(kApplicationFileName)),
        QSettings::NativeFormat);
    bool changed = false;

    if (enabled) {
        changed |= setRegistryValue(extension, QStringLiteral("."), QLatin1String(kProjectProgId));
        changed |= setRegistryValue(
            extension,
            QStringLiteral("OpenWithProgids/%1").arg(QLatin1String(kProjectProgId)),
            QString());
        changed |= setRegistryValue(
            project, QStringLiteral("."), QStringLiteral("Forza Livery Studio Project"));
        changed |= setRegistryValue(
            project,
            QStringLiteral("DefaultIcon/."),
            iconRegistryValue(projectIconResource(iconSet)));
        changed |= setRegistryValue(
            project, QStringLiteral("shell/open/command/."), openCommandRegistryValue());
        changed |= setRegistryValue(
            application, QStringLiteral("FriendlyAppName"), QStringLiteral("Forza Livery Studio"));
        changed |= setRegistryValue(
            application,
            QStringLiteral("DefaultIcon/."),
            iconRegistryValue(applicationIconResource(iconSet)));
        changed |= setRegistryValue(
            application, QStringLiteral("shell/open/command/."), openCommandRegistryValue());
        changed |= setRegistryValue(
            application, QStringLiteral("SupportedTypes/.3so"), QString());
    } else {
        if (extension.value(QStringLiteral(".")).toString()
            == QLatin1String(kProjectProgId)) {
            changed |= removeRegistryValue(extension, QStringLiteral("."));
        }
        changed |= removeRegistryValue(
            extension,
            QStringLiteral("OpenWithProgids/%1").arg(QLatin1String(kProjectProgId)));
        if (!project.allKeys().isEmpty()) {
            project.clear();
            changed = true;
        }
        if (!application.allKeys().isEmpty()) {
            application.clear();
            changed = true;
        }
    }

    extension.sync();
    project.sync();
    application.sync();
    const bool success = extension.status() == QSettings::NoError
        && project.status() == QSettings::NoError
        && application.status() == QSettings::NoError;
    if (!success) {
        if (error != nullptr) {
            *error = QStringLiteral("Windows could not update the .3so file association.");
        }
        return false;
    }
    if (changed) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    return true;
#else
    Q_UNUSED(enabled);
    Q_UNUSED(iconSet);
    if (error != nullptr) {
        *error = QStringLiteral("File association is available only on Windows.");
    }
    return false;
#endif
}

void configureSystemIntegration(QApplication &app) {
    selectedIconSet = loadSystemIconSet();
    const QIcon icon = applicationIcon(selectedIconSet);
    if (!icon.isNull()) {
        app.setWindowIcon(icon);
    }
}

void applySystemWindowIcon(QWidget &window) {
    window.setWindowIcon(QApplication::windowIcon());

#ifdef Q_OS_WIN
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const int resourceId = applicationIconResource(selectedIconSet);
    HICON largeIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(resourceId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    HICON smallIcon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(resourceId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    const HWND handle = reinterpret_cast<HWND>(window.winId());

    if (largeIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    }
    if (smallIcon != nullptr) {
        SendMessageW(handle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    }
#endif
}

void warnIfPowerSavingGpuPreference(QWidget &parent) {
#ifdef Q_OS_WIN
    if (!applicationUsesPowerSavingGpuPreference()) {
        return;
    }

    QMessageBox::warning(
        &parent,
        QStringLiteral("Integrated Graphics"),
        QStringLiteral(
            "Forza Livery Studio is currently set to use the CPU's integrated graphics. "
            "If it is not switched to the main GPU, the 3D preview renderer might not work correctly."));
#else
    Q_UNUSED(parent);
#endif
}

void offerProjectFileAssociation(QWidget &parent) {
#ifdef Q_OS_WIN
    QSettings settings;
    if (settings.value(QLatin1String(kAssociationPromptedSettingsKey), false).toBool()
        || associationOwnedByApplication()) {
        settings.setValue(QLatin1String(kAssociationPromptedSettingsKey), true);
        return;
    }

    QMessageBox box(&parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("File Association"));
    box.setText(QStringLiteral("Associate .3so project files with Forza Livery Studio?"));
    QPushButton *yes = box.addButton(QStringLiteral("Yes"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(yes);
    box.exec();
    settings.setValue(QLatin1String(kAssociationPromptedSettingsKey), true);
    if (box.clickedButton() != yes) {
        return;
    }

    QString error;
    if (!setProjectFileAssociationEnabled(true, loadSystemIconSet(), &error)) {
        QMessageBox::warning(&parent, QStringLiteral("File Association"), error);
    }
#else
    Q_UNUSED(parent);
#endif
}

} // namespace gui
