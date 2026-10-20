#pragma once

#include "compact_fit_gpu_rank.h"

#include <QtGui>

namespace gui::compact::gpu {

inline void appendGeometryPiece(Geometry &result, const QVector<QPolygonF> &polygons) {
    const int firstLoop = static_cast<int>(result.loops.size());

    for (const QPolygonF &polygon : polygons) {
        const int firstPoint = static_cast<int>(result.points.size());
        for (const QPointF &point : polygon) {
            result.points.push_back({static_cast<float>(point.x()),
                static_cast<float>(point.y())});
        }
        result.loops.push_back({firstPoint, static_cast<int>(polygon.size())});
    }
    result.pieces.push_back({firstLoop,
        static_cast<int>(result.loops.size()) - firstLoop});
}

inline Geometry gpuGeometry(const QVector<QPolygonF> &polygons) {
    Geometry result;
    appendGeometryPiece(result, polygons);

    return result;
}

template<typename Piece>
Geometry gpuGeometry(const QVector<Piece> &pieces) {
    Geometry result;
    for (const Piece &piece : pieces) {
        appendGeometryPiece(result, piece.polygons);
    }

    return result;
}

inline Affine gpuAffine(const QTransform &transform) {
    return {static_cast<float>(transform.m11()),
        static_cast<float>(transform.m12()),
        static_cast<float>(transform.m21()),
        static_cast<float>(transform.m22()),
        static_cast<float>(transform.dx()),
        static_cast<float>(transform.dy())};
}

} // namespace gui::compact::gpu
