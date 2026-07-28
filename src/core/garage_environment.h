#pragma once

#include "swatchbin.h"

#include <QByteArray>
#include <QString>

#include <array>

namespace fh6 {

struct GaragePanoramaResources {
    SwatchTexture texture;
    int sphericalMode = 0;
    float sphericalPower = 0.0f;
    float rotation = 0.0f;
    float frameScale = 1.0f;

    bool valid() const {
        return texture.valid() && sphericalMode == 2 && frameScale > 0.0f;
    }
};

bool parseGaragePanoramaMetadata(
    const QByteArray &bytes, GaragePanoramaResources *panorama,
    QString *error = nullptr);

bool validateGaragePanoramaTexture(
    const SwatchTexture &texture, QString *error = nullptr);

std::array<float, 2> garagePanoramaUv(
    const std::array<float, 3> &direction, float sphericalPower);

struct GarageEnvironmentResources {
    GaragePanoramaResources panorama;
    SwatchTexture diffuseCubemap;
    SwatchTexture specularCubemap;
    QString panoramaError;
    QString error;

    bool valid() const {
        return diffuseCubemap.valid() && specularCubemap.valid();
    }
};

GarageEnvironmentResources loadGarageEnvironmentResources(const QString &gameFolder);

} // namespace fh6
