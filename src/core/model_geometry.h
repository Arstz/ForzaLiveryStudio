#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace fls {

namespace car_part_types {
constexpr int RearWing = 9;
constexpr int CarBody = 2;
constexpr int FrontBumper = 34;
constexpr int RearBumper = 35;
constexpr int Hood = 36;
constexpr int SideSkirts = 37;
} // namespace car_part_types

namespace car_draw_groups {
constexpr quint32 kExterior = 1u << 0;
constexpr quint32 kCockpit = 1u << 1;
constexpr quint32 kShadow = 1u << 2;
constexpr quint32 kHood = 1u << 3;
constexpr quint32 kWindshieldReflection = 1u << 4;
constexpr quint32 kDriverlessCockpit = 1u << 5;
constexpr quint32 kWindshieldReflectionDriverless = 1u << 6;
constexpr quint32 kProxyLod = 1u << 7;
} // namespace car_draw_groups

struct ModelMaterial;

struct ModelVec2 {
    float u = 0.0f;
    float v = 0.0f;
};

struct ModelVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct TexCoordTransform {
    float offsetU = 0.0f;
    float scaleU = 1.0f;
    float offsetV = 0.0f;
    float scaleV = 1.0f;
};

// Matrices use row-vector order with translation in elements 12..14.
struct ModelMat4 {
    std::array<float, 16> m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    ModelVec3 transformPoint(const ModelVec3 &p) const;

    ModelVec3 transformVector(const ModelVec3 &v) const;
};

ModelMat4 matMul(const ModelMat4 &a, const ModelMat4 &b);

struct SkeletonBone {
    QString name;
    ModelMat4 world;
};

struct CarMesh {
    QString name;
    QString sourceModelPath;
    QString materialName;
    qint16 materialId = -1;
    std::shared_ptr<ModelMaterial> material;
    quint64 paintMaterialHash = 0;
    std::vector<ModelVec3> positions;
    std::vector<ModelVec3> normals;
    std::vector<std::vector<ModelVec2>> uvChannels;
    std::vector<quint32> indices;
    ModelMat4 boneTransform;
    std::array<TexCoordTransform, 5> texCoordTransforms;
    int liveryUvChannel = -1;
    int carPartType = -1;
    int modelInstanceId = -1;
    quint32 drawGroups = 0;
    bool stockPart = true;
    std::vector<int> partOptionIds;
};

struct CarPartOption {
    int partType = -1;
    int id = -1;
    int level = 0;
    int carBodyId = -1;
    bool parentIsStock = false;
    bool stock = false;
    QStringList modelPaths;
};

struct CarLocator {
    QString name;
    ModelVec3 position;
};

struct CarModel {
    QString sourcePath;
    std::vector<CarMesh> meshes;
    std::vector<std::vector<CarMesh>> additionalLodMeshes;
    std::vector<CarMesh> variantMeshes;
    std::vector<std::vector<CarMesh>> additionalVariantLodMeshes;
    std::vector<CarMesh> liveryProjectionMeshes;
    std::vector<CarPartOption> partOptions;
    std::vector<CarLocator> locators;
    ModelVec3 boundsMin;
    ModelVec3 boundsMax;

    int lodCount() const;
    const std::vector<CarMesh> &meshesForLod(int lodIndex) const;
    const std::vector<CarMesh> &variantMeshesForLod(int lodIndex) const;
    long long totalVertices() const;
    long long totalIndices() const;
};

CarModel loadModelBin(const QString &path, QString *error = nullptr);

struct ModelBundle;
CarModel decodeModel(const ModelBundle &bundle, QString *error = nullptr);

std::vector<SkeletonBone> loadSkeletonBones(const ModelBundle &bundle);

} // namespace fls
