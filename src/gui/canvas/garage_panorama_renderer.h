#pragma once

#include <QtGui>
#include <QtOpenGL>

namespace fh6 {
struct SwatchTexture;
}

namespace gui {

class GaragePanoramaRenderer {
public:
    void initialize();
    void release();
    bool isInitialized() const { return initialized_; }
    static QString shaderSelfTest();

    bool setPanorama(const fh6::SwatchTexture &texture, QString *error = nullptr);
    void clearPanorama();
    bool hasPanorama() const { return panoramaTexture_ != 0; }

    void render(
        const QMatrix4x4 &view,
        const QMatrix4x4 &projection,
        float sphericalPower,
        float frameScale);

private:
    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    GLuint panoramaTexture_ = 0;
    int panoramaLocation_ = -1;
    int inverseViewProjectionLocation_ = -1;
    int sphericalPowerLocation_ = -1;
    int frameScaleLocation_ = -1;
    bool initialized_ = false;
};

} // namespace gui
