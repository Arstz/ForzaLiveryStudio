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
Project projectFromJson(const QJsonObject &object);
QJsonObject projectToJson(const Project &project);

Matrix3 affine(double a, double b, double c, double d, double e, double f);
Matrix3 shapeMatrix(const FlattenedLayer &layer);
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

} // namespace fh6
