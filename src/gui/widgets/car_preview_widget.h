#pragma once

#include "car_model_renderer.h"
#include "core_types.h"
#include "garage_camera_presets.h"
#include "garage_render_settings.h"
#include "garage_environment.h"
#include "garage_ground_renderer.h"
#include "garage_lut.h"
#include "garage_panorama_renderer.h"
#include "livery_masks.h"
#include "manufacturer_colors.h"
#include "native_shape_renderer.h"
#include "shape_geometry_store.h"
#include "model_geometry.h"

#include <QtCore>
#include <QtGui>
#include <QtOpenGLWidgets>

#include <array>
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
    GarageCameraFrame currentCameraFrame() const;
    void fitCameraToModel();
    void resetComparisonCamera();
    void beginCameraTransition(GarageCameraPreset preset);
    void finishCameraTransition();
    void stopCameraTransitionForManualControl();
    void syncOrbitToCameraFrame(const GarageCameraFrame &frame);
    void setLightDirectionCandidate(LightDirectionCandidate candidate);
    void setGameEnvironmentEnabled(bool enabled);
    void setPanoramaBackgroundEnabled(bool enabled);
    void updateReferenceNote();
    void logGlCapabilities() const;
    void initializePostProcessing();
    void releasePostProcessing();
    void releaseHdrFramebuffers();
    bool uploadColorLut();
    bool ensureHdrFramebuffers(const QSize &size);
    bool renderBloomExtract(const QSize &size);
    bool renderBloomBlur(const QSize &size, int sourceIndex, int targetIndex, bool horizontal);
    bool renderBloomComposite(const QSize &size);
    bool renderColorGrade(const QSize &size);
    bool renderDisplayOutput(const QSize &size);
    void clearRenderTarget(bool linearColor) const;
    void renderBackground(bool linearOutput);
    void renderGround(bool linearOutput);
    void renderCar(GLuint liveryTexture, bool linearOutput);
    bool renderCarHdr(GLuint liveryTexture, const QSize &size);
    void invalidateCachedLivery();

    NativeShapeRenderer shapeRenderer_;
    ShapeGeometryStore geometry_;
    CarModelRenderer carRenderer_;
    GarageGroundRenderer groundRenderer_;
    GaragePanoramaRenderer panoramaRenderer_;
    GarageRenderSettings renderSettings_;
    QOpenGLShaderProgram bloomExtractProgram_;
    QOpenGLShaderProgram bloomBlurProgram_;
    QOpenGLShaderProgram bloomCompositeProgram_;
    QOpenGLShaderProgram colorGradeProgram_;
    QOpenGLShaderProgram postProcessProgram_;
    QOpenGLVertexArrayObject postProcessVao_;
    QSize hdrFramebufferSize_;
    GarageCameraFrame cameraTransitionFrom_;
    GarageCameraFrame cameraTransitionTo_;
    GarageCameraFrame activeCameraFrame_;
    fh6::PaintFinishLibrary paintFinishes_;
    fh6::GarageEnvironmentResources environmentResources_;
    fh6::GarageColorLut colorLut_;
    QString colorLutError_;
    QString gameFolder_;
    quint64 paintFinishLoadGeneration_ = 0;
    bool environmentUploadPending_ = false;
    bool panoramaUploadPending_ = false;
    bool gameEnvironmentEnabled_ = true;
    bool panoramaBackgroundEnabled_ = true;
    QString environmentSourceLabel_ = QStringLiteral("Analytic env");
    QString backgroundSourceLabel_ = QStringLiteral("Analytic background");
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
    GLuint hdrCompositeFramebuffer_ = 0;
    GLuint hdrCompositeTexture_ = 0;
    GLuint hdrGradeFramebuffer_ = 0;
    GLuint hdrGradeTexture_ = 0;
    std::array<GLuint, 2> bloomFramebuffers_ = {};
    std::array<GLuint, 2> bloomTextures_ = {};
    GLuint colorLutTexture_ = 0;
    int liveryTextureScale_ = 4;
    int hdrSampleCount_ = 0;
    int bloomSceneTextureLocation_ = -1;
    int bloomExposureLocation_ = -1;
    int bloomCutoffLocation_ = -1;
    int bloomBlurTextureLocation_ = -1;
    int bloomBlurTexelStepLocation_ = -1;
    int bloomCompositeSceneLocation_ = -1;
    int bloomCompositeTextureLocation_ = -1;
    int bloomCompositeExposureLocation_ = -1;
    int bloomCompositeScaleLocation_ = -1;
    int bloomCompositeEnabledLocation_ = -1;
    int colorGradeSceneLocation_ = -1;
    int colorGradeLutLocation_ = -1;
    int colorGradeDimensionLocation_ = -1;
    int colorGradeScaleLocation_ = -1;
    int colorGradeEnabledLocation_ = -1;
    int postSceneTextureLocation_ = -1;
    int postFilmicWhiteLocation_ = -1;

    QColor basePaint_ = QColor(180, 182, 190);
    bool transparentBackground_ = false;
    bool postProcessInitialized_ = false;
    bool colorLutUploadPending_ = false;

    QLabel *referenceNote_ = nullptr;
    QVariantAnimation *cameraTransitionAnimation_ = nullptr;

    QVector3D modelCenter_;
    QVector3D target_;
    float modelRadius_ = 1.0f;
    float yaw_;
    float pitch_;
    float distance_;
    float fovDegrees_;
    bool activeCameraFrameEnabled_ = false;
    QPoint lastMousePos_;
};

} // namespace gui
