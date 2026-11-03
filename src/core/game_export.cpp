#include "game_export.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "layer.h"
#include "livery_codec.h"
#include "matrix_math.h"
#include "project_codec.h"

#include <QDir>
#include <QFile>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace fls {
namespace {

constexpr int kPaintRecordCount = 34;
constexpr int kPaintRecordSize = 40;
constexpr int kMotorsportBlockSize = 32768;
constexpr int kCreatorTagSize = 8;
constexpr quint32 kDefaultMotorsportCategory = 11;
constexpr quint32 kDefaultMotorsportPayloadFlags = 2;
constexpr int kMotorsportArtworkOffset = 66;
constexpr int kLiveryEntrySize = 7;
constexpr int kMotorsportEntryCountOffset = 58;
constexpr int kMotorsportEntriesOffset = 62;
constexpr int kEarlyEntryCountOffset = 41;
constexpr int kTaggedLiveryOverhead = 40;
constexpr int kMaskFlag = 64;
constexpr quint32 kLastEarlyLiveryVersion = 7;
constexpr quint32 kDefaultPaintFinish = 0;
constexpr quint32 kDefaultPaintSelector = 0xffffffffu;
constexpr double kNativeFramePositionRange = 25000.0;
constexpr double kNativeFrameScaleRange = 100.0;

struct ExportProfile {
    quint32 headerVersion;
    quint32 liveryVersion;
    int sectionCount;
    int paintRecordCount;
    GroupRecordGeneration groupGeneration;
};

ExportProfile exportProfile(ExportGameVersion game) {
    switch (game) {
    case ExportGameVersion::Horizon2:
        return {0, 5, kBodyLiverySectionCount, 20, GroupRecordGeneration::First};
    case ExportGameVersion::Horizon3:
        return {1, 6, kBodyLiverySectionCount, 30, GroupRecordGeneration::Second};
    case ExportGameVersion::Motorsport5:
        return {0, 2, kBodyLiverySectionCount, 20, GroupRecordGeneration::First};
    case ExportGameVersion::Motorsport6:
        return {0, 5, kBodyLiverySectionCount, 30, GroupRecordGeneration::First};
    case ExportGameVersion::Motorsport7:
        return {2, 7, kBodyLiverySectionCount, 30, GroupRecordGeneration::Second};
    case ExportGameVersion::Horizon4:
        return {4, 17, kBodyLiverySectionCount, kPaintRecordCount, GroupRecordGeneration::Current};
    case ExportGameVersion::Horizon5:
        return {6, 18, kBodyLiverySectionCount, kPaintRecordCount, GroupRecordGeneration::Current};
    case ExportGameVersion::Motorsport2023:
        return {9, 19, kBodyLiverySectionCount, kPaintRecordCount, GroupRecordGeneration::Current};
    default:
        throw std::runtime_error("unsupported native export game");
    }
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()) {
        throw std::runtime_error(("could not write export file: " + path).toStdString());
    }
}

HeaderMetadata projectHeader(const Project &project) {
    if (project.headerMetadata) {
        return *project.headerMetadata;
    }
    if (!project.sourceHeader.isEmpty()) {
        return parseHeader(project.sourceHeader);
    }

    return defaultDraftHeader(project.name, QString(), static_cast<quint32>(project.carId));
}

QByteArray destinationCreatorTag(const Project &project, const QString &outputFolder) {
    QByteArray tag = creatorTagFromSavePath(outputFolder);
    if (tag.isEmpty()) {
        tag = projectHeader(project).creatorTag.left(kCreatorTagSize);
        tag.append(QByteArray(kCreatorTagSize - tag.size(), '\0'));
    }

    return tag;
}

QByteArray nativeGuidBytes(const QUuid &guid) {
    QByteArray bytes;
    detail::appendLeU32(bytes, guid.data1);
    detail::appendLeU16(bytes, guid.data2);
    detail::appendLeU16(bytes, guid.data3);
    bytes.append(reinterpret_cast<const char *>(guid.data4), 8);

    return bytes;
}

QByteArray nativeHeader(const Project &project, ExportGameVersion game,
                       quint32 shapeCount, const QByteArray &creatorTag,
                       const QString &outputFolder) {
    HeaderMetadata metadata = projectHeader(project);
    const QUuid folderId(QDir(outputFolder).dirName());
    const quint32 sourceVersion = metadata.formatVersion;
    const bool motorsport = game == ExportGameVersion::Motorsport2023;
    const ExportProfile profile = exportProfile(game);

    metadata.formatVersion = profile.headerVersion;
    metadata.name = project.name;
    metadata.published = false;
    metadata.description.clear();
    metadata.publishedTail.clear();
    metadata.trailing.clear();
    metadata.typeValue = shapeCount;
    metadata.carId = project.isLivery ? static_cast<quint32>(project.carId) : 0;
    metadata.hasCarId = motorsport || project.isLivery;
    metadata.creatorTag = creatorTag;
    metadata.fieldBlock = metadata.fieldBlock.left(16);
    metadata.fieldBlock.append(QByteArray(16 - metadata.fieldBlock.size(), '\0'));
    metadata.fieldBlock.replace(12, 4, QByteArray(project.isLivery ? "\x03\0\0\0" : "\x02\0\0\0", 4));
    if (profile.headerVersion <= 1) {
        const int identifierSize = 8;
        if (metadata.guid.size() != identifierSize || sourceVersion != profile.headerVersion) {
            metadata.guid = nativeGuidBytes(QUuid::createUuid()).left(identifierSize);
        }
        metadata.sectionPrefix = QByteArray(16, '\0');
        metadata.sectionPrefix.replace(12, 4, QByteArray(4, '\xff'));
        metadata.sectionMarker.clear();
        metadata.sectionPadding = QByteArray(static_cast<int>(profile.headerVersion), '\x01');
        metadata.category.reset();
        if (profile.headerVersion == 0) {
            metadata.guid = QByteArray(identifierSize, '\xff');
        }
        if (game == ExportGameVersion::Motorsport5 || game == ExportGameVersion::Horizon2
            || (game == ExportGameVersion::Horizon3 && !project.isLivery)) {
            metadata.trailing = QByteArray(project.isLivery ? 2 : 6 - profile.headerVersion, '\0');
            if (!project.isLivery) {
                metadata.trailing.append(QByteArray(4, '\xff'));
                metadata.trailing.append(metadata.sectionPadding);
            }
            detail::appendLeU32(metadata.trailing, project.isLivery ? metadata.carId : shapeCount);
            metadata.trailing.append(QByteArray(identifierSize, '\xff'));
        }

        return buildHeader(metadata);
    }
    if (metadata.guid.size() != 16) {
        metadata.guid = nativeGuidBytes(QUuid::createUuid());
    }
    if (motorsport && !folderId.isNull()) {
        metadata.guid = nativeGuidBytes(folderId);
    }
    metadata.sectionPrefix = QByteArray(28, '\0');
    metadata.sectionMarker = profile.headerVersion == 2
        ? QByteArray(1, '\x01') : QByteArray("\x01\x02", 2);
    metadata.sectionPadding = QByteArray(profile.headerVersion == 2 ? 0 : motorsport ? 7 : 3, '\0');
    if (motorsport) {
        metadata.sectionPrefix[0] = '\x01';
        metadata.sectionPrefix.replace(12, 16, metadata.guid);
        metadata.sectionPadding[3] = '\x01';
        metadata.category = metadata.category.value_or(kDefaultMotorsportCategory);
    } else {
        metadata.category.reset();
        if (profile.headerVersion == 2) {
            metadata.trailing = QByteArray(project.isLivery ? 7 : 11, '\0');
            metadata.trailing.append('\x01');
            detail::appendLeU32(metadata.trailing, shapeCount);
            if (project.isLivery) {
                detail::appendLeU32(metadata.trailing, metadata.carId);
            }
            metadata.trailing.append(QByteArray(16, '\0'));
        } else {
            metadata.trailing = QByteArray(project.isLivery ? 14 : 2, '\0');
            if (!project.isLivery) {
                detail::appendLeU32(metadata.trailing, shapeCount);
                metadata.trailing.append(QByteArray(16, '\0'));
            }
        }
    }

    return buildHeader(metadata);
}

QByteArray legacyPaintBlock(const Project &project, const ExportProfile &profile) {
    const QByteArray &source = project.liverySource;
    const int artwork = source.indexOf(QByteArray("gyvl", 4));
    const bool tagged = profile.liveryVersion != 2;
    const int paintSize = 1 + profile.paintRecordCount * kPaintRecordSize + 2;
    if (artwork >= 4) {
        const quint32 artworkSize = detail::readLeU32(source, artwork - 4);
        const qint64 stats = static_cast<qint64>(artwork) + artworkSize;
        const qint64 paintTag = stats + (tagged ? 4 : 0) + kLegacyLiveryCounterCount * 4;
        const qint64 paint = paintTag + (tagged ? 4 : 0);
        const int informationVersionOffset = detail::readLeU32(source, 4) == 2 ? 45 : 29;
        const quint32 sourceVersion = source.size() >= informationVersionOffset + 4
            ? detail::readLeU32(source, informationVersionOffset) : 0;
        const bool matchingVersion = sourceVersion == profile.liveryVersion
            || (profile.liveryVersion >= 17 && sourceVersion >= 17 && sourceVersion <= 19);
        if (stats >= artwork && paint + paintSize <= source.size() && matchingVersion
            && (!tagged || (source.mid(stats, 4) == QByteArray("yrvl", 4)
                && source.mid(paintTag, 4) == QByteArray("yrvl", 4)
                && source.mid(paint + paintSize, 4) == QByteArray("yrvl", 4)))) {
            bool validRecords = true;
            for (int index = 0; index < profile.paintRecordCount; ++index) {
                const int offset = static_cast<int>(paint) + 1 + index * kPaintRecordSize;
                validRecords = validRecords && detail::bytesAt(source, offset, {1, 1});
            }
            if (validRecords) {
                return source.mid(paint, paintSize);
            }
        }
    }

    QByteArray paint;
    paint.append('\0');
    for (int index = 0; index < profile.paintRecordCount; ++index) {
        QByteArray record(kPaintRecordSize, '\0');
        record[0] = '\x01';
        record[1] = '\x01';
        QByteArray paintDefaults;
        const bool selectable = (index >= 5 && index <= 7) || index >= 12;
        detail::appendLeU32(paintDefaults, selectable ? kDefaultPaintSelector : 0);
        detail::appendLeU32(paintDefaults, kDefaultPaintFinish);
        record.replace(27, paintDefaults.size(), paintDefaults);
        paint.append(record);
    }
    paint.append("\x00\x01", 2);

    return paint;
}

void prepareNativeArtworkLayers(scene::Group &group) {
    auto &children = group.children;
    children.erase(std::remove_if(children.begin(), children.end(), [](const auto &child) {
        return !child->visible;
    }), children.end());
    for (const auto &child : children) {
        if (child->kind() == scene::LayerKind::Group) {
            prepareNativeArtworkLayers(static_cast<scene::Group &>(*child));
        } else if (child->kind() == scene::LayerKind::Shape) {
            static_cast<scene::Shape &>(*child).marker.clear();
        }
    }
}

bool containsArtwork(const scene::Layer &layer) {
    if (layer.kind() == scene::LayerKind::Shape) {
        return true;
    }
    if (layer.kind() == scene::LayerKind::Group) {
        const auto &group = static_cast<const scene::Group &>(layer);

        return std::any_of(group.children.begin(), group.children.end(), [](const auto &child) {
            return containsArtwork(*child);
        });
    }

    return false;
}

quint64 maskedLayerCount(const scene::Layer &layer, bool inheritedMask) {
    if (!layer.visible) {
        return 0;
    }
    if (layer.kind() == scene::LayerKind::Shape) {
        return inheritedMask || static_cast<const scene::Shape &>(layer).mask ? 1 : 0;
    }
    quint64 count = 0;
    if (layer.kind() == scene::LayerKind::Group) {
        const auto &group = static_cast<const scene::Group &>(layer);
        const bool masked = inheritedMask || (group.flags & kMaskFlag) != 0;

        for (const auto &child : group.children) {
            count += maskedLayerCount(*child, masked);
        }
    }

    return count;
}

void discardMaskedLayers(scene::Group &group, bool inheritedMask = false) {
    auto &children = group.children;
    const bool masked = inheritedMask || (group.flags & kMaskFlag) != 0;

    children.erase(std::remove_if(children.begin(), children.end(), [masked](const auto &child) {
        if (child->kind() == scene::LayerKind::Shape) {
            return masked || static_cast<const scene::Shape &>(*child).mask;
        }
        if (child->kind() == scene::LayerKind::Group) {
            auto &childGroup = static_cast<scene::Group &>(*child);
            discardMaskedLayers(childGroup, masked);

            return !childGroup.isLiverySection && !containsArtwork(childGroup);
        }

        return false;
    }), children.end());
}

Project nativeArtworkProject(const Project &project, const ExportProfile &profile, ExportGameVersion game) {
    Project artwork = project;
    std::array<bool, 11> sections{};
    if (artwork.root == nullptr) {
        return artwork;
    }
    prepareNativeArtworkLayers(*artwork.root);
    if (!exportGameSupportsMasks(game)) {
        discardMaskedLayers(*artwork.root);
    }
    auto &children = artwork.root->children;
    children.erase(std::remove_if(children.begin(), children.end(), [](const auto &child) {
        return child->kind() == scene::LayerKind::Group
            && static_cast<const scene::Group &>(*child).isLiverySection
            && static_cast<const scene::Group &>(*child).liverySectionSlot >= kBodyLiverySectionCount
            && static_cast<const scene::Group &>(*child).liverySectionSlot < kFH6SectionCount;
    }), children.end());
    if (profile.headerVersion <= 1 || (!artwork.liverySource.isEmpty()
        && parseInflatedLiveryPayload(artwork.liverySource).legacyRecords)) {
        artwork.liverySource.clear();
    }
    for (const auto &child : artwork.root->children) {
        if (!containsArtwork(*child)) {
            continue;
        }
        if (child->kind() != scene::LayerKind::Group) {
            throw std::runtime_error("livery artwork must belong to a section");
        }
        const auto &group = static_cast<const scene::Group &>(*child);
        const int slot = group.liverySectionSlot;
        if (!group.isLiverySection || slot < 0 || slot >= static_cast<int>(sections.size())
            || sections[slot]) {
            throw std::runtime_error("livery artwork has invalid or duplicate section metadata");
        }
        sections[slot] = true;
    }

    return artwork;
}

int bakeNativeArtwork(scene::Group &group, const Matrix3 &adjustment) {
    int count = 0;
    auto &children = group.children;
    for (auto child = children.begin(); child != children.end();) {
        if ((*child)->kind() == scene::LayerKind::Group) {
            const int childCount = bakeNativeArtwork(static_cast<scene::Group &>(**child), adjustment);
            if (childCount == 0) {
                child = children.erase(child);
                continue;
            }
            count += childCount;
        } else if ((*child)->kind() == scene::LayerKind::Shape) {
            (*child)->transform = decomposeTransform2D(detail::multiply(adjustment, (*child)->worldMatrix()));
            ++count;
        }
        ++child;
    }
    group.transform = {};

    return count;
}

double nativeArtworkScale(const scene::Layer &layer, const Matrix3 &adjustment) {
    double scale = 1.0;
    if (layer.kind() == scene::LayerKind::Shape) {
        const auto transform = decomposeTransform2D(detail::multiply(adjustment, layer.worldMatrix()));
        scale = std::max({scale, std::abs(transform.x) / kNativeFramePositionRange,
            std::abs(transform.y) / kNativeFramePositionRange,
            std::abs(transform.scaleX) / kNativeFrameScaleRange,
            std::abs(transform.scaleY) / kNativeFrameScaleRange});
    } else if (layer.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(layer).children) {
            scale = std::max(scale, nativeArtworkScale(*child, adjustment));
        }
    }

    return std::ceil(scale);
}

QByteArray earlyArtwork(const Project &project, const ExportProfile &profile,
                        std::array<int, 11> &counts) {
    constexpr int kStandaloneRootCountOffset = 30;
    constexpr int kSectionRootHeaderSize = 7;
    QByteArray artwork("gyvl", 4);
    artwork.append(QByteArray(8, '\0'));
    detail::appendLeFloat(artwork, 1.0f);
    artwork.append(QByteArray(5, '\0'));
    for (int slot = 0; slot < profile.sectionCount; ++slot) {
        Project section = project;
        auto &children = section.root->children;
        children.erase(std::remove_if(children.begin(), children.end(), [slot](const auto &child) {
            return child->kind() != scene::LayerKind::Group
                || static_cast<const scene::Group &>(*child).liverySectionSlot != slot;
        }), children.end());
        const Matrix3 canvasInverse = invertAffine(liverySectionCanvasTransform(slot));
        const double scale = nativeArtworkScale(*section.root, canvasInverse);
        const Matrix3 scaleInverse = affine(1.0 / scale, 0, 0, 0, 1.0 / scale, 0);
        counts[slot] = bakeNativeArtwork(*section.root, detail::multiply(scaleInverse, canvasInverse));
        if (counts[slot] == 0) {
            artwork.append(QByteArray(8, '\0'));
            detail::appendLeFloat(artwork, 1.0f);
            detail::appendLeFloat(artwork, static_cast<float>(kFH6LiverySlots[std::max(0, slot - 1)].rotationDeg));
            artwork.append(QByteArray(7, '\0'));
            continue;
        }
        if (scale == 1.0) {
            section.root = std::unique_ptr<scene::Group>(
                static_cast<scene::Group *>(section.root->takeAt(0).release()));
        }
        section.sourceDecPrefix.clear();
        const QByteArray group = buildNestedPayload(section, {}, GroupRecordGeneration::First, false, true);
        artwork.append(group.mid(kStandaloneRootCountOffset, kSectionRootHeaderSize));
        if (scale > 1.0) {
            artwork.append('\x01');
            detail::appendLeFloat(artwork, 0.0f);
            detail::appendLeFloat(artwork, 0.0f);
            detail::appendLeFloat(artwork, static_cast<float>(scale));
            detail::appendLeFloat(artwork, 0.0f);
        }
        artwork.append(group.mid(kStandaloneRootCountOffset + kSectionRootHeaderSize));
        if (slot + 1 < profile.sectionCount) {
            artwork.append(QByteArray(8, '\0'));
            detail::appendLeFloat(artwork, 1.0f);
            detail::appendLeFloat(artwork, static_cast<float>(kFH6LiverySlots[slot].rotationDeg));
            artwork.append('\0');
        }
    }
    if (counts[profile.sectionCount - 1] == 0) {
        artwork.chop(17);
    }

    return artwork;
}

QByteArray nativeArtwork(const Project &project, const ExportProfile &profile, ExportGameVersion game,
                         std::array<int, 11> &counts) {
    const Project artworkProject = nativeArtworkProject(project, profile, game);
    if (profile.liveryVersion <= kLastEarlyLiveryVersion) {
        return earlyArtwork(artworkProject, profile, counts);
    }

    return buildLiveryGyvlForSectionCount(artworkProject, profile.sectionCount, &counts);
}

QByteArray nativeLiveryPayload(const Project &project, ExportGameVersion game,
                              const QByteArray &creatorTag,
                              std::array<int, 11> &counts) {
    if (!project.isLivery) {
        throw std::runtime_error("not a livery project");
    }
    const bool motorsport = game == ExportGameVersion::Motorsport2023;
    const ExportProfile profile = exportProfile(game);
    const bool early = profile.liveryVersion <= kLastEarlyLiveryVersion;
    const bool tagged = profile.liveryVersion != 2;
    const QByteArray paint = legacyPaintBlock(project, profile);
    const QByteArray artwork = nativeArtwork(project, profile, game, counts);
    QByteArray payload("vlrc", 4);
    QByteArray sourceEntries;
    const QByteArray &source = project.liverySource;
    const int sourceArtwork = source.indexOf(QByteArray("gyvl", 4));
    const bool sourceMotorsport = sourceArtwork >= kMotorsportArtworkOffset
        && detail::readLeU32(source, 4) == 2;
    quint32 entryCount = 0;
    if (motorsport && sourceMotorsport) {
        entryCount = detail::readLeU32(source, kMotorsportEntryCountOffset);
        const quint64 entriesSize = static_cast<quint64>(entryCount) * kLiveryEntrySize;
        if (entriesSize + kMotorsportArtworkOffset != static_cast<quint64>(sourceArtwork)) {
            throw std::runtime_error("invalid Motorsport livery entry metadata");
        }
        sourceEntries = source.mid(kMotorsportEntriesOffset, static_cast<qsizetype>(entriesSize));
    } else if (early && profile.liveryVersion >= 5 && sourceArtwork >= kEarlyEntryCountOffset + 8
        && detail::readLeU32(source, 4) == 1
        && detail::readLeU32(source, 29) == profile.liveryVersion) {
        const int entryCountOffset = kEarlyEntryCountOffset + (profile.liveryVersion >= 6 ? 1 : 0);
        const quint32 sourceEntryCount = detail::readLeU32(source, entryCountOffset);
        const quint64 entriesSize = static_cast<quint64>(sourceEntryCount) * kLiveryEntrySize;
        const int sourcePaintSize = source.size() - sourceArtwork
            - static_cast<int>(detail::readLeU32(source, sourceArtwork - 4)) - kTaggedLiveryOverhead;
        if (entriesSize + entryCountOffset + 8 == static_cast<quint64>(sourceArtwork)
            && sourcePaintSize == 1 + profile.paintRecordCount * kPaintRecordSize + 2) {
            entryCount = sourceEntryCount;
            sourceEntries = source.mid(entryCountOffset + 4, static_cast<qsizetype>(entriesSize));
        }
    }

    detail::appendLeU32(payload, motorsport ? 2 : 1);
    detail::appendLeU32(payload, 0);
    detail::appendLeU32(payload, 0);
    detail::appendLeU32(payload, static_cast<quint32>(project.carId));
    detail::appendLeU32(payload, motorsport
        ? (sourceMotorsport ? detail::readLeU32(source, 20) : kDefaultMotorsportPayloadFlags) : 0);
    if (motorsport) {
        payload.append(QByteArray(8, '\xff'));
        payload.append(QByteArray(8, '\0'));
    }
    payload.append('\0');
    payload.append("yrvl", 4);
    detail::appendLeU32(payload, profile.liveryVersion);
    payload.append(creatorTag);
    if (motorsport) {
        payload.append('\0');
        detail::appendLeU32(payload, entryCount);
        payload.append(sourceEntries);
    } else if (!early) {
        detail::appendLeU32(payload, 1);
        payload.append('\0');
    } else if (profile.liveryVersion >= 5) {
        if (profile.liveryVersion >= 6) {
            payload.append('\x01');
        }
        detail::appendLeU32(payload, entryCount);
        payload.append(sourceEntries);
    }
    detail::appendLeU32(payload, static_cast<quint32>(artwork.size()));
    payload.append(artwork);
    if (tagged) {
        payload.append("yrvl", 4);
    }
    for (int index = 0; index < kLegacyLiveryCounterCount; ++index) {
        detail::appendLeU32(payload, static_cast<quint32>(counts[index]));
    }
    if (tagged) {
        payload.append("yrvl", 4);
    }
    payload.append(paint);
    if (tagged) {
        payload.append("yrvl", 4);
    }
    if (motorsport || game == ExportGameVersion::Horizon5) {
        detail::appendLeU32(payload, 0);
    }

    return payload;
}

void createOutputFolder(const QString &path) {
    if (path.isEmpty() || !QDir().mkpath(path)) {
        throw std::runtime_error(("could not create export folder: " + path).toStdString());
    }
}

void copyThumbnail(const Project &project, const QString &outputFolder,
                   ExportGameVersion game) {
    if (project.sourceFolder.isEmpty()) {
        return;
    }
    const QStringList candidates = {
        exportThumbnailName(game, project.isLivery), QStringLiteral("thumbnail"),
        QStringLiteral("bigThumb.png"), QStringLiteral("bigThumb.webp"),
        QStringLiteral("thumb.png"), QStringLiteral("thumb.webp")};
    for (const QString &name : candidates) {
        QFile file(QDir(project.sourceFolder).filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray thumbnail = file.readAll();
        if (thumbnail.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
            writeFile(QDir(outputFolder).filePath(exportThumbnailName(game, project.isLivery)), thumbnail);
            return;
        }
    }
}

quint64 visibleShapeCount(const scene::Layer &layer) {
    if (!layer.visible) {
        return 0;
    }
    if (layer.kind() == scene::LayerKind::Shape) {
        if (static_cast<const scene::Shape &>(layer).raster) {
            throw std::runtime_error("logo layers can only be exported from livery projects");
        }
        return 1;
    }
    quint64 count = 0;
    if (layer.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(layer).children) {
            count += visibleShapeCount(*child);
        }
    }

    return count;
}

} // namespace

QString exportThumbnailName(ExportGameVersion game, bool livery) {
    if (game == ExportGameVersion::Motorsport2023) {
        return QStringLiteral("thumbnail");
    }
    if (game != ExportGameVersion::Horizon6) {
        return livery ? QStringLiteral("bigThumb.png") : QStringLiteral("thumb.png");
    }

    return livery ? QStringLiteral("bigThumb.webp") : QStringLiteral("thumb.webp");
}

bool exportGameSupportsMasks(ExportGameVersion game) {
    switch (game) {
    case ExportGameVersion::Horizon2:
    case ExportGameVersion::Horizon3:
    case ExportGameVersion::Motorsport5:
    case ExportGameVersion::Motorsport6:
    case ExportGameVersion::Motorsport7:
        return false;
    default:
        return true;
    }
}

quint64 exportMaskedLayerCount(const Project &project) {
    return project.root ? maskedLayerCount(*project.root, false) : 0;
}

QByteArray encodeGameLiveryPayload(const Project &project, ExportGameVersion game) {
    if (game == ExportGameVersion::Horizon6) {
        return encodeCLiveryPayload(project);
    }
    std::array<int, 11> counts{};

    return nativeLiveryPayload(project, game, destinationCreatorTag(project, QString()), counts);
}

void exportGameLivery(const Project &project, const QString &outputFolder, ExportGameVersion game) {
    if (game == ExportGameVersion::Horizon6) {
        exportCLivery(project, outputFolder);
        return;
    }
    std::array<int, 11> counts{};
    const QByteArray creatorTag = destinationCreatorTag(project, outputFolder);
    const QByteArray payload = nativeLiveryPayload(project, game, creatorTag, counts);
    const quint32 shapeCount = static_cast<quint32>(std::accumulate(counts.begin(), counts.end(), 0));
    const QByteArray header = nativeHeader(project, game, shapeCount, creatorTag, outputFolder);
    QByteArray wrapped;

    if (game == ExportGameVersion::Motorsport2023) {
        wrapped = deflateContainerBlocks(payload, kMotorsportBlockSize);
    } else {
        wrapped = deflateContainer(payload);
    }
    createOutputFolder(outputFolder);
    QDir output(outputFolder);
    writeFile(output.filePath(game == ExportGameVersion::Motorsport2023
        ? QStringLiteral("data") : QStringLiteral("C_livery")), wrapped);
    writeFile(output.filePath(QStringLiteral("header")), header);
    copyThumbnail(project, outputFolder, game);
}

void exportGameGroup(const Project &project, const QString &outputFolder,
                    ExportGameVersion game, const SpriteSizeFn &spriteSize) {
    if (game == ExportGameVersion::Horizon6) {
        exportNestedProjectFolder(project, outputFolder, project.name, spriteSize);
        return;
    }
    if (project.isLivery || project.root == nullptr) {
        throw std::runtime_error("not a group project");
    }
    Project nativeProject = project;
    if (!exportGameSupportsMasks(game)) {
        discardMaskedLayers(*nativeProject.root);
    }
    const quint64 count = visibleShapeCount(*nativeProject.root);
    if (count > std::numeric_limits<quint32>::max()) {
        throw std::runtime_error("too many group shapes for header metadata");
    }
    nativeProject.sourceDecPrefix.clear();
    const QByteArray payload = buildNestedPayload(nativeProject, spriteSize,
                                                 exportProfile(game).groupGeneration);
    const QByteArray header = nativeHeader(project, game, static_cast<quint32>(count),
                                           destinationCreatorTag(project, outputFolder), outputFolder);
    const bool motorsport = game == ExportGameVersion::Motorsport2023;
    const QByteArray fileBytes = motorsport ? payload : deflateContainer(payload);

    createOutputFolder(outputFolder);
    QDir output(outputFolder);
    writeFile(output.filePath(motorsport ? QStringLiteral("data") : QStringLiteral("C_group")), fileBytes);
    writeFile(output.filePath(QStringLiteral("header")), header);
    copyThumbnail(project, outputFolder, game);
}

} // namespace fls
