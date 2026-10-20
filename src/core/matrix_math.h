#pragma once

#include "core_types.h"

#include <QPointF>

#include <array>

namespace fls::scene {
struct Transform2D;
}

namespace fls {

double normalizeRotation(double value);
Matrix3 affine(double a, double b, double c, double d, double e, double f);
bool hasColorData(const std::array<quint8, 4> &color);
Matrix3 shapeMatrix(const FlattenedLayer &layer);
Matrix3 shapeMatrix(const VinylShape &shape);
Matrix3 vinylGroupMatrix(const VinylGroup &group);
QPointF pointBoundsCenter(const QVector<QPointF> &points);
scene::Transform2D decomposeTransform2D(const Matrix3 &matrix);
// Inverse of an affine (bottom row 0 0 1) Matrix3. Returns identity if singular.
Matrix3 invertAffine(const Matrix3 &matrix);

namespace detail {
Matrix3 multiply(const Matrix3 &left, const Matrix3 &right);
}

} // namespace fls
