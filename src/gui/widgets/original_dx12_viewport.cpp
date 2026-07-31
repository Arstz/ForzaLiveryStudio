#include "original_dx12_viewport.h"

#include <QExposeEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace gui {

OriginalDx12Viewport::OriginalDx12Viewport(QWindow *parent)
    : QWindow(parent) {
    setSurfaceType(QSurface::RasterSurface);
    initializeCameraOrbit();
}

void OriginalDx12Viewport::setScene(
    std::shared_ptr<const fh6::OriginalShaderGarageScene> scene) {
    renderer_.release();
    scene_ = std::move(scene);
    if (scene_ != nullptr) {
        camera_ = originalDx12SceneCamera(*scene_);
        initializeCameraOrbit();
    }
    if (isExposed()) {
        renderFrame();
    }
}

void OriginalDx12Viewport::clearScene() {
    renderer_.release();
    scene_.reset();
}

bool OriginalDx12Viewport::updateLivery(const fh6::SwatchImage &livery) {
    if (!renderer_.ready() || !renderer_.updateLivery(livery)) {
        return false;
    }
    renderFrame();
    return true;
}

void OriginalDx12Viewport::setFailureCallback(
    std::function<void(const QString &)> callback) {
    failureCallback_ = std::move(callback);
}

void OriginalDx12Viewport::setContextMenuCallback(
    std::function<void(const QPoint &)> callback) {
    contextMenuCallback_ = std::move(callback);
}

void OriginalDx12Viewport::exposeEvent(QExposeEvent *event) {
    QWindow::exposeEvent(event);
    if (isExposed()) {
        renderFrame();
    }
}

void OriginalDx12Viewport::resizeEvent(QResizeEvent *event) {
    QWindow::resizeEvent(event);
    if (renderer_.ready() && !renderer_.resize(physicalSize())) {
        fail(renderer_.error());
        return;
    }
    renderFrame();
}

void OriginalDx12Viewport::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton && contextMenuCallback_) {
        contextMenuCallback_(mapToGlobal(event->position().toPoint()));
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        lastMousePosition_ = event->position().toPoint();
    }
    QWindow::mousePressEvent(event);
}

void OriginalDx12Viewport::mouseMoveEvent(QMouseEvent *event) {
    const bool pan = (event->buttons() & Qt::MiddleButton) != 0
        || ((event->buttons() & Qt::LeftButton) != 0
            && (event->modifiers() & Qt::ShiftModifier) != 0);
    const bool orbit = (event->buttons() & Qt::LeftButton) != 0 && !pan;
    if (!pan && !orbit) {
        QWindow::mouseMoveEvent(event);
        return;
    }
    const QPoint position = event->position().toPoint();
    const QPoint delta = position - lastMousePosition_;
    lastMousePosition_ = position;
    if (pan) {
        panOriginalDx12Camera(&camera_, delta, size());
    } else {
        constexpr float kOrbitRadiansPerPixel = 0.006f;
        constexpr float kMaximumPitch = 1.45f;
        cameraYawRadians_ -= delta.x() * kOrbitRadiansPerPixel;
        cameraPitchRadians_ = std::clamp(
            cameraPitchRadians_ + delta.y() * kOrbitRadiansPerPixel,
            -kMaximumPitch, kMaximumPitch);
        updateCameraPosition();
    }
    renderFrame();
    event->accept();
}

void OriginalDx12Viewport::wheelEvent(QWheelEvent *event) {
    constexpr float kMinimumDistance = 0.5f;
    constexpr float kWheelExponent = -0.001f;
    const float maximumDistance = std::max(20.0f, camera_.farPlane * 0.75f);
    cameraDistance_ = std::clamp(
        cameraDistance_ * std::exp(event->angleDelta().y() * kWheelExponent),
        kMinimumDistance, maximumDistance);
    updateCameraPosition();
    renderFrame();
    event->accept();
}

void OriginalDx12Viewport::initializeCameraOrbit() {
    const QVector3D offset = camera_.position - camera_.target;
    cameraDistance_ = std::max(offset.length(), 0.5f);
    cameraYawRadians_ = std::atan2(offset.x(), offset.z());
    cameraPitchRadians_ = std::asin(std::clamp(
        offset.y() / cameraDistance_, -1.0f, 1.0f));
}

void OriginalDx12Viewport::updateCameraPosition() {
    const float horizontalDistance =
        cameraDistance_ * std::cos(cameraPitchRadians_);
    camera_.position = camera_.target + QVector3D(
        horizontalDistance * std::sin(cameraYawRadians_),
        cameraDistance_ * std::sin(cameraPitchRadians_),
        horizontalDistance * std::cos(cameraYawRadians_));
}

QSize OriginalDx12Viewport::physicalSize() const {
    const qreal scale = devicePixelRatio();

    return QSize(
        std::max(1, qRound(width() * scale)),
        std::max(1, qRound(height() * scale)));
}

void OriginalDx12Viewport::renderFrame() {
    if (!isExposed() || scene_ == nullptr || size().isEmpty()) {
        return;
    }
    if (!renderer_.ready()
        && !renderer_.initialize(*scene_, winId(), physicalSize(), camera_)) {
        fail(renderer_.error());
        return;
    }
    if (!renderer_.render(camera_)) {
        fail(renderer_.error());
    }
}

void OriginalDx12Viewport::fail(const QString &error) {
    renderer_.release();
    if (failureCallback_) {
        failureCallback_(error);
    }
}

} // namespace gui
