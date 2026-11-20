#pragma once

#include "model_bundle.h"
#include "swatchbin.h"

#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace fls {

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
    PackedVector = 12,
};

struct ModelMaterialParameter {
    QUuid parameterId;
    std::optional<quint32> aliasHash;
    quint8 versionMajor = 0;
    quint8 versionMinor = 0;
    quint32 nameHash = 0;
    ModelMaterialParameterType type = ModelMaterialParameterType::Float;
    std::array<float, 4> vector = {0.0f, 0.0f, 0.0f, 0.0f};
    float scalar = 0.0f;
    qint32 integer = 0;
    bool boolean = false;
    bool shaderDefault = false;
    QString texturePath;
    quint32 texturePathHash = 0;
    qint32 samplerAddressU = 0;
    qint32 samplerAddressV = 0;
    qint32 samplerFilter = 1;
    QVector<std::array<float, 4>> gradient;
};

struct MaterialUvMapping {
    std::array<float, 2> tiling = {1.0f, 1.0f};
    int channel = 0;
    float rotationDegrees = 0.0f;
    bool tileBeforeRotation = false;
};

struct MaterialOpacity {
    std::optional<float> primary;
    std::optional<float> secondary;
    std::optional<bool> useAlphaTexture;
    bool dualOpacity = false;
    bool interpolateAlphaTexture = false;
    float maskMin = 0.0f;
    float maskMax = 1.0f;
};

enum class LegacyMetalBlendMode : int {
    Constant = 1,
    SingleMask = 2,
    SeparateMasks = 3,
};

struct LegacyMetalBlend {
    MaterialUvMapping maskUv;
    std::array<float, 3> unmaskedReflectance{};
    std::array<float, 3> maskedReflectance{};
    std::array<float, 3> outerReflectance{};
    LegacyMetalBlendMode mode = LegacyMetalBlendMode::Constant;
    float outerGloss = 1.0f;
    bool opacityFromNormalAlpha = false;
};

struct LegacyMaterialSurface {
    std::optional<LegacyMetalBlend> metalBlend;
    MaterialUvMapping glossUv;
    std::array<float, 2> glossRange = {0.0f, 1.0f};
    quint32 diffuseMap = 0;
    quint32 normalMap = 0;
    quint32 opacityMap = 0;
    quint32 glossMap = 0;
    quint32 metalBlendMaskMap = 0;
    quint32 emissionMap = 0;
    quint32 emissionMaskMap = 0;
    bool diffuseAlpha = false;
};

struct MaterialWeave {
    std::array<float, 3> tintA{};
    std::array<float, 3> tintB{};
    bool maskedLivery = false;
};

struct ShaderTextureBinding {
    QUuid parameterId;
    quint32 nameHash = 0;
    quint16 shaderRegister = 0;
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
    std::vector<ShaderTextureBinding> shaderTextures;
    std::optional<bool> legacyShaderBindings;
    MaterialOpacity glassOpacity;
    std::optional<LegacyMaterialSurface> legacySurface;
    std::optional<MaterialWeave> weave;
    std::optional<MaterialUvMapping> diffuseUv;
    std::optional<MaterialUvMapping> normalUv;
    std::optional<MaterialUvMapping> surfaceUv;
    std::optional<MaterialUvMapping> alphaUv;
    std::optional<MaterialUvMapping> emissiveUv;
    std::optional<MaterialUvMapping> emissiveMaskUv;
    std::optional<MaterialUvMapping> glassTintUv;
    std::optional<bool> useDiffuseTexture;
    std::optional<bool> emissionEnabled;
    std::optional<bool> emissionMaskEnabled;
    bool glass = false;
    bool usesTintedEmission = false;
    bool useGlassTintMask = false;
    bool useGlassTintForOpacity = false;
    bool hasBaseColor = false;
    std::array<float, 3> baseColor = {0.55f, 0.55f, 0.55f};
    std::array<float, 3> emissiveColor = {0.0f, 0.0f, 0.0f};
    float emissiveIntensity = 0.0f;
    float opacity = 1.0f;
    float gloss = 0.45f;
    float uTiling = 1.0f;
    float vTiling = 1.0f;
    float uvOrientationDegrees = 0.0f;
    bool resolvedFromResource = false;
    bool resolvedFromShader = false;
    bool overriddenByScene = false;
    bool hasMetallic = false;
    float metallic = 0.0f;
    float flakeAmount = 0.0f;
    float normalIntensity = 1.0f;
    float roughnessShift = 0.0f;
    float glassTintMultiplier = 1.0f;
    int materialUvChannel = -1;
    QString patternTexture;      // BaseColorAlpha colour/pattern swatch
    QString detailNormalTexture; // weave / brushed / flake normal swatch
    QString roughMetalAoTexture; // packed roughness/metal/AO swatch
    std::shared_ptr<const ModelMaterialTexture> diffuseTexture;
    std::shared_ptr<const ModelMaterialTexture> alphaTexture;
    std::shared_ptr<const ModelMaterialTexture> normalTexture;
    std::shared_ptr<const ModelMaterialTexture> tireHeightTexture;
    std::shared_ptr<const ModelMaterialTexture> surfaceTexture;
    std::shared_ptr<const ModelMaterialTexture> glossTexture;
    std::shared_ptr<const ModelMaterialTexture> metalBlendMaskTexture;
    std::shared_ptr<const ModelMaterialTexture> emissiveTexture;
    std::shared_ptr<const ModelMaterialTexture> emissiveMaskTexture;
    std::shared_ptr<const ModelMaterialTexture> glassTintTexture;
    std::shared_ptr<const ModelMaterialTexture> weaveMaskTexture;
};

std::shared_ptr<ModelMaterial> decodeModelMaterial(const BundleBlobRecord &blob);

std::shared_ptr<ModelMaterial> decodeMaterialBundle(const QByteArray &bytes);

std::shared_ptr<ModelMaterial> mergeModelMaterialDefaults(
    const ModelMaterial &defaults, const ModelMaterial &instance);

std::optional<quint32> shaderTextureParameter(
    const ModelMaterial &material, const ModelMaterialParameter &parameter);

float lampGlassPreviewOpacity(const ModelMaterial *material, bool colored);

} // namespace fls
