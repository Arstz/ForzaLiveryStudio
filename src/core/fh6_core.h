#pragma once

#include "core_types.h"
#include "header_codec.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <array>

namespace fh6 {

double normalizeRotation(double value);
ShapeLayer shapeLayerFromJson(const QJsonObject &object);
GuideLayer guideLayerFromJson(const QJsonObject &object);
LayerGroup layerGroupFromJson(const QJsonObject &object);
Project projectFromJson(const QJsonObject &object);
QJsonObject shapeLayerToJson(const ShapeLayer &layer);
QJsonObject guideLayerToJson(const GuideLayer &layer);
QJsonObject layerGroupToJson(const LayerGroup &group);
QJsonObject projectToJson(const Project &project);

Matrix3 affine(double a, double b, double c, double d, double e, double f);
Matrix3 shapeMatrix(const FlattenedLayer &layer);
ShapeLayer decomposeLayerMatrix(const Matrix3 &matrix);
bool hasColorData(const std::array<quint8, 4> &color);
LayerData getLayerData(const QByteArray &payload);
VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload = {});
QVector<QString> validateTree(const VinylGroup &root);
QVector<FlattenedLayer> flattenGroup(const VinylGroup &root);
Project importCGroupFlat(const QString &folderOrFile);
Project importCGroupNested(const QString &folderOrFile);
Project importCLivery(const QString &folderOrFile);
QByteArray readCGroupPayload(const QString &folderOrFile);
void writeCGroupFile(const QString &path, const QByteArray &payload);
QByteArray packShape(const ShapeLayer &layer, bool maskRecord = false);
QByteArray buildFlatPayload(const Project &project);
void exportFlatProjectFolder(const Project &project, const QString &outputFolder, const QString &name = {});

} // namespace fh6
