#pragma once

#include "livery_masks.h"
#include "model_geometry.h"

#include <QtGui>
#include <QtOpenGL>

#include <memory>
#include <vector>

namespace gui {

class CarModelRenderer {
public:
    CarModelRenderer();
    ~CarModelRenderer();

    void initialize();
    void release();
    bool isInitialized() const;
    bool hasModel() const;

    void uploadModel(const fh6::CarModel &model);
    void clearModel();

    void setLivery(const fh6::CarModel &model, const fh6::LiveryMaskSet &masks);
    void setPaintTextureRegions(const QVector<QVector4D> &regions);
    void clearLivery();

    void setDebugMode(int mode) { debugMode_ = mode; }
    int debugMode() const { return debugMode_; }

    void render(const QMatrix4x4 &view,
                const QMatrix4x4 &projection,
                GLuint liveryTexture,
                const QColor &basePaint);

private:
    struct MeshBuffers {
        QOpenGLVertexArrayObject vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        int indexCount = 0;
        bool applyLivery = false;
        bool hasDirectLiveryUv = false;
        int allowedSides = 0;
        bool translucent = false;
        float alpha = 1.0f;
        QVector3D center;
        QMatrix4x4 model;
    };

    static constexpr int kLiverySideCount = fh6::kLiverySideCount;

    QOpenGLShaderProgram program_;
    std::vector<std::unique_ptr<MeshBuffers>> meshes_;
    bool initialized_ = false;

    GLuint sideMaskArray_ = 0;
    int sideCount_ = 0;
    int debugMode_ = 0;
    QVector<QVector4D> sideAxis_;
    QVector<QVector2D> sideEMin_;
    QVector<QVector2D> sideEMax_;
    QVector<QVector4D> sideRegion_;
    QVector<QVector4D> defaultSidePaintRegion_;
    QVector<QVector4D> sidePaintRegion_;
    QVector<QVector3D> sideFacing_;

    int mvpLocation_ = -1;
    int modelLocation_ = -1;
    int liveryTexLocation_ = -1;
    int basePaintLocation_ = -1;
    int hasLiveryLocation_ = -1;
    int useDirectUvLocation_ = -1;
    int sideMasksLocation_ = -1;
    int sideCountLocation_ = -1;
    int sideAxisLocation_ = -1;
    int sideEMinLocation_ = -1;
    int sideEMaxLocation_ = -1;
    int sideRegionLocation_ = -1;
    int sidePaintRegionLocation_ = -1;
    int sideFacingLocation_ = -1;
    int debugModeLocation_ = -1;
    int allowedSidesLocation_ = -1;
    int materialAlphaLocation_ = -1;
};

} // namespace gui
