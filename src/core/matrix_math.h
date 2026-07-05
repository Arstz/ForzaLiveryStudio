#pragma once

#include "core_types.h"

#include <array>

namespace fh6 {

double normalizeRotation(double value);
Matrix3 affine(double a, double b, double c, double d, double e, double f);
bool hasColorData(const std::array<quint8, 4> &color);
Matrix3 shapeMatrix(const FlattenedLayer &layer);
ShapeLayer decomposeLayerMatrix(const Matrix3 &matrix);

namespace detail {
Matrix3 multiply(const Matrix3 &left, const Matrix3 &right);
}

} // namespace fh6
