#include "car_unwrap_overlay_cache.h"

#include <QPainter>

#include <cmath>

namespace gui {

void CarUnwrapOverlayCache::setOverlay(const CarUnwrapOverlay &overlay) {
    if (overlay_ == overlay) {
        return;
    }
    overlay_ = overlay;
    image_ = {};
}

bool CarUnwrapOverlayCache::empty() const {
    return overlay_.empty();
}

const QImage &CarUnwrapOverlayCache::image(
    const QTransform &worldToScreen, const QSize &size, qreal devicePixelRatio,
    bool showUnwrap, bool showWireframe) {
    if (empty() || size.isEmpty() || (!showUnwrap && !showWireframe)) {
        image_ = {};

        return image_;
    }
    if (!image_.isNull() && worldToScreen_ == worldToScreen && size_ == size
        && devicePixelRatio_ == devicePixelRatio && showUnwrap_ == showUnwrap
        && showWireframe_ == showWireframe) {
        return image_;
    }
    worldToScreen_ = worldToScreen;
    size_ = size;
    devicePixelRatio_ = devicePixelRatio;
    showUnwrap_ = showUnwrap;
    showWireframe_ = showWireframe;
    image_ = QImage(QSize(static_cast<int>(std::ceil(size.width() * devicePixelRatio)),
                          static_cast<int>(std::ceil(size.height() * devicePixelRatio))),
                    QImage::Format_ARGB32_Premultiplied);
    image_.setDevicePixelRatio(devicePixelRatio);
    image_.fill(Qt::transparent);
    QPainter painter(&image_);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setTransform(worldToScreen);
    if (showUnwrap) {
        static const std::array<QColor, fls::kLiverySideCount> kSideColors = {
            QColor(230, 60, 60), QColor(60, 200, 60), QColor(70, 120, 240),
            QColor(230, 220, 60), QColor(220, 80, 220), QColor(60, 210, 210),
            QColor(255, 130, 60), QColor(120, 255, 140), QColor(120, 190, 255),
            QColor(255, 235, 120), QColor(255, 140, 255),
        };
        painter.setPen(Qt::NoPen);
        painter.setOpacity(0.45);
        for (int sideIndex = 0; sideIndex < fls::kLiverySideCount; ++sideIndex) {
            const CarUnwrapSide &side = overlay_.sides[sideIndex];
            if (side.valid()) {
                painter.setBrush(kSideColors[sideIndex]);
                painter.drawPath(side.path);
            }
        }
    }
    if (showWireframe) {
        QPen wireframePen(Qt::white);
        wireframePen.setWidthF(1.0);
        wireframePen.setCosmetic(true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(wireframePen);
        painter.setOpacity(1.0);
        for (const CarUnwrapSide &side : overlay_.sides) {
            if (!side.valid() || side.wireframe.isEmpty()) {
                continue;
            }
            painter.save();
            painter.setClipPath(side.path, Qt::IntersectClip);
            painter.drawPath(side.wireframe);
            painter.restore();
        }
    }
    painter.end();

    return image_;
}

} // namespace gui
