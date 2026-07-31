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

// Fully decoded, renderer-neutral input for the experimental original-DXIL
// garage path. Tokyo House geometry is exact; materialStatus records the remaining
// shader/texture limitations without substituting guessed game assets.
struct OriginalShaderGarageScene {
    std::vector<OriginalShaderGarageDraw> draws;
    OriginalShaderProgram defaultProgram;
    OriginalShaderProgram floorProgram;
    std::array<OriginalShaderMaterialTexture, 7> materialTextures;
    GarageEnvironmentResources environment;
    QString name;
    QString geometryStatus;
    QString materialStatus;
    QString glassStatus;
    QString error;

    bool valid() const;
    long long totalVertices() const;
    long long totalTriangles() const;
};

OriginalShaderGarageScene loadOriginalShaderGarageScene(const QString &gameFolder);

} // namespace fh6
