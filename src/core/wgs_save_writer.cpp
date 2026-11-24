#include "wgs_save_writer.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "header_codec.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>

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
constexpr qint64 kMaximumFileSize = 64 * 1024 * 1024;
constexpr quint32 kMaximumEntries = 1000000;
constexpr quint64 kWindowsEpochOffset = 116444736000000000;
constexpr quint64 kFileTimeTicksPerMillisecond = 10000;
constexpr int kIndexEntryMetadataSize = 45;
constexpr int kLiveryCreatorTagOffset = 33;
constexpr int kLiveryInfoVersionOffset = 29;
constexpr quint32 kFh5LiveryInfoVersion = 18;
constexpr quint32 kIndexCountOffset = 4;
constexpr quint32 kFileTimeSize = 8;
constexpr char kBackupFolder[] = ".fls-wgs-backups";
constexpr char kLockName[] = ".fls-wgs-export.lock";
constexpr char kIndexName[] = "containers.index";
constexpr char kFh5Scid[] = "0000000000000000000000007900C3C7";
constexpr char kMotorsportPackage[] = "Microsoft.ForzaMotorsport_8wekyb3d8bbwe!ReleaseFinal";
constexpr const char *kLiveryBlobNames[] = {"C_livery", "header", "bigThumb.png", "smallThumb.png"};
constexpr const char *kGroupBlobNames[] = {"C_group", "header", "thumb.png"};

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
    const QString package = readString(bytes, offset);
    if (snapshot.target.game == ExportGameVersion::Motorsport2023) {
        require(package.compare(QLatin1String(kMotorsportPackage), Qt::CaseInsensitive) == 0,
                QStringLiteral("Select the FH5 import profile under Forza Motorsport 2023."));
    } else {
        require(package.endsWith(QStringLiteral("!Forzahorizon5"), Qt::CaseInsensitive),
                QStringLiteral("The selected WGS profile is not Forza Horizon 5"));
    }
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
            QStringLiteral("Invalid FH5 payload wrapper"));
    const QByteArray raw = inflateContainer(wrapped);
    require(raw.startsWith(livery ? QByteArray("vlrc", 4) : QByteArray("gyvl", 4))
                && detail::readLeU32(raw, 4) == 1, QStringLiteral("The payload is not the selected FH5 asset type"));

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
    AppendSnapshot append;
    int offset = 0;

    require(game == ExportGameVersion::Horizon5 || game == ExportGameVersion::Motorsport2023,
            QStringLiteral("WGS append requires an FH5 or FM2023 FH5 import profile."));
    require(kind != WgsAssetKind::Livery || carId > 0, QStringLiteral("Select a valid car for the WGS livery."));
    require(QDir::isAbsolutePath(indexPath) && indexInfo.fileName() == QLatin1String(kIndexName),
            QStringLiteral("Select containers.index from the destination WGS profile."));
    require(profile.dirName().section(QLatin1Char('_'), 1).compare(QLatin1String(kFh5Scid),
                Qt::CaseInsensitive) == 0,
            QStringLiteral("Select the FH5 service profile ending in 7900C3C7."));
    append.target.indexPath = checkedFile(profile.absolutePath(), QLatin1String(kIndexName));
    append.target.creatorTag = profileCreatorTag(profile);
    append.target.game = game;
    append.target.kind = kind;
    append.target.carId = kind == WgsAssetKind::Livery ? carId : 0;
    append.index = readFile(append.target.indexPath);
    append.indexModifiedAt = QFileInfo(append.target.indexPath).lastModified();
    append.target.fingerprint = QCryptographicHash::hash(append.index, QCryptographicHash::Sha256);
    append.count = readIndexHeader(append, offset);
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

void validateAppendBlobs(const WgsExportTarget &target, const QMap<QString, QByteArray> &blobs,
                         std::span<const char *const> names) {
    const bool livery = target.kind == WgsAssetKind::Livery;
    const QString payloadKey = livery ? QStringLiteral("c_livery") : QStringLiteral("c_group");
    const QByteArray pngSignature = QByteArray::fromHex("89504e470d0a1a0a");

    require(blobs.size() == static_cast<qsizetype>(names.size()),
            QStringLiteral("WGS append requires artwork, a header, and the registered PNG thumbnails."));
    for (const char *name : names) {
        const QString key = QString::fromLatin1(name).toLower();
        const auto blob = blobs.constFind(key);
        require(blob != blobs.cend() && !blob->isEmpty() && blob->size() <= kMaximumFileSize,
                QStringLiteral("Missing or invalid WGS blob: %1").arg(QLatin1String(name)));
        require(!key.endsWith(QStringLiteral(".png")) || blob->startsWith(pngSignature),
                QStringLiteral("WGS thumbnails must be PNG blobs."));
    }
    const QByteArray raw = inflatedPayload(blobs.value(payloadKey), livery);
    require(!livery || (detail::readLeU32(raw, kLiveryInfoVersionOffset) == kFh5LiveryInfoVersion
                && detail::readLeU32(raw, 8) == 0
                && detail::readLeU32(raw, 16) == static_cast<quint32>(target.carId)
                && raw.mid(kLiveryCreatorTagOffset, target.creatorTag.size()) == target.creatorTag),
            QStringLiteral("WGS artwork must be an editable FH5 livery for the selected car and profile."));
    const HeaderMetadata header = parseHeader(blobs.value(QStringLiteral("header")));
    require(header.formatVersion == 6 && header.hasCarId == livery
                && (!livery || header.carId == static_cast<quint32>(target.carId))
                && header.guid == target.assetGuid && header.creatorTag == target.creatorTag,
            QStringLiteral("WGS header must match the new asset and profile."));
}

QString newContainerName(const AppendSnapshot &append, const QDateTime &now) {
    const WgsExportTarget &target = append.target;
    const QString prefix = target.kind == WgsAssetKind::Livery
        ? QStringLiteral("Livery_%1_").arg(target.carId, 4, 10, QLatin1Char('0'))
        : QStringLiteral("LayerGroup_0000_");
    QDateTime nameTime = now;
    QString name = prefix + nameTime.toString(QStringLiteral("yyyyMMddHHmmss"));

    while (append.entryOffsets.contains(name.toCaseFolded())) {
        nameTime = nameTime.addSecs(1);
        name = prefix + nameTime.toString(QStringLiteral("yyyyMMddHHmmss"));
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
        const QByteArray &blob = blobs[blobName.toLower()];
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

WgsExportTarget inspectWgsAppendTarget(const QString &indexPath, ExportGameVersion game,
                                      WgsAssetKind kind, int carId) {
    WgsExportTarget target = readAppendSnapshot(indexPath, game, kind, carId).target;
    target.assetGuid = guidBytes(QUuid::createUuid());

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
    require(target.assetGuid.size() == kGuidSize && !QUuid::fromRfc4122(target.assetGuid).isNull(),
            QStringLiteral("Invalid new WGS asset identity."));
    validateAppendBlobs(target, blobs, names);

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString name = newContainerName(append, now);
    const QUuid folderId = QUuid::createUuid();
    const QString folderName = folderId.toString(QUuid::Id128).toUpper();
    const QTemporaryDir temporary(profile.filePath(QStringLiteral(".fls-wgs-stage-XXXXXX")));
    require(temporary.isValid() && QDir(temporary.path()).mkdir(folderName),
            QStringLiteral("Could not stage the new WGS container."));
    const QDir staged(QDir(temporary.path()).filePath(folderName));
    const quint64 blobSize = stageContainer(staged, blobs, names);
    const QByteArray updatedIndex = indexWithNewContainer(append, name, folderId, now, blobSize);
    const QString backup = createBackup(append);
    const QString stagedPath = profile.relativeFilePath(staged.absolutePath());
    require(profile.rename(stagedPath, folderName),
            QStringLiteral("Could not publish the new WGS container."));
    try {
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

} // namespace fls
