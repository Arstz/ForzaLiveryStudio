#pragma once

#include "garage_render_settings.h"
#include "model_geometry.h"

#include <QtGui>
#include <QtOpenGL>

namespace gui {

class GarageGroundRenderer {
public:
    void initialize();
    void release();
    bool isInitialized() const { return initialized_; }
    static QString shaderSelfTest();

    void render(
        const QMatrix4x4 &view,
        const QMatrix4x4 &projection,
        const fh6::ModelVec3 &boundsMin,
        const fh6::ModelVec3 &boundsMax,
        const GarageRenderSettings::Ground &settings,
        const QVector3D &backgroundColor,
        bool linearOutput);

private:
    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    int mvpLocation_ = -1;
    int groundCenterLocation_ = -1;
    int planeHalfSizeLocation_ = -1;
    int shadowCenterLocation_ = -1;
    int shadowRadiiLocation_ = -1;
    int groundColorLocation_ = -1;
    int backgroundColorLocation_ = -1;
    int shadowOpacityLocation_ = -1;
    bool initialized_ = false;
};

} // namespace gui
