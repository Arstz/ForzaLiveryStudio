#pragma once

#include "swatchbin.h"

#include <QString>

namespace fh6 {

struct GarageEnvironmentResources {
    SwatchTexture diffuseCubemap;
    SwatchTexture specularCubemap;
    QString error;

    bool valid() const {
        return diffuseCubemap.valid() && specularCubemap.valid();
    }
};

GarageEnvironmentResources loadGarageEnvironmentResources(const QString &gameFolder);

} // namespace fh6
