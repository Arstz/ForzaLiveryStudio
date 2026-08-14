#pragma once

#include <QtCore>
#include <QtGui>

namespace gui {

struct SvgVectorObject {
    QPainterPath path;
    QColor color;
};

struct SvgVectorDocument {
    QVector<SvgVectorObject> objects;
    QString fallbackReason;

    bool supportsObjectSelection() const {
        return fallbackReason.isEmpty();
    }
};

struct SvgVectorObjectHit {
    QPainterPath path;
    QColor color;
    int objectIndex = -1;

    bool valid() const {
        return objectIndex >= 0 && !path.isEmpty();
    }
};

SvgVectorDocument extractSvgVectorObjects(const QByteArray &svg,
                                          const QSize &logicalSize);

bool useSvgObjectSelection(const SvgVectorDocument &document,
                           bool rasterFallbackRequested);

SvgVectorObjectHit svgVectorObjectAt(const SvgVectorDocument &document,
                                     const QPointF &point);

} // namespace gui
