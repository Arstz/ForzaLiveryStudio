#pragma once

#include <QColor>
#include <QPointF>
#include <QVector>

#include <optional>

namespace fls::scene {

enum class ContourPointKind { Hard, Soft };

struct ContourPoint {
    QPointF position;
    ContourPointKind kind = ContourPointKind::Soft;
    QPointF incoming;
    QPointF outgoing;
    bool explicitHandles = false;
    bool operator==(const ContourPoint &) const = default;
};

struct ContourData {
    QVector<ContourPoint> points;
    QVector<QVector<ContourPoint>> cutouts;
    std::optional<QColor> fillColor;
    bool closed = false;
    bool cutoutClosed = true;
    bool fillMask = false;
    bool operator==(const ContourData &) const = default;
};

} // namespace fls::scene
