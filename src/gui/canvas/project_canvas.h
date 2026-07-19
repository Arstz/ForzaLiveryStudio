#pragma once

#include "bucket_fill.h"
#include "core_types.h"
#include "layer.h"
#include "native_shape_renderer.h"
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
class QKeyEvent;
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
    void resetRelativeSelectionFrame();
    void refitView();
    bool centerViewOnSelection();
    QPointF viewCenterWorld();
    void setCanvasColor(const QColor &color);
    QColor canvasColor() const;
    void setTransformRelativeMode(bool relative);
    void setMoveToolAutoSelect(bool enabled);
    bool moveToolAutoSelect() const;
    void setSelectionFlashEnabled(bool enabled);
    bool selectionFlashEnabled() const;
    void setDisplayAnchorsDuringTransformDrag(bool enabled);
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

    void setCarUnwrapOverlay(const QImage &overlay);
    void setCarUnwrapVisible(bool visible);
    bool carUnwrapVisible() const;
    bool hasCarUnwrap() const;
    void cycleFlipSelection();

    // Runs the region-extraction draft on a selected guide layer and shows the
    // result as a canvas overlay. Returns false with a human-readable reason.
    bool createRegionsForSelectedGuide(QString *message = nullptr);
    void clearRegionOverlay();

    // Fills the non-lineart regions of the current overlay with affine
    // primitives via the Pen fitter and shows the placements. Returns false
    // with a human-readable reason.
    bool fillRegionsForOverlay(QString *message = nullptr);
    void clearRegionFills();
    // The computed fill placements, mapped from image-pixel space into world
    // space and tagged with each region's colour, ready for scene insertion.
    QVector<GeneratedRegionShape> regionFillWorldPlacements();
    // Hide the region overlay preview (e.g. once real shapes were inserted)
    // without discarding the extracted regions, so a re-fill stays cheap.
    void hideRegionOverlay();

    enum class AlignEdge { Left, HCenter, Right, Top, VCenter, Bottom };
    enum class DistributeAxis { Horizontal, Vertical };
    bool alignSelection(AlignEdge edge);
    bool distributeSelection(DistributeAxis axis);

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
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    friend class CanvasTool;
    friend class SelectTool;
    friend class MoveTool;
    friend class MarqueeTool;
    friend class TransformTool;
    friend class RotateTool;
    friend class PipetteTool;
    friend class PenTool;
    friend class BucketTool;

    static constexpr double PenCloseRadius = 8.0;
    static constexpr double PenEditRadius = 9.0;

    enum class DragMode {
        None,
        Pan,
        Move,
        Marquee,
        TransformMove,
        Scale,
        Skew,
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
    };

    struct SelectionBox {
        bool valid = false;
        QRectF localRect;
        QTransform localToWorld;
    };

    struct PenCurveHit {
        int insertIndex = -1;
        QPointF worldPosition;
        double screenDistance = std::numeric_limits<double>::max();

        bool valid() const { return insertIndex >= 0; }
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
    void closePenPath();
    void drawPenOverlay(QPainter &painter);
    QPainterPath penPreviewPath(bool closeToStart) const;
    int penPointAtScreen(const QPointF &screenPoint) const;
    PenCurveHit penCurveAtScreen(const QPointF &screenPoint) const;
    void normalizePenPointOrder();
    void validatePenInteraction();
    void refreshPenInteractionHint(const QPointF &screenPoint,
                                   Qt::KeyboardModifiers modifiers);
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
    std::vector<std::unique_ptr<CanvasTool>> tools_;
    CanvasTool *activeTool_ = nullptr;
    QTransform worldToScreen_;
    QTransform screenToWorld_;
    QRectF currentBounds_;
    QRectF viewBounds_;
    double baseScale_ = 1.0;
    double zoom_ = 1.0;
    QPointF pan_;
    bool spaceDown_ = false;
    DragMode dragMode_ = DragMode::None;
    QPointF dragStartScreen_;
    QPointF dragLastScreen_;
    QPointF dragStartWorld_;
    QRectF dragStartSelectionBounds_;
    SelectionBox dragStartBox_;
    QPointF rotateCenterWorld_;
    double rotateStartAngle_ = 0.0;
    bool transformRelativeMode_ = false;
    bool moveToolAutoSelect_ = false;
    bool selectionFlashEnabled_ = true;
    bool displayAnchorsDuringTransformDrag_ = true;
    bool guideLayersVisible_ = true;
    bool guideLayersOnTop_ = true;
    bool guidelinesVisible_ = true;
    bool guidelinesLocked_ = false;
    QColor guidelineColor_ = QColor(0, 170, 255);
    GuidelineOrientation draggedGuidelineOrientation_ = GuidelineOrientation::None;
    int draggedGuidelineIndex_ = -1;
    bool rulerPressActive_ = false;
    bool visibilityBordersEnabled_ = true;
    bool positionLimitBorderEnabled_ = false;
    QSize visibilityBorderResolution_ = QSize(1920, 1080);
    double nudgeStep_ = 0.1;
    double nudgeShiftStep_ = 1.0;
    std::function<void(const QColor &)> pipetteColorPickedCallback_;
    std::function<void(const QVector<PenPoint> &, const std::optional<QColor> &)> penFillRequestedCallback_;
    std::function<void()> penFillCancelCallback_;
    QVector<PenPoint> penPoints_;
    std::optional<QColor> penFillColor_;
    QPointF penHoverWorld_;
    bool penLooped_ = false;
    int penHoverPoint_ = -1;
    PenCurveHit penHoverCurve_;
    int penDragPoint_ = -1;
    QPointF penDragOffsetWorld_;
    QVector<QPointF> penCrossings_;
    QString penError_;
    QString penFillMessage_;
    bool penFillRunning_ = false;
    int bucketTolerance_ = 16;
    QString bucketGuideId_;
    QString bucketSourceGuideId_;
    QPoint bucketSeedPixel_ = QPoint(-1, -1);
    BucketFillResult bucketFill_;
    QImage bucketSourceImage_;
    QImage bucketPreviewImage_;
    mutable double frameReferenceRotation_ = 0.0;
    mutable QSet<QString> frameLayerSignature_;
    mutable QSet<QString> frameGuideSignature_;
    QPointF scaleHandleLocal_;
    QPointF scaleAnchorLocal_;
    QPointF scaleCenterLocal_;
    QPointF scaleAnchorWorld_;
    QPointF scaleHandleStartWorld_;
    QPointF scaleCenterWorld_;
    QString scaleHintSelectionSignature_;
    QSizeF scaleHintBaseExtents_;
    bool scaleHintBaseValid_ = false;
    double scaleHintStartScaleX_ = 1.0;
    double scaleHintStartScaleY_ = 1.0;
    QRectF marqueeRect_;
    QPointF cursorHintPoint_;
    QStringList cursorHintLines_;
    QString activeHandle_;
    bool dragDuplicated_ = false;
    bool dragUsesProjectEdit_ = false;
    QString hoverLayerId_;
    QPolygonF hoverPolygon_;
    QVector<HitEntry> hitCache_;
    bool hitCacheDirty_ = true;
    NativeShapeRenderer renderer_;
    bool rendererGeometryDirty_ = true;
    QSet<QString> flashingLayerIds_;
    QElapsedTimer selectionFlashClock_;
    QTimer selectionFlashTimer_;
    QHash<QString, EntryStart> dragStarts_;
    QHash<QString, EntryStart> dragGuideStarts_;
    QVector<fh6::scene::Shape *> dragLayers_;
    QVector<fh6::scene::GuideLayer *> dragGuides_;
    QVector<QString> dragGroupIds_;
    QHash<QString, QTransform> dragGroupStartFrames_;
    mutable QHash<QString, QImage> guideImageCache_;
    mutable QHash<QString, QImage> sectionCanvasCache_;
    QImage carUnwrapOverlay_;
    bool carUnwrapVisible_ = false;
    QString regionOverlayGuideId_;
    RegionExtractionResult regionOverlay_;
    QVector<RegionFillLayer> regionFills_;
    QHash<int, QPainterPath> regionFillSilhouettes_;
    bool showRegionFills_ = false;
    bool regionOverlayHidden_ = false;
    mutable std::optional<QRectF> selectionWorldBoundsCache_;
    QColor canvasColor_;
};

} // namespace gui
