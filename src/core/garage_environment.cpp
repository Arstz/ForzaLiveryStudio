#include "garage_environment.h"

#include "game_paths.h"
#include "zip_extract.h"

#include <QDir>

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

GarageEnvironmentResources loadGarageEnvironmentResources(const QString &gameFolder) {
    GarageEnvironmentResources resources;
    const QString mediaDir = gameMediaDir(gameFolder);
    if (mediaDir.isEmpty()) {
        resources.error = QStringLiteral("game folder is not configured");
        return resources;
    }

    const QString garageArchive = QDir(mediaDir).filePath(
        QStringLiteral("HDRSkies/Forte_Garage_01.zip"));
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
