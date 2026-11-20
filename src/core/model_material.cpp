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
    if (!bundle.blobsWithTag(bundle_tags::TextureParameterMapping).empty()) {
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
