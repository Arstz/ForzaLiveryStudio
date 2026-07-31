#pragma once

#include "automotive_paint.h"
#include "model_bundle.h"
#include "swatchbin.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <memory>
#include <vector>

namespace fh6 {

enum class ModelMaterialParameterType : quint8 {
    Vector = 0,
    Color = 1,
    Float = 2,
    Bool = 3,
    Int = 4,
    Swizzle = 5,
    Texture2D = 6,
    Sampler = 7,
    ColorGradient = 8,
    FunctionRange = 9,
    Vector2 = 11,
};

struct ModelMaterialParameter {
    quint8 versionMajor = 0;
    quint8 versionMinor = 0;
    quint32 nameHash = 0;
    ModelMaterialParameterType type = ModelMaterialParameterType::Float;
    std::array<float, 4> vector = {0.0f, 0.0f, 0.0f, 0.0f};
    float scalar = 0.0f;
    qint32 integer = 0;
    bool boolean = false;
    QString texturePath;
    quint32 texturePathHash = 0;
    qint32 samplerAddressU = 0;
    qint32 samplerAddressV = 0;
    qint32 samplerFilter = 1;
    QVector<std::array<float, 4>> gradient;
};

struct ModelMaterialTexture {
    QString path;
    SwatchImage image;
    std::vector<SwatchImage> authoredMips;
};

struct ModelMaterialSampler {
    bool authored = false;
    qint32 addressU = 1;
    qint32 addressV = 1;
    qint32 filter = 1;
};

enum class ModelShaderFamily {
    Generic,
    AutomotivePaint,
    CarbonFiber,
    Glass,
    Emissive,
};

struct ModelMaterial {
    QString name;
    QString resourcePath;
    QStringList linkedPaths;
    std::vector<ModelMaterialParameter> parameters;
    bool hasBaseColor = false;
    std::array<float, 3> baseColor = {0.55f, 0.55f, 0.55f};
    std::array<float, 3> emissiveColor = {0.0f, 0.0f, 0.0f};
    float emissiveIntensity = 0.0f;
    float opacity = 1.0f;
    float gloss = 0.45f;
    float uTiling = 1.0f;
    float vTiling = 1.0f;
    float uvOrientationDegrees = 0.0f;
    float normalIntensity = 1.0f;
    float weaveNormalIntensity = 1.0f;
    float clearCoatNormalUTiling = 1.0f;
    float clearCoatNormalVTiling = 1.0f;
    std::array<float, 3> weaveColorTintA = {0.035f, 0.04f, 0.045f};
    std::array<float, 3> weaveColorTintB = {0.09f, 0.10f, 0.11f};
    ModelMaterialSampler sampler;
    bool resolvedFromLibrary = false; // params merged from the shared _library material
    bool hasMetallic = false;
    float metallic = 0.0f;
    float flakeAmount = 0.0f;
    AutomotivePaintParameters automotivePaint;
    QString patternTexture;      // BaseColorAlpha colour/pattern swatch
    QString detailNormalTexture; // weave / brushed / flake normal swatch
    QString roughMetalAoTexture; // packed roughness/metal/AO swatch
    std::shared_ptr<const ModelMaterialTexture> diffuseTexture;
    std::shared_ptr<const ModelMaterialTexture> alphaTexture;
    std::shared_ptr<const ModelMaterialTexture> normalTexture;
    std::shared_ptr<const ModelMaterialTexture> weaveMaskTexture;
    std::shared_ptr<const ModelMaterialTexture> weaveNormalTexture;
    std::shared_ptr<const ModelMaterialTexture> clearCoatNormalTexture;
    std::shared_ptr<const ModelMaterialTexture> surfaceTexture;
    std::shared_ptr<const ModelMaterialTexture> emissiveTexture;
    std::shared_ptr<const ModelMaterialTexture> paintNormalMap00Texture;
    std::shared_ptr<const ModelMaterialTexture> paintNormalMap0Texture;
    std::shared_ptr<const ModelMaterialTexture> orangePeelNormalTexture;
};

std::shared_ptr<ModelMaterial> decodeModelMaterial(const BundleBlobRecord &blob);

std::shared_ptr<ModelMaterial> decodeMaterialBundle(const QByteArray &bytes);

std::shared_ptr<ModelMaterial> mergeModelMaterialDefaults(
    const ModelMaterial &defaults, const ModelMaterial &instance);

ModelShaderFamily modelShaderFamily(const ModelMaterial &material);

} // namespace fh6
