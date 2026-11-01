#pragma once

#include "contour_data.h"

#include <QString>

namespace fls::scene {

struct MeshPoint {
    QPointF incoming;
    QPointF outgoing;
    ContourPointKind kind = ContourPointKind::Soft;
    int vertex = -1;
    bool explicitHandles = false;
    double strokeWidth = 0.0;
    double endCap = 1.0;
    bool operator==(const MeshPoint &) const = default;
};

struct MeshContour {
    QString name;
    ContourData style;
    QVector<MeshPoint> points;
    QVector<QVector<MeshPoint>> cutouts;
    bool visible = true;
    double opacity = 1.0;
    bool operator==(const MeshContour &) const = default;
};

struct MeshData {
    QVector<QPointF> vertices;
    QVector<MeshContour> contours;

    ContourData contourData(int index) const;
    void appendContour(const ContourData &data, const QString &name, bool visible, double opacity);
    bool replaceContour(int index, const ContourData &data, QString *error = nullptr);
    bool connected() const;
    bool valid() const;
    bool operator==(const MeshData &) const = default;
};

} // namespace fls::scene
