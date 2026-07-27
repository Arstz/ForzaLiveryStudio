#include "garage_camera_presets.h"

#include <algorithm>
#include <cstddef>

namespace gui {
namespace {

const std::array<GarageCameraDefinition, 5> kCameraDefinitions = {{
    {
        "CarColorSel",
        QVector3D(3.223908f, 1.084031f, 2.486952f),
        QVector3D(-0.557933f, -0.210321f, -0.802793f),
        36.749542f,
    },
    {
        "LiveryLayerSelect_Front",
        QVector3D(0.268128f, 0.557300f, 3.843994f),
        QVector3D(0.017241f, -0.050620f, -0.998569f),
        27.249580f,
    },
    {
        "LiveryLayerSelect_Back",
        QVector3D(-0.267885f, 1.091830f, -3.554683f),
        QVector3D(0.005278f, -0.138550f, 0.990341f),
        27.249580f,
    },
    {
        "LiveryLayerSelect_Glass_Front",
        QVector3D(0.261110f, 1.317958f, 2.521606f),
        QVector3D(-0.013304f, -0.239036f, -0.970920f),
        27.249580f,
    },
    {
        "LiveryLayerSelect_Glass_Back",
        QVector3D(-0.227208f, 1.373127f, -3.260316f),
        QVector3D(-0.023538f, -0.233239f, 0.972135f),
        27.249580f,
    },
}};

QVector3D gameToRenderer(const QVector3D &value) {
    return QVector3D(-value.x(), value.y(), value.z());
}

std::size_t definitionIndex(GarageCameraPreset preset) {
    switch (preset) {
    case GarageCameraPreset::PaintColor:
        return 0;
    case GarageCameraPreset::LiveryFront:
        return 1;
    case GarageCameraPreset::LiveryBack:
        return 2;
    case GarageCameraPreset::LiveryGlassFront:
        return 3;
    case GarageCameraPreset::LiveryGlassBack:
        return 4;
    }

    return 0;
}

} // namespace

const std::array<GarageCameraDefinition, 5> &garageCameraDefinitions() {
    return kCameraDefinitions;
}

const GarageCameraDefinition &garageCameraDefinition(GarageCameraPreset preset) {
    return kCameraDefinitions[definitionIndex(preset)];
}

GarageCameraFrame garageCameraFrame(GarageCameraPreset preset) {
    const GarageCameraDefinition &definition = garageCameraDefinition(preset);
    const QVector3D forward = gameToRenderer(definition.gameForward).normalized();

    return {
        gameToRenderer(definition.gamePosition),
        QQuaternion::fromDirection(forward, QVector3D(0.0f, 1.0f, 0.0f)).normalized(),
        definition.fovDegrees,
    };
}

GarageCameraFrame interpolateCameraFrame(
    const GarageCameraFrame &from, const GarageCameraFrame &to, float amount) {
    const float boundedAmount = std::clamp(amount, 0.0f, 1.0f);

    return {
        from.position * (1.0f - boundedAmount) + to.position * boundedAmount,
        QQuaternion::slerp(from.orientation, to.orientation, boundedAmount).normalized(),
        from.fovDegrees * (1.0f - boundedAmount) + to.fovDegrees * boundedAmount,
    };
}

QVector3D garageCameraForward(const GarageCameraFrame &frame) {
    return frame.orientation.rotatedVector(QVector3D(0.0f, 0.0f, 1.0f)).normalized();
}

QVector3D garageCameraUp(const GarageCameraFrame &frame) {
    return frame.orientation.rotatedVector(QVector3D(0.0f, 1.0f, 0.0f)).normalized();
}

float boundedCameraEase(float amount) {
    const float boundedAmount = std::clamp(amount, 0.0f, 1.0f);

    return boundedAmount * boundedAmount * (3.0f - 2.0f * boundedAmount);
}

} // namespace gui
