#include "original_shader_garage.h"

#include "game_paths.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QStandardPaths>

#include <cstring>
#include <limits>
#include <type_traits>

namespace fh6 {
namespace {

constexpr quint32 kGarageCacheVersion = 2;
constexpr quint64 kMaximumCachedElements = 100000000;
constexpr char kGarageCacheMagic[] = "FLS-TOKYO-GARAGE";

template<typename T>
void writePodVector(QDataStream &stream, const std::vector<T> &values) {
    static_assert(std::is_trivially_copyable_v<T>);
    const qsizetype byteCount = static_cast<qsizetype>(values.size() * sizeof(T));
    const QByteArray raw(
        reinterpret_cast<const char *>(values.data()), byteCount);
    stream << static_cast<quint64>(values.size()) << qCompress(raw, 1);
}

template<typename T>
bool readPodVector(QDataStream &stream, std::vector<T> *values) {
    static_assert(std::is_trivially_copyable_v<T>);
    quint64 count = 0;
    QByteArray compressed;
    stream >> count >> compressed;
    if (count > kMaximumCachedElements
        || count > static_cast<quint64>(
            std::numeric_limits<qsizetype>::max() / sizeof(T))) {
        return false;
    }
    const QByteArray raw = qUncompress(compressed);
    const qsizetype byteCount = static_cast<qsizetype>(count * sizeof(T));
    if (raw.size() != byteCount) {
        return false;
    }
    values->resize(static_cast<std::size_t>(count));
    if (byteCount > 0) {
        std::memcpy(values->data(), raw.constData(), static_cast<std::size_t>(byteCount));
    }
    return true;
}

void writeMatrix(QDataStream &stream, const ModelMat4 &matrix) {
    for (const float value : matrix.m) {
        stream << value;
    }
}

bool readMatrix(QDataStream &stream, ModelMat4 *matrix) {
    for (float &value : matrix->m) {
        stream >> value;
    }
    return stream.status() == QDataStream::Ok;
}

void writeVec3(QDataStream &stream, const ModelVec3 &value) {
    stream << value.x << value.y << value.z;
}

bool readVec3(QDataStream &stream, ModelVec3 *value) {
    stream >> value->x >> value->y >> value->z;
    return stream.status() == QDataStream::Ok;
}

void writeImage(QDataStream &stream, const SwatchImage &image) {
    stream << image.width << image.height;
    writePodVector(stream, image.rgba);
}

bool readImage(QDataStream &stream, SwatchImage *image) {
    stream >> image->width >> image->height;
    return readPodVector(stream, &image->rgba)
        && ((!image->valid() && image->width == 0 && image->height == 0)
            || image->valid());
}

void writeTexture(QDataStream &stream, const OriginalShaderMaterialTexture &texture) {
    stream << texture.semantic << texture.sourceEntry;
    writeImage(stream, texture.image);
}

bool readTexture(QDataStream &stream, OriginalShaderMaterialTexture *texture) {
    stream >> texture->semantic >> texture->sourceEntry;
    return readImage(stream, &texture->image);
}

void writeSwatchTexture(QDataStream &stream, const SwatchTexture &texture) {
    stream << texture.width << texture.height << texture.arraySize
           << texture.platform << texture.sliceCount << texture.mipCount
           << texture.textureType << texture.transcoding
           << texture.sliceTableOffset << texture.payload
           << static_cast<quint32>(texture.slices.size());
    for (const SwatchTextureSlice &slice : texture.slices) {
        stream << static_cast<qint32>(slice.encoding) << slice.encodedFormat
               << slice.descriptorOffset << slice.nextSliceOffset
               << static_cast<quint32>(slice.mipLevels.size());
        for (const SwatchMipPayload &mip : slice.mipLevels) {
            stream << mip.byteOffset << mip.byteLength << mip.nextDescriptorOffset;
        }
    }
}

bool readSwatchTexture(QDataStream &stream, SwatchTexture *texture) {
    quint32 sliceCount = 0;
    stream >> texture->width >> texture->height >> texture->arraySize
           >> texture->platform >> texture->sliceCount >> texture->mipCount
           >> texture->textureType >> texture->transcoding
           >> texture->sliceTableOffset >> texture->payload >> sliceCount;
    if (sliceCount > 4096) {
        return false;
    }
    texture->slices.resize(sliceCount);
    for (SwatchTextureSlice &slice : texture->slices) {
        qint32 encoding = 0;
        quint32 mipCount = 0;
        stream >> encoding >> slice.encodedFormat >> slice.descriptorOffset
               >> slice.nextSliceOffset >> mipCount;
        if (mipCount > 64) {
            return false;
        }
        slice.encoding = static_cast<SwatchEncoding>(encoding);
        slice.mipLevels.resize(mipCount);
        for (SwatchMipPayload &mip : slice.mipLevels) {
            stream >> mip.byteOffset >> mip.byteLength >> mip.nextDescriptorOffset;
        }
    }
    return stream.status() == QDataStream::Ok;
}

void writeCarModel(QDataStream &stream, const CarModel &model) {
    stream << model.sourcePath;
    writeVec3(stream, model.boundsMin);
    writeVec3(stream, model.boundsMax);
    stream << static_cast<quint32>(model.meshes.size());
    for (const CarMesh &mesh : model.meshes) {
        stream << mesh.name << mesh.sourceModelPath << mesh.materialName
               << mesh.materialId << mesh.paintMaterialHash;
        writePodVector(stream, mesh.positions);
        writePodVector(stream, mesh.normals);
        stream << static_cast<quint32>(mesh.uvChannels.size());
        for (const std::vector<ModelVec2> &channel : mesh.uvChannels) {
            writePodVector(stream, channel);
        }
        writePodVector(stream, mesh.indices);
        writeMatrix(stream, mesh.boneTransform);
        for (const TexCoordTransform &transform : mesh.texCoordTransforms) {
            stream << transform.offsetU << transform.scaleU
                   << transform.offsetV << transform.scaleV;
        }
        stream << mesh.liveryUvChannel << mesh.carPartType
               << mesh.modelInstanceId << mesh.stockPart;
    }
}

bool readCarModel(QDataStream &stream, CarModel *model) {
    quint32 meshCount = 0;
    stream >> model->sourcePath;
    if (!readVec3(stream, &model->boundsMin) || !readVec3(stream, &model->boundsMax)) {
        return false;
    }
    stream >> meshCount;
    if (meshCount > 100000) {
        return false;
    }
    model->meshes.resize(meshCount);
    for (CarMesh &mesh : model->meshes) {
        quint32 uvChannelCount = 0;
        stream >> mesh.name >> mesh.sourceModelPath >> mesh.materialName
               >> mesh.materialId >> mesh.paintMaterialHash;
        if (!readPodVector(stream, &mesh.positions)
            || !readPodVector(stream, &mesh.normals)) {
            return false;
        }
        stream >> uvChannelCount;
        if (uvChannelCount > 32) {
            return false;
        }
        mesh.uvChannels.resize(uvChannelCount);
        for (std::vector<ModelVec2> &channel : mesh.uvChannels) {
            if (!readPodVector(stream, &channel)) {
                return false;
            }
        }
        if (!readPodVector(stream, &mesh.indices)
            || !readMatrix(stream, &mesh.boneTransform)) {
            return false;
        }
        for (TexCoordTransform &transform : mesh.texCoordTransforms) {
            stream >> transform.offsetU >> transform.scaleU
                   >> transform.offsetV >> transform.scaleV;
        }
        stream >> mesh.liveryUvChannel >> mesh.carPartType
               >> mesh.modelInstanceId >> mesh.stockPart;
    }
    return stream.status() == QDataStream::Ok;
}

using TexturePointer = std::shared_ptr<const OriginalShaderMaterialTexture>;

qint32 collectTexture(
    const TexturePointer &texture,
    QHash<const OriginalShaderMaterialTexture *, qint32> *indices,
    std::vector<TexturePointer> *textures) {
    if (texture == nullptr) {
        return -1;
    }
    const auto found = indices->constFind(texture.get());
    if (found != indices->cend()) {
        return found.value();
    }
    const qint32 index = static_cast<qint32>(textures->size());
    indices->insert(texture.get(), index);
    textures->push_back(texture);
    return index;
}

void writeScene(QDataStream &stream, const OriginalShaderGarageScene &scene) {
    QHash<const OriginalShaderMaterialTexture *, qint32> textureIndices;
    std::vector<TexturePointer> textures;
    for (const OriginalShaderGarageDraw &draw : scene.draws) {
        collectTexture(draw.diffuseTexture, &textureIndices, &textures);
        collectTexture(draw.alphaTexture, &textureIndices, &textures);
        collectTexture(draw.normalTexture, &textureIndices, &textures);
        collectTexture(draw.surfaceTexture, &textureIndices, &textures);
        collectTexture(draw.emissiveTexture, &textureIndices, &textures);
    }
    stream << scene.name << scene.geometryStatus << scene.materialStatus
           << scene.lightingStatus << scene.carStatus << scene.glassStatus
           << scene.defaultProgram.vertexShader << scene.defaultProgram.pixelShader
           << scene.floorProgram.vertexShader << scene.floorProgram.pixelShader;
    stream << static_cast<quint32>(textures.size());
    for (const TexturePointer &texture : textures) {
        writeTexture(stream, *texture);
    }
    stream << static_cast<quint32>(scene.draws.size());
    for (const OriginalShaderGarageDraw &draw : scene.draws) {
        stream << draw.name << draw.source << static_cast<qint32>(draw.family);
        writeCarModel(stream, draw.geometry);
        writeMatrix(stream, draw.placement);
        stream << textureIndices.value(draw.diffuseTexture.get(), -1)
               << textureIndices.value(draw.alphaTexture.get(), -1)
               << textureIndices.value(draw.normalTexture.get(), -1)
               << textureIndices.value(draw.surfaceTexture.get(), -1)
               << textureIndices.value(draw.emissiveTexture.get(), -1)
               << draw.diffuseUvChannel << draw.materialUvChannel
               << draw.materialUvRotationDegrees;
        for (const float value : draw.baseColor) stream << value;
        for (const float value : draw.emissiveColor) stream << value;
        stream << draw.opacity << draw.gloss << draw.metallic
               << draw.uTiling << draw.vTiling
               << draw.detailUTiling << draw.detailVTiling;
        for (const float value : draw.clearCoatTint) stream << value;
        stream << draw.clearCoatCoverage << draw.clearCoatRoughness
               << draw.rawMaterialUv << draw.translucent << draw.hidden
               << draw.clearCoatOnLivery << draw.liveryBaseTexture
               << draw.liveryAllowedSides;
    }
    for (const OriginalShaderMaterialTexture &texture : scene.materialTextures) {
        writeTexture(stream, texture);
    }
    writeSwatchTexture(stream, scene.environment.panorama.texture);
    stream << scene.environment.panorama.sphericalMode
           << scene.environment.panorama.sphericalPower
           << scene.environment.panorama.rotation
           << scene.environment.panorama.frameScale;
    writeSwatchTexture(stream, scene.environment.diffuseCubemap);
    writeSwatchTexture(stream, scene.environment.specularCubemap);
    stream << scene.colorLut.dimension << scene.colorLut.scale;
    writePodVector(stream, scene.colorLut.rgba);
    writeVec3(stream, scene.lighting.direction);
    writeVec3(stream, scene.lighting.directColor);
    writeVec3(stream, scene.lighting.ambientColor);
    stream << scene.lighting.source << static_cast<quint32>(scene.authoredLights.size());
    for (const OriginalShaderPointLight &light : scene.authoredLights) {
        writeMatrix(stream, light.transform);
        stream << light.presetHash;
        writeVec3(stream, light.color);
        stream << light.range << light.intensity << light.penumbraAngleDegrees
               << light.coneAngleDegrees << light.type << light.enabled;
    }
    writeMatrix(stream, scene.carPlacement);
}

TexturePointer textureAt(
    const std::vector<TexturePointer> &textures, qint32 index) {
    return index >= 0 && index < static_cast<qint32>(textures.size())
        ? textures[static_cast<std::size_t>(index)] : TexturePointer{};
}

bool readScene(QDataStream &stream, OriginalShaderGarageScene *scene) {
    quint32 textureCount = 0;
    quint32 drawCount = 0;
    stream >> scene->name >> scene->geometryStatus >> scene->materialStatus
           >> scene->lightingStatus >> scene->carStatus >> scene->glassStatus
           >> scene->defaultProgram.vertexShader >> scene->defaultProgram.pixelShader
           >> scene->floorProgram.vertexShader >> scene->floorProgram.pixelShader
           >> textureCount;
    if (textureCount > 100000) {
        return false;
    }
    std::vector<TexturePointer> textures;
    textures.reserve(textureCount);
    for (quint32 index = 0; index < textureCount; ++index) {
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        if (!readTexture(stream, texture.get())) {
            return false;
        }
        textures.push_back(std::move(texture));
    }
    stream >> drawCount;
    if (drawCount > 100000) {
        return false;
    }
    scene->draws.resize(drawCount);
    for (OriginalShaderGarageDraw &draw : scene->draws) {
        qint32 family = 0;
        std::array<qint32, 5> textureReferences{};
        stream >> draw.name >> draw.source >> family;
        draw.family = static_cast<OriginalShaderSurfaceFamily>(family);
        if (!readCarModel(stream, &draw.geometry)
            || !readMatrix(stream, &draw.placement)) {
            return false;
        }
        for (qint32 &reference : textureReferences) stream >> reference;
        stream >> draw.diffuseUvChannel >> draw.materialUvChannel
               >> draw.materialUvRotationDegrees;
        draw.diffuseTexture = textureAt(textures, textureReferences[0]);
        draw.alphaTexture = textureAt(textures, textureReferences[1]);
        draw.normalTexture = textureAt(textures, textureReferences[2]);
        draw.surfaceTexture = textureAt(textures, textureReferences[3]);
        draw.emissiveTexture = textureAt(textures, textureReferences[4]);
        for (float &value : draw.baseColor) stream >> value;
        for (float &value : draw.emissiveColor) stream >> value;
        stream >> draw.opacity >> draw.gloss >> draw.metallic
               >> draw.uTiling >> draw.vTiling
               >> draw.detailUTiling >> draw.detailVTiling;
        for (float &value : draw.clearCoatTint) stream >> value;
        stream >> draw.clearCoatCoverage >> draw.clearCoatRoughness
               >> draw.rawMaterialUv >> draw.translucent >> draw.hidden
               >> draw.clearCoatOnLivery >> draw.liveryBaseTexture
               >> draw.liveryAllowedSides;
    }
    for (OriginalShaderMaterialTexture &texture : scene->materialTextures) {
        if (!readTexture(stream, &texture)) {
            return false;
        }
    }
    if (!readSwatchTexture(stream, &scene->environment.panorama.texture)) {
        return false;
    }
    stream >> scene->environment.panorama.sphericalMode
           >> scene->environment.panorama.sphericalPower
           >> scene->environment.panorama.rotation
           >> scene->environment.panorama.frameScale;
    if (!readSwatchTexture(stream, &scene->environment.diffuseCubemap)
        || !readSwatchTexture(stream, &scene->environment.specularCubemap)) {
        return false;
    }
    stream >> scene->colorLut.dimension >> scene->colorLut.scale;
    if (!readPodVector(stream, &scene->colorLut.rgba)
        || !readVec3(stream, &scene->lighting.direction)
        || !readVec3(stream, &scene->lighting.directColor)
        || !readVec3(stream, &scene->lighting.ambientColor)) {
        return false;
    }
    quint32 lightCount = 0;
    stream >> scene->lighting.source >> lightCount;
    if (lightCount > 100000) {
        return false;
    }
    scene->authoredLights.resize(lightCount);
    for (OriginalShaderPointLight &light : scene->authoredLights) {
        if (!readMatrix(stream, &light.transform)) {
            return false;
        }
        stream >> light.presetHash;
        if (!readVec3(stream, &light.color)) {
            return false;
        }
        stream >> light.range >> light.intensity >> light.penumbraAngleDegrees
               >> light.coneAngleDegrees >> light.type >> light.enabled;
    }
    return readMatrix(stream, &scene->carPlacement)
        && stream.status() == QDataStream::Ok;
}

QString cacheFingerprint(const QString &gameFolder) {
    const QString media = gameMediaDir(gameFolder);
    const QStringList sources = {
        QDir(media).filePath(QStringLiteral("Stripped/gs/generalsceneassets.manifest")),
        QDir(media).filePath(QStringLiteral("Stripped/gs/swatchbins.zip")),
        QDir(media).filePath(QStringLiteral("colourgrades.zip")),
        QDir(media).filePath(QStringLiteral("HDRSkies/Forte_Garage_01.zip")),
    };
    QByteArray identity = QDir::cleanPath(gameFolder).toLower().toUtf8();
    for (const QString &source : sources) {
        const QFileInfo info(source);
        identity += '|';
        identity += QDir::cleanPath(info.absoluteFilePath()).toLower().toUtf8();
        identity += ':' + QByteArray::number(info.size());
        identity += ':' + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QString cacheDirectory() {
    const QString override = qEnvironmentVariable("FLS_GARAGE_CACHE_DIR");
    if (!override.isEmpty()) {
        return override;
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("garage"));
}

bool loadCache(
    const QString &path, const QString &fingerprint,
    OriginalShaderGarageScene *scene) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    QByteArray magic;
    quint32 version = 0;
    QString storedFingerprint;
    stream >> magic >> version >> storedFingerprint;
    if (magic != QByteArray(kGarageCacheMagic)
        || version != kGarageCacheVersion
        || storedFingerprint != fingerprint
        || !readScene(stream, scene) || !scene->valid()) {
        *scene = {};
        return false;
    }
    scene->materialStatus += QStringLiteral("; persistent garage cache hit");
    return true;
}

bool saveCache(
    const QString &path, const QString &fingerprint,
    const OriginalShaderGarageScene &scene) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << QByteArray(kGarageCacheMagic) << kGarageCacheVersion << fingerprint;
    writeScene(stream, scene);
    return stream.status() == QDataStream::Ok && file.commit();
}

} // namespace

QString originalShaderGarageCachePath(const QString &gameFolder) {
    const QByteArray folderHash = QCryptographicHash::hash(
        QDir::cleanPath(gameFolder).toLower().toUtf8(), QCryptographicHash::Sha256)
        .toHex().left(16);
    return QDir(cacheDirectory()).filePath(
        QStringLiteral("tokyo-house-v%1-%2.bin")
            .arg(kGarageCacheVersion)
            .arg(QString::fromLatin1(folderHash)));
}

OriginalShaderGarageScene loadCachedOriginalShaderGarageScene(
    const QString &gameFolder, const SwatchImage &missingTexture) {
    const QString fingerprint = cacheFingerprint(gameFolder);
    const QString path = originalShaderGarageCachePath(gameFolder);
    OriginalShaderGarageScene scene;
    if (loadCache(path, fingerprint, &scene)) {
        return scene;
    }
    scene = loadOriginalShaderGarageScene(gameFolder, missingTexture);
    if (scene.valid() && saveCache(path, fingerprint, scene)) {
        scene.materialStatus += QStringLiteral("; persistent garage cache refreshed");
    }
    return scene;
}

} // namespace fh6
