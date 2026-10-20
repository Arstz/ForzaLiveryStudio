#include "vinyl_encoding.h"

#include "binary_io.h"
#include "layer.h"
#include "matrix_math.h"

#include <algorithm>
#include <stdexcept>

namespace fls::detail {
namespace {

constexpr int kPrefixBytes = 29;
constexpr int kGenerationOffset = 12;
constexpr int kTransformOffset = 13;

} // namespace

VinylShapePlacement vinylShapePlacement(
    const scene::Shape &shape, const std::optional<Matrix3> &worldAdjustment) {
    const Matrix3 worldMatrix = worldAdjustment
        ? multiply(*worldAdjustment, shape.worldMatrix()) : shape.worldMatrix();
    const scene::Transform2D world = decomposeTransform2D(worldMatrix);
    VinylShapePlacement out;
    out.color = shape.color;
    out.shapeId = shape.shapeId;
    out.mask = shape.mask;
    out.rotation = world.rotation;
    out.x = world.x;
    out.y = world.y;
    out.scaleX = world.scaleX;
    out.scaleY = world.scaleY;
    out.skew = world.skew;

    return out;
}

void appendVinylShapeRecord(QByteArray &out, const VinylShapePlacement &shape,
                            double offsetX, double offsetY, quint8 shapeMarker,
                            std::optional<quint8> state) {
    if (state) {
        out.append(static_cast<char>(*state));
    }
    out.append(static_cast<char>(shapeMarker));
    appendLeU16(out, shape.shapeId);
    appendLeFloat(out, static_cast<float>(normalizeRotation(shape.rotation)));
    appendLeFloat(out, static_cast<float>(shape.x - offsetX));
    appendLeFloat(out, static_cast<float>(shape.y - offsetY));
    appendLeFloat(out, static_cast<float>(shape.scaleX));
    appendLeFloat(out, static_cast<float>(shape.scaleY));
    appendLeFloat(out, static_cast<float>(shape.skew));
    for (quint8 channel : shape.color) {
        out.append(static_cast<char>(channel));
    }
}

QByteArray standaloneVinylPrefix(const QByteArray &sourcePrefix,
                                 std::optional<quint8> generation) {
    QByteArray prefix = sourcePrefix.isEmpty() ? QByteArray(kPrefixBytes, '\0')
                                              : sourcePrefix.left(kPrefixBytes);
    QByteArray transform;

    if (prefix.size() < kPrefixBytes) {
        throw std::runtime_error("source decompressed prefix is shorter than 0x1d bytes");
    }
    if (sourcePrefix.isEmpty()) {
        prefix.replace(0, 4, QByteArrayLiteral("gyvl"));
        prefix[4] = '\x01';
        prefix[kGenerationOffset] = '\x03';
    }
    if (generation) {
        prefix[kGenerationOffset] = static_cast<char>(*generation);
    }
    appendLeFloat(transform, 0.0f);
    appendLeFloat(transform, 0.0f);
    appendLeFloat(transform, 1.0f);
    appendLeFloat(transform, 0.0f);
    prefix.replace(kTransformOffset, transform.size(), transform);

    return prefix;
}

QByteArray vinylSiblingGroupMarker(int previousGroupDepth) {
    QByteArray out(1, '\0');
    out.append(QByteArray(std::max(1, previousGroupDepth), '\x01'));
    out.append('\x03');

    return out;
}

void appendVinylGroupShapeTransition(QByteArray &out, int previousGroupDepth) {
    out.append('\0');
    out.append(QByteArray(std::max(0, previousGroupDepth - 1), '\x01'));
}

} // namespace fls::detail
