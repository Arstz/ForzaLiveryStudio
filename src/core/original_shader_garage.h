#pragma once

#include "garage_environment.h"
#include "model_geometry.h"
#include "swatchbin.h"

#include <QByteArray>
#include <QString>

#include <array>
#include <memory>
#include <vector>

namespace fh6 {

enum class OriginalShaderSurfaceFamily {
    Default,
    Floor,
};

struct OriginalShaderProgram {
    QByteArray vertexShader;
    QByteArray pixelShader;

    bool valid() const {
        return vertexShader.startsWith("DXBC") && pixelShader.startsWith("DXBC");
    }
};

struct OriginalShaderMaterialTexture;

struct OriginalShaderGarageDraw {
    QString name;
    QString source;
    OriginalShaderSurfaceFamily family = OriginalShaderSurfaceFamily::Default;
    CarModel geometry;
    ModelMat4 placement;
    std::shared_ptr<const OriginalShaderMaterialTexture> diffuseTexture;
    int diffuseUvChannel = 0;

    bool valid() const {
        return !name.isEmpty() && !geometry.meshes.empty()
            && geometry.totalVertices() > 0 && geometry.totalIndices() > 0;
    }
};

struct OriginalShaderMaterialTexture {
    QString semantic;
    QString sourceEntry;
    SwatchImage image;

    bool valid() const {
        return !semantic.isEmpty() && !sourceEntry.isEmpty() && image.valid();
    }
};

struct OriginalShaderLighting {
    ModelVec3 direction;
    ModelVec3 directColor;
    ModelVec3 ambientColor;
    QString source;
};

struct OriginalShaderPointLight {
    ModelMat4 transform;
    quint32 presetHash = 0;
};

// Fully decoded, renderer-neutral input for the experimental original-DXIL
// garage path. The House 8 shell, car locator, and roof light transforms are
// exact; status fields record unresolved layout-catalog and shader limitations
// without substituting guessed game assets.
struct OriginalShaderGarageScene {
    std::vector<OriginalShaderGarageDraw> draws;
    OriginalShaderProgram defaultProgram;
    OriginalShaderProgram floorProgram;
    std::array<OriginalShaderMaterialTexture, 7> materialTextures;
    GarageEnvironmentResources environment;
    OriginalShaderLighting lighting;
    std::vector<OriginalShaderPointLight> authoredLights;
    ModelMat4 carPlacement;
    QString name;
    QString geometryStatus;
    QString materialStatus;
    QString lightingStatus;
    QString carStatus;
    QString glassStatus;
    QString error;

    bool valid() const;
    long long totalVertices() const;
    long long totalTriangles() const;
};

OriginalShaderGarageScene loadOriginalShaderGarageScene(const QString &gameFolder);

bool appendOriginalShaderGarageCar(
    OriginalShaderGarageScene *scene, CarModel car,
    const std::array<float, 3> &paintColor,
    const SwatchImage &livery = {}, QString *error = nullptr);

} // namespace fh6
