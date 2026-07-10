#include "vinyl_flattener.h"

#include "matrix_math.h"

#include <cmath>
#include <variant>

namespace fh6 {
namespace {
using detail::multiply;

constexpr double Pi = 3.14159265358979323846;

void flattenInto(const VinylGroup &node, const Matrix3 &parentMat, bool parentMask,
                 bool rootCall, QVector<FlattenedLayer> &result)
{
    const bool inheritedMask = parentMask || node.isMask;
    const double radians = node.rot * Pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const Matrix3 nodeMat = affine(c * node.sx, -s * node.sy, node.px,
                                   s * node.sx, c * node.sy, node.py);
    const Matrix3 groupMat = multiply(parentMat, nodeMat);
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            const VinylShape &shape = std::get<VinylShape>(item.value);
            FlattenedLayer layer;
            layer.shapeId = shape.shapeId;
            layer.raster = shape.isLogo;
            layer.rasterId = shape.rasterId;
            layer.sourceLogoId = shape.logoId;
            layer.rotation = shape.rotation;
            layer.posX = shape.posX;
            layer.posY = shape.posY;
            layer.scaleX = shape.scaleX;
            layer.scaleY = shape.scaleY;
            layer.skew = shape.skew;
            layer.color = shape.color;
            layer.absPos = shape.absPos;
            layer.marker = shape.marker;
            layer.flags = shape.flags;
            layer.isMask = inheritedMask || shape.isMask;
            if (hasColorData(shape.color)) {
                layer.isMask = false;
            }
            layer.groupMatrix = groupMat;
            result.push_back(layer);
        } else {
            const auto child = std::get<VinylGroupPtr>(item.value);
            const Matrix3 &childParent = (rootCall && child->rootTransformExempt) ? parentMat : groupMat;
            flattenInto(*child, childParent, inheritedMask, false, result);
        }
    }
}

} // namespace

QVector<FlattenedLayer> VinylFlattener::flattenGroup(const VinylGroup &root) const
{
    QVector<FlattenedLayer> result;
    Matrix3 identity;
    flattenInto(root, identity, false, true, result);
    return result;
}

QVector<FlattenedLayer> flattenGroup(const VinylGroup &root)
{
    return VinylFlattener{}.flattenGroup(root);
}

} // namespace fh6
