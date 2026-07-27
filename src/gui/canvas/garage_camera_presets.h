#pragma once

#include <QQuaternion>
#include <QVector3D>

#include <array>

namespace gui {

enum class GarageCameraPreset {
    PaintColor,
    LiveryFront,
    LiveryBack,
    LiveryGlassFront,
    LiveryGlassBack,
};

struct GarageCameraDefinition {
    const char *sourceName = nullptr;
    QVector3D gamePosition;
    QVector3D gameForward;
    float fovDegrees = 0.0f;
};

struct GarageCameraFrame {
    QVector3D position;
    QQuaternion orientation;
    float fovDegrees = 0.0f;
};

const std::array<GarageCameraDefinition, 5> &garageCameraDefinitions();
const GarageCameraDefinition &garageCameraDefinition(GarageCameraPreset preset);
GarageCameraFrame garageCameraFrame(GarageCameraPreset preset);
GarageCameraFrame interpolateCameraFrame(
    const GarageCameraFrame &from, const GarageCameraFrame &to, float amount);
QVector3D garageCameraForward(const GarageCameraFrame &frame);
QVector3D garageCameraUp(const GarageCameraFrame &frame);
float boundedCameraEase(float amount);

} // namespace gui
