#pragma once

#include "core_types.h"

#include <array>

namespace fh6::scene {
struct Transform2D;
}

namespace fh6 {

double normalizeRotation(double value);
Matrix3 affine(double a, double b, double c, double d, double e, double f);
bool hasColorData(const std::array<quint8, 4> &color);
Matrix3 shapeMatrix(const FlattenedLayer &layer);
scene::Transform2D decomposeTransform2D(const Matrix3 &matrix);
// Inverse of an affine (bottom row 0 0 1) Matrix3. Returns identity if singular.
Matrix3 invertAffine(const Matrix3 &matrix);

namespace detail {
Matrix3 multiply(const Matrix3 &left, const Matrix3 &right);
}

} // namespace fh6
