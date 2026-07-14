#include "model_geometry.h"

#include "binary_io.h"
#include "model_bundle.h"

#include <QFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace fh6 {

using fh6::detail::readLeFloat;
using fh6::detail::readLeU16;
using fh6::detail::readLeU32;

namespace {

quint16 readLeU16Raw(const char *p, int offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p + offset));
}
quint32 readLeU32Raw(const char *p, int offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(p + offset));
}
float readLeFloatRaw(const char *p, int offset)
{
    quint32 raw = readLeU32Raw(p, offset);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(float));
    return value;
}

struct Cursor {
    const QByteArray &bytes;
    int pos = 0;
    explicit Cursor(const QByteArray &data) : bytes(data) {}

    bool has(int n) const { return pos >= 0 && pos + n <= bytes.size(); }
    quint8 u8() { quint8 v = has(1) ? static_cast<quint8>(bytes[pos]) : 0; pos += 1; return v; }
    qint8 i8() { return static_cast<qint8>(u8()); }
    quint16 u16() { quint16 v = has(2) ? readLeU16(bytes, pos) : 0; pos += 2; return v; }
    qint16 i16() { return static_cast<qint16>(u16()); }
    quint32 u32() { quint32 v = has(4) ? readLeU32(bytes, pos) : 0; pos += 4; return v; }
    qint32 i32() { return static_cast<qint32>(u32()); }
    float f32() { float v = has(4) ? readLeFloat(bytes, pos) : 0.0f; pos += 4; return v; }
    void skip(int n) { pos += n; }

    QString stringInt32()
    {
        const int n = i32();
        if (n < 0 || !has(n)) {
            pos = bytes.size();
            return {};
        }
        QString s = QString::fromLatin1(bytes.constData() + pos, n);
        pos += n;
        return s;
    }
};

struct VertexElement {
    qint16 semanticNameIndex = -1;
    qint16 semanticIndex = 0;
    qint16 inputSlot = 0;
    qint32 format = 0;
};

struct VertexLayout {
    QStringList semanticNames;
    std::vector<VertexElement> elements;

    QString semanticFor(const VertexElement &e) const
    {
        if (e.semanticNameIndex >= 0 && e.semanticNameIndex < semanticNames.size()) {
            return semanticNames[e.semanticNameIndex];
        }
        return QStringLiteral("UNKNOWN");
    }
};

struct VbUsage {
    qint32 index = 0;
    quint32 inputSlot = 0;
    quint32 stride = 0;
    quint32 offset = 0;
};

struct MeshInfo {
    QString name;
    qint16 materialId = -1;
    qint16 rigidBoneIndex = -1;
    bool is32BitIndices = true;
    qint32 indexBufferOffset = 0;
    qint32 indexBufferDrawOffset = 0;
    qint32 indexedVertexOffset = 0;
    qint32 indexCount = 0;
    qint32 vertexLayoutIndex = 0;
    std::vector<VbUsage> vertexBuffers;
    std::array<TexCoordTransform, 5> texCoordTransforms;
    std::array<float, 4> positionScale = {1, 1, 1, 1};
    std::array<float, 4> positionTranslate = {0, 0, 0, 0};
};

struct BufferData {
    quint16 stride = 0;
    QByteArray raw;
};

int formatSize(int fmt)
{
    switch (fmt) {
    case 6:  return 12; // R32G32B32_FLOAT
    case 10: return 8;  // R16G16B16A16_FLOAT
    case 13: return 8;  // R16G16B16A16_SNORM
    case 16: return 8;  // R32G32_FLOAT
    case 24: return 4;  // R10G10B10A2_UNORM
    case 28: return 4;  // R8G8B8A8_UNORM
    case 35: return 4;  // R16G16_UNORM
    case 37: return 4;  // R16G16_SNORM
    default: return 4;
    }
}

float snorm16(qint16 v) { return std::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f); }

ModelVec3 normalize(ModelVec3 v)
{
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-8f) {
        v.x /= len;
        v.y /= len;
        v.z /= len;
    }
    return v;
}

float halfToFloat(quint16 h)
{
    const int exp = (h >> 10) & 0x1F;
    const int mant = h & 0x3FF;
    const int sign = h >> 15;
    float f;
    if (exp == 0) {
        f = mant * (1.0f / (1 << 24));
    } else if (exp == 31) {
        f = mant == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    } else {
        f = (mant + 1024) * (1.0f / static_cast<float>(1 << (25 - exp)));
    }
    return sign == 0 ? f : -f;
}

struct PositionSample {
    ModelVec3 pos;
    float w = 0.0f;
};

PositionSample readPosition(const char *p, int format, const MeshInfo &mesh)
{
    PositionSample out;
    if (format == 13) { // R16G16B16A16_SNORM
        const float rx = snorm16(static_cast<qint16>(readLeU16Raw(p, 0)));
        const float ry = snorm16(static_cast<qint16>(readLeU16Raw(p, 2)));
        const float rz = snorm16(static_cast<qint16>(readLeU16Raw(p, 4)));
        out.w = snorm16(static_cast<qint16>(readLeU16Raw(p, 6)));
        const auto &s = mesh.positionScale;
        const auto &t = mesh.positionTranslate;
        if (s[0] != 0.0f || s[1] != 0.0f || s[2] != 0.0f) {
            out.pos = {rx * s[0] + t[0], ry * s[1] + t[1], rz * s[2] + t[2]};
        } else {
            out.pos = {rx, ry, rz};
        }
        return out;
    }
    if (format == 6) { // R32G32B32_FLOAT
        out.pos = {readLeFloatRaw(p, 0), readLeFloatRaw(p, 4), readLeFloatRaw(p, 8)};
        out.w = 1.0f;
        return out;
    }
    return out;
}

ModelVec3 readNormal(const char *p, int format, float wFromPos)
{
    if (format == 6) {
        return normalize({readLeFloatRaw(p, 0), readLeFloatRaw(p, 4), readLeFloatRaw(p, 8)});
    }
    if (format == 37) { // R16G16_SNORM; X reconstructed from position W
        return normalize({wFromPos,
                          snorm16(static_cast<qint16>(readLeU16Raw(p, 0))),
                          snorm16(static_cast<qint16>(readLeU16Raw(p, 2)))});
    }
    if (format == 10) { // R16G16B16A16_FLOAT
        return normalize({halfToFloat(readLeU16Raw(p, 0)),
                          halfToFloat(readLeU16Raw(p, 2)),
                          halfToFloat(readLeU16Raw(p, 4))});
    }
    if (format == 24) { // R10G10B10A2_UNORM
        const quint32 v = readLeU32Raw(p, 0);
        return normalize({((v >> 0) & 0x3FF) / 1023.0f * 2.0f - 1.0f,
                          ((v >> 10) & 0x3FF) / 1023.0f * 2.0f - 1.0f,
                          ((v >> 20) & 0x3FF) / 1023.0f * 2.0f - 1.0f});
    }
    return {0.0f, 1.0f, 0.0f};
}

ModelVec2 readUv(const char *p, int format)
{
    if (format == 35) { // R16G16_UNORM
        return {readLeU16Raw(p, 0) / 65535.0f, readLeU16Raw(p, 2) / 65535.0f};
    }
    if (format == 16) { // R32G32_FLOAT
        return {readLeFloatRaw(p, 0), readLeFloatRaw(p, 4)};
    }
    return {0.0f, 0.0f};
}

VertexLayout decodeLayout(const BundleBlobRecord &blob)
{
    Cursor c(blob.data);
    VertexLayout layout;
    const quint16 semanticCount = c.u16();
    for (quint16 i = 0; i < semanticCount; ++i) {
        layout.semanticNames << c.stringInt32();
    }
    const quint16 elementCount = c.u16();
    layout.elements.reserve(elementCount);
    for (quint16 i = 0; i < elementCount; ++i) {
        VertexElement e;
        e.semanticNameIndex = c.i16();
        e.semanticIndex = c.i16();
        e.inputSlot = c.i16();
        c.i16(); // inputSlotClass
        e.format = c.i32();
        c.i32(); // alignedByteOffset
        c.i32(); // instanceDataStepRate
        layout.elements.push_back(e);
    }
    return layout;
}

MeshInfo decodeMesh(const BundleBlobRecord &blob)
{
    Cursor c(blob.data);
    MeshInfo mesh;
    mesh.name = blob.name;

    const int materialGroupCount = blob.isAtLeastVersion(1, 13) ? c.i32() : 1;
    const bool v19 = blob.isAtLeastVersion(1, 9);
    for (int i = 0; i < materialGroupCount; ++i) {
        if (v19) {
            const qint16 g0 = c.i16();
            const qint16 g1 = c.i16();
            c.i16();
            c.i16();
            if (i == 0) {
                mesh.materialId = g1; // game uses group[1] as the material index
                (void)g0;
            }
        } else {
            const qint16 g = c.i16();
            if (i == 0) {
                mesh.materialId = g;
            }
        }
    }

    mesh.rigidBoneIndex = c.i16();
    c.u16(); // LODFlags
    c.u8();  // LODLevel1
    c.u8();  // LODLevel2
    c.u16(); // bucket flags
    c.u8();  // bucket order

    if (blob.isAtLeastVersion(1, 2)) {
        c.u8(); // skinning element count
        if (blob.isAtLeastVersion(1, 10)) {
            c.u32();
        } else {
            c.u8(); // morph target count
        }
    }
    if (blob.isAtLeastVersion(1, 3)) {
        c.u8(); // isMorphDamage
    }

    mesh.is32BitIndices = c.u8() != 0;
    c.u16(); // topology
    c.i32(); // index buffer index
    mesh.indexBufferOffset = c.i32();
    mesh.indexBufferDrawOffset = c.i32();
    mesh.indexedVertexOffset = c.i32();
    mesh.indexCount = c.i32();
    c.i32(); // prim count

    if (blob.isAtLeastVersion(1, 6)) {
        c.f32(); // ACMR
        c.u32(); // referenced vertex count
    }
    if (blob.isAtLeastVersion(1, 11)) {
        const quint32 refCount = c.u32();
        for (quint32 i = 0; i < refCount; ++i) {
            c.u32();
        }
    }

    mesh.vertexLayoutIndex = c.i32();
    const int vbCount = c.i32();
    mesh.vertexBuffers.reserve(std::max(0, vbCount));
    for (int i = 0; i < vbCount; ++i) {
        VbUsage u;
        u.index = c.i32();
        u.inputSlot = c.u32();
        u.stride = c.u32();
        u.offset = c.u32();
        if (blob.isAtLeastVersion(1, 12)) {
            c.u32(); // reserved
        }
        mesh.vertexBuffers.push_back(u);
    }

    if (blob.isAtLeastVersion(1, 4)) {
        c.i32(); // morph data buffer index
        c.i32(); // skinning data buffer index
    }
    const int cbCount = c.i32();
    for (int i = 0; i < cbCount; ++i) {
        c.i32();
    }
    if (blob.isAtLeastVersion(1, 1)) {
        c.u32(); // source mesh index
    }
    if (blob.isAtLeastVersion(1, 5)) {
        for (TexCoordTransform &transform : mesh.texCoordTransforms) {
            transform.offsetU = c.f32();
            transform.scaleU = c.f32();
            transform.offsetV = c.f32();
            transform.scaleV = c.f32();
        }
    }
    if (blob.isAtLeastVersion(1, 8)) {
        for (int i = 0; i < 4; ++i) {
            mesh.positionScale[i] = c.f32();
        }
        for (int i = 0; i < 4; ++i) {
            mesh.positionTranslate[i] = c.f32();
        }
    }
    return mesh;
}

BufferData decodeBuffer(const BundleBlobRecord &blob)
{
    Cursor c(blob.data);
    const qint32 length = c.i32();
    const qint32 size = c.i32();
    const quint16 stride = c.u16();
    if (blob.versionMajor >= 1) {
        c.u8();  // sub-element count
        c.u8();  // padding
        c.i32(); // DXGI format
    } else {
        c.skip(2);
    }
    BufferData out;
    out.stride = stride;
    const int dataLen = size > 0 ? size : (length * stride);
    out.raw = blob.data.mid(c.pos, dataLen);
    return out;
}

std::vector<SkeletonBone> decodeSkeletonBones(const BundleBlobRecord &blob)
{
    Cursor c(blob.data);
    const quint16 boneCount = c.u16();

    std::vector<SkeletonBone> bones(boneCount);
    std::vector<qint16> parents(boneCount, -1);
    std::vector<ModelMat4> local(boneCount);
    for (quint16 i = 0; i < boneCount; ++i) {
        bones[i].name = c.stringInt32();
        parents[i] = c.i16();
        c.i16(); // first child
        c.i16(); // next sibling
        for (int k = 0; k < 16; ++k) {
            local[i].m[k] = c.f32();
        }
    }

    for (quint16 i = 0; i < boneCount; ++i) {
        ModelMat4 m = local[i];
        const qint16 parent = parents[i];
        if (parent >= 0 && parent < static_cast<qint16>(i)) {
            m = matMul(m, bones[parent].world);
        }
        bones[i].world = m;
    }
    return bones;
}

} // namespace

ModelMat4 matMul(const ModelMat4 &a, const ModelMat4 &b)
{
    ModelMat4 out;
    for (int r = 0; r < 4; ++r) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[r * 4 + k] * b.m[k * 4 + col];
            }
            out.m[r * 4 + col] = sum;
        }
    }
    return out;
}

std::vector<SkeletonBone> loadSkeletonBones(const ModelBundle &bundle)
{
    const auto skeletonBlobs = bundle.blobsWithTag(bundle_tags::Skeleton);
    if (skeletonBlobs.empty()) {
        return {};
    }
    return decodeSkeletonBones(*skeletonBlobs.front());
}

ModelVec3 ModelMat4::transformPoint(const ModelVec3 &p) const
{
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12],
            p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}

ModelVec3 ModelMat4::transformVector(const ModelVec3 &v) const
{
    return {v.x * m[0] + v.y * m[4] + v.z * m[8],
            v.x * m[1] + v.y * m[5] + v.z * m[9],
            v.x * m[2] + v.y * m[6] + v.z * m[10]};
}

long long CarModel::totalVertices() const
{
    long long n = 0;
    for (const CarMesh &mesh : meshes) {
        n += static_cast<long long>(mesh.positions.size());
    }
    return n;
}

long long CarModel::totalIndices() const
{
    long long n = 0;
    for (const CarMesh &mesh : meshes) {
        n += static_cast<long long>(mesh.indices.size());
    }
    return n;
}

CarModel decodeModel(const ModelBundle &bundle, QString *error)
{
    CarModel model;

    const auto meshBlobs = bundle.blobsWithTag(bundle_tags::Mesh);
    const auto layoutBlobs = bundle.blobsWithTag(bundle_tags::VertexLayout);
    const auto indexBlobs = bundle.blobsWithTag(bundle_tags::IndexBuffer);
    const auto vertexBlobs = bundle.blobsWithTag(bundle_tags::VertexBuffer);
    const auto skeletonBlobs = bundle.blobsWithTag(bundle_tags::Skeleton);

    if (meshBlobs.empty() || indexBlobs.empty()) {
        if (error) {
            *error = QStringLiteral("modelbin has no decodable geometry (missing Mesh/IndexBuffer blobs)");
        }
        return model;
    }

    const BufferData indexBuffer = decodeBuffer(*indexBlobs.front());

    std::vector<BufferData> vertexBuffers;
    std::unordered_map<quint32, int> bufferById;
    std::unordered_map<int, int> bufferByBlobIndex;
    vertexBuffers.reserve(vertexBlobs.size());
    for (int blobIdx = 0; blobIdx < static_cast<int>(bundle.blobs.size()); ++blobIdx) {
        const BundleBlobRecord &blob = bundle.blobs[blobIdx];
        if (blob.tag != bundle_tags::VertexBuffer) {
            continue;
        }
        const int slot = static_cast<int>(vertexBuffers.size());
        vertexBuffers.push_back(decodeBuffer(blob));
        bufferByBlobIndex[blobIdx] = slot;
        if (blob.id.has_value()) {
            bufferById[*blob.id] = slot;
        }
    }

    std::vector<VertexLayout> layouts;
    std::unordered_map<quint32, int> layoutById;
    layouts.reserve(layoutBlobs.size());
    for (const BundleBlobRecord *blob : layoutBlobs) {
        const int slot = static_cast<int>(layouts.size());
        layouts.push_back(decodeLayout(*blob));
        if (blob->id.has_value()) {
            layoutById[*blob->id] = slot;
        }
    }

    std::vector<SkeletonBone> boneWorld;
    if (!skeletonBlobs.empty()) {
        boneWorld = decodeSkeletonBones(*skeletonBlobs.front());
    }

    std::unordered_map<quint32, QString> materialNames;
    for (const BundleBlobRecord &blob : bundle.blobs) {
        if (blob.id.has_value() && !blob.name.isEmpty()) {
            materialNames.emplace(*blob.id, blob.name);
        }
    }

    auto resolveBuffer = [&](int usageIndex) -> const BufferData * {
        if (auto it = bufferById.find(static_cast<quint32>(usageIndex)); it != bufferById.end()) {
            return &vertexBuffers[it->second];
        }
        if (auto it = bufferByBlobIndex.find(usageIndex); it != bufferByBlobIndex.end()) {
            return &vertexBuffers[it->second];
        }
        return nullptr;
    };

    float minX = std::numeric_limits<float>::max();
    float minY = minX;
    float minZ = minX;
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = maxX;
    float maxZ = maxX;

    for (int mi = 0; mi < static_cast<int>(meshBlobs.size()); ++mi) {
        const MeshInfo info = decodeMesh(*meshBlobs[mi]);

        if (info.name.compare(QStringLiteral("shadow"), Qt::CaseInsensitive) == 0) {
            continue;
        }

        const VertexLayout *layout = nullptr;
        if (auto it = layoutById.find(static_cast<quint32>(info.vertexLayoutIndex)); it != layoutById.end()) {
            layout = &layouts[it->second];
        } else if (info.vertexLayoutIndex >= 0 && info.vertexLayoutIndex < static_cast<int>(layouts.size())) {
            layout = &layouts[info.vertexLayoutIndex];
        } else if (!layouts.empty()) {
            layout = &layouts[0];
        }
        if (layout == nullptr) {
            continue;
        }

        const int indexStride = info.is32BitIndices ? 4 : 2;
        const long long startIndexOffset =
            static_cast<long long>(info.indexBufferOffset) + static_cast<long long>(info.indexBufferDrawOffset) * indexStride;
        if (info.indexCount <= 0
            || startIndexOffset < 0
            || startIndexOffset + static_cast<long long>(info.indexCount) * indexStride > indexBuffer.raw.size()) {
            continue;
        }

        std::vector<quint32> indices(info.indexCount);
        quint32 minIndex = std::numeric_limits<quint32>::max();
        quint32 maxIndex = 0;
        const char *indexData = indexBuffer.raw.constData();
        for (int i = 0; i < info.indexCount; ++i) {
            const long long addr = startIndexOffset + static_cast<long long>(i) * indexStride;
            const quint32 idx = indexStride == 4 ? readLeU32Raw(indexData, static_cast<int>(addr))
                                                 : readLeU16Raw(indexData, static_cast<int>(addr));
            indices[i] = idx;
            minIndex = std::min(minIndex, idx);
            maxIndex = std::max(maxIndex, idx);
        }
        if (maxIndex < minIndex) {
            continue;
        }
        const int vertexCount = static_cast<int>(maxIndex - minIndex) + 1;
        if (vertexCount <= 0) {
            continue;
        }

        CarMesh out;
        out.name = info.name;
        if (auto it = materialNames.find(static_cast<quint32>(info.materialId)); it != materialNames.end()) {
            out.materialName = it->second;
        }
        out.positions.assign(vertexCount, {});
        out.normals.assign(vertexCount, {0.0f, 1.0f, 0.0f});
        out.indices.resize(indices.size());
        for (size_t i = 0; i < indices.size(); ++i) {
            out.indices[i] = indices[i] - minIndex;
        }
        if (info.rigidBoneIndex >= 0 && info.rigidBoneIndex < static_cast<int>(boneWorld.size())) {
            out.boneTransform = boneWorld[info.rigidBoneIndex].world;
        }

        std::vector<float> posW(vertexCount, 0.0f);

        std::unordered_map<int, int> slotOffset;
        struct Elem {
            const VertexElement *desc;
            int offsetInStride;
        };
        std::vector<Elem> elements;
        elements.reserve(layout->elements.size());
        for (const VertexElement &e : layout->elements) {
            int &off = slotOffset[e.inputSlot];
            elements.push_back({&e, off});
            off += formatSize(e.format);
        }

        auto readAttribute = [&](const Elem &elem, auto &&perVertex) {
            const VertexElement &e = *elem.desc;
            const VbUsage *usage = nullptr;
            for (const VbUsage &u : info.vertexBuffers) {
                if (static_cast<qint16>(u.inputSlot) == e.inputSlot) {
                    usage = &u;
                    break;
                }
            }
            if (usage == nullptr) {
                return;
            }
            const BufferData *vb = resolveBuffer(usage->index);
            if (vb == nullptr || vb->raw.isEmpty()) {
                return;
            }
            long long stride = vb->stride > 0 ? vb->stride : usage->stride;
            if (stride == 0) {
                stride = 28;
            }
            const char *data = vb->raw.constData();
            const int bufferLen = vb->raw.size();
            const int fmtSize = formatSize(e.format);
            for (int i = 0; i < vertexCount; ++i) {
                const long long vertexId = static_cast<long long>(minIndex) + i + info.indexedVertexOffset;
                const long long addr = usage->offset + vertexId * stride + elem.offsetInStride;
                if (addr >= 0 && addr + fmtSize <= bufferLen) {
                    perVertex(data + addr, i);
                }
            }
        };

        for (const Elem &elem : elements) {
            if (layout->semanticFor(*elem.desc) != QStringLiteral("POSITION")) {
                continue;
            }
            readAttribute(elem, [&](const char *p, int i) {
                const PositionSample s = readPosition(p, elem.desc->format, info);
                out.positions[i] = s.pos;
                posW[i] = s.w;
            });
        }

        int maxChannel = -1;
        for (const Elem &elem : elements) {
            const QString semantic = layout->semanticFor(*elem.desc);
            if (semantic == QStringLiteral("POSITION")) {
                continue;
            }
            if (semantic == QStringLiteral("NORMAL")) {
                readAttribute(elem, [&](const char *p, int i) {
                    out.normals[i] = readNormal(p, elem.desc->format, posW[i]);
                });
            } else if (semantic == QStringLiteral("TEXCOORD")) {
                const int channel = elem.desc->semanticIndex;
                if (channel < 0) {
                    continue;
                }
                maxChannel = std::max(maxChannel, channel);
                if (static_cast<int>(out.uvChannels.size()) <= channel) {
                    out.uvChannels.resize(channel + 1);
                }
                out.uvChannels[channel].assign(vertexCount, {});
                readAttribute(elem, [&](const char *p, int i) {
                    out.uvChannels[channel][i] = readUv(p, elem.desc->format);
                });
            }
        }

        out.texCoordTransforms = info.texCoordTransforms;
        out.liveryUvChannel = (out.uvChannels.size() > 3 && !out.uvChannels[3].empty()) ? 3 : -1;

        for (const ModelVec3 &p : out.positions) {
            const ModelVec3 w = out.boneTransform.transformPoint(p);
            minX = std::min(minX, w.x);
            minY = std::min(minY, w.y);
            minZ = std::min(minZ, w.z);
            maxX = std::max(maxX, w.x);
            maxY = std::max(maxY, w.y);
            maxZ = std::max(maxZ, w.z);
        }

        model.meshes.push_back(std::move(out));
    }

    if (model.meshes.empty()) {
        if (error) {
            *error = QStringLiteral("modelbin decoded to zero meshes");
        }
        return model;
    }

    model.boundsMin = {minX, minY, minZ};
    model.boundsMax = {maxX, maxY, maxZ};
    return model;
}

CarModel loadModelBin(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open %1").arg(path);
        }
        return {};
    }
    const QByteArray bytes = file.readAll();
    file.close();

    try {
        const ModelBundle bundle = parseModelBundle(bytes);
        CarModel model = decodeModel(bundle, error);
        model.sourcePath = path;
        return model;
    } catch (const std::exception &ex) {
        if (error) {
            *error = QString::fromUtf8(ex.what());
        }
        return {};
    }
}

} // namespace fh6
