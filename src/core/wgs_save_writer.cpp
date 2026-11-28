#include "wgs_save_writer.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "header_codec.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

namespace fls {
namespace {

constexpr quint32 kIndexVersion = 14;
constexpr quint32 kDescriptorVersion = 4;
constexpr quint32 kCreatedContainerState = 5;
constexpr quint32 kFullyUploaded = 1;
constexpr quint32 kUnresolvedConflicts = 16;
constexpr quint8 kInitialGeneration = 1;
constexpr int kCurrentGuidOffset = 144;
constexpr int kGuidSize = 16;
constexpr int kLegacyAssetIdSize = 8;
constexpr quint32 kFirstFullGuidHeaderVersion = 2;
constexpr int kFh3LiveryHeaderTailPrefixSize = 2;
constexpr int kFh4LiveryHeaderTailSize = 18;
constexpr int kFh5PaintOnlyHeaderTailSize = 16;
constexpr qint64 kMaximumFileSize = 64 * 1024 * 1024;
constexpr quint32 kMaximumEntries = 1000000;
constexpr quint64 kWindowsEpochOffset = 116444736000000000;
constexpr quint64 kFileTimeTicksPerMillisecond = 10000;
constexpr int kIndexEntryMetadataSize = 45;
constexpr int kLiveryCreatorTagOffset = 33;
constexpr int kLiveryInfoVersionOffset = 29;
constexpr quint32 kIndexCountOffset = 4;
constexpr quint32 kFileTimeSize = 8;
constexpr char kBackupFolder[] = ".fls-wgs-backups";
constexpr char kLockName[] = ".fls-wgs-export.lock";
constexpr char kIndexName[] = "containers.index";
constexpr char kLiveryName[] = "C_livery";
constexpr char kGroupName[] = "C_group";
constexpr char kHeaderName[] = "header";
constexpr char kBigThumbnailName[] = "bigThumb.png";
constexpr char kSmallThumbnailName[] = "smallThumb.png";
constexpr char kGroupThumbnailName[] = "thumb.png";
constexpr const char *kLiveryBlobNames[] = {kLiveryName, kBigThumbnailName, kHeaderName, kSmallThumbnailName};
constexpr const char *kGroupBlobNames[] = {kGroupName, kHeaderName, kGroupThumbnailName};
constexpr QSize kGroupThumbnailSize(256, 256);
constexpr QSize kLegacyLiveryThumbnailSize(816, 459);
constexpr QSize kLegacyHorizonSmallThumbnailSize(224, 126);
constexpr QSize kFh5LiveryThumbnailSize(670, 376);
constexpr QSize kFh5SmallLiveryThumbnailSize(328, 328);

struct WgsTitle {
    ExportGameVersion game;
    const char *name;
    const char *id;
};

constexpr WgsTitle kWgsTitles[] = {
    {ExportGameVersion::Motorsport5, "Forza Motorsport 5", "A72E0E3DCE314AE089B8741C625AB197"},
    {ExportGameVersion::Horizon2, "Forza Horizon 2", "788F0100A4BC46C3B19BD1001A96545B"},
    {ExportGameVersion::Motorsport6, "Forza Motorsport 6", "417D0100B23041CF975D3EAA64F9397E"},
    {ExportGameVersion::Horizon3, "Forza Horizon 3", "1902010095754C2B99163D664CE1DFAB"},
    {ExportGameVersion::Motorsport7, "Forza Motorsport 7", "000000000000000000000000772F15B8"},
    {ExportGameVersion::Horizon4, "Forza Horizon 4", "000000000000000000000000765B6743"},
    {ExportGameVersion::Horizon5, "Forza Horizon 5", "0000000000000000000000007900C3C7"},
};

struct WgsProfile {
    ExportGameVersion game;
    const char *gameName;
    QSize liveryThumbnailSize;
    QSize smallLiveryThumbnailSize;
};

constexpr WgsProfile kWgsProfiles[] = {
    {ExportGameVersion::Horizon3, "Forza Horizon 3",
     kLegacyLiveryThumbnailSize, kLegacyHorizonSmallThumbnailSize},
    {ExportGameVersion::Horizon4, "Forza Horizon 4",
     kLegacyLiveryThumbnailSize, kLegacyHorizonSmallThumbnailSize},
    {ExportGameVersion::Motorsport7, "Forza Motorsport 7",
     kLegacyLiveryThumbnailSize, QSize(192, 108)},
    {ExportGameVersion::Horizon5, "Forza Horizon 5",
     kFh5LiveryThumbnailSize, kFh5SmallLiveryThumbnailSize},
    {ExportGameVersion::Motorsport2023, "Forza Motorsport 2023",
     kFh5LiveryThumbnailSize, kFh5SmallLiveryThumbnailSize},
};

struct AppendSnapshot {
    WgsExportTarget target;
    QMap<QString, int> entryOffsets;
    QByteArray index;
    QDateTime indexModifiedAt;
    int indexTimeOffset = 0;
    quint32 count = 0;
};

void require(bool condition, const QString &message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

const WgsProfile *wgsProfile(ExportGameVersion game) {
    const auto profile = std::ranges::find(kWgsProfiles, game, &WgsProfile::game);

    return profile == std::end(kWgsProfiles) ? nullptr : profile;
}

void validateProfile(const WgsProfile &expected, ExportGameVersion encodingGame, const QString &folderName) {
    const QString titleId = folderName.section(QLatin1Char('_'), 1);
    const auto title = std::ranges::find_if(kWgsTitles, [&titleId](const WgsTitle &entry) {
        return titleId.compare(QLatin1String(entry.id), Qt::CaseInsensitive) == 0;
    });
    const bool knownTitle = title != std::end(kWgsTitles);

    if (knownTitle && title->game == encodingGame) {
        return;
    }
    const QString selectedName = knownTitle ? QString::fromLatin1(title->name)
                                            : QStringLiteral("an unrecognized game");
    throw std::runtime_error(QStringLiteral(
        "Selected a container for %1, please select container for %2 or change export game version")
        .arg(selectedName, QLatin1String(expected.gameName)).toStdString());
}

int assetIdSize(const NativeExportVersions &versions) {
    return versions.header < kFirstFullGuidHeaderVersion ? kLegacyAssetIdSize : kGuidSize;
}

QString blobKey(const char *name) {
    return QString::fromLatin1(name).toLower();
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Could not read %1").arg(path));
    require(file.size() >= 0 && file.size() <= kMaximumFileSize,
            QStringLiteral("WGS file exceeds the supported size: %1").arg(path));
    const QByteArray bytes = file.readAll();
    require(bytes.size() == file.size(), QStringLiteral("Incomplete read of %1").arg(path));

    return bytes;
}

QString readString(const QByteArray &bytes, int &offset) {
    const quint32 count = detail::readLeU32(bytes, offset);
    offset += 4;
    require(count <= static_cast<quint32>((bytes.size() - offset) / 2),
            QStringLiteral("WGS index string is truncated"));
    QString value;
    value.reserve(count);
    for (quint32 index = 0; index < count; ++index) {
        value.append(QChar(detail::readLeU16(bytes, offset)));
        offset += 2;
    }

    return value;
}

void appendString(QByteArray &bytes, const QString &value) {
    detail::appendLeU32(bytes, static_cast<quint32>(value.size()));
    for (QChar character : value) {
        detail::appendLeU16(bytes, character.unicode());
    }
}

void skip(const QByteArray &bytes, int &offset, int count) {
    require(offset >= 0 && count <= bytes.size() - offset,
            QStringLiteral("WGS index is truncated"));
    offset += count;
}

QByteArray guidBytes(const QUuid &guid) {
    QByteArray bytes;
    detail::appendLeU32(bytes, guid.data1);
    detail::appendLeU16(bytes, guid.data2);
    detail::appendLeU16(bytes, guid.data3);
    bytes.append(reinterpret_cast<const char *>(guid.data4), 8);

    return bytes;
}

QString checkedFile(const QString &directory, const QString &name) {
    const QDir parent(directory);
    const QFileInfo info(parent.filePath(name));
    require(info.isFile() && !info.isSymLink()
                && info.canonicalFilePath() == QDir(parent.canonicalPath()).filePath(name),
            QStringLiteral("WGS file is missing or resolves outside its container: %1").arg(info.filePath()));

    return info.absoluteFilePath();
}

quint32 readIndexHeader(AppendSnapshot &snapshot, int &offset) {
    const QByteArray &bytes = snapshot.index;

    require(detail::readLeU32(bytes, 0) == kIndexVersion,
            QStringLiteral("Unsupported WGS index version"));
    const quint32 count = detail::readLeU32(bytes, kIndexCountOffset);
    require(count <= kMaximumEntries, QStringLiteral("Invalid WGS index container count"));
    offset = 12;
    readString(bytes, offset);
    snapshot.indexTimeOffset = offset;
    skip(bytes, offset, 12);
    readString(bytes, offset);
    skip(bytes, offset, 8);

    return count;
}

QByteArray inflatedPayload(const QByteArray &wrapped, bool livery) {
    require(wrapped.size() >= 8 && detail::readLeU32(wrapped, 0) == wrapped.size() - 8
                && detail::readLeU32(wrapped, 4) > 0
                && detail::readLeU32(wrapped, 4) <= kMaximumFileSize,
            QStringLiteral("Invalid WGS payload wrapper"));
    const QByteArray raw = inflateContainer(wrapped);
    require(raw.startsWith(livery ? QByteArray("vlrc", 4) : QByteArray("gyvl", 4))
                && detail::readLeU32(raw, 4) == 1, QStringLiteral("The payload does not match the selected asset type"));

    return raw;
}

QByteArray profileCreatorTag(const QDir &profile) {
    const QString ownerText = profile.dirName().section(QLatin1Char('_'), 0, 0);
    QByteArray tag;
    bool validOwner = false;
    const quint64 owner = ownerText.toULongLong(&validOwner, 16);

    require(validOwner && owner != 0 && ownerText.compare(
                QStringLiteral("%1").arg(owner, 16, 16, QLatin1Char('0')), Qt::CaseInsensitive) == 0,
            QStringLiteral("The WGS profile folder must retain its account identifier"));
    detail::appendLeU32(tag, static_cast<quint32>(owner));
    detail::appendLeU32(tag, static_cast<quint32>(owner >> 32));

    return tag;
}

AppendSnapshot readAppendSnapshot(const QString &indexPath, ExportGameVersion game,
                                  WgsAssetKind kind, int carId) {
    const QFileInfo indexInfo(indexPath);
    const QDir profile(indexInfo.absolutePath());
    const WgsProfile *format = wgsProfile(game);
    AppendSnapshot append;
    int offset = 0;

    require(format != nullptr, QStringLiteral("WGS export is unavailable for the selected game."));
    require(kind != WgsAssetKind::Livery || carId > 0, QStringLiteral("Select a valid car for the WGS livery."));
    require(QDir::isAbsolutePath(indexPath) && indexInfo.fileName() == QLatin1String(kIndexName),
            QStringLiteral("Select containers.index from the destination WGS profile."));
    append.target.indexPath = checkedFile(profile.absolutePath(), QLatin1String(kIndexName));
    append.target.liveryThumbnailSize = format->liveryThumbnailSize;
    append.target.smallLiveryThumbnailSize = format->smallLiveryThumbnailSize;
    append.target.game = game;
    append.target.versions = nativeExportVersions(append.target.encodingGame());
    append.target.kind = kind;
    append.target.carId = kind == WgsAssetKind::Livery ? carId : 0;
    append.index = readFile(append.target.indexPath);
    append.indexModifiedAt = QFileInfo(append.target.indexPath).lastModified();
    append.target.fingerprint = QCryptographicHash::hash(append.index, QCryptographicHash::Sha256);
    append.count = readIndexHeader(append, offset);
    validateProfile(*format, append.target.encodingGame(), profile.dirName());
    append.target.creatorTag = profileCreatorTag(profile);
    const quint32 flags = detail::readLeU32(append.index, append.indexTimeOffset + kFileTimeSize);
    require((flags & kUnresolvedConflicts) == 0,
            QStringLiteral("Resolve the WGS profile's cloud conflict before exporting."));
    require(append.count < kMaximumEntries, QStringLiteral("WGS index is full."));
    for (quint32 index = 0; index < append.count; ++index) {
        const int start = offset;
        const QString name = readString(append.index, offset);
        readString(append.index, offset);
        readString(append.index, offset);
        skip(append.index, offset, kIndexEntryMetadataSize);
        append.entryOffsets.insert(name.toCaseFolded(), start);
    }
    require(offset == append.index.size(), QStringLiteral("Unsupported trailing WGS index data"));

    return append;
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
            QStringLiteral("Could not atomically write %1").arg(path));
}

void writeU64(QByteArray &bytes, int offset, quint64 value) {
    require(offset >= 0 && offset + 8 <= bytes.size(), QStringLiteral("Invalid WGS metadata offset"));
    for (int byte = 0; byte < 8; ++byte) {
        bytes[offset + byte] = static_cast<char>((value >> (byte * 8)) & 0xff);
    }
}

void writeU32(QByteArray &bytes, int offset, quint32 value) {
    QByteArray encoded;
    detail::appendLeU32(encoded, value);
    bytes.replace(offset, encoded.size(), encoded);
}

quint64 readU64(const QByteArray &bytes, int offset) {
    const quint64 low = detail::readLeU32(bytes, offset);
    const quint64 high = detail::readLeU32(bytes, offset + 4);

    return low | (high << 32);
}

quint64 fileTime(const QDateTime &time) {
    return kWindowsEpochOffset
        + static_cast<quint64>(time.toMSecsSinceEpoch()) * kFileTimeTicksPerMillisecond;
}

void appendU64(QByteArray &bytes, quint64 value) {
    detail::appendLeU32(bytes, static_cast<quint32>(value));
    detail::appendLeU32(bytes, static_cast<quint32>(value >> 32));
}

HeaderMetadata validateAppendBlobs(const WgsExportTarget &target, const NativeExportVersions &versions,
                                   const QMap<QString, QByteArray> &blobs, std::span<const char *const> names) {
    const bool livery = target.kind == WgsAssetKind::Livery;
    const QString payloadKey = blobKey(livery ? kLiveryName : kGroupName);
    const QByteArray pngSignature = QByteArray::fromHex("89504e470d0a1a0a");

    require(blobs.size() == static_cast<qsizetype>(names.size()),
            QStringLiteral("WGS append requires artwork, a header, and the registered PNG thumbnails."));
    for (const char *name : names) {
        const QString key = blobKey(name);
        const auto blob = blobs.constFind(key);
        require(blob != blobs.cend() && !blob->isEmpty() && blob->size() <= kMaximumFileSize,
                QStringLiteral("Missing or invalid WGS blob: %1").arg(QLatin1String(name)));
        require(!key.endsWith(QStringLiteral(".png")) || blob->startsWith(pngSignature),
                QStringLiteral("WGS thumbnails must be PNG blobs."));
    }
    const QByteArray raw = inflatedPayload(blobs.value(payloadKey), livery);
    require(!livery || (detail::readLeU32(raw, kLiveryInfoVersionOffset) == versions.livery
                && detail::readLeU32(raw, 8) == 0
                && detail::readLeU32(raw, 16) == static_cast<quint32>(target.carId)
                && raw.mid(kLiveryCreatorTagOffset, target.creatorTag.size()) == target.creatorTag),
            QStringLiteral("WGS artwork must be an editable livery for the selected game, car, and profile."));
    const HeaderMetadata header = parseHeader(blobs.value(QLatin1String(kHeaderName)));
    require(header.formatVersion == versions.header && header.hasCarId == livery
                && (!livery || header.carId == static_cast<quint32>(target.carId))
                && header.guid == target.assetGuid && header.creatorTag == target.creatorTag,
            QStringLiteral("WGS header must match the new asset and profile."));

    return header;
}

QByteArray encodeWgsThumbnail(const QImage &thumbnail, const QSize &size) {
    const QImage scaled = thumbnail.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QPoint offset((scaled.width() - size.width()) / 2, (scaled.height() - size.height()) / 2);
    const QImage cropped = scaled.copy(QRect(offset, size)).convertToFormat(QImage::Format_RGBA8888);
    QByteArray png;
    QBuffer buffer(&png);

    require(!thumbnail.isNull() && size.isValid() && buffer.open(QIODevice::WriteOnly)
                && cropped.size() == size && cropped.save(&buffer, "PNG"),
            QStringLiteral("Could not encode the WGS thumbnail."));

    return png;
}

void setHeaderCreationTime(HeaderMetadata &header, const QDateTime &createdAt) {
    const QDate date = createdAt.date();
    const QTime time = createdAt.time();
    QByteArray fields;

    header.year = static_cast<quint16>(date.year());
    header.month = static_cast<quint8>(date.month());
    header.day = 0;
    detail::appendLeU16(fields, static_cast<quint16>(date.dayOfWeek() % 7));
    detail::appendLeU16(fields, static_cast<quint16>(date.day()));
    detail::appendLeU16(fields, static_cast<quint16>(time.hour()));
    detail::appendLeU16(fields, static_cast<quint16>(time.minute()));
    detail::appendLeU16(fields, static_cast<quint16>(time.second()));
    detail::appendLeU16(fields, static_cast<quint16>(time.msec()));
    header.fieldBlock.replace(0, fields.size(), fields);
}

void prepareHorizonHeader(HeaderMetadata &header, const WgsExportTarget &target, const QDateTime &createdAt,
                          QMap<QString, QByteArray> &blobs) {
    const bool livery = target.kind == WgsAssetKind::Livery;

    switch (target.game) {
    case ExportGameVersion::Horizon3:
        if (livery) {
            header.trailing = QByteArray(kFh3LiveryHeaderTailPrefixSize, '\0');
            detail::appendLeU32(header.trailing, header.carId);
            header.trailing.append(QByteArray(kLegacyAssetIdSize, '\xff'));
        }
        break;
    case ExportGameVersion::Horizon4:
        if (livery) {
            header.trailing = QByteArray(kFh4LiveryHeaderTailSize, '\0');
        }
        break;
    case ExportGameVersion::Horizon5:
        if (livery && header.typeValue == 0) {
            header.trailing = QByteArray(kFh5PaintOnlyHeaderTailSize, '\0');
        }
        break;
    default:
        return;
    }
    setHeaderCreationTime(header, createdAt);
    blobs.insert(QLatin1String(kHeaderName), buildHeader(header));
}

QString newContainerName(const AppendSnapshot &append, QDateTime &createdAt) {
    const WgsExportTarget &target = append.target;
    const QString prefix = target.kind == WgsAssetKind::Livery
        ? QStringLiteral("Livery_%1_").arg(target.carId, 4, 10, QLatin1Char('0'))
        : QStringLiteral("LayerGroup_0000_");
    QString name = prefix + createdAt.toString(QStringLiteral("yyyyMMddHHmmss"));

    while (append.entryOffsets.contains(name.toCaseFolded())) {
        createdAt = createdAt.addSecs(1);
        name = prefix + createdAt.toString(QStringLiteral("yyyyMMddHHmmss"));
    }

    return name;
}

QByteArray indexWithNewContainer(const AppendSnapshot &append, const QString &name, const QUuid &folderId,
                                const QDateTime &now, quint64 blobSize) {
    QByteArray index = append.index;
    QByteArray entry;
    const auto next = append.entryOffsets.lowerBound(name.toCaseFolded());
    const qsizetype position = next == append.entryOffsets.cend() ? index.size() : next.value();
    const int timeOffset = append.indexTimeOffset;
    const int flagsOffset = timeOffset + kFileTimeSize;
    const quint64 previousTime = readU64(index, timeOffset);
    const quint32 flags = detail::readLeU32(index, flagsOffset);

    require(previousTime < std::numeric_limits<quint64>::max(), QStringLiteral("Invalid WGS index timestamp."));
    const quint64 timestamp = std::max(fileTime(now), previousTime + 1);
    appendString(entry, name);
    appendString(entry, name);
    appendString(entry, QString());
    entry.append(static_cast<char>(kInitialGeneration));
    detail::appendLeU32(entry, kCreatedContainerState);
    entry.append(guidBytes(folderId));
    appendU64(entry, timestamp);
    appendU64(entry, 0);
    appendU64(entry, blobSize);
    index.insert(position, entry);

    writeU32(index, kIndexCountOffset, append.count + 1);
    writeU64(index, timeOffset, timestamp);
    writeU32(index, flagsOffset, flags & ~kFullyUploaded);

    return index;
}

quint64 stageContainer(const QDir &directory, const QMap<QString, QByteArray> &blobs,
                       std::span<const char *const> names) {
    QByteArray descriptor;
    quint64 size = 0;

    detail::appendLeU32(descriptor, kDescriptorVersion);
    detail::appendLeU32(descriptor, static_cast<quint32>(names.size()));
    for (const char *name : names) {
        const QString blobName = QString::fromLatin1(name);
        const QUuid localId = QUuid::createUuid();
        const QByteArray &blob = blobs[blobKey(name)];
        for (QChar character : blobName) {
            detail::appendLeU16(descriptor, character.unicode());
        }
        descriptor.append(QByteArray(kCurrentGuidOffset - blobName.size() * 2, '\0'));
        descriptor.append(guidBytes(localId));
        writeFile(directory.filePath(localId.toString(QUuid::Id128).toUpper()), blob);
        size += static_cast<quint64>(blob.size());
    }
    writeFile(directory.filePath(QStringLiteral("container.%1").arg(kInitialGeneration)), descriptor);

    return size;
}

#ifdef Q_OS_WIN
void inheritContainerPermissions(const QDir &directory, qsizetype expectedFileCount) {
    const auto files = directory.entryList(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    require(files.size() == expectedFileCount, QStringLiteral("Could not list the published WGS container files."));
    // An empty explicit ACL lets Windows inherit the rules from the WGS directory.
    ACL acl;
    require(InitializeAcl(&acl, sizeof(acl), ACL_REVISION),
            QStringLiteral("Could not initialize WGS permissions."));
    const auto inherit = [&acl](const QString &path) {
        auto nativePath = QDir::toNativeSeparators(path).toStdWString();
        const DWORD result = SetNamedSecurityInfoW(nativePath.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &acl, nullptr);
        require(result == ERROR_SUCCESS,
                QStringLiteral("Could not inherit WGS permissions for %1 (Windows error %2).")
                    .arg(path).arg(result));
    };
    inherit(directory.absolutePath());
    for (const QString &file : files) {
        inherit(directory.filePath(file));
    }
}
#endif

QString createBackup(const AppendSnapshot &snapshot) {
    const QDir profile(QFileInfo(snapshot.target.indexPath).absolutePath());
    const QString backupRoot = profile.filePath(QLatin1String(kBackupFolder));
    require(QDir().mkpath(backupRoot) && QDir(backupRoot).canonicalPath() == profile.filePath(QLatin1String(kBackupFolder)),
            QStringLiteral("Could not create the WGS backup folder within the profile"));
    const QString name = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))
        + QLatin1Char('-') + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QDir backup(QDir(backupRoot).filePath(name));
    const QString indexPath = backup.filePath(QLatin1String(kIndexName));
    require(QDir().mkpath(backup.absolutePath()), QStringLiteral("Could not create the WGS backup directory"));
    writeFile(indexPath, snapshot.index);
    QFile copy(indexPath);
    require(copy.open(QIODevice::ReadWrite)
                && copy.setFileTime(snapshot.indexModifiedAt, QFileDevice::FileModificationTime),
            QStringLiteral("Could not preserve the WGS index backup timestamp"));

    return backup.absolutePath();
}

} // namespace

ExportGameVersion WgsExportTarget::encodingGame() const {
    return game == ExportGameVersion::Motorsport2023 ? ExportGameVersion::Horizon5 : game;
}

QSize WgsExportTarget::thumbnailSize() const {
    return kind == WgsAssetKind::Livery ? liveryThumbnailSize : kGroupThumbnailSize;
}

bool supportsWgsAppend(ExportGameVersion game) {
    return wgsProfile(game) != nullptr;
}

WgsExportTarget inspectWgsAppendTarget(const QString &indexPath, ExportGameVersion game,
                                      WgsAssetKind kind, int carId) {
    WgsExportTarget target = readAppendSnapshot(indexPath, game, kind, carId).target;
    target.assetGuid = guidBytes(QUuid::createUuid()).left(assetIdSize(target.versions));

    return target;
}

WgsAppendResult appendWgsContainer(const WgsExportTarget &target, const QMap<QString, QByteArray> &blobs) {
    QDir profile(QFileInfo(target.indexPath).absolutePath());
    QLockFile lock(profile.filePath(QLatin1String(kLockName)));
    const std::span<const char *const> names = target.kind == WgsAssetKind::Livery
        ? std::span<const char *const>(kLiveryBlobNames) : std::span<const char *const>(kGroupBlobNames);

    require(lock.tryLock(0), QStringLiteral("Another WGS export is using this save profile"));
    const AppendSnapshot append = readAppendSnapshot(target.indexPath, target.game, target.kind, target.carId);
    require(append.target.fingerprint == target.fingerprint && append.target.creatorTag == target.creatorTag,
            QStringLiteral("The WGS profile changed. Reselect containers.index before exporting."));
    const NativeExportVersions &versions = append.target.versions;
    const int identifierSize = assetIdSize(versions);
    require(target.assetGuid.size() == identifierSize && target.assetGuid != QByteArray(identifierSize, '\0'),
            QStringLiteral("Invalid new WGS asset identity."));
    HeaderMetadata header = validateAppendBlobs(target, versions, blobs, names);

    QDateTime createdAt = QDateTime::currentDateTimeUtc();
    const QString name = newContainerName(append, createdAt);
    QMap<QString, QByteArray> stagedBlobs = blobs;
    prepareHorizonHeader(header, target, createdAt, stagedBlobs);
    const QUuid folderId = QUuid::createUuid();
    const QString folderName = folderId.toString(QUuid::Id128).toUpper();
    const QTemporaryDir temporary(profile.filePath(QStringLiteral(".fls-wgs-stage-XXXXXX")));
    require(temporary.isValid() && QDir(temporary.path()).mkdir(folderName),
            QStringLiteral("Could not stage the new WGS container."));
    const QDir staged(QDir(temporary.path()).filePath(folderName));
    const quint64 blobSize = stageContainer(staged, stagedBlobs, names);
    const QByteArray updatedIndex = indexWithNewContainer(append, name, folderId, createdAt, blobSize);
    const QString backup = createBackup(append);
    const QString stagedPath = profile.relativeFilePath(staged.absolutePath());
    require(profile.rename(stagedPath, folderName),
            QStringLiteral("Could not publish the new WGS container."));
    try {
#ifdef Q_OS_WIN
        inheritContainerPermissions(QDir(profile.filePath(folderName)), static_cast<qsizetype>(names.size()) + 1);
#endif
        require(readFile(target.indexPath) == append.index,
                QStringLiteral("The WGS index changed during export."));
        writeFile(target.indexPath, updatedIndex);
    } catch (const std::exception &error) {
        const bool restored = profile.rename(folderName, stagedPath);
        throw std::runtime_error(QStringLiteral("%1\nBackup: %2%3")
            .arg(QString::fromUtf8(error.what()), backup, restored ? QString()
                : QStringLiteral("\nUnregistered container: %1").arg(profile.filePath(folderName)))
            .toStdString());
    }

    return {name, backup};
}

WgsAppendResult exportWgsAsset(const WgsExportTarget &target, Project project, const QImage &thumbnail,
                              const SpriteSizeFn &spriteSize, const ExportLayerChoices &choices) {
    const bool livery = target.kind == WgsAssetKind::Livery;

    require(project.isLivery == livery && (!livery || project.carId == target.carId),
            QStringLiteral("The artwork must match the selected WGS asset type and car."));
    if (!project.headerMetadata) {
        project.headerMetadata = project.sourceHeader.isEmpty()
            ? defaultDraftHeader(project.name, QString(), static_cast<quint32>(project.carId))
            : parseHeader(project.sourceHeader);
    }
    HeaderMetadata &metadata = *project.headerMetadata;
    metadata.formatVersion = target.versions.header;
    metadata.guid = target.assetGuid;
    metadata.creatorTag = target.creatorTag;
    metadata.published = false;
    metadata.description.clear();
    project.sourceHeader.clear();

    const NativeAssetData asset = encodeNativeGameAsset(project, target.encodingGame(), spriteSize, choices);
    QMap<QString, QByteArray> blobs{
        {blobKey(livery ? kLiveryName : kGroupName), asset.artwork},
        {blobKey(kHeaderName), asset.header},
        {blobKey(livery ? kBigThumbnailName : kGroupThumbnailName), encodeWgsThumbnail(thumbnail, target.thumbnailSize())},
    };
    if (livery) {
        blobs.insert(blobKey(kSmallThumbnailName), encodeWgsThumbnail(thumbnail, target.smallLiveryThumbnailSize));
    }

    return appendWgsContainer(target, blobs);
}

} // namespace fls
