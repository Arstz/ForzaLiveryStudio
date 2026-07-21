#pragma once

#include <QPointF>
#include <QRectF>
#include <QTransform>

#include <algorithm>

namespace gui {

class CanvasCamera {
public:
    void reset() {
        pan_ = {};
        zoom_ = 1.0;
    }

    QRectF viewBounds() const { return viewBounds_; }
    bool hasViewBounds() const { return viewBounds_.isValid() && !viewBounds_.isEmpty(); }
    void setViewBounds(const QRectF &bounds) { viewBounds_ = bounds; }

    double zoom() const { return zoom_; }
    void setZoom(double zoom) { zoom_ = zoom; }
    QPointF pan() const { return pan_; }
    void setPan(const QPointF &pan) { pan_ = pan; }
    void adjustPan(const QPointF &delta) { pan_ += delta; }

    double baseScale() const { return baseScale_; }
    double scale() const { return baseScale_ * zoom_; }
    double worldPerPixel() const { return 1.0 / std::max(scale(), kScaleEpsilon); }

    void recompute(const QPointF &viewportOrigin, double viewportWidth, double viewportHeight) {
        const double paddedWidth = std::max(viewBounds_.width() * kViewFitPadding, 1.0);
        const double paddedHeight = std::max(viewBounds_.height() * kViewFitPadding, 1.0);
        const QPointF center = viewBounds_.center();
        baseScale_ = std::min(viewportWidth / paddedWidth, viewportHeight / paddedHeight);

        worldToScreen_.reset();
        worldToScreen_.translate(viewportOrigin.x() + viewportWidth * 0.5 + pan_.x(),
                                 viewportOrigin.y() + viewportHeight * 0.5 + pan_.y());
        worldToScreen_.scale(baseScale_ * zoom_, -baseScale_ * zoom_);
        worldToScreen_.translate(-center.x(), -center.y());
        screenToWorld_ = worldToScreen_.inverted();
    }

    const QTransform &matrix() const { return worldToScreen_; }
    QPointF worldToScreen(const QPointF &point) const { return worldToScreen_.map(point); }
    QPointF screenToWorld(const QPointF &point) const { return screenToWorld_.map(point); }

private:
    static constexpr double kViewFitPadding = 1.16;
    static constexpr double kScaleEpsilon = 1e-8;

    QTransform worldToScreen_;
    QTransform screenToWorld_;
    QRectF viewBounds_;
    QPointF pan_;
    double baseScale_ = 1.0;
    double zoom_ = 1.0;
};

} // namespace gui
