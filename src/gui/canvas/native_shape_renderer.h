#pragma once

#include "core_types.h"
#include "shape_geometry_store.h"

#include <QHash>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QSet>
#include <QSize>
#include <QTransform>

#include <memory>

class QOpenGLFunctions;

namespace gui {

class NativeShapeRenderer {
public:
    NativeShapeRenderer();
    ~NativeShapeRenderer();

    void initialize();
    void release();
    bool isInitialized() const;
    void uploadGeometry(const ShapeGeometryStore &geometry);
    // When clearBackground is false the default framebuffer is left intact (its existing
    // contents are composited under the shapes), so the caller can paint a background and
    // guide layers behind the shapes first.
    void render(const fh6::Project &project,
                const ShapeGeometryStore &geometry,
                const QTransform &worldToScreen,
                const QSize &size,
                const QSet<QString> &flashingLayerIds,
                double flashHue,
                double flashStrength,
                bool clearBackground = true);

private:
    struct ShapeRange {
        int firstVertex = 0;
        int vertexCount = 0;
    };

    void setUniformRows(int row0Location, int row1Location, const QTransform &transform);
    ShapeRange fallbackRange(int shapeId, const ShapeGeometryStore &geometry);
    void ensureSceneFramebuffer(const QSize &size);
    void compositeScene(QOpenGLFunctions *functions, const QSize &size, bool clearBackground);

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram compositeProgram_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLVertexArrayObject compositeVao_;
    QOpenGLBuffer vertexBuffer_;
    QOpenGLBuffer compositeBuffer_;
    QHash<int, ShapeRange> ranges_;
    QVector<float> vertices_;
    std::unique_ptr<QOpenGLFramebufferObject> sceneFbo_;
    QSize sceneFboSize_;
    bool initialized_ = false;
    bool geometryUploaded_ = false;

    // Uniform locations resolved once at link time so per-layer draws avoid a
    // glGetUniformLocation string lookup for every uniform, every frame.
    int viewportLocation_ = -1;
    int cameraRow0Location_ = -1;
    int cameraRow1Location_ = -1;
    int worldRow0Location_ = -1;
    int worldRow1Location_ = -1;
    int tintLocation_ = -1;
    int compositeTextureLocation_ = -1;
};

} // namespace gui
