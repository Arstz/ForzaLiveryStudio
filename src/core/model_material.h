#pragma once

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
    std::shared_ptr<const ModelMaterialTexture> diffuseTexture;
    std::shared_ptr<const ModelMaterialTexture> alphaTexture;
    std::shared_ptr<const ModelMaterialTexture> normalTexture;
    std::shared_ptr<const ModelMaterialTexture> surfaceTexture;
    std::shared_ptr<const ModelMaterialTexture> emissiveTexture;
};

std::shared_ptr<ModelMaterial> decodeModelMaterial(const BundleBlobRecord &blob);

std::shared_ptr<ModelMaterial> decodeMaterialBundle(const QByteArray &bytes);

std::shared_ptr<ModelMaterial> mergeModelMaterialDefaults(
    const ModelMaterial &defaults, const ModelMaterial &instance);

} // namespace fh6
