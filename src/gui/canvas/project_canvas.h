#pragma once

#include "core_types.h"
#include "layer.h"
#include "native_shape_renderer.h"
#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>
#include <QtOpenGLWidgets>

#include <functional>
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
    // Local size of a shape from the geometry store; used by the property panel to
    // pivot multi/group transforms about the selection's true bounding box.
    QSizeF shapeSize(int shapeId) const;
    // Local bounding box of a shape's actual inked geometry (ignoring the
    // declared square canvas / transparent corner markers); used to lay out
    // Place Text glyphs by their true width.
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
    void setGuideLayersOnTop(bool enabled);
    bool guideLayersOnTop() const;
    void setVisibilityBordersEnabled(bool enabled);
    void setPositionLimitBorderEnabled(bool enabled);
    void setVisibilityBorderResolution(const QSize &resolution);
    void setNudgeSteps(double normalStep, double shiftStep);
    std::optional<QColor> guideColorAtScreenPoint(const QPointF &point) const;
    std::optional<QColor> colorAtScreenPoint(const QPointF &point) const;
    void setPipetteColorPickedCallback(std::function<void(const QColor &)> callback);

    // Car UV-unwrap overlay: a canvas-space image (livery-canvas coords, canvasX in
    // [-1024,1024], canvasY in [-512,512], texel (0,0) maps to canvas
    // (-1024,-512)) showing where the imported car's rendered body panels land,
    // so vinyls can be aligned to the body. A null image clears it.
    void setCarUnwrapOverlay(const QImage &overlay);
    void setCarUnwrapVisible(bool visible);
    bool carUnwrapVisible() const;
    bool hasCarUnwrap() const;
    void cycleFlipSelection();

    // Align / distribute the current selection by evaluating each top-level unit's
    // world-space bounding box (a whole group counts as one unit; loose leaves count
    // individually). Align snaps every unit to a shared edge/centre; distribute evens
    // the spacing of unit centres along one axis. Both return false when the selection
    // has too few units to act on. Note world +Y is up, so Top = larger Y.
    enum class AlignEdge { Left, HCenter, Right, Top, VCenter, Bottom, Center };
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
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // Return false so Tab/Backtab is delivered to keyPressEvent (selection flip) instead of moving focus.
    bool focusNextPrevChild(bool next) override;

private:
    // Per-tool strategies (canvas_tools.h). Tools hold a reference to the
    // canvas and reach into the shared drag state directly.
    friend class CanvasTool;
    friend class SelectTool;
    friend class MoveTool;
    friend class MarqueeTool;
    friend class TransformTool;
    friend class RotateTool;
    friend class PipetteTool;

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

    struct HitEntry {
        int layerIndex = -1;
        QString layerId;
        QPolygonF screenPolygon;
        QRectF screenBounds;
    };

    // Transform fields captured at drag start, shared by shapes and guides
    // (guides have no skew and leave it at 0; shear(0, 0) is the identity).
    struct EntryStart {
        double x = 0.0;
        double y = 0.0;
        double scaleX = 1.0;
        double scaleY = 1.0;
        double rotation = 0.0;
        double skew = 0.0;
    };

    // The Transform tool's selection box described in its own local frame. In Absolute mode
    // localToWorld is the identity and localRect is the world-axis AABB (so everything reduces
    // to the legacy behaviour). In Relative mode localToWorld follows the shape/group
    // orientation, so the box, handles and scale axes track the shape.
    struct SelectionBox {
        bool valid = false;
        QRectF localRect;          // box rectangle in its own local coords (world units)
        QTransform localToWorld;   // maps localRect coords -> world
    };

    QRectF projectBounds() const;
    void updateViewTransform();
    QPointF worldToScreen(const QPointF &point) const;
    QPointF screenToWorld(const QPointF &point) const;
    QPolygonF screenQuad(const QTransform &entryToWorld, const QRectF &localRect) const;
    // Derived scene tree the read paths walk. Uses EditorState's cached tree when a
    // state is attached; otherwise a canvas-local tree rebuilt from project_ on scene
    // cache invalidation.
    const fh6::scene::Group *sceneTree() const;
    // True when the given section owns the active livery section (or section filtering
    // does not apply): centralises the repeated `isLivery && activeSectionId` gate.
    bool isSectionActive(const QString &sectionGroupId) const;
    // Walk the scene's shape/guide leaves from whichever backend is active (EditorState's
    // cached render entries, or the canvas-local tree), yielding each leaf with its world
    // transform (and owning section for guides / draw order for shapes). `reverse` visits
    // topmost-first for hit testing; the visitor returns false to stop early. No visibility
    // or selection filtering is applied — callers add their own predicate.
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
    // Resolve the whole-groups being dragged (top-most fully selected, minus locked ones):
    // their ids, drag-start frames, and the descendant leaf/guide ids they cover (so loose
    // items nested under them are excluded and each transform is applied once). Shared by
    // captureDragStarts() and nudgeSelection().
    void collectDragGroups(QVector<QString> &groupIds,
                           QHash<QString, QTransform> &startFrames,
                           QSet<QString> &groupedLayerIds,
                           QSet<QString> &groupedGuideIds) const;
    QVector<QString> dragTransformTargetIds() const;
    void captureScaleReference();
    // Chooses the drag mode (and captures rotate/scale references) for the active
    // tool once a selection and its drag-start box exist. Leaves dragMode_ as None
    // when the press does not begin a drag. Delegates to activeTool_->beginDrag().
    void beginToolDrag(const QPointF &screenPos, const QPointF &boxCenterWorld);
    // Shared by the Rotate tool and the Transform tool's rotate zone: enters the
    // rotate drag about the given world-space pivot.
    void beginRotateDrag(const QPointF &boxCenterWorld);
    void applyMoveDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers);
    bool nudgeSelection(const QPointF &delta);
    // Shared machinery for alignSelection()/distributeSelection(): partitions the
    // selection into movable units, hands their world bounds to computeDeltas (in unit
    // order), and applies the returned per-unit world translations as one command. When
    // descendSoleGroup is set and the selection resolves to a single group, its direct
    // children become the units instead (so a lone group can distribute its members).
    bool applyAlignDistribute(const std::function<QVector<QPointF>(const QVector<QRectF> &)> &computeDeltas,
                              bool descendSoleGroup = false);
    void applyScaleDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers);
    void applySkewDrag(const QPointF &screenPoint);
    // Applies a linear-transform gesture (scale/skew) to every dragged item by composing it
    // with each item's drag-start transform, then decomposes the result back to per-item
    // fields. preMultiply picks the side: true -> transform * start (Relative single-item scale),
    // false -> start * transform (world/box-frame scale and group skew). Shared by both gestures.
    void applyDragTransform(const QTransform &transform, bool preMultiply);
    // Compose a world-space transform onto every dragged shape/guide (decomposing back to
    // per-item fields) and accumulate it into each dragged group's frame. Shared by the move
    // and rotate gestures.
    void applyWorldTransformToDragItems(const QTransform &worldTransform);
    QString transformSelectionSignature() const;
    QSizeF transformBoxVisualExtents(const SelectionBox &box) const;
    void captureScaleHintReference();
    void applyRotateDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers);
    void clearCursorHint();
    void setCursorHint(const QPointF &point, const QStringList &lines);
    void drawCursorHint(QPainter &painter);
    // True while a drag that mutates entry transforms is active (not Pan/Marquee).
    bool isTransformDrag() const;
    void requestLiveSceneUpdate();
    void resetDragState();
    void finishDrag();
    void cancelDrag();
    QString transformHandleAt(const QPointF &point, const SelectionBox &box) const;
    bool rotateZoneAt(const QPointF &point, const SelectionBox &box) const;
    // Resolve the native cursor shape and themed cursor-art file for a scale/skew handle,
    // orienting to the box's (possibly rotated) frame when one is supplied. Backs both
    // cursorForScaleHandle() and cursorForTransformHandle().
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
    void drawOverlay(QPainter &painter);
    bool selectionIsGroupLike() const;
    void updateSelectionFlashState();
    void setFlashingLayerIds(const QSet<QString> &ids);
    void scheduleSelectionFlashTimer();
    // Flash progress in [0,1) while inside the flash window, nullopt otherwise.
    std::optional<double> selectionFlashProgress() const;
    double selectionFlashHue() const;
    double selectionFlashStrength() const;
    void refreshHover(const QPointF &point, Qt::KeyboardModifiers modifiers);
    void selectByMarquee(Qt::KeyboardModifiers modifiers);
    bool pointInPolygon(const QPointF &point, const QPolygonF &polygon) const;
    bool movedPastClickThreshold(const QPointF &point) const;
    QString guideAtScreenPoint(const QPointF &point);
    void drawGuideLayers(QPainter &painter);
    QImage guideImage(const fh6::scene::GuideLayer &guide) const;
    QString sectionCanvasCacheKey() const;
    void storeSectionCanvasCache(const QString &key);


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
    // Full selection box captured at drag start, so scaling/rotation references survive
    // pan/zoom and follow the box's (possibly rotated) local frame.
    SelectionBox dragStartBox_;
    QPointF rotateCenterWorld_;
    double rotateStartAngle_ = 0.0;
    // Relative transform mode: the selection box follows the shape/group orientation.
    bool transformRelativeMode_ = false;
    bool moveToolAutoSelect_ = false;
    bool selectionFlashEnabled_ = true;
    bool guideLayersOnTop_ = true;
    bool visibilityBordersEnabled_ = true;
    bool positionLimitBorderEnabled_ = false;
    QSize visibilityBorderResolution_ = QSize(1920, 1080);
    double nudgeStep_ = 0.1;
    double nudgeShiftStep_ = 1.0;
    std::function<void(const QColor &)> pipetteColorPickedCallback_;
    // Frame angle for a Relative-mode multi-selection box. It is NOT stored directly (that
    // would go stale on undo/redo); instead it is derived live as
    // (primary selected item's current rotation - frameReferenceRotation_). The reference is
    // captured when the selection set changes, so the box starts axis-aligned and then follows
    // any rotation applied afterwards. Because every rotate gesture adds the same delta to all
    // selected items, the primary's rotation tracks the cumulative box rotation, and undo/redo
    // restore it automatically. Scale/skew drags are the exception: they shear children and drift
    // the primary's decomposed rotation, so currentSelectionBox() pins the frame to the drag-start
    // angle and rebases this reference to keep it steady (see there). Mutable + signature-tracked
    // so currentSelectionBox() can refresh the reference lazily.
    mutable double frameReferenceRotation_ = 0.0;
    mutable QSet<QString> frameLayerSignature_;
    mutable QSet<QString> frameGuideSignature_;
    // Scale handle/anchor/centre expressed in the drag-start box's local coords, used to
    // compute pure per-axis scale factors in the box frame.
    QPointF scaleHandleLocal_;
    QPointF scaleAnchorLocal_;
    QPointF scaleCenterLocal_;
    // World-space scale references captured at drag start so scaling stays correct across
    // pan/zoom and tracks the grabbed handle exactly (anchor = fixed opposite side/corner).
    QPointF scaleAnchorWorld_;
    QPointF scaleHandleStartWorld_;
    // Selection centre in world space; used as the scale anchor when Alt is held so the
    // selection scales about its centre (the centre stays fixed) instead of the opposite handle.
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
    // True when the in-progress drag began with an Alt-duplicate, so finishing the drag
    // refreshes the layer tree to show the newly inserted clones.
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
    // Selected (unlocked) layers resolved once at drag start so per-move handlers
    // don't rebuild the entry/lock maps and rescan the project every mouse event.
    QVector<fh6::scene::Shape *> dragLayers_;
    QVector<fh6::scene::GuideLayer *> dragGuides_;
    // Whole groups (top-most fully-selected) transformed as units during a drag,
    // with descendant leaves/guides filtered out so the group frame receives the
    // transform exactly once.
    QVector<QString> dragGroupIds_;
    QHash<QString, QTransform> dragGroupStartFrames_;
    mutable QHash<QString, QImage> guideImageCache_;
    mutable QHash<QString, QImage> sectionCanvasCache_;
    // Car UV-unwrap overlay (canvas space) and its visibility toggle.
    QImage carUnwrapOverlay_;
    bool carUnwrapVisible_ = false;
    // Cached world-space union of the selection's bounds; mapped through the view transform to
    // produce the screen-space selection box without rescanning layers on every repaint.
    mutable std::optional<QRectF> selectionWorldBoundsCache_;
    QColor canvasColor_;
};

} // namespace gui
