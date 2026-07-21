#pragma once

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

    bool loadCar(const QString &path, QString *error = nullptr);
    bool hasModel() const;
    void clearModel();
    QImage renderThumbnail(const QSize &size);

    QImage unwrapOverlay(int liverySectionSlot = -1) const;

    void setProject(fh6::Project *project);
    void setEditorState(EditorState *state);

    QColor basePaint() const;
    void setBasePaint(const QColor &color);
    int liveryTextureScale() const;
    void setLiveryTextureScale(int scale);
    void setLoadCarTextures(bool enabled);
    void cycleDebugMode();

public Q_SLOTS:
    void markLiveryDirty();
    void markLiveryDirtyImmediate();
    void markLiverySectionsDirty(const QVector<QString> &nodeIds);
    void onProjectGeometryChanged(bool refreshPreviews, const QVector<QString> &changedNodeIds);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct CachedProjectedLiverySection {
        fh6::Project project;
        QRect clipRect;
    };

    QMatrix4x4 cameraView() const;
    QMatrix4x4 cameraProjection() const;
    QSize liveryTextureSize() const;
    QTransform liveryWorldToScreen(const QSize &textureSize) const;
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
    QString loadedCarPath_;
    bool loadCarTextures_ = false;

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
    bool transparentBackground_ = false;

    QLabel *referenceNote_ = nullptr;

    QVector3D target_;
    float modelRadius_ = 1.0f;
    float yaw_ = 0.6f;
    float pitch_ = 0.3f;
    float distance_ = 4.0f;
    QPoint lastMousePos_;
};

} // namespace gui
