#pragma once

// Dockable 3D preview of a car model with the current vinyl scene applied to its
// paint surface. Self-contained in its own GL context: it owns a NativeShapeRenderer
// (+ ShapeGeometryStore) to rasterize the vinyl scene into a texture, and a
// CarModelRenderer to draw the model by runtime projection through the car's
// livery masks. The livery texture is only re-rendered when the project changes.

#include "car_model_renderer.h"
#include "core_types.h"
#include "livery_masks.h"
#include "native_shape_renderer.h"
#include "shape_geometry_store.h"
#include "model_geometry.h"

#include <QtCore>
#include <QtGui>
#include <QtOpenGLWidgets>

#include <memory>

class QTemporaryDir;
class QLabel;

namespace fh6 {
struct Project;
}

namespace gui {

class EditorState;

class CarPreviewWidget final : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit CarPreviewWidget(QWidget *parent = nullptr);
    ~CarPreviewWidget() override;

    // Loads and displays a car model. Returns false (and sets *error) on failure.
    bool loadCar(const QString &path, QString *error = nullptr);
    bool hasModel() const;
    // Drops the currently loaded car model (and its livery masks), leaving an empty
    // preview. Safe to call with no model loaded.
    void clearModel();

    // Builds a canvas-space RGBA overlay of the rendered livery coverage masks
    // (each side tinted) for the 2D editor to draw as a UV-unwrap guide.
    // Returns a null image when no livery masks are available.
    QImage unwrapOverlay(int liverySectionSlot = -1) const;

    void setProject(fh6::Project *project);
    void setEditorState(EditorState *state);

    QColor basePaint() const;
    void setBasePaint(const QColor &color);
    int liveryTextureScale() const;
    void setLiveryTextureScale(int scale);

public Q_SLOTS:
    // Marks the livery texture stale so it is re-rasterized on the next paint.
    void markLiveryDirty();
    void markLiveryDirtyImmediate();
    void markLiverySectionsDirty(const QVector<QString> &nodeIds);
    // Geometry-change notification carrying the edited nodes: scopes the livery
    // re-raster to their section(s) when known, else falls back to a full rebuild.
    void onProjectGeometryChanged(bool refreshPreviews, const QVector<QString> &changedNodeIds);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct CachedProjectedLiverySection {
        fh6::Project project;
        QRect clipRect;
    };

    QMatrix4x4 cameraView() const;
    QMatrix4x4 cameraProjection() const;
    QSize liveryTextureSize() const;
    QTransform liveryWorldToScreen() const;
    void fitCameraToModel();

    NativeShapeRenderer shapeRenderer_;
    ShapeGeometryStore geometry_;
    CarModelRenderer carRenderer_;
    bool geometryLoaded_ = false;

    fh6::Project *project_ = nullptr;
    EditorState *state_ = nullptr;

    fh6::CarModel model_;
    bool modelUploadPending_ = false;
    std::unique_ptr<QTemporaryDir> extractedCarDir_;

    // Per-side livery masks discovered next to the loaded car (LiveryMasks/).
    fh6::LiveryMaskSet liveryMasks_;
    QString liveryMasksDir_;
    bool liveryMasksPending_ = false;

    bool liveryDirty_ = true;
    bool liveLiveryFullDirty_ = false;
    QSet<QString> dirtySectionIds_;
    QHash<QString, CachedProjectedLiverySection> projectedSectionCache_;
    GLuint liveryTexture_ = 0;
    int liveryTextureScale_ = 4;

    QColor basePaint_ = QColor(180, 182, 190);

    // Non-interactive "reference only" disclaimer pinned to the top-left corner.
    QLabel *referenceNote_ = nullptr;

    // Orbit camera state.
    QVector3D target_;
    float modelRadius_ = 1.0f;
    float yaw_ = 0.6f;
    float pitch_ = 0.3f;
    float distance_ = 4.0f;
    QPoint lastMousePos_;
};

} // namespace gui
