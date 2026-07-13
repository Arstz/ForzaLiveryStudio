#pragma once

#include <QtCore>
#include <QtGui>

namespace gui {

struct ShapeTriangle {
    QPointF p0;
    QPointF p1;
    QPointF p2;
    double alpha0 = 1.0;
    double alpha1 = 1.0;
    double alpha2 = 1.0;
};

struct ShapeGeometry {
    int width = 128;
    int height = 128;
    QString source;
    QVector<ShapeTriangle> triangles;
};

class ShapeGeometryStore {
public:
    bool loadDefault(QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);
    const ShapeGeometry *shape(int shapeId) const;
    QSizeF shapeSize(int shapeId) const;
    QRectF shapeInkBounds(int shapeId) const;
    QVector<int> shapeIds() const;

private:
    QHash<int, ShapeGeometry> shapes_;
};

} // namespace gui
