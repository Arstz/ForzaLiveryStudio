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
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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
    for (const fh6::OriginalShaderGarageDraw &draw : scene.draws) {
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
    camera.farPlane = distance + radius * 4.0f;
    return camera;
}

#ifdef Q_OS_WIN
namespace {

struct Binding {
    UINT shaderRegister;
    UINT registerSpace;
};

constexpr DXGI_FORMAT kTargetFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr std::array<float, 4> kClearColor = {0.125f, 0.25f, 0.5f, 1.0f};
constexpr UINT kFixedDescriptorCount = 176;
constexpr UINT kMaterialDescriptorStart = 124;
constexpr UINT kMaterialDescriptorCount = 16;

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
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    UINT materialDescriptorStart = kMaterialDescriptorStart;
    std::shared_ptr<const fh6::OriginalShaderMaterialTexture> diffuseTexture;
    fh6::OriginalShaderSurfaceFamily family =
        fh6::OriginalShaderSurfaceFamily::Default;
};

struct UploadedTexture {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> upload;
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
    const fh6::CarModel &model, const fh6::ModelMat4 &placement) {
    Geometry result;
    for (const fh6::CarMesh &mesh : model.meshes) {
        const auto transformedUv = [&mesh](std::size_t channel, std::size_t vertex) {
            fh6::ModelVec2 uv{};
            if (channel >= mesh.uvChannels.size()
                || vertex >= mesh.uvChannels[channel].size()) {
                return uv;
            }
            uv = mesh.uvChannels[channel][vertex];
            if (channel < mesh.texCoordTransforms.size()) {
                const fh6::TexCoordTransform &transform =
                    mesh.texCoordTransforms[channel];
                uv.u = uv.u * transform.scaleU + transform.offsetU;
                uv.v = uv.v * transform.scaleV + transform.offsetV;
            }
            return uv;
        };
        const fh6::ModelMat4 transform =
            fh6::matMul(mesh.boneTransform, placement);
        const std::uint32_t base = static_cast<std::uint32_t>(result.vertices.size());
        std::vector<std::array<float, 3>> tangents(
            mesh.positions.size(), {0.0f, 0.0f, 0.0f});
        std::vector<std::array<float, 3>> bitangents(
            mesh.positions.size(), {0.0f, 0.0f, 0.0f});
        if (!mesh.uvChannels.empty()) {
            const auto &uvs = mesh.uvChannels[0];
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
                const fh6::ModelVec2 uv0 = transformedUv(0, indices[0]);
                const fh6::ModelVec2 uv1 = transformedUv(0, indices[1]);
                const fh6::ModelVec2 uv2 = transformedUv(0, indices[2]);
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
            const float handedness = cross[0] * bitangents[index][0]
                    + cross[1] * bitangents[index][1]
                    + cross[2] * bitangents[index][2]
                < 0.0f ? -1.0f : 1.0f;
            const fh6::ModelVec2 uv = transformedUv(0, index);
            fh6::ModelVec2 uv2 = uv;
            if (mesh.uvChannels.size() > 2 && index < mesh.uvChannels[2].size()) {
                uv2 = transformedUv(2, index);
            }
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

D3D12_SHADER_RESOURCE_VIEW_DESC texture2DView(DXGI_FORMAT format) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;

    return view;
}

UploadedTexture uploadRgba8Texture(
    ID3D12Device *device, ID3D12GraphicsCommandList *commands,
    const fh6::SwatchImage &image) {
    if (!image.valid()) {
        return {};
    }
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = static_cast<UINT64>(image.width);
    description.Height = static_cast<UINT>(image.height);
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
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
    const std::size_t sourcePitch = static_cast<std::size_t>(image.width) * 4;
    for (UINT row = 0; row < rows; ++row) {
        std::memcpy(
            static_cast<std::uint8_t *>(mapped) + footprint.Offset
                + static_cast<UINT64>(row) * footprint.Footprint.RowPitch,
            image.rgba.data() + static_cast<std::size_t>(row) * sourcePitch,
            sourcePitch);
    }
    result.upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = result.texture.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = result.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
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
    const OriginalDx12Camera &camera, float aspectRatio) {
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
        {verticalScale / aspectRatio, 0.0f, 0.0f, 0.0f},
        {0.0f, verticalScale, 0.0f, 0.0f},
        {0.0f, 0.0f, depthScale, -camera.nearPlane * depthScale},
        {0.0f, 0.0f, 1.0f, 0.0f},
    }};

    return multiply(projection, view);
}

std::array<std::vector<std::uint8_t>, 8> shaderConstantData(
    const OriginalDx12Camera &camera, const QSize &frameSize) {
    std::array<std::vector<std::uint8_t>, 8> data;
    constexpr std::array<std::size_t, 8> sizes = {
        256, 256, 512, 11264, 18432, 256, 65536, 65536};
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
        camera, static_cast<float>(frameSize.width()) / frameSize.height());
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
    writeFloat(&data[4], 0, 0, -0.197420f);
    writeFloat(&data[4], 0, 1, -0.302618f);
    writeFloat(&data[4], 0, 2, 0.932442f);
    writeFloat(&data[4], 1, 0, 0.900001f);
    writeFloat(&data[4], 1, 1, 0.900000f);
    writeFloat(&data[4], 1, 2, 0.750001f);
    writeFloat(&data[4], 1, 3, 1.0f);
    writeFloat(&data[4], 2, 0, 0.142109f);
    writeFloat(&data[4], 2, 1, 0.175171f);
    writeFloat(&data[4], 2, 2, 0.197850f);
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
    setCube(kSpace0Start + 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    setCube(kSpace0Start + 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    setBuffer(kSpace0Start + 45, DXGI_FORMAT_UNKNOWN, 192);
    setBuffer(kSpace0Start + 86, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
    setBuffer(kSpace0Start + 87, DXGI_FORMAT_R32G32_FLOAT, 0);
    setBuffer(kSpace0Start + 109, DXGI_FORMAT_UNKNOWN, 144);
    setCube(kSpace0Start + 117, DXGI_FORMAT_R32_FLOAT);
    setCube(kSpace0Start + 118, DXGI_FORMAT_R8G8B8A8_UNORM);
    setTexture3D(kSpace0Start + 123, DXGI_FORMAT_R8G8B8A8_UNORM);
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
    description.Format = DXGI_FORMAT_D32_FLOAT;
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
        : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler->AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler->AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler->AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler->MaxAnisotropy = 1;
    sampler->ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    sampler->BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler->MinLOD = 0.0f;
    sampler->MaxLOD = FLT_MAX;
    sampler->ShaderRegister = shaderRegister;
    sampler->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
}

ComPtr<ID3D12RootSignature> createRootSignature(
    ID3D12Device *device, QString *error) {
    constexpr std::array<Binding, 8> cbvs = {{
        {1, 2}, {1, 0}, {9, 0}, {2, 0},
        {3, 0}, {0, 2}, {0, 1}, {0, 3},
    }};
    std::array<D3D12_DESCRIPTOR_RANGE, 4> ranges{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 124, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0};
    ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 6, 0};
    ranges[3] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 0, 12, 0};

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

    constexpr std::array<UINT, 7> samplerRegisters = {0, 1, 2, 3, 5, 10, 12};
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

D3D12_BLEND_DESC blendDescription() {
    D3D12_BLEND_DESC description{};
    D3D12_RENDER_TARGET_BLEND_DESC target{};
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_ZERO;
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

D3D12_DEPTH_STENCIL_DESC depthDescription() {
    D3D12_DEPTH_STENCIL_DESC description{};
    description.DepthEnable = TRUE;
    description.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    return description;
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
    if (floorPipeline == nullptr || defaultPipeline == nullptr) {
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
    if (rootSignature == nullptr || floorPipeline == nullptr
        || defaultPipeline == nullptr) {
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
    for (const fh6::OriginalShaderGarageDraw &source : scene.draws) {
        DrawResources draw;
        draw.geometry = prepareGeometry(source.geometry, source.placement);
        draw.family = source.family;
        draw.diffuseTexture = source.diffuseTexture;
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
        draws.push_back(std::move(draw));
    }

    const std::array<std::vector<std::uint8_t>, 8> constantData =
        shaderConstantData(camera, size);
    std::array<ComPtr<ID3D12Resource>, 8> constantBuffers;
    for (std::size_t index = 0; index < constantBuffers.size(); ++index) {
        constantBuffers[index] =
            createConstantBuffer(device.Get(), constantData[index]);
        if (constantBuffers[index] == nullptr) {
            frame.error = QStringLiteral("original-DXIL constant upload failed");
            return frame;
        }
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

    std::vector<UploadedTexture> materialTextures;
    materialTextures.reserve(scene.materialTextures.size());
    for (std::size_t index = 0; index < scene.materialTextures.size(); ++index) {
        UploadedTexture texture = uploadRgba8Texture(
            device.Get(), commands.Get(), scene.materialTextures[index].image);
        if (texture.texture == nullptr) {
            frame.error = QStringLiteral("original-DXIL material texture upload failed");
            return frame;
        }
        const D3D12_SHADER_RESOURCE_VIEW_DESC view =
            texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
        device->CreateShaderResourceView(
            texture.texture.Get(), &view,
            cpuHandleAt(kMaterialDescriptorStart + static_cast<UINT>(index)));
        materialTextures.push_back(std::move(texture));
    }
    std::vector<UploadedTexture> authoredDiffuseTextures;
    authoredDiffuseTextures.reserve(draws.size());
    for (std::size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex) {
        DrawResources &draw = draws[drawIndex];
        draw.materialDescriptorStart = kFixedDescriptorCount
            + static_cast<UINT>(drawIndex) * kMaterialDescriptorCount;
        const D3D12_SHADER_RESOURCE_VIEW_DESC fallbackView =
            texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
        for (std::size_t index = 0; index < materialTextures.size(); ++index) {
            device->CreateShaderResourceView(
                materialTextures[index].texture.Get(), &fallbackView,
                cpuHandleAt(
                    draw.materialDescriptorStart + static_cast<UINT>(index)));
        }
        if (draw.diffuseTexture == nullptr || !draw.diffuseTexture->valid()) {
            continue;
        }
        UploadedTexture texture = uploadRgba8Texture(
            device.Get(), commands.Get(), draw.diffuseTexture->image);
        if (texture.texture == nullptr) {
            frame.error = QStringLiteral("Tokyo diffuse texture upload failed");
            return frame;
        }
        const D3D12_SHADER_RESOURCE_VIEW_DESC view =
            texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
        device->CreateShaderResourceView(
            texture.texture.Get(), &view,
            cpuHandleAt(draw.materialDescriptorStart + 1));
        authoredDiffuseTextures.push_back(std::move(texture));
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
    // Tokyo House does not use the homespace material contract. Leaving the
    // specular-cubemap SRV null prevents that compatibility shader from projecting
    // the garage lightprobe across unresolved building surfaces. The diffuse cube
    // remains bound so neutral fallback materials retain readable irradiance.

    const ComPtr<ID3D12Resource> target = createTarget(device.Get(), size);
    const ComPtr<ID3D12Resource> depthTarget =
        createDepthTarget(device.Get(), size);
    D3D12_DESCRIPTOR_HEAP_DESC targetHeapDescription{};
    targetHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    targetHeapDescription.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> targetHeap;
    D3D12_DESCRIPTOR_HEAP_DESC depthHeapDescription{};
    depthHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    depthHeapDescription.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> depthHeap;
    if (target == nullptr || depthTarget == nullptr
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
    const D3D12_CPU_DESCRIPTOR_HANDLE depthView =
        depthHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
    depthViewDescription.Format = DXGI_FORMAT_D32_FLOAT;
    depthViewDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(
        depthTarget.Get(), &depthViewDescription, depthView);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 readbackBytes = 0;
    const D3D12_RESOURCE_DESC targetDescription = target->GetDesc();
    device->GetCopyableFootprints(
        &targetDescription, 0, 1, 0, &footprint, &rows, &rowBytes,
        &readbackBytes);
    const ComPtr<ID3D12Resource> readback = createBuffer(
        device.Get(), readbackBytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (readback == nullptr) {
        frame.error = QStringLiteral("original-DXIL readback creation failed");
        return frame;
    }

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
    ID3D12DescriptorHeap *heaps[] = {shaderHeap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    for (UINT index = 0; index < constantBuffers.size(); ++index) {
        commands->SetGraphicsRootConstantBufferView(
            index, constantBuffers[index]->GetGPUVirtualAddress());
    }
    commands->SetGraphicsRootDescriptorTable(8, gpuHandleAt(0));
    commands->SetGraphicsRootDescriptorTable(10, gpuHandleAt(140));
    commands->SetGraphicsRootDescriptorTable(11, gpuHandleAt(156));
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (const DrawResources &draw : draws) {
        commands->SetGraphicsRootDescriptorTable(
            9, gpuHandleAt(draw.materialDescriptorStart));
        commands->SetPipelineState(
            draw.family == fh6::OriginalShaderSurfaceFamily::Floor
                ? floorPipeline.Get() : defaultPipeline.Get());
        commands->IASetVertexBuffers(0, 1, &draw.vertexView);
        commands->IASetIndexBuffer(&draw.indexView);
        commands->DrawIndexedInstanced(
            static_cast<UINT>(draw.geometry.indices.size()), 1, 0, 0, 0);
    }
    const D3D12_RESOURCE_BARRIER targetBarrier = transition(
        target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->ResourceBarrier(1, &targetBarrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = target.Get();
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
    if (infoQueue != nullptr) {
        frame.debugErrors = countMessages(
            infoQueue.Get(), D3D12_MESSAGE_SEVERITY_ERROR);
        frame.debugWarnings = countMessages(
            infoQueue.Get(), D3D12_MESSAGE_SEVERITY_WARNING);
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
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> shaderHeap;
    ComPtr<ID3D12DescriptorHeap> targetHeap;
    ComPtr<ID3D12DescriptorHeap> depthHeap;
    ComPtr<ID3D12Resource> depthTarget;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12Resource>, 2> backBuffers;
    std::array<ComPtr<ID3D12Resource>, 8> constantBuffers;
    std::vector<DrawResources> draws;
    std::vector<UploadedTexture> materialTextures;
    std::vector<UploadedTexture> authoredDiffuseTextures;
    UploadedTexture diffuseCube;
    QString adapterName;
    QString failure;
    HANDLE completionEvent = nullptr;
    QSize viewportSize;
    UINT shaderDescriptorStride = 0;
    UINT targetDescriptorStride = 0;
    UINT64 fenceValue = 0;

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
        if (depthTarget == nullptr) {
            failure = QStringLiteral("D3D12 viewport depth target creation failed");
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(
            depthTarget.Get(), &view,
            depthHeap->GetCPUDescriptorHandleForHeapStart());

        return true;
    }

    bool initializeScene(
        const fh6::OriginalShaderGarageScene &scene, quintptr nativeWindow,
        const QSize &size, const OriginalDx12Camera &camera) {
        viewportSize = size;
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
                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            : ComPtr<ID3D12PipelineState>{};
        defaultPipeline = rootSignature != nullptr
            ? createExactPipeline(
                  device.Get(), rootSignature.Get(), scene.defaultProgram, &failure,
                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            : ComPtr<ID3D12PipelineState>{};
        if (rootSignature == nullptr || floorPipeline == nullptr
            || defaultPipeline == nullptr) {
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
        if (FAILED(result)) {
            failure = QStringLiteral("D3D12 viewport command setup failed: %1")
                .arg(hresultText(result));
            return false;
        }

        draws.reserve(scene.draws.size());
        for (const fh6::OriginalShaderGarageDraw &source : scene.draws) {
            DrawResources draw;
            draw.geometry = prepareGeometry(source.geometry, source.placement);
            draw.family = source.family;
            draw.diffuseTexture = source.diffuseTexture;
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
            draws.push_back(std::move(draw));
        }

        const auto constants = shaderConstantData(camera, size);
        for (std::size_t index = 0; index < constantBuffers.size(); ++index) {
            constantBuffers[index] =
                createConstantBuffer(device.Get(), constants[index]);
            if (constantBuffers[index] == nullptr) {
                failure = QStringLiteral("D3D12 viewport constant upload failed");
                return false;
            }
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
        auto cpuHandleAt = [&](UINT index) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                shaderHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(index) * shaderDescriptorStride;
            return handle;
        };
        materialTextures.reserve(scene.materialTextures.size());
        for (std::size_t index = 0; index < scene.materialTextures.size(); ++index) {
            UploadedTexture texture = uploadRgba8Texture(
                device.Get(), commands.Get(), scene.materialTextures[index].image);
            if (texture.texture == nullptr) {
                failure = QStringLiteral("D3D12 viewport material upload failed");
                return false;
            }
            const D3D12_SHADER_RESOURCE_VIEW_DESC view =
                texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
            device->CreateShaderResourceView(
                texture.texture.Get(), &view,
                cpuHandleAt(kMaterialDescriptorStart + static_cast<UINT>(index)));
            materialTextures.push_back(std::move(texture));
        }
        authoredDiffuseTextures.reserve(draws.size());
        for (std::size_t drawIndex = 0; drawIndex < draws.size(); ++drawIndex) {
            DrawResources &draw = draws[drawIndex];
            draw.materialDescriptorStart = kFixedDescriptorCount
                + static_cast<UINT>(drawIndex) * kMaterialDescriptorCount;
            const D3D12_SHADER_RESOURCE_VIEW_DESC fallbackView =
                texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
            for (std::size_t index = 0; index < materialTextures.size(); ++index) {
                device->CreateShaderResourceView(
                    materialTextures[index].texture.Get(), &fallbackView,
                    cpuHandleAt(
                        draw.materialDescriptorStart + static_cast<UINT>(index)));
            }
            if (draw.diffuseTexture == nullptr || !draw.diffuseTexture->valid()) {
                continue;
            }
            UploadedTexture texture = uploadRgba8Texture(
                device.Get(), commands.Get(), draw.diffuseTexture->image);
            if (texture.texture == nullptr) {
                failure = QStringLiteral("D3D12 viewport Tokyo diffuse upload failed");
                return false;
            }
            const D3D12_SHADER_RESOURCE_VIEW_DESC view =
                texture2DView(DXGI_FORMAT_R8G8B8A8_UNORM);
            device->CreateShaderResourceView(
                texture.texture.Get(), &view,
                cpuHandleAt(draw.materialDescriptorStart + 1));
            authoredDiffuseTextures.push_back(std::move(texture));
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
        // Keep the specular-cubemap descriptor null for the Tokyo compatibility
        // material; see the offscreen path above.

        D3D12_DESCRIPTOR_HEAP_DESC targetHeapDescription{};
        targetHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        targetHeapDescription.NumDescriptors = static_cast<UINT>(backBuffers.size());
        D3D12_DESCRIPTOR_HEAP_DESC depthHeapDescription{};
        depthHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        depthHeapDescription.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(
                &targetHeapDescription, IID_PPV_ARGS(&targetHeap)))
            || FAILED(device->CreateDescriptorHeap(
                &depthHeapDescription, IID_PPV_ARGS(&depthHeap)))) {
            failure = QStringLiteral("D3D12 viewport target heap creation failed");
            return false;
        }
        targetDescriptorStride = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (!createTargets()) {
            return false;
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
        const auto constants = shaderConstantData(camera, viewportSize);
        if (!uploadBuffer(
                constantBuffers[3].Get(), constants[3].data(), constants[3].size())) {
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
        const UINT bufferIndex = swapChain->GetCurrentBackBufferIndex();
        const D3D12_RESOURCE_BARRIER beginBarrier = transition(
            backBuffers[bufferIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands->ResourceBarrier(1, &beginBarrier);
        D3D12_CPU_DESCRIPTOR_HANDLE targetView =
            targetHeap->GetCPUDescriptorHandleForHeapStart();
        targetView.ptr += static_cast<SIZE_T>(bufferIndex) * targetDescriptorStride;
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
        commands->OMSetRenderTargets(1, &targetView, FALSE, &depthView);
        commands->ClearRenderTargetView(
            targetView, kClearColor.data(), 0, nullptr);
        commands->ClearDepthStencilView(
            depthView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commands->SetGraphicsRootSignature(rootSignature.Get());
        ID3D12DescriptorHeap *heaps[] = {shaderHeap.Get()};
        commands->SetDescriptorHeaps(1, heaps);
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
        commands->SetGraphicsRootDescriptorTable(8, gpuHandleAt(0));
        commands->SetGraphicsRootDescriptorTable(10, gpuHandleAt(140));
        commands->SetGraphicsRootDescriptorTable(11, gpuHandleAt(156));
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (const DrawResources &draw : draws) {
            commands->SetGraphicsRootDescriptorTable(
                9, gpuHandleAt(draw.materialDescriptorStart));
            commands->SetPipelineState(
                draw.family == fh6::OriginalShaderSurfaceFamily::Floor
                    ? floorPipeline.Get() : defaultPipeline.Get());
            commands->IASetVertexBuffers(0, 1, &draw.vertexView);
            commands->IASetIndexBuffer(&draw.indexView);
            commands->DrawIndexedInstanced(
                static_cast<UINT>(draw.geometry.indices.size()), 1, 0, 0, 0);
        }
        const D3D12_RESOURCE_BARRIER endBarrier = transition(
            backBuffers[bufferIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        commands->ResourceBarrier(1, &endBarrier);
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
