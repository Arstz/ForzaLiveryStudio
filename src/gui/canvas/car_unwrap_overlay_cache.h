#pragma once

#include "car_unwrap_overlay.h"

#include <QImage>
#include <QSize>

namespace gui {

class CarUnwrapOverlayCache {
public:
    void setOverlay(const CarUnwrapOverlay &overlay);
    bool empty() const;
    const QImage &image(const QTransform &worldToScreen, const QSize &size,
                        qreal devicePixelRatio, bool showUnwrap, bool showWireframe);

private:
    CarUnwrapOverlay overlay_;
    QImage image_;
    QTransform worldToScreen_;
    QSize size_;
    bool showUnwrap_ = false;
    bool showWireframe_ = false;
    qreal devicePixelRatio_ = 0.0;
};

} // namespace gui
