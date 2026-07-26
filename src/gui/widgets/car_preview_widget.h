#pragma once

#include "car_model_renderer.h"
#include "core_types.h"
#include "garage_render_settings.h"
#include "garage_environment.h"
#include "garage_ground_renderer.h"
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
    static QString postProcessShaderSelfTest();

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
    QSize physicalFramebufferSize() const;
    void fitCameraToModel();
    void resetComparisonCamera();
    void setLightDirectionCandidate(LightDirectionCandidate candidate);
    void setGameEnvironmentEnabled(bool enabled);
    void updateReferenceNote();
    void logGlCapabilities() const;
    void initializePostProcessing();
    void releasePostProcessing();
    void releaseHdrFramebuffers();
    bool ensureHdrFramebuffers(const QSize &size);
    void clearRenderTarget(bool linearColor) const;
    void renderGround(bool linearOutput);
    void renderCar(GLuint liveryTexture, bool linearOutput);
    bool renderCarHdr(GLuint liveryTexture, const QSize &size);
    void invalidateCachedLivery();

    NativeShapeRenderer shapeRenderer_;
    ShapeGeometryStore geometry_;
    CarModelRenderer carRenderer_;
    GarageGroundRenderer groundRenderer_;
    GarageRenderSettings renderSettings_;
    QOpenGLShaderProgram postProcessProgram_;
    QOpenGLVertexArrayObject postProcessVao_;
    QSize hdrFramebufferSize_;
    fh6::PaintFinishLibrary paintFinishes_;
    fh6::GarageEnvironmentResources environmentResources_;
    QString gameFolder_;
    quint64 paintFinishLoadGeneration_ = 0;
    bool environmentUploadPending_ = false;
    bool gameEnvironmentEnabled_ = true;
    QString environmentSourceLabel_ = QStringLiteral("Analytic env");
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
    GLuint hdrSceneFramebuffer_ = 0;
    GLuint hdrSceneColor_ = 0;
    GLuint hdrSceneDepth_ = 0;
    GLuint hdrResolveFramebuffer_ = 0;
    GLuint hdrResolveTexture_ = 0;
    int liveryTextureScale_ = 4;
    int hdrSampleCount_ = 0;
    int postSceneTextureLocation_ = -1;
    int postExposureLocation_ = -1;
    int postFilmicWhiteLocation_ = -1;

    QColor basePaint_ = QColor(180, 182, 190);
    bool transparentBackground_ = false;
    bool postProcessInitialized_ = false;

    QLabel *referenceNote_ = nullptr;

    QVector3D target_;
    float modelRadius_ = 1.0f;
    float yaw_;
    float pitch_;
    float distance_;
    QPoint lastMousePos_;
};

} // namespace gui
