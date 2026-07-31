#pragma once

#include "original_dx12_backend.h"
#include "original_shader_garage.h"

#include <QWindow>

#include <functional>
#include <memory>

namespace gui {

class OriginalDx12Viewport final : public QWindow {
public:
    explicit OriginalDx12Viewport(QWindow *parent = nullptr);

    void setScene(std::shared_ptr<const fh6::OriginalShaderGarageScene> scene);
    void clearScene();
    void setFailureCallback(std::function<void(const QString &)> callback);
    void setContextMenuCallback(std::function<void(const QPoint &)> callback);

protected:
    void exposeEvent(QExposeEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void initializeCameraOrbit();
    void updateCameraPosition();
    QSize physicalSize() const;
    void renderFrame();
    void fail(const QString &error);

    OriginalDx12ViewportRenderer renderer_;
    OriginalDx12Camera camera_;
    std::shared_ptr<const fh6::OriginalShaderGarageScene> scene_;
    std::function<void(const QString &)> failureCallback_;
    std::function<void(const QPoint &)> contextMenuCallback_;
    QPoint lastMousePosition_;
    float cameraYawRadians_ = 0.0f;
    float cameraPitchRadians_ = 0.0f;
    float cameraDistance_ = 1.0f;
};

} // namespace gui
