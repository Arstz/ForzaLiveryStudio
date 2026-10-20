#pragma once

#include "core_types.h"

#include <optional>

namespace fls::scene {
class Shape;
}

namespace fls::detail {

struct VinylShapePlacement {
    std::array<quint8, 4> color = {255, 255, 255, 255};
    quint16 shapeId = 0;
    bool mask = false;
    double rotation = 0.0;
    double x = 0.0;
    double y = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double skew = 0.0;
};

VinylShapePlacement vinylShapePlacement(
    const scene::Shape &shape, const std::optional<Matrix3> &worldAdjustment = std::nullopt);
void appendVinylShapeRecord(QByteArray &out, const VinylShapePlacement &shape,
                            double offsetX, double offsetY, quint8 shapeMarker,
                            std::optional<quint8> state = std::nullopt);
QByteArray standaloneVinylPrefix(const QByteArray &sourcePrefix,
                                 std::optional<quint8> generation = std::nullopt);
QByteArray vinylSiblingGroupMarker(int previousGroupDepth);
void appendVinylGroupShapeTransition(QByteArray &out, int previousGroupDepth);

} // namespace fls::detail
