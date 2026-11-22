#include "raster_decals.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QtEndian>

#include <algorithm>
#include <utility>

namespace fls {
namespace {

constexpr qsizetype kHeaderSize = 16;
constexpr qsizetype kLegacyEntrySize = 24;
constexpr qsizetype kTextureEntrySize = 16;
constexpr qsizetype kGameCodeSize = 8;
constexpr qsizetype kLogoEntrySize = kGameCodeSize + 8;
constexpr quint32 kMaxLogoId = 32767;
constexpr quint32 kMaxImageExtent = 16383;
constexpr char kLegacyMagic[] = "FH6RAST1";
constexpr char kMagic[] = "FLSRAST2";
const QString kDefaultGame = QStringLiteral("FH6");

quint32 readU32(const QByteArray &data, qsizetype offset) {
    return qFromLittleEndian<quint32>(data.constData() + offset);
}

void setError(QString *error, const QString &message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

bool RasterDecalPack::load(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("could not open raster decal pack: %1").arg(path));
        return false;
    }

    QByteArray bytes = file.readAll();
    QVector<Entry> entries;
    QHash<QString, QHash<quint32, quint32>> games;
    const QByteArray magic = bytes.left(8);
    const bool webp = magic == kMagic;
    if (bytes.size() < kHeaderSize || (!webp && magic != kLegacyMagic)) {
        setError(error, QStringLiteral("not a raster decal pack: %1").arg(path));
        return false;
    }

    const quint32 first = readU32(bytes, 8);
    const quint32 second = readU32(bytes, 12);
    const quint32 textureCount = webp ? first : second;
    const quint32 logoCount = webp ? second : 0;
    const qsizetype textureRecordSize = webp ? kTextureEntrySize : kLegacyEntrySize;
    const quint64 texturesEnd = kHeaderSize + quint64(textureCount) * textureRecordSize;
    const quint64 directoryEnd = texturesEnd + quint64(logoCount) * kLogoEntrySize;

    if (!webp && first != 1) {
        setError(error, QStringLiteral("unsupported raster decal pack version %1").arg(first));
        return false;
    }
    if (directoryEnd > quint64(bytes.size())) {
        setError(error, QStringLiteral("truncated raster decal pack index: %1").arg(path));
        return false;
    }

    entries.reserve(textureCount);
    for (quint32 i = 0; i < textureCount; ++i) {
        const qsizetype record = kHeaderSize + qsizetype(i) * textureRecordSize;
        const qsizetype pos = webp ? record : record + 4;
        const quint32 width = readU32(bytes, pos);
        const quint32 height = readU32(bytes, pos + 4);
        const quint32 offset = readU32(bytes, pos + 8);
        const quint32 length = readU32(bytes, pos + 12);

        if (width == 0 || width > kMaxImageExtent || height == 0 || height > kMaxImageExtent
            || length == 0 || offset < directoryEnd || quint64(offset) + length > quint64(bytes.size())) {
            setError(error, QStringLiteral("invalid texture %1 in raster decal pack: %2").arg(i).arg(path));
            return false;
        }
        if (!webp) {
            const quint32 id = readU32(bytes, record);
            if (id == 0 || id > kMaxLogoId || readU32(bytes, pos + 16) != 0 || length != width * height * 4) {
                setError(error, QStringLiteral("invalid legacy logo %1 in raster decal pack: %2").arg(id).arg(path));
                return false;
            }
            games[kDefaultGame].insert(id, i + 1);
        }
        entries.push_back({QSize(static_cast<int>(width), static_cast<int>(height)), offset, length});
    }
    for (quint32 i = 0; i < logoCount; ++i) {
        const qsizetype pos = static_cast<qsizetype>(texturesEnd) + qsizetype(i) * kLogoEntrySize;
        const QByteArray gameCode = bytes.mid(pos, kGameCodeSize);
        const QString game = QString::fromLatin1(gameCode.constData());
        const quint32 id = readU32(bytes, pos + kGameCodeSize);
        const quint32 texture = readU32(bytes, pos + kGameCodeSize + 4);

        if (game.isEmpty() || id == 0 || id > kMaxLogoId || texture >= textureCount) {
            setError(error, QStringLiteral("invalid logo entry %1 in raster decal pack: %2").arg(i).arg(path));
            return false;
        }
        auto &logos = games[game];
        if (logos.contains(id)) {
            setError(error, QStringLiteral("duplicate logo %1 for %2 in raster decal pack: %3").arg(id).arg(game, path));
            return false;
        }
        logos.insert(id, texture + 1);
    }

    data_ = std::move(bytes);
    entries_ = std::move(entries);
    games_ = std::move(games);
    path_ = path;
    loaded_ = true;
    webp_ = webp;

    return true;
}

QVector<quint32> RasterDecalPack::ids(const QString &game) const {
    const auto logos = games_.constFind(game);
    if (logos == games_.cend()) return {};
    QVector<quint32> result = logos->keys();
    std::sort(result.begin(), result.end());

    return result;
}

quint32 RasterDecalPack::textureKey(quint32 id, const QString &game) const {
    const auto logos = games_.constFind(game.isEmpty() ? kDefaultGame : game);

    return logos == games_.cend() ? 0 : logos->value(id);
}

QSize RasterDecalPack::decalSize(quint32 id, const QString &game) const {
    const Entry *texture = entry(id, game);

    return texture == nullptr ? QSize() : texture->size;
}

RasterDecal RasterDecalPack::decal(quint32 id, const QString &game) const {
    const Entry *texture = entry(id, game);
    if (texture == nullptr) return {};
    const QByteArray payload = QByteArray::fromRawData(data_.constData() + texture->offset, texture->length);
    QByteArray rgba;

    if (webp_) {
        const QImage image = QImage::fromData(payload, "webp").convertToFormat(QImage::Format_RGBA8888);
        if (image.size() != texture->size) return {};
        rgba = QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    } else {
        rgba = QByteArray(payload.constData(), payload.size());
    }

    return {id, texture->size.width(), texture->size.height(), std::move(rgba)};
}

const RasterDecalPack::Entry *RasterDecalPack::entry(quint32 id, const QString &game) const {
    const quint32 key = textureKey(id, game);

    return key == 0 ? nullptr : &entries_[key - 1];
}

const RasterDecalPack &sharedRasterDecals() {
    static const RasterDecalPack pack = [] {
        RasterDecalPack loaded;
        const QString appPath = QCoreApplication::applicationDirPath();
        const QStringList candidates = {
            QDir(appPath).filePath(QStringLiteral("assets/raster/decal_textures.bin")),
            QDir(QDir::currentPath()).filePath(QStringLiteral("assets/raster/decal_textures.bin")),
        };
        for (const QString &path : candidates) {
            if (QFileInfo::exists(path) && loaded.load(path)) {
                break;
            }
        }

        return loaded;
    }();

    return pack;
}

} // namespace fls
