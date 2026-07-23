#pragma once

#include "bucket_fill.h"
#include "canvas_camera.h"
#include "core_types.h"
#include "layer.h"
#include "lining_fill.h"
#include "gui/key_bindings.h"
#include "native_shape_renderer.h"
#include "path_interaction.h"
#include "pen_fill.h"
#include "region_extract.h"
#include "region_fill.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>
#include <QtOpenGLWidgets>

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

class QPainter;
class QMouseEvent;
class QWheelEvent;

namespace gui {

class CanvasTool;
class EditorState;

class ProjectCanvas final : public QOpenGLWidget {
public:
    explicit ProjectCanvas(QWidget *parent = nullptr);
    ~ProjectCanvas() override;

    void setProject(fh6::Project *project);
    void setEditorState(EditorState *state);
    bool loadGeometry(QString *error = nullptr);
    QSizeF shapeSize(int shapeId) const;
    QRectF shapeInkBounds(int shapeId) const;
    void setTool(const QString &tool);
    QString tool() const;
    void invalidateSelectionCache();
    void invalidateSceneCache();
    void invalidateGuideImageCache();
    void resetRelativeSelectionFrame();
    void refitView();
    bool centerViewOnSelection();
    QPointF viewCenterWorld();
    void setCanvasColor(const QColor &color);
    QColor canvasColor() const;
    void setTransformRelativeMode(bool relative);
    void setMoveToolAutoSelect(bool enabled);
    bool moveToolAutoSelect() const;
    void setAllowMoveOutsideBoundingBox(bool enabled);
    void setSelectionFlashEnabled(bool enabled);
    bool selectionFlashEnabled() const;
    void setDisplayAnchorsDuringTransformDrag(bool enabled);
    void setSeparateOpacityAndSkewTools(bool enabled);
    void setGuideLayersVisible(bool visible);
    void setGuideLayersOnTop(bool enabled);
    bool guideLayersOnTop() const;
    void setGuidelinesVisible(bool visible);
    void setGuidelinesLocked(bool locked);
    bool guidelinesLocked() const;
    void setGuidelineColor(const QColor &color);
    bool deleteAllGuidelines();
    void setVisibilityBordersEnabled(bool enabled);
    void setPositionLimitBorderEnabled(bool enabled);
    void setVisibilityBorderResolution(const QSize &resolution);
    void setNudgeSteps(double normalStep, double shiftStep);
    std::optional<QColor> guideColorAtScreenPoint(const QPointF &point) const;
    std::optional<QColor> colorAtScreenPoint(const QPointF &point) const;
    void setPipetteColorPickedCallback(std::function<void(const QColor &)> callback);
    void setPenFillRequestedCallback(
        std::function<void(const QVector<PenPoint> &, const std::optional<QColor> &)> callback);
    void setPenFillCancelCallback(std::function<void()> callback);
    QVector<PenPrimitive> penPrimitiveCatalog() const;
    void setPenFillRunning(bool running, const QString &message = QString());
    void cancelPenInteraction();
    void setLiningFillRequestedCallback(
        std::function<void(const QVector<PenPoint> &, double, const std::optional<QColor> &)> callback);
    void setLiningFillCancelCallback(std::function<void()> callback);
    void setLiningWidthChangedCallback(std::function<void(double)> callback);
    QVector<PenPrimitive> liningPrimitiveCatalog() const;
    void setLiningFillRunning(bool running, const QString &message = QString());
    void cancelLiningInteraction();
    void setLiningWidth(double width);
    double liningWidth() const;

    void setCarUnwrapOverlay(const QImage &overlay);
    void setCarUnwrapVisible(bool visible);
    bool carUnwrapVisible() const;
    bool hasCarUnwrap() const;
    void cycleFlipSelection();

    bool createRegionsForSelectedGuide(int smallRegionMergeArea,
                                       QString *message = nullptr);
    void clearRegionOverlay();

    bool prepareRegionFillBatch(RegionFillBatchRequest *request,
                                QString *message = nullptr) const;
    bool applyRegionFillBatch(RegionFillBatchResult result,
                              QString *message = nullptr);
    void clearRegionFills();
    QVector<GeneratedRegionVariant> regionFillWorldVariants();
    void hideRegionOverlay();

    enum class AlignEdge { Left, HCenter, Right, Top, VCenter, Bottom };
    enum class DistributeAxis { Horizontal, Vertical };
    bool alignSelection(AlignEdge edge);
    bool distributeSelection(DistributeAxis axis);
    bool handleKeyBinding(KeyInteraction interaction, KeyEventPhase phase, bool autoRepeat);
    bool undoContourEdit();
    bool redoContourEdit();

    bool currentTransformBox(QPointF *center,
                             double *width,
                             double *height,
                             QTransform *boxFrame) const;

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    friend class CanvasTool;
    friend class SelectTool;
    friend class MoveTool;
    friend class MarqueeTool;
    friend class TransformTool;
    friend class RotateTool;
    friend class SkewTool;
    friend class OpacityTool;
    friend class PipetteTool;
    friend class PenTool;
    friend class LiningTool;
    friend class BucketTool;

    static constexpr double kPenCloseRadius = 8.0;
    static constexpr double kPenEditRadius = 9.0;
    static constexpr double kDefaultNudgeStep = 0.1;
    static constexpr double kDefaultNudgeShiftStep = 1.0;
    static constexpr double kDefaultLiningWidth = 8.0;
    static constexpr double kMinimumLiningWidth = 0.1;
    static constexpr double kMaximumLiningWidth = 256.0;
    static constexpr int kDefaultBucketTolerance = 16;
    static constexpr QSize kDefaultVisibilityBorderResolution{1920, 1080};
    inline static const QColor kDefaultGuidelineColor{0, 170, 255};

    enum class DragMode {
        None,
        Pan,
        Move,
        Marquee,
        TransformMove,
        Scale,
        Skew,
        Opacity,
        Rotate,
    };

    enum class GuidelineOrientation {
        None,
        Horizontal,
        Vertical,
    };

    struct HitEntry {
        int layerIndex = -1;
        QString layerId;
        QPolygonF screenPolygon;
        QRectF screenBounds;
    };

    struct EntryStart {
        double x = 0.0;
        double y = 0.0;
        double scaleX = 1.0;
        double scaleY = 1.0;
        double rotation = 0.0;
        double skew = 0.0;
        double opacity = 1.0;
    };

    struct SelectionBox {
        bool valid = false;
        QRectF localRect;
        QTransform localToWorld;
    };

    struct DragState {
        SelectionBox startBox;
        QRectF startSelectionBounds;
        QRectF marqueeRect;
        QPointF startScreen;
        QPointF lastScreen;
        QPointF startWorld;
        QPointF rotateCenterWorld;
        QPointF scaleHandleLocal;
        QPointF scaleAnchorLocal;
        QPointF scaleCenterLocal;
        QPointF scaleAnchorWorld;
        QPointF scaleHandleStartWorld;
        QPointF scaleCenterWorld;
        QSizeF scaleHintBaseExtents;
        QString activeHandle;
        QString scaleHintSelectionSignature;
        QHash<QString, EntryStart> starts;
        QHash<QString, EntryStart> guideStarts;
        QHash<QString, QTransform> groupStartFrames;
        QVector<fh6::scene::Shape *> layers;
        QVector<fh6::scene::GuideLayer *> guides;
        QVector<QString> groupIds;
        DragMode mode = DragMode::None;
        bool duplicated = false;
        bool usesProjectEdit = false;
        bool scaleHintBaseValid = false;
        double rotateStartAngle = 0.0;
        double scaleHintStartScaleX = 1.0;
        double scaleHintStartScaleY = 1.0;
    };

    struct BucketState {
        QString guideId;
        QString sourceGuideId;
        QPoint seedPixel = QPoint(-1, -1);
        BucketFillResult fill;
        QImage sourceImage;
        QImage previewImage;
        int tolerance = kDefaultBucketTolerance;
    };

    struct RegionOverlayState {
        QString guideId;
        RegionExtractionResult overlay;
        QVector<RegionFillLayer> fills;
        QHash<int, QPainterPath> fillSilhouettes;
        quint64 generation = 0;
        bool showFills = false;
        bool hidden = false;
    };

    struct GuidelineState {
        QColor color = kDefaultGuidelineColor;
        GuidelineOrientation draggedOrientation = GuidelineOrientation::None;
        int draggedIndex = -1;
        bool visible = true;
        bool locked = false;
        bool rulerPressActive = false;
    };

    struct FlashState {
        QSet<QString> layerIds;
        QElapsedTimer clock;
        QTimer timer;
        bool enabled = true;
    };

    struct SelectionFrame {
        QSet<QString> layerSignature;
        QSet<QString> guideSignature;
        double referenceRotation = 0.0;
    };

    struct FlipCycleState {
        QVector<QString> targetIds;
        QHash<QString, fh6::scene::Transform2D> baseline;
        QHash<QString, fh6::scene::Transform2D> expected;
        QRectF bounds;
        int step = 0;
    };

    struct CanvasOptions {
        QColor canvasColor;
        QSize borderResolution = kDefaultVisibilityBorderResolution;
        bool transformRelativeMode = false;
        bool moveToolAutoSelect = false;
        bool allowMoveOutsideBoundingBox = true;
        bool displayAnchorsDuringTransformDrag = true;
        bool separateOpacityAndSkewTools = false;
        bool guideLayersVisible = true;
        bool guideLayersOnTop = true;
        bool visibilityBordersEnabled = true;
        bool positionLimitBorderEnabled = false;
        double nudgeStep = kDefaultNudgeStep;
        double nudgeShiftStep = kDefaultNudgeShiftStep;
    };

    QRectF projectBounds() const;
    void updateViewTransform();
    QPointF worldToScreen(const QPointF &point) const;
    QPointF screenToWorld(const QPointF &point) const;
    QPolygonF screenQuad(const QTransform &entryToWorld, const QRectF &localRect) const;
    const fh6::scene::Group *sceneTree() const;
    bool isSectionActive(const QString &sectionGroupId) const;
    void forEachSceneShape(const std::function<bool(const fh6::scene::Shape &, const QTransform &, int)> &visit,
                           bool reverse) const;
    void forEachSceneGuide(const std::function<bool(const fh6::scene::GuideLayer &, const QTransform &, const QString &)> &visit,
                           bool reverse) const;
    QVector<HitEntry> hitEntries();
    QVector<QString> layersAtScreenPoint(const QPointF &point);
    std::optional<QColor> layerColorAtScreenPoint(const QPointF &point) const;
    QString selectTargetAtScreenPoint(const QPointF &point, Qt::KeyboardModifiers modifiers);
    QRectF selectedScreenBounds() const;
    QRectF selectedWorldBounds() const;
    const QRectF &cachedSelectionWorldBounds() const;
    SelectionBox currentSelectionBox() const;
    QTransform boxToScreen(const SelectionBox &box) const;
    bool boxContainsScreenPoint(const SelectionBox &box, const QPointF &screenPoint) const;
    QVector<fh6::scene::Shape *> selectedLayers() const;
    QVector<fh6::scene::GuideLayer *> selectedGuideLayers() const;
    void captureDragStarts();
    void collectDragGroups(QVector<QString> &groupIds,
                           QHash<QString, QTransform> &startFrames,
                           QSet<QString> &groupedLayerIds,
                           QSet<QString> &groupedGuideIds) const;
    QVector<QString> dragTransformTargetIds() const;
    void captureScaleReference();
    void beginToolDrag(const QPointF &screenPos, const QPointF &boxCenterWorld);
    void beginRotateDrag(const QPointF &boxCenterWorld);
    void applyMoveDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers);
    bool nudgeSelection(const QPointF &delta);
    bool applyAlignDistribute(const std::function<QVector<QPointF>(const QVector<QRectF> &)> &computeDeltas,
                              bool descendSoleGroup = false);
    void applyScaleDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers);
    void applySkewDrag(const QPointF &screenPoint);
    void applyOpacityDrag(const QPointF &screenPoint);
    void applyDragTransform(const QTransform &transform, bool preMultiply);
    void applyWorldTransformToDragItems(const QTransform &worldTransform);
    QString transformSelectionSignature() const;
    QSizeF transformBoxVisualExtents(const SelectionBox &box) const;
    void captureScaleHintReference();
    void applyRotateDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers);
    void clearCursorHint();
    void setCursorHint(const QPointF &point, const QStringList &lines);
    void drawCursorHint(QPainter &painter);
    bool isTransformDrag() const;
    void requestLiveSceneUpdate();
    void resetDragState();
    void finishDrag();
    void cancelDrag();
    QString transformHandleAt(const QPointF &point, const SelectionBox &box) const;
    bool rotateZoneAt(const QPointF &point, const SelectionBox &box) const;
    void resolveHandleCursor(const QString &handle, const SelectionBox *box,
                             Qt::CursorShape *shape, QString *iconFile) const;
    Qt::CursorShape cursorForScaleHandle(const QString &handle, const SelectionBox *box = nullptr) const;
    QCursor cursorForTransformHandle(const QString &handle, const SelectionBox *box = nullptr) const;
    Qt::CursorShape cursorForPoint(const QPointF &point);
    void updateCursorForPoint(const QPointF &point);
    QCursor pipetteCursor() const;
    QCursor rotateCursor() const;
    QCursor rotateCursorForPoint(const QPointF &point, const SelectionBox &box) const;
    void drawVisibilityBorders(QPainter &painter);
    void drawRulersAndGuidelines(QPainter &painter);
    void drawOverlay(QPainter &painter);
    GuidelineOrientation rulerAt(const QPointF &point) const;
    int guidelineAtRuler(const QPointF &point, GuidelineOrientation orientation) const;
    double guidelineCoordinateAt(const QPointF &point, GuidelineOrientation orientation) const;
    bool handleRulerPress(QMouseEvent *event);
    bool selectionIsGroupLike() const;
    void updateSelectionFlashState();
    void setFlashingLayerIds(const QSet<QString> &ids);
    void scheduleSelectionFlashTimer();
    std::optional<double> selectionFlashProgress() const;
    double selectionFlashHue() const;
    double selectionFlashStrength() const;
    void refreshHover(const QPointF &point, Qt::KeyboardModifiers modifiers);
    void selectByMarquee(Qt::KeyboardModifiers modifiers);
    bool pointInPolygon(const QPointF &point, const QPolygonF &polygon) const;
    bool movedPastClickThreshold(const QPointF &point) const;
    QString guideAtScreenPoint(const QPointF &point);
    void drawGuideLayers(QPainter &painter);
    void drawRegionOverlay(QPainter &painter);
    QImage guideImage(const fh6::scene::GuideLayer &guide) const;
    QString sectionCanvasCacheKey() const;
    void storeSectionCanvasCache(const QString &key);
    void setPathFillRunning(PathInteraction &path, bool running, const QString &message);
    void cancelPathInteraction(PathInteraction &path, const std::function<void()> &cancelCallback);
    void discardPathInteraction(PathInteraction &path, const std::function<void()> &cancelCallback);
    void beginPathEdit(PathInteraction &path);
    void commitPathEdit(PathInteraction &path);
    bool applyPathHistory(PathInteraction &path, bool undo);
    void refreshPathAfterHistory(PathInteraction &path);
    void closePenPath();
    void drawPenOverlay(QPainter &painter);
    QPainterPath penPreviewPath(bool closeToStart) const;
    int pointAtScreen(const QVector<PenPoint> &points, const QPointF &screenPoint) const;
    void accumulateCurveHit(const PenBoundarySegment &segment, int insertIndex,
                            const QPointF &screenPoint, PenCurveHit &best) const;
    void appendPointEditHints(QStringList &lines, const QVector<PenPoint> &points,
                              int hoverPoint, const PenCurveHit &hoverCurve) const;
    PenCurveHit penCurveAtScreen(const QPointF &screenPoint) const;
    void normalizePenPointOrder();
    void validatePenInteraction();
    void refreshPenInteractionHint(const QPointF &screenPoint,
                                   Qt::KeyboardModifiers modifiers);
    void requestLiningFill();
    void drawLiningOverlay(QPainter &painter);
    QPainterPath liningPreviewPath() const;
    PenCurveHit liningCurveAtScreen(const QPointF &screenPoint) const;
    void validateLiningInteraction();
    void refreshLiningInteractionHint(const QPointF &screenPoint,
                                      Qt::KeyboardModifiers modifiers);
    void adjustLiningWidth(double delta, const QPointF &screenPoint);
    bool updateBucketPreview(const QPointF &screenPoint);
    bool commitBucketPreview(const QPointF &screenPoint);
    void adjustBucketTolerance(int delta, const QPointF &screenPoint);
    void clearBucketPreview();
    void drawBucketOverlay(QPainter &painter);
    bool bucketGuideContext(const QPointF &screenPoint,
                            const fh6::scene::GuideLayer **guide,
                            QTransform *guideWorld,
                            QImage *image,
                            QPoint *imagePoint,
                            QString *error) const;


    EditorState *state_ = nullptr;
    fh6::Project *project_ = nullptr;
    ShapeGeometryStore geometry_;
    bool geometryLoaded_ = false;
    QString tool_ = QStringLiteral("select");
    QString lastNonPipetteTool_ = QStringLiteral("select");
    std::vector<std::unique_ptr<CanvasTool>> tools_;
    CanvasTool *activeTool_ = nullptr;
    CanvasCamera camera_;
    DragState drag_;
    GuidelineState guidelines_;
    CanvasOptions options_;
    bool spaceDown_ = false;
    std::function<void(const QColor &)> pipetteColorPickedCallback_;
    std::function<void(const QVector<PenPoint> &, const std::optional<QColor> &)> penFillRequestedCallback_;
    std::function<void()> penFillCancelCallback_;
    std::function<void(const QVector<PenPoint> &, double, const std::optional<QColor> &)> liningFillRequestedCallback_;
    std::function<void()> liningFillCancelCallback_;
    std::function<void(double)> liningWidthChangedCallback_;
    PathInteraction pen_;
    PathInteraction lining_;
    double liningWidth_ = kDefaultLiningWidth;
    BucketState bucket_;
    mutable SelectionFrame frame_;
    QPointF cursorHintPoint_;
    QStringList cursorHintLines_;
    QString hoverLayerId_;
    QPolygonF hoverPolygon_;
    QVector<HitEntry> hitCache_;
    bool hitCacheDirty_ = true;
    NativeShapeRenderer renderer_;
    bool rendererGeometryDirty_ = true;
    FlashState flash_;
    mutable QHash<QString, QImage> guideImageCache_;
    mutable QHash<QString, QImage> sectionCanvasCache_;
    std::optional<FlipCycleState> flipCycle_;
    QImage carUnwrapOverlay_;
    bool carUnwrapVisible_ = false;
    RegionOverlayState region_;
    mutable std::optional<QRectF> selectionWorldBoundsCache_;
};

} // namespace gui
