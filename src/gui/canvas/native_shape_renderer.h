#pragma once

#include "core_types.h"
#include "raster_decals.h"
#include "scene_view.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>
#include <QtOpenGL>

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
    void render(const QVector<SceneRenderEntry> &entries,
                const ShapeGeometryStore &geometry,
                const QTransform &worldToScreen,
                const QSize &size,
                const QSet<QString> &flashingLayerIds,
                double flashHue,
                double flashStrength,
                bool clearBackground = true);

    // Renders the project's shapes into the renderer's offscreen scene texture and
    // returns its GL texture id. Unlike render(), it does not composite to the
    // default framebuffer, so the 3D car preview can sample the result as a livery
    // decal. Returns 0 if the scene could not be rendered. Call uploadGeometry() first.
    GLuint renderSceneToTexture(const fh6::Project &project,
                                const ShapeGeometryStore &geometry,
                                const QTransform &worldToScreen,
                                const QSize &size);
    // Renders multiple project fragments into one offscreen texture. Each
    // fragment is clipped to the corresponding Qt pixel rect (top-left origin)
    // before being blended into the shared texture. Empty rects draw unclipped.
    //
    // When preserveExisting is true the shared texture is NOT cleared as a whole:
    // only each fragment's clip rect is cleared and redrawn, leaving pixels outside
    // those rects (i.e. sections that did not change) intact from the previous frame.
    // Every clip rect must be non-empty in that mode, and the caller must guarantee
    // the persistent framebuffer already holds a valid full composite. This is the
    // incremental-livery path: only the edited section(s) are re-rasterized.
    GLuint renderScenesToTexture(const QVector<fh6::Project> &projects,
                                 const QVector<QRect> &clipRects,
                                 const ShapeGeometryStore &geometry,
                                 const QTransform &worldToScreen,
                                 const QSize &size,
                                 bool preserveExisting = false);

private:
    struct ShapeRange {
        int firstVertex = 0;
        int vertexCount = 0;
    };

    void setUniformRows(int row0Location, int row1Location, const QTransform &transform);
    ShapeRange fallbackRange(int shapeId, const ShapeGeometryStore &geometry);
    // Draws the project's visible shapes into sceneFbo_ with a transparent clear.
    // Shared by render() (which then composites) and renderSceneToTexture() (which
    // hands the texture off). Returns false when there is nothing to draw.
    bool drawSceneToFbo(QOpenGLFunctions *functions,
                        const fh6::Project &project,
                        const ShapeGeometryStore &geometry,
                        const QTransform &worldToScreen,
                        const QSize &size);
    bool drawProjectLayers(QOpenGLFunctions *functions,
                           const fh6::Project &project,
                           const ShapeGeometryStore &geometry,
                           const QTransform &worldToScreen,
                           const QSize &size);
    bool drawRenderEntries(QOpenGLFunctions *functions,
                           const QVector<SceneRenderEntry> &entries,
                           const ShapeGeometryStore &geometry,
                           const QTransform &worldToScreen,
                           const QSize &size);
    bool drawRasterLayer(QOpenGLFunctions *functions,
                         quint32 rasterId,
                         const std::array<quint8, 4> &color,
                         bool mask,
                         const QSize &fallbackSize,
                         const QTransform &world);
    bool ensureRasterPackLoaded();
    GLuint ensureRasterTexture(QOpenGLFunctions *functions, quint32 rasterId);
    void ensureSceneFramebuffer(const QSize &size);
    void compositeScene(QOpenGLFunctions *functions, const QSize &size, bool clearBackground);

    QOpenGLShaderProgram program_;
    QOpenGLShaderProgram rasterProgram_;
    QOpenGLShaderProgram compositeProgram_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLVertexArrayObject rasterVao_;
    QOpenGLVertexArrayObject compositeVao_;
    QOpenGLBuffer vertexBuffer_;
    QOpenGLBuffer rasterBuffer_;
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
    int rasterViewportLocation_ = -1;
    int rasterCameraRow0Location_ = -1;
    int rasterCameraRow1Location_ = -1;
    int rasterWorldRow0Location_ = -1;
    int rasterWorldRow1Location_ = -1;
    int rasterTintLocation_ = -1;
    int rasterTextureLocation_ = -1;
    int compositeTextureLocation_ = -1;
    fh6::RasterDecalPack rasterPack_;
    QString rasterPackError_;
    QHash<quint32, GLuint> rasterTextures_;
    QHash<quint32, QSize> rasterTextureSizes_;
};

} // namespace gui
