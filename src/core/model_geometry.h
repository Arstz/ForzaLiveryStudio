#pragma once

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
    QString materialName;
    std::vector<ModelVec3> positions;
    std::vector<ModelVec3> normals;
    std::vector<std::vector<ModelVec2>> uvChannels;
    std::vector<quint32> indices;
    ModelMat4 boneTransform;
    int liveryUvChannel = 0;
};

struct CarModel {
    QString sourcePath;
    std::vector<CarMesh> meshes;
    ModelVec3 boundsMin;
    ModelVec3 boundsMax;

    long long totalVertices() const;
    long long totalIndices() const;
};

CarModel loadModelBin(const QString &path, QString *error = nullptr);

struct ModelBundle;
CarModel decodeModel(const ModelBundle &bundle, QString *error = nullptr);

std::vector<SkeletonBone> loadSkeletonBones(const ModelBundle &bundle);

} // namespace fh6
