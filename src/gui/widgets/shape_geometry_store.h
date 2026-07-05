#pragma once

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

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
    // Bounding box of the shape's actual inked triangles, in local coordinates
    // about the shape origin. Unlike shapeSize(), this ignores the declared
    // square canvas (font glyphs all declare 220x220 with transparent corner
    // markers that the loader already drops), so it reflects the glyph's real
    // extent. Falls back to a centred square of the declared size when the shape
    // has no triangles or is unknown.
    QRectF shapeInkBounds(int shapeId) const;
    QVector<int> shapeIds() const;

private:
    QHash<int, ShapeGeometry> shapes_;
};

} // namespace gui
