#pragma once

#include "car_model_renderer.h"
#include "core_types.h"
#include "livery_masks.h"
#include "manufacturer_colors.h"
#include "native_shape_renderer.h"
#include "shape_geometry_store.h"
#include "model_geometry.h"

#include <QtCore>
#include <QtGui>
#include <QtOpenGLWidgets>

#include <functional>
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
    using CarLoadCallback = std::function<void(bool, const QString &)>;

    explicit CarPreviewWidget(QWidget *parent = nullptr);
    ~CarPreviewWidget() override;

    void loadCarAsync(const QString &path, CarLoadCallback callback = {});
    void cancelCarLoad();
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
    void setGameFolder(const QString &folder);
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
    void invalidateCachedLivery();

    NativeShapeRenderer shapeRenderer_;
    ShapeGeometryStore geometry_;
    CarModelRenderer carRenderer_;
    fh6::PaintFinishLibrary paintFinishes_;
    QString gameFolder_;
    quint64 paintFinishLoadGeneration_ = 0;
    bool geometryLoaded_ = false;

    fh6::Project *project_ = nullptr;
    EditorState *state_ = nullptr;

    fh6::CarModel model_;
    fh6::ManufacturerColorPalette manufacturerColors_;
    bool modelUploadPending_ = false;
    std::unique_ptr<QTemporaryDir> extractedCarDir_;
    QString loadedCarPath_;
    quint64 carLoadGeneration_ = 0;
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
