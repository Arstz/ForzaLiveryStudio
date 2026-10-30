#pragma once

#include <QLineF>
#include <QHash>
#include <QPainterPath>
#include <QPolygonF>
#include <QTransform>
#include <QVector>

namespace gui {

struct GeometrySnapOutline {
    QVector<QPolygonF> contours;
};

GeometrySnapOutline geometrySnapOutline(const QPainterPath &worldPath, double pixelsPerWorld);

class GeometrySnapTargets {
public:
    void setOutlines(const QVector<GeometrySnapOutline> &outlines);
    QVector<QLineF> nearbySegments(const QRectF &bounds) const;
    const QVector<double> &horizontalCoordinates() const { return horizontalCoordinates_; }
    const QVector<double> &verticalCoordinates() const { return verticalCoordinates_; }
    QLineF alignmentLine(double coordinate, bool horizontal) const;

private:
    struct Segment {
        QLineF line;
        QRectF bounds;
    };
    struct Node {
        QRectF bounds;
        int first = 0;
        int count = 0;
        int left = -1;
        int right = -1;
    };

    int buildNode(int first, int count);
    void collectSegments(int nodeIndex, const QRectF &bounds, QVector<QLineF> &result) const;

    QVector<Segment> segments_;
    QVector<Node> nodes_;
    QVector<double> horizontalCoordinates_;
    QVector<double> verticalCoordinates_;
    QHash<double, QLineF> horizontalLines_;
    QHash<double, QLineF> verticalLines_;
};

struct GeometrySnapMotion {
    QTransform transform;
    QTransform firstStep;
    QTransform secondStep;
    QPointF controlPoint;
    bool twoDimensions = true;
};

enum class GeometrySnapIndicatorKind { Alignment, Contact };

struct GeometrySnapIndicator {
    QLineF line;
    QPointF point;
    GeometrySnapIndicatorKind kind = GeometrySnapIndicatorKind::Alignment;
    bool infiniteLine = false;
};

struct GeometrySnapResult {
    QPointF adjustment;
    QVector<GeometrySnapIndicator> indicators;
};

GeometrySnapResult snapGeometry(const QVector<GeometrySnapOutline> &outlines,
                              const GeometrySnapTargets *targets,
                              const GeometrySnapMotion &motion,
                              const QVector<double> &verticalGuidelines,
                              const QVector<double> &horizontalGuidelines,
                              double tolerance);

} // namespace gui
