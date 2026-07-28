#pragma once

#include "swatchbin.h"

#include <QString>

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
