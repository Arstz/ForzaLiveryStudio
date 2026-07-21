#pragma once

#include "core_types.h"
#include "layer.h"
#include "shape_geometry_store.h"
#include "theme_manager.h"

#include <QTransform>
#include <QWidget>

class QPainter;

namespace gui {

struct ProjectClipboard;

class ClipboardBufferWidget final : public QWidget {
public:
    explicit ClipboardBufferWidget(QWidget *parent = nullptr);

    void setClipboard(const ProjectClipboard *clipboard);
    void setPreviewBackground(const PreviewBackground &background, UiTheme theme);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void paintLayer(QPainter &painter, const fh6::scene::Shape &layer, const QTransform &world) const;

    const ProjectClipboard *clipboard_ = nullptr;
    ShapeGeometryStore geometry_;
    PreviewBackground previewBackground_;
    UiTheme theme_ = UiTheme::Dark;
    bool geometryLoaded_ = false;
};

} // namespace gui
