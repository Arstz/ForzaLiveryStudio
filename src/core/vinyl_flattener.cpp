#include "vinyl_flattener.h"

#include "matrix_math.h"

#include <variant>

namespace fls {
namespace {
using detail::multiply;

void flattenInto(const VinylGroup &node, const Matrix3 &parentMat, bool parentMask,
                 QVector<FlattenedLayer> &result) {
    const bool inheritedMask = parentMask || node.isMask;
    const Matrix3 groupMat = multiply(parentMat, vinylGroupMatrix(node));
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
            layer.isMask = inheritedMask || (shape.isMask && !hasColorData(shape.color));
            layer.groupMatrix = groupMat;
            result.push_back(layer);
        } else {
            const auto child = std::get<VinylGroupPtr>(item.value);
            flattenInto(*child, groupMat, inheritedMask, result);
        }
    }
}

} // namespace

QVector<FlattenedLayer> VinylFlattener::flattenGroup(const VinylGroup &root) const {
    QVector<FlattenedLayer> result;
    Matrix3 identity;
    flattenInto(root, identity, false, result);
    return result;
}

QVector<FlattenedLayer> flattenGroup(const VinylGroup &root) {
    return VinylFlattener{}.flattenGroup(root);
}

} // namespace fls
