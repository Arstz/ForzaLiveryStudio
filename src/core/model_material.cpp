#include "model_material.h"

#include "binary_io.h"
#include "material_hashes.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace fls {
namespace {

class Cursor : public detail::BinaryCursor {
public:
    explicit Cursor(const QByteArray &bytes)
        : BinaryCursor(bytes, "material data is truncated") {}

    QString string7() {
        quint32 length = 0;
        int shift = 0;
        for (int i = 0; i < 5; ++i) {
            const quint8 byte = u8();
            length |= static_cast<quint32>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                if (length > (1u << 24)) {
                    throw std::runtime_error("material string is too large");
                }
                require(static_cast<int>(length));
                const QString value = QString::fromUtf8(bytes_.constData() + position_, static_cast<int>(length));
                position_ += static_cast<int>(length);
                return value;
            }
            shift += 7;
        }
        throw std::runtime_error("invalid material string length");
    }

    QUuid guid() {
        std::array<char, 16> id;
        for (char &byte : id) byte = static_cast<char>(u8());

        return QUuid::fromBytes(id.data(), QSysInfo::LittleEndian);
    }
};

bool atLeast(quint8 major, quint8 minor, quint8 wantedMajor, quint8 wantedMinor) {
    return major > wantedMajor || (major == wantedMajor && minor >= wantedMinor);
}

std::array<float, 4> readVector(Cursor &cursor) {
    return {cursor.f32(), cursor.f32(), cursor.f32(), cursor.f32()};
}

ModelMaterialParameter readParameter(Cursor &cursor) {
    ModelMaterialParameter parameter;
    parameter.versionMajor = cursor.u8();
    parameter.versionMinor = cursor.u8();
    parameter.nameHash = cursor.u32();
    if (atLeast(parameter.versionMajor, parameter.versionMinor, 3, 1) && cursor.u8() != 0) {
        parameter.aliasHash = cursor.u32();
    }
    parameter.type = static_cast<ModelMaterialParameterType>(cursor.u8());
    if (parameter.versionMajor >= 3) {
        parameter.parameterId = cursor.guid();
    }

    switch (parameter.type) {
    case ModelMaterialParameterType::Vector:
    case ModelMaterialParameterType::Color:
    case ModelMaterialParameterType::Swizzle:
    case ModelMaterialParameterType::FunctionRange:
    case ModelMaterialParameterType::PackedVector:
        parameter.vector = readVector(cursor);
        break;
    case ModelMaterialParameterType::Float:
        parameter.scalar = cursor.f32();
        break;
    case ModelMaterialParameterType::Bool:
        parameter.boolean = cursor.i32() != 0;
        break;
    case ModelMaterialParameterType::Int:
        parameter.integer = cursor.i32();
        break;
    case ModelMaterialParameterType::Texture2D:
        parameter.texturePath = cursor.string7();
        if (parameter.versionMajor >= 2) {
            parameter.texturePathHash = cursor.u32();
        }
        break;
    case ModelMaterialParameterType::Sampler:
        parameter.samplerAddressU = cursor.i32();
        parameter.samplerAddressV = cursor.i32();
        if (atLeast(parameter.versionMajor, parameter.versionMinor, 1, 1)) {
            parameter.samplerFilter = cursor.i32();
        }
        if (atLeast(parameter.versionMajor, parameter.versionMinor, 3, 4)) {
            cursor.u32();
        }
        break;
    case ModelMaterialParameterType::ColorGradient: {
        const quint32 count = cursor.u32();
        if (count > (1u << 20)) {
            throw std::runtime_error("material gradient is too large");
        }
        parameter.gradient.reserve(static_cast<qsizetype>(count));
        for (quint32 i = 0; i < count; ++i) {
            parameter.gradient.push_back(readVector(cursor));
        }
        break;
    }
    case ModelMaterialParameterType::Vector2:
        parameter.vector[0] = cursor.f32();
        parameter.vector[1] = cursor.f32();
        if (parameter.versionMajor == 1) {
            cursor.skip(8);
        }
        break;
    default:
        throw std::runtime_error("unsupported material parameter type");
    }
    return parameter;
}

std::vector<ModelMaterialParameter> readParameters(const BundleBlobRecord &blob) {
    Cursor cursor(blob.data);
    const quint32 count = blob.isAtLeastVersion(2, 1) ? cursor.u16() : cursor.u8();
    if (count > (1u << 16)) {
        throw std::runtime_error("material parameter count is too large");
    }
    std::vector<ModelMaterialParameter> parameters;
    parameters.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        parameters.push_back(readParameter(cursor));
    }
    return parameters;
}

void appendShaderTextures(ModelMaterial &material, const BundleBlobRecord &blob) {
    if (!blob.isAtLeastVersion(3, 1)) return;
    Cursor cursor(blob.data);
    const quint16 count = cursor.u16();
    for (quint16 index = 0; index < count; ++index) {
        ShaderTextureBinding binding;
        binding.nameHash = cursor.u32();
        binding.shaderRegister = cursor.u16();
        binding.parameterId = cursor.guid();
        material.shaderTextures.push_back(std::move(binding));
    }
}

void applyTextureCoordinates(ModelMaterial &material, const ModelMaterialParameter &parameter) {
    using namespace material_hashes::parameter;
    using Type = ModelMaterialParameterType;
    if (parameter.type == Type::Bool && parameter.nameHash == kPrimaryUvSelection) {
        material.materialUvChannel = parameter.boolean ? 0 : 1;
    } else if (parameter.type == Type::Vector2) {
        const std::array<float, 2> tiling = {parameter.vector[0], parameter.vector[1]};
        if (parameter.nameHash == kLegacyGlassAlphaTiling) {
            material.alphaUv = MaterialUvMapping{tiling};
            material.glassOpacity.interpolateAlphaTexture = true;
        } else if (parameter.nameHash == kLegacyGlassNormalTiling || parameter.nameHash == kLegacyBulbNormalTiling) {
            material.normalUv = MaterialUvMapping{tiling};
        } else if (parameter.nameHash == kSecondUvGlassNormalTiling) {
            material.normalUv = MaterialUvMapping{tiling, 1};
        } else if (parameter.nameHash == kLegacyEmissionTiling) {
            material.emissiveUv = MaterialUvMapping{tiling};
        } else if (parameter.nameHash == kLegacyEmissionMaskTiling) {
            material.emissiveMaskUv = MaterialUvMapping{tiling};
        } else if (parameter.nameHash == kGlassTintMaskTiling) {
            material.glassTintUv = MaterialUvMapping{tiling};
        }
    } else if (parameter.type == Type::Float && std::isfinite(parameter.scalar)) {
        MaterialUvMapping emission = material.emissiveUv.value_or(MaterialUvMapping{});
        MaterialUvMapping mask = material.emissiveMaskUv.value_or(MaterialUvMapping{});
        const float value = parameter.scalar;
        switch (parameter.nameHash) {
        case kSecondUvGlassTilingU:
        case kSecondUvGlassTilingV:
        case kSecondUvGlassRotation: {
            MaterialUvMapping mapping = material.normalUv.value_or(MaterialUvMapping{});
            mapping.channel = 1;
            mapping.tileBeforeRotation = true;
            if (parameter.nameHash == kSecondUvGlassRotation) mapping.rotationDegrees = value;
            else mapping.tiling[parameter.nameHash == kSecondUvGlassTilingU ? 0 : 1] = value;
            material.normalUv = material.alphaUv = mapping;
            material.glassOpacity.interpolateAlphaTexture = true;
            return;
        }
        case kEmissionUvOrientation:
            emission.rotationDegrees = value;
            mask.rotationDegrees = value;
            break;
        case kEmissionTilingU: emission.tiling[0] = value; break;
        case kEmissionTilingV: emission.tiling[1] = value; break;
        case kEmissionMaskTilingU: mask.tiling[0] = value; break;
        case kEmissionMaskTilingV: mask.tiling[1] = value; break;
        default: return;
        }
        material.emissiveUv = emission;
        material.emissiveMaskUv = mask;
    }
}

void applyGlassAndEmission(ModelMaterial &material, const ModelMaterialParameter &parameter) {
    using namespace material_hashes::parameter;
    using Type = ModelMaterialParameterType;
    const quint32 id = parameter.nameHash;
    if (parameter.type == Type::Bool) {
        if (id == kGlassDualOpacity) material.glassOpacity.dualOpacity = parameter.boolean;
        if (id == kAlphaTextureEnabled) material.glassOpacity.useAlphaTexture = parameter.boolean;
        if (id == kEmissionEnabled) material.emissionEnabled = parameter.boolean;
        if (id == kLegacyEmissionMaskEnabled) material.emissionMaskEnabled = parameter.boolean;
        if (id == kGlassTintMaskEnabled) material.useGlassTintMask = parameter.boolean;
        if (id == kGlassOpacityTintMask) material.useGlassTintForOpacity = parameter.boolean;
    } else if (parameter.type == Type::Float && std::isfinite(parameter.scalar)) {
        const float opacity = std::clamp(parameter.scalar, 0.0f, 1.0f);
        if (id == kGlassOpacity || id == kLegacyGlassOpacity) material.glassOpacity.primary = opacity;
        if (id == kSecondaryGlassOpacity) material.glassOpacity.secondary = opacity;
        if (id == kGlassOpacityMaskMin) material.glassOpacity.maskMin = opacity;
        if (id == kGlassOpacityMaskMax) material.glassOpacity.maskMax = opacity;
        if (id == kGlassBackNormalIntensity || id == kLegacyGlassNormalIntensity) {
            material.normalIntensity = parameter.scalar;
        }
        if (id == kGlassTintMultiplier) material.glassTintMultiplier = parameter.scalar;
    } else if (parameter.type == Type::Color || parameter.type == Type::Vector) {
        if (id == kEmissionTint) {
            material.usesTintedEmission = true;
            material.emissiveColor = {parameter.vector[0], parameter.vector[1], parameter.vector[2]};
        }
        if (id == kGlassColor) material.glass = true;
    }
}

void applyPreviewParameter(ModelMaterial &material, const ModelMaterialParameter &parameter) {
    applyTextureCoordinates(material, parameter);
    applyGlassAndEmission(material, parameter);
    const bool vectorValue = parameter.type == ModelMaterialParameterType::Vector
        || parameter.type == ModelMaterialParameterType::Color;
    if (vectorValue
        && (material_hashes::contains(material_hashes::parameter::kBaseColor, parameter.nameHash)
            || parameter.nameHash == material_hashes::parameter::kTireBaseColor)) {
        material.hasBaseColor = true;
        material.baseColor = {parameter.vector[0], parameter.vector[1], parameter.vector[2]};
        if (parameter.vector[3] > 0.0f && parameter.vector[3] < 1.0f) {
            material.opacity = parameter.vector[3];
        }
    }
    if (vectorValue
        && material_hashes::contains(material_hashes::parameter::kEmissiveColor, parameter.nameHash)) {
        material.emissiveColor = {parameter.vector[0], parameter.vector[1], parameter.vector[2]};
        material.emissiveIntensity = std::max(material.emissiveIntensity, 1.0f);
    }
    if (parameter.type == ModelMaterialParameterType::Float
        && material_hashes::contains(
            material_hashes::parameter::kEmissiveIntensity, parameter.nameHash)) {
        material.emissiveIntensity = std::max(0.0f, parameter.scalar);
    }
    if (parameter.type == ModelMaterialParameterType::Float
        && material_hashes::contains(material_hashes::parameter::kOpacity, parameter.nameHash)) {
        material.opacity = std::clamp(parameter.scalar, 0.0f, 1.0f);
    }
    if (parameter.type == ModelMaterialParameterType::Float
        && material_hashes::contains(material_hashes::parameter::kGloss, parameter.nameHash)) {
        material.gloss = std::clamp(parameter.scalar, 0.0f, 1.0f);
    }
    // The clearcoat is the visible gloss for automotive paint; its roughness overrides.
    if (parameter.type == ModelMaterialParameterType::Float
        && parameter.nameHash == material_hashes::parameter::kClearcoatRoughness
        && std::isfinite(parameter.scalar)) {
        material.gloss = std::clamp(1.0f - parameter.scalar, 0.0f, 1.0f);
    }
    if (parameter.type == ModelMaterialParameterType::Float
        && parameter.nameHash == material_hashes::parameter::kUvOrientation
        && std::isfinite(parameter.scalar)) {
        material.uvOrientationDegrees = parameter.scalar;
    }
    if (parameter.type == ModelMaterialParameterType::Float
        && parameter.nameHash == material_hashes::parameter::kTextureTilingU
        && std::isfinite(parameter.scalar)) {
        material.uTiling = std::abs(parameter.scalar) > 0.000001f ? parameter.scalar : 1.0f;
    }
    if (parameter.type == ModelMaterialParameterType::Float
        && parameter.nameHash == material_hashes::parameter::kTextureTilingV
        && std::isfinite(parameter.scalar)) {
        material.vTiling = std::abs(parameter.scalar) > 0.000001f ? parameter.scalar : 1.0f;
    }
    if (parameter.type == ModelMaterialParameterType::Vector2
        && (parameter.nameHash == material_hashes::parameter::kTextureTiling
            || parameter.nameHash == material_hashes::parameter::kTireMapTiling)
        && std::isfinite(parameter.vector[0]) && std::isfinite(parameter.vector[1])) {
        material.uTiling = std::abs(parameter.vector[0]) > 0.000001f ? parameter.vector[0] : 1.0f;
        material.vTiling = std::abs(parameter.vector[1]) > 0.000001f ? parameter.vector[1] : 1.0f;
    }
    if (vectorValue
        && material_hashes::contains(material_hashes::parameter::kMetallic, parameter.nameHash)) {
        const float f0 = std::max({parameter.vector[0], parameter.vector[1], parameter.vector[2]});
        if (std::isfinite(f0)) {
            material.hasMetallic = true;
            material.metallic = std::clamp((f0 - 0.10f) / 0.40f, 0.0f, 1.0f);
        }
    }
    if (parameter.type == ModelMaterialParameterType::Float
        && material_hashes::contains(material_hashes::parameter::kFlakeAmount, parameter.nameHash)
        && std::isfinite(parameter.scalar)) {
        material.flakeAmount = std::clamp(parameter.scalar, 0.0f, 1.0f);
    }
    if (parameter.type == ModelMaterialParameterType::Texture2D
        && parameter.nameHash == material_hashes::parameter::kColorTexture) {
        material.patternTexture = parameter.texturePath;
    }
    if (parameter.type == ModelMaterialParameterType::Texture2D
        && material_hashes::contains(
            material_hashes::parameter::kDetailNormalTexture, parameter.nameHash)) {
        material.detailNormalTexture = parameter.texturePath;
    }
    if (parameter.type == ModelMaterialParameterType::Texture2D
        && parameter.nameHash == material_hashes::parameter::kSurfaceTexture) {
        material.roughMetalAoTexture = parameter.texturePath;
    }
}

void appendParameters(ModelMaterial &material, const BundleBlobRecord &blob) {
    std::vector<ModelMaterialParameter> decoded = readParameters(blob);
    material.parameters.insert(material.parameters.end(), std::make_move_iterator(decoded.begin()),
                               std::make_move_iterator(decoded.end()));
}

constexpr quint32 kLegacyOpacityMap = 0x698CA64F;
constexpr quint32 kLegacyColorMap = 0x810E7224;
constexpr quint32 kLegacySpecularMap = 0x4C96A196;
constexpr quint32 kLegacyGlossMap = 0xA91F5977;
constexpr quint32 kNormalTiling = 0x942CA044;
constexpr quint32 kNormalStrength = 0xEEBAB966;
constexpr quint32 kDetailNormalMap = 0xD62CC872;
constexpr quint32 kDetailNormalTiling = 0x3F9FD878;
constexpr quint32 kDetailNormalStrength = 0xAE0B41F9;
constexpr quint32 kDetailNormalEnabled = 0x255EF28A;
constexpr quint32 kSecondChannelTilingU = 0xB0B8947E;
constexpr quint32 kSecondChannelTilingV = 0xCCD9B1A5;
constexpr quint32 kSpecularColor = 0x1F9B6488;
constexpr quint32 kSecondarySpecularColor = 2434032491;
constexpr quint32 kSimpleSpecularColor = 0x938926B0;
constexpr quint32 kSpecularTint = 0x0E29312A;
constexpr quint32 kGlossLow = 0xB9DE26A0;
constexpr quint32 kGlossHigh = 0x52E99DA3;
constexpr quint32 kFirstChannelNormalEnabled = 0x553D641D;
constexpr quint32 kFirstChannelOpacityEnabled = 0xA6BF15E8;
constexpr quint32 kFirstChannelOpacityTiling = 0x4F90F34B;
constexpr quint32 kFirstChannelGlossEnabled = 0x5A0DA36A;

class MaterialParameters {
public:
    explicit MaterialParameters(const ModelMaterial &material) : parameters_(material.parameters) {}

    float number(quint32 hash, float fallback) const {
        const auto *parameter = find(hash, ModelMaterialParameterType::Float);

        return parameter && std::isfinite(parameter->scalar) ? parameter->scalar : fallback;
    }

    bool enabled(quint32 hash, bool fallback = false) const {
        const auto *parameter = find(hash, ModelMaterialParameterType::Bool);

        return parameter ? parameter->boolean : fallback;
    }

    std::array<float, 3> color(quint32 hash, std::array<float, 3> fallback) const {
        const auto *parameter = find(hash, ModelMaterialParameterType::Color);
        if (!parameter) parameter = find(hash, ModelMaterialParameterType::Vector);

        return parameter ? std::array{parameter->vector[0], parameter->vector[1], parameter->vector[2]} : fallback;
    }

    MaterialUvMapping uv(int channel, quint32 tilingHash = 0) const {
        const auto *tiling = find(tilingHash, ModelMaterialParameterType::Vector2);
        MaterialUvMapping mapping;
        mapping.channel = channel;
        if (tiling && std::isfinite(tiling->vector[0]) && std::isfinite(tiling->vector[1])) {
            mapping.tiling = {tiling->vector[0], tiling->vector[1]};
        }

        return mapping;
    }

    MaterialUvMapping secondChannelUv(quint32 tilingHash = 0) const {
        auto mapping = uv(1, tilingHash);
        mapping.tiling[0] *= number(kSecondChannelTilingU, 1.0f);
        mapping.tiling[1] *= number(kSecondChannelTilingV, 1.0f);

        return mapping;
    }

private:
    const ModelMaterialParameter *find(quint32 hash, ModelMaterialParameterType type) const {
        const auto parameter = std::find_if(parameters_.crbegin(), parameters_.crend(), [=](const auto &value) {
            return value.nameHash == hash && value.type == type;
        });

        return parameter == parameters_.crend() ? nullptr : &*parameter;
    }

    const std::vector<ModelMaterialParameter> &parameters_;
};

void setMaterialColor(ModelMaterial &material, const std::array<float, 3> &color) {
    material.hasBaseColor = true;
    material.baseColor = color;
}

void setLegacySpecular(ModelMaterial &material, const std::array<float, 3> &reflectance) {
    const float peak = *std::max_element(reflectance.cbegin(), reflectance.cend());
    constexpr float kDielectricLimit = 0.10f;
    constexpr float kMetalRange = 0.40f;
    material.hasMetallic = true;
    material.metallic = std::clamp((peak - kDielectricLimit) / kMetalRange, 0.0f, 1.0f);
    if (material.metallic > 0.0f) setMaterialColor(material, reflectance);
}

void setLegacyDiffuseUv(ModelMaterial &material, const MaterialUvMapping &mapping) {
    material.materialUvChannel = mapping.channel;
    material.uTiling = mapping.tiling[0];
    material.vTiling = mapping.tiling[1];
}

void setLegacyNormal(ModelMaterial &material, LegacyMaterialSurface &surface,
                     quint32 map, const MaterialUvMapping &mapping, float strength) {
    surface.normalMap = map;
    material.normalUv = mapping;
    material.normalIntensity = strength;
}

void applyLegacyMetalEmission(ModelMaterial &material, LegacyMaterialSurface &surface,
                              const MaterialParameters &parameters, const MaterialUvMapping &uv,
                              bool firstChannel) {
    using material_hashes::parameter::kLegacyEmissionTexture;
    using material_hashes::parameter::kLegacyEmissionTiling;
    constexpr std::array kWhite = {1.0f, 1.0f, 1.0f};
    constexpr quint32 kEmissionMapEnabled = 200487307;
    constexpr quint32 kEmissionColor = 2614023278;
    constexpr quint32 kFirstChannelEmissionColor = 3272132913;
    constexpr quint32 kEmissionUsesMapColor = 128760095;
    constexpr quint32 kEmissionIntensity = 1866843633;
    constexpr quint32 kEmissionMultiplier = 325377669;
    material.emissionEnabled = parameters.enabled(kEmissionMapEnabled);
    if (!*material.emissionEnabled) return;

    const bool useMapColor = firstChannel && parameters.enabled(kEmissionUsesMapColor);
    material.emissiveIntensity = parameters.number(kEmissionIntensity, 1.0f)
        * parameters.number(kEmissionMultiplier, 1.0f);
    if (useMapColor) {
        surface.emissionMap = kLegacyEmissionTexture;
        material.emissiveUv = parameters.uv(0, kLegacyEmissionTiling);
        material.emissiveColor = kWhite;
        material.usesTintedEmission = true;
    } else {
        surface.emissionMaskMap = kLegacyEmissionTexture;
        material.emissiveMaskUv = firstChannel ? parameters.uv(0, kLegacyEmissionTiling) : uv;
        material.emissiveColor = parameters.color(firstChannel ? kFirstChannelEmissionColor : kEmissionColor, kWhite);
    }
}

void applyLegacyBadgeSurface(ModelMaterial &material, LegacyMaterialSurface &surface,
                             const MaterialParameters &parameters, float normalStrength, bool separateMasks) {
    using material_hashes::parameter::kNormalTexture;
    constexpr std::array kWhite = {1.0f, 1.0f, 1.0f};
    constexpr std::array kDielectric = {0.04f, 0.04f, 0.04f};
    constexpr quint32 kBadgeColor = 1661209993;
    constexpr quint32 kBadgeUsesColorMap = 94634471;
    constexpr quint32 kBadgeUsesMetalMask = 145932671;
    constexpr quint32 kBadgeUsesOpacity = 3417561480;
    constexpr quint32 kMaskTexture = 70589296;
    constexpr quint32 kMaskTiling = 734603102;
    constexpr quint32 kGlossTexture = 1944199385;
    constexpr quint32 kGlossTiling = 4223801439;
    constexpr quint32 kOuterReflectance = 2764571043;
    constexpr quint32 kOuterGloss = 1444695454;
    const bool mappedColor = parameters.enabled(kBadgeUsesColorMap);
    const bool metalMask = parameters.enabled(kBadgeUsesMetalMask);
    const float highGloss = parameters.number(kGlossHigh, material.gloss);
    const float lowGloss = parameters.number(kGlossLow, highGloss);

    LegacyMetalBlend &blend = surface.metalBlend.emplace();
    blend.unmaskedReflectance = parameters.color(kSpecularColor, kDielectric);
    blend.maskedReflectance = parameters.color(kSecondarySpecularColor, kDielectric);
    blend.opacityFromNormalAlpha = parameters.enabled(kBadgeUsesOpacity);
    material.useDiffuseTexture = mappedColor;

    setMaterialColor(material, mappedColor ? kWhite : parameters.color(kBadgeColor, kWhite));
    if (mappedColor) {
        surface.diffuseMap = kLegacyColorMap;
        material.diffuseUv = MaterialUvMapping{};
    }
    if (separateMasks) {
        blend.mode = LegacyMetalBlendMode::SeparateMasks;
        blend.maskUv = parameters.uv(0, kMaskTiling);
        blend.outerReflectance = parameters.color(kOuterReflectance, kDielectric);
        blend.outerGloss = std::clamp(parameters.number(kOuterGloss, highGloss), 0.0f, 1.0f);
        surface.metalBlendMaskMap = kMaskTexture;
        surface.glossMap = kGlossTexture;
        surface.glossUv = parameters.uv(0, kGlossTiling);
    } else if (metalMask) {
        blend.mode = LegacyMetalBlendMode::SingleMask;
        if (!mappedColor) {
            surface.glossMap = kMaskTexture;
            surface.glossUv = parameters.uv(0, kMaskTiling);
        }
    }
    surface.glossRange = {std::clamp(highGloss, 0.0f, 1.0f), std::clamp(lowGloss, 0.0f, 1.0f)};
    material.gloss = surface.glossRange[1];
    setLegacyNormal(material, surface, kNormalTexture, parameters.uv(0, kNormalTiling), normalStrength);
}

void applyLegacyBadgeOcclusion(ModelMaterial &material, LegacyMaterialSurface &surface,
                              const MaterialParameters &parameters, bool separateOcclusion) {
    constexpr quint32 kUseAmbientOcclusion = 3900104140;
    constexpr quint32 kUseSpecularOcclusion = 4267420983;
    constexpr quint32 kAmbientOcclusionMap = 4101427739;
    constexpr quint32 kAmbientOcclusionTiling = 1737689294;
    auto &blend = *surface.metalBlend;
    blend.usesAmbientOcclusion = parameters.enabled(kUseAmbientOcclusion);
    blend.usesSpecularOcclusion = separateOcclusion
        ? parameters.enabled(kUseSpecularOcclusion) : blend.usesAmbientOcclusion;
    if (blend.usesAmbientOcclusion || blend.usesSpecularOcclusion) {
        surface.ambientOcclusionMap = kAmbientOcclusionMap;
        material.surfaceUv = parameters.uv(0, kAmbientOcclusionTiling);
    }
}

void applyLegacyBadgeGlossSurface(ModelMaterial &material, LegacyMaterialSurface &surface,
                                 const MaterialParameters &parameters, float normalStrength) {
    using material_hashes::parameter::kNormalTexture;
    constexpr std::array kWhite = {1.0f, 1.0f, 1.0f};
    constexpr std::array kDielectric = {0.04f, 0.04f, 0.04f};
    constexpr quint32 kUseDiffuseMap = 83425786;
    constexpr quint32 kUseSpecularMap = 259058124;
    constexpr quint32 kUseReflectanceMask = 1898331831;
    constexpr quint32 kDiffuseColor = 4111874494;
    constexpr quint32 kDiffuseTiling = 1972415244;
    constexpr quint32 kGlossTiling = 1711498804;
    constexpr quint32 kSpecularTiling = 977636089;
    const bool mappedColor = parameters.enabled(kUseDiffuseMap);
    const bool mappedSpecular = parameters.enabled(kUseSpecularMap);
    const bool mappedGloss = parameters.enabled(kFirstChannelGlossEnabled);
    const auto reflectance = parameters.color(kSpecularColor, kDielectric);
    const auto tint = parameters.color(kSpecularTint, kWhite);
    const float highGloss = parameters.number(kGlossHigh, material.gloss);
    const float lowGloss = mappedGloss ? parameters.number(kGlossLow, highGloss) : highGloss;

    LegacyMetalBlend &blend = surface.metalBlend.emplace();
    blend.mode = LegacyMetalBlendMode::GlossReflectance;
    blend.unmaskedReflectance = parameters.color(kSecondarySpecularColor, kDielectric);
    blend.maskedReflectance = mappedSpecular ? reflectance
        : std::array{reflectance[0] * tint[0], reflectance[1] * tint[1], reflectance[2] * tint[2]};
    blend.usesReflectanceMask = parameters.enabled(kUseReflectanceMask);
    blend.opacityFromNormalAlpha = parameters.enabled(kFirstChannelOpacityEnabled);
    material.useDiffuseTexture = mappedColor;
    setMaterialColor(material, mappedColor ? kWhite : parameters.color(kDiffuseColor, kWhite));
    if (mappedColor) {
        surface.diffuseMap = kLegacyColorMap;
        material.diffuseUv = parameters.uv(0, kDiffuseTiling);
    } else if (mappedGloss || blend.usesReflectanceMask) {
        surface.glossMap = kLegacyGlossMap;
        surface.glossUv = parameters.uv(0, kGlossTiling);
    }
    if (mappedSpecular) {
        surface.metalBlendMaskMap = kLegacySpecularMap;
        blend.maskUv = parameters.uv(0, kSpecularTiling);
    }
    applyLegacyBadgeOcclusion(material, surface, parameters, true);
    surface.glossRange = {std::clamp(lowGloss, 0.0f, 1.0f), std::clamp(highGloss, 0.0f, 1.0f)};
    material.gloss = surface.glossRange[1];
    setLegacyNormal(material, surface, kNormalTexture, parameters.uv(0, kNormalTiling), normalStrength);
}

void applyLegacySimpleBadgeSurface(ModelMaterial &material, LegacyMaterialSurface &surface,
                                  const MaterialParameters &parameters, float normalStrength) {
    using material_hashes::parameter::kNormalTexture;
    constexpr quint32 kMetalEnabled = 2560295535;
    constexpr std::array kWhite = {1.0f, 1.0f, 1.0f};
    constexpr std::array kDielectric = {0.04f, 0.04f, 0.04f};
    const bool metal = parameters.enabled(kMetalEnabled, true);
    const auto reflectance = parameters.color(kSpecularColor, kDielectric);
    const auto tint = parameters.color(kSpecularTint, kWhite);

    LegacyMetalBlend &blend = surface.metalBlend.emplace();
    blend.mode = LegacyMetalBlendMode::GlossReflectance;
    blend.maskedReflectance = metal
        ? std::array{reflectance[0] * tint[0], reflectance[1] * tint[1], reflectance[2] * tint[2]}
        : kDielectric;
    blend.opacityFromNormalAlpha = parameters.enabled(kFirstChannelOpacityEnabled);
    if (metal) setLegacySpecular(material, blend.maskedReflectance);
    material.useDiffuseTexture = false;
    surface.glossRange = {material.gloss, material.gloss};
    applyLegacyBadgeOcclusion(material, surface, parameters, false);
    setLegacyNormal(material, surface, kNormalTexture, parameters.uv(0, kNormalTiling), normalStrength);
}

bool applyLegacyPaintSurface(ModelMaterial &material, LegacyMaterialSurface &surface, const MaterialParameters &parameters) {
    constexpr quint32 kPaintColorMap = 3165326196;
    constexpr quint32 kPaintOpacityMap = 3719701651;
    constexpr quint32 kPaintOpacityEnabled = 3780761211;
    constexpr quint32 kUseDiffuseColor = 3789039613;
    constexpr quint32 kDiffuseUsesTexture = 2518010538;
    constexpr quint32 kFlakeUsesTexture = 1902750849;
    constexpr quint32 kPackedOpacity = 3494355569;
    constexpr quint32 kFlakeColorMultiplier = 1948413061;
    constexpr quint32 kFlakeColor = 3234539552;
    constexpr quint32 kFlakeTiling = 2916271230;
    constexpr quint32 kFlakeNormal = 1071456347;
    constexpr quint32 kFlakeGloss = 2580310449;
    constexpr quint32 kFlakeStrength = 2263846833;
    constexpr quint32 kPaintNormalEnabled = 1992599822;
    constexpr quint32 kPaintNormal = 3344015883;
    constexpr quint32 kPaintNormalStrength = 224249680;
    const bool diffuseColor = parameters.enabled(kUseDiffuseColor);
    const bool flakeTexture = parameters.enabled(kFlakeUsesTexture);
    const bool mappedColor = diffuseColor ? parameters.enabled(kDiffuseUsesTexture) : flakeTexture;
    const bool mappedOpacity = parameters.enabled(kPaintOpacityEnabled);
    const bool wheelPaint = material.resourcePath.contains(QStringLiteral("wheelmaterials"), Qt::CaseInsensitive);
    const float flakes = parameters.number(kFlakeStrength, 0.0f);
    if (!mappedColor && !mappedOpacity && !wheelPaint) return false;

    material.useDiffuseTexture = mappedColor;
    if (mappedColor) {
        const float multiplier = diffuseColor ? 1.0f : parameters.number(kFlakeColorMultiplier, 1.0f);
        surface.diffuseMap = kPaintColorMap;
        surface.paintColorTexture = true;
        material.diffuseUv = MaterialUvMapping{};
        setMaterialColor(material, {multiplier, multiplier, multiplier});
    } else if (wheelPaint && flakes > 0.0f) {
        setMaterialColor(material, parameters.color(kFlakeColor, material.baseColor));
    }
    if (mappedOpacity) {
        const bool packed = flakeTexture || parameters.enabled(kPackedOpacity);
        surface.opacityCutout = true;
        surface.opacityFromAlpha = packed;
        surface.diffuseAlpha = mappedColor && packed;
        if (!surface.diffuseAlpha) surface.opacityMap = packed ? kPaintColorMap : kPaintOpacityMap;
        material.alphaUv = MaterialUvMapping{};
    }
    if (mappedColor || flakes > 0.0f) {
        material.gloss = std::clamp(parameters.number(kFlakeGloss, material.gloss), 0.0f, 1.0f);
    }
    if (flakes > 0.0f) {
        setLegacyNormal(material, surface, kFlakeNormal, parameters.uv(1, kFlakeTiling), flakes);
    } else if (parameters.enabled(kPaintNormalEnabled)) {
        setLegacyNormal(material, surface, kPaintNormal, MaterialUvMapping{},
                        parameters.number(kPaintNormalStrength, 1.0f));
    }

    return true;
}

void applyLegacyShaderSurface(ModelMaterial &material, const QString &program) {
    using material_hashes::parameter::kNormalTexture;
    constexpr std::array kWhite = {1.0f, 1.0f, 1.0f};
    constexpr std::array kBlack = {0.0f, 0.0f, 0.0f};
    constexpr std::array kDielectric = {0.04f, 0.04f, 0.04f};
    const MaterialParameters parameters(material);
    const bool firstChannelMetal = program == QStringLiteral("m_ch1normglossalphaemissive.shaderbin")
        || program == QStringLiteral("int_m_ch1normglossalphaemissive.shaderbin");
    const bool metal = firstChannelMetal || program == QStringLiteral("m_ch2normglossalphaemissive.shaderbin")
        || program == QStringLiteral("int_m_ch2normglossalphaemissive.shaderbin");
    const bool channelTwo = program == QStringLiteral("ch2diffnormglossalphaemissive.shaderbin");
    const bool channelOne = program == QStringLiteral("ch1diffnormglossalphaemissive.shaderbin")
        || program == QStringLiteral("int_ch1diffnormglossalpha.shaderbin");
    const bool interiorDual = program == QStringLiteral("int_ch1_dualsurface_diffuse.shaderbin");
    const bool interiorEmbossed = program == QStringLiteral("int_ch1_normdiffuseopacity.shaderbin");
    const bool wetInteriorEmbossed = program == QStringLiteral("w_int_ch1_normdiffuseopacity.shaderbin");
    const bool mixedNormals = program == QStringLiteral("ch1ch2normlerpch1glossdiff.shaderbin");
    const bool interiorMixedNormals = program == QStringLiteral("int_ch1_dualsurface_simple.shaderbin");
    const bool separateBadgeMasks = program == QStringLiteral("badge_clc_2metaldiffuselerpch1.shaderbin");
    const float normalStrength = parameters.number(kNormalStrength, material.normalIntensity);
    LegacyMaterialSurface surface;
    if (metal || channelTwo || channelOne || interiorDual) {
        const bool firstChannel = channelOne || interiorDual || firstChannelMetal;
        const auto sharedUv = parameters.secondChannelUv();
        const bool mappedColor = firstChannel && !metal && parameters.enabled(0x04F8F9FA);
        const bool mappedSpecular = parameters.enabled(firstChannel ? 0x0F70E9CC : 0x6F80D6E1);
        const quint32 colorHash = firstChannel ? (interiorDual ? 0x57C321A6 : 0xF51639BE) : 0xEF5CCE09;
        const auto color = metal ? kBlack : parameters.color(colorHash, kWhite);
        setMaterialColor(material, color);
        const auto specular = parameters.color(interiorDual ? 0x440B1B33 : kSpecularColor, kDielectric);
        const auto tint = parameters.color(kSpecularTint, kWhite);
        setLegacySpecular(material, {specular[0] * tint[0], specular[1] * tint[1], specular[2] * tint[2]});
        if (mappedColor) {
            surface.diffuseMap = kLegacyColorMap;
            setMaterialColor(material, parameters.color(interiorDual ? 0x5AE9F6C5 : 0x1F3EB7A9, kWhite));
            setLegacyDiffuseUv(material, parameters.uv(0, 0x7590AB0C));
        } else if (mappedSpecular && (metal || material.metallic > 0.0f)) {
            surface.diffuseMap = kLegacySpecularMap;
            setMaterialColor(material, specular);
            setLegacyDiffuseUv(material, firstChannel ? parameters.uv(0, 0x3A458AF9) : sharedUv);
        }
        if (parameters.enabled(firstChannel ? kFirstChannelNormalEnabled : 0xFA9429D7)) {
            const auto uv = firstChannel ? parameters.uv(0, kNormalTiling)
                : metal ? parameters.secondChannelUv(kNormalTiling) : sharedUv;
            setLegacyNormal(material, surface, kNormalTexture, uv, normalStrength);
        }
        if (parameters.enabled(firstChannel ? kFirstChannelOpacityEnabled : 0x9FC7B8A8)
            && !(interiorDual && mappedColor)) {
            surface.opacityMap = kLegacyOpacityMap;
            material.alphaUv = firstChannel ? parameters.uv(0, kFirstChannelOpacityTiling) : sharedUv;
        }
        const float highGloss = parameters.number(interiorDual ? 0xD2AFDCA3 : kGlossHigh, material.gloss);
        const float lowGloss = parameters.number(interiorDual ? 0xDC5CC796 : kGlossLow, highGloss);
        material.gloss = std::clamp(interiorDual ? lowGloss : highGloss, 0.0f, 1.0f);
        if (parameters.enabled(firstChannel ? kFirstChannelGlossEnabled : 0xF5A4EEA0)) {
            surface.glossMap = kLegacyGlossMap;
            surface.glossUv = firstChannel ? parameters.uv(0, 0x66036634) : sharedUv;
            surface.glossRange = {std::clamp(lowGloss, 0.0f, 1.0f), std::clamp(highGloss, 0.0f, 1.0f)};
        }
        if (metal) applyLegacyMetalEmission(material, surface, parameters, sharedUv, firstChannel);
        surface.diffuseAlpha = interiorDual && mappedColor;
    } else if (separateBadgeMasks || program == QStringLiteral("badge_clc_diffusemetallerpch1masknorm.shaderbin")) {
        applyLegacyBadgeSurface(material, surface, parameters, normalStrength, separateBadgeMasks);
    } else if (program == QStringLiteral("badge_ch1difnormglossao.shaderbin")) {
        applyLegacyBadgeGlossSurface(material, surface, parameters, normalStrength);
    } else if (program == QStringLiteral("clc_metalch2normalch1mask.shaderbin")) {
        if (parameters.enabled(0x05A401E7)) {
            surface.diffuseMap = 0xCA4E3249;
            setMaterialColor(material, kWhite);
            setLegacyDiffuseUv(material, parameters.uv(1, 0x31048129));
        }
        if (parameters.enabled(kDetailNormalEnabled)) {
            setLegacyNormal(material, surface, kNormalTexture, parameters.uv(1, kNormalTiling), normalStrength);
        }
        if (parameters.enabled(0xE5BC49CB)) {
            surface.opacityMap = kLegacyOpacityMap;
            material.alphaUv = parameters.uv(0);
        }
    } else if (program == QStringLiteral("int_ch2_norm_diffuse.shaderbin")) {
        surface.diffuseMap = 0x29C58482;
        setMaterialColor(material, kWhite);
        setLegacyDiffuseUv(material, parameters.uv(1, 0xD9B958A1));
        if (parameters.enabled(kDetailNormalEnabled)) {
            setLegacyNormal(material, surface, kDetailNormalMap, parameters.uv(1, kDetailNormalTiling),
                            parameters.number(kDetailNormalStrength, 1.0f));
        }
    } else if (interiorEmbossed || wetInteriorEmbossed) {
        const bool mappedColor = parameters.enabled(0x05A401E7);
        const bool opacity = parameters.enabled(kFirstChannelOpacityEnabled);
        if (mappedColor) {
            surface.diffuseMap = 0xBCAB0B74;
            surface.diffuseAlpha = opacity;
            setMaterialColor(material, kWhite);
            setLegacyDiffuseUv(material, parameters.uv(0, 0x087C2C69));
        } else if (opacity) {
            surface.opacityMap = kLegacyOpacityMap;
            material.alphaUv = parameters.uv(0, kFirstChannelOpacityTiling);
        }
        const quint32 primaryReflectance = wetInteriorEmbossed ? kSimpleSpecularColor : 0x8ECDBAD4;
        const quint32 reflectance = parameters.enabled(0x989B026F) ? primaryReflectance : 0x2A257AE0;
        if (!mappedColor) setLegacySpecular(material, parameters.color(reflectance, kDielectric));
        if (parameters.enabled(kFirstChannelNormalEnabled)) {
            setLegacyNormal(material, surface, kNormalTexture, parameters.uv(0, kNormalTiling), normalStrength);
        }
    } else if (mixedNormals || interiorMixedNormals) {
        setMaterialColor(material, parameters.color(0xEF5CCE09, kWhite));
        setLegacySpecular(material, parameters.color(kSpecularColor, kDielectric));
        if (!interiorMixedNormals || parameters.enabled(kDetailNormalEnabled)) {
            setLegacyNormal(material, surface, kDetailNormalMap, parameters.uv(1, kDetailNormalTiling),
                            parameters.number(kDetailNormalStrength, 1.0f));
        }
        if (interiorMixedNormals && parameters.enabled(kFirstChannelOpacityEnabled)) {
            surface.opacityMap = kLegacyOpacityMap;
            material.alphaUv = parameters.uv(0, kFirstChannelOpacityTiling);
        }
        if (parameters.enabled(kFirstChannelGlossEnabled)) {
            surface.glossMap = 0x5139CFD8;
            surface.glossUv = parameters.uv(0, 0x223BA0E9);
            surface.glossRange = {parameters.number(kGlossLow, material.gloss), material.gloss};
        }
    } else if (program == QStringLiteral("int_ch2_simplenormalao_glossvar.shaderbin")
        || program == QStringLiteral("int_ch2_simplenormalao_glossvar_001.shaderbin")
        || program == QStringLiteral("int_ch2_simplenormalao_glossvar_002.shaderbin")) {
        setLegacySpecular(material, parameters.color(kSimpleSpecularColor, kDielectric));
        setLegacyNormal(material, surface, kNormalTexture, parameters.secondChannelUv(kNormalTiling), normalStrength);
    } else if (program == QStringLiteral("simplediffuse.shaderbin")
        || program == QStringLiteral("int_simplediffuse.shaderbin")
        || program == QStringLiteral("w_int_simplediffuse.shaderbin")
        || program == QStringLiteral("ext_simplemetal.shaderbin")) {
        setLegacySpecular(material, parameters.color(kSimpleSpecularColor, kDielectric));
    } else if (program == QStringLiteral("badge_simplemetal.shaderbin")) {
        applyLegacySimpleBadgeSurface(material, surface, parameters, normalStrength);
    } else if (program == QStringLiteral("lod1interior.shaderbin")) {
        setMaterialColor(material, kWhite);
        surface.diffuseMap = kLegacyColorMap;
        surface.diffuseAlpha = true;
        setLegacyDiffuseUv(material, parameters.uv(0));
    } else if (program == QStringLiteral("sparetire.shaderbin")) {
        constexpr quint32 kRubberColor = 0xBA9B668F;
        constexpr quint32 kTreadNormal = 0xC751A20B;
        constexpr quint32 kTreadNormalTiling = 0x43FEFDA3;
        constexpr quint32 kTreadNormalStrength = 0x0D5DC750;
        setMaterialColor(material, parameters.color(kRubberColor, material.baseColor));
        setLegacyNormal(material, surface, kTreadNormal, parameters.uv(0, kTreadNormalTiling),
                        parameters.number(kTreadNormalStrength, 1.0f));
    } else if (program == QStringLiteral("carpaint_standard.shaderbin")) {
        if (!applyLegacyPaintSurface(material, surface, parameters)) return;
    } else {
        return;
    }
    material.legacySurface = surface;
    material.opacity = 1.0f;
}

void applyCarbonWeave(ModelMaterial &material, const MaterialParameters &parameters) {
    using namespace material_hashes::parameter;
    constexpr quint32 kMaskedLivery = 3198824316;
    constexpr float kDefaultTint = 0.009721218f;
    constexpr float kDefaultWeaveTiling = 50.0f;
    constexpr std::array kTint = {kDefaultTint, kDefaultTint, kDefaultTint};
    const auto tintA = parameters.color(kWeaveColorTintA, kTint);
    const auto tintB = parameters.color(kWeaveColorTintB, kTint);
    const float roughness = parameters.number(kWeaveRoughness, 1.0f - material.gloss);
    material.weave = MaterialWeave{tintA, tintB, parameters.enabled(kMaskedLivery, true)};
    material.diffuseUv = material.alphaUv = material.surfaceUv = MaterialUvMapping{};
    material.normalUv = MaterialUvMapping{{parameters.number(kTextureTilingU, kDefaultWeaveTiling),
                                           parameters.number(kTextureTilingV, kDefaultWeaveTiling)},
                                          1, parameters.number(kUvOrientation, 0.0f)};
    material.normalIntensity = parameters.number(kWeaveNormalIntensity, material.normalIntensity);
    material.hasMetallic = true;
    material.metallic = std::clamp(parameters.number(kWeaveMetalness, material.metallic), 0.0f, 1.0f);
    material.gloss = 1.0f - std::clamp(roughness, 0.0f, 1.0f);
    setMaterialColor(material, {(tintA[0] + tintB[0]) * 0.5f,
                                (tintA[1] + tintB[1]) * 0.5f,
                                (tintA[2] + tintB[2]) * 0.5f});
}

void applyCurrentShaderSurface(ModelMaterial &material, const QString &program) {
    using namespace material_hashes::parameter;
    constexpr quint32 kUniqueLivery = 4051335103;
    constexpr quint32 kStandardNormalStrength = 3446342299;
    constexpr quint32 kRoughnessShift = 1311252771;
    const MaterialParameters parameters(material);
    const bool carbon = program == QStringLiteral("car_carbonfiber.shaderbin");
    if (carbon || program == QStringLiteral("car_automotive_paint.shaderbin")) {
        material.useDiffuseTexture = parameters.enabled(kUniqueLivery);
        if (carbon) {
            applyCarbonWeave(material, parameters);
            material.roughnessShift = parameters.number(kRoughnessShift, material.roughnessShift);
        }
        return;
    }
    const bool standard = program == QStringLiteral("car_standard.shaderbin");
    if (!standard && program != QStringLiteral("car_standard_coated.shaderbin")
        && program != QStringLiteral("car_standard_emissive.shaderbin")
        && program != QStringLiteral("car_standard_fabric.shaderbin")) return;

    constexpr quint32 kOverrideDiffuseTiling = 3102088726;
    constexpr quint32 kBaseColorTilingOverride = 289723392;
    constexpr quint32 kOverrideNormalTiling = 452999269;
    constexpr quint32 kNormalTilingOverride = 4085508950;
    constexpr quint32 kOverrideSurfaceTiling = 3972970745;
    constexpr quint32 kSurfaceTilingOverride = 2343278259;
    constexpr quint32 kOverrideAlphaTiling = 151699307;
    constexpr quint32 kAlphaTilingOverride = 1288535941;
    const int channel = parameters.enabled(kPrimaryUvSelection) ? 0 : 1;
    const float rotation = parameters.number(kUvOrientation, 0.0f);
    const MaterialUvMapping common{{parameters.number(kTextureTilingU, 1.0f),
                                    parameters.number(kTextureTilingV, 1.0f)}, channel, rotation};
    const auto mapped = [&](quint32 enableOverride, quint32 overrideTiling) {
        if (!parameters.enabled(enableOverride)) return common;
        auto mapping = parameters.uv(channel, overrideTiling);
        mapping.rotationDegrees = rotation;

        return mapping;
    };
    material.diffuseUv = mapped(kOverrideDiffuseTiling, standard ? kTextureTiling : kBaseColorTilingOverride);
    material.normalUv = mapped(kOverrideNormalTiling, kNormalTilingOverride);
    material.surfaceUv = mapped(kOverrideSurfaceTiling, kSurfaceTilingOverride);
    material.alphaUv = mapped(kOverrideAlphaTiling, kAlphaTilingOverride);
    material.normalIntensity = parameters.number(kStandardNormalStrength, material.normalIntensity);
    material.roughnessShift = parameters.number(kRoughnessShift, material.roughnessShift);
}

void applyEffectiveParameters(ModelMaterial &material) {
    const auto key = [](const ModelMaterialParameter &parameter) {
        return (static_cast<quint64>(parameter.nameHash) << 8) | static_cast<quint8>(parameter.type);
    };
    QHash<quint64, const ModelMaterialParameter *> effective;
    for (const auto &parameter : material.parameters) effective.insert(key(parameter), &parameter);
    std::vector<ModelMaterialParameter> parameters;
    parameters.reserve(effective.size());
    for (auto &parameter : material.parameters) {
        if (effective.value(key(parameter)) != &parameter) continue;
        applyPreviewParameter(material, parameter);
        parameters.push_back(std::move(parameter));
    }
    material.parameters = std::move(parameters);
    QString resource = material.resourcePath.toLower();
    resource.replace(QLatin1Char('\\'), QLatin1Char('/'));
    material.glass |= resource.contains(QStringLiteral("/glass/"))
        || resource.contains(QStringLiteral("/exterior_glass/"))
        || resource.contains(QStringLiteral("/exterior_litglass/"))
        || resource.contains(QStringLiteral("/rad_glm_"))
        || resource.contains(QStringLiteral("/gls_"));
    const auto shader = std::find_if(material.linkedPaths.crbegin(), material.linkedPaths.crend(),
        [](const QString &path) { return path.endsWith(QStringLiteral(".shaderbin"), Qt::CaseInsensitive); });
    if (shader == material.linkedPaths.crend()) return;
    QString shaderPath = shader->toLower();
    shaderPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString program = shaderPath.section(QLatin1Char('/'), -1);
    if (material.legacyShaderBindings.value_or(false)) {
        applyLegacyShaderSurface(material, program);
    } else {
        applyCurrentShaderSurface(material, program);
    }
    if (program == QStringLiteral("glm_ch2_illuminatingglass_ch1mask.shaderbin")
        || program == QStringLiteral("glm_ch2bumpch1mask.shaderbin")
        || program == QStringLiteral("gls_ch2glossopacityemissive.shaderbin")) {
        material.materialUvChannel = 1;
        MaterialUvMapping alpha = material.alphaUv.value_or(MaterialUvMapping{});
        MaterialUvMapping emission = material.emissiveUv.value_or(MaterialUvMapping{});
        alpha.channel = emission.channel = 1;
        material.alphaUv = alpha;
        material.emissiveUv = emission;
    }
}

std::shared_ptr<ModelMaterial> decodeMaterialFromBundle(
    const ModelBundle &bundle, const QString &name) {
    auto material = std::make_shared<ModelMaterial>();
    material->name = name;
    std::optional<bool> legacyShaderBindings;
    bool shaderBundle = false;
    bool legacyVariants = false;

    for (const BundleBlobRecord &child : bundle.blobs) {
        if (child.tag == bundle_tags::MaterialResource) {
            Cursor cursor(child.data);
            material->resourcePath = cursor.string7();
        } else if (child.tag == bundle_tags::MaterialLinks) {
            Cursor cursor(child.data);
            material->linkedPaths.push_back(cursor.string7());
            if (child.isAtLeastVersion(1, 1)) {
                material->linkedPaths.push_back(cursor.string7());
            }
            if (child.isAtLeastVersion(1, 2)) {
                material->linkedPaths.push_back(cursor.string7());
            }
        } else if (child.tag == bundle_tags::TextureParameterMapping) {
            appendShaderTextures(*material, child);
            shaderBundle = true;
        } else if (child.tag == bundle_tags::ShaderVersion) {
            legacyShaderBindings = child.versionMajor == 1 && child.versionMinor == 1;
        } else if (child.tag == bundle_tags::ShaderVariants) {
            legacyVariants = child.versionMajor == 1 && child.versionMinor == 3;
        }
    }
    for (const BundleBlobRecord &child : bundle.blobs) {
        if (child.tag == bundle_tags::DefaultMaterialParameters) {
            appendParameters(*material, child);
        }
    }
    for (const BundleBlobRecord &child : bundle.blobs) {
        if (child.tag == bundle_tags::MaterialParameters) {
            appendParameters(*material, child);
        }
    }
    if (shaderBundle) {
        if (!legacyShaderBindings && legacyVariants) legacyShaderBindings = true;
        material->legacyShaderBindings = legacyShaderBindings;
        for (auto &parameter : material->parameters) parameter.shaderDefault = true;
    }
    applyEffectiveParameters(*material);

    return material;
}

} // namespace

std::shared_ptr<ModelMaterial> decodeModelMaterial(const BundleBlobRecord &blob) {
    if (blob.tag != bundle_tags::MaterialInstance || blob.data.isEmpty()) {
        return {};
    }
    const ModelBundle nested = parseModelBundle(blob.data);
    return decodeMaterialFromBundle(nested, blob.name);
}

std::shared_ptr<ModelMaterial> decodeMaterialBundle(const QByteArray &bytes) {
    if (bytes.isEmpty()) {
        return {};
    }
    return decodeMaterialFromBundle(parseModelBundle(bytes), {});
}

std::shared_ptr<ModelMaterial> mergeModelMaterialDefaults(
    const ModelMaterial &defaults, const ModelMaterial &instance) {
    auto merged = std::make_shared<ModelMaterial>();
    merged->name = instance.name;
    merged->resourcePath = instance.resourcePath.isEmpty() ? defaults.resourcePath : instance.resourcePath;
    merged->linkedPaths = defaults.linkedPaths;
    merged->linkedPaths.append(instance.linkedPaths);
    merged->parameters.reserve(defaults.parameters.size() + instance.parameters.size());
    merged->shaderTextures = instance.shaderTextures.empty() ? defaults.shaderTextures : instance.shaderTextures;
    merged->legacyShaderBindings = instance.legacyShaderBindings
        ? instance.legacyShaderBindings : defaults.legacyShaderBindings;

    merged->parameters.insert(merged->parameters.end(), defaults.parameters.begin(), defaults.parameters.end());
    merged->parameters.insert(merged->parameters.end(), instance.parameters.begin(), instance.parameters.end());
    merged->overriddenByScene = instance.overriddenByScene;
    merged->resolvedFromResource = defaults.resolvedFromResource || instance.resolvedFromResource;
    merged->resolvedFromShader = defaults.resolvedFromShader || instance.resolvedFromShader;
    applyEffectiveParameters(*merged);

    return merged;
}

std::optional<quint32> shaderTextureParameter(
    const ModelMaterial &material, const ModelMaterialParameter &parameter) {
    if (material.shaderTextures.empty()) return parameter.nameHash;
    const auto &bindings = material.shaderTextures;
    const auto exact = std::find_if(bindings.cbegin(), bindings.cend(), [&](const auto &binding) {
        return binding.nameHash == parameter.nameHash;
    });
    if (exact != bindings.cend()) return exact->nameHash;
    const auto alias = std::find_if(bindings.cbegin(), bindings.cend(), [&](const auto &binding) {
        return (!parameter.parameterId.isNull() && parameter.parameterId == binding.parameterId)
            || parameter.aliasHash == binding.nameHash;
    });

    return alias == bindings.cend() ? std::nullopt : std::optional(alias->nameHash);
}

float lampGlassPreviewOpacity(const ModelMaterial *material, bool colored) {
    constexpr float kMaskedOpacity = 0.50f;
    constexpr float kColoredOpacity = 0.34f;
    constexpr float kClearOpacity = 0.20f;

    if (material != nullptr) {
        if (material->glassOpacity.primary) return *material->glassOpacity.primary;
        if (!material->resolvedFromResource && !material->resolvedFromShader
            && material->glassOpacity.useAlphaTexture.value_or(true)) {
            const auto &parameters = material->parameters;
            const bool hasOpacityMap = std::any_of(parameters.cbegin(), parameters.cend(),
                [&](const ModelMaterialParameter &parameter) {
                    if (parameter.type != ModelMaterialParameterType::Texture2D
                        || parameter.texturePath.isEmpty()) return false;
                    const auto binding = shaderTextureParameter(*material, parameter);

                    return binding && material_hashes::contains(material_hashes::parameter::kAlphaTexture, *binding);
                });
            if (hasOpacityMap) return kMaskedOpacity;
        }
    }

    return colored ? kColoredOpacity : kClearOpacity;
}

} // namespace fls
