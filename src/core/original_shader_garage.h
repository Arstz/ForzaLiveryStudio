#pragma once

#include "garage_environment.h"
#include "model_geometry.h"
#include "swatchbin.h"

#include <QByteArray>
#include <QString>

#include <array>
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

struct OriginalShaderGarageDraw {
    QString name;
    QString source;
    OriginalShaderSurfaceFamily family = OriginalShaderSurfaceFamily::Default;
    CarModel geometry;

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
// homespace path. It deliberately excludes glass until its material instance can be
// mapped to one exact shader family without guessing.
struct OriginalShaderGarageScene {
    std::vector<OriginalShaderGarageDraw> draws;
    OriginalShaderProgram defaultProgram;
    OriginalShaderProgram floorProgram;
    std::array<OriginalShaderMaterialTexture, 7> materialTextures;
    GarageEnvironmentResources environment;
    QString glassStatus;
    QString error;

    bool valid() const;
    long long totalVertices() const;
    long long totalTriangles() const;
};

OriginalShaderGarageScene loadOriginalShaderGarageScene(const QString &gameFolder);

} // namespace fh6
