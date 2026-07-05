#include "matrix_math.h"

#include <cmath>

namespace fh6 {

namespace detail {
Matrix3 multiply(const Matrix3 &left, const Matrix3 &right)
{
    Matrix3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r][c] = 0.0;
            for (int k = 0; k < 3; ++k) {
                out.m[r][c] += left.m[r][k] * right.m[k][c];
            }
        }
    }
    return out;
}

} // namespace detail

namespace {
constexpr double Pi = 3.14159265358979323846;
}

double normalizeRotation(double value)
{
    if (!std::isfinite(value)) {
        return 0.0;
    }
    double normalized = std::fmod(value, 360.0);
    if (normalized < 0.0) {
        normalized += 360.0;
    }
    if (std::abs(normalized - 360.0) < 1e-9) {
        return 0.0;
    }
    return normalized == 0.0 ? 0.0 : normalized;
}

Matrix3 affine(double a, double b, double c, double d, double e, double f)
{
    Matrix3 matrix;
    matrix.m[0][0] = a;
    matrix.m[0][1] = b;
    matrix.m[0][2] = c;
    matrix.m[1][0] = d;
    matrix.m[1][1] = e;
    matrix.m[1][2] = f;
    matrix.m[2][0] = 0.0;
    matrix.m[2][1] = 0.0;
    matrix.m[2][2] = 1.0;
    return matrix;
}

bool hasColorData(const std::array<quint8, 4> &color)
{
    return color[0] != color[1] || color[1] != color[2];
}

Matrix3 shapeMatrix(const FlattenedLayer &layer)
{
    const double radians = layer.rotation * Pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Matrix3 result = affine(1.0, 0.0, layer.posX, 0.0, 1.0, layer.posY);
    result = detail::multiply(result, affine(c, -s, 0.0, s, c, 0.0));
    result = detail::multiply(result, affine(1.0, layer.skew, 0.0, 0.0, 1.0, 0.0));
    result = detail::multiply(result, affine(layer.scaleX, 0.0, 0.0, 0.0, layer.scaleY, 0.0));
    return result;
}

ShapeLayer decomposeLayerMatrix(const Matrix3 &matrix)
{
    ShapeLayer layer;
    layer.x = matrix.m[0][2];
    layer.y = matrix.m[1][2];
    const double a = matrix.m[0][0];
    const double b = matrix.m[0][1];
    const double c = matrix.m[1][0];
    const double d = matrix.m[1][1];
    const double sxMagnitude = std::hypot(a, c);
    if (sxMagnitude < 1e-8) {
        layer.scaleX = 0.0;
        layer.scaleY = std::hypot(b, d);
        layer.skew = 0.0;
        layer.rotation = 0.0;
        return layer;
    }
    double rot = 0.0;
    if (a * d - b * c < 0.0) {
        layer.scaleX = -sxMagnitude;
        rot = std::atan2(-c, -a);
    } else {
        layer.scaleX = sxMagnitude;
        rot = std::atan2(c, a);
    }
    const double cosR = std::cos(rot);
    const double sinR = std::sin(rot);
    const double m01 = cosR * b + sinR * d;
    const double m11 = -sinR * b + cosR * d;
    layer.scaleY = m11;
    layer.skew = std::abs(m11) > 1e-8 ? m01 / m11 : 0.0;
    layer.rotation = normalizeRotation(rot * 180.0 / Pi);
    return layer;
}

} // namespace fh6
