#pragma once

#include "core_types.h"
#include "layer.h"
#include "shape_geometry_store.h"

#include <QTransform>
#include <QWidget>

class QPainter;

namespace gui {

struct ProjectClipboard;

class ClipboardBufferWidget final : public QWidget {
public:
    explicit ClipboardBufferWidget(QWidget *parent = nullptr);

    void setClipboard(const ProjectClipboard *clipboard);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRectF layerBounds(const fh6::scene::Shape &layer, const QTransform &parentWorld) const;
    void paintLayer(QPainter &painter, const fh6::scene::Shape &layer, const QTransform &parentWorld) const;

    const ProjectClipboard *clipboard_ = nullptr;
    ShapeGeometryStore geometry_;
    bool geometryLoaded_ = false;
};

} // namespace gui
