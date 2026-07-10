#pragma once

// Decodes a parsed `.modelbin` bundle into renderable meshes. Ports the geometry
// path of ForzaTechStudio's ModelImporter.ExtractMesh: it resolves each mesh's
// vertex layout + buffers, dequantizes positions/normals/UVs per DXGI format, and
// re-indexes to a 0-based triangle list. Materials, shaders, textures, LOD/morph
// selection, and animation are intentionally out of scope.
//
// Math is kept on plain POD types so `fh6_core` stays Core-only (no Qt Gui). Bone
// matrices are stored in .NET Matrix4x4 layout (row-major, row-vector convention:
// v' = v * M, translation in elements [12..14]); the GUI renderer transposes when
// loading into a QMatrix4x4.

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace fh6 {

struct ModelVec2 {
    float u = 0.0f;
    float v = 0.0f;
};

struct ModelVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 4x4 matrix in .NET Matrix4x4 order (see file header for convention).
struct ModelMat4 {
    std::array<float, 16> m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // Row-vector transform of a point (applies translation), matching
    // System.Numerics.Vector3.Transform.
    ModelVec3 transformPoint(const ModelVec3 &p) const;

    // Row-vector transform of a direction (no translation), for normals.
    ModelVec3 transformVector(const ModelVec3 &v) const;
};

// Matrix product in .NET row-vector convention: v * (a*b) == (v*a) * b.
ModelMat4 matMul(const ModelMat4 &a, const ModelMat4 &b);

// One skeleton bone with its accumulated world matrix.
struct SkeletonBone {
    QString name;
    ModelMat4 world;
};

// One draw-call mesh, decoded to mesh-local space (position scale/translate applied,
// bone transform NOT baked — it is carried separately so the renderer can compose it
// with a per-instance transform on the GPU).
struct CarMesh {
    QString name;
    QString materialName; // resolved from the mesh's material id, when available
    std::vector<ModelVec3> positions; // mesh-local, pre-bone
    std::vector<ModelVec3> normals;
    // UV sets indexed by channel id; an empty inner vector means the channel is absent.
    std::vector<std::vector<ModelVec2>> uvChannels;
    std::vector<quint32> indices;     // 0-based triangle list
    ModelMat4 boneTransform;          // accumulated bone world matrix (identity if none)
    int liveryUvChannel = 0;          // UV channel to sample the livery decal from
};

struct CarModel {
    QString sourcePath;
    std::vector<CarMesh> meshes;
    ModelVec3 boundsMin;
    ModelVec3 boundsMax;

    long long totalVertices() const;
    long long totalIndices() const;
};

// Loads and decodes a `.modelbin`. On failure returns an empty model and, if
// `error` is non-null, sets a human-readable message.
CarModel loadModelBin(const QString &path, QString *error = nullptr);

// Decodes an already-parsed bundle (shared by loadModelBin and, later, carbin
// assembly which loads referenced modelbins from disk).
struct ModelBundle;
CarModel decodeModel(const ModelBundle &bundle, QString *error = nullptr);

// Reads the bundle's skeleton (name + accumulated world matrix per bone). Empty if
// the bundle has no Skel blob. Used by carbin assembly to place parts on car bones.
std::vector<SkeletonBone> loadSkeletonBones(const ModelBundle &bundle);

} // namespace fh6
