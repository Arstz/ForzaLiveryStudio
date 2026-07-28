#include "garage_environment.h"

#include "game_paths.h"
#include "zip_extract.h"

#include <QDir>
#include <QXmlStreamReader>

#include <cmath>

namespace fh6 {
namespace {

bool validateCubemap(
    const SwatchTexture &texture, SwatchEncoding encoding, int mipCount,
    const QString &resource, QString *error) {
    if (texture.platform != 0 || texture.sliceCount != 6 || texture.arraySize != 1
        || texture.mipCount != mipCount) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 has unsupported texture topology").arg(resource);
        }
        return false;
    }
    for (const SwatchTextureSlice &slice : texture.slices) {
        if (slice.encoding != encoding
            || slice.mipLevels.size() != static_cast<size_t>(mipCount)) {
            if (error != nullptr) {
                *error = QStringLiteral("%1 has unsupported slice encoding or mip count")
                             .arg(resource);
            }
            return false;
        }
    }
    return true;
}

std::optional<SwatchTexture> parseResource(
    const QByteArray &bytes, const QString &resource, QString *error) {
    QString parseError;
    auto texture = parseSwatchTexture(bytes, &parseError);
    if (!texture && error != nullptr) {
        *error = QStringLiteral("%1: %2").arg(resource, parseError);
    }
    return texture;
}

bool parsePanoramaMetadata(
    const QByteArray &bytes, GaragePanoramaResources *panorama, QString *error) {
    QXmlStreamReader xml(bytes);
    bool foundRoot = false;
    bool foundFrame = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) {
            continue;
        }
        if (!foundRoot && xml.name() == QStringLiteral("FrameData")) {
            const QXmlStreamAttributes attributes = xml.attributes();
            bool modeOk = false;
            bool powerOk = false;
            bool rotationOk = false;
            panorama->sphericalMode = attributes.value(QStringLiteral("spherical")).toInt(&modeOk);
            panorama->sphericalPower = attributes.value(QStringLiteral("sphericalpower")).toFloat(&powerOk);
            panorama->rotation = attributes.value(QStringLiteral("rotation")).toFloat(&rotationOk);
            if (!modeOk || !powerOk || !rotationOk) {
                if (error != nullptr) {
                    *error = QStringLiteral("FrameData.xml has invalid spherical metadata");
                }
                return false;
            }
            foundRoot = true;
        } else if (foundRoot && xml.name() == QStringLiteral("Frame")) {
            bool scaleOk = false;
            panorama->frameScale = xml.attributes().value(QStringLiteral("scale")).toFloat(&scaleOk);
            if (!scaleOk) {
                if (error != nullptr) {
                    *error = QStringLiteral("FrameData.xml has an invalid frame scale");
                }
                return false;
            }
            foundFrame = true;
            break;
        }
    }
    if (xml.hasError() || !foundRoot || !foundFrame
        || !std::isfinite(panorama->sphericalPower)
        || !std::isfinite(panorama->rotation)
        || !std::isfinite(panorama->frameScale)
        || panorama->sphericalMode != 2
        || panorama->sphericalPower < 0.0f || panorama->sphericalPower > 1.0f
        || panorama->rotation != 0.0f || panorama->frameScale <= 0.0f) {
        if (error != nullptr) {
            *error = xml.hasError()
                ? QStringLiteral("FrameData.xml: %1").arg(xml.errorString())
                : QStringLiteral("FrameData.xml has unsupported garage panorama metadata");
        }
        return false;
    }
    return true;
}

bool validatePanorama(const SwatchTexture &texture, QString *error) {
    if (texture.platform != 0 || texture.arraySize != 1 || texture.sliceCount != 1
        || texture.mipCount != 1 || texture.width != texture.height * 2
        || texture.slices.size() != 1
        || texture.slices[0].encoding != SwatchEncoding::UnsignedBc6H
        || texture.slices[0].mipLevels.size() != 1) {
        if (error != nullptr) {
            *error = QStringLiteral("garage panorama has unsupported texture topology");
        }
        return false;
    }
    const quint64 blockWidth = static_cast<quint64>((texture.width + 3) / 4);
    const quint64 blockHeight = static_cast<quint64>((texture.height + 3) / 4);
    const quint64 expectedBytes = blockWidth * blockHeight * 16;
    if (static_cast<quint64>(texture.mipBytes(0, 0).size()) != expectedBytes) {
        if (error != nullptr) {
            *error = QStringLiteral("garage panorama BC6H payload size is invalid");
        }
        return false;
    }
    return true;
}

void loadPanorama(
    const QString &garageArchive, GarageEnvironmentResources *resources) {
    QString error;
    const QByteArray metadataBytes = readZipEntry(
        garageArchive, QStringLiteral("FrameData.xml"), &error);
    if (metadataBytes.isEmpty()) {
        resources->panoramaError = QStringLiteral("garage panorama metadata: %1").arg(error);
        return;
    }
    if (!parsePanoramaMetadata(
            metadataBytes, &resources->panorama, &resources->panoramaError)) {
        return;
    }
    error.clear();
    const QByteArray panoramaBytes = readZipEntry(
        garageArchive, QStringLiteral("SphericalFrames/0001.swatchbin"), &error);
    if (panoramaBytes.isEmpty()) {
        resources->panoramaError = QStringLiteral("garage panorama: %1").arg(error);
        return;
    }
    auto panorama = parseResource(
        panoramaBytes, QStringLiteral("SphericalFrames/0001.swatchbin"),
        &resources->panoramaError);
    if (!panorama || !validatePanorama(*panorama, &resources->panoramaError)) {
        return;
    }
    resources->panorama.texture = std::move(*panorama);
    resources->panoramaError.clear();
}

} // namespace

GarageEnvironmentResources loadGarageEnvironmentResources(const QString &gameFolder) {
    GarageEnvironmentResources resources;
    const QString mediaDir = gameMediaDir(gameFolder);
    if (mediaDir.isEmpty()) {
        resources.error = QStringLiteral("game folder is not configured");
        return resources;
    }

    const QString garageArchive = QDir(mediaDir).filePath(
        QStringLiteral("HDRSkies/Forte_Garage_01.zip"));
    loadPanorama(garageArchive, &resources);
    QString error;
    const QByteArray diffuseBytes = readZipEntry(
        garageArchive, QStringLiteral("Lighting/0001.swatchbin"), &error);
    if (diffuseBytes.isEmpty()) {
        resources.error = QStringLiteral("garage diffuse probe: %1").arg(error);
        return resources;
    }
    auto diffuse = parseResource(
        diffuseBytes, QStringLiteral("Lighting/0001.swatchbin"), &resources.error);
    if (!diffuse || !validateCubemap(
            *diffuse, SwatchEncoding::R16G16B16A16Float, 1,
            QStringLiteral("garage diffuse probe"), &resources.error)) {
        return resources;
    }

    const QString objectsArchive = QDir(mediaDir).filePath(
        QStringLiteral("_library/TexturesPG/objects.zip"));
    error.clear();
    const QByteArray specularBytes = readZipEntry(
        objectsArchive,
        QStringLiteral("swatches/CubemapProbeSpecular_Thumbnail.swatchbin"), &error);
    if (specularBytes.isEmpty()) {
        resources.error = QStringLiteral("garage specular probe: %1").arg(error);
        return resources;
    }
    auto specular = parseResource(
        specularBytes, QStringLiteral("CubemapProbeSpecular_Thumbnail.swatchbin"),
        &resources.error);
    if (!specular || !validateCubemap(
            *specular, SwatchEncoding::UnsignedBc6H, 10,
            QStringLiteral("garage specular probe"), &resources.error)) {
        return resources;
    }

    resources.diffuseCubemap = std::move(*diffuse);
    resources.specularCubemap = std::move(*specular);
    resources.error.clear();
    return resources;
}

} // namespace fh6
