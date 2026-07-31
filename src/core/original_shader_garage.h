#pragma once

#include "garage_environment.h"
#include "garage_lut.h"
#include "livery_masks.h"
#include "model_geometry.h"
#include "swatchbin.h"

#include <QByteArray>
#include <QString>

#include <array>
#include <memory>
#include <vector>

namespace fh6 {

struct LiveryPaintState;
struct ManufacturerColorPalette;
class PaintFinishLibrary;

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
    std::shared_ptr<const OriginalShaderMaterialTexture> alphaTexture;
    std::shared_ptr<const OriginalShaderMaterialTexture> normalTexture;
    std::shared_ptr<const OriginalShaderMaterialTexture> surfaceTexture;
    std::shared_ptr<const OriginalShaderMaterialTexture> emissiveTexture;
    int diffuseUvChannel = 0;
    int materialUvChannel = 0;
    float materialUvRotationDegrees = 0.0f;
    std::array<float, 3> baseColor = {0.55f, 0.55f, 0.55f};
    std::array<float, 3> emissiveColor = {0.0f, 0.0f, 0.0f};
    float opacity = 1.0f;
    float gloss = 0.45f;
    float metallic = 0.0f;
    float uTiling = 1.0f;
    float vTiling = 1.0f;
    float detailUTiling = 1.0f;
    float detailVTiling = 1.0f;
    std::array<float, 3> clearCoatTint = {1.0f, 1.0f, 1.0f};
    float clearCoatCoverage = 0.0f;
    float clearCoatRoughness = 0.1f;
    bool rawMaterialUv = false;
    bool translucent = false;
    bool hidden = false;
    bool clearCoatOnLivery = true;
    bool liveryBaseTexture = false;
    quint32 liveryAllowedSides = 0;

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

struct OriginalShaderLiveryMapping {
    std::array<OriginalShaderMaterialTexture, kLiverySideCount> masks;
    std::array<std::array<float, 4>, kLiverySideCount> sourceRegions{};
    std::array<std::array<float, 4>, kLiverySideCount> paintRegions{};
    std::array<std::array<float, 3>, kLiverySideCount> facing{};
    int sideCount = 0;

    bool valid() const {
        return sideCount == kLiverySideCount;
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
    ModelVec3 color = {1.0f, 1.0f, 1.0f};
    float range = 0.0f;
    float intensity = 0.0f;
    float penumbraAngleDegrees = 0.0f;
    float coneAngleDegrees = 0.0f;
    quint32 type = 0;
    bool enabled = false;
};

// Fully decoded, renderer-neutral input for the experimental original-DXIL
// garage path. The coordinate-matched garage_customiser enclosure is combined
// with the exact Default-House8 prop transforms; status fields expose unresolved
// material and lighting slots rather than silently substituting plausible game
// assets.
struct OriginalShaderGarageScene {
    std::vector<OriginalShaderGarageDraw> draws;
    OriginalShaderProgram defaultProgram;
    OriginalShaderProgram floorProgram;
    std::array<OriginalShaderMaterialTexture, 7> materialTextures;
    GarageEnvironmentResources environment;
    GarageColorLut colorLut;
    OriginalShaderLighting lighting;
    std::vector<OriginalShaderPointLight> authoredLights;
    OriginalShaderLiveryMapping liveryMapping;
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

OriginalShaderGarageScene loadOriginalShaderGarageScene(
    const QString &gameFolder, const SwatchImage &missingTexture = {});

OriginalShaderGarageScene loadCachedOriginalShaderGarageScene(
    const QString &gameFolder, const SwatchImage &missingTexture = {});

QString originalShaderGarageCachePath(const QString &gameFolder);

bool appendOriginalShaderGarageCar(
    OriginalShaderGarageScene *scene, CarModel car,
    const std::array<float, 3> &paintColor,
    const SwatchImage &livery = {},
    const LiveryMaskSet *liveryMasks = nullptr,
    const std::array<std::array<float, 4>, kLiverySideCount> *paintRegions = nullptr,
    const LiveryPaintState *paintState = nullptr,
    const ManufacturerColorPalette *manufacturerColors = nullptr,
    const PaintFinishLibrary *paintFinishes = nullptr,
    QString *error = nullptr);

} // namespace fh6
