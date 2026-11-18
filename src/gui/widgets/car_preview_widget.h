#pragma once

#include "car_model_renderer.h"
#include "car_paint_catalog.h"
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
#include <atomic>
#include <memory>
#include <optional>

class QTemporaryDir;
class QAbstractButton;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace fls {
struct Project;
}

namespace gui {

class EditorState;
class PartRadialSelector;
struct WheelPreviewState;

QJsonObject profileCarLoading(const QString &path, bool loadTextures, int repetitions, bool verifyUnwrap);

class CarPreviewWidget final : public QOpenGLWidget {
    Q_OBJECT
public:
    using CarLoadCallback = std::function<void(bool, const QString &)>;

    explicit CarPreviewWidget(QWidget *parent = nullptr);
    ~CarPreviewWidget() override;

    void loadCarAsync(const QString &path, CarLoadCallback callback = {}, const QString &game = {});
    void cancelCarLoad();
    bool hasModel() const;
    void clearModel();
    QImage renderThumbnail(const QSize &size);

    CarUnwrapOverlay unwrapOverlay(int liverySectionSlot = -1) const;
    std::optional<QTransform> liverySectionToAtlasTransform(int liverySectionSlot) const;
    void setSectionWireframeVisible(bool visible);
    void setCarUnwrapSection(int liverySectionSlot);

    void setProject(fls::Project *project);
    void setEditorState(EditorState *state);

    QColor basePaint() const;
    void setBasePaint(const QColor &color);
    int liveryTextureScale() const;
    void setLiveryTextureScale(int scale);
    void setLoadCarTextures(bool enabled);
    void setGameFolder(const QString &folder);
    void cycleLod();
    void cycleDebugMode();

public Q_SLOTS:
    void markLiveryDirty();
    void markLiveryDirtyImmediate();
    void markLiverySectionsDirty(const QVector<QString> &nodeIds);
    void onProjectGeometryChanged(bool refreshPreviews, const QVector<QString> &changedNodeIds);

Q_SIGNALS:
    void unwrapOverlayChanged();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct CachedProjectedLiverySection {
        fls::Project project;
        QRect clipRect;
    };

    struct PaintControls {
        CarPaintSlot slot;
        QComboBox *material = nullptr;
        QPushButton *primary = nullptr;
        QPushButton *secondary = nullptr;
    };

    struct PartHotspot {
        QAbstractButton *button = nullptr;
        QHash<int, fls::ModelBounds> optionBounds;
        QVector<const fls::CarPartOption *> options;
        std::optional<QVector3D> surfaceAnchor;
        std::optional<QPoint> visibleCenter;
        int partType = -1;
    };

    QMatrix4x4 cameraView() const;
    QMatrix4x4 cameraProjection() const;
    QSize liveryTextureSize() const;
    QTransform liveryWorldToScreen(const QSize &textureSize) const;
    void fitCameraToModel();
    void invalidateCachedLivery();
    void updateModelStatus();
    void layoutOverlayControls();
    void resetPartSelections();
    void initializePartControls();
    void initializePartDepthTest();
    void rebuildPartControls();
    QVector<const fls::CarPartOption *> availablePartOptions(int partType) const;
    void syncPartControls();
    void rebuildPartAnchors();
    void updatePartVisibility();
    void layoutPartControls();
    void beginPartSelection(int partType, QAbstractButton *anchor);
    QImage renderPartOptionPreview(int partType, int optionId);
    QImage renderGeometryPreview(CarModelRenderer &renderer, const fls::ModelBounds &bounds,
        const QHash<int, int> &selections, int partType, bool wholeCar);
    void selectPartOption(int partType, int optionId);
    QImage renderWheelOptionPreview(int partType, int optionId);
    void selectWheelOption(int partType, int optionId);
    bool applyWheelGeometry();
    void rebuildWheelSizingControls();
    void selectWheelSize(bool front, bool diameter, int optionId, float value);
    void rebuildColorsPanel();
    void syncPaintControls();
    void selectLod(int lodIndex);
    void switchCarUnwrapOverlay();
    void setSidePanelVisible(bool visible);
    int previewViewportWidth() const;
    void chooseSlotColor(const CarPaintSlot &slot, bool secondary, QWidget *anchor);
    void chooseSlotFinish(quint64 hash, quint32 finish);
    fls::LiveryPaintMaterial slotPaint(const CarPaintSlot &slot) const;
    QColor slotColor(const CarPaintSlot &slot, bool secondary) const;
    void clearPaintOverrides();
    void updateCarColorControl();

    NativeShapeRenderer shapeRenderer_;
    ShapeGeometryStore geometry_;
    CarModelRenderer carRenderer_;
    CarModelRenderer wheelRenderer_;
    std::shared_ptr<WheelPreviewState> wheelPreview_;
    fls::PaintFinishLibrary paintFinishes_;
    QString gameFolder_;
    quint64 paintFinishLoadGeneration_ = 0;
    bool geometryLoaded_ = false;

    fls::Project *project_ = nullptr;
    QPointer<EditorState> state_;
    QPointer<PaintColorPopup> paintPopup_;

    fls::CarModel model_;
    fls::ManufacturerColorPalette manufacturerColors_;
    bool modelUploadPending_ = false;
    bool modelFitPending_ = false;
    std::shared_ptr<QTemporaryDir> extractedCarDir_;
    QString loadedCarPath_;
    QString loadedCarGame_;
    QString loadedCarKey_;
    QString pendingCarKey_;
    std::vector<CarLoadCallback> carLoadCallbacks_;
    std::shared_ptr<std::atomic_bool> carLoadCancelled_;
    QElapsedTimer carLoadTimer_;
    QJsonObject carLoadTimings_;
    quint64 carLoadGeneration_ = 0;
    bool modelLoadPending_ = false;
    bool graphicsInitializationAttempted_ = false;
    bool graphicsApiSuitable_ = false;
    bool loadCarTextures_ = false;

    fls::LiveryMaskSet liveryMasks_;
    CarUnwrapOverlay carUnwrapOverlay_;
    std::vector<CarUnwrapOverlaySet> carUnwrapOverlaySets_;
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
    QLabel *modelStatus_ = nullptr;
    QFrame *sidePanel_ = nullptr;
    QToolButton *sidePanelToggle_ = nullptr;
    QWidget *colorsPage_ = nullptr;
    QVBoxLayout *colorsLayout_ = nullptr;
    QPushButton *clearPaintButton_ = nullptr;
    QVector<PaintControls> paintControls_;
    QVector<CarPaintFinish> paintFinishChoices_;
    QToolButton *partsToggle_ = nullptr;
    PartRadialSelector *partSelector_ = nullptr;
    QVector<PartHotspot> partHotspots_;
    QOpenGLShaderProgram partDepthProgram_;
    QOpenGLVertexArrayObject partDepthVao_;
    std::optional<QMatrix4x4> partDepthViewProjection_;
    QSize partDepthViewport_;
    bool partAnchorsDirty_ = true;
    QHash<int, int> selectedPartOptions_;
    QHash<int, int> individualPartOptions_;
    int partSelectorType_ = -1;
    int selectedLodIndex_ = 0;
    bool sidePanelVisible_ = false;

    QVector3D target_;
    float modelRadius_ = 1.0f;
    float yaw_ = 0.6f;
    float pitch_ = 0.3f;
    float distance_ = 4.0f;
    QPoint lastMousePos_;
};

} // namespace gui
