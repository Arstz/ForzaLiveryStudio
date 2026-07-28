#include "garage_environment.h"

#include "game_paths.h"
#include "zip_extract.h"

#include <QDir>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <utility>

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

} // namespace

bool parseGaragePanoramaMetadata(
    const QByteArray &bytes, GaragePanoramaResources *panorama, QString *error) {
    if (panorama == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("garage panorama metadata destination is null");
        }
        return false;
    }

    GaragePanoramaResources parsed;
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
            parsed.sphericalMode = attributes.value(QStringLiteral("spherical")).toInt(&modeOk);
            parsed.sphericalPower = attributes.value(QStringLiteral("sphericalpower")).toFloat(&powerOk);
            parsed.rotation = attributes.value(QStringLiteral("rotation")).toFloat(&rotationOk);
            if (!modeOk || !powerOk || !rotationOk) {
                if (error != nullptr) {
                    *error = QStringLiteral("FrameData.xml has invalid spherical metadata");
                }
                return false;
            }
            foundRoot = true;
        } else if (foundRoot && !foundFrame && xml.name() == QStringLiteral("Frame")) {
            bool scaleOk = false;
            parsed.frameScale = xml.attributes().value(QStringLiteral("scale")).toFloat(&scaleOk);
            if (!scaleOk) {
                if (error != nullptr) {
                    *error = QStringLiteral("FrameData.xml has an invalid frame scale");
                }
                return false;
            }
            foundFrame = true;
        }
    }
    if (xml.hasError() || !foundRoot || !foundFrame
        || !std::isfinite(parsed.sphericalPower)
        || !std::isfinite(parsed.rotation)
        || !std::isfinite(parsed.frameScale)
        || parsed.sphericalMode != 2
        || parsed.sphericalPower < 0.0f || parsed.sphericalPower > 1.0f
        || parsed.rotation != 0.0f || parsed.frameScale <= 0.0f) {
        if (error != nullptr) {
            *error = xml.hasError()
                ? QStringLiteral("FrameData.xml: %1").arg(xml.errorString())
                : QStringLiteral("FrameData.xml has unsupported garage panorama metadata");
        }
        return false;
    }
    parsed.texture = std::move(panorama->texture);
    *panorama = std::move(parsed);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool validateGaragePanoramaTexture(const SwatchTexture &texture, QString *error) {
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
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::array<float, 2> garagePanoramaUv(
    const std::array<float, 3> &direction, float sphericalPower) {
    constexpr float pi = 3.14159265358979323846f;
    const float length = std::sqrt(
        direction[0] * direction[0]
        + direction[1] * direction[1]
        + direction[2] * direction[2]);
    if (length <= 0.0f) {
        return {0.25f, 0.5f};
    }

    const float x = direction[0] / length;
    const float y = direction[1] / length;
    const float z = direction[2] / length;
    const float theta = std::acos(std::clamp(std::abs(y), 0.0f, 1.0f));
    const float polar = theta * (2.0f / pi);
    const float power = std::clamp(sphericalPower, 0.0f, 1.0f);
    const float radius = theta * (1.0f / pi)
        * ((1.0f - power) + polar * power);
    const float horizontalLength = std::sqrt(x * x + z * z);
    const float diskX = horizontalLength > 0.000001f
        ? z * radius / horizontalLength
        : 0.0f;
    const float diskY = horizontalLength > 0.000001f
        ? -x * radius / horizontalLength
        : 0.0f;
    float u = (diskX + 0.5f) * 0.5f;
    float v = diskY + 0.5f;
    if (y < 0.0f) {
        u += 0.5f;
        v = 1.0f - v;
    }
    return {u, v};
}

namespace {

void loadPanorama(
    const QString &garageArchive, GarageEnvironmentResources *resources) {
    QString error;
    const QByteArray metadataBytes = readZipEntry(
        garageArchive, QStringLiteral("FrameData.xml"), &error);
    if (metadataBytes.isEmpty()) {
        resources->panoramaError = QStringLiteral("garage panorama metadata: %1").arg(error);
        return;
    }
    if (!parseGaragePanoramaMetadata(
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
    if (!panorama
        || !validateGaragePanoramaTexture(*panorama, &resources->panoramaError)) {
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
