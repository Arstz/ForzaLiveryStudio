#include "car_scene.h"
#include "game_paths.h"
#include "material_hashes.h"
#include "model_material.h"
#include "paint_finish_catalog.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void checkSamplerDefaults() {
    QByteArray parameters;
    const auto u32 = [&](quint32 value) {
        const int offset = parameters.size();
        parameters.resize(offset + 4);
        qToLittleEndian(value, parameters.data() + offset);
    };
    const auto parameter = [&](quint32 hash, quint8 type) {
        parameters.append('\x03').append('\x04');
        u32(hash);
        parameters.append('\0').append(static_cast<char>(type)).append(QByteArray(16, '\0'));
    };
    parameters.append('\x03').append('\0');
    parameter(123, 7);
    for (quint32 value : {4u, 4u, 3u, 287454020u}) u32(value);
    parameter(456, 12);
    for (quint32 value : {0x3f800000u, 0x3f800000u, 0u, 0u}) u32(value);
    parameter(fls::material_hashes::parameter::kTextureTilingU, 2);
    const float tiling = 32.0f;
    quint32 bits;
    std::memcpy(&bits, &tiling, sizeof(bits));
    u32(bits);
    QByteArray bundle(44, '\0');
    const auto field = [&](int offset, quint32 value) { qToLittleEndian(value, bundle.data() + offset); };
    field(0, fls::bundle_tags::Bundle);
    bundle[4] = bundle[5] = 1;
    field(16, 1);
    field(20, fls::bundle_tags::DefaultMaterialParameters);
    bundle[24] = 2;
    bundle[25] = 1;
    field(32, bundle.size());
    field(36, parameters.size());
    field(40, parameters.size());
    bundle.append(parameters);
    const auto material = fls::decodeMaterialBundle(bundle);
    require(material && material->parameters.size() == 3 && material->uTiling == tiling,
            "shader parameter payload misaligned the following parameter");
    require(material->parameters[1].type == fls::ModelMaterialParameterType::PackedVector
            && material->parameters[1].vector == std::array<float, 4>{1, 1, 0, 0},
            "packed shader vector lost its components");
}

void checkLampGlassOpacityFallback() {
    fls::ModelMaterial material;
    fls::ModelMaterialParameter parameter;
    parameter.nameHash = fls::material_hashes::parameter::kAlphaTexture[1];
    parameter.type = fls::ModelMaterialParameterType::Texture2D;
    parameter.texturePath = QStringLiteral("shared/diffuser.swatchbin");
    material.parameters.push_back(parameter);
    require(fls::lampGlassPreviewOpacity(&material, false) == 0.50f,
            "a masked lens lost its opacity fallback without the shared resources");
    material.glassOpacity.primary = 0.12f;
    require(fls::lampGlassPreviewOpacity(&material, false) == 0.12f,
            "lamp fallback replaced authored opacity");
    material.glassOpacity.primary.reset();
    material.resolvedFromResource = true;
    require(fls::lampGlassPreviewOpacity(&material, false) == 0.20f,
            "missing-resource fallback changed a resolved material");
    material.resolvedFromResource = false;
    material.glassOpacity.useAlphaTexture = false;
    require(fls::lampGlassPreviewOpacity(&material, false) == 0.20f,
            "disabled opacity map changed the clear-lens fallback");
}

void checkLegacyShaderMappings() {
    using Type = fls::ModelMaterialParameterType;
    const auto shader = [](std::optional<quint8> version) {
        using namespace fls::bundle_tags;
        const int headerSize = version ? 92 : 68;
        QByteArray bytes(headerSize + (version ? 10 : 6), '\0');
        const auto field = [&](int offset, quint32 value) { qToLittleEndian(value, bytes.data() + offset); };
        field(0, Bundle);
        bytes[4] = bytes[5] = 1;
        field(16, version ? 3 : 2);
        field(20, TextureParameterMapping);
        bytes[24] = 3;
        bytes[25] = 1;
        field(32, headerSize);
        field(36, 2);
        field(40, 2);
        field(44, ShaderVariants);
        bytes[48] = 1;
        bytes[49] = 3;
        field(56, headerSize + 2);
        field(60, 4);
        field(64, 4);
        if (version) {
            field(68, ShaderVersion);
            bytes[72] = 1;
            bytes[73] = *version;
            field(80, headerSize + 6);
            field(84, 4);
            field(88, 4);
        }

        return fls::decodeMaterialBundle(bytes);
    };
    fls::ModelMaterial defaults;
    fls::ModelMaterial instance;
    const auto flag = [](quint32 hash, bool enabled) {
        fls::ModelMaterialParameter parameter;
        parameter.nameHash = hash;
        parameter.type = Type::Bool;
        parameter.boolean = enabled;
        return parameter;
    };
    const auto number = [](quint32 hash, float value) {
        fls::ModelMaterialParameter parameter;
        parameter.nameHash = hash;
        parameter.type = Type::Float;
        parameter.scalar = value;
        return parameter;
    };
    const auto vector = [](quint32 hash, Type type, std::array<float, 4> value) {
        fls::ModelMaterialParameter parameter;
        parameter.nameHash = hash;
        parameter.type = type;
        parameter.vector = value;
        return parameter;
    };
    defaults.linkedPaths = {QStringLiteral("clc_metalch2normalch1mask.shaderbin")};
    defaults.parameters = {flag(0x05A401E7, true), vector(0x31048129, Type::Vector2, {3, 4, 0, 0})};
    auto material = fls::mergeModelMaterialDefaults(*shader(1), defaults);
    require(material->legacySurface && material->legacySurface->diffuseMap == 0xCA4E3249
            && material->materialUvChannel == 1 && material->uTiling == 3.0f && material->vTiling == 4.0f
            && !material->legacySurface->diffuseAlpha && material->baseColor == std::array<float, 3>{1, 1, 1},
            "legacy colour swatch lost its shader binding or UV mapping");
    require(!fls::mergeModelMaterialDefaults(*shader(2), defaults)->legacySurface,
            "legacy mappings were applied to a newer shader with the same name");

    defaults.linkedPaths = {QStringLiteral("m_ch2normglossalphaemissive.shaderbin")};
    defaults.parameters = {
        flag(0xFA9429D7, true), flag(0xF5A4EEA0, true), flag(0x9FC7B8A8, true),
        vector(0x942CA044, Type::Vector2, {2, 3, 0, 0}),
        vector(0x1F9B6488, Type::Vector, {0.625f, 0.425f, 0.21f, 1.0f}),
        number(0xB0B8947E, 5), number(0xCCD9B1A5, 7),
        number(0xEEBAB966, -0.5f), number(0xB9DE26A0, 0.25f), number(0x52E99DA3, 0.75f)};
    instance.parameters = {flag(0x9FC7B8A8, false), number(0xB0B8947E, 4)};
    material = fls::mergeModelMaterialDefaults(
        *shader(std::nullopt), *fls::mergeModelMaterialDefaults(defaults, instance));
    require(material->normalUv && material->normalUv->channel == 1
            && material->normalUv->tiling == std::array<float, 2>{8, 21}
            && material->normalIntensity == -0.5f,
            "legacy normal map lost its signed strength or composed tiling");
    require(material->legacySurface && material->legacySurface->opacityMap == 0
            && material->legacySurface->diffuseMap == 0 && material->legacySurface->emissionMaskMap == 0
            && material->emissionEnabled == false
            && material->legacySurface->glossMap == 0xA91F5977 && material->legacySurface->glossUv.channel == 1
            && material->legacySurface->glossRange == std::array<float, 2>{0.25f, 0.75f}
            && material->metallic == 1.0f && material->baseColor == std::array<float, 3>{0.625f, 0.425f, 0.21f},
            "legacy metal lost its authored opacity switch, gloss range, or reflectance");
    instance.parameters.push_back(flag(0x6F80D6E1, true));
    instance.parameters.push_back(flag(0x0BF3318B, true));
    instance.parameters.push_back(vector(0x9BCED46E, Type::Color, {0.25f, 0.5f, 0.75f, 1.0f}));
    instance.parameters.push_back(number(0x6F45C5F1, 0.5f));
    instance.parameters.push_back(number(0x1364DE85, 4.0f));
    material = fls::mergeModelMaterialDefaults(
        *shader(std::nullopt), *fls::mergeModelMaterialDefaults(defaults, instance));
    require(material->legacySurface && material->legacySurface->diffuseMap == 0x4C96A196
            && material->materialUvChannel == 1 && material->uTiling == 4.0f && material->vTiling == 7.0f
            && material->legacySurface->emissionMaskMap == 0x44EC5833 && material->emissionEnabled == true
            && material->emissiveMaskUv && material->emissiveMaskUv->channel == 1
            && material->emissiveMaskUv->tiling == std::array<float, 2>{4.0f, 7.0f}
            && material->emissiveColor == std::array<float, 3>{0.25f, 0.5f, 0.75f}
            && material->emissiveIntensity == 2.0f,
            "legacy metal ignored its enabled specular or emission map");

    defaults.linkedPaths = {QStringLiteral("m_ch1normglossalphaemissive.shaderbin")};
    defaults.parameters = {
        flag(0x553D641D, true), flag(0xA6BF15E8, true), flag(0x0BF3318B, false),
        vector(0x942CA044, Type::Vector2, {2, 3, 0, 0}),
        vector(0x4F90F34B, Type::Vector2, {4, 5, 0, 0}),
        vector(0x1F9B6488, Type::Vector, {0.6f, 0.5f, 0.4f, 1.0f})};
    material = fls::mergeModelMaterialDefaults(*shader(std::nullopt), defaults);
    require(material->legacySurface && material->legacySurface->diffuseMap == 0
            && material->legacySurface->emissionMap == 0 && material->legacySurface->emissionMaskMap == 0
            && material->emissionEnabled == false && material->legacySurface->opacityMap == 0x698CA64F
            && material->alphaUv && material->alphaUv->channel == 0
            && material->alphaUv->tiling == std::array<float, 2>{4, 5}
            && material->normalUv && material->normalUv->channel == 0
            && material->normalUv->tiling == std::array<float, 2>{2, 3},
            "first-channel metal lost its authored opacity, normal, or emission switch");

    defaults.linkedPaths = {QStringLiteral("badge_clc_diffusemetallerpch1masknorm.shaderbin")};
    defaults.parameters = {
        flag(0x05A401E7, true), flag(0x08B2C17F, true), flag(0xCBB3D988, true),
        vector(0x63040D89, Type::Color, {0.1f, 0.2f, 0.3f, 1.0f}),
        vector(0x1F9B6488, Type::Vector, {0.6f, 0.5f, 0.4f, 1.0f}),
        vector(0x9114636B, Type::Vector, {0.04f, 0.05f, 0.06f, 1.0f}),
        vector(0x2BC9275E, Type::Vector2, {2, 4, 0, 0}), vector(0x942CA044, Type::Vector2, {3, 5, 0, 0}),
        number(0x52E99DA3, 0.9f), number(0xB9DE26A0, 0.7f), number(0xEEBAB966, 1.3f)};
    material = fls::mergeModelMaterialDefaults(*shader(std::nullopt), defaults);
    require(material->legacySurface && material->legacySurface->metalBlend
            && material->useDiffuseTexture == true && material->legacySurface->diffuseMap == 0x810E7224
            && material->diffuseUv && material->diffuseUv->channel == 0
            && material->legacySurface->normalMap == 0xF9E8078D && material->normalUv
            && material->normalUv->channel == 0 && material->normalUv->tiling == std::array<float, 2>{3, 5}
            && material->legacySurface->metalBlend->mode == fls::LegacyMetalBlendMode::SingleMask
            && material->legacySurface->metalBlend->opacityFromNormalAlpha && !material->legacySurface->diffuseAlpha
            && material->legacySurface->metalBlend->unmaskedReflectance == std::array<float, 3>{0.6f, 0.5f, 0.4f}
            && material->legacySurface->metalBlend->maskedReflectance == std::array<float, 3>{0.04f, 0.05f, 0.06f}
            && material->legacySurface->glossRange == std::array<float, 2>{0.9f, 0.7f},
            "mixed-metal badge lost its artwork, material mask, or opacity from the normal map");
    instance.parameters = {flag(0x05A401E7, false)};
    material = fls::mergeModelMaterialDefaults(
        *shader(std::nullopt), *fls::mergeModelMaterialDefaults(defaults, instance));
    require(material->legacySurface && material->useDiffuseTexture == false && material->legacySurface->diffuseMap == 0
            && material->legacySurface->glossMap == 0x04351B70
            && material->legacySurface->glossUv.channel == 0
            && material->legacySurface->glossUv.tiling == std::array<float, 2>{2, 4}
            && material->baseColor == std::array<float, 3>{0.1f, 0.2f, 0.3f},
            "mixed-metal badge lost its constant-color and separate mask mapping");

    defaults.linkedPaths = {QStringLiteral("badge_clc_2metaldiffuselerpch1.shaderbin")};
    defaults.parameters.push_back(vector(0xFBC2185F, Type::Vector2, {6, 8, 0, 0}));
    defaults.parameters.push_back(vector(0xA4C801A3, Type::Vector, {1.0f, 0.75f, 0.30f, 1.0f}));
    defaults.parameters.push_back(number(0x561C4D9E, 0.55f));
    material = fls::mergeModelMaterialDefaults(*shader(std::nullopt), defaults);
    require(material->legacySurface && material->legacySurface->metalBlend
            && material->legacySurface->metalBlend->mode == fls::LegacyMetalBlendMode::SeparateMasks
            && material->useDiffuseTexture == true && material->legacySurface->diffuseMap == 0x810E7224
            && material->legacySurface->metalBlendMaskMap == 0x04351B70
            && material->legacySurface->metalBlend->maskUv.channel == 0
            && material->legacySurface->metalBlend->maskUv.tiling == std::array<float, 2>{2, 4}
            && material->legacySurface->glossMap == 0x73E220D9
            && material->legacySurface->glossUv.channel == 0
            && material->legacySurface->glossUv.tiling == std::array<float, 2>{6, 8}
            && material->legacySurface->metalBlend->outerReflectance == std::array<float, 3>{1.0f, 0.75f, 0.30f}
            && material->legacySurface->metalBlend->outerGloss == 0.55f
            && material->legacySurface->metalBlend->opacityFromNormalAlpha,
            "separate-mask badge lost one of its texture bindings or material layers");

    defaults.linkedPaths = {QStringLiteral("badge_ch1difnormglossao.shaderbin")};
    defaults.parameters = {
        flag(0x04F8F9FA, true), flag(0x71263EB7, true), flag(0x5A0DA36A, true), flag(0xA6BF15E8, true),
        vector(0x7590AB0C, Type::Vector2, {2, 3, 0, 0}), vector(0x942CA044, Type::Vector2, {4, 5, 0, 0}),
        vector(0x66036634, Type::Vector2, {6, 7, 0, 0}), vector(0x3A458AF9, Type::Vector2, {8, 9, 0, 0}),
        vector(0x679308CE, Type::Vector2, {10, 11, 0, 0}),
        vector(0x1F9B6488, Type::Color, {0.25f, 0.5f, 0.75f, 1.0f}),
        vector(0x0E29312A, Type::Color, {0.5f, 0.5f, 1.0f, 1.0f}),
        number(0xB9DE26A0, 0.5f), number(0x52E99DA3, 0.9f)};
    material = fls::mergeModelMaterialDefaults(*shader(std::nullopt), defaults);
    require(material->legacySurface && material->legacySurface->metalBlend
            && material->legacySurface->metalBlend->mode == fls::LegacyMetalBlendMode::GlossReflectance
            && material->useDiffuseTexture == true && material->legacySurface->diffuseMap == 0x810E7224
            && material->diffuseUv && material->diffuseUv->channel == 0
            && material->diffuseUv->tiling == std::array<float, 2>{2, 3}
            && material->legacySurface->glossMap == 0 && material->legacySurface->glossRange == std::array<float, 2>{0.5f, 0.9f}
            && material->legacySurface->metalBlend->usesReflectanceMask
            && material->legacySurface->metalBlend->maskedReflectance == std::array<float, 3>{0.125f, 0.25f, 0.75f}
            && material->legacySurface->metalBlend->opacityFromNormalAlpha && !material->legacySurface->diffuseAlpha
            && material->normalUv && material->normalUv->tiling == std::array<float, 2>{4, 5},
            "first-channel badge lost its artwork, gloss, reflectance mask, or packed opacity");
    instance.parameters = {flag(0x04F8F9FA, false), flag(0x0F70E9CC, true), flag(0xFE5BAD37, true)};
    material = fls::mergeModelMaterialDefaults(
        *shader(std::nullopt), *fls::mergeModelMaterialDefaults(defaults, instance));
    require(material->legacySurface && material->legacySurface->metalBlend
            && material->useDiffuseTexture == false && material->legacySurface->diffuseMap == 0
            && material->legacySurface->glossMap == 0xA91F5977
            && material->legacySurface->glossUv.tiling == std::array<float, 2>{6, 7}
            && material->legacySurface->metalBlendMaskMap == 0x4C96A196
            && material->legacySurface->metalBlend->maskUv.tiling == std::array<float, 2>{8, 9}
            && material->legacySurface->ambientOcclusionMap == 0xF476D21B && material->surfaceUv
            && !material->legacySurface->metalBlend->usesAmbientOcclusion
            && material->legacySurface->metalBlend->usesSpecularOcclusion
            && material->surfaceUv->tiling == std::array<float, 2>{10, 11},
            "first-channel badge lost its separate gloss, reflectance, or occlusion map");
    instance.parameters = {flag(0x04F8F9FA, false), flag(0xE876DDCC, true)};
    material = fls::mergeModelMaterialDefaults(
        *shader(std::nullopt), *fls::mergeModelMaterialDefaults(defaults, instance));
    require(material->legacySurface && material->legacySurface->metalBlend
            && material->legacySurface->metalBlend->usesAmbientOcclusion
            && !material->legacySurface->metalBlend->usesSpecularOcclusion
            && material->legacySurface->ambientOcclusionMap == 0xF476D21B,
            "badge ambient occlusion incorrectly enabled specular occlusion");

    defaults.linkedPaths = {QStringLiteral("badge_simplemetal.shaderbin")};
    defaults.parameters = {
        flag(0x989B026F, true), flag(0xA6BF15E8, true), number(0x52E99DA3, 0.8f),
        vector(0x942CA044, Type::Vector2, {2, 4, 0, 0}),
        vector(0x1F9B6488, Type::Color, {0.4f, 0.5f, 0.6f, 1.0f})};
    material = fls::mergeModelMaterialDefaults(*shader(std::nullopt), defaults);
    require(material->legacySurface && material->legacySurface->metalBlend
            && material->legacySurface->metalBlend->opacityFromNormalAlpha
            && material->legacySurface->metalBlend->maskedReflectance == std::array<float, 3>{0.4f, 0.5f, 0.6f}
            && material->legacySurface->normalMap == 0xF9E8078D && material->normalUv
            && material->normalUv->tiling == std::array<float, 2>{2, 4}
            && material->legacySurface->glossRange == std::array<float, 2>{0.8f, 0.8f}
            && material->legacySurface->ambientOcclusionMap == 0,
            "simple metal badge lost its packed opacity or constant reflectance");

    defaults.linkedPaths = {QStringLiteral("carpaint_standard.shaderbin")};
    defaults.parameters = {flag(0x7169AC81, true), number(0x74226C85, 1.0f), number(0x99CC69B1, 0.4f)};
    material = fls::mergeModelMaterialDefaults(*shader(std::nullopt), defaults);
    require(material->legacySurface && material->useDiffuseTexture == true
            && material->legacySurface->paintColorTexture && material->legacySurface->diffuseMap == 0xBCAB0B74
            && material->diffuseUv && material->diffuseUv->channel == 0 && material->gloss == 0.4f
            && !material->legacySurface->diffuseAlpha && material->legacySurface->opacityMap == 0
            && !material->legacySurface->opacityCutout && material->opacity == 1.0f,
            "legacy paint lost its opaque colour binding");
    instance.parameters = {flag(0xE159D67B, true)};
    material = fls::mergeModelMaterialDefaults(
        *shader(std::nullopt), *fls::mergeModelMaterialDefaults(defaults, instance));
    require(material->legacySurface && material->legacySurface->diffuseAlpha
            && material->legacySurface->opacityCutout && material->legacySurface->opacityFromAlpha
            && material->legacySurface->opacityMap == 0 && material->alphaUv && material->alphaUv->channel == 0,
            "legacy paint lost its enabled packed opacity binding");
    instance.parameters.push_back(flag(0x7169AC81, false));
    material = fls::mergeModelMaterialDefaults(
        *shader(std::nullopt), *fls::mergeModelMaterialDefaults(defaults, instance));
    require(material->legacySurface && material->useDiffuseTexture == false
            && material->legacySurface->opacityMap == 0xDDB62493 && material->legacySurface->opacityCutout
            && !material->legacySurface->diffuseAlpha && !material->legacySurface->opacityFromAlpha,
            "legacy paint lost its separate opacity binding");

    defaults.linkedPaths = {QStringLiteral("sparetire.shaderbin")};
    defaults.parameters = {
        vector(0xBA9B668F, Type::Color, {0.006f, 0.006f, 0.007f, 1.0f}),
        vector(0x43FEFDA3, Type::Vector2, {1, 2, 0, 0}),
        vector(0x3F9FD878, Type::Vector2, {15, 15, 0, 0}),
        number(0x0D5DC750, 1.2f)};
    material = fls::mergeModelMaterialDefaults(*shader(1), defaults);
    require(material->legacySurface && material->legacySurface->normalMap == 0xC751A20B
            && material->normalUv && material->normalUv->channel == 0
            && material->normalUv->tiling == std::array<float, 2>{1, 2}
            && material->normalIntensity == 1.2f
            && material->baseColor == std::array<float, 3>{0.006f, 0.006f, 0.007f},
            "spare rubber lost its authored color or tread normal mapping");
}

void checkStandardShaderMappings() {
    using Type = fls::ModelMaterialParameterType;
    using namespace fls::material_hashes::parameter;
    const auto parameter = [](quint32 hash, Type type, std::array<float, 4> value) {
        fls::ModelMaterialParameter result;
        result.nameHash = hash;
        result.type = type;
        result.vector = value;
        result.scalar = value[0];
        result.boolean = value[0] != 0.0f;
        return result;
    };
    fls::ModelMaterial shader;
    fls::ModelMaterial instance;
    shader.linkedPaths = {QStringLiteral("car_standard.shaderbin")};
    shader.parameters = {
        parameter(kPrimaryUvSelection, Type::Bool, {0}),
        parameter(kTextureTilingU, Type::Float, {3}),
        parameter(kTextureTilingV, Type::Float, {3}),
        parameter(kTextureTiling, Type::Vector2, {1, 1}),
        parameter(0xB8E61E16, Type::Bool, {0}),
        parameter(0xCD6B029B, Type::Float, {1}),
    };
    instance.parameters = {
        parameter(kTextureTilingU, Type::Float, {45}),
        parameter(kTextureTilingV, Type::Float, {75}),
        parameter(kUvOrientation, Type::Float, {135}),
        parameter(0x1B003865, Type::Bool, {1}),
        parameter(0xF383EB56, Type::Vector2, {8, 9}),
        parameter(0xECCEB8F9, Type::Bool, {1}),
        parameter(0x8BAB96B3, Type::Vector2, {4, 5}),
        parameter(0x090ABF6B, Type::Bool, {1}),
        parameter(0x4CCD7F85, Type::Vector2, {2, 6}),
        parameter(0xCD6B029B, Type::Float, {0}),
        parameter(0x4E282123, Type::Float, {0.7f}),
    };
    auto material = fls::mergeModelMaterialDefaults(shader, instance);
    require(material->diffuseUv && material->diffuseUv->channel == 1
            && material->diffuseUv->rotationDegrees == 135.0f
            && material->diffuseUv->tiling == std::array<float, 2>{45, 75},
            "disabled diffuse override replaced the shader's shared UV transform");
    require(material->normalUv && material->normalUv->channel == 1
            && material->normalUv->tiling == std::array<float, 2>{8, 9}
            && material->normalIntensity == 0.0f,
            "the standard normal map lost its independent transform or authored strength");
    require(material->surfaceUv && material->surfaceUv->tiling == std::array<float, 2>{4, 5}
            && material->alphaUv && material->alphaUv->tiling == std::array<float, 2>{2, 6}
            && material->roughnessShift == 0.7f,
            "surface and opacity maps lost their independent transforms");

    constexpr quint32 uniqueLivery = 0xF17A77BF;
    shader.linkedPaths = {QStringLiteral("car_automotive_paint.shaderbin")};
    shader.parameters = {parameter(uniqueLivery, Type::Bool, {0})};
    instance.parameters.clear();
    instance.name = QStringLiteral("paint_slot");
    material = fls::mergeModelMaterialDefaults(shader, instance);
    require(material->useDiffuseTexture == false, "stock paint activated an unused diffuse texture");
    instance.parameters = {parameter(uniqueLivery, Type::Bool, {1})};
    require(fls::mergeModelMaterialDefaults(shader, instance)->useDiffuseTexture == true,
            "an enabled paint texture was lost with a renamed slot");

    shader.linkedPaths = {QStringLiteral("car_carbonfiber.shaderbin")};
    shader.parameters = {
        parameter(uniqueLivery, Type::Bool, {0}),
        parameter(kTextureTilingU, Type::Float, {32}),
        parameter(kTextureTilingV, Type::Float, {32}),
        parameter(kWeaveColorTintA, Type::Color, {0.01f, 0.01f, 0.01f}),
        parameter(kWeaveMetalness, Type::Float, {0.2f}),
        parameter(kWeaveNormalIntensity, Type::Float, {0.8f}),
    };
    instance.parameters = {
        parameter(kWeaveRoughness, Type::Float, {0.5f}),
        parameter(kWeaveColorTintB, Type::Color, {0.005f, 0.005f, 0.005f}),
        parameter(kUvOrientation, Type::Float, {90}),
    };
    material = fls::mergeModelMaterialDefaults(shader, instance);
    require(material->useDiffuseTexture == false && material->weave
            && material->weave->tintA == std::array<float, 3>{0.01f, 0.01f, 0.01f}
            && material->weave->tintB == std::array<float, 3>{0.005f, 0.005f, 0.005f}
            && material->normalUv && material->normalUv->channel == 1
            && material->normalUv->tiling == std::array<float, 2>{32, 32}
            && material->normalUv->rotationDegrees == 90.0f
            && material->diffuseUv && material->diffuseUv->channel == 0
            && material->metallic == 0.2f && material->gloss == 0.5f && material->normalIntensity == 0.8f,
            "carbon weave inherited the decal coordinates or lost its authored surface parameters");
}

void checkWeaveLibrary(const QString &folder) {
    fls::PaintFinishLibrary library;
    library.load(folder);
    require(library.loaded(), "finish library unavailable");
    for (int code : {6, 7, 8, 9}) {
        const auto *finish = library.find(code);
        require(finish && finish->valid && finish->selfColored
            && finish->patternUv.channel == 1 && finish->normalUv.channel == 1, "weave shader UV channel lost");
        require(finish->hasWeaveMask() && finish->hasDetailNormal(), "weave texture or normal unavailable");
        require(!finish->hasPattern(), "weave tints were baked into a color texture");
        require(finish->patternUv.tiling == std::array<float, 2>{32, 32}
            && finish->normalUv.tiling == finish->patternUv.tiling, "authored weave tiling lost");
        const auto &pixels = finish->weaveMaskImage.rgba;
        quint8 minimumMask = 255;
        quint8 maximumMask = 0;
        for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
            minimumMask = std::min(minimumMask, pixels[pixel]);
            maximumMask = std::max(maximumMask, pixels[pixel]);
        }
        require(minimumMask == 0 && maximumMask == 255, "weave directions lost from mask");
        require(std::abs(finish->weaveColorTintA[0] - (code == 6 ? 0.009721218f : code == 7 ? 0.0001821162f : 0.054480281f)) < 0.000001f,
                "linear weave tint was converted or replaced");
        require(std::abs(finish->weaveColorTintB[0] - (code < 8 ? 0.009721218f : 0.003676507f)) < 0.000001f,
                "second linear weave tint was converted or replaced");
        require(finish->weaveMaskMips.size() == 9 && finish->detailNormalMips.size() == 9, "authored weave mips lost");
        require(finish->weaveMaskMips.back().width == 1 && finish->detailNormalMips.back().height == 1, "weave mip dimensions invalid");
        require(std::abs(finish->metallic - (code < 8 ? 0.2f : 0.8f)) < 0.0001f, "weave metalness lost");
        require(std::abs(finish->anisotropy - 0.8f) < 0.0001f && finish->normalIntensity == 1.0f, "weave directional shading lost");
        require(std::abs(finish->gloss - (code < 8 ? 0.65f : 0.6f)) < 0.0001f, "weave roughness replaced by coat roughness");
        require(finish->clearCoatCoverage == 1.0f
                    && std::abs(finish->clearCoatRoughness - (code % 2 == 0 ? 0.6f : 0.0f)) < 0.0001f,
                "authored weave coating lost");
        std::printf("finish %d: mask=%dx%d normal=%dx%d tiling=%.0fx%.0f mips=%zu metallic=%.1f UV=%d anisotropy=%.1f roughness=%.2f coat=%.1f\n",
                    code, finish->weaveMaskImage.width, finish->weaveMaskImage.height,
                    finish->detailNormalImage.width, finish->detailNormalImage.height,
                    finish->normalUv.tiling[0], finish->normalUv.tiling[1], finish->weaveMaskMips.size() + 1,
                    finish->metallic, finish->normalUv.channel, finish->anisotropy,
                    1.0f - finish->gloss, finish->clearCoatRoughness);
    }
    const auto *camo = library.find(60);
    require(camo && camo->hasPattern() && camo->patternUv.channel == 1
        && camo->patternUv.tiling == std::array<float, 2>{0.6f, 0.6f}
        && camo->patternUv.rotationDegrees == 270.0f, "modern camouflage shader mapping lost");
    const auto *chrome = library.find(12);
    const auto *brushed = library.find(14);
    require(chrome && chrome->hasMaterialColor && chrome->gloss == 1.0f
        && chrome->normalUv.channel == 0, "modern chrome lost its polish or mirror normal UV");
    require(brushed && brushed->hasDetailNormal() && brushed->normalUv.channel == 1
        && std::abs(brushed->gloss - 0.6f) < 0.0001f, "modern brushed metal lost its roughness shift");
}

void checkLegacyCamouflage(const fls::PaintFinishLibrary &library) {
    for (int code : {20, 21, 22, 23, 24, 25, 36, 37, 38, 39, 40, 41, 42, 43, 60, 61, 62, 63}) {
        const auto *finish = library.find(library.previewCode(code, QStringLiteral("FH6")));
        require(finish && finish->hasPattern() && !finish->patternMips.empty()
            && finish->patternUv.channel == 1, "legacy camouflage lost its texture or UV channel");
    }
    const auto *camo = library.find(60);
    require(camo && camo->hasPattern() && camo->patternUv.channel == 1
        && camo->patternUv.tiling == std::array<float, 2>{0.6f, 0.6f}
        && camo->patternUv.rotationDegrees == 277.0f && camo->patternUv.rotationPivot == 0.04f,
        "legacy camouflage shader mapping lost");
    require(camo->hasDetailNormal() && camo->normalUv.channel == 1
        && camo->normalUv.tiling == std::array<float, 2>{20, 20}
        && camo->normalUv.rotationDegrees == 0.0f, "legacy flake normal mapping lost");
}

void checkEarlyFinishLibrary(const QString &folder) {
    for (const QString &source : {folder, fls::gameMediaDir(folder)}) {
        fls::PaintFinishLibrary library;
        library.load(source, QStringLiteral("FH3"));
        checkLegacyCamouflage(library);
        const auto *gloss = library.find(1);
        const auto *wood = library.find(53);
        const auto *kevlar = library.find(8);
        const auto *pearl = library.find(67);
        const auto *damascus = library.find(66);
        require(gloss && !gloss->hasPattern(), "early Gloss loaded a disabled color map");
        require(wood && wood->hasPattern() && wood->selfColored, "early wood lost its color map");
        require(kevlar && kevlar->hasPattern() && kevlar->hasDetailNormal()
            && kevlar->patternUv.tiling == std::array<float, 2>{70, 70}
            && kevlar->patternUv.tiling == kevlar->normalUv.tiling,
            "early Kevlar lost its tint map or mapping");
        require(pearl && pearl->hasPattern() && pearl->patternUv.tiling == std::array<float, 2>{1.3f, 1.4f},
                "early pearl lost its tint map or mapping");
        require(damascus && damascus->metalMask && damascus->metalMask->variesGloss
            && damascus->surfaceUv.tiling == std::array<float, 2>{0.6f, 0.6f},
            "early Damascus lost its gloss map or mapping");
    }
}

void checkFm7FinishLibrary(const QString &folder) {
    const QDir media(fls::gameMediaDir(folder));
    for (const auto &source : {folder, media.filePath(QStringLiteral("pcfamily")), media.filePath(QStringLiteral("base"))}) {
        fls::PaintFinishLibrary library;
        library.load(source, QStringLiteral("FM7"));
        checkLegacyCamouflage(library);
        for (int code : {6, 7, 8, 9}) {
            const auto *finish = library.find(code);
            require(finish && finish->selfColored && finish->hasDetailNormal()
                && finish->normalUv.channel == 1 && finish->normalUv.tiling == std::array<float, 2>{55, 55},
                "FM7 carbon or Kevlar lost its weave normal");
            require(finish->hasPattern() == (code >= 8), "FM7 Kevlar lost its shader-default tint texture");
        }
        const auto *wood = library.find(53);
        const auto *damascus = library.find(66);
        require(wood && wood->hasPattern(), "FM7 wood lost its color map");
        require(damascus && damascus->hasRoughMetalAo(), "FM7 Damascus lost its gloss map");
    }
}

void checkLegacyFinishLibrary(const QString &folder, const QString &game) {
    fls::PaintFinishLibrary library;
    library.load(folder, game);
    for (int code : {1, 2, 3, 4}) {
        const auto *finish = library.find(library.previewCode(code, QStringLiteral("FH6")));
        require(finish && finish->valid && !finish->hasPattern() && finish->patternMips.empty(),
                "legacy solid paint loaded a disabled color map");
    }
    const auto *wood = library.find(53);
    require(wood && wood->hasPattern() && wood->selfColored, "legacy wood lost its enabled color map");
    checkLegacyCamouflage(library);
    for (int code : {6, 7, 8, 9}) {
        const auto *finish = library.find(code);
        require(finish && finish->hasDetailNormal() && finish->normalUv.channel == 1
            && finish->normalUv.tiling == std::array<float, 2>{55, 55}, "legacy weave shader mapping lost");
        require(finish->specularReflectance == std::array<float, 3>{0.1f, 0.1f, 0.1f}
            && finish->selfColored && finish->flakeAmount == 0.0f, "legacy weave lost its reflectance");
        require(finish->hasPattern() == (code >= 8) && (code < 8 || finish->patternUv.tiling == finish->normalUv.tiling),
            "legacy Kevlar lost its shader-default tint texture");
    }
    const auto *gold = library.find(13);
    const auto *brushed = library.find(26);
    const auto *polished = library.find(27);
    const auto *semiGloss = library.find(28);
    const auto *damascus = library.find(66);
    const auto *galvanized = library.find(68);
    require(gold && gold->hasMaterialColor && gold->materialColor[0] > gold->materialColor[1]
        && gold->materialColor[1] > gold->materialColor[2], "legacy gold lost its shader-default reflectance");
    require(brushed && polished && semiGloss && brushed->gloss > semiGloss->gloss && polished->gloss > brushed->gloss
        && brushed->hasMaterialColor && polished->materialColor == brushed->materialColor,
        "legacy brass finish variants lost their shared reflectance or different gloss");
    require(damascus && damascus->metalMask && damascus->metalMask->variesGloss
        && damascus->surfaceUv.tiling == std::array<float, 2>{0.6f, 0.6f},
        "legacy Damascus lost its material gloss map");
    require(galvanized && galvanized->metalMask && galvanized->metalMask->variesGloss
        && galvanized->metalMask->variesReflectance && galvanized->surfaceUv.tiling == std::array<float, 2>{1.3f, 1.3f},
        "legacy galvanized steel lost its gloss and reflectance map");
}

void checkCrossGameFinishCodes() {
    fls::PaintFinishLibrary current;
    fls::PaintFinishLibrary legacy;
    current.load({}, QStringLiteral("FH6"));
    legacy.load({}, QStringLiteral("FH5"));
    const unsigned generation = current.generation();
    current.replace(std::move(legacy));
    require(current.generation() != generation && current.previewCode(8, QStringLiteral("FH6")) == 8
        && current.previewCode(31, QStringLiteral("FH6")) == 31,
        "target library replacement lost equivalent finishes");
    require(current.previewCode(72, QStringLiteral("FH6")) < 0 && current.previewCode(72, QStringLiteral("FH5")) == 72,
        "a target finish reused an unrelated source code");
}

void checkAuthoredTangents(const QString &archive) {
    QString error;
    const auto extracted = fls::sharedZipExtraction(archive, &error);
    require(extracted != nullptr, qPrintable(error));
    QDirIterator files(extracted->path(), QStringList{QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    require(files.hasNext(), "model archive has no carbin");
    const auto model = fls::loadCarBin(files.next(), &error);
    require(!model.meshes.empty(), qPrintable(error));
    size_t authoredVertices = 0;
    for (const auto &mesh : model.meshes) {
        if (mesh.tangentChannels.size() < 2 || mesh.tangentChannels[1].empty()) continue;
        require(mesh.tangentChannels[1].size() == mesh.positions.size(), "UV1 tangent count mismatch");
        for (const auto &tangent : mesh.tangentChannels[1]) {
            const float length = tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;
            require(std::isfinite(length) && std::abs(length - 1.0f) < 0.01f, "decoded tangent is not a unit direction");
            require(std::isfinite(tangent.w) && std::abs(std::abs(tangent.w) - 1.0f) < 0.0001f,
                    "tangent handedness is not a unit sign");
        }
        authoredVertices += mesh.tangentChannels[1].size();
    }
    require(authoredVertices > 0, "no authored UV1 tangents decoded");
    std::printf("Authored UV1 tangents checked: %zu vertices\n", authoredVertices);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        checkSamplerDefaults();
        checkLampGlassOpacityFallback();
        checkLegacyShaderMappings();
        checkStandardShaderMappings();
        checkCrossGameFinishCodes();
        const QStringList args = app.arguments();
        if (args.size() == 3 && args[1] == QStringLiteral("--legacy-library")) {
            checkLegacyFinishLibrary(args[2], QStringLiteral("FH5"));
        } else if (args.size() == 3 && args[1] == QStringLiteral("--fm7-library")) {
            checkFm7FinishLibrary(args[2]);
        } else if (args.size() == 4 && args[1] == QStringLiteral("--older-libraries")) {
            checkEarlyFinishLibrary(args[2]);
            checkLegacyFinishLibrary(args[3], QStringLiteral("FH4"));
        } else {
            if (args.size() >= 2) checkWeaveLibrary(args[1]);
            if (args.size() >= 3) checkAuthoredTangents(args[2]);
        }
        std::puts("Paint finish checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
