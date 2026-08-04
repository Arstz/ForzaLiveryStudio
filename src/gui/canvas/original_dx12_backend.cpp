#include "original_dx12_backend.h"

#include "original_shader_garage.h"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef Q_OS_WIN

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

#endif

namespace gui {

OriginalDx12Camera originalDx12SceneCamera(
    const fh6::OriginalShaderGarageScene &scene) {
    OriginalDx12Camera camera;
    if (scene.draws.empty()) {
        return camera;
    }

    QVector3D minimum(
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    QVector3D maximum(
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());
    const bool hasCar = std::any_of(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.name.startsWith(QStringLiteral("car/"));
        });
    for (const fh6::OriginalShaderGarageDraw &draw : scene.draws) {
        if (hasCar && !draw.name.startsWith(QStringLiteral("car/"))) {
            continue;
        }
        const fh6::ModelVec3 low = draw.geometry.boundsMin;
        const fh6::ModelVec3 high = draw.geometry.boundsMax;
        for (int mask = 0; mask < 8; ++mask) {
            const fh6::ModelVec3 corner = draw.placement.transformPoint({
                (mask & 1) != 0 ? high.x : low.x,
                (mask & 2) != 0 ? high.y : low.y,
                (mask & 4) != 0 ? high.z : low.z});
            minimum.setX(std::min(minimum.x(), corner.x));
            minimum.setY(std::min(minimum.y(), corner.y));
            minimum.setZ(std::min(minimum.z(), corner.z));
            maximum.setX(std::max(maximum.x(), corner.x));
            maximum.setY(std::max(maximum.y(), corner.y));
            maximum.setZ(std::max(maximum.z(), corner.z));
        }
    }
    const QVector3D extent = maximum - minimum;
    const float radius = std::max(1.0f, extent.length() * 0.5f);
    camera.target = (minimum + maximum) * 0.5f;
    camera.target.setY(minimum.y() + extent.y() * 0.42f);
    camera.verticalFovDegrees = 40.0f;
    const float halfFovRadians = camera.verticalFovDegrees
        * 0.5f * static_cast<float>(3.14159265358979323846 / 180.0);
    const float distance = radius / std::tan(halfFovRadians) * 1.08f;
    camera.position = camera.target
        + QVector3D(0.65f, 0.28f, 1.0f).normalized() * distance;
    camera.nearPlane = std::max(0.05f, radius * 0.002f);
    camera.farPlane = hasCar ? 60.0f : distance + radius * 4.0f;
    return camera;
}

void panOriginalDx12Camera(
    OriginalDx12Camera *camera, const QPointF &pixelDelta,
    const QSize &viewportSize) {
    if (camera == nullptr || !camera->valid() || viewportSize.height() <= 0) {
        return;
    }
    const QVector3D forward = (camera->target - camera->position).normalized();
    QVector3D right = QVector3D::crossProduct(forward, camera->up).normalized();
    if (right.isNull()) {
        return;
    }
    const QVector3D viewUp = QVector3D::crossProduct(right, forward).normalized();
    constexpr float kDegreesToRadians =
        static_cast<float>(3.14159265358979323846 / 180.0);
    const float worldPerPixel =
        2.0f * (camera->target - camera->position).length()
        * std::tan(camera->verticalFovDegrees * 0.5f * kDegreesToRadians)
        / static_cast<float>(viewportSize.height());
    const QVector3D movement = right * (-pixelDelta.x() * worldPerPixel)
        + viewUp * (pixelDelta.y() * worldPerPixel);
    camera->position += movement;
    camera->target += movement;
}

#ifdef Q_OS_WIN
namespace {

struct Binding {
    UINT shaderRegister;
    UINT registerSpace;
};

constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr std::array<float, 4> kClearColor = {0.125f, 0.25f, 0.5f, 1.0f};
constexpr UINT kShadowMapSize = 2048;
constexpr UINT kShadowDescriptorIndex = 119;
constexpr UINT kDropShadowDescriptorIndex = 121;
constexpr UINT kSceneDepthDescriptorIndex = 116;
constexpr UINT kHdrSceneDescriptorIndex = 122;
constexpr UINT kGlassBackDepthDescriptorIndex = 113;
constexpr std::array<UINT, 2> kTemporalHistoryDescriptorIndices = {110, 111};
constexpr std::array<UINT, 2> kLocalShadowDescriptorIndices = {114, 115};
constexpr float kShadowStrength = 1.0f;
constexpr float kDropShadowStrength = 0.35f;
constexpr float kShadowDepthBias = 0.0015f;
constexpr UINT kFixedDescriptorCount = 176;
constexpr UINT kMaterialDescriptorStart = 124;
constexpr UINT kMaterialDescriptorCount = 21;

struct Vertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float uv[2];
    float uv2[2];
};

struct Geometry {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct DrawResources {
    Geometry geometry;
    QVector3D center;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    UINT materialDescriptorStart = kMaterialDescriptorStart;
    UINT samplerDescriptorIndex = 0;
    ComPtr<ID3D12Resource> materialConstants;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> diffuseTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> alphaTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> normalTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> weaveMaskTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> weaveNormalTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> clearCoatNormalTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> surfaceTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> tireHeightAoTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> aoTexture;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> emissiveTexture;
    std::array<float, 3> baseColor = {0.55f, 0.55f, 0.55f};
    std::array<float, 3> secondaryPaintColor = {0.55f, 0.55f, 0.55f};
    std::array<float, 3> flakeColor = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> emissiveColor = {0.0f, 0.0f, 0.0f};
    float opacity = 1.0f;
    float gloss = 0.45f;
    float roughnessShift = 0.0f;
    float metallic = 0.0f;
    float flakeCoverage = 0.0f;
    float flakeRoughness = 0.4f;
    float glitterIntensity = 1.4f;
    float glancingFlopStrength = 0.0f;
    float glancingFlopPower = 2.0f;
    float uTiling = 1.0f;
    float vTiling = 1.0f;
    float detailUTiling = 1.0f;
    float detailVTiling = 1.0f;
    float normalIntensity = 1.0f;
    float weaveNormalIntensity = 1.0f;
    float clearCoatNormalUTiling = 1.0f;
    float clearCoatNormalVTiling = 1.0f;
    std::array<float, 3> weaveColorTintA = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> weaveColorTintB = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> clearCoatTint = {1.0f, 1.0f, 1.0f};
    float clearCoatCoverage = 0.0f;
    float clearCoatRoughness = 0.1f;
    bool translucent = false;
    bool glancingFlopEnabled = false;
    bool selfShadowCaster = false;
    bool dropShadowCaster = false;
    bool visible = true;
    bool reflectionProbeShell = false;
    bool clearCoatOnLivery = true;
    bool liveryBaseTexture = false;
    quint32 liveryAllowedSides = 0;
    quint32 drawGroups = 0;
    bool interiorWindshield = false;
    fh6::ModelMaterialSampler sampler;
    fh6::ModelShaderFamily shaderFamily = fh6::ModelShaderFamily::Generic;
    int liverySideCount = 0;
    std::array<std::array<float, 4>, fh6::kLiverySideCount> liverySourceRegions{};
    std::array<std::array<float, 4>, fh6::kLiverySideCount> liveryPaintRegions{};
    std::array<std::array<float, 3>, fh6::kLiverySideCount> liveryFacing{};
    fh6::OriginalShaderSurfaceFamily family =
        fh6::OriginalShaderSurfaceFamily::Default;
};

QVector3D geometryCenter(const Geometry &geometry) {
    QVector3D center;
    if (geometry.vertices.empty()) {
        return center;
    }
    for (const Vertex &vertex : geometry.vertices) {
        center += QVector3D(
            vertex.position[0], vertex.position[1], vertex.position[2]);
    }
    return center / static_cast<float>(geometry.vertices.size());
}

std::vector<const DrawResources *> drawPassOrder(
    const std::vector<DrawResources> &draws, bool translucent,
    const QVector3D &cameraPosition) {
    std::vector<const DrawResources *> ordered;
    ordered.reserve(draws.size());
    for (const DrawResources &draw : draws) {
        if (draw.visible && draw.translucent == translucent) {
            ordered.push_back(&draw);
        }
    }
    if (translucent) {
        std::stable_sort(
            ordered.begin(), ordered.end(), [&](const auto *left, const auto *right) {
                constexpr quint32 kReflectionGroups =
                    fh6::car_draw_groups::kWindshieldReflection
                    | fh6::car_draw_groups::kWindshieldReflectionDriverless;
                const bool leftReflection =
                    (left->drawGroups & kReflectionGroups) != 0;
                const bool rightReflection =
                    (right->drawGroups & kReflectionGroups) != 0;
                if (leftReflection != rightReflection) {
                    return !leftReflection;
                }
                return (left->center - cameraPosition).lengthSquared()
                    > (right->center - cameraPosition).lengthSquared();
            });
    }
    return ordered;
}

struct UploadedTexture {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
    UINT mipLevels = 1;
};

QString hresultText(HRESULT result) {
    return QStringLiteral("HRESULT 0x%1").arg(
        static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;

    return properties;
}

D3D12_RESOURCE_DESC bufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return description;
}

ComPtr<ID3D12Resource> createBuffer(
    ID3D12Device *device, UINT64 size, D3D12_HEAP_TYPE type,
    D3D12_RESOURCE_STATES state) {
    const D3D12_HEAP_PROPERTIES heap = heapProperties(type);
    const D3D12_RESOURCE_DESC description = bufferDescription(size);
    ComPtr<ID3D12Resource> resource;

    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description, state,
            nullptr, IID_PPV_ARGS(&resource)))) {
        return {};
    }

    return resource;
}

bool uploadBuffer(ID3D12Resource *resource, const void *data, std::size_t size) {
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};

    if (FAILED(resource->Map(0, &noRead, &mapped))) {
        return false;
    }
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);

    return true;
}

D3D12_RESOURCE_BARRIER transition(
    ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;

    return barrier;
}

Geometry prepareGeometry(
    const fh6::CarModel &model, const fh6::ModelMat4 &placement,
    int diffuseUvChannel = 0, bool rawMaterialUv = false,
    int materialUvChannel = 0, float materialUvRotationDegrees = 0.0f) {
    Geometry result;
    for (const fh6::CarMesh &mesh : model.meshes) {
        const auto transformedUv = [&mesh](
                                       std::size_t channel, std::size_t vertex,
                                       bool raw = false) {
            fh6::ModelVec2 uv{};
            if (channel >= mesh.uvChannels.size()
                || vertex >= mesh.uvChannels[channel].size()) {
                return uv;
            }
            uv = mesh.uvChannels[channel][vertex];
            if (!raw && channel < mesh.texCoordTransforms.size()) {
                const fh6::TexCoordTransform &transform =
                    mesh.texCoordTransforms[channel];
                uv.u = uv.u * transform.scaleU + transform.offsetU;
                uv.v = uv.v * transform.scaleV + transform.offsetV;
            }
            return uv;
        };
        const std::size_t detailUvChannel = materialUvChannel >= 0
            ? static_cast<std::size_t>(materialUvChannel) : 0;
        const float materialUvRotation =
            materialUvRotationDegrees * 0.01745329251994329577f;
        const float materialUvCosine = std::cos(materialUvRotation);
        const float materialUvSine = std::sin(materialUvRotation);
        const auto detailUv = [&](std::size_t vertex) {
            const fh6::ModelVec2 uv = transformedUv(
                detailUvChannel, vertex,
                rawMaterialUv && detailUvChannel == 0);
            return fh6::ModelVec2{
                materialUvCosine * uv.u - materialUvSine * uv.v,
                materialUvSine * uv.u + materialUvCosine * uv.v};
        };
        const fh6::ModelMat4 transform =
            fh6::matMul(mesh.boneTransform, placement);
        const bool hasAuthoredTangents =
            detailUvChannel < mesh.tangentChannels.size()
            && mesh.tangentChannels[detailUvChannel].size() == mesh.positions.size();
        const float transformDeterminant =
            transform.m[0] * (transform.m[5] * transform.m[10]
                              - transform.m[6] * transform.m[9])
            - transform.m[1] * (transform.m[4] * transform.m[10]
                                - transform.m[6] * transform.m[8])
            + transform.m[2] * (transform.m[4] * transform.m[9]
                                - transform.m[5] * transform.m[8]);
        const std::uint32_t base = static_cast<std::uint32_t>(result.vertices.size());
        std::vector<std::array<float, 3>> tangents(
            mesh.positions.size(), {0.0f, 0.0f, 0.0f});
        std::vector<std::array<float, 3>> bitangents(
            mesh.positions.size(), {0.0f, 0.0f, 0.0f});
        // The original carbon-fibre shader uses transformed UV1 plus its
        // UV_Orientation parameter; ordinary materials retain UV0.
        if (detailUvChannel < mesh.uvChannels.size()) {
            const auto &uvs = mesh.uvChannels[detailUvChannel];
            for (std::size_t triangle = 0;
                 triangle + 2 < mesh.indices.size(); triangle += 3) {
                const std::array<std::uint32_t, 3> indices = {
                    mesh.indices[triangle], mesh.indices[triangle + 1],
                    mesh.indices[triangle + 2]};
                if (indices[0] >= mesh.positions.size()
                    || indices[1] >= mesh.positions.size()
                    || indices[2] >= mesh.positions.size()
                    || indices[0] >= uvs.size()
                    || indices[1] >= uvs.size()
                    || indices[2] >= uvs.size()) {
                    continue;
                }
                const fh6::ModelVec3 position0 =
                    transform.transformPoint(mesh.positions[indices[0]]);
                const fh6::ModelVec3 position1 =
                    transform.transformPoint(mesh.positions[indices[1]]);
                const fh6::ModelVec3 position2 =
                    transform.transformPoint(mesh.positions[indices[2]]);
                const std::array<float, 3> edge1 = {
                    position1.x - position0.x, position1.y - position0.y,
                    position1.z - position0.z};
                const std::array<float, 3> edge2 = {
                    position2.x - position0.x, position2.y - position0.y,
                    position2.z - position0.z};
                const fh6::ModelVec2 uv0 = detailUv(indices[0]);
                const fh6::ModelVec2 uv1 = detailUv(indices[1]);
                const fh6::ModelVec2 uv2 = detailUv(indices[2]);
                const float deltaU1 = uv1.u - uv0.u;
                const float deltaV1 = uv1.v - uv0.v;
                const float deltaU2 = uv2.u - uv0.u;
                const float deltaV2 = uv2.v - uv0.v;
                const float determinant = deltaU1 * deltaV2 - deltaU2 * deltaV1;
                if (std::abs(determinant) <= 1e-8f) {
                    continue;
                }
                const float inverse = 1.0f / determinant;
                const std::array<float, 3> tangent = {
                    (edge1[0] * deltaV2 - edge2[0] * deltaV1) * inverse,
                    (edge1[1] * deltaV2 - edge2[1] * deltaV1) * inverse,
                    (edge1[2] * deltaV2 - edge2[2] * deltaV1) * inverse};
                const std::array<float, 3> bitangent = {
                    (edge2[0] * deltaU1 - edge1[0] * deltaU2) * inverse,
                    (edge2[1] * deltaU1 - edge1[1] * deltaU2) * inverse,
                    (edge2[2] * deltaU1 - edge1[2] * deltaU2) * inverse};
                for (const std::uint32_t index : indices) {
                    for (int component = 0; component < 3; ++component) {
                        tangents[index][component] += tangent[component];
                        bitangents[index][component] += bitangent[component];
                    }
                }
            }
        }
        for (std::size_t index = 0; index < mesh.positions.size(); ++index) {
            const fh6::ModelVec3 position =
                transform.transformPoint(mesh.positions[index]);
            fh6::ModelVec3 normal = transform.transformVector(
                index < mesh.normals.size() ? mesh.normals[index]
                                            : fh6::ModelVec3{0.0f, 1.0f, 0.0f});
            const float normalLength = std::sqrt(
                normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
            if (normalLength > 1e-6f) {
                normal.x /= normalLength;
                normal.y /= normalLength;
                normal.z /= normalLength;
            }
            std::array<float, 3> tangent = tangents[index];
            float authoredHandedness = 1.0f;
            if (hasAuthoredTangents) {
                const fh6::ModelVec4 &authored =
                    mesh.tangentChannels[detailUvChannel][index];
                const fh6::ModelVec3 transformed =
                    transform.transformVector({authored.x, authored.y, authored.z});
                tangent = {transformed.x, transformed.y, transformed.z};
                authoredHandedness = authored.w
                    * (transformDeterminant < 0.0f ? -1.0f : 1.0f);
            }
            const float normalDotTangent = normal.x * tangent[0]
                + normal.y * tangent[1] + normal.z * tangent[2];
            tangent[0] -= normal.x * normalDotTangent;
            tangent[1] -= normal.y * normalDotTangent;
            tangent[2] -= normal.z * normalDotTangent;
            float tangentLength = std::sqrt(
                tangent[0] * tangent[0] + tangent[1] * tangent[1]
                + tangent[2] * tangent[2]);
            if (tangentLength <= 1e-6f) {
                tangent = std::abs(normal.y) < 0.99f
                    ? std::array<float, 3>{normal.z, 0.0f, -normal.x}
                    : std::array<float, 3>{1.0f, 0.0f, 0.0f};
                tangentLength = std::sqrt(
                    tangent[0] * tangent[0] + tangent[1] * tangent[1]
                    + tangent[2] * tangent[2]);
            }
            for (float &component : tangent) {
                component /= tangentLength;
            }
            const std::array<float, 3> cross = {
                normal.y * tangent[2] - normal.z * tangent[1],
                normal.z * tangent[0] - normal.x * tangent[2],
                normal.x * tangent[1] - normal.y * tangent[0]};
            const float generatedHandedness = cross[0] * bitangents[index][0]
                    + cross[1] * bitangents[index][1]
                    + cross[2] * bitangents[index][2]
                < 0.0f ? -1.0f : 1.0f;
            const float handedness = hasAuthoredTangents
                ? authoredHandedness : generatedHandedness;
            const std::size_t baseUvChannel = diffuseUvChannel >= 0
                ? static_cast<std::size_t>(diffuseUvChannel) : 0;
            const fh6::ModelVec2 uv = transformedUv(
                baseUvChannel, index, rawMaterialUv && baseUvChannel == 0);
            const fh6::ModelVec2 uv2 = detailUv(index);
            result.vertices.push_back({
                {position.x, position.y, position.z},
                {normal.x, normal.y, normal.z},
                {tangent[0], tangent[1], tangent[2], handedness},
                {uv.u, uv.v},
                {uv2.u, uv2.v},
            });
        }
        for (const std::uint32_t index : mesh.indices) {
            result.indices.push_back(base + index);
        }
    }

    return result;
}

D3D12_SHADER_RESOURCE_VIEW_DESC texture2DView(
    DXGI_FORMAT format, UINT mipLevels = 1) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = mipLevels;

    return view;
}

UploadedTexture uploadRgba8Texture(
    ID3D12Device *device, ID3D12GraphicsCommandList *commands,
    const fh6::SwatchImage &image, bool generateMipmaps = true,
    bool normalMap = false,
    const std::vector<fh6::SwatchImage> *authoredMips = nullptr) {
    if (!image.valid()) {
        return {};
    }
    std::vector<std::vector<std::uint8_t>> mipData;
    std::vector<UINT> mipWidths;
    std::vector<UINT> mipHeights;
    mipData.push_back(image.rgba);
    mipWidths.push_back(static_cast<UINT>(image.width));
    mipHeights.push_back(static_cast<UINT>(image.height));
    if (authoredMips != nullptr) {
        for (const fh6::SwatchImage &mip : *authoredMips) {
            if (!mip.valid()) {
                continue;
            }
            mipData.push_back(mip.rgba);
            mipWidths.push_back(static_cast<UINT>(mip.width));
            mipHeights.push_back(static_cast<UINT>(mip.height));
        }
    }
    const bool hasAuthoredMips = mipData.size() > 1;
    while (generateMipmaps && !hasAuthoredMips
           && (mipWidths.back() > 1 || mipHeights.back() > 1)) {
        const UINT sourceWidth = mipWidths.back();
        const UINT sourceHeight = mipHeights.back();
        const UINT width = std::max(1u, sourceWidth / 2u);
        const UINT height = std::max(1u, sourceHeight / 2u);
        const auto &source = mipData.back();
        std::vector<std::uint8_t> filtered(
            static_cast<std::size_t>(width) * height * 4u);
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                float average[4]{};
                for (UINT sampleY = 0; sampleY < 2; ++sampleY) {
                    for (UINT sampleX = 0; sampleX < 2; ++sampleX) {
                        const UINT sourceX = std::min(sourceWidth - 1, x * 2 + sampleX);
                        const UINT sourceY = std::min(sourceHeight - 1, y * 2 + sampleY);
                        const std::size_t sourceOffset =
                            (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4u;
                        for (UINT channel = 0; channel < 4; ++channel) {
                            average[channel] += source[sourceOffset + channel] * 0.25f;
                        }
                    }
                }
                if (normalMap) {
                    float nx = average[0] / 127.5f - 1.0f;
                    float ny = average[1] / 127.5f - 1.0f;
                    float nz = average[2] / 127.5f - 1.0f;
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (length > 0.00001f) {
                        nx /= length;
                        ny /= length;
                        nz /= length;
                    }
                    average[0] = (nx * 0.5f + 0.5f) * 255.0f;
                    average[1] = (ny * 0.5f + 0.5f) * 255.0f;
                    average[2] = (nz * 0.5f + 0.5f) * 255.0f;
                }
                const std::size_t destinationOffset =
                    (static_cast<std::size_t>(y) * width + x) * 4u;
                for (UINT channel = 0; channel < 4; ++channel) {
                    filtered[destinationOffset + channel] =
                        static_cast<std::uint8_t>(std::clamp(
                            average[channel], 0.0f, 255.0f));
                }
            }
        }
        mipWidths.push_back(width);
        mipHeights.push_back(height);
        mipData.push_back(std::move(filtered));
    }

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(image.width);
    description.Height = static_cast<UINT>(image.height);
    description.DepthOrArraySize = 1;
    description.MipLevels = static_cast<UINT16>(mipData.size());
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES defaultHeap =
        heapProperties(D3D12_HEAP_TYPE_DEFAULT);

    UploadedTexture result;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&result.texture)))) {
        return {};
    }
    result.mipLevels = static_cast<UINT>(mipData.size());
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(result.mipLevels);
    std::vector<UINT> rows(result.mipLevels);
    std::vector<UINT64> rowSizes(result.mipLevels);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &description, 0, result.mipLevels, 0, footprints.data(), rows.data(),
        rowSizes.data(), &uploadSize);
    result.upload = createBuffer(
        device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (result.upload == nullptr) {
        return {};
    }
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(result.upload->Map(0, &noRead, &mapped))) {
        return {};
    }
    for (UINT mip = 0; mip < result.mipLevels; ++mip) {
        const std::size_t sourcePitch = static_cast<std::size_t>(mipWidths[mip]) * 4u;
        for (UINT row = 0; row < rows[mip]; ++row) {
            std::memcpy(
                static_cast<std::uint8_t *>(mapped) + footprints[mip].Offset
                    + static_cast<UINT64>(row) * footprints[mip].Footprint.RowPitch,
                mipData[mip].data() + static_cast<std::size_t>(row) * sourcePitch,
                sourcePitch);
        }
    }
    result.upload->Unmap(0, nullptr);
    for (UINT mip = 0; mip < result.mipLevels; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = result.texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = result.upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprints[mip];
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    const D3D12_RESOURCE_BARRIER barrier = transition(
        result.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &barrier);

    return result;
}

UploadedTexture uploadColorLut(
    ID3D12Device *device, ID3D12GraphicsCommandList *commands,
    const fh6::GarageColorLut &lut) {
    if (!lut.valid()) {
        return {};
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = static_cast<UINT64>(lut.dimension);
    description.Height = static_cast<UINT>(lut.dimension);
    description.DepthOrArraySize = static_cast<UINT16>(lut.dimension);
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES defaultHeap =
        heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    UploadedTexture result;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&result.texture)))) {
        return {};
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &description, 0, 1, 0, &footprint, &rows, &rowSize, &uploadSize);
    result.upload = createBuffer(
        device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (result.upload == nullptr) {
        return {};
    }
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(result.upload->Map(0, &noRead, &mapped))) {
        return {};
    }
    const std::size_t sourceRow = static_cast<std::size_t>(lut.dimension) * 4
        * sizeof(float);
    const std::size_t sourceSlice = sourceRow * static_cast<std::size_t>(lut.dimension);
    const auto *source = reinterpret_cast<const std::uint8_t *>(lut.rgba.data());
    for (int z = 0; z < lut.dimension; ++z) {
        for (UINT y = 0; y < rows; ++y) {
            std::memcpy(
                static_cast<std::uint8_t *>(mapped) + footprint.Offset
                    + static_cast<UINT64>(z) * footprint.Footprint.RowPitch * rows
                    + static_cast<UINT64>(y) * footprint.Footprint.RowPitch,
                source + static_cast<std::size_t>(z) * sourceSlice
                    + static_cast<std::size_t>(y) * sourceRow,
                sourceRow);
        }
    }
    result.upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = result.texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
    sourceLocation.pResource = result.upload.Get();
    sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sourceLocation.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
    const D3D12_RESOURCE_BARRIER barrier = transition(
        result.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &barrier);
    return result;
}

UploadedTexture uploadCubeTexture(
    ID3D12Device *device, ID3D12GraphicsCommandList *commands,
    const fh6::SwatchTexture &source, fh6::SwatchEncoding encoding,
    DXGI_FORMAT format) {
    if (!source.valid() || source.platform != 0 || source.sliceCount != 6) {
        return {};
    }
    for (const fh6::SwatchTextureSlice &slice : source.slices) {
        if (slice.encoding != encoding) {
            return {};
        }
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(source.width);
    description.Height = static_cast<UINT>(source.height);
    description.DepthOrArraySize = 6;
    description.MipLevels = static_cast<UINT16>(source.mipCount);
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES defaultHeap =
        heapProperties(D3D12_HEAP_TYPE_DEFAULT);

    UploadedTexture result;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&result.texture)))) {
        return {};
    }
    const UINT subresourceCount = static_cast<UINT>(6 * source.mipCount);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
    std::vector<UINT> rows(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &description, 0, subresourceCount, 0, footprints.data(), rows.data(),
        rowSizes.data(), &uploadSize);
    result.upload = createBuffer(
        device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (result.upload == nullptr) {
        return {};
    }
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(result.upload->Map(0, &noRead, &mapped))) {
        return {};
    }
    for (UINT face = 0; face < 6; ++face) {
        for (UINT mip = 0; mip < static_cast<UINT>(source.mipCount); ++mip) {
            const UINT subresource = face * static_cast<UINT>(source.mipCount) + mip;
            const QByteArray bytes = source.mipBytes(
                static_cast<int>(face), static_cast<int>(mip));
            if (bytes.size()
                != static_cast<qsizetype>(rowSizes[subresource] * rows[subresource])) {
                result.upload->Unmap(0, nullptr);
                return {};
            }
            for (UINT row = 0; row < rows[subresource]; ++row) {
                std::memcpy(
                    static_cast<std::uint8_t *>(mapped)
                        + footprints[subresource].Offset
                        + static_cast<UINT64>(row)
                            * footprints[subresource].Footprint.RowPitch,
                    bytes.constData()
                        + static_cast<qsizetype>(row * rowSizes[subresource]),
                    static_cast<std::size_t>(rowSizes[subresource]));
            }
        }
    }
    result.upload->Unmap(0, nullptr);
    for (UINT subresource = 0; subresource < subresourceCount; ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = result.texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION upload{};
        upload.pResource = result.upload.Get();
        upload.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        upload.PlacedFootprint = footprints[subresource];
        commands->CopyTextureRegion(&destination, 0, 0, 0, &upload, nullptr);
    }
    const D3D12_RESOURCE_BARRIER barrier = transition(
        result.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &barrier);

    return result;
}

UploadedTexture uploadBc6Panorama(
    ID3D12Device *device, ID3D12GraphicsCommandList *commands,
    const fh6::SwatchTexture &source) {
    if (!source.valid() || source.platform != 0 || source.sliceCount != 1
        || source.mipCount != 1 || source.slices.size() != 1
        || source.slices[0].encoding != fh6::SwatchEncoding::UnsignedBc6H) {
        return {};
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(source.width);
    description.Height = static_cast<UINT>(source.height);
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_BC6H_UF16;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const D3D12_HEAP_PROPERTIES defaultHeap =
        heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    UploadedTexture result;
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&result.texture)))) {
        return {};
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &description, 0, 1, 0, &footprint, &rows, &rowSize, &uploadSize);
    const QByteArray bytes = source.mipBytes(0, 0);
    if (bytes.size() != static_cast<qsizetype>(rowSize * rows)) {
        return {};
    }
    result.upload = createBuffer(
        device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (result.upload == nullptr) {
        return {};
    }
    void *mapped = nullptr;
    const D3D12_RANGE noRead{0, 0};
    if (FAILED(result.upload->Map(0, &noRead, &mapped))) {
        return {};
    }
    for (UINT row = 0; row < rows; ++row) {
        std::memcpy(
            static_cast<std::uint8_t *>(mapped) + footprint.Offset
                + static_cast<UINT64>(row) * footprint.Footprint.RowPitch,
            bytes.constData() + static_cast<qsizetype>(row * rowSize),
            static_cast<std::size_t>(rowSize));
    }
    result.upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = result.texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION upload{};
    upload.pResource = result.upload.Get();
    upload.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    upload.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &upload, nullptr);
    const D3D12_RESOURCE_BARRIER barrier = transition(
        result.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &barrier);
    return result;
}

void writeFloat(
    std::vector<std::uint8_t> *bytes, UINT registerIndex,
    UINT component, float value) {
    const std::size_t offset = static_cast<std::size_t>(registerIndex) * 16
        + static_cast<std::size_t>(component) * 4;
    if (offset + sizeof(value) <= bytes->size()) {
        std::memcpy(bytes->data() + offset, &value, sizeof(value));
    }
}

void writeUint(
    std::vector<std::uint8_t> *bytes, UINT registerIndex,
    UINT component, UINT value) {
    const std::size_t offset = static_cast<std::size_t>(registerIndex) * 16
        + static_cast<std::size_t>(component) * 4;
    if (offset + sizeof(value) <= bytes->size()) {
        std::memcpy(bytes->data() + offset, &value, sizeof(value));
    }
}

using Matrix4 = std::array<std::array<float, 4>, 4>;

Matrix4 multiply(const Matrix4 &left, const Matrix4 &right) {
    Matrix4 result{};
    for (std::size_t row = 0; row < result.size(); ++row) {
        for (std::size_t column = 0; column < result[row].size(); ++column) {
            for (std::size_t inner = 0; inner < result.size(); ++inner) {
                result[row][column] += left[row][inner] * right[inner][column];
            }
        }
    }

    return result;
}

Matrix4 cameraViewProjection(
    const OriginalDx12Camera &camera, float aspectRatio,
    const QPointF &jitterNdc = {}) {
    const QVector3D forward = (camera.target - camera.position).normalized();
    const QVector3D right = QVector3D::crossProduct(
        camera.up.normalized(), forward).normalized();
    const QVector3D up = QVector3D::crossProduct(forward, right);
    const Matrix4 view = {{
        {right.x(), right.y(), right.z(),
         -QVector3D::dotProduct(right, camera.position)},
        {up.x(), up.y(), up.z(),
         -QVector3D::dotProduct(up, camera.position)},
        {forward.x(), forward.y(), forward.z(),
         -QVector3D::dotProduct(forward, camera.position)},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }};
    constexpr float kPi = 3.14159265358979323846f;
    const float verticalScale = 1.0f / std::tan(
        camera.verticalFovDegrees * kPi / 360.0f);
    const float depthScale = camera.farPlane
        / (camera.farPlane - camera.nearPlane);
    const Matrix4 projection = {{
        {verticalScale / aspectRatio, 0.0f,
         static_cast<float>(jitterNdc.x()), 0.0f},
        {0.0f, verticalScale, static_cast<float>(jitterNdc.y()), 0.0f},
        {0.0f, 0.0f, depthScale, -camera.nearPlane * depthScale},
        {0.0f, 0.0f, 1.0f, 0.0f},
    }};

    return multiply(projection, view);
}

struct ShadowProjection {
    Matrix4 viewProjection{};
    float strength = 0.0f;
    float depthBias = 0.0f;
    bool valid = false;
    int lightIndex = -1;
};

struct ReflectionProbeVolume {
    QVector3D minimum;
    QVector3D maximum;
    QVector3D position;
    bool valid = false;
};

struct TemporalFrameState {
    Matrix4 previousViewProjection{};
    QPointF jitterNdc;
    bool historyValid = false;
    int previousHistoryIndex = 0;
};

QPointF temporalJitterNdc(quint64 frameIndex, const QSize &size) {
    if (frameIndex == 0 || size.isEmpty()) {
        return {};
    }
    constexpr std::array<std::array<float, 2>, 8> kHalton = {{
        {{0.5f, 1.0f / 3.0f}}, {{0.25f, 2.0f / 3.0f}},
        {{0.75f, 1.0f / 9.0f}}, {{0.125f, 4.0f / 9.0f}},
        {{0.625f, 7.0f / 9.0f}}, {{0.375f, 2.0f / 9.0f}},
        {{0.875f, 5.0f / 9.0f}}, {{0.0625f, 8.0f / 9.0f}},
    }};
    const auto &sample = kHalton[static_cast<std::size_t>((frameIndex - 1) % 8)];
    return {
        (sample[0] - 0.5f) * 2.0f / static_cast<float>(size.width()),
        (0.5f - sample[1]) * 2.0f / static_cast<float>(size.height())};
}

ReflectionProbeVolume reflectionProbeVolume(
    const std::vector<DrawResources> &draws) {
    QVector3D minimum(
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    QVector3D maximum(
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());
    QVector3D carMinimum = minimum;
    QVector3D carMaximum = maximum;
    bool foundShell = false;
    bool foundCar = false;
    const auto include = [](const Vertex &vertex, QVector3D *low, QVector3D *high) {
        low->setX(std::min(low->x(), vertex.position[0]));
        low->setY(std::min(low->y(), vertex.position[1]));
        low->setZ(std::min(low->z(), vertex.position[2]));
        high->setX(std::max(high->x(), vertex.position[0]));
        high->setY(std::max(high->y(), vertex.position[1]));
        high->setZ(std::max(high->z(), vertex.position[2]));
    };
    for (const DrawResources &draw : draws) {
        if (draw.reflectionProbeShell) {
            foundShell = true;
            for (const Vertex &vertex : draw.geometry.vertices) {
                include(vertex, &minimum, &maximum);
            }
        }
        if (draw.family == fh6::OriginalShaderSurfaceFamily::Car
            && draw.visible) {
            foundCar = true;
            for (const Vertex &vertex : draw.geometry.vertices) {
                include(vertex, &carMinimum, &carMaximum);
            }
        }
    }
    ReflectionProbeVolume result;
    if (!foundShell || !foundCar
        || minimum.x() >= maximum.x() || minimum.y() >= maximum.y()
        || minimum.z() >= maximum.z()) {
        return result;
    }
    result.minimum = minimum;
    result.maximum = maximum;
    result.position = (carMinimum + carMaximum) * 0.5f;
    result.valid = true;
    return result;
}

enum class ShadowPassKind {
    Self,
    Drop,
};

bool castsShadow(const DrawResources &draw, ShadowPassKind kind) {
    return kind == ShadowPassKind::Self
        ? draw.selfShadowCaster : draw.dropShadowCaster;
}

ShadowProjection carShadowProjection(
    const std::vector<DrawResources> &draws,
    const fh6::ModelVec3 &lightRayDirection, ShadowPassKind kind,
    float strength) {
    QVector3D minimum(
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    QVector3D maximum(
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());
    bool found = false;
    for (const auto &draw : draws) {
        if (!castsShadow(draw, kind)) {
            continue;
        }
        for (const Vertex &vertex : draw.geometry.vertices) {
            found = true;
            minimum.setX(std::min(minimum.x(), vertex.position[0]));
            minimum.setY(std::min(minimum.y(), vertex.position[1]));
            minimum.setZ(std::min(minimum.z(), vertex.position[2]));
            maximum.setX(std::max(maximum.x(), vertex.position[0]));
            maximum.setY(std::max(maximum.y(), vertex.position[1]));
            maximum.setZ(std::max(maximum.z(), vertex.position[2]));
        }
    }
    if (!found) {
        return {};
    }

    const QVector3D direction(
        lightRayDirection.x, lightRayDirection.y, lightRayDirection.z);
    if (direction.lengthSquared() < 0.000001f) {
        return {};
    }
    const QVector3D forward = direction.normalized();
    // Homespace's ShadowLightDirection is the direction travelled by the
    // light, rather than the direction from the surface back to the light.
    // A ground shadow therefore lies beyond the caster in light-view depth.
    // Fitting only the car clips that receiver point entirely for the shallow
    // Tokyo light angle.
    if (forward.y() >= -0.000001f) {
        return {};
    }
    const float contactPlaneY = minimum.y();
    const QVector3D upReference = std::abs(QVector3D::dotProduct(
        forward, QVector3D(0.0f, 1.0f, 0.0f))) < 0.95f
        ? QVector3D(0.0f, 1.0f, 0.0f)
        : QVector3D(0.0f, 0.0f, 1.0f);
    const QVector3D right = QVector3D::crossProduct(
        upReference, forward).normalized();
    const QVector3D up = QVector3D::crossProduct(forward, right);
    float minimumLightX = std::numeric_limits<float>::max();
    float maximumLightX = std::numeric_limits<float>::lowest();
    float minimumLightY = std::numeric_limits<float>::max();
    float maximumLightY = std::numeric_limits<float>::lowest();
    float minimumLightZ = std::numeric_limits<float>::max();
    float maximumLightZ = std::numeric_limits<float>::lowest();
    for (int mask = 0; mask < 8; ++mask) {
        const QVector3D point(
            (mask & 1) != 0 ? maximum.x() : minimum.x(),
            (mask & 2) != 0 ? maximum.y() : minimum.y(),
            (mask & 4) != 0 ? maximum.z() : minimum.z());
        const float lightX = QVector3D::dotProduct(right, point);
        const float lightY = QVector3D::dotProduct(up, point);
        const float lightZ = QVector3D::dotProduct(forward, point);
        minimumLightX = std::min(minimumLightX, lightX);
        maximumLightX = std::max(maximumLightX, lightX);
        minimumLightY = std::min(minimumLightY, lightY);
        maximumLightY = std::max(maximumLightY, lightY);
        minimumLightZ = std::min(minimumLightZ, lightZ);
        maximumLightZ = std::max(maximumLightZ, lightZ);

        const float travelToContact =
            (contactPlaneY - point.y()) / forward.y();
        if (travelToContact > 0.0f) {
            const QVector3D contactPoint = point + forward * travelToContact;
            maximumLightZ = std::max(
                maximumLightZ,
                QVector3D::dotProduct(forward, contactPoint));
        }
    }
    constexpr float kLateralMargin = 0.20f;
    constexpr float kDepthMargin = 0.50f;
    const float width = maximumLightX - minimumLightX + 2.0f * kLateralMargin;
    const float height = maximumLightY - minimumLightY + 2.0f * kLateralMargin;
    const float depth = maximumLightZ - minimumLightZ + 2.0f * kDepthMargin;
    const float texelWidth = width / static_cast<float>(kShadowMapSize);
    const float texelHeight = height / static_cast<float>(kShadowMapSize);
    const float centerX = std::round(
        (minimumLightX + maximumLightX) * 0.5f / texelWidth) * texelWidth;
    const float centerY = std::round(
        (minimumLightY + maximumLightY) * 0.5f / texelHeight) * texelHeight;
    const float depthStart = minimumLightZ - kDepthMargin;
    const Matrix4 viewProjection = {{
        {2.0f * right.x() / width,
         2.0f * right.y() / width,
         2.0f * right.z() / width,
         -2.0f * centerX / width},
        {2.0f * up.x() / height,
         2.0f * up.y() / height,
         2.0f * up.z() / height,
         -2.0f * centerY / height},
        {forward.x() / depth,
         forward.y() / depth,
         forward.z() / depth,
         -depthStart / depth},
        {0.0f, 0.0f, 0.0f, 1.0f},
    }};
    ShadowProjection result;
    result.viewProjection = viewProjection;
    result.strength = strength;
    result.depthBias = kShadowDepthBias;
    result.valid = true;

    return result;
}

std::array<ShadowProjection, 2> localLightShadowProjections(
    const std::vector<DrawResources> &draws,
    const std::vector<fh6::OriginalShaderPointLight> &authoredLights) {
    QVector3D minimum(
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    QVector3D maximum(
        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());
    bool foundCar = false;
    for (const DrawResources &draw : draws) {
        if (!draw.selfShadowCaster) {
            continue;
        }
        for (const Vertex &vertex : draw.geometry.vertices) {
            foundCar = true;
            const QVector3D point(
                vertex.position[0], vertex.position[1], vertex.position[2]);
            minimum.setX(std::min(minimum.x(), point.x()));
            minimum.setY(std::min(minimum.y(), point.y()));
            minimum.setZ(std::min(minimum.z(), point.z()));
            maximum.setX(std::max(maximum.x(), point.x()));
            maximum.setY(std::max(maximum.y(), point.y()));
            maximum.setZ(std::max(maximum.z(), point.z()));
        }
    }
    if (!foundCar) {
        return {};
    }
    const QVector3D carCenter = (minimum + maximum) * 0.5f;
    struct Candidate {
        float score = 0.0f;
        int pointIndex = -1;
        QVector3D position;
        QVector3D direction;
        float range = 0.0f;
        float coneDegrees = 0.0f;
    };
    std::vector<Candidate> candidates;
    int pointIndex = 0;
    constexpr float kPi = 3.14159265358979323846f;
    for (const fh6::OriginalShaderPointLight &light : authoredLights) {
        if (!light.enabled || light.range <= 0.0f || light.intensity <= 0.0f
            || pointIndex >= 32) {
            continue;
        }
        const fh6::ModelVec3 sourcePosition =
            light.transform.transformPoint({});
        const fh6::ModelVec3 sourceDirection =
            light.transform.transformVector({0.0f, 0.0f, 1.0f});
        const QVector3D position(
            sourcePosition.x, sourcePosition.y, sourcePosition.z);
        QVector3D direction(
            sourceDirection.x, sourceDirection.y, sourceDirection.z);
        if (direction.lengthSquared() < 0.000001f) {
            ++pointIndex;
            continue;
        }
        direction.normalize();
        const QVector3D toCar = carCenter - position;
        const float distance = toCar.length();
        const float cone = QVector3D::dotProduct(
            toCar / std::max(distance, 0.0001f), direction);
        const float outer = std::cos(
            std::max(light.coneAngleDegrees, 1.0f) * 0.5f * kPi / 180.0f);
        if (distance < light.range && cone > outer) {
            const float rangeFade = 1.0f - distance / light.range;
            candidates.push_back({
                light.intensity * rangeFade * rangeFade
                    * (cone - outer) / std::max(distance * distance, 0.25f),
                pointIndex, position, direction, light.range,
                std::max(light.coneAngleDegrees, 20.0f)});
        }
        ++pointIndex;
    }
    std::sort(
        candidates.begin(), candidates.end(),
        [](const Candidate &left, const Candidate &right) {
            return left.score > right.score;
        });
    std::array<ShadowProjection, 2> result{};
    for (std::size_t index = 0;
         index < result.size() && index < candidates.size(); ++index) {
        const Candidate &candidate = candidates[index];
        const QVector3D upReference = std::abs(QVector3D::dotProduct(
            candidate.direction, QVector3D(0.0f, 1.0f, 0.0f))) < 0.95f
            ? QVector3D(0.0f, 1.0f, 0.0f)
            : QVector3D(0.0f, 0.0f, 1.0f);
        const QVector3D right = QVector3D::crossProduct(
            upReference, candidate.direction).normalized();
        const QVector3D up = QVector3D::crossProduct(
            candidate.direction, right);
        const Matrix4 view = {{
            {right.x(), right.y(), right.z(),
             -QVector3D::dotProduct(right, candidate.position)},
            {up.x(), up.y(), up.z(),
             -QVector3D::dotProduct(up, candidate.position)},
            {candidate.direction.x(), candidate.direction.y(),
             candidate.direction.z(),
             -QVector3D::dotProduct(candidate.direction, candidate.position)},
            {0.0f, 0.0f, 0.0f, 1.0f},
        }};
        constexpr float nearPlane = 0.05f;
        const float farPlane = std::max(candidate.range, nearPlane + 0.1f);
        const float scale = 1.0f / std::tan(
            candidate.coneDegrees * 0.5f * kPi / 180.0f);
        const float depthScale = farPlane / (farPlane - nearPlane);
        const Matrix4 projection = {{
            {scale, 0.0f, 0.0f, 0.0f},
            {0.0f, scale, 0.0f, 0.0f},
            {0.0f, 0.0f, depthScale, -nearPlane * depthScale},
            {0.0f, 0.0f, 1.0f, 0.0f},
        }};
        result[index].viewProjection = multiply(projection, view);
        result[index].strength = 0.72f;
        result[index].depthBias = kShadowDepthBias;
        result[index].valid = true;
        result[index].lightIndex = candidate.pointIndex;
    }
    return result;
}

std::array<std::vector<std::uint8_t>, 8> shaderConstantData(
    const OriginalDx12Camera &camera, const QSize &frameSize,
    const fh6::OriginalShaderLighting &lighting,
    const std::vector<fh6::OriginalShaderPointLight> &authoredLights,
    const fh6::GaragePanoramaResources &panorama,
    const fh6::GarageColorLut &colorLut,
    const ShadowProjection &shadow, const ShadowProjection &dropShadow,
    const std::array<ShadowProjection, 2> &localShadows,
    const ReflectionProbeVolume &reflectionProbe,
    const TemporalFrameState &temporal) {
    std::array<std::vector<std::uint8_t>, 8> data;
    constexpr std::array<std::size_t, 8> sizes = {
        // FrameData ends at temporal reprojection parameters (row 168). Keep enough
        // source bytes for every row before createConstantBuffer applies the
        // required 256-byte allocation alignment.
        256, 256, 512, 11264, 18432, 2704, 65536, 65536};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].resize(sizes[index], 0);
    }
    writeFloat(&data[1], 6, 0, 1.0f);
    writeFloat(&data[1], 6, 1, 1.0f);
    writeFloat(&data[1], 6, 2, 1.0f);
    writeFloat(&data[2], 8, 0, 1.0f);
    writeFloat(&data[2], 9, 1, 1.0f);
    writeFloat(&data[2], 10, 2, 1.0f);
    writeFloat(&data[2], 12, 0, 1.0f);
    writeFloat(&data[2], 13, 1, 1.0f);
    writeFloat(&data[2], 14, 2, 1.0f);
    const Matrix4 viewProjection = cameraViewProjection(
        camera, static_cast<float>(frameSize.width()) / frameSize.height(),
        temporal.jitterNdc);
    for (UINT row = 0; row < 4; ++row) {
        for (UINT component = 0; component < 4; ++component) {
            writeFloat(&data[5], row, component, viewProjection[row][component]);
        }
    }
    writeFloat(&data[5], 4, 0, camera.position.x());
    writeFloat(&data[5], 4, 1, camera.position.y());
    writeFloat(&data[5], 4, 2, camera.position.z());
    writeFloat(&data[5], 4, 3, camera.nearPlane);
    writeFloat(&data[5], 5, 0, lighting.direction.x);
    writeFloat(&data[5], 5, 1, lighting.direction.y);
    writeFloat(&data[5], 5, 2, lighting.direction.z);
    writeFloat(&data[5], 6, 0, lighting.directColor.x);
    writeFloat(&data[5], 6, 1, lighting.directColor.y);
    writeFloat(&data[5], 6, 2, lighting.directColor.z);
    writeFloat(&data[5], 6, 3, 1.0f);
    writeFloat(&data[5], 7, 0, lighting.ambientColor.x);
    writeFloat(&data[5], 7, 1, lighting.ambientColor.y);
    writeFloat(&data[5], 7, 2, lighting.ambientColor.z);
    writeFloat(&data[5], 7, 3, 1.0f);
    const QVector3D forward = (camera.target - camera.position).normalized();
    const QVector3D right = QVector3D::crossProduct(
        camera.up.normalized(), forward).normalized();
    const QVector3D up = QVector3D::crossProduct(forward, right);
    const std::array<QVector3D, 3> cameraAxes = {forward, right, up};
    for (UINT axis = 0; axis < cameraAxes.size(); ++axis) {
        writeFloat(&data[5], 8 + axis, 0, cameraAxes[axis].x());
        writeFloat(&data[5], 8 + axis, 1, cameraAxes[axis].y());
        writeFloat(&data[5], 8 + axis, 2, cameraAxes[axis].z());
    }
    writeFloat(&data[5], 8, 3, camera.farPlane);
    writeFloat(
        &data[5], 9, 3,
        static_cast<float>(frameSize.width()) / frameSize.height());
    constexpr float kPi = 3.14159265358979323846f;
    writeFloat(&data[5], 11, 0, panorama.sphericalPower);
    writeFloat(&data[5], 11, 1, panorama.frameScale);
    writeFloat(&data[5], 11, 2, std::tan(camera.verticalFovDegrees * kPi / 360.0f));
    UINT activeLightCount = 0;
    for (const fh6::OriginalShaderPointLight &light : authoredLights) {
        if (!light.enabled || light.range <= 0.0f || light.intensity <= 0.0f
            || activeLightCount >= 32) {
            continue;
        }
        const fh6::ModelVec3 position = light.transform.transformPoint({});
        // PVSL fixtures emit along their authored local +Z axis.  Treating this
        // as -Z mirrored the ceiling spots vertically, lighting the roof rather
        // than the room below it.
        fh6::ModelVec3 direction = light.transform.transformVector({0.0f, 0.0f, 1.0f});
        const float directionLength = std::sqrt(
            direction.x * direction.x + direction.y * direction.y
            + direction.z * direction.z);
        if (directionLength > 0.000001f) {
            direction.x /= directionLength;
            direction.y /= directionLength;
            direction.z /= directionLength;
        }
        const UINT positionRow = 12 + activeLightCount;
        const UINT colorRow = 44 + activeLightCount;
        const UINT directionRow = 76 + activeLightCount;
        const UINT spotRow = 108 + activeLightCount;
        writeFloat(&data[5], positionRow, 0, position.x);
        writeFloat(&data[5], positionRow, 1, position.y);
        writeFloat(&data[5], positionRow, 2, position.z);
        writeFloat(&data[5], positionRow, 3, light.range);
        writeFloat(&data[5], colorRow, 0, light.color.x);
        writeFloat(&data[5], colorRow, 1, light.color.y);
        writeFloat(&data[5], colorRow, 2, light.color.z);
        writeFloat(&data[5], colorRow, 3, light.intensity);
        writeFloat(&data[5], directionRow, 0, direction.x);
        writeFloat(&data[5], directionRow, 1, direction.y);
        writeFloat(&data[5], directionRow, 2, direction.z);
        const float outer = std::cos(
            light.coneAngleDegrees * 0.5f * kPi / 180.0f);
        const float inner = std::cos(
            std::max(0.0f, light.coneAngleDegrees - light.penumbraAngleDegrees)
            * 0.5f * kPi / 180.0f);
        writeFloat(&data[5], directionRow, 3, outer);
        writeFloat(&data[5], spotRow, 0, inner);
        ++activeLightCount;
    }
    writeFloat(&data[5], 11, 3, static_cast<float>(activeLightCount));
    writeFloat(&data[5], 140, 0, static_cast<float>(colorLut.dimension));
    writeFloat(&data[5], 140, 1, colorLut.scale);
    for (UINT row = 0; row < 4; ++row) {
        for (UINT component = 0; component < 4; ++component) {
            writeFloat(
                &data[5], 141 + row, component,
                shadow.viewProjection[row][component]);
        }
    }
    writeFloat(&data[5], 145, 0, 1.0f / static_cast<float>(kShadowMapSize));
    writeFloat(&data[5], 145, 1, shadow.strength);
    writeFloat(&data[5], 145, 2, shadow.depthBias);
    writeFloat(&data[5], 145, 3, shadow.valid ? 1.0f : 0.0f);
    for (UINT row = 0; row < 4; ++row) {
        for (UINT component = 0; component < 4; ++component) {
            writeFloat(
                &data[5], 146 + row, component,
                dropShadow.viewProjection[row][component]);
        }
    }
    writeFloat(&data[5], 150, 0, 1.0f / static_cast<float>(kShadowMapSize));
    writeFloat(&data[5], 150, 1, dropShadow.strength);
    writeFloat(&data[5], 150, 2, dropShadow.depthBias);
    writeFloat(&data[5], 150, 3, dropShadow.valid ? 1.0f : 0.0f);
    for (UINT shadowIndex = 0; shadowIndex < localShadows.size(); ++shadowIndex) {
        const ShadowProjection &local = localShadows[shadowIndex];
        const UINT matrixStart = 151 + shadowIndex * 5;
        for (UINT row = 0; row < 4; ++row) {
            for (UINT component = 0; component < 4; ++component) {
                writeFloat(
                    &data[5], matrixStart + row, component,
                    local.viewProjection[row][component]);
            }
        }
        writeFloat(&data[5], matrixStart + 4, 0, local.strength);
        writeFloat(&data[5], matrixStart + 4, 1, local.depthBias);
        writeFloat(
            &data[5], matrixStart + 4, 2,
            static_cast<float>(local.lightIndex));
        writeFloat(&data[5], matrixStart + 4, 3, local.valid ? 1.0f : 0.0f);
    }
    writeFloat(&data[5], 161, 0, reflectionProbe.minimum.x());
    writeFloat(&data[5], 161, 1, reflectionProbe.minimum.y());
    writeFloat(&data[5], 161, 2, reflectionProbe.minimum.z());
    writeFloat(&data[5], 161, 3, reflectionProbe.valid ? 1.0f : 0.0f);
    writeFloat(&data[5], 162, 0, reflectionProbe.maximum.x());
    writeFloat(&data[5], 162, 1, reflectionProbe.maximum.y());
    writeFloat(&data[5], 162, 2, reflectionProbe.maximum.z());
    writeFloat(&data[5], 163, 0, reflectionProbe.position.x());
    writeFloat(&data[5], 163, 1, reflectionProbe.position.y());
    writeFloat(&data[5], 163, 2, reflectionProbe.position.z());
    for (UINT row = 0; row < 4; ++row) {
        for (UINT component = 0; component < 4; ++component) {
            writeFloat(
                &data[5], 164 + row, component,
                temporal.previousViewProjection[row][component]);
        }
    }
    writeFloat(&data[5], 168, 0, temporal.historyValid ? 1.0f : 0.0f);
    writeFloat(
        &data[5], 168, 1,
        static_cast<float>(temporal.previousHistoryIndex));
    writeFloat(
        &data[5], 168, 2, static_cast<float>(temporal.jitterNdc.x()));
    writeFloat(
        &data[5], 168, 3, static_cast<float>(temporal.jitterNdc.y()));
    for (UINT row = 0; row < 4; ++row) {
        for (UINT component = 0; component < 4; ++component) {
            writeFloat(
                &data[3], 16 + row, component,
                viewProjection[row][component]);
        }
    }
    writeFloat(&data[3], 59, 0, 1.0f);
    writeFloat(&data[3], 59, 1, 1.0f);
    writeFloat(&data[3], 59, 3, 1.0f);
    writeFloat(&data[3], 94, 3, 1.0f);
    writeFloat(&data[3], 111, 2, 1.0f);
    writeFloat(&data[3], 111, 3, 1.0f);
    writeFloat(&data[3], 112, 0, 1.0f);
    writeFloat(&data[3], 112, 2, 1.0f);
    writeFloat(&data[3], 164, 0, 1.0f);
    writeFloat(&data[4], 0, 0, lighting.direction.x);
    writeFloat(&data[4], 0, 1, lighting.direction.y);
    writeFloat(&data[4], 0, 2, lighting.direction.z);
    writeFloat(&data[4], 1, 0, lighting.directColor.x);
    writeFloat(&data[4], 1, 1, lighting.directColor.y);
    writeFloat(&data[4], 1, 2, lighting.directColor.z);
    writeFloat(&data[4], 1, 3, 1.0f);
    writeFloat(&data[4], 2, 0, lighting.ambientColor.x);
    writeFloat(&data[4], 2, 1, lighting.ambientColor.y);
    writeFloat(&data[4], 2, 2, lighting.ambientColor.z);
    writeFloat(&data[4], 2, 3, 1.0f);
    writeFloat(&data[4], 55, 1, 1.0f);
    writeUint(&data[4], 1136, 2, 1);
    for (UINT offset = 0; offset < 7; ++offset) {
        writeUint(&data[6], offset / 4, offset % 4, offset);
    }
    for (UINT component = 0; component < 3; ++component) {
        writeFloat(&data[7], 1, component, 0.04f);
        writeFloat(&data[7], 2, component, 0.04f);
    }
    writeFloat(&data[7], 1, 3, 1.0f);
    writeFloat(&data[7], 2, 3, 1.0f);
    for (UINT component = 0; component < 4; ++component) {
        writeFloat(&data[7], 3, component, 1.0f);
    }
    writeFloat(&data[7], 4, 0, 1.0f);
    writeFloat(&data[7], 4, 1, 1.0f);
    writeFloat(&data[7], 5, 0, 0.5f);
    writeFloat(&data[7], 5, 1, 0.5f);

    return data;
}

std::vector<std::uint8_t> shadowConstantData(
    const ShadowProjection &shadow) {
    std::vector<std::uint8_t> data(80, 0);
    for (UINT row = 0; row < 4; ++row) {
        for (UINT component = 0; component < 4; ++component) {
            writeFloat(
                &data, row, component,
                shadow.viewProjection[row][component]);
        }
    }
    writeFloat(&data, 4, 0, 1.0f / static_cast<float>(kShadowMapSize));
    writeFloat(&data, 4, 1, shadow.strength);
    writeFloat(&data, 4, 2, shadow.depthBias);
    writeFloat(&data, 4, 3, shadow.valid ? 1.0f : 0.0f);
    return data;
}

std::vector<std::uint8_t> materialConstantData(const DrawResources &draw) {
    std::vector<std::uint8_t> data(784, 0);
    writeFloat(&data, 0, 0, draw.baseColor[0]);
    writeFloat(&data, 0, 1, draw.baseColor[1]);
    writeFloat(&data, 0, 2, draw.baseColor[2]);
    writeFloat(&data, 0, 3, draw.opacity);
    writeFloat(&data, 1, 0, draw.emissiveColor[0]);
    writeFloat(&data, 1, 1, draw.emissiveColor[1]);
    writeFloat(&data, 1, 2, draw.emissiveColor[2]);
    writeFloat(&data, 1, 3, draw.emissiveTexture != nullptr ? 1.0f : 0.0f);
    writeFloat(&data, 2, 0, std::clamp(draw.gloss, 0.0f, 1.0f));
    writeFloat(&data, 2, 1, std::clamp(draw.metallic, 0.0f, 1.0f));
    writeFloat(&data, 2, 2, draw.normalTexture != nullptr ? 1.0f : 0.0f);
    writeFloat(&data, 2, 3, draw.surfaceTexture != nullptr ? 1.0f : 0.0f);
    writeFloat(&data, 3, 0, draw.diffuseTexture != nullptr ? 1.0f : 0.0f);
    writeFloat(&data, 3, 1, draw.uTiling);
    writeFloat(&data, 3, 2, draw.vTiling);
    writeFloat(&data, 3, 3, draw.alphaTexture != nullptr ? 1.0f : 0.0f);
    writeFloat(&data, 4, 0, draw.detailUTiling);
    writeFloat(&data, 4, 1, draw.detailVTiling);
    writeUint(&data, 5, 0, draw.liveryBaseTexture ? 1u : 0u);
    writeUint(&data, 5, 1, draw.liveryAllowedSides);
    writeUint(&data, 5, 2, static_cast<quint32>(draw.liverySideCount));
    writeUint(
        &data, 5, 3,
        draw.family == fh6::OriginalShaderSurfaceFamily::Floor ? 1u
        : draw.family == fh6::OriginalShaderSurfaceFamily::Car ? 2u
        : 0u);
    for (int side = 0; side < fh6::kLiverySideCount; ++side) {
        for (int component = 0; component < 4; ++component) {
            writeFloat(
                &data, 6 + side, component,
                draw.liverySourceRegions[side][component]);
            writeFloat(
                &data, 17 + side, component,
                draw.liveryPaintRegions[side][component]);
        }
        for (int component = 0; component < 3; ++component) {
            writeFloat(
                &data, 28 + side, component,
                draw.liveryFacing[side][component]);
        }
    }
    writeFloat(&data, 39, 0, draw.clearCoatCoverage);
    writeFloat(&data, 39, 1, draw.clearCoatRoughness);
    writeFloat(&data, 39, 2, draw.clearCoatOnLivery ? 1.0f : 0.0f);
    writeFloat(&data, 40, 0, draw.clearCoatTint[0]);
    writeFloat(&data, 40, 1, draw.clearCoatTint[1]);
    writeFloat(&data, 40, 2, draw.clearCoatTint[2]);
    writeFloat(&data, 40, 3, draw.roughnessShift);
    writeFloat(&data, 41, 0, draw.normalIntensity);
    writeFloat(&data, 41, 1, draw.weaveNormalIntensity);
    writeFloat(&data, 41, 2, draw.clearCoatNormalUTiling);
    writeFloat(&data, 41, 3, draw.clearCoatNormalVTiling);
    for (UINT component = 0; component < 3; ++component) {
        writeFloat(&data, 42, component, draw.weaveColorTintA[component]);
        writeFloat(&data, 43, component, draw.weaveColorTintB[component]);
    }
    writeUint(&data, 44, 0, static_cast<quint32>(draw.shaderFamily));
    writeUint(&data, 44, 1, draw.weaveMaskTexture != nullptr ? 1u : 0u);
    writeUint(&data, 44, 2, draw.weaveNormalTexture != nullptr ? 1u : 0u);
    writeUint(&data, 44, 3, draw.clearCoatNormalTexture != nullptr ? 1u : 0u);
    writeFloat(
        &data, 39, 3,
        (draw.tireHeightAoTexture != nullptr ? 1.0f : 0.0f)
            + (draw.aoTexture != nullptr ? 2.0f : 0.0f));
    for (UINT component = 0; component < 3; ++component) {
        writeFloat(&data, 45, component, draw.secondaryPaintColor[component]);
        writeFloat(&data, 46, component, draw.flakeColor[component]);
    }
    writeFloat(&data, 45, 3, draw.glancingFlopStrength);
    writeFloat(&data, 46, 3, draw.flakeCoverage);
    writeFloat(&data, 47, 0, draw.flakeRoughness);
    writeFloat(&data, 47, 1, draw.glitterIntensity);
    writeFloat(&data, 47, 2, draw.glancingFlopPower);
    writeFloat(&data, 47, 3, draw.glancingFlopEnabled ? 1.0f : 0.0f);
    constexpr quint32 kReflectionGroups =
        fh6::car_draw_groups::kWindshieldReflection
        | fh6::car_draw_groups::kWindshieldReflectionDriverless;
    writeUint(
        &data, 48, 0,
        (draw.drawGroups & kReflectionGroups) != 0 ? 1u : 0u);
    writeUint(&data, 48, 1, draw.interiorWindshield ? 1u : 0u);
    return data;
}

ComPtr<ID3D12Resource> createConstantBuffer(
    ID3D12Device *device, const std::vector<std::uint8_t> &bytes) {
    const UINT64 alignedSize =
        (static_cast<UINT64>(bytes.size()) + 255) & ~255ull;
    ComPtr<ID3D12Resource> resource = createBuffer(
        device, alignedSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (resource == nullptr
        || !uploadBuffer(resource.Get(), bytes.data(), bytes.size())) {
        return {};
    }

    return resource;
}

void createNullDescriptors(
    ID3D12Device *device, ID3D12DescriptorHeap *heap, UINT stride,
    UINT descriptorCount) {
    constexpr UINT kSpace0Start = 0;
    constexpr UINT kSpace1Start = 124;
    constexpr UINT kSpace6Start = 140;
    constexpr UINT kSpace12Start = 156;
    auto handleAt = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * stride;
        return handle;
    };
    const D3D12_SHADER_RESOURCE_VIEW_DESC texture2D =
        texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
    for (UINT index = 0; index < descriptorCount; ++index) {
        device->CreateShaderResourceView(nullptr, &texture2D, handleAt(index));
    }
    auto setCube = [&](UINT index, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.TextureCube.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &view, handleAt(index));
    };
    auto setBuffer = [&](UINT index, DXGI_FORMAT format, UINT structureStride) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements = 1;
        view.Buffer.StructureByteStride = structureStride;
        device->CreateShaderResourceView(nullptr, &view, handleAt(index));
    };
    auto setTexture3D = [&](UINT index, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture3D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &view, handleAt(index));
    };
    auto setTexture2DArray = [&](UINT index) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.ArraySize = 1;
        device->CreateShaderResourceView(nullptr, &view, handleAt(index));
    };
    setCube(kSpace0Start + 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    setCube(kSpace0Start + 1, DXGI_FORMAT_BC6H_UF16);
    setBuffer(kSpace0Start + 45, DXGI_FORMAT_UNKNOWN, 192);
    setBuffer(kSpace0Start + 86, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    setBuffer(kSpace0Start + 87, DXGI_FORMAT_R32G32_FLOAT, 0);
    setBuffer(kSpace0Start + 109, DXGI_FORMAT_UNKNOWN, 144);
    setCube(kSpace0Start + 117, DXGI_FORMAT_R32_FLOAT);
    setCube(kSpace0Start + 118, DXGI_FORMAT_R8G8B8A8_UNORM);
    setTexture3D(kSpace0Start + 123, DXGI_FORMAT_R32G32B32A32_FLOAT);
    for (UINT index = 0; index < 16; ++index) {
        device->CreateShaderResourceView(
            nullptr, &texture2D, handleAt(kSpace1Start + index));
        device->CreateShaderResourceView(
            nullptr, &texture2D, handleAt(kSpace6Start + index));
    }
    setTexture3D(kSpace12Start + 0, DXGI_FORMAT_R32_UINT);
    setBuffer(kSpace12Start + 1, DXGI_FORMAT_UNKNOWN, 4);
    setBuffer(kSpace12Start + 2, DXGI_FORMAT_UNKNOWN, 16);
    setBuffer(kSpace12Start + 3, DXGI_FORMAT_UNKNOWN, 16);
    setTexture2DArray(kSpace12Start + 4);
    setBuffer(kSpace12Start + 9, DXGI_FORMAT_UNKNOWN, 16);
}

ComPtr<ID3D12Resource> createTarget(
    ID3D12Device *device, const QSize &size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(size.width());
    description.Height = static_cast<UINT>(size.height());
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = kTargetFormat;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kTargetFormat;
    std::copy(kClearColor.cbegin(), kClearColor.cend(), clear.Color);
    const D3D12_HEAP_PROPERTIES heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resource;

    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
            IID_PPV_ARGS(&resource)))) {
        return {};
    }

    return resource;
}

ComPtr<ID3D12Resource> createDepthTarget(
    ID3D12Device *device, const QSize &size) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(size.width());
    description.Height = static_cast<UINT>(size.height());
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    const D3D12_HEAP_PROPERTIES heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resource;

    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(&resource)))) {
        return {};
    }

    return resource;
}

void createSceneDepthView(
    ID3D12Device *device, ID3D12Resource *target,
    D3D12_CPU_DESCRIPTOR_HANDLE shaderHandle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target, &view, shaderHandle);
}

ComPtr<ID3D12Resource> createShadowDepthTarget(ID3D12Device *device) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = kShadowMapSize;
    description.Height = kShadowMapSize;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    const D3D12_HEAP_PROPERTIES heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> resource;

    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
            IID_PPV_ARGS(&resource)))) {
        return {};
    }

    return resource;
}

void createShadowViews(
    ID3D12Device *device, ID3D12Resource *target,
    D3D12_CPU_DESCRIPTOR_HANDLE depthHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE shaderHandle) {
    D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
    depthView.Format = DXGI_FORMAT_D32_FLOAT;
    depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(target, &depthView, depthHandle);
    D3D12_SHADER_RESOURCE_VIEW_DESC shaderView{};
    shaderView.Format = DXGI_FORMAT_R32_FLOAT;
    shaderView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shaderView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shaderView.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target, &shaderView, shaderHandle);
}

ComPtr<ID3D12Device> createHardwareDevice(
    IDXGIFactory6 *factory, QString *adapterName) {
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(D3D12CreateDevice(
                adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&device)))) {
            *adapterName = QString::fromWCharArray(description.Description);
            return device;
        }
    }

    return {};
}

int countMessages(
    ID3D12InfoQueue *queue, D3D12_MESSAGE_SEVERITY severity) {
    int count = 0;
    const UINT64 total = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < total; ++index) {
        SIZE_T length = 0;
        queue->GetMessage(index, nullptr, &length);
        std::vector<std::uint8_t> storage(length);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(queue->GetMessage(index, message, &length))
            && message->Severity == severity) {
            ++count;
        }
    }

    return count;
}

QString firstMessage(
    ID3D12InfoQueue *queue, D3D12_MESSAGE_SEVERITY severity) {
    const UINT64 total = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < total; ++index) {
        SIZE_T length = 0;
        queue->GetMessage(index, nullptr, &length);
        std::vector<std::uint8_t> storage(length);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(queue->GetMessage(index, message, &length))
            && message->Severity == severity) {
            return QString::fromUtf8(message->pDescription);
        }
    }
    return {};
}

void configureSampler(D3D12_STATIC_SAMPLER_DESC *sampler, UINT shaderRegister) {
    sampler->Filter = shaderRegister == 10
        ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
        : shaderRegister == 0
        ? D3D12_FILTER_ANISOTROPIC
        : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler->AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler->AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler->AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler->BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    if (shaderRegister == 12 || shaderRegister == 13) {
        sampler->AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler->AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler->AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
    if (shaderRegister == 13) {
        sampler->Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sampler->AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler->AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler->AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler->BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    }
    sampler->MaxAnisotropy = shaderRegister == 0 ? 16 : 1;
    sampler->ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    sampler->MinLOD = 0.0f;
    sampler->MaxLOD = FLT_MAX;
    sampler->ShaderRegister = shaderRegister;
    sampler->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
}

D3D12_SAMPLER_DESC materialSamplerDescription(
    const fh6::ModelMaterialSampler &source) {
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = source.authored && source.filter == 0
        ? D3D12_FILTER_MIN_MAG_MIP_POINT
        : source.authored && source.filter == 1
        ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
        : D3D12_FILTER_ANISOTROPIC;
    const auto addressMode = [](qint32 value) {
        switch (value) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    };
    sampler.AddressU = addressMode(source.addressU);
    sampler.AddressV = addressMode(source.addressV);
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = sampler.Filter == D3D12_FILTER_ANISOTROPIC ? 16 : 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = FLT_MAX;
    return sampler;
}

ComPtr<ID3D12RootSignature> createRootSignature(
    ID3D12Device *device, QString *error) {
    constexpr std::array<Binding, 8> cbvs = {{
        {1, 2}, {1, 0}, {9, 0}, {2, 0},
        {3, 0}, {0, 2}, {0, 1}, {0, 3},
    }};
    std::array<D3D12_DESCRIPTOR_RANGE, 5> ranges{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 124, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0};
    ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 6, 0};
    ranges[3] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 0, 12, 0};
    ranges[4] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};

    std::array<D3D12_ROOT_PARAMETER, cbvs.size() + ranges.size()> parameters{};
    for (std::size_t index = 0; index < cbvs.size(); ++index) {
        parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[index].Descriptor.ShaderRegister = cbvs[index].shaderRegister;
        parameters[index].Descriptor.RegisterSpace = cbvs[index].registerSpace;
        parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        D3D12_ROOT_PARAMETER &parameter = parameters[cbvs.size() + index];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &ranges[index];
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    constexpr std::array<UINT, 7> samplerRegisters = {1, 2, 3, 5, 10, 12, 13};
    std::array<D3D12_STATIC_SAMPLER_DESC, samplerRegisters.size()> samplers{};
    for (std::size_t index = 0; index < samplers.size(); ++index) {
        configureSampler(&samplers[index], samplerRegisters[index]);
    }

    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size());
    description.pParameters = parameters.data();
    description.NumStaticSamplers = static_cast<UINT>(samplers.size());
    description.pStaticSamplers = samplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
        | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
        | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> diagnostics;
    HRESULT result = D3D12SerializeRootSignature(
        &description, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &diagnostics);
    if (FAILED(result)) {
        const QString detail = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : hresultText(result);
        *error = QStringLiteral("root-signature serialization failed: %1").arg(detail);
        return {};
    }

    ComPtr<ID3D12RootSignature> signature;
    result = device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&signature));
    if (FAILED(result)) {
        *error = QStringLiteral("root-signature creation failed: %1").arg(hresultText(result));
        return {};
    }
    return signature;
}

D3D12_BLEND_DESC blendDescription(bool translucent = false) {
    D3D12_BLEND_DESC description{};
    D3D12_RENDER_TARGET_BLEND_DESC target{};
    target.BlendEnable = translucent ? TRUE : FALSE;
    target.SrcBlend = translucent ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
    target.DestBlend = translucent
        ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_ZERO;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (auto &entry : description.RenderTarget) {
        entry = target;
    }
    return description;
}

D3D12_RASTERIZER_DESC rasterizerDescription() {
    D3D12_RASTERIZER_DESC description{};
    description.FillMode = D3D12_FILL_MODE_SOLID;
    description.CullMode = D3D12_CULL_MODE_NONE;
    description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    description.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    description.DepthClipEnable = TRUE;
    return description;
}

D3D12_DEPTH_STENCIL_DESC depthDescription(bool writeDepth = true) {
    D3D12_DEPTH_STENCIL_DESC description{};
    description.DepthEnable = TRUE;
    description.DepthWriteMask = writeDepth
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    return description;
}

ComPtr<ID3D12PipelineState> createPanoramaPipeline(
    ID3D12Device *device, ID3D12RootSignature *rootSignature,
    QString *error, DXGI_FORMAT targetFormat = kTargetFormat) {
    static constexpr const char *kShader = R"hlsl(
cbuffer FrameData : register(b0, space2) {
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 directColor;
    float4 ambientColor;
    float4 cameraForward;
    float4 cameraRight;
    float4 cameraUp;
    float4 panoramaParameters;
    float4 pointLightPositionRange[32];
    float4 pointLightColorIntensity[32];
    float4 pointLightDirectionOuter[32];
    float4 pointLightInner[32];
    float4 colorGradeParameters;
    row_major float4x4 shadowViewProjection;
    float4 shadowParameters;
    row_major float4x4 dropShadowViewProjection;
    float4 dropShadowParameters;
    row_major float4x4 localShadowViewProjection0;
    float4 localShadowParameters0;
    row_major float4x4 localShadowViewProjection1;
    float4 localShadowParameters1;
    float4 reflectionProbeMinimum;
    float4 reflectionProbeMaximum;
    float4 reflectionProbePosition;
    row_major float4x4 previousViewProjection;
    float4 temporalParameters;
};
Texture2D<float4> panoramaTexture : register(t120);
Texture3D<float4> colorGradeLut : register(t123);
SamplerState panoramaSampler : register(s0);

float3 pqEncode(float3 value) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 powered = pow(saturate(value), m1);
    return pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}

float3 hableCurve(float3 value) {
    const float a = 0.151;
    const float b = 0.05;
    const float c = 0.566;
    const float d = 0.21;
    const float e = 0.003;
    const float f = 0.141;
    return ((value * (a * value + c * b) + d * e)
            / (value * (a * value + b) + d * f)) - e / f;
}

float3 applyColorGrade(float3 color) {
    // The Homespace LUT's transfer domain and ordering are not established by
    // the decoded assets.  Feeding linear HDR through a guessed PQ transform
    // overexposes the entire scene.  Keep the documented preview fallback:
    // linear HDR -> exposure -> Hable, exactly once per shaded result.
    float3 exposed = max(color, 0.0) * exp2(-0.4);
    return hableCurve(exposed) / hableCurve(float3(1.5, 1.5, 1.5));
}

struct PixelInput {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

PixelInput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    PixelInput output;
    output.ndc = positions[vertexId];
    output.position = float4(output.ndc, 1.0, 1.0);
    return output;
}

float4 PSMain(PixelInput input) : SV_Target0 {
    float2 sampleNdc = input.ndc - temporalParameters.zw;
    float3 direction = normalize(
        cameraForward.xyz
        + cameraRight.xyz * sampleNdc.x * cameraRight.w * panoramaParameters.z
        + cameraUp.xyz * sampleNdc.y * panoramaParameters.z);
    float theta = acos(saturate(abs(direction.y)));
    float polar = theta * (2.0 / 3.14159265358979323846);
    float radius = theta * (1.0 / 3.14159265358979323846)
        * lerp(1.0, polar, saturate(panoramaParameters.x));
    float2 horizontal = float2(direction.z, -direction.x);
    float horizontalLength = length(horizontal);
    float2 disk = horizontalLength > 0.000001
        ? horizontal * (radius / horizontalLength) : float2(0.0, 0.0);
    float2 uv = float2((disk.x + 0.5) * 0.5, disk.y + 0.5);
    if (direction.y < 0.0) {
        uv.x += 0.5;
        uv.y = 1.0 - uv.y;
    }
    float3 color = panoramaTexture.SampleLevel(panoramaSampler, uv, 0).rgb
        / max(panoramaParameters.y, 0.000001);
    return float4(color, 1.0);
}
)hlsl";
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> diagnostics;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT result = D3DCompile(
        kShader, std::strlen(kShader), "tokyo_panorama.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_1", flags, 0, &vertexShader, &diagnostics);
    if (FAILED(result)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 panorama vertex shader compilation failed");
        return {};
    }
    diagnostics.Reset();
    result = D3DCompile(
        kShader, std::strlen(kShader), "tokyo_panorama.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_1", flags, 0, &pixelShader, &diagnostics);
    if (FAILED(result)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 panorama pixel shader compilation failed");
        return {};
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature;
    description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    description.BlendState = blendDescription(false);
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizerDescription();
    description.DepthStencilState = depthDescription(false);
    description.DepthStencilState.DepthEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = targetFormat;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        *error = QStringLiteral("DX12 panorama pipeline creation failed: %1")
            .arg(hresultText(result));
        return {};
    }
    return pipeline;
}

ComPtr<ID3D12PipelineState> createPostPipeline(
    ID3D12Device *device, ID3D12RootSignature *rootSignature,
    QString *error, DXGI_FORMAT targetFormat = kTargetFormat) {
    static constexpr const char *kShader = R"hlsl(
cbuffer FrameData : register(b0, space2) {
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 directColor;
    float4 ambientColor;
    float4 cameraForward;
    float4 cameraRight;
    float4 cameraUp;
    float4 panoramaParameters;
    float4 pointLightPositionRange[32];
    float4 pointLightColorIntensity[32];
    float4 pointLightDirectionOuter[32];
    float4 pointLightInner[32];
    float4 colorGradeParameters;
    row_major float4x4 shadowViewProjection;
    float4 shadowParameters;
    row_major float4x4 dropShadowViewProjection;
    float4 dropShadowParameters;
    row_major float4x4 localShadowViewProjection0;
    float4 localShadowParameters0;
    row_major float4x4 localShadowViewProjection1;
    float4 localShadowParameters1;
    float4 reflectionProbeMinimum;
    float4 reflectionProbeMaximum;
    float4 reflectionProbePosition;
    row_major float4x4 previousViewProjection;
    float4 temporalParameters;
};
Texture2D<float> sceneDepth : register(t116);
Texture2D<float4> hdrScene : register(t122);
Texture2D<float4> temporalHistory0 : register(t110);
Texture2D<float4> temporalHistory1 : register(t111);
SamplerState postSampler : register(s12);

struct PixelInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

PixelInput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    PixelInput output;
    float2 ndc = positions[vertexId];
    output.position = float4(ndc, 0.0, 1.0);
    output.uv = ndc * float2(0.5, -0.5) + 0.5;
    return output;
}

float linearDepth(float depth) {
    float nearPlane = max(cameraPosition.w, 0.0001);
    float farPlane = max(cameraForward.w, nearPlane + 0.001);
    float a = farPlane / (farPlane - nearPlane);
    float b = nearPlane * farPlane / (farPlane - nearPlane);
    return b / max(a - depth, 0.000001);
}

float3 viewPosition(float2 uv, float depth) {
    float z = linearDepth(depth);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0)
        - temporalParameters.zw;
    float tanHalfFov = panoramaParameters.z;
    float aspect = cameraRight.w;
    return float3(
        ndc.x * z * tanHalfFov * aspect,
        ndc.y * z * tanHalfFov,
        z);
}

float screenSpaceOcclusion(float2 uv, float centerDepth) {
    if (centerDepth >= 0.999999) {
        return 1.0;
    }
    uint width;
    uint height;
    sceneDepth.GetDimensions(width, height);
    float2 texel = rcp(float2(width, height));
    float3 center = viewPosition(uv, centerDepth);
    float depthRight = sceneDepth.SampleLevel(
        postSampler, uv + float2(texel.x, 0.0), 0);
    float depthDown = sceneDepth.SampleLevel(
        postSampler, uv + float2(0.0, texel.y), 0);
    float3 right = viewPosition(uv + float2(texel.x, 0.0), depthRight);
    float3 down = viewPosition(uv + float2(0.0, texel.y), depthDown);
    float3 normal = normalize(cross(down - center, right - center));
    if (normal.z > 0.0) {
        normal = -normal;
    }
    const float2 disk[12] = {
        float2(1.0, 0.0), float2(-1.0, 0.0),
        float2(0.0, 1.0), float2(0.0, -1.0),
        float2(0.707, 0.707), float2(-0.707, 0.707),
        float2(0.707, -0.707), float2(-0.707, -0.707),
        float2(0.383, 0.924), float2(-0.924, 0.383),
        float2(0.924, -0.383), float2(-0.383, -0.924)
    };
    float radiusPixels = clamp(42.0 / max(center.z, 1.0), 4.0, 14.0);
    float radiusWorld = 0.025 + center.z * 0.018;
    float occlusion = 0.0;
    [unroll]
    for (int index = 0; index < 12; ++index) {
        float ring = 0.35 + 0.65 * ((index % 3) + 1.0) / 3.0;
        float2 sampleUv = saturate(
            uv + disk[index] * texel * radiusPixels * ring);
        float sampleDepth = sceneDepth.SampleLevel(postSampler, sampleUv, 0);
        if (sampleDepth >= 0.999999) {
            continue;
        }
        float3 delta = viewPosition(sampleUv, sampleDepth) - center;
        float distanceToSample = length(delta);
        float range = saturate(1.0 - distanceToSample / radiusWorld);
        float horizon = saturate(dot(normal, delta / max(distanceToSample, 0.0001)) - 0.06);
        occlusion += horizon * range;
    }
    return saturate(1.0 - occlusion * (0.78 / 12.0));
}

float3 hableCurve(float3 value) {
    const float a = 0.151;
    const float b = 0.05;
    const float c = 0.566;
    const float d = 0.21;
    const float e = 0.003;
    const float f = 0.141;
    return ((value * (a * value + c * b) + d * e)
            / (value * (a * value + b) + d * f)) - e / f;
}

struct PostOutput {
    float4 display : SV_Target0;
    float4 history : SV_Target1;
};

float3 currentRadiance(float2 uv, float ao) {
    return max(hdrScene.SampleLevel(postSampler, uv, 0).rgb, 0.0) * ao;
}

float3 gradeRadiance(float3 radiance) {
    // The staged Tokyo assets are illuminated by a cool probe, while the
    // in-game homespace grade retains a warmer neutral. Apply the balance in
    // linear light and preserve luminance so this does not become another
    // exposure adjustment.
    float sourceLuminance = dot(
        radiance, float3(0.2126, 0.7152, 0.0722));
    float3 balanced = radiance * float3(1.045, 1.008, 0.930);
    float balancedLuminance = dot(
        balanced, float3(0.2126, 0.7152, 0.0722));
    balanced *= sourceLuminance / max(balancedLuminance, 0.000001);
    float3 exposed = balanced * exp2(-0.4);
    return hableCurve(exposed) / hableCurve(float3(1.5, 1.5, 1.5));
}

PostOutput PSMain(PixelInput input) {
    float3 hdr = max(hdrScene.SampleLevel(postSampler, input.uv, 0).rgb, 0.0);
    float depth = sceneDepth.SampleLevel(postSampler, input.uv, 0);
    float ao = screenSpaceOcclusion(input.uv, depth);
    float3 resolvedRadiance = hdr * ao;
    if (temporalParameters.x > 0.5 && depth < 0.999999) {
        float3 view = viewPosition(input.uv, depth);
        float3 world = cameraPosition.xyz
            + cameraRight.xyz * view.x
            + cameraUp.xyz * view.y
            + cameraForward.xyz * view.z;
        float4 previousClip = mul(
            previousViewProjection, float4(world, 1.0));
        float3 previousNdc = previousClip.xyz / max(previousClip.w, 0.000001);
        float2 previousUv = previousNdc.xy * float2(0.5, -0.5) + 0.5;
        if (previousClip.w > 0.0 && all(previousUv > 0.0)
            && all(previousUv < 1.0)) {
            float4 history = temporalParameters.y < 0.5
                ? temporalHistory0.SampleLevel(postSampler, previousUv, 0)
                : temporalHistory1.SampleLevel(postSampler, previousUv, 0);
            float depthAgreement = 1.0 - smoothstep(
                0.0004, 0.004,
                abs(history.a - saturate(previousNdc.z)));
            uint width;
            uint height;
            hdrScene.GetDimensions(width, height);
            float2 texel = rcp(float2(width, height));
            float3 neighborhoodMinimum = resolvedRadiance;
            float3 neighborhoodMaximum = resolvedRadiance;
            [unroll]
            for (int y = -1; y <= 1; ++y) {
                [unroll]
                for (int x = -1; x <= 1; ++x) {
                    float3 neighbor = currentRadiance(
                        saturate(input.uv + float2(x, y) * texel), ao);
                    neighborhoodMinimum = min(neighborhoodMinimum, neighbor);
                    neighborhoodMaximum = max(neighborhoodMaximum, neighbor);
                }
            }
            float3 clampedHistory = clamp(
                history.rgb, neighborhoodMinimum, neighborhoodMaximum);
            float velocity = length(input.uv - previousUv);
            // A 0.88 bilinear history contribution visibly erased livery and
            // garage detail. Keep enough current-frame energy for stable
            // texture detail while retaining sub-pixel edge accumulation.
            float historyWeight = 0.72 * depthAgreement
                * exp2(-velocity * 64.0);
            resolvedRadiance = lerp(
                resolvedRadiance, clampedHistory, historyWeight);
        }
    }
    // Restore only high-frequency information from the current jittered
    // frame. Depth compatibility prevents the unsharp term from producing
    // silhouettes around the car; the resolved value remains in history so
    // sharpening cannot accumulate into temporal ringing.
    uint sharpenWidth;
    uint sharpenHeight;
    hdrScene.GetDimensions(sharpenWidth, sharpenHeight);
    float2 sharpenTexel = rcp(float2(sharpenWidth, sharpenHeight));
    const float2 crossOffsets[4] = {
        float2(-1.0, 0.0), float2(1.0, 0.0),
        float2(0.0, -1.0), float2(0.0, 1.0)};
    float3 currentCenter = hdr * ao;
    float3 crossAverage = 0.0;
    [unroll]
    for (int sharpenIndex = 0; sharpenIndex < 4; ++sharpenIndex) {
        float2 neighborUv = saturate(
            input.uv + crossOffsets[sharpenIndex] * sharpenTexel);
        float neighborDepth = sceneDepth.SampleLevel(
            postSampler, neighborUv, 0);
        float compatible = 0.0;
        if (depth >= 0.999999) {
            compatible = neighborDepth >= 0.999999 ? 1.0 : 0.0;
        } else if (neighborDepth < 0.999999) {
            compatible = 1.0 - smoothstep(
                0.006, 0.06,
                abs(linearDepth(neighborDepth) - linearDepth(depth)));
        }
        float3 neighbor = currentRadiance(neighborUv, ao);
        crossAverage += lerp(currentCenter, neighbor, compatible);
    }
    crossAverage *= 0.25;
    float3 detail = clamp(currentCenter - crossAverage, -0.22, 0.22);
    float sharpenStrength = temporalParameters.x > 0.5 ? 0.30 : 0.12;
    float3 displayRadiance = max(
        resolvedRadiance + detail * sharpenStrength, 0.0);
    PostOutput output;
    output.display = float4(gradeRadiance(displayRadiance), 1.0);
    output.history = float4(resolvedRadiance, depth);
    return output;
}
)hlsl";
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> diagnostics;
    constexpr UINT flags =
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT result = D3DCompile(
        kShader, std::strlen(kShader), "tokyo_post.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_1", flags, 0, &vertexShader, &diagnostics);
    if (FAILED(result)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 post vertex shader compilation failed");
        return {};
    }
    diagnostics.Reset();
    result = D3DCompile(
        kShader, std::strlen(kShader), "tokyo_post.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_1", flags, 0, &pixelShader, &diagnostics);
    if (FAILED(result)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 post pixel shader compilation failed");
        return {};
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature;
    description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    description.BlendState = blendDescription(false);
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizerDescription();
    description.DepthStencilState = depthDescription(false);
    description.DepthStencilState.DepthEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 2;
    description.RTVFormats[0] = targetFormat;
    description.RTVFormats[1] = kTargetFormat;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        *error = QStringLiteral("DX12 post pipeline creation failed: %1")
            .arg(hresultText(result));
        return {};
    }
    return pipeline;
}

ComPtr<ID3D12PipelineState> createMaterialPipeline(
    ID3D12Device *device, ID3D12RootSignature *rootSignature,
    QString *error, DXGI_FORMAT targetFormat = kTargetFormat,
    bool translucent = false) {
    const QByteArray shader = QByteArrayLiteral(R"hlsl(
cbuffer FrameData : register(b0, space2) {
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 lightDirection;
    float4 directColor;
    float4 ambientColor;
    float4 cameraForward;
    float4 cameraRight;
    float4 cameraUp;
    float4 panoramaParameters;
    float4 pointLightPositionRange[32];
    float4 pointLightColorIntensity[32];
    float4 pointLightDirectionOuter[32];
    float4 pointLightInner[32];
    float4 colorGradeParameters;
    row_major float4x4 shadowViewProjection;
    float4 shadowParameters;
    row_major float4x4 dropShadowViewProjection;
    float4 dropShadowParameters;
    row_major float4x4 localShadowViewProjection0;
    float4 localShadowParameters0;
    row_major float4x4 localShadowViewProjection1;
    float4 localShadowParameters1;
    float4 reflectionProbeMinimum;
    float4 reflectionProbeMaximum;
    float4 reflectionProbePosition;
};
cbuffer MaterialData : register(b0, space3) {
    float4 baseColor;
    float4 emissiveColorAndMap;
    float4 surfaceParameters;
    float4 textureParameters;
    float4 detailTextureParameters;
    uint4 liveryParameters;
    float4 liverySourceRegions[11];
    float4 liveryPaintRegions[11];
    float4 liveryFacing[11];
    float4 clearCoatParameters;
    float4 clearCoatTint;
    float4 normalParameters;
    float4 weaveColorTintA;
    float4 weaveColorTintB;
    uint4 materialFamilyParameters;
    float4 secondaryPaintAndStrength;
    float4 flakeColorAndCoverage;
    float4 automotivePaintParameters;
    uint4 renderPassParameters;
};
TextureCube<float4> diffuseEnvironment : register(t0);
TextureCube<float4> specularEnvironment : register(t1);
Texture2D<float> shadowMap : register(t119);
Texture2D<float> dropShadowMap : register(t121);
Texture2D<float> localShadowMap0 : register(t114);
Texture2D<float> localShadowMap1 : register(t115);
Texture2D<float> glassBackDepth : register(t113);
Texture3D<float4> colorGradeLut : register(t123);
Texture2D<float4> baseColorTexture : register(t0, space1);
Texture2D<float4> normalTexture : register(t1, space1);
Texture2D<float4> surfaceTexture : register(t2, space1);
Texture2D<float4> emissiveTexture : register(t3, space1);
Texture2D<float4> liveryMasks[11] : register(t4, space1);
Texture2D<float4> alphaTexture : register(t15, space1);
Texture2D<float4> weaveMaskTexture : register(t16, space1);
Texture2D<float4> weaveNormalTexture : register(t17, space1);
Texture2D<float4> clearCoatNormalTexture : register(t18, space1);
Texture2D<float4> tireHeightAoTexture : register(t19, space1);
Texture2D<float4> aoTexture : register(t20, space1);
SamplerState materialSampler : register(s0);
SamplerState liverySampler : register(s12);
SamplerComparisonState shadowSampler : register(s13);

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float2 uv2 : TEXCOORD2;
};
struct PixelInput {
    float4 position : SV_Position;
    float3 worldPosition : WORLDPOS;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    float2 detailUv : TEXCOORD2;
};

PixelInput VSMain(VertexInput input) {
    PixelInput output;
    output.position = mul(viewProjection, float4(input.position, 1.0));
    output.worldPosition = input.position;
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.uv = input.uv;
    output.detailUv = input.uv2;
    return output;
}

float3 fresnelSchlick(float cosine, float3 f0) {
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosine), 5.0);
}

float3 fresnelSchlickRoughness(
    float cosine, float3 f0, float roughness) {
    return f0 + (max(1.0 - roughness, f0) - f0)
        * pow(saturate(1.0 - cosine), 5.0);
}

float3 reflectionLookupDirection(float3 worldPosition, float3 rayDirection) {
    if (liveryParameters.w != 2u || reflectionProbeMinimum.w < 0.5) {
        return rayDirection;
    }
    float3 safeDirection = sign(rayDirection)
        * max(abs(rayDirection), float3(0.0001, 0.0001, 0.0001));
    float3 exitPlane = lerp(
        reflectionProbeMinimum.xyz, reflectionProbeMaximum.xyz,
        step(0.0, safeDirection));
    float3 distances = (exitPlane - worldPosition) / safeDirection;
    float distanceToWall = min(distances.x, min(distances.y, distances.z));
    if (distanceToWall <= 0.0) {
        return rayDirection;
    }
    float3 wallPosition = worldPosition + safeDirection * distanceToWall;
    return normalize(wallPosition - reflectionProbePosition.xyz);
}

float linearCameraDepth(float depth) {
    float nearPlane = max(cameraPosition.w, 0.0001);
    float farPlane = max(cameraForward.w, nearPlane + 0.001);
    float a = farPlane / (farPlane - nearPlane);
    float b = nearPlane * farPlane / (farPlane - nearPlane);
    return b / max(a - depth, 0.000001);
}

float glassPathLength(PixelInput input, float3 viewDirection) {
    if (renderPassParameters.x != 0u
        || dot(normalize(input.normal), viewDirection) <= 0.0) {
        return 0.0;
    }
    uint width;
    uint height;
    glassBackDepth.GetDimensions(width, height);
    int2 pixel = clamp(
        int2(input.position.xy), int2(0, 0),
        int2((int)width - 1, (int)height - 1));
    float backDepth = glassBackDepth.Load(int3(pixel, 0));
    if (backDepth >= 0.999999 || backDepth <= input.position.z + 0.000001) {
        return 0.0;
    }
    return clamp(
        linearCameraDepth(backDepth) - linearCameraDepth(input.position.z),
        0.0, 0.12);
}

float3 environmentBrdfApproximation(
    float3 f0, float roughness, float ndotv) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndotv)) * r.x + r.y;
    float2 scaleBias = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * scaleBias.x + scaleBias.y;
}

float specularOcclusion(float ndotv, float ao, float roughness) {
    return saturate(
        pow(ndotv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

float3 evaluateDirectClearCoat(
    float3 n, float3 v, float3 l, float roughness) {
    float ndotl = saturate(dot(n, l));
    float ndotv = saturate(dot(n, v));
    if (ndotl <= 0.0) {
        return 0.0;
    }
    float3 h = normalize(v + l);
    float ndoth = saturate(dot(n, h));
    float vdoth = saturate(dot(v, h));
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = ndoth * ndoth * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2
        / max(3.14159265 * denominator * denominator, 0.0001);
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    float geometryV = ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
    float geometryL = ndotl / max(ndotl * (1.0 - k) + k, 0.0001);
    float3 fresnel = fresnelSchlick(vdoth, float3(0.04, 0.04, 0.04));
    return distribution * geometryV * geometryL * fresnel
        / max(4.0 * ndotv * ndotl, 0.0001) * ndotl;
}

float3 pqEncode(float3 value) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 powered = pow(saturate(value), m1);
    return pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}

float3 hableCurve(float3 value) {
    const float a = 0.151;
    const float b = 0.05;
    const float c = 0.566;
    const float d = 0.21;
    const float e = 0.003;
    const float f = 0.141;
    return ((value * (a * value + c * b) + d * e)
            / (value * (a * value + b) + d * f)) - e / f;
}

float3 applyColorGrade(float3 color) {
    float3 exposed = max(color, 0.0) * exp2(-0.4);
    return hableCurve(exposed) / hableCurve(float3(1.5, 1.5, 1.5));
}

float2 canvasToUv(float2 canvas) {
    return float2((canvas.x + 1024.0) / 2048.0,
                  (512.0 - canvas.y) / 1024.0);
}

float3 applyMappedNormal(
    float3 normal, float4 tangent, float3 sampled, float intensity) {
    float3 t = normalize(tangent.xyz - normal * dot(normal, tangent.xyz));
    float3 b = normalize(cross(normal, t) * tangent.w);
    sampled = sampled * 2.0 - 1.0;
    sampled.xy *= max(intensity, 0.0);
    sampled.z = sqrt(max(1.0 - dot(sampled.xy, sampled.xy), 0.0));
    return normalize(t * sampled.x + b * sampled.y + normal * sampled.z);
}

)hlsl") + QByteArrayLiteral(R"hlsl(

static const float2 shadowDisk[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
};

float shadowVisibility(float3 worldPosition) {
    if (shadowParameters.w < 0.5) {
        return 1.0;
    }
    float4 projected = mul(
        shadowViewProjection, float4(worldPosition, 1.0));
    float3 shadowCoordinate = projected.xyz / projected.w;
    float2 uv = shadowCoordinate.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)
        || shadowCoordinate.z <= 0.0 || shadowCoordinate.z >= 1.0) {
        return 1.0;
    }
    // Approximate the area-light penumbra from the distance between the
    // receiver and nearby blockers.  Contact remains defined while gaps
    // between car parts soften, unlike the old fixed square PCF kernel.
    float receiverDepth = shadowCoordinate.z - shadowParameters.z;
    float blockerDepth = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (int blockerIndex = 0; blockerIndex < 16; ++blockerIndex) {
        int2 texel = int2(uv * 2048.0 + shadowDisk[blockerIndex] * 5.0);
        texel = clamp(texel, int2(0, 0), int2(2047, 2047));
        float depth = shadowMap.Load(int3(texel, 0));
        if (depth < receiverDepth) {
            blockerDepth += depth;
            blockerCount += 1.0;
        }
    }
    float averageBlocker = blockerCount > 0.0
        ? blockerDepth / blockerCount : receiverDepth;
    float separation = saturate((receiverDepth - averageBlocker) * 45.0);
    float filterRadius = lerp(2.5, 13.0, separation);
    float visibility = 0.0;
    [unroll]
    for (int sampleIndex = 0; sampleIndex < 16; ++sampleIndex) {
        visibility += shadowMap.SampleCmpLevelZero(
            shadowSampler,
            uv + shadowDisk[sampleIndex] * shadowParameters.x * filterRadius,
            receiverDepth);
    }
    return visibility / 16.0;
}

float dropShadowVisibility(float3 worldPosition) {
    if (dropShadowParameters.w < 0.5) {
        return 1.0;
    }
    float4 clip = mul(dropShadowViewProjection, float4(worldPosition, 1.0));
    float3 projected = clip.xyz / max(clip.w, 0.000001);
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0
        || any(uv <= 0.0) || any(uv >= 1.0)) {
        return 1.0;
    }
    float receiverDepth = projected.z - dropShadowParameters.z;
    float blockerDepth = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (int blockerIndex = 0; blockerIndex < 16; ++blockerIndex) {
        int2 texel = int2(uv * 2048.0 + shadowDisk[blockerIndex] * 7.0);
        texel = clamp(texel, int2(0, 0), int2(2047, 2047));
        float depth = dropShadowMap.Load(int3(texel, 0));
        if (depth < receiverDepth) {
            blockerDepth += depth;
            blockerCount += 1.0;
        }
    }
    float averageBlocker = blockerCount > 0.0
        ? blockerDepth / blockerCount : receiverDepth;
    float separation = saturate((receiverDepth - averageBlocker) * 38.0);
    float filterRadius = lerp(7.0, 22.0, separation);
    float visibility = 0.0;
    [unroll]
    for (int sampleIndex = 0; sampleIndex < 16; ++sampleIndex) {
        visibility += dropShadowMap.SampleCmpLevelZero(
            shadowSampler,
            uv + shadowDisk[sampleIndex] * dropShadowParameters.x
                * filterRadius,
            receiverDepth);
    }
    return visibility / 16.0;
}

float localLightShadowVisibility(float3 worldPosition, uint shadowIndex) {
    float4 parameters = shadowIndex == 0u
        ? localShadowParameters0 : localShadowParameters1;
    if (parameters.w < 0.5) {
        return 1.0;
    }
    float4 clip = shadowIndex == 0u
        ? mul(localShadowViewProjection0, float4(worldPosition, 1.0))
        : mul(localShadowViewProjection1, float4(worldPosition, 1.0));
    float3 projected = clip.xyz / max(clip.w, 0.000001);
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0
        || any(uv <= 0.0) || any(uv >= 1.0)) {
        return 1.0;
    }
    float receiverDepth = projected.z - parameters.y;
    float blockerDepth = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (int blockerIndex = 0; blockerIndex < 16; ++blockerIndex) {
        int2 texel = int2(uv * 2048.0 + shadowDisk[blockerIndex] * 5.0);
        texel = clamp(texel, int2(0, 0), int2(2047, 2047));
        float depth = shadowIndex == 0u
            ? localShadowMap0.Load(int3(texel, 0))
            : localShadowMap1.Load(int3(texel, 0));
        if (depth < receiverDepth) {
            blockerDepth += depth;
            blockerCount += 1.0;
        }
    }
    float averageBlocker = blockerCount > 0.0
        ? blockerDepth / blockerCount : receiverDepth;
    float separation = saturate((receiverDepth - averageBlocker) * 55.0);
    float filterRadius = lerp(3.5, 14.0, separation);
    float visibility = 0.0;
    [unroll]
    for (int sampleIndex = 0; sampleIndex < 16; ++sampleIndex) {
        float2 sampleUv = uv + shadowDisk[sampleIndex]
            * shadowParameters.x * filterRadius;
        visibility += shadowIndex == 0u
            ? localShadowMap0.SampleCmpLevelZero(
                  shadowSampler, sampleUv, receiverDepth)
            : localShadowMap1.SampleCmpLevelZero(
                  shadowSampler, sampleUv, receiverDepth);
    }
    return visibility / 16.0;
}

float finishGlitter(float3 worldPosition, float facing) {
    float coverage = saturate(flakeColorAndCoverage.w);
    if (coverage <= 0.0) {
        return 0.0;
    }
    float3 cell = floor(worldPosition * 900.0);
    float randomValue = frac(
        sin(dot(cell, float3(12.9898, 78.233, 37.719))) * 43758.5453);
    float randomFacing = frac(randomValue * 251.7);
    float active = step(1.0 - 0.3 * coverage, randomValue);
    float highlightStart = lerp(
        0.97, 0.84, saturate(automotivePaintParameters.x));
    float lit = smoothstep(
        highlightStart, 1.0, facing + (randomFacing - 0.5) * 0.05);
    return active * lit;
}

)hlsl") + QByteArrayLiteral(R"hlsl(

float4 PSMain(PixelInput input) : SV_Target0 {
    float2 uv = input.uv * textureParameters.yz;
    float2 detailUv = input.detailUv * detailTextureParameters.xy;
    float3 earlyView = normalize(cameraPosition.xyz - input.worldPosition);
    float edge = pow(
        1.0 - saturate(dot(normalize(input.normal), earlyView)),
        max(automotivePaintParameters.z, 0.01));
    float glancingMix = automotivePaintParameters.w > 0.5
        ? saturate(secondaryPaintAndStrength.w * edge) : 0.0;
    float3 surfacePaint = lerp(
        baseColor.rgb, secondaryPaintAndStrength.rgb, glancingMix);
    float4 authoredBase = float4(1, 1, 1, 1);
    float liveryCoverage = 0.0;
    float3 albedo = surfacePaint;
    if (liveryParameters.x != 0) {
        float2 atlasUv = float2(input.uv.x * 0.5, input.uv.y);
        float2 paintUv = float2(atlasUv.x, 1.0 - atlasUv.y);
        float bestCoverage = 0.0;
        int coveredSide = -1;
        [loop]
        for (uint side = 0; side < min(liveryParameters.z, 11u); ++side) {
            if ((liveryParameters.y & (1u << side)) == 0
                || dot(liveryFacing[side].xyz, normalize(input.normal)) <= 0.0) {
                continue;
            }
            float coverage = liveryMasks[side].Sample(liverySampler, atlasUv).r;
            if (coverage > bestCoverage) {
                bestCoverage = coverage;
                coveredSide = int(side);
            }
        }
        if (coveredSide >= 0) {
            float4 sourceRegion = liverySourceRegions[coveredSide];
            float2 sourceStart = canvasToUv(float2(sourceRegion.x, sourceRegion.z));
            float2 sourceEnd = canvasToUv(float2(sourceRegion.y, sourceRegion.w));
            float2 sourceRange = sourceEnd - sourceStart;
            if (abs(sourceRange.x) > 0.000001 && abs(sourceRange.y) > 0.000001) {
                float2 sectionUv = (atlasUv - sourceStart) / sourceRange;
                if (coveredSide == 5 || coveredSide == 6 || coveredSide == 7) {
                    sectionUv = sectionUv.yx;
                }
                float4 paintRegion = liveryPaintRegions[coveredSide];
                paintUv = float2(
                    lerp(paintRegion.x, paintRegion.y, sectionUv.x),
                    lerp(paintRegion.z, paintRegion.w, sectionUv.y));
            }
        }
        authoredBase = bestCoverage > 0.5
            ? baseColorTexture.Sample(liverySampler, paintUv)
            : float4(0, 0, 0, 0);
        float3 liveryColor = pow(max(authoredBase.rgb, 0.0), 2.2);
        albedo = lerp(surfacePaint, liveryColor, authoredBase.a);
        liveryCoverage = authoredBase.a;
    } else if (textureParameters.x > 0.5) {
        authoredBase = baseColorTexture.Sample(materialSampler, uv);
        authoredBase.rgb = pow(max(authoredBase.rgb, 0.0), 2.2);
        albedo = surfacePaint * authoredBase.rgb;
    }
    if (materialFamilyParameters.x == 2u && materialFamilyParameters.y != 0u) {
        float weave = weaveMaskTexture.Sample(materialSampler, detailUv).r;
        albedo *= lerp(weaveColorTintA.rgb, weaveColorTintB.rgb, weave);
    }
    albedo = saturate(albedo);

    float3 n = normalize(input.normal);
    if (surfaceParameters.z > 0.5) {
        n = applyMappedNormal(
            n, input.tangent,
            normalTexture.Sample(materialSampler, detailUv).xyz,
            normalParameters.x);
    }
    if (materialFamilyParameters.x == 2u && materialFamilyParameters.z != 0u) {
        n = applyMappedNormal(
            n, input.tangent,
            weaveNormalTexture.Sample(materialSampler, detailUv).xyz,
            normalParameters.y);
    }
    float3 coatNormal = normalize(input.normal);
    if (materialFamilyParameters.w != 0u) {
        coatNormal = applyMappedNormal(
            coatNormal, input.tangent,
            clearCoatNormalTexture.Sample(
                materialSampler, detailUv * normalParameters.zw).xyz,
            normalParameters.x);
    }

    float roughness = saturate(1.0 - surfaceParameters.x);
    float metallic = saturate(surfaceParameters.y);
    float ao = 1.0;
    if (surfaceParameters.w > 0.5) {
        float3 rmao = surfaceTexture.Sample(materialSampler, detailUv).rgb;
        roughness = saturate(rmao.r);
        metallic = saturate(rmao.g);
        ao = saturate(rmao.b);
    }
    roughness = saturate(roughness + clearCoatTint.w);
    if (clearCoatParameters.w == 1.0 || clearCoatParameters.w == 3.0) {
        float4 tireHeightAo = tireHeightAoTexture.Sample(materialSampler, detailUv);
        ao *= saturate(tireHeightAo.g);
    }
    if (clearCoatParameters.w >= 2.0) {
        ao *= saturate(aoTexture.Sample(materialSampler, detailUv).r);
    }
    roughness = max(roughness, 0.045);

    float3 v = normalize(cameraPosition.xyz - input.worldPosition);
    float3 l = normalize(-lightDirection.xyz);
    float3 h = normalize(v + l);
    float ndotl = saturate(dot(n, l));
    float ndotv = saturate(dot(n, v));
    float ndoth = saturate(dot(n, h));
    float vdoth = saturate(dot(v, h));
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = ndoth * ndoth * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(3.14159265 * denominator * denominator, 0.0001);
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    float geometryV = ndotv / max(ndotv * (1.0 - k) + k, 0.0001);
    float geometryL = ndotl / max(ndotl * (1.0 - k) + k, 0.0001);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 fresnel = fresnelSchlick(vdoth, f0);
    float3 specular = distribution * geometryV * geometryL * fresnel
        / max(4.0 * ndotv * ndotl, 0.0001);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * albedo / 3.14159265;
    float3 environment = diffuseEnvironment.SampleLevel(materialSampler, n, 0).rgb;
    float3 reflected = reflect(-v, n);
    float3 reflectedLookup = reflectionLookupDirection(
        input.worldPosition, reflected);
    float3 reflectedEnvironment = specularEnvironment.SampleLevel(
        materialSampler, reflectedLookup, roughness * 9.0).rgb;
    float rawShadowVisibility = 1.0;
    float directionalVisibility = 1.0;
    if (liveryParameters.w == 2u) {
        rawShadowVisibility = shadowVisibility(input.worldPosition);
        directionalVisibility = lerp(
            1.0, rawShadowVisibility, shadowParameters.y);
    }
    // Local fixtures use only their matching shadow maps. Authored AO and the
    // final screen-space pass handle cavity occlusion without imposing the
    // compatibility light's direction on the entire car.
    float compatibilityKey = liveryParameters.w == 2u ? 0.28 : 1.0;
    float3 color = (diffuse + specular) * directColor.rgb * compatibilityKey
        * ndotl * directionalVisibility;
    float glitterFacing = ndotl > 0.0 ? saturate(dot(n, h)) : 0.0;
    float coatIntensity = saturate(clearCoatParameters.x)
        * lerp(1.0, clearCoatParameters.z, saturate(liveryCoverage));
    float coatRoughness = clamp(clearCoatParameters.y, 0.04, 1.0);
    float3 clearCoatDirect = evaluateDirectClearCoat(
        coatNormal, v, l, coatRoughness) * directColor.rgb
        * compatibilityKey * directionalVisibility;
    [loop]
    for (uint pointIndex = 0;
         pointIndex < min((uint)panoramaParameters.w, 32u); ++pointIndex) {
        float3 toLight = pointLightPositionRange[pointIndex].xyz
            - input.worldPosition;
        float distanceToLight = length(toLight);
        float3 pointDirection = toLight / max(distanceToLight, 0.0001);
        float rangeFade = saturate(
            1.0 - distanceToLight / pointLightPositionRange[pointIndex].w);
        rangeFade *= rangeFade;
        float coneCosine = dot(
            -pointDirection, pointLightDirectionOuter[pointIndex].xyz);
        float spot = smoothstep(
            pointLightDirectionOuter[pointIndex].w,
            pointLightInner[pointIndex].x, coneCosine);
        float pointNdotL = saturate(dot(n, pointDirection));
        float3 pointHalf = normalize(v + pointDirection);
        float pointNdotH = saturate(dot(n, pointHalf));
        float pointVdotH = saturate(dot(v, pointHalf));
        float pointDenominator = pointNdotH * pointNdotH * (alpha2 - 1.0) + 1.0;
        float pointDistribution = alpha2 / max(
            3.14159265 * pointDenominator * pointDenominator, 0.0001);
        float pointGeometryL = pointNdotL
            / max(pointNdotL * (1.0 - k) + k, 0.0001);
        float3 pointFresnel = fresnelSchlick(pointVdotH, f0);
        float3 pointSpecular = pointDistribution * geometryV * pointGeometryL
            * pointFresnel / max(4.0 * ndotv * pointNdotL, 0.0001);
        float3 pointDiffuse = (1.0 - pointFresnel) * (1.0 - metallic)
            * albedo / 3.14159265;
        float inverseSquare = rcp(max(distanceToLight * distanceToLight, 0.25));
        float radiance = pointLightColorIntensity[pointIndex].w
            * 0.00025 * inverseSquare * rangeFade * spot;
        float pointVisibility = 1.0;
        if (liveryParameters.w == 2u
            && localShadowParameters0.w > 0.5
            && abs((float)pointIndex - localShadowParameters0.z) < 0.5) {
            pointVisibility = lerp(
                1.0, localLightShadowVisibility(input.worldPosition, 0u),
                localShadowParameters0.x);
        } else if (liveryParameters.w == 2u
                   && localShadowParameters1.w > 0.5
                   && abs((float)pointIndex - localShadowParameters1.z) < 0.5) {
            pointVisibility = lerp(
                1.0, localLightShadowVisibility(input.worldPosition, 1u),
                localShadowParameters1.x);
        }
        if (pointNdotL > 0.0 && radiance > 0.0) {
            glitterFacing = max(glitterFacing, pointNdotH);
        }
        color += (pointDiffuse + pointSpecular)
            * pointLightColorIntensity[pointIndex].rgb * radiance * pointNdotL
            * pointVisibility;
        clearCoatDirect += evaluateDirectClearCoat(
                coatNormal, v, pointDirection, coatRoughness)
            * pointLightColorIntensity[pointIndex].rgb * radiance
            * pointVisibility;
    }
    float3 environmentFresnel = fresnelSchlickRoughness(
        ndotv, f0, roughness);
    color += albedo * ambientColor.rgb * ao;
    color += albedo * environment * 0.35
        * (1.0 - environmentFresnel) * (1.0 - metallic) * ao;
    color += reflectedEnvironment
        * environmentBrdfApproximation(f0, roughness, ndotv)
        * specularOcclusion(ndotv, ao, roughness);
    float glitter = finishGlitter(input.worldPosition, glitterFacing);
    color += flakeColorAndCoverage.rgb
        * (glitter * saturate(flakeColorAndCoverage.w)
           * max(automotivePaintParameters.y, 0.0))
        * environmentFresnel;
    if (coatIntensity > 0.01) {
        float coatNdotV = saturate(dot(coatNormal, v));
        float3 coatFresnel = fresnelSchlickRoughness(
            coatNdotV, float3(0.04, 0.04, 0.04), coatRoughness);
        float3 coatReflected = reflect(-v, coatNormal);
        float3 coatLookup = reflectionLookupDirection(
            input.worldPosition, coatReflected);
        float3 coatReflection = specularEnvironment.SampleLevel(
            materialSampler, coatLookup, coatRoughness * 9.0).rgb;
        color += clearCoatDirect * clearCoatTint.rgb * coatIntensity;
        color += coatReflection * coatFresnel * clearCoatTint.rgb
            * lerp(0.15, 0.95, 1.0 - coatRoughness) * coatIntensity;
        color += clearCoatTint.rgb
            * (pow(1.0 - coatNdotV, 4.0) * 0.06 * coatIntensity);
    }
    float glassThicknessWeight = 0.42;
    if (materialFamilyParameters.x == 3u) {
        float3 glassReflection = reflectedEnvironment
            * environmentBrdfApproximation(
                float3(0.04, 0.04, 0.04), roughness, ndotv);
        float3 glassTint = max(albedo, float3(0.001, 0.001, 0.001));
        // GlassColor is absorption/transmission tint, not an opaque diffuse
        // colour. Neutralize the strongly blue garage probe at normal angles
        // while retaining a brighter authored reflection at grazing angles.
        float reflectionLuminance = dot(
            glassReflection, float3(0.2126, 0.7152, 0.0722));
        float edgeReflection = pow(1.0 - ndotv, 5.0);
        float tintLuminance = dot(
            glassTint, float3(0.2126, 0.7152, 0.0722));
        // Retain the decoded GlassColor value, but treat its chroma as a weak
        // absorption bias. The previous direct teal bias made nominally dark
        // glass appear pale blue under the cool garage probe.
        float3 neutralGlassTint = lerp(
            tintLuminance.xxx, glassTint, 0.12);
        float3 neutralReflection = lerp(
            reflectionLuminance.xxx, glassReflection,
            lerp(0.08, 0.52, edgeReflection));
        float pathLength = glassPathLength(input, v);
        glassThicknessWeight = pathLength > 0.0001
            ? saturate(pathLength / 0.045) : 0.42;
        if (renderPassParameters.x != 0u) {
            color = neutralReflection * lerp(0.24, 0.68, edgeReflection);
        } else {
            float interiorScale = renderPassParameters.y != 0u ? 0.68 : 1.0;
            float3 absorption = (float3(0.0028, 0.0026, 0.0023)
                + neutralGlassTint * 0.010) * interiorScale
                * lerp(0.62, 1.38, glassThicknessWeight);
            color = absorption + neutralReflection
                * lerp(0.20, 0.56, edgeReflection);
        }
    }
    // Homespace's plane-mode car drop shadow is a separate floor composite.
    // Applying it after the floor's ambient/probe lighting prevents those terms
    // from washing the footprint away; focus-car self-shadow remains a direct-
    // light mask above.
    if (liveryParameters.w == 1u) {
        float floorShadow = dropShadowVisibility(input.worldPosition);
        color *= lerp(1.0, floorShadow, dropShadowParameters.y);
    }
    float3 emission = emissiveColorAndMap.rgb;
    if (emissiveColorAndMap.w > 0.5) {
        float3 authoredEmission = emissiveTexture.Sample(materialSampler, detailUv).rgb;
        emission += pow(max(authoredEmission, 0.0), 2.2);
    }
    color += emission;
    float outputAlpha = baseColor.a;
    if (liveryParameters.x == 0 && textureParameters.x > 0.5) {
        outputAlpha *= authoredBase.a;
    }
    float authoredAlpha = textureParameters.w > 0.5
        ? alphaTexture.Sample(materialSampler, detailUv).r : 1.0;
    if (textureParameters.w > 0.5
        && materialFamilyParameters.x != 3u) {
        outputAlpha *= authoredAlpha;
    }
    if (materialFamilyParameters.x == 3u) {
        float glassEdge = pow(1.0 - ndotv, 5.0);
        if (renderPassParameters.x != 0u) {
            // The separate windshield member is a reflection mask, not a
            // second full transmission layer.
            outputAlpha = saturate(
                outputAlpha * authoredAlpha
                * lerp(0.18, 0.48, glassEdge));
        } else {
            float transmissionAlpha = outputAlpha
                * lerp(0.82, 1.0, authoredAlpha)
                * lerp(0.94, 1.16, glassEdge)
                * lerp(0.90, 1.12, glassThicknessWeight);
            if (liveryParameters.w == 2u
                && renderPassParameters.y == 0u) {
                // Exterior car glass needs enough body to reproduce the dark
                // game tint even when its authored alpha map is mid-grey.
                transmissionAlpha = max(
                    transmissionAlpha,
                    lerp(0.62, 0.68, glassThicknessWeight));
            }
            outputAlpha = saturate(transmissionAlpha);
        }
    }
    if (outputAlpha < 0.02) {
        discard;
    }
    return float4(color, saturate(outputAlpha));
}
)hlsl");

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> diagnostics;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    HRESULT result = D3DCompile(
        shader.constData(), static_cast<SIZE_T>(shader.size()),
        "original_material_preview.hlsl",
        nullptr, nullptr, "VSMain", "vs_5_1", flags, 0,
        &vertexShader, &diagnostics);
    if (FAILED(result)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 material vertex shader compilation failed");
        return {};
    }
    diagnostics.Reset();
    result = D3DCompile(
        shader.constData(), static_cast<SIZE_T>(shader.size()),
        "original_material_preview.hlsl",
        nullptr, nullptr, "PSMain", "ps_5_1", flags, 0,
        &pixelShader, &diagnostics);
    if (FAILED(result)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 material pixel shader compilation failed");
        return {};
    }

    constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 5> inputLayout = {{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 48,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature;
    description.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    description.BlendState = blendDescription(translucent);
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizerDescription();
    description.DepthStencilState = depthDescription(!translucent);
    description.InputLayout = {inputLayout.data(), static_cast<UINT>(inputLayout.size())};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = targetFormat;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    result = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        *error = QStringLiteral("DX12 material pipeline creation failed: %1")
            .arg(hresultText(result));
        return {};
    }
    return pipeline;
}

ComPtr<ID3D12PipelineState> createShadowPipeline(
    ID3D12Device *device, ID3D12RootSignature *rootSignature,
    QString *error) {
    static constexpr const char *kShader = R"hlsl(
cbuffer ShadowData : register(b0, space3) {
    row_major float4x4 shadowViewProjection;
    float4 shadowParameters;
};

struct VertexInput {
    float3 position : POSITION;
};

float4 VSMain(VertexInput input) : SV_Position {
    return mul(shadowViewProjection, float4(input.position, 1.0));
}
)hlsl";
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> diagnostics;
    constexpr UINT kFlags =
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT compileResult = D3DCompile(
        kShader, std::strlen(kShader), "tokyo_car_shadow.hlsl", nullptr,
        nullptr, "VSMain", "vs_5_1", kFlags, 0, &vertexShader, &diagnostics);
    if (FAILED(compileResult)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 car-shadow vertex shader compilation failed");
        return {};
    }
    constexpr D3D12_INPUT_ELEMENT_DESC kInput = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature;
    description.VS = {
        vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.BlendState = blendDescription(false);
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizerDescription();
    description.DepthStencilState = depthDescription();
    description.InputLayout = {&kInput, 1};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 0;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        *error = QStringLiteral("DX12 car-shadow pipeline creation failed: %1")
            .arg(hresultText(result));
        return {};
    }

    return pipeline;
}

ComPtr<ID3D12PipelineState> createGlassBackDepthPipeline(
    ID3D12Device *device, ID3D12RootSignature *rootSignature,
    QString *error) {
    static constexpr const char *kShader = R"hlsl(
cbuffer FrameData : register(b0, space2) {
    row_major float4x4 viewProjection;
};
struct VertexInput {
    float3 position : POSITION;
};
float4 VSMain(VertexInput input) : SV_Position {
    return mul(viewProjection, float4(input.position, 1.0));
}
)hlsl";
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> diagnostics;
    constexpr UINT kFlags =
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT compileResult = D3DCompile(
        kShader, std::strlen(kShader), "tokyo_glass_back_depth.hlsl", nullptr,
        nullptr, "VSMain", "vs_5_1", kFlags, 0, &vertexShader, &diagnostics);
    if (FAILED(compileResult)) {
        *error = diagnostics != nullptr
            ? QString::fromUtf8(
                  static_cast<const char *>(diagnostics->GetBufferPointer()),
                  static_cast<qsizetype>(diagnostics->GetBufferSize())).trimmed()
            : QStringLiteral("DX12 glass-depth vertex shader compilation failed");
        return {};
    }
    constexpr D3D12_INPUT_ELEMENT_DESC kInput = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature;
    description.VS = {
        vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    description.BlendState = blendDescription(false);
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizerDescription();
    description.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    description.DepthStencilState = depthDescription();
    description.InputLayout = {&kInput, 1};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 0;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        *error = QStringLiteral("DX12 glass-depth pipeline creation failed: %1")
            .arg(hresultText(result));
        return {};
    }
    return pipeline;
}

void recordShadowPass(
    ID3D12GraphicsCommandList *commands, ID3D12RootSignature *rootSignature,
    ID3D12PipelineState *pipeline, ID3D12Resource *frameConstants,
    D3D12_CPU_DESCRIPTOR_HANDLE depthView,
    const std::vector<DrawResources> &draws, ShadowPassKind kind) {
    const D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(kShadowMapSize),
        static_cast<float>(kShadowMapSize), 0.0f, 1.0f};
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(kShadowMapSize),
        static_cast<LONG>(kShadowMapSize)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(0, nullptr, FALSE, &depthView);
    commands->ClearDepthStencilView(
        depthView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commands->SetGraphicsRootSignature(rootSignature);
    commands->SetGraphicsRootConstantBufferView(
        7, frameConstants->GetGPUVirtualAddress());
    commands->SetPipelineState(pipeline);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const DrawResources &draw : draws) {
        if (!castsShadow(draw, kind)) {
            continue;
        }
        commands->IASetVertexBuffers(0, 1, &draw.vertexView);
        commands->IASetIndexBuffer(&draw.indexView);
        commands->DrawIndexedInstanced(
            static_cast<UINT>(draw.geometry.indices.size()), 1, 0, 0, 0);
    }
}

void recordGlassBackDepthPass(
    ID3D12GraphicsCommandList *commands, ID3D12RootSignature *rootSignature,
    ID3D12PipelineState *pipeline, ID3D12Resource *frameConstants,
    D3D12_CPU_DESCRIPTOR_HANDLE depthView, const QSize &size,
    const std::vector<DrawResources> &draws) {
    commands->SetGraphicsRootSignature(rootSignature);
    commands->SetGraphicsRootConstantBufferView(
        5, frameConstants->GetGPUVirtualAddress());
    commands->SetPipelineState(pipeline);
    commands->OMSetRenderTargets(0, nullptr, FALSE, &depthView);
    commands->ClearDepthStencilView(
        depthView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    const D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(size.width()),
        static_cast<float>(size.height()), 0.0f, 1.0f};
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(size.width()), static_cast<LONG>(size.height())};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    constexpr quint32 kReflectionGroups =
        fh6::car_draw_groups::kWindshieldReflection
        | fh6::car_draw_groups::kWindshieldReflectionDriverless;
    for (const DrawResources &draw : draws) {
        if (!draw.visible || draw.shaderFamily != fh6::ModelShaderFamily::Glass
            || (draw.drawGroups & kReflectionGroups) != 0) {
            continue;
        }
        commands->IASetVertexBuffers(0, 1, &draw.vertexView);
        commands->IASetIndexBuffer(&draw.indexView);
        commands->DrawIndexedInstanced(
            static_cast<UINT>(draw.geometry.indices.size()), 1, 0, 0, 0);
    }
}

ComPtr<ID3D12PipelineState> createExactPipeline(
    ID3D12Device *device, ID3D12RootSignature *rootSignature,
    const fh6::OriginalShaderProgram &program, QString *error,
    DXGI_FORMAT targetFormat = kTargetFormat) {
    constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 5> inputLayout = {{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 48,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = rootSignature;
    description.VS = {
        program.vertexShader.constData(), static_cast<SIZE_T>(program.vertexShader.size())};
    description.PS = {
        program.pixelShader.constData(), static_cast<SIZE_T>(program.pixelShader.size())};
    description.BlendState = blendDescription();
    description.SampleMask = UINT_MAX;
    description.RasterizerState = rasterizerDescription();
    description.DepthStencilState = depthDescription();
    description.InputLayout = {
        inputLayout.data(), static_cast<UINT>(inputLayout.size())};
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = targetFormat;
    description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT result = device->CreateGraphicsPipelineState(
        &description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        *error = QStringLiteral("exact DXIL pipeline creation failed: %1").arg(
            hresultText(result));
        return {};
    }

    return pipeline;
}

} // namespace
#endif

OriginalDx12BackendStatus probeOriginalDx12Backend(
    const fh6::OriginalShaderGarageScene &scene) {
    OriginalDx12BackendStatus status;
    if (!scene.valid()) {
        status.error = scene.error.isEmpty()
            ? QStringLiteral("original-shader garage scene is incomplete")
            : scene.error;
        return status;
    }

#ifndef Q_OS_WIN
    status.error = QStringLiteral("the original-DXIL backend is Windows-only");
    return status;
#else
    ComPtr<IDXGIFactory6> factory;
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        status.error = QStringLiteral("DXGI factory creation failed: %1").arg(
            hresultText(result));
        return status;
    }

    ComPtr<ID3D12Device> device;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        result = D3D12CreateDevice(
            adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (SUCCEEDED(result)) {
            status.adapter = QString::fromWCharArray(description.Description);
            break;
        }
    }
    if (device == nullptr) {
        status.error = QStringLiteral("no hardware D3D12 device is available");
        return status;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
    result = device->CheckFeatureSupport(
        D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
    if (FAILED(result) || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        status.error = QStringLiteral("D3D12 Shader Model 6.6 is unavailable");
        return status;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    result = device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (FAILED(result) || options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
        status.error = QStringLiteral("D3D12 resource binding tier 3 is unavailable");
        return status;
    }

    ComPtr<ID3D12RootSignature> rootSignature =
        createRootSignature(device.Get(), &status.error);
    if (rootSignature == nullptr) {
        return status;
    }
    const ComPtr<ID3D12PipelineState> floorPipeline = createExactPipeline(
        device.Get(), rootSignature.Get(), scene.floorProgram, &status.error);
    const ComPtr<ID3D12PipelineState> defaultPipeline = createExactPipeline(
        device.Get(), rootSignature.Get(), scene.defaultProgram, &status.error);
    const ComPtr<ID3D12PipelineState> materialPipeline = createMaterialPipeline(
        device.Get(), rootSignature.Get(), &status.error);
    const ComPtr<ID3D12PipelineState> translucentMaterialPipeline =
        createMaterialPipeline(
            device.Get(), rootSignature.Get(), &status.error, kTargetFormat, true);
    const ComPtr<ID3D12PipelineState> panoramaPipeline = createPanoramaPipeline(
        device.Get(), rootSignature.Get(), &status.error);
    const ComPtr<ID3D12PipelineState> postPipeline = createPostPipeline(
        device.Get(), rootSignature.Get(), &status.error);
    const ComPtr<ID3D12PipelineState> glassBackDepthPipeline =
        createGlassBackDepthPipeline(
            device.Get(), rootSignature.Get(), &status.error);
    if (floorPipeline == nullptr || defaultPipeline == nullptr
        || materialPipeline == nullptr || translucentMaterialPipeline == nullptr
        || panoramaPipeline == nullptr || postPipeline == nullptr
        || glassBackDepthPipeline == nullptr) {
        return status;
    }

    status.available = true;
    status.exactPipelinesCreated = true;
    status.error.clear();
    return status;
#endif
}

OriginalDx12FrameResult renderOriginalDx12GarageFrame(
    const fh6::OriginalShaderGarageScene &scene, const QSize &size,
    const OriginalDx12Camera &camera) {
    OriginalDx12FrameResult frame;
    if (!scene.valid()) {
        frame.error = scene.error.isEmpty()
            ? QStringLiteral("original-shader garage scene is incomplete")
            : scene.error;
        return frame;
    }
    constexpr int kMaximumFrameDimension = 4096;
    if (size.isEmpty() || size.width() > kMaximumFrameDimension
        || size.height() > kMaximumFrameDimension) {
        frame.error = QStringLiteral("invalid original-DXIL frame size %1x%2")
            .arg(size.width()).arg(size.height());
        return frame;
    }
    if (!camera.valid()) {
        frame.error = QStringLiteral("invalid original-DXIL camera");
        return frame;
    }

#ifndef Q_OS_WIN
    frame.error = QStringLiteral("the original-DXIL backend is Windows-only");
    return frame;
#else
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory;
    HRESULT result = CreateDXGIFactory2(
        debug != nullptr ? DXGI_CREATE_FACTORY_DEBUG : 0,
        IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        frame.error = QStringLiteral("DXGI factory creation failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    const ComPtr<ID3D12Device> device =
        createHardwareDevice(factory.Get(), &frame.adapter);
    if (device == nullptr) {
        frame.error = QStringLiteral("no hardware D3D12 device is available");
        return frame;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
    result = device->CheckFeatureSupport(
        D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
    if (FAILED(result) || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        frame.error = QStringLiteral("D3D12 Shader Model 6.6 is unavailable");
        return frame;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    result = device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    if (FAILED(result)
        || options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
        frame.error = QStringLiteral("D3D12 resource binding tier 3 is unavailable");
        return frame;
    }
    ComPtr<ID3D12InfoQueue> infoQueue;
    device.As(&infoQueue);
    if (infoQueue != nullptr) {
        infoQueue->ClearStoredMessages();
    }
    QString setupError;
    const ComPtr<ID3D12RootSignature> rootSignature =
        createRootSignature(device.Get(), &setupError);
    const ComPtr<ID3D12PipelineState> floorPipeline = rootSignature != nullptr
        ? createExactPipeline(
              device.Get(), rootSignature.Get(), scene.floorProgram, &setupError)
        : ComPtr<ID3D12PipelineState>{};
    const ComPtr<ID3D12PipelineState> defaultPipeline = rootSignature != nullptr
        ? createExactPipeline(
              device.Get(), rootSignature.Get(), scene.defaultProgram, &setupError)
        : ComPtr<ID3D12PipelineState>{};
    const ComPtr<ID3D12PipelineState> materialPipeline = rootSignature != nullptr
        ? createMaterialPipeline(device.Get(), rootSignature.Get(), &setupError)
        : ComPtr<ID3D12PipelineState>{};
    const ComPtr<ID3D12PipelineState> translucentMaterialPipeline =
        rootSignature != nullptr
        ? createMaterialPipeline(
              device.Get(), rootSignature.Get(), &setupError, kTargetFormat, true)
        : ComPtr<ID3D12PipelineState>{};
    const ComPtr<ID3D12PipelineState> panoramaPipeline = rootSignature != nullptr
        ? createPanoramaPipeline(
              device.Get(), rootSignature.Get(), &setupError, kTargetFormat)
        : ComPtr<ID3D12PipelineState>{};
    const ComPtr<ID3D12PipelineState> postPipeline = rootSignature != nullptr
        ? createPostPipeline(
              device.Get(), rootSignature.Get(), &setupError, kTargetFormat)
        : ComPtr<ID3D12PipelineState>{};
    const ComPtr<ID3D12PipelineState> shadowPipeline = rootSignature != nullptr
        ? createShadowPipeline(device.Get(), rootSignature.Get(), &setupError)
        : ComPtr<ID3D12PipelineState>{};
    const ComPtr<ID3D12PipelineState> glassBackDepthPipeline =
        rootSignature != nullptr
        ? createGlassBackDepthPipeline(
              device.Get(), rootSignature.Get(), &setupError)
        : ComPtr<ID3D12PipelineState>{};
    if (rootSignature == nullptr || floorPipeline == nullptr
        || defaultPipeline == nullptr || materialPipeline == nullptr
        || translucentMaterialPipeline == nullptr || panoramaPipeline == nullptr
        || postPipeline == nullptr || shadowPipeline == nullptr
        || glassBackDepthPipeline == nullptr) {
        frame.error = setupError;
        return frame;
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    result = device->CreateCommandQueue(
        &queueDescription, IID_PPV_ARGS(&queue));
    if (FAILED(result)) {
        frame.error = QStringLiteral("D3D12 command queue creation failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    result = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) {
        frame.error = QStringLiteral("D3D12 command allocator creation failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    ComPtr<ID3D12GraphicsCommandList> commands;
    result = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), floorPipeline.Get(),
        IID_PPV_ARGS(&commands));
    if (FAILED(result)) {
        frame.error = QStringLiteral("D3D12 command list creation failed: %1")
            .arg(hresultText(result));
        return frame;
    }

    std::vector<DrawResources> draws;
    draws.reserve(scene.draws.size());
    const bool hasAuthoredDropShadow = std::any_of(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return !draw.hidden && draw.shadowCasterOnly;
        });
    for (const fh6::OriginalShaderGarageDraw &source : scene.draws) {
        if (source.hidden) {
            continue;
        }
        DrawResources draw;
        draw.geometry = prepareGeometry(
            source.geometry, source.placement, source.diffuseUvChannel,
            source.rawMaterialUv, source.materialUvChannel,
            source.materialUvRotationDegrees);
        draw.center = geometryCenter(draw.geometry);
        draw.family = source.family;
        draw.reflectionProbeShell = source.source.contains(
            QStringLiteral("garage_customiser"), Qt::CaseInsensitive)
            || source.source.contains(
                QStringLiteral("bld_gbl_grge_custom_02"), Qt::CaseInsensitive);
        draw.diffuseTexture = source.diffuseTexture;
        draw.alphaTexture = source.alphaTexture;
        draw.normalTexture = source.normalTexture;
        draw.weaveMaskTexture = source.weaveMaskTexture;
        draw.weaveNormalTexture = source.weaveNormalTexture;
        draw.clearCoatNormalTexture = source.clearCoatNormalTexture;
        draw.surfaceTexture = source.surfaceTexture;
        draw.tireHeightAoTexture = source.tireHeightAoTexture;
        draw.aoTexture = source.aoTexture;
        draw.emissiveTexture = source.emissiveTexture;
        draw.baseColor = source.baseColor;
        draw.secondaryPaintColor = source.secondaryPaintColor;
        draw.flakeColor = source.flakeColor;
        draw.emissiveColor = source.emissiveColor;
        draw.opacity = source.opacity;
        draw.gloss = source.gloss;
        draw.roughnessShift = source.roughnessShift;
        draw.metallic = source.metallic;
        draw.flakeCoverage = source.flakeCoverage;
        draw.flakeRoughness = source.flakeRoughness;
        draw.glitterIntensity = source.glitterIntensity;
        draw.glancingFlopStrength = source.glancingFlopStrength;
        draw.glancingFlopPower = source.glancingFlopPower;
        draw.glancingFlopEnabled = source.glancingFlopEnabled;
        draw.uTiling = source.uTiling;
        draw.vTiling = source.vTiling;
        draw.detailUTiling = source.detailUTiling;
        draw.detailVTiling = source.detailVTiling;
        draw.normalIntensity = source.normalIntensity;
        draw.weaveNormalIntensity = source.weaveNormalIntensity;
        draw.clearCoatNormalUTiling = source.clearCoatNormalUTiling;
        draw.clearCoatNormalVTiling = source.clearCoatNormalVTiling;
        draw.weaveColorTintA = source.weaveColorTintA;
        draw.weaveColorTintB = source.weaveColorTintB;
        draw.sampler = source.sampler;
        draw.shaderFamily = source.shaderFamily;
        draw.clearCoatTint = source.clearCoatTint;
        draw.clearCoatCoverage = source.clearCoatCoverage;
        draw.clearCoatRoughness = source.clearCoatRoughness;
        draw.clearCoatOnLivery = source.clearCoatOnLivery;
        draw.translucent = source.translucent;
        // ForzaTech separates the authored Shadow/LODS plane proxy from the
        // focus car's detailed receive-shadow mask. Opaque visible geometry
        // supplies part-on-part self-shadow; the proxy supplies only the floor
        // footprint, with detailed geometry as a fallback for models lacking it.
        const bool authoredShadowMember = source.drawGroups == 0
            || (source.drawGroups & fh6::car_draw_groups::kShadow) != 0;
        draw.selfShadowCaster = authoredShadowMember && !source.shadowCasterOnly
            && source.family == fh6::OriginalShaderSurfaceFamily::Car
            && !source.translucent;
        draw.dropShadowCaster = source.shadowCasterOnly
            || (!hasAuthoredDropShadow && draw.selfShadowCaster);
        draw.visible = !source.shadowCasterOnly;
        draw.liveryBaseTexture = source.liveryBaseTexture;
        draw.liveryAllowedSides = source.liveryAllowedSides;
        draw.drawGroups = source.drawGroups;
        draw.interiorWindshield = source.interiorWindshield;
        if (source.liveryBaseTexture && scene.liveryMapping.valid()) {
            draw.liverySideCount = scene.liveryMapping.sideCount;
            draw.liverySourceRegions = scene.liveryMapping.sourceRegions;
            draw.liveryPaintRegions = scene.liveryMapping.paintRegions;
            draw.liveryFacing = scene.liveryMapping.facing;
        }
        const UINT64 vertexBytes =
            static_cast<UINT64>(draw.geometry.vertices.size() * sizeof(Vertex));
        const UINT64 indexBytes = static_cast<UINT64>(
            draw.geometry.indices.size() * sizeof(std::uint32_t));
        draw.vertexBuffer = createBuffer(
            device.Get(), vertexBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        draw.indexBuffer = createBuffer(
            device.Get(), indexBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        if (draw.vertexBuffer == nullptr || draw.indexBuffer == nullptr
            || !uploadBuffer(
                draw.vertexBuffer.Get(), draw.geometry.vertices.data(),
                static_cast<std::size_t>(vertexBytes))
            || !uploadBuffer(
                draw.indexBuffer.Get(), draw.geometry.indices.data(),
                static_cast<std::size_t>(indexBytes))) {
            frame.error = QStringLiteral("original-DXIL geometry upload failed");
            return frame;
        }
        draw.vertexView = {
            draw.vertexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(vertexBytes), sizeof(Vertex)};
        draw.indexView = {
            draw.indexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(indexBytes), DXGI_FORMAT_R32_UINT};
        draw.materialConstants = createConstantBuffer(
            device.Get(), materialConstantData(draw));
        if (draw.materialConstants == nullptr) {
            frame.error = QStringLiteral("DX12 material constant upload failed");
            return frame;
        }
        draws.push_back(std::move(draw));
    }

    const ShadowProjection shadow = carShadowProjection(
        draws, scene.lighting.direction, ShadowPassKind::Self,
        kShadowStrength);
    const ShadowProjection dropShadow = carShadowProjection(
        draws, {0.0f, -1.0f, 0.0f}, ShadowPassKind::Drop,
        kDropShadowStrength);
    const std::array<ShadowProjection, 2> localShadows =
        localLightShadowProjections(draws, scene.authoredLights);
    const ReflectionProbeVolume reflectionProbe = reflectionProbeVolume(draws);
    frame.reflectionProbeActive = reflectionProbe.valid;
    TemporalFrameState temporal;
    temporal.previousViewProjection = cameraViewProjection(
        camera, static_cast<float>(size.width()) / size.height());
    frame.localShadowMapCount = static_cast<int>(std::count_if(
        localShadows.cbegin(), localShadows.cend(),
        [](const ShadowProjection &projection) { return projection.valid; }));
    const std::array<std::vector<std::uint8_t>, 8> constantData =
        shaderConstantData(
            camera, size, scene.lighting, scene.authoredLights,
            scene.environment.panorama, scene.colorLut, shadow, dropShadow,
            localShadows, reflectionProbe, temporal);
    std::array<ComPtr<ID3D12Resource>, 8> constantBuffers;
    for (std::size_t index = 0; index < constantBuffers.size(); ++index) {
        constantBuffers[index] =
            createConstantBuffer(device.Get(), constantData[index]);
        if (constantBuffers[index] == nullptr) {
            frame.error = QStringLiteral("original-DXIL constant upload failed");
            return frame;
        }
    }
    const ComPtr<ID3D12Resource> shadowConstants = createConstantBuffer(
        device.Get(), shadowConstantData(shadow));
    const ComPtr<ID3D12Resource> dropShadowConstants = createConstantBuffer(
        device.Get(), shadowConstantData(dropShadow));
    std::array<ComPtr<ID3D12Resource>, 2> localShadowConstants;
    for (std::size_t index = 0; index < localShadows.size(); ++index) {
        localShadowConstants[index] = createConstantBuffer(
            device.Get(), shadowConstantData(localShadows[index]));
    }
    if (shadowConstants == nullptr || dropShadowConstants == nullptr
        || localShadowConstants[0] == nullptr
        || localShadowConstants[1] == nullptr) {
        frame.error = QStringLiteral("DX12 shadow constant upload failed");
        return frame;
    }

    D3D12_DESCRIPTOR_HEAP_DESC shaderHeapDescription{};
    shaderHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    const UINT descriptorCount = kFixedDescriptorCount
        + static_cast<UINT>(draws.size()) * kMaterialDescriptorCount;
    shaderHeapDescription.NumDescriptors = descriptorCount;
    shaderHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> shaderHeap;
    result = device->CreateDescriptorHeap(
        &shaderHeapDescription, IID_PPV_ARGS(&shaderHeap));
    if (FAILED(result)) {
        frame.error = QStringLiteral("original-DXIL descriptor heap creation failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    const UINT shaderDescriptorStride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    createNullDescriptors(
        device.Get(), shaderHeap.Get(), shaderDescriptorStride, descriptorCount);
    auto cpuHandleAt = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            shaderHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * shaderDescriptorStride;
        return handle;
    };
    auto gpuHandleAt = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            shaderHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * shaderDescriptorStride;
        return handle;
    };
    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDescription{};
    samplerHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDescription.NumDescriptors = std::max(1u, static_cast<UINT>(draws.size()));
    samplerHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;
    result = device->CreateDescriptorHeap(
        &samplerHeapDescription, IID_PPV_ARGS(&samplerHeap));
    if (FAILED(result)) {
        frame.error = QStringLiteral("DX12 material sampler heap creation failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    const UINT samplerDescriptorStride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    const fh6::ModelMaterialSampler defaultSampler;
    for (UINT index = 0; index < samplerHeapDescription.NumDescriptors; ++index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            samplerHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * samplerDescriptorStride;
        const D3D12_SAMPLER_DESC description = materialSamplerDescription(
            index < draws.size() ? draws[index].sampler : defaultSampler);
        device->CreateSampler(&description, handle);
        if (index < draws.size()) {
            draws[index].samplerDescriptorIndex = index;
        }
    }
    const auto samplerGpuHandleAt = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            samplerHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * samplerDescriptorStride;
        return handle;
    };

    std::vector<UploadedTexture> materialTextures;
    materialTextures.reserve(scene.materialTextures.size());
    for (std::size_t index = 0; index < scene.materialTextures.size(); ++index) {
        UploadedTexture texture = uploadRgba8Texture(
            device.Get(), commands.Get(), scene.materialTextures[index].image,
            true, false, &scene.materialTextures[index].authoredMips);
        if (texture.texture == nullptr) {
            frame.error = QStringLiteral("original-DXIL material texture upload failed");
            return frame;
        }
        const D3D12_SHADER_RESOURCE_VIEW_DESC view =
            texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM, texture.mipLevels);
        device->CreateShaderResourceView(
            texture.texture.Get(), &view,
            cpuHandleAt(kMaterialDescriptorStart + static_cast<UINT>(index)));
        materialTextures.push_back(std::move(texture));
    }
    std::vector<UploadedTexture> authoredMaterialTextures;
    authoredMaterialTextures.reserve(draws.size() * 10);
    std::unordered_map<const fh6::OriginalShaderMaterialTexture *, std::size_t>
        authoredTextureIndices;
    std::array<UploadedTexture, fh6::kLiverySideCount> liveryMaskTextures;
    if (scene.liveryMapping.valid()) {
        for (int side = 0; side < fh6::kLiverySideCount; ++side) {
            if (!scene.liveryMapping.masks[side].valid()) {
                continue;
            }
            liveryMaskTextures[side] = uploadRgba8Texture(
                device.Get(), commands.Get(),
                scene.liveryMapping.masks[side].image, false);
            if (liveryMaskTextures[side].texture == nullptr) {
                frame.error = QStringLiteral("DX12 livery mask upload failed");
                return frame;
            }
        }
    }
    for (std::size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex) {
        DrawResources &draw = draws[drawIndex];
        draw.materialDescriptorStart = kFixedDescriptorCount
            + static_cast<UINT>(drawIndex) * kMaterialDescriptorCount;
        const D3D12_SHADER_RESOURCE_VIEW_DESC fallbackView =
            texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
        constexpr std::array<std::size_t, 4> fallbackIndices = {1, 6, 0, 4};
        for (std::size_t index = 0; index < fallbackIndices.size(); ++index) {
            device->CreateShaderResourceView(
                materialTextures[fallbackIndices[index]].texture.Get(), &fallbackView,
                cpuHandleAt(
                    draw.materialDescriptorStart + static_cast<UINT>(index)));
        }
        device->CreateShaderResourceView(
            materialTextures[1].texture.Get(), &fallbackView,
            cpuHandleAt(draw.materialDescriptorStart + 15u));
        const std::array<std::pair<
            std::shared_ptr<const fh6::OriginalShaderMaterialTexture>, UINT>, 10>
            sources = {{{draw.diffuseTexture, 0u}, {draw.normalTexture, 1u},
                        {draw.surfaceTexture, 2u}, {draw.emissiveTexture, 3u},
                        {draw.alphaTexture, 15u}, {draw.weaveMaskTexture, 16u},
                        {draw.weaveNormalTexture, 17u},
                        {draw.clearCoatNormalTexture, 18u},
                        {draw.tireHeightAoTexture, 19u},
                        {draw.aoTexture, 20u}}};
        for (const auto &[source, slot] : sources) {
            if (source == nullptr || !source->valid()) {
                continue;
            }
            const auto [found, inserted] = authoredTextureIndices.emplace(
                source.get(), authoredMaterialTextures.size());
            if (inserted) {
                const bool liveLivery = source->sourceEntry.startsWith(
                    QStringLiteral("car://composited-livery"));
                UploadedTexture texture = uploadRgba8Texture(
                    device.Get(), commands.Get(), source->image,
                    !liveLivery, slot == 1u || slot == 17u || slot == 18u,
                    &source->authoredMips);
                if (texture.texture == nullptr) {
                    frame.error = QStringLiteral(
                        "DX12 authored material texture upload failed");
                    return frame;
                }
                authoredMaterialTextures.push_back(std::move(texture));
            }
            const D3D12_SHADER_RESOURCE_VIEW_DESC view =
                texture2DView(
                    DXGI_FORMAT_R8G8B8A8_UNORM,
                    authoredMaterialTextures[found->second].mipLevels);
            device->CreateShaderResourceView(
                authoredMaterialTextures[found->second].texture.Get(), &view,
                cpuHandleAt(draw.materialDescriptorStart + static_cast<UINT>(slot)));
        }
        if (draw.liveryBaseTexture && scene.liveryMapping.valid()) {
            for (int side = 0; side < fh6::kLiverySideCount; ++side) {
                if (liveryMaskTextures[side].texture == nullptr) {
                    continue;
                }
                device->CreateShaderResourceView(
                    liveryMaskTextures[side].texture.Get(), &fallbackView,
                    cpuHandleAt(draw.materialDescriptorStart + 4u
                                + static_cast<UINT>(side)));
            }
        }
    }
    UploadedTexture diffuseCube = uploadCubeTexture(
        device.Get(), commands.Get(), scene.environment.diffuseCubemap,
        fh6::SwatchEncoding::R16G16B16A16Float,
        DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (diffuseCube.texture == nullptr) {
        frame.error = QStringLiteral("original-DXIL cubemap upload failed");
        return frame;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC diffuseView{};
    diffuseView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    diffuseView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    diffuseView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    diffuseView.TextureCube.MipLevels =
        diffuseCube.texture->GetDesc().MipLevels;
    device->CreateShaderResourceView(
        diffuseCube.texture.Get(), &diffuseView, cpuHandleAt(0));
    UploadedTexture specularCube = uploadCubeTexture(
        device.Get(), commands.Get(), scene.environment.specularCubemap,
        fh6::SwatchEncoding::UnsignedBc6H, DXGI_FORMAT_BC6H_UF16);
    if (specularCube.texture == nullptr) {
        frame.error = QStringLiteral("Tokyo reflection-probe upload failed");
        return frame;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC specularView{};
    specularView.Format = DXGI_FORMAT_BC6H_UF16;
    specularView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    specularView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    specularView.TextureCube.MipLevels = specularCube.texture->GetDesc().MipLevels;
    device->CreateShaderResourceView(
        specularCube.texture.Get(), &specularView, cpuHandleAt(1));
    UploadedTexture panoramaTexture = uploadBc6Panorama(
        device.Get(), commands.Get(), scene.environment.panorama.texture);
    if (panoramaTexture.texture == nullptr) {
        frame.error = QStringLiteral("Tokyo staged-space panorama upload failed");
        return frame;
    }
    const D3D12_SHADER_RESOURCE_VIEW_DESC panoramaView =
        texture2DView(DXGI_FORMAT_BC6H_UF16);
    device->CreateShaderResourceView(
        panoramaTexture.texture.Get(), &panoramaView, cpuHandleAt(120));
    UploadedTexture colorLutTexture = uploadColorLut(
        device.Get(), commands.Get(), scene.colorLut);
    if (colorLutTexture.texture == nullptr) {
        frame.error = QStringLiteral("Homespace colour-grade upload failed");
        return frame;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC colorLutView{};
    colorLutView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    colorLutView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    colorLutView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorLutView.Texture3D.MipLevels = 1;
    device->CreateShaderResourceView(
        colorLutTexture.texture.Get(), &colorLutView, cpuHandleAt(123));

    const ComPtr<ID3D12Resource> target = createTarget(device.Get(), size);
    const ComPtr<ID3D12Resource> finalTarget = createTarget(device.Get(), size);
    std::array<ComPtr<ID3D12Resource>, 2> temporalHistoryTargets = {
        createTarget(device.Get(), size), createTarget(device.Get(), size)};
    const ComPtr<ID3D12Resource> depthTarget =
        createDepthTarget(device.Get(), size);
    const ComPtr<ID3D12Resource> glassBackDepthTarget =
        createDepthTarget(device.Get(), size);
    const ComPtr<ID3D12Resource> shadowTarget =
        createShadowDepthTarget(device.Get());
    const ComPtr<ID3D12Resource> dropShadowTarget =
        createShadowDepthTarget(device.Get());
    std::array<ComPtr<ID3D12Resource>, 2> localShadowTargets = {
        createShadowDepthTarget(device.Get()),
        createShadowDepthTarget(device.Get())};
    D3D12_DESCRIPTOR_HEAP_DESC targetHeapDescription{};
    targetHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    targetHeapDescription.NumDescriptors = 4;
    ComPtr<ID3D12DescriptorHeap> targetHeap;
    D3D12_DESCRIPTOR_HEAP_DESC depthHeapDescription{};
    depthHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    depthHeapDescription.NumDescriptors = 6;
    ComPtr<ID3D12DescriptorHeap> depthHeap;
    if (target == nullptr || finalTarget == nullptr || depthTarget == nullptr
        || glassBackDepthTarget == nullptr
        || temporalHistoryTargets[0] == nullptr
        || temporalHistoryTargets[1] == nullptr
        || shadowTarget == nullptr
        || dropShadowTarget == nullptr
        || localShadowTargets[0] == nullptr
        || localShadowTargets[1] == nullptr
        || FAILED(device->CreateDescriptorHeap(
            &targetHeapDescription, IID_PPV_ARGS(&targetHeap)))
        || FAILED(device->CreateDescriptorHeap(
            &depthHeapDescription, IID_PPV_ARGS(&depthHeap)))) {
        frame.error = QStringLiteral("original-DXIL render target creation failed");
        return frame;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE targetView =
        targetHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, targetView);
    D3D12_CPU_DESCRIPTOR_HANDLE finalTargetView = targetView;
    finalTargetView.ptr += device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(
        finalTarget.Get(), nullptr, finalTargetView);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> temporalHistoryViews;
    for (std::size_t index = 0; index < temporalHistoryTargets.size(); ++index) {
        temporalHistoryViews[index] = finalTargetView;
        temporalHistoryViews[index].ptr += (index + 1)
            * device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        device->CreateRenderTargetView(
            temporalHistoryTargets[index].Get(), nullptr,
            temporalHistoryViews[index]);
        const D3D12_SHADER_RESOURCE_VIEW_DESC historyView =
            texture2DView(kTargetFormat);
        device->CreateShaderResourceView(
            temporalHistoryTargets[index].Get(), &historyView,
            cpuHandleAt(kTemporalHistoryDescriptorIndices[index]));
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE depthView =
        depthHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
    depthViewDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthViewDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(
        depthTarget.Get(), &depthViewDescription, depthView);
    createSceneDepthView(
        device.Get(), depthTarget.Get(),
        cpuHandleAt(kSceneDepthDescriptorIndex));
    const D3D12_SHADER_RESOURCE_VIEW_DESC hdrSceneView =
        texture2DView(kTargetFormat);
    device->CreateShaderResourceView(
        target.Get(), &hdrSceneView, cpuHandleAt(kHdrSceneDescriptorIndex));
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDepthView = depthView;
    shadowDepthView.ptr += device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    createShadowViews(
        device.Get(), shadowTarget.Get(), shadowDepthView,
        cpuHandleAt(kShadowDescriptorIndex));
    D3D12_CPU_DESCRIPTOR_HANDLE dropShadowDepthView = shadowDepthView;
    dropShadowDepthView.ptr += device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    createShadowViews(
        device.Get(), dropShadowTarget.Get(), dropShadowDepthView,
        cpuHandleAt(kDropShadowDescriptorIndex));
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> localShadowDepthViews;
    for (std::size_t index = 0; index < localShadowTargets.size(); ++index) {
        localShadowDepthViews[index] = dropShadowDepthView;
        localShadowDepthViews[index].ptr += (index + 1)
            * device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        createShadowViews(
            device.Get(), localShadowTargets[index].Get(),
            localShadowDepthViews[index],
            cpuHandleAt(kLocalShadowDescriptorIndices[index]));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE glassBackDepthView = dropShadowDepthView;
    glassBackDepthView.ptr += 3
        * device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    device->CreateDepthStencilView(
        glassBackDepthTarget.Get(), &depthViewDescription, glassBackDepthView);
    createSceneDepthView(
        device.Get(), glassBackDepthTarget.Get(),
        cpuHandleAt(kGlassBackDepthDescriptorIndex));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 readbackBytes = 0;
    const D3D12_RESOURCE_DESC targetDescription = finalTarget->GetDesc();
    device->GetCopyableFootprints(
        &targetDescription, 0, 1, 0, &footprint, &rows, &rowBytes,
        &readbackBytes);
    const ComPtr<ID3D12Resource> readback = createBuffer(
        device.Get(), readbackBytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shadowFootprint{};
    UINT shadowRows = 0;
    UINT64 shadowRowBytes = 0;
    UINT64 shadowReadbackBytes = 0;
    const D3D12_RESOURCE_DESC shadowDescription = shadowTarget->GetDesc();
    device->GetCopyableFootprints(
        &shadowDescription, 0, 1, 0, &shadowFootprint, &shadowRows,
        &shadowRowBytes, &shadowReadbackBytes);
    const ComPtr<ID3D12Resource> shadowReadback = createBuffer(
        device.Get(), shadowReadbackBytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (readback == nullptr || shadowReadback == nullptr) {
        frame.error = QStringLiteral("original-DXIL readback creation failed");
        return frame;
    }

    recordShadowPass(
        commands.Get(), rootSignature.Get(), shadowPipeline.Get(),
        shadowConstants.Get(), shadowDepthView, draws, ShadowPassKind::Self);
    const D3D12_RESOURCE_BARRIER shadowCopyBarrier = transition(
        shadowTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->ResourceBarrier(1, &shadowCopyBarrier);
    D3D12_TEXTURE_COPY_LOCATION shadowDestination{};
    shadowDestination.pResource = shadowReadback.Get();
    shadowDestination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    shadowDestination.PlacedFootprint = shadowFootprint;
    D3D12_TEXTURE_COPY_LOCATION shadowSource{};
    shadowSource.pResource = shadowTarget.Get();
    shadowSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(
        &shadowDestination, 0, 0, 0, &shadowSource, nullptr);
    const D3D12_RESOURCE_BARRIER shadowSampleBarrier = transition(
        shadowTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &shadowSampleBarrier);
    recordShadowPass(
        commands.Get(), rootSignature.Get(), shadowPipeline.Get(),
        dropShadowConstants.Get(), dropShadowDepthView, draws,
        ShadowPassKind::Drop);
    const D3D12_RESOURCE_BARRIER dropShadowSampleBarrier = transition(
        dropShadowTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &dropShadowSampleBarrier);
    for (std::size_t index = 0; index < localShadows.size(); ++index) {
        if (localShadows[index].valid) {
            recordShadowPass(
                commands.Get(), rootSignature.Get(), shadowPipeline.Get(),
                localShadowConstants[index].Get(), localShadowDepthViews[index],
                draws, ShadowPassKind::Self);
        }
        const D3D12_RESOURCE_BARRIER localReadBarrier = transition(
            localShadowTargets[index].Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &localReadBarrier);
    }
    recordGlassBackDepthPass(
        commands.Get(), rootSignature.Get(), glassBackDepthPipeline.Get(),
        constantBuffers[5].Get(), glassBackDepthView, size, draws);
    const D3D12_RESOURCE_BARRIER glassDepthReadBarrier = transition(
        glassBackDepthTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &glassDepthReadBarrier);

    const D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(size.width()),
        static_cast<float>(size.height()), 0.0f, 1.0f};
    const D3D12_RECT scissor{
        0, 0, static_cast<LONG>(size.width()),
        static_cast<LONG>(size.height())};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->OMSetRenderTargets(1, &targetView, FALSE, &depthView);
    commands->ClearRenderTargetView(
        targetView, kClearColor.data(), 0, nullptr);
    commands->ClearDepthStencilView(
        depthView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commands->SetGraphicsRootSignature(rootSignature.Get());
    ID3D12DescriptorHeap *heaps[] = {shaderHeap.Get(), samplerHeap.Get()};
    commands->SetDescriptorHeaps(2, heaps);
    for (UINT index = 0; index < constantBuffers.size(); ++index) {
        commands->SetGraphicsRootConstantBufferView(
            index, constantBuffers[index]->GetGPUVirtualAddress());
    }
    commands->SetGraphicsRootDescriptorTable(8, gpuHandleAt(0));
    commands->SetGraphicsRootDescriptorTable(10, gpuHandleAt(140));
    commands->SetGraphicsRootDescriptorTable(11, gpuHandleAt(156));
    commands->SetGraphicsRootDescriptorTable(12, samplerGpuHandleAt(0));
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->SetPipelineState(panoramaPipeline.Get());
    commands->DrawInstanced(3, 1, 0, 0);
    for (const bool translucentPass : {false, true}) {
        commands->SetPipelineState(
            translucentPass ? translucentMaterialPipeline.Get()
                            : materialPipeline.Get());
        for (const DrawResources *drawPointer
             : drawPassOrder(draws, translucentPass, camera.position)) {
            const DrawResources &draw = *drawPointer;
            commands->SetGraphicsRootDescriptorTable(
                9, gpuHandleAt(draw.materialDescriptorStart));
            commands->SetGraphicsRootDescriptorTable(
                12, samplerGpuHandleAt(draw.samplerDescriptorIndex));
            commands->SetGraphicsRootConstantBufferView(
                7, draw.materialConstants->GetGPUVirtualAddress());
            commands->IASetVertexBuffers(0, 1, &draw.vertexView);
            commands->IASetIndexBuffer(&draw.indexView);
            commands->DrawIndexedInstanced(
                static_cast<UINT>(draw.geometry.indices.size()), 1, 0, 0, 0);
        }
    }
    const std::array<D3D12_RESOURCE_BARRIER, 2> postReadBarriers = {{
        transition(
            target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        transition(
            depthTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    }};
    commands->ResourceBarrier(
        static_cast<UINT>(postReadBarriers.size()), postReadBarriers.data());
    const D3D12_RESOURCE_BARRIER previousHistoryReadBarrier = transition(
        temporalHistoryTargets[1].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &previousHistoryReadBarrier);
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> postTargets = {
        finalTargetView, temporalHistoryViews[0]};
    commands->OMSetRenderTargets(
        static_cast<UINT>(postTargets.size()), postTargets.data(), FALSE,
        nullptr);
    commands->SetPipelineState(postPipeline.Get());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER targetBarrier = transition(
        finalTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->ResourceBarrier(1, &targetBarrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = finalTarget.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    result = commands->Close();
    if (FAILED(result)) {
        frame.error = QStringLiteral("original-DXIL command recording failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    ID3D12CommandList *commandLists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, commandLists);
    ComPtr<ID3D12Fence> fence;
    result = device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result)) {
        frame.error = QStringLiteral("original-DXIL fence creation failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    const HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (completionEvent == nullptr) {
        frame.error = QStringLiteral("original-DXIL completion event creation failed");
        return frame;
    }
    result = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(result)) {
        result = fence->SetEventOnCompletion(1, completionEvent);
    }
    if (FAILED(result)) {
        CloseHandle(completionEvent);
        frame.error = QStringLiteral("original-DXIL GPU synchronization failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    WaitForSingleObject(completionEvent, INFINITE);
    CloseHandle(completionEvent);

    void *mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(readbackBytes)};
    result = readback->Map(0, &readRange, &mapped);
    if (FAILED(result)) {
        frame.error = QStringLiteral("original-DXIL readback mapping failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    frame.image = QImage(size, QImage::Format_RGBA8888);
    frame.finiteMinimum = std::numeric_limits<float>::max();
    frame.finiteMaximum = std::numeric_limits<float>::lowest();
    for (int y = 0; y < size.height(); ++y) {
        const float *sourceRow = reinterpret_cast<const float *>(
            static_cast<const std::uint8_t *>(mapped)
            + static_cast<UINT64>(y) * footprint.Footprint.RowPitch);
        auto *destinationRow = frame.image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const float *pixel = sourceRow + x * 4;
            bool differs = false;
            for (int component = 0; component < 4; ++component) {
                if (!std::isfinite(pixel[component])) {
                    ++frame.nonFiniteComponents;
                } else {
                    frame.finiteMinimum =
                        std::min(frame.finiteMinimum, pixel[component]);
                    frame.finiteMaximum =
                        std::max(frame.finiteMaximum, pixel[component]);
                }
                differs = differs
                    || std::abs(pixel[component] - kClearColor[component]) > 1e-5f;
            }
            frame.changedPixels += differs ? 1 : 0;
            for (int component = 0; component < 3; ++component) {
                float value = std::isfinite(pixel[component])
                    ? pixel[component] : 0.0f;
                value = std::pow(
                    std::clamp(value, 0.0f, 1.0f), 1.0f / 2.2f);
                destinationRow[x * 4 + component] =
                    static_cast<uchar>(value * 255.0f + 0.5f);
            }
            destinationRow[x * 4 + 3] = 255;
        }
    }
    readback->Unmap(0, nullptr);
    mapped = nullptr;
    const D3D12_RANGE shadowReadRange{
        0, static_cast<SIZE_T>(shadowReadbackBytes)};
    result = shadowReadback->Map(0, &shadowReadRange, &mapped);
    if (FAILED(result)) {
        frame.error = QStringLiteral("DX12 shadow readback mapping failed: %1")
            .arg(hresultText(result));
        return frame;
    }
    for (UINT y = 0; y < kShadowMapSize; ++y) {
        const float *sourceRow = reinterpret_cast<const float *>(
            static_cast<const std::uint8_t *>(mapped)
            + static_cast<UINT64>(y) * shadowFootprint.Footprint.RowPitch);
        for (UINT x = 0; x < kShadowMapSize; ++x) {
            frame.shadowMapPixels += sourceRow[x] < 1.0f ? 1u : 0u;
        }
    }
    shadowReadback->Unmap(0, nullptr);
    if (infoQueue != nullptr) {
        frame.debugErrors = countMessages(
            infoQueue.Get(), D3D12_MESSAGE_SEVERITY_ERROR);
        frame.debugWarnings = countMessages(
            infoQueue.Get(), D3D12_MESSAGE_SEVERITY_WARNING);
        if (frame.debugWarnings > 0) {
            frame.debugWarningDetail = firstMessage(
                infoQueue.Get(), D3D12_MESSAGE_SEVERITY_WARNING);
        }
    }
    if (frame.nonFiniteComponents > 0) {
        frame.error = QStringLiteral("original-DXIL frame contains non-finite values");
    } else if (frame.debugErrors > 0) {
        frame.error = QStringLiteral("original-DXIL debug layer reported errors: %1")
            .arg(firstMessage(infoQueue.Get(), D3D12_MESSAGE_SEVERITY_ERROR));
    } else if (frame.changedPixels == 0) {
        frame.error = QStringLiteral("original-DXIL frame contains only the clear color");
    }

    return frame;
#endif
}

OriginalDx12FrameResult renderOriginalDx12GarageFrame(
    const fh6::OriginalShaderGarageScene &scene, const QSize &size) {
    return renderOriginalDx12GarageFrame(
        scene, size, originalDx12SceneCamera(scene));
}

#ifdef Q_OS_WIN
struct OriginalDx12ViewportRenderer::Impl {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> infoQueue;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> floorPipeline;
    ComPtr<ID3D12PipelineState> defaultPipeline;
    ComPtr<ID3D12PipelineState> materialPipeline;
    ComPtr<ID3D12PipelineState> translucentMaterialPipeline;
    ComPtr<ID3D12PipelineState> panoramaPipeline;
    ComPtr<ID3D12PipelineState> postPipeline;
    ComPtr<ID3D12PipelineState> shadowPipeline;
    ComPtr<ID3D12PipelineState> glassBackDepthPipeline;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12CommandAllocator> liveryAllocator;
    ComPtr<ID3D12GraphicsCommandList> liveryCommands;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> shaderHeap;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;
    ComPtr<ID3D12DescriptorHeap> targetHeap;
    ComPtr<ID3D12DescriptorHeap> depthHeap;
    ComPtr<ID3D12Resource> depthTarget;
    ComPtr<ID3D12Resource> sceneTarget;
    ComPtr<ID3D12Resource> glassBackDepthTarget;
    std::array<ComPtr<ID3D12Resource>, 2> temporalHistoryTargets;
    ComPtr<ID3D12Resource> shadowTarget;
    ComPtr<ID3D12Resource> dropShadowTarget;
    std::array<ComPtr<ID3D12Resource>, 2> localShadowTargets;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12Resource>, 2> backBuffers;
    std::array<ComPtr<ID3D12Resource>, 8> constantBuffers;
    ComPtr<ID3D12Resource> shadowConstants;
    ComPtr<ID3D12Resource> dropShadowConstants;
    std::array<ComPtr<ID3D12Resource>, 2> localShadowConstants;
    std::vector<DrawResources> draws;
    std::vector<UploadedTexture> materialTextures;
    std::vector<UploadedTexture> authoredMaterialTextures;
    std::array<UploadedTexture, fh6::kLiverySideCount> liveryMaskTextures;
    UploadedTexture liveLiveryTexture;
    std::vector<UINT> liveryDescriptorIndices;
    UploadedTexture diffuseCube;
    UploadedTexture specularCube;
    UploadedTexture panoramaTexture;
    UploadedTexture colorLutTexture;
    fh6::OriginalShaderLighting lighting;
    std::vector<fh6::OriginalShaderPointLight> authoredLights;
    fh6::GaragePanoramaResources panorama;
    fh6::GarageColorLut colorLut;
    ShadowProjection shadow;
    ShadowProjection dropShadow;
    std::array<ShadowProjection, 2> localShadows;
    ReflectionProbeVolume reflectionProbe;
    Matrix4 previousViewProjection{};
    QString adapterName;
    QString failure;
    HANDLE completionEvent = nullptr;
    QSize viewportSize;
    UINT shaderDescriptorStride = 0;
    UINT samplerDescriptorStride = 0;
    UINT targetDescriptorStride = 0;
    UINT depthDescriptorStride = 0;
    UINT64 fenceValue = 0;
    bool liveryCommandsInFlight = false;
    bool sceneTargetsReadable = false;
    bool temporalHistoryValid = false;
    std::array<bool, 2> temporalHistoryReadable = {false, false};
    int temporalWriteIndex = 0;
    quint64 temporalFrameIndex = 0;

    ~Impl() {
        waitForGpu();
        if (completionEvent != nullptr) {
            CloseHandle(completionEvent);
        }
    }

    bool waitForGpu() {
        if (queue == nullptr || fence == nullptr || completionEvent == nullptr) {
            return true;
        }
        const UINT64 value = ++fenceValue;
        HRESULT result = queue->Signal(fence.Get(), value);
        if (SUCCEEDED(result) && fence->GetCompletedValue() < value) {
            result = fence->SetEventOnCompletion(value, completionEvent);
            if (SUCCEEDED(result)) {
                WaitForSingleObject(completionEvent, INFINITE);
            }
        }
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport synchronization failed: %1")
                .arg(hresultText(result));
            return false;
        }
        liveryCommandsInFlight = false;

        return true;
    }

    bool createTargets() {
        for (std::size_t index = 0; index < backBuffers.size(); ++index) {
            HRESULT result = swapChain->GetBuffer(
                static_cast<UINT>(index), IID_PPV_ARGS(&backBuffers[index]));
            if (FAILED(result)) {
                failure = QStringLiteral("D3D12 swap-chain buffer access failed: %1")
                    .arg(hresultText(result));
                return false;
            }
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                targetHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * targetDescriptorStride;
            device->CreateRenderTargetView(backBuffers[index].Get(), &view, handle);
        }
        depthTarget = createDepthTarget(device.Get(), viewportSize);
        sceneTarget = createTarget(device.Get(), viewportSize);
        glassBackDepthTarget = createDepthTarget(device.Get(), viewportSize);
        for (ComPtr<ID3D12Resource> &history : temporalHistoryTargets) {
            history = createTarget(device.Get(), viewportSize);
        }
        if (depthTarget == nullptr || sceneTarget == nullptr
            || glassBackDepthTarget == nullptr
            || temporalHistoryTargets[0] == nullptr
            || temporalHistoryTargets[1] == nullptr) {
            failure = QStringLiteral("D3D12 viewport HDR/depth target creation failed");
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(
            depthTarget.Get(), &view,
            depthHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE sceneView =
            targetHeap->GetCPUDescriptorHandleForHeapStart();
        sceneView.ptr += static_cast<SIZE_T>(backBuffers.size())
            * targetDescriptorStride;
        device->CreateRenderTargetView(sceneTarget.Get(), nullptr, sceneView);
        for (std::size_t index = 0; index < temporalHistoryTargets.size(); ++index) {
            D3D12_CPU_DESCRIPTOR_HANDLE historyTargetView = sceneView;
            historyTargetView.ptr += (index + 1) * targetDescriptorStride;
            device->CreateRenderTargetView(
                temporalHistoryTargets[index].Get(), nullptr,
                historyTargetView);
            D3D12_CPU_DESCRIPTOR_HANDLE historyShaderView =
                shaderHeap->GetCPUDescriptorHandleForHeapStart();
            historyShaderView.ptr += static_cast<SIZE_T>(
                kTemporalHistoryDescriptorIndices[index])
                * shaderDescriptorStride;
            const D3D12_SHADER_RESOURCE_VIEW_DESC historyView =
                texture2DView(kTargetFormat);
            device->CreateShaderResourceView(
                temporalHistoryTargets[index].Get(), &historyView,
                historyShaderView);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE depthShaderView =
            shaderHeap->GetCPUDescriptorHandleForHeapStart();
        depthShaderView.ptr += static_cast<SIZE_T>(kSceneDepthDescriptorIndex)
            * shaderDescriptorStride;
        createSceneDepthView(
            device.Get(), depthTarget.Get(), depthShaderView);
        D3D12_CPU_DESCRIPTOR_HANDLE glassDepthView =
            depthHeap->GetCPUDescriptorHandleForHeapStart();
        glassDepthView.ptr += 5 * depthDescriptorStride;
        device->CreateDepthStencilView(
            glassBackDepthTarget.Get(), &view, glassDepthView);
        D3D12_CPU_DESCRIPTOR_HANDLE glassDepthShaderView =
            shaderHeap->GetCPUDescriptorHandleForHeapStart();
        glassDepthShaderView.ptr +=
            static_cast<SIZE_T>(kGlassBackDepthDescriptorIndex)
            * shaderDescriptorStride;
        createSceneDepthView(
            device.Get(), glassBackDepthTarget.Get(), glassDepthShaderView);
        D3D12_CPU_DESCRIPTOR_HANDLE sceneShaderView =
            shaderHeap->GetCPUDescriptorHandleForHeapStart();
        sceneShaderView.ptr += static_cast<SIZE_T>(kHdrSceneDescriptorIndex)
            * shaderDescriptorStride;
        const D3D12_SHADER_RESOURCE_VIEW_DESC hdrView =
            texture2DView(kTargetFormat);
        device->CreateShaderResourceView(
            sceneTarget.Get(), &hdrView, sceneShaderView);
        sceneTargetsReadable = false;
        temporalHistoryValid = false;
        temporalHistoryReadable = {false, false};
        temporalWriteIndex = 0;
        temporalFrameIndex = 0;

        return true;
    }

    bool initializeScene(
        const fh6::OriginalShaderGarageScene &scene, quintptr nativeWindow,
        const QSize &size, const OriginalDx12Camera &camera) {
        viewportSize = size;
        lighting = scene.lighting;
        authoredLights = scene.authoredLights;
        panorama = scene.environment.panorama;
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
        HRESULT result = CreateDXGIFactory2(
            debug != nullptr ? DXGI_CREATE_FACTORY_DEBUG : 0,
            IID_PPV_ARGS(&factory));
        if (FAILED(result)) {
            failure = QStringLiteral("DXGI viewport factory creation failed: %1")
                .arg(hresultText(result));
            return false;
        }
        device = createHardwareDevice(factory.Get(), &adapterName);
        if (device == nullptr) {
            failure = QStringLiteral("no hardware D3D12 device is available");
            return false;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_6};
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (FAILED(device->CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))
            || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6
            || FAILED(device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
            || options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
            failure = QStringLiteral("required D3D12 SM6.6 capabilities are unavailable");
            return false;
        }
        device.As(&infoQueue);
        rootSignature = createRootSignature(device.Get(), &failure);
        floorPipeline = rootSignature != nullptr
            ? createExactPipeline(
                  device.Get(), rootSignature.Get(), scene.floorProgram, &failure,
                  kTargetFormat)
            : ComPtr<ID3D12PipelineState>{};
        defaultPipeline = rootSignature != nullptr
            ? createExactPipeline(
                  device.Get(), rootSignature.Get(), scene.defaultProgram, &failure,
                  kTargetFormat)
            : ComPtr<ID3D12PipelineState>{};
        materialPipeline = rootSignature != nullptr
            ? createMaterialPipeline(
                  device.Get(), rootSignature.Get(), &failure,
                  kTargetFormat)
            : ComPtr<ID3D12PipelineState>{};
        translucentMaterialPipeline = rootSignature != nullptr
            ? createMaterialPipeline(
                  device.Get(), rootSignature.Get(), &failure,
                  kTargetFormat, true)
            : ComPtr<ID3D12PipelineState>{};
        panoramaPipeline = rootSignature != nullptr
            ? createPanoramaPipeline(
                  device.Get(), rootSignature.Get(), &failure,
                  kTargetFormat)
            : ComPtr<ID3D12PipelineState>{};
        postPipeline = rootSignature != nullptr
            ? createPostPipeline(
                  device.Get(), rootSignature.Get(), &failure,
                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            : ComPtr<ID3D12PipelineState>{};
        shadowPipeline = rootSignature != nullptr
            ? createShadowPipeline(device.Get(), rootSignature.Get(), &failure)
            : ComPtr<ID3D12PipelineState>{};
        glassBackDepthPipeline = rootSignature != nullptr
            ? createGlassBackDepthPipeline(
                  device.Get(), rootSignature.Get(), &failure)
            : ComPtr<ID3D12PipelineState>{};
        if (rootSignature == nullptr || floorPipeline == nullptr
            || defaultPipeline == nullptr || materialPipeline == nullptr
            || translucentMaterialPipeline == nullptr
            || panoramaPipeline == nullptr || postPipeline == nullptr
            || shadowPipeline == nullptr || glassBackDepthPipeline == nullptr) {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result = device->CreateCommandQueue(
            &queueDescription, IID_PPV_ARGS(&queue));
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport queue creation failed: %1")
                .arg(hresultText(result));
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = static_cast<UINT>(size.width());
        swapDescription.Height = static_cast<UINT>(size.height());
        swapDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = static_cast<UINT>(backBuffers.size());
        swapDescription.SampleDesc.Count = 1;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> baseSwapChain;
        result = factory->CreateSwapChainForHwnd(
            queue.Get(), reinterpret_cast<HWND>(nativeWindow), &swapDescription,
            nullptr, nullptr, &baseSwapChain);
        if (FAILED(result) || FAILED(baseSwapChain.As(&swapChain))) {
            failure = QStringLiteral("D3D12 viewport swap-chain creation failed: %1")
                .arg(hresultText(result));
            return false;
        }
        factory->MakeWindowAssociation(
            reinterpret_cast<HWND>(nativeWindow), DXGI_MWA_NO_ALT_ENTER);
        result = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(result)) {
            result = device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                floorPipeline.Get(), IID_PPV_ARGS(&commands));
        }
        if (SUCCEEDED(result)) {
            result = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&liveryAllocator));
        }
        if (SUCCEEDED(result)) {
            result = device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, liveryAllocator.Get(),
                nullptr, IID_PPV_ARGS(&liveryCommands));
        }
        if (SUCCEEDED(result)) {
            result = liveryCommands->Close();
        }
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport command setup failed: %1")
                .arg(hresultText(result));
            return false;
        }

        draws.reserve(scene.draws.size());
        const bool hasAuthoredDropShadow = std::any_of(
            scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
                return !draw.hidden && draw.shadowCasterOnly;
            });
        for (const fh6::OriginalShaderGarageDraw &source : scene.draws) {
            if (source.hidden) {
                continue;
            }
            DrawResources draw;
            draw.geometry = prepareGeometry(
                source.geometry, source.placement, source.diffuseUvChannel,
                source.rawMaterialUv, source.materialUvChannel,
                source.materialUvRotationDegrees);
            draw.center = geometryCenter(draw.geometry);
            draw.family = source.family;
            draw.reflectionProbeShell = source.source.contains(
                QStringLiteral("garage_customiser"), Qt::CaseInsensitive)
                || source.source.contains(
                    QStringLiteral("bld_gbl_grge_custom_02"), Qt::CaseInsensitive);
            draw.diffuseTexture = source.diffuseTexture;
            draw.alphaTexture = source.alphaTexture;
            draw.normalTexture = source.normalTexture;
            draw.weaveMaskTexture = source.weaveMaskTexture;
            draw.weaveNormalTexture = source.weaveNormalTexture;
            draw.clearCoatNormalTexture = source.clearCoatNormalTexture;
            draw.surfaceTexture = source.surfaceTexture;
            draw.tireHeightAoTexture = source.tireHeightAoTexture;
            draw.aoTexture = source.aoTexture;
            draw.emissiveTexture = source.emissiveTexture;
            draw.baseColor = source.baseColor;
            draw.secondaryPaintColor = source.secondaryPaintColor;
            draw.flakeColor = source.flakeColor;
            draw.emissiveColor = source.emissiveColor;
            draw.opacity = source.opacity;
            draw.gloss = source.gloss;
            draw.roughnessShift = source.roughnessShift;
            draw.metallic = source.metallic;
            draw.flakeCoverage = source.flakeCoverage;
            draw.flakeRoughness = source.flakeRoughness;
            draw.glitterIntensity = source.glitterIntensity;
            draw.glancingFlopStrength = source.glancingFlopStrength;
            draw.glancingFlopPower = source.glancingFlopPower;
            draw.glancingFlopEnabled = source.glancingFlopEnabled;
            draw.uTiling = source.uTiling;
            draw.vTiling = source.vTiling;
            draw.detailUTiling = source.detailUTiling;
            draw.detailVTiling = source.detailVTiling;
            draw.normalIntensity = source.normalIntensity;
            draw.weaveNormalIntensity = source.weaveNormalIntensity;
            draw.clearCoatNormalUTiling = source.clearCoatNormalUTiling;
            draw.clearCoatNormalVTiling = source.clearCoatNormalVTiling;
            draw.weaveColorTintA = source.weaveColorTintA;
            draw.weaveColorTintB = source.weaveColorTintB;
            draw.sampler = source.sampler;
            draw.shaderFamily = source.shaderFamily;
            draw.clearCoatTint = source.clearCoatTint;
            draw.clearCoatCoverage = source.clearCoatCoverage;
            draw.clearCoatRoughness = source.clearCoatRoughness;
            draw.clearCoatOnLivery = source.clearCoatOnLivery;
            draw.translucent = source.translucent;
            const bool authoredShadowMember = source.drawGroups == 0
                || (source.drawGroups & fh6::car_draw_groups::kShadow) != 0;
            draw.selfShadowCaster = authoredShadowMember && !source.shadowCasterOnly
                && source.family == fh6::OriginalShaderSurfaceFamily::Car
                && !source.translucent;
            draw.dropShadowCaster = source.shadowCasterOnly
                || (!hasAuthoredDropShadow && draw.selfShadowCaster);
            draw.visible = !source.shadowCasterOnly;
            draw.liveryBaseTexture = source.liveryBaseTexture;
            draw.liveryAllowedSides = source.liveryAllowedSides;
            draw.drawGroups = source.drawGroups;
            draw.interiorWindshield = source.interiorWindshield;
            if (source.liveryBaseTexture && scene.liveryMapping.valid()) {
                draw.liverySideCount = scene.liveryMapping.sideCount;
                draw.liverySourceRegions = scene.liveryMapping.sourceRegions;
                draw.liveryPaintRegions = scene.liveryMapping.paintRegions;
                draw.liveryFacing = scene.liveryMapping.facing;
            }
            const UINT64 vertexBytes = static_cast<UINT64>(
                draw.geometry.vertices.size() * sizeof(Vertex));
            const UINT64 indexBytes = static_cast<UINT64>(
                draw.geometry.indices.size() * sizeof(std::uint32_t));
            draw.vertexBuffer = createBuffer(
                device.Get(), vertexBytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ);
            draw.indexBuffer = createBuffer(
                device.Get(), indexBytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ);
            if (draw.vertexBuffer == nullptr || draw.indexBuffer == nullptr
                || !uploadBuffer(
                    draw.vertexBuffer.Get(), draw.geometry.vertices.data(),
                    static_cast<std::size_t>(vertexBytes))
                || !uploadBuffer(
                    draw.indexBuffer.Get(), draw.geometry.indices.data(),
                    static_cast<std::size_t>(indexBytes))) {
                failure = QStringLiteral("D3D12 viewport geometry upload failed");
                return false;
            }
            draw.vertexView = {
                draw.vertexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(vertexBytes), sizeof(Vertex)};
            draw.indexView = {
                draw.indexBuffer->GetGPUVirtualAddress(),
                static_cast<UINT>(indexBytes), DXGI_FORMAT_R32_UINT};
            draw.materialConstants = createConstantBuffer(
                device.Get(), materialConstantData(draw));
            if (draw.materialConstants == nullptr) {
                failure = QStringLiteral("D3D12 viewport material constants failed");
                return false;
            }
            draws.push_back(std::move(draw));
        }

        colorLut = scene.colorLut;
        shadow = carShadowProjection(
            draws, lighting.direction, ShadowPassKind::Self,
            kShadowStrength);
        dropShadow = carShadowProjection(
            draws, {0.0f, -1.0f, 0.0f}, ShadowPassKind::Drop,
            kDropShadowStrength);
        localShadows = localLightShadowProjections(draws, authoredLights);
        reflectionProbe = reflectionProbeVolume(draws);
        previousViewProjection = cameraViewProjection(
            camera, static_cast<float>(size.width()) / size.height());
        TemporalFrameState temporal;
        temporal.previousViewProjection = previousViewProjection;
        const auto constants = shaderConstantData(
            camera, size, lighting, authoredLights, panorama, colorLut,
            shadow, dropShadow, localShadows, reflectionProbe, temporal);
        for (std::size_t index = 0; index < constantBuffers.size(); ++index) {
            constantBuffers[index] =
                createConstantBuffer(device.Get(), constants[index]);
            if (constantBuffers[index] == nullptr) {
                failure = QStringLiteral("D3D12 viewport constant upload failed");
                return false;
            }
        }
        shadowConstants = createConstantBuffer(
            device.Get(), shadowConstantData(shadow));
        dropShadowConstants = createConstantBuffer(
            device.Get(), shadowConstantData(dropShadow));
        for (std::size_t index = 0; index < localShadows.size(); ++index) {
            localShadowConstants[index] = createConstantBuffer(
                device.Get(), shadowConstantData(localShadows[index]));
        }
        if (shadowConstants == nullptr || dropShadowConstants == nullptr
            || localShadowConstants[0] == nullptr
            || localShadowConstants[1] == nullptr) {
            failure = QStringLiteral("D3D12 viewport shadow constants failed");
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC shaderHeapDescription{};
        shaderHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        const UINT descriptorCount = kFixedDescriptorCount
            + static_cast<UINT>(draws.size()) * kMaterialDescriptorCount;
        shaderHeapDescription.NumDescriptors = descriptorCount;
        shaderHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device->CreateDescriptorHeap(
            &shaderHeapDescription, IID_PPV_ARGS(&shaderHeap));
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport shader heap creation failed: %1")
                .arg(hresultText(result));
            return false;
        }
        shaderDescriptorStride = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        createNullDescriptors(
            device.Get(), shaderHeap.Get(), shaderDescriptorStride,
            descriptorCount);
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDescription{};
        samplerHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDescription.NumDescriptors =
            std::max(1u, static_cast<UINT>(draws.size()));
        samplerHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device->CreateDescriptorHeap(
            &samplerHeapDescription, IID_PPV_ARGS(&samplerHeap));
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport sampler heap creation failed: %1")
                .arg(hresultText(result));
            return false;
        }
        samplerDescriptorStride = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        const fh6::ModelMaterialSampler defaultSampler;
        for (UINT index = 0; index < samplerHeapDescription.NumDescriptors; ++index) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                samplerHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * samplerDescriptorStride;
            const D3D12_SAMPLER_DESC description = materialSamplerDescription(
                index < draws.size() ? draws[index].sampler : defaultSampler);
            device->CreateSampler(&description, handle);
            if (index < draws.size()) {
                draws[index].samplerDescriptorIndex = index;
            }
        }
        auto cpuHandleAt = [&](UINT index) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                shaderHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * shaderDescriptorStride;
            return handle;
        };
        materialTextures.reserve(scene.materialTextures.size());
        for (std::size_t index = 0; index < scene.materialTextures.size(); ++index) {
            UploadedTexture texture = uploadRgba8Texture(
                device.Get(), commands.Get(), scene.materialTextures[index].image,
                true, false, &scene.materialTextures[index].authoredMips);
            if (texture.texture == nullptr) {
                failure = QStringLiteral("D3D12 viewport material upload failed");
                return false;
            }
            const D3D12_SHADER_RESOURCE_VIEW_DESC view =
                texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM, texture.mipLevels);
            device->CreateShaderResourceView(
                texture.texture.Get(), &view,
                cpuHandleAt(kMaterialDescriptorStart + static_cast<UINT>(index)));
            materialTextures.push_back(std::move(texture));
        }
        authoredMaterialTextures.reserve(draws.size() * 10);
        std::unordered_map<const fh6::OriginalShaderMaterialTexture *, std::size_t>
            authoredTextureIndices;
        if (scene.liveryMapping.valid()) {
            for (int side = 0; side < fh6::kLiverySideCount; ++side) {
                if (!scene.liveryMapping.masks[side].valid()) {
                    continue;
                }
                liveryMaskTextures[side] = uploadRgba8Texture(
                    device.Get(), commands.Get(),
                    scene.liveryMapping.masks[side].image, false);
                if (liveryMaskTextures[side].texture == nullptr) {
                    failure = QStringLiteral("D3D12 viewport livery mask upload failed");
                    return false;
                }
            }
        }
        for (std::size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex) {
            DrawResources &draw = draws[drawIndex];
            draw.materialDescriptorStart = kFixedDescriptorCount
                + static_cast<UINT>(drawIndex) * kMaterialDescriptorCount;
            const D3D12_SHADER_RESOURCE_VIEW_DESC fallbackView =
                texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
            constexpr std::array<std::size_t, 4> fallbackIndices = {1, 6, 0, 4};
            for (std::size_t index = 0; index < fallbackIndices.size(); ++index) {
                device->CreateShaderResourceView(
                    materialTextures[fallbackIndices[index]].texture.Get(), &fallbackView,
                    cpuHandleAt(
                        draw.materialDescriptorStart + static_cast<UINT>(index)));
            }
            device->CreateShaderResourceView(
                materialTextures[1].texture.Get(), &fallbackView,
                cpuHandleAt(draw.materialDescriptorStart + 15u));
            const std::array<std::pair<
                std::shared_ptr<const fh6::OriginalShaderMaterialTexture>, UINT>, 10>
                sources = {{{draw.diffuseTexture, 0u}, {draw.normalTexture, 1u},
                            {draw.surfaceTexture, 2u}, {draw.emissiveTexture, 3u},
                            {draw.alphaTexture, 15u}, {draw.weaveMaskTexture, 16u},
                            {draw.weaveNormalTexture, 17u},
                            {draw.clearCoatNormalTexture, 18u},
                            {draw.tireHeightAoTexture, 19u},
                            {draw.aoTexture, 20u}}};
            for (const auto &[source, slot] : sources) {
                if (source == nullptr || !source->valid()) {
                    continue;
                }
                const auto [found, inserted] = authoredTextureIndices.emplace(
                    source.get(), authoredMaterialTextures.size());
                if (inserted) {
                    const bool liveLivery = source->sourceEntry.startsWith(
                        QStringLiteral("car://composited-livery"));
                    UploadedTexture texture = uploadRgba8Texture(
                        device.Get(), commands.Get(), source->image,
                        !liveLivery, slot == 1u || slot == 17u || slot == 18u,
                        &source->authoredMips);
                    if (texture.texture == nullptr) {
                        failure = QStringLiteral(
                            "D3D12 viewport authored material upload failed");
                        return false;
                    }
                    authoredMaterialTextures.push_back(std::move(texture));
                }
                const D3D12_SHADER_RESOURCE_VIEW_DESC view =
                    texture2DView(
                        DXGI_FORMAT_R8G8B8A8_UNORM,
                        authoredMaterialTextures[found->second].mipLevels);
                device->CreateShaderResourceView(
                    authoredMaterialTextures[found->second].texture.Get(), &view,
                    cpuHandleAt(
                        draw.materialDescriptorStart + static_cast<UINT>(slot)));
            }
            if (draw.liveryBaseTexture && scene.liveryMapping.valid()) {
                liveryDescriptorIndices.push_back(draw.materialDescriptorStart);
                for (int side = 0; side < fh6::kLiverySideCount; ++side) {
                    if (liveryMaskTextures[side].texture == nullptr) {
                        continue;
                    }
                    device->CreateShaderResourceView(
                        liveryMaskTextures[side].texture.Get(), &fallbackView,
                        cpuHandleAt(draw.materialDescriptorStart + 4u
                                    + static_cast<UINT>(side)));
                }
            }
        }
        diffuseCube = uploadCubeTexture(
            device.Get(), commands.Get(), scene.environment.diffuseCubemap,
            fh6::SwatchEncoding::R16G16B16A16Float,
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (diffuseCube.texture == nullptr) {
            failure = QStringLiteral("D3D12 viewport cubemap upload failed");
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC diffuseView{};
        diffuseView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        diffuseView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        diffuseView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        diffuseView.TextureCube.MipLevels = diffuseCube.texture->GetDesc().MipLevels;
        device->CreateShaderResourceView(
            diffuseCube.texture.Get(), &diffuseView, cpuHandleAt(0));
        specularCube = uploadCubeTexture(
            device.Get(), commands.Get(), scene.environment.specularCubemap,
            fh6::SwatchEncoding::UnsignedBc6H, DXGI_FORMAT_BC6H_UF16);
        if (specularCube.texture == nullptr) {
            failure = QStringLiteral("D3D12 viewport reflection-probe upload failed");
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC specularView{};
        specularView.Format = DXGI_FORMAT_BC6H_UF16;
        specularView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        specularView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        specularView.TextureCube.MipLevels = specularCube.texture->GetDesc().MipLevels;
        device->CreateShaderResourceView(
            specularCube.texture.Get(), &specularView, cpuHandleAt(1));
        panoramaTexture = uploadBc6Panorama(
            device.Get(), commands.Get(), scene.environment.panorama.texture);
        if (panoramaTexture.texture == nullptr) {
            failure = QStringLiteral("Tokyo staged-space panorama upload failed");
            return false;
        }
        const D3D12_SHADER_RESOURCE_VIEW_DESC panoramaView =
            texture2DView(DXGI_FORMAT_BC6H_UF16);
        device->CreateShaderResourceView(
            panoramaTexture.texture.Get(), &panoramaView, cpuHandleAt(120));
        colorLutTexture = uploadColorLut(device.Get(), commands.Get(), colorLut);
        if (colorLutTexture.texture == nullptr) {
            failure = QStringLiteral("D3D12 viewport Homespace LUT upload failed");
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC colorLutView{};
        colorLutView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        colorLutView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        colorLutView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        colorLutView.Texture3D.MipLevels = 1;
        device->CreateShaderResourceView(
            colorLutTexture.texture.Get(), &colorLutView, cpuHandleAt(123));

        D3D12_DESCRIPTOR_HEAP_DESC targetHeapDescription{};
        targetHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targetHeapDescription.NumDescriptors =
            static_cast<UINT>(backBuffers.size()) + 3u;
        D3D12_DESCRIPTOR_HEAP_DESC depthHeapDescription{};
        depthHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        depthHeapDescription.NumDescriptors = 6;
        if (FAILED(device->CreateDescriptorHeap(
                &targetHeapDescription, IID_PPV_ARGS(&targetHeap)))
            || FAILED(device->CreateDescriptorHeap(
                &depthHeapDescription, IID_PPV_ARGS(&depthHeap)))) {
            failure = QStringLiteral("D3D12 viewport target heap creation failed");
            return false;
        }
        targetDescriptorStride = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        depthDescriptorStride = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        if (!createTargets()) {
            return false;
        }
        shadowTarget = createShadowDepthTarget(device.Get());
        dropShadowTarget = createShadowDepthTarget(device.Get());
        for (ComPtr<ID3D12Resource> &target : localShadowTargets) {
            target = createShadowDepthTarget(device.Get());
        }
        if (shadowTarget == nullptr || dropShadowTarget == nullptr
            || localShadowTargets[0] == nullptr
            || localShadowTargets[1] == nullptr) {
            failure = QStringLiteral("D3D12 car-shadow target creation failed");
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDepthView =
            depthHeap->GetCPUDescriptorHandleForHeapStart();
        shadowDepthView.ptr += depthDescriptorStride;
        createShadowViews(
            device.Get(), shadowTarget.Get(), shadowDepthView,
            cpuHandleAt(kShadowDescriptorIndex));
        D3D12_CPU_DESCRIPTOR_HANDLE dropShadowDepthView = shadowDepthView;
        dropShadowDepthView.ptr += depthDescriptorStride;
        createShadowViews(
            device.Get(), dropShadowTarget.Get(), dropShadowDepthView,
            cpuHandleAt(kDropShadowDescriptorIndex));
        for (std::size_t index = 0; index < localShadowTargets.size(); ++index) {
            D3D12_CPU_DESCRIPTOR_HANDLE localDepthView = dropShadowDepthView;
            localDepthView.ptr += (index + 1) * depthDescriptorStride;
            createShadowViews(
                device.Get(), localShadowTargets[index].Get(), localDepthView,
                cpuHandleAt(kLocalShadowDescriptorIndices[index]));
        }
        result = commands->Close();
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport upload recording failed: %1")
                .arg(hresultText(result));
            return false;
        }
        ID3D12CommandList *lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        result = device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(result) || completionEvent == nullptr || !waitForGpu()) {
            failure = QStringLiteral("D3D12 viewport fence setup failed");
            return false;
        }
        if (infoQueue != nullptr) {
            infoQueue->ClearStoredMessages();
        }

        return true;
    }

    bool replaceLivery(const fh6::SwatchImage &source) {
        if (liveryDescriptorIndices.empty()
            || (liveryCommandsInFlight && !waitForGpu())) {
            return false;
        }
        fh6::SwatchImage transparent;
        const fh6::SwatchImage *image = &source;
        if (!source.valid()) {
            transparent.width = 1;
            transparent.height = 1;
            transparent.rgba = {0, 0, 0, 0};
            image = &transparent;
        }
        HRESULT result = liveryAllocator->Reset();
        if (SUCCEEDED(result)) {
            result = liveryCommands->Reset(liveryAllocator.Get(), nullptr);
        }
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 livery command reset failed: %1")
                .arg(hresultText(result));
            return false;
        }
        UploadedTexture replacement = uploadRgba8Texture(
            device.Get(), liveryCommands.Get(), *image, false);
        if (replacement.texture == nullptr) {
            failure = QStringLiteral("D3D12 live livery upload failed");
            return false;
        }
        const D3D12_SHADER_RESOURCE_VIEW_DESC view =
            texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM, replacement.mipLevels);
        for (const UINT descriptorIndex : liveryDescriptorIndices) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                shaderHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(descriptorIndex)
                * shaderDescriptorStride;
            device->CreateShaderResourceView(
                replacement.texture.Get(), &view, handle);
        }
        result = liveryCommands->Close();
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 livery command recording failed: %1")
                .arg(hresultText(result));
            return false;
        }
        ID3D12CommandList *lists[] = {liveryCommands.Get()};
        queue->ExecuteCommandLists(1, lists);
        liveryCommandsInFlight = true;
        liveLiveryTexture = std::move(replacement);
        temporalHistoryValid = false;
        return true;
    }

    bool resizeTargets(const QSize &size) {
        if (size.isEmpty() || size == viewportSize) {
            return !size.isEmpty();
        }
        if (!waitForGpu()) {
            return false;
        }
        for (auto &buffer : backBuffers) {
            buffer.Reset();
        }
        depthTarget.Reset();
        sceneTarget.Reset();
        glassBackDepthTarget.Reset();
        for (ComPtr<ID3D12Resource> &history : temporalHistoryTargets) {
            history.Reset();
        }
        sceneTargetsReadable = false;
        viewportSize = size;
        const HRESULT result = swapChain->ResizeBuffers(
            static_cast<UINT>(backBuffers.size()),
            static_cast<UINT>(size.width()), static_cast<UINT>(size.height()),
            DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport resize failed: %1")
                .arg(hresultText(result));
            return false;
        }

        return createTargets();
    }

    bool renderFrame(const OriginalDx12Camera &camera) {
        const QPointF jitter = temporalJitterNdc(
            temporalFrameIndex, viewportSize);
        TemporalFrameState temporal;
        temporal.previousViewProjection = previousViewProjection;
        temporal.jitterNdc = jitter;
        temporal.historyValid = temporalHistoryValid;
        temporal.previousHistoryIndex = 1 - temporalWriteIndex;
        const auto constants = shaderConstantData(
            camera, viewportSize, lighting, authoredLights, panorama, colorLut,
            shadow, dropShadow, localShadows, reflectionProbe, temporal);
        if (!uploadBuffer(
                constantBuffers[3].Get(), constants[3].data(), constants[3].size())
            || !uploadBuffer(
                constantBuffers[5].Get(), constants[5].data(), constants[5].size())) {
            failure = QStringLiteral("D3D12 viewport camera upload failed");
            return false;
        }
        HRESULT result = allocator->Reset();
        if (SUCCEEDED(result)) {
            result = commands->Reset(allocator.Get(), floorPipeline.Get());
        }
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport command reset failed: %1")
                .arg(hresultText(result));
            return false;
        }
        const int previousHistoryIndex = 1 - temporalWriteIndex;
        std::array<D3D12_RESOURCE_BARRIER, 2> historyBeginBarriers{};
        UINT historyBeginCount = 0;
        if (temporalHistoryReadable[temporalWriteIndex]) {
            historyBeginBarriers[historyBeginCount++] = transition(
                temporalHistoryTargets[temporalWriteIndex].Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (!temporalHistoryReadable[previousHistoryIndex]) {
            historyBeginBarriers[historyBeginCount++] = transition(
                temporalHistoryTargets[previousHistoryIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            temporalHistoryReadable[previousHistoryIndex] = true;
        }
        if (historyBeginCount > 0) {
            commands->ResourceBarrier(
                historyBeginCount, historyBeginBarriers.data());
        }
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDepthView =
            depthHeap->GetCPUDescriptorHandleForHeapStart();
        shadowDepthView.ptr += depthDescriptorStride;
        recordShadowPass(
            commands.Get(), rootSignature.Get(), shadowPipeline.Get(),
            shadowConstants.Get(), shadowDepthView, draws,
            ShadowPassKind::Self);
        const D3D12_RESOURCE_BARRIER shadowReadBarrier = transition(
            shadowTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &shadowReadBarrier);
        D3D12_CPU_DESCRIPTOR_HANDLE dropShadowDepthView = shadowDepthView;
        dropShadowDepthView.ptr += depthDescriptorStride;
        recordShadowPass(
            commands.Get(), rootSignature.Get(), shadowPipeline.Get(),
            dropShadowConstants.Get(), dropShadowDepthView, draws,
            ShadowPassKind::Drop);
        const D3D12_RESOURCE_BARRIER dropShadowReadBarrier = transition(
            dropShadowTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &dropShadowReadBarrier);
        for (std::size_t index = 0; index < localShadows.size(); ++index) {
            D3D12_CPU_DESCRIPTOR_HANDLE localDepthView = dropShadowDepthView;
            localDepthView.ptr += (index + 1) * depthDescriptorStride;
            if (localShadows[index].valid) {
                recordShadowPass(
                    commands.Get(), rootSignature.Get(), shadowPipeline.Get(),
                    localShadowConstants[index].Get(), localDepthView, draws,
                    ShadowPassKind::Self);
            }
            const D3D12_RESOURCE_BARRIER localReadBarrier = transition(
                localShadowTargets[index].Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commands->ResourceBarrier(1, &localReadBarrier);
        }
        D3D12_CPU_DESCRIPTOR_HANDLE glassBackDepthView =
            depthHeap->GetCPUDescriptorHandleForHeapStart();
        glassBackDepthView.ptr += 5 * depthDescriptorStride;
        recordGlassBackDepthPass(
            commands.Get(), rootSignature.Get(), glassBackDepthPipeline.Get(),
            constantBuffers[5].Get(), glassBackDepthView, viewportSize, draws);
        const D3D12_RESOURCE_BARRIER glassDepthReadBarrier = transition(
            glassBackDepthTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->ResourceBarrier(1, &glassDepthReadBarrier);
        if (sceneTargetsReadable) {
            const std::array<D3D12_RESOURCE_BARRIER, 2> sceneWriteBarriers = {{
                transition(
                    sceneTarget.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET),
                transition(
                    depthTarget.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE),
            }};
            commands->ResourceBarrier(
                static_cast<UINT>(sceneWriteBarriers.size()),
                sceneWriteBarriers.data());
        }
        const UINT bufferIndex = swapChain->GetCurrentBackBufferIndex();
        const D3D12_RESOURCE_BARRIER beginBarrier = transition(
            backBuffers[bufferIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands->ResourceBarrier(1, &beginBarrier);
        D3D12_CPU_DESCRIPTOR_HANDLE targetView =
            targetHeap->GetCPUDescriptorHandleForHeapStart();
        targetView.ptr += static_cast<SIZE_T>(bufferIndex) * targetDescriptorStride;
        D3D12_CPU_DESCRIPTOR_HANDLE sceneView =
            targetHeap->GetCPUDescriptorHandleForHeapStart();
        sceneView.ptr += static_cast<SIZE_T>(backBuffers.size())
            * targetDescriptorStride;
        const D3D12_CPU_DESCRIPTOR_HANDLE depthView =
            depthHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_VIEWPORT viewport{
            0.0f, 0.0f, static_cast<float>(viewportSize.width()),
            static_cast<float>(viewportSize.height()), 0.0f, 1.0f};
        const D3D12_RECT scissor{
            0, 0, static_cast<LONG>(viewportSize.width()),
            static_cast<LONG>(viewportSize.height())};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->OMSetRenderTargets(1, &sceneView, FALSE, &depthView);
        commands->ClearRenderTargetView(
            sceneView, kClearColor.data(), 0, nullptr);
        commands->ClearDepthStencilView(
            depthView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commands->SetGraphicsRootSignature(rootSignature.Get());
        ID3D12DescriptorHeap *heaps[] = {shaderHeap.Get(), samplerHeap.Get()};
        commands->SetDescriptorHeaps(2, heaps);
        for (UINT index = 0; index < constantBuffers.size(); ++index) {
            commands->SetGraphicsRootConstantBufferView(
                index, constantBuffers[index]->GetGPUVirtualAddress());
        }
        auto gpuHandleAt = [&](UINT index) {
            D3D12_GPU_DESCRIPTOR_HANDLE handle =
                shaderHeap->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(index) * shaderDescriptorStride;
            return handle;
        };
        const auto samplerGpuHandleAt = [&](UINT index) {
            D3D12_GPU_DESCRIPTOR_HANDLE handle =
                samplerHeap->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(index) * samplerDescriptorStride;
            return handle;
        };
        commands->SetGraphicsRootDescriptorTable(8, gpuHandleAt(0));
        commands->SetGraphicsRootDescriptorTable(10, gpuHandleAt(140));
        commands->SetGraphicsRootDescriptorTable(11, gpuHandleAt(156));
        commands->SetGraphicsRootDescriptorTable(12, samplerGpuHandleAt(0));
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->SetPipelineState(panoramaPipeline.Get());
        commands->DrawInstanced(3, 1, 0, 0);
        for (const bool translucentPass : {false, true}) {
            commands->SetPipelineState(
                translucentPass ? translucentMaterialPipeline.Get()
                                : materialPipeline.Get());
            for (const DrawResources *drawPointer
                 : drawPassOrder(draws, translucentPass, camera.position)) {
                const DrawResources &draw = *drawPointer;
                commands->SetGraphicsRootDescriptorTable(
                    9, gpuHandleAt(draw.materialDescriptorStart));
                commands->SetGraphicsRootDescriptorTable(
                    12, samplerGpuHandleAt(draw.samplerDescriptorIndex));
                commands->SetGraphicsRootConstantBufferView(
                    7, draw.materialConstants->GetGPUVirtualAddress());
                commands->IASetVertexBuffers(0, 1, &draw.vertexView);
                commands->IASetIndexBuffer(&draw.indexView);
                commands->DrawIndexedInstanced(
                    static_cast<UINT>(draw.geometry.indices.size()), 1, 0, 0, 0);
            }
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> postReadBarriers = {{
            transition(
                sceneTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition(
                depthTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        }};
        commands->ResourceBarrier(
            static_cast<UINT>(postReadBarriers.size()),
            postReadBarriers.data());
        D3D12_CPU_DESCRIPTOR_HANDLE historyTargetView =
            targetHeap->GetCPUDescriptorHandleForHeapStart();
        historyTargetView.ptr += static_cast<SIZE_T>(
            backBuffers.size() + 1 + temporalWriteIndex)
            * targetDescriptorStride;
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> postTargets = {
            targetView, historyTargetView};
        commands->OMSetRenderTargets(
            static_cast<UINT>(postTargets.size()), postTargets.data(), FALSE,
            nullptr);
        commands->SetPipelineState(postPipeline.Get());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        const std::array<D3D12_RESOURCE_BARRIER, 7> endBarriers = {{
            transition(
                backBuffers[bufferIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT),
            transition(
                shadowTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE),
            transition(
                dropShadowTarget.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE),
            transition(
                localShadowTargets[0].Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE),
            transition(
                localShadowTargets[1].Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE),
            transition(
                glassBackDepthTarget.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE),
            transition(
                temporalHistoryTargets[temporalWriteIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        }};
        commands->ResourceBarrier(
            static_cast<UINT>(endBarriers.size()), endBarriers.data());
        result = commands->Close();
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport recording failed: %1")
                .arg(hresultText(result));
            return false;
        }
        ID3D12CommandList *lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        result = swapChain->Present(1, 0);
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport presentation failed: %1")
                .arg(hresultText(result));
            return false;
        }
        if (!waitForGpu()) {
            return false;
        }
        sceneTargetsReadable = true;
        temporalHistoryReadable[temporalWriteIndex] = true;
        temporalHistoryValid = true;
        previousViewProjection = cameraViewProjection(
            camera,
            static_cast<float>(viewportSize.width()) / viewportSize.height(),
            jitter);
        temporalWriteIndex = 1 - temporalWriteIndex;
        ++temporalFrameIndex;
        if (infoQueue != nullptr) {
            const int errors = countMessages(
                infoQueue.Get(), D3D12_MESSAGE_SEVERITY_ERROR);
            if (errors > 0) {
                failure = QStringLiteral("D3D12 viewport debug layer reported errors: %1")
                    .arg(firstMessage(
                        infoQueue.Get(), D3D12_MESSAGE_SEVERITY_ERROR));
                infoQueue->ClearStoredMessages();
                return false;
            }
            infoQueue->ClearStoredMessages();
        }

        return true;
    }
};
#else
struct OriginalDx12ViewportRenderer::Impl {
    QString failure = QStringLiteral("the original-DXIL viewport is Windows-only");
};
#endif

OriginalDx12ViewportRenderer::OriginalDx12ViewportRenderer()
    : impl_(std::make_unique<Impl>()) {
}

OriginalDx12ViewportRenderer::~OriginalDx12ViewportRenderer() = default;

bool OriginalDx12ViewportRenderer::initialize(
    const fh6::OriginalShaderGarageScene &scene, quintptr nativeWindow,
    const QSize &size, const OriginalDx12Camera &camera) {
    release();
    impl_ = std::make_unique<Impl>();
#ifdef Q_OS_WIN
    if (!scene.valid() || nativeWindow == 0 || size.isEmpty() || !camera.valid()) {
        impl_->failure = QStringLiteral("invalid D3D12 viewport initialization input");
        return false;
    }

    return impl_->initializeScene(scene, nativeWindow, size, camera);
#else
    Q_UNUSED(scene)
    Q_UNUSED(nativeWindow)
    Q_UNUSED(size)
    Q_UNUSED(camera)
    return false;
#endif
}

bool OriginalDx12ViewportRenderer::resize(const QSize &size) {
#ifdef Q_OS_WIN
    return ready() && impl_->resizeTargets(size);
#else
    Q_UNUSED(size)
    return false;
#endif
}

bool OriginalDx12ViewportRenderer::render(const OriginalDx12Camera &camera) {
#ifdef Q_OS_WIN
    if (!ready() || !camera.valid()) {
        return false;
    }

    return impl_->renderFrame(camera);
#else
    Q_UNUSED(camera)
    return false;
#endif
}

bool OriginalDx12ViewportRenderer::updateLivery(
    const fh6::SwatchImage &livery) {
#ifdef Q_OS_WIN
    return ready() && impl_->replaceLivery(livery);
#else
    Q_UNUSED(livery)
    return false;
#endif
}

void OriginalDx12ViewportRenderer::release() {
    impl_.reset();
}

bool OriginalDx12ViewportRenderer::ready() const {
#ifdef Q_OS_WIN
    return impl_ != nullptr && impl_->swapChain != nullptr
        && impl_->failure.isEmpty();
#else
    return false;
#endif
}

QString OriginalDx12ViewportRenderer::adapter() const {
#ifdef Q_OS_WIN
    return impl_ != nullptr ? impl_->adapterName : QString();
#else
    return {};
#endif
}

QString OriginalDx12ViewportRenderer::error() const {
    return impl_ != nullptr ? impl_->failure
                            : QStringLiteral("D3D12 viewport is not initialized");
}

} // namespace gui
