#include "model_material.h"

#include "binary_io.h"
#include "material_hashes.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fh6 {
namespace {

class Cursor {
public:
    explicit Cursor(const QByteArray &bytes) : bytes_(bytes) {}

    quint8 u8() {
        require(1);
        return static_cast<quint8>(bytes_[pos_++]);
    }

    quint16 u16() {
        require(2);
        const quint16 value = detail::readLeU16(bytes_, pos_);
        pos_ += 2;
        return value;
    }

    quint32 u32() {
        require(4);
        const quint32 value = detail::readLeU32(bytes_, pos_);
        pos_ += 4;
        return value;
    }

    qint32 i32() { return static_cast<qint32>(u32()); }

    float f32() {
        require(4);
        const float value = detail::readLeFloat(bytes_, pos_);
        pos_ += 4;
        return value;
    }

    void skip(int count) {
        require(count);
        pos_ += count;
    }

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
                const QString value = QString::fromUtf8(bytes_.constData() + pos_, static_cast<int>(length));
                pos_ += static_cast<int>(length);
                return value;
            }
            shift += 7;
        }
        throw std::runtime_error("invalid material string length");
    }

private:
    void require(int count) const {
        if (count < 0 || pos_ < 0 || pos_ + count > bytes_.size()) {
            throw std::runtime_error("material data is truncated");
        }
    }

    const QByteArray &bytes_;
    int pos_ = 0;
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
        cursor.u32();
    }
    parameter.type = static_cast<ModelMaterialParameterType>(cursor.u8());
    if (parameter.versionMajor >= 3) {
        cursor.skip(16);
    }

    switch (parameter.type) {
    case ModelMaterialParameterType::Vector:
    case ModelMaterialParameterType::Color:
    case ModelMaterialParameterType::Swizzle:
    case ModelMaterialParameterType::FunctionRange:
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

void applyPreviewParameter(ModelMaterial &material, const ModelMaterialParameter &parameter) {
    const bool vectorValue = parameter.type == ModelMaterialParameterType::Vector
        || parameter.type == ModelMaterialParameterType::Color;
    if (vectorValue
        && material_hashes::contains(material_hashes::parameter::kBaseColor, parameter.nameHash)) {
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
        && parameter.nameHash == material_hashes::parameter::kTextureTiling
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
    for (const ModelMaterialParameter &parameter : decoded) {
        applyPreviewParameter(material, parameter);
        material.parameters.push_back(parameter);
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
    merged->resourcePath = instance.resourcePath;
    merged->linkedPaths = defaults.linkedPaths;
    merged->linkedPaths.append(instance.linkedPaths);
    merged->parameters.reserve(defaults.parameters.size() + instance.parameters.size());

    for (const ModelMaterialParameter &parameter : defaults.parameters) {
        applyPreviewParameter(*merged, parameter);
        merged->parameters.push_back(parameter);
    }
    for (const ModelMaterialParameter &parameter : instance.parameters) {
        applyPreviewParameter(*merged, parameter);
        merged->parameters.push_back(parameter);
    }
    return merged;
}

} // namespace fh6
