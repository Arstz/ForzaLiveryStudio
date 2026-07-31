#pragma once

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector3D>

#include <memory>

namespace fh6 {
struct OriginalShaderGarageScene;
struct SwatchImage;
}

namespace gui {

struct OriginalDx12BackendStatus {
    bool available = false;
    bool exactPipelinesCreated = false;
    QString adapter;
    QString error;
};

struct OriginalDx12FrameResult {
    QImage image;
    QString adapter;
    QString error;
    quint64 changedPixels = 0;
    quint64 nonFiniteComponents = 0;
    float finiteMinimum = 0.0f;
    float finiteMaximum = 0.0f;
    int debugErrors = 0;
    int debugWarnings = 0;

    bool valid() const {
        return !image.isNull() && error.isEmpty() && changedPixels > 0
            && nonFiniteComponents == 0 && debugErrors == 0;
    }
};

struct OriginalDx12Camera {
    QVector3D position = QVector3D(3.223908f, 1.084031f, 2.486952f);
    QVector3D target = QVector3D(0.992176f, 0.242747f, -0.724220f);
    QVector3D up = QVector3D(0.0f, 1.0f, 0.0f);
    float verticalFovDegrees = 36.749542f;
    float nearPlane = 0.05f;
    float farPlane = 100.0f;

    bool valid() const {
        return !position.isNull() && !up.isNull()
            && !qFuzzyCompare(position, target)
            && verticalFovDegrees > 1.0f && verticalFovDegrees < 170.0f
            && nearPlane > 0.0f && farPlane > nearPlane;
    }
};

OriginalDx12BackendStatus probeOriginalDx12Backend(
    const fh6::OriginalShaderGarageScene &scene);

OriginalDx12Camera originalDx12SceneCamera(
    const fh6::OriginalShaderGarageScene &scene);

void panOriginalDx12Camera(
    OriginalDx12Camera *camera, const QPointF &pixelDelta,
    const QSize &viewportSize);

OriginalDx12FrameResult renderOriginalDx12GarageFrame(
    const fh6::OriginalShaderGarageScene &scene, const QSize &size,
    const OriginalDx12Camera &camera);

OriginalDx12FrameResult renderOriginalDx12GarageFrame(
    const fh6::OriginalShaderGarageScene &scene, const QSize &size);

class OriginalDx12ViewportRenderer {
public:
    OriginalDx12ViewportRenderer();
    ~OriginalDx12ViewportRenderer();

    OriginalDx12ViewportRenderer(const OriginalDx12ViewportRenderer &) = delete;
    OriginalDx12ViewportRenderer &operator=(
        const OriginalDx12ViewportRenderer &) = delete;

    bool initialize(
        const fh6::OriginalShaderGarageScene &scene, quintptr nativeWindow,
        const QSize &size, const OriginalDx12Camera &camera);
    bool resize(const QSize &size);
    bool render(const OriginalDx12Camera &camera);
    bool updateLivery(const fh6::SwatchImage &livery);
    void release();

    bool ready() const;
    QString adapter() const;
    QString error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gui
