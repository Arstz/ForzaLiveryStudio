#include "project_canvas.h"

#include "project_canvas_internal.h"

#include <cmath>

namespace gui {

using namespace pc_detail;

namespace {

const QRectF kDefaultProjectBounds(-128.0, -128.0, 256.0, 256.0);

} // namespace

ProjectCanvas::ProjectCanvas(QWidget *parent)
    : QOpenGLWidget(parent) {
    options_.canvasColor = canvasColorForTheme(currentUiTheme(), loadCanvasColorSettings());
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    setFormat(format);
    setMinimumSize(320, 240);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    flash_.clock.start();
    flash_.timer.setInterval(kSelectionFlashFrameMs);
    connect(&flash_.timer, &QTimer::timeout, this, [this]() {
        update();
        scheduleSelectionFlashTimer();
    });

    tools_.push_back(std::make_unique<SelectTool>(*this));
    tools_.push_back(std::make_unique<MoveTool>(*this));
    tools_.push_back(std::make_unique<MarqueeTool>(*this));
    tools_.push_back(std::make_unique<TransformTool>(*this));
    tools_.push_back(std::make_unique<RotateTool>(*this));
    tools_.push_back(std::make_unique<PipetteTool>(*this));
    tools_.push_back(std::make_unique<PenTool>(*this));
    tools_.push_back(std::make_unique<LiningTool>(*this));
    tools_.push_back(std::make_unique<BucketTool>(*this));
    activeTool_ = tools_.front().get();
}

ProjectCanvas::~ProjectCanvas() = default;

void ProjectCanvas::setProject(fh6::Project *project) {
    cancelPenInteraction();
    cancelLiningInteraction();
    clearBucketPreview();
    project_ = project;
    guidelines_.draggedOrientation = GuidelineOrientation::None;
    guidelines_.draggedIndex = -1;
    guidelines_.rulerPressActive = false;
    guideImageCache_.clear();
    sectionCanvasCache_.clear();
    camera_.reset();
    refitView();
    invalidateSceneCache();
    update();
}

void ProjectCanvas::setEditorState(EditorState *state) {
    state_ = state;
    setProject(state_ == nullptr ? nullptr : state_->project());
}

bool ProjectCanvas::loadGeometry(QString *error) {
    geometryLoaded_ = geometry_.loadDefault(error);
    rendererGeometryDirty_ = true;
    invalidateSceneCache();
    update();
    return geometryLoaded_;
}

QSizeF ProjectCanvas::shapeSize(int shapeId) const {
    return geometry_.shapeSize(shapeId);
}

QRectF ProjectCanvas::shapeInkBounds(int shapeId) const {
    return geometry_.shapeInkBounds(shapeId);
}

void ProjectCanvas::setTool(const QString &tool) {
    CanvasTool *next = nullptr;
    for (const std::unique_ptr<CanvasTool> &candidate : tools_) {
        if (candidate->name() == tool) {
            next = candidate.get();
            break;
        }
    }
    if (next == nullptr) {
        return;
    }
    cancelDrag();
    if (tool_ == QStringLiteral("pen") && tool != tool_) {
        pen_.resetHover();
        clearCursorHint();
    }
    if (tool_ == QStringLiteral("lining") && tool != tool_) {
        lining_.resetHover();
        clearCursorHint();
    }
    if (tool_ == QStringLiteral("bucket") && tool != tool_) {
        clearBucketPreview();
    }
    if (tool == QStringLiteral("pipette")) {
        if (tool_ != QStringLiteral("pipette")) {
            lastNonPipetteTool_ = tool_;
        }
    } else {
        lastNonPipetteTool_ = tool;
    }
    tool_ = tool;
    activeTool_ = next;
    hoverLayerId_.clear();
    hoverPolygon_ = {};
    if (state_ != nullptr) {
        state_->setToolName(tool_);
    }
    const QPoint cursorPoint = mapFromGlobal(QCursor::pos());
    updateCursorForPoint(cursorPoint);
    if (tool_ == QStringLiteral("pen") && !pen_.fillRunning) {
        refreshPenInteractionHint(cursorPoint, QGuiApplication::keyboardModifiers());
    }
    if (tool_ == QStringLiteral("lining") && !lining_.fillRunning) {
        refreshLiningInteractionHint(cursorPoint, QGuiApplication::keyboardModifiers());
    }
    update();
}

QString ProjectCanvas::tool() const {
    return tool_;
}

void ProjectCanvas::invalidateSelectionCache() {
    hitCacheDirty_ = true;
    selectionWorldBoundsCache_.reset();
    hoverLayerId_.clear();
    hoverPolygon_ = {};
    updateSelectionFlashState();
}

void ProjectCanvas::invalidateSceneCache() {
    invalidateSelectionCache();
    sectionCanvasCache_.clear();
}

void ProjectCanvas::invalidateGuideImageCache() {
    guideImageCache_.clear();
    sectionCanvasCache_.clear();
}

const fh6::scene::Group *ProjectCanvas::sceneTree() const {
    if (state_ != nullptr) {
        return state_->sceneRoot();
    }
    if (project_ == nullptr) {
        return nullptr;
    }
    return project_->root.get();
}

bool ProjectCanvas::isSectionActive(const QString &sectionGroupId) const {
    if (state_ == nullptr || project_ == nullptr || !project_->isLivery || state_->activeSectionId_.isEmpty()) {
        return true;
    }
    return sectionGroupId == state_->activeSectionId_;
}

void ProjectCanvas::forEachSceneShape(
    const std::function<bool(const fh6::scene::Shape &, const QTransform &, int)> &visit,
    bool reverse) const {
    if (state_ != nullptr) {
        const QVector<SceneRenderEntry> &entries = state_->renderEntries();
        const int n = entries.size();
        for (int i = 0; i < n; ++i) {
            const SceneRenderEntry &entry = entries[reverse ? n - 1 - i : i];
            if (entry.shape == nullptr) {
                continue;
            }
            if (!visit(*entry.shape, entry.worldTransform, entry.drawOrder)) {
                return;
            }
        }
        return;
    }
    const fh6::scene::Group *root = sceneTree();
    if (root == nullptr) {
        return;
    }
    const QVector<const fh6::scene::Shape *> leaves = sceneShapeLeaves(*root);
    const int n = leaves.size();
    for (int i = 0; i < n; ++i) {
        const int index = reverse ? n - 1 - i : i;
        const fh6::scene::Shape &shape = *leaves[index];
        if (!visit(shape, sceneWorldTransform(shape), index)) {
            return;
        }
    }
}

void ProjectCanvas::forEachSceneGuide(
    const std::function<bool(const fh6::scene::GuideLayer &, const QTransform &, const QString &)> &visit,
    bool reverse) const {
    if (state_ != nullptr) {
        const QVector<SceneRenderEntry> &entries = state_->renderEntries();
        const int n = entries.size();
        for (int i = 0; i < n; ++i) {
            const SceneRenderEntry &entry = entries[reverse ? n - 1 - i : i];
            if (entry.guide == nullptr) {
                continue;
            }
            if (!visit(*entry.guide, entry.worldTransform, entry.sectionGroupId)) {
                return;
            }
        }
        return;
    }
    const fh6::scene::Group *root = sceneTree();
    if (root == nullptr) {
        return;
    }
    const QVector<const fh6::scene::GuideLayer *> guides = sceneGuideLeaves(*root);
    const int n = guides.size();
    for (int i = 0; i < n; ++i) {
        const fh6::scene::GuideLayer &guide = *guides[reverse ? n - 1 - i : i];
        if (!visit(guide, sceneWorldTransform(guide), QString())) {
            return;
        }
    }
}

void ProjectCanvas::resetRelativeSelectionFrame() {
    frame_.layerSignature.clear();
    frame_.guideSignature.clear();
    frame_.referenceRotation = 0.0;
    invalidateSelectionCache();
}

void ProjectCanvas::setCanvasColor(const QColor &color) {
    options_.canvasColor = color.isValid() ? color : defaultCanvasColor(currentUiTheme());
    update();
}

QColor ProjectCanvas::canvasColor() const {
    return options_.canvasColor;
}

void ProjectCanvas::setTransformRelativeMode(bool relative) {
    if (options_.transformRelativeMode == relative) {
        return;
    }
    options_.transformRelativeMode = relative;
    frame_.layerSignature.clear();
    frame_.guideSignature.clear();
    invalidateSelectionCache();
    update();
}

void ProjectCanvas::setMoveToolAutoSelect(bool enabled) {
    options_.moveToolAutoSelect = enabled;
}

bool ProjectCanvas::moveToolAutoSelect() const {
    return options_.moveToolAutoSelect;
}

void ProjectCanvas::setAllowMoveOutsideBoundingBox(bool enabled) {
    options_.allowMoveOutsideBoundingBox = enabled;
}

void ProjectCanvas::setSelectionFlashEnabled(bool enabled) {
    if (flash_.enabled == enabled) {
        return;
    }
    flash_.enabled = enabled;
    if (!flash_.enabled) {
        setFlashingLayerIds({});
    } else {
        updateSelectionFlashState();
    }
    update();
}

bool ProjectCanvas::selectionFlashEnabled() const {
    return flash_.enabled;
}

void ProjectCanvas::setDisplayAnchorsDuringTransformDrag(bool enabled) {
    if (options_.displayAnchorsDuringTransformDrag == enabled) {
        return;
    }
    options_.displayAnchorsDuringTransformDrag = enabled;
    update();
}

void ProjectCanvas::setCarUnwrapOverlay(const QImage &overlay) {
    carUnwrapOverlay_ = overlay;
    update();
}

void ProjectCanvas::setCarUnwrapVisible(bool visible) {
    if (carUnwrapVisible_ == visible) {
        return;
    }
    carUnwrapVisible_ = visible;
    update();
}

bool ProjectCanvas::carUnwrapVisible() const {
    return carUnwrapVisible_;
}

bool ProjectCanvas::hasCarUnwrap() const {
    return !carUnwrapOverlay_.isNull();
}

void ProjectCanvas::setGuideLayersOnTop(bool enabled) {
    if (options_.guideLayersOnTop == enabled) {
        return;
    }
    options_.guideLayersOnTop = enabled;
    sectionCanvasCache_.clear();
    update();
}

void ProjectCanvas::setGuideLayersVisible(bool visible) {
    if (options_.guideLayersVisible == visible) {
        return;
    }
    options_.guideLayersVisible = visible;
    sectionCanvasCache_.clear();
    update();
}

bool ProjectCanvas::guideLayersOnTop() const {
    return options_.guideLayersOnTop;
}

void ProjectCanvas::setGuidelinesVisible(bool visible) {
    if (guidelines_.visible == visible) {
        return;
    }
    guidelines_.visible = visible;
    if (!visible && guidelines_.draggedOrientation != GuidelineOrientation::None) {
        if (state_ != nullptr) {
            state_->commitProjectEdit();
        }
        guidelines_.draggedOrientation = GuidelineOrientation::None;
        guidelines_.draggedIndex = -1;
    }
    updateCursorForPoint(mapFromGlobal(QCursor::pos()));
    update();
}

void ProjectCanvas::setGuidelinesLocked(bool locked) {
    if (guidelines_.locked == locked) {
        return;
    }
    guidelines_.locked = locked;
    if (locked && guidelines_.draggedOrientation != GuidelineOrientation::None) {
        if (state_ != nullptr) {
            state_->commitProjectEdit();
        }
        guidelines_.draggedOrientation = GuidelineOrientation::None;
        guidelines_.draggedIndex = -1;
    }
    updateCursorForPoint(mapFromGlobal(QCursor::pos()));
    update();
}

bool ProjectCanvas::guidelinesLocked() const {
    return guidelines_.locked;
}

void ProjectCanvas::setGuidelineColor(const QColor &color) {
    const QColor validColor = color.isValid() ? color : kDefaultGuidelineColor;
    if (guidelines_.color == validColor) {
        return;
    }
    guidelines_.color = validColor;
    update();
}

bool ProjectCanvas::deleteAllGuidelines() {
    if (project_ == nullptr
        || (project_->horizontalGuidelines.isEmpty() && project_->verticalGuidelines.isEmpty())) {
        return false;
    }
    if (guidelines_.draggedOrientation != GuidelineOrientation::None) {
        if (state_ != nullptr) {
            state_->commitProjectEdit();
        }
        guidelines_.draggedOrientation = GuidelineOrientation::None;
        guidelines_.draggedIndex = -1;
    }
    if (state_ != nullptr) {
        state_->beginProjectEdit();
    }
    project_->horizontalGuidelines.clear();
    project_->verticalGuidelines.clear();
    if (state_ != nullptr) {
        state_->commitProjectEdit();
    }
    guidelines_.draggedOrientation = GuidelineOrientation::None;
    guidelines_.draggedIndex = -1;
    update();
    return true;
}

void ProjectCanvas::setVisibilityBordersEnabled(bool enabled) {
    if (options_.visibilityBordersEnabled == enabled) {
        return;
    }
    options_.visibilityBordersEnabled = enabled;
    update();
}

void ProjectCanvas::setPositionLimitBorderEnabled(bool enabled) {
    if (options_.positionLimitBorderEnabled == enabled) {
        return;
    }
    options_.positionLimitBorderEnabled = enabled;
    update();
}

void ProjectCanvas::setVisibilityBorderResolution(const QSize &resolution) {
    const QSize validResolution = resolution.isValid() ? resolution : kDefaultVisibilityBorderResolution;
    if (options_.borderResolution == validResolution) {
        return;
    }
    options_.borderResolution = validResolution;
    update();
}

void ProjectCanvas::setNudgeSteps(double normalStep, double shiftStep) {
    options_.nudgeStep = normalStep > 0.0 ? normalStep : kDefaultNudgeStep;
    options_.nudgeShiftStep = shiftStep > 0.0 ? shiftStep : kDefaultNudgeShiftStep;
}

void ProjectCanvas::setPipetteColorPickedCallback(std::function<void(const QColor &)> callback) {
    pipetteColorPickedCallback_ = std::move(callback);
}

void ProjectCanvas::setPenFillRequestedCallback(
    std::function<void(const QVector<PenPoint> &, const std::optional<QColor> &)> callback) {
    penFillRequestedCallback_ = std::move(callback);
}

void ProjectCanvas::setPenFillCancelCallback(std::function<void()> callback) {
    penFillCancelCallback_ = std::move(callback);
}

QVector<PenPrimitive> ProjectCanvas::penPrimitiveCatalog() const {
    return buildPenPrimitiveCatalog(geometry_);
}

void ProjectCanvas::setPathFillRunning(PathInteraction &path, bool running, const QString &message) {
    path.fillRunning = running;
    path.fillMessage = running ? message : QString();
    if (!running) {
        path.crossings.clear();
        path.error.clear();
    }
    update();
}

void ProjectCanvas::cancelPathInteraction(PathInteraction &path, const std::function<void()> &cancelCallback) {
    const bool wasRunning = path.fillRunning;
    path.reset();
    clearCursorHint();
    if (wasRunning && cancelCallback != nullptr) {
        cancelCallback();
    }
    update();
}

void ProjectCanvas::setPenFillRunning(bool running, const QString &message) {
    setPathFillRunning(pen_, running, message);
}

void ProjectCanvas::cancelPenInteraction() {
    cancelPathInteraction(pen_, penFillCancelCallback_);
}

void ProjectCanvas::setLiningFillRequestedCallback(
    std::function<void(const QVector<PenPoint> &, double, const std::optional<QColor> &)> callback) {
    liningFillRequestedCallback_ = std::move(callback);
}

void ProjectCanvas::setLiningFillCancelCallback(std::function<void()> callback) {
    liningFillCancelCallback_ = std::move(callback);
}

void ProjectCanvas::setLiningWidthChangedCallback(std::function<void(double)> callback) {
    liningWidthChangedCallback_ = std::move(callback);
}

QVector<PenPrimitive> ProjectCanvas::liningPrimitiveCatalog() const {
    return buildLiningPrimitiveCatalog(geometry_);
}

void ProjectCanvas::setLiningFillRunning(bool running, const QString &message) {
    setPathFillRunning(lining_, running, message);
}

void ProjectCanvas::cancelLiningInteraction() {
    cancelPathInteraction(lining_, liningFillCancelCallback_);
}

void ProjectCanvas::setLiningWidth(double width) {
    if (!std::isfinite(width) || width <= 0.0) {
        return;
    }
    const double adjusted = std::clamp(width, kMinimumLiningWidth, kMaximumLiningWidth);
    if (qFuzzyCompare(liningWidth_, adjusted)) {
        return;
    }
    liningWidth_ = adjusted;
    validateLiningInteraction();
    if (liningWidthChangedCallback_ != nullptr) {
        liningWidthChangedCallback_(liningWidth_);
    }
    update();
}

double ProjectCanvas::liningWidth() const {
    return liningWidth_;
}

void ProjectCanvas::adjustLiningWidth(double delta, const QPointF &screenPoint) {
    setLiningWidth(liningWidth_ + delta);
    refreshLiningInteractionHint(screenPoint, QGuiApplication::keyboardModifiers());
}

void ProjectCanvas::closePenPath() {
    const double worldPerPixel = camera_.worldPerPixel();
    const PenContour contour = buildPenContour(pen_.points, worldPerPixel * 0.25);
    if (!contour.valid()) {
        pen_.crossings = contour.crossings;
        pen_.error = contour.error.isEmpty() ? QStringLiteral("Invalid Pen path") : contour.error;
        refreshPenInteractionHint(worldToScreen(pen_.hoverWorld),
                                  QGuiApplication::keyboardModifiers());
        update();
        return;
    }
    if (penFillRequestedCallback_ == nullptr) {
        pen_.error = QStringLiteral("Pen fill is unavailable");
        refreshPenInteractionHint(worldToScreen(pen_.hoverWorld),
                                  QGuiApplication::keyboardModifiers());
        update();
        return;
    }
    const QVector<PenPoint> points = pen_.points;
    const std::optional<QColor> fillColor = pen_.fillColor;
    pen_.resetHover();
    pen_.crossings.clear();
    pen_.error.clear();
    clearCursorHint();
    pen_.fillRunning = true;
    pen_.fillMessage = QStringLiteral("Filling Pen path…");
    update();
    penFillRequestedCallback_(points, fillColor);
}

void ProjectCanvas::requestLiningFill() {
    validateLiningInteraction();
    if (!lining_.error.isEmpty()) {
        refreshLiningInteractionHint(worldToScreen(lining_.hoverWorld),
                                     QGuiApplication::keyboardModifiers());
        update();
        return;
    }
    if (liningFillRequestedCallback_ == nullptr) {
        lining_.error = QStringLiteral("Lining fill is unavailable");
        refreshLiningInteractionHint(worldToScreen(lining_.hoverWorld),
                                     QGuiApplication::keyboardModifiers());
        update();
        return;
    }
    const QVector<PenPoint> points = lining_.points;
    const std::optional<QColor> fillColor = lining_.fillColor;
    lining_.resetHover();
    lining_.error.clear();
    clearCursorHint();
    lining_.fillRunning = true;
    lining_.fillMessage = QStringLiteral("Filling lining path…");
    update();
    liningFillRequestedCallback_(points, liningWidth_, fillColor);
}

void ProjectCanvas::refitView() {
    camera_.setViewBounds(projectBounds());
    updateViewTransform();
}

bool ProjectCanvas::centerViewOnSelection() {
    if (project_ == nullptr || state_ == nullptr) {
        return false;
    }
    updateViewTransform();
    const QRectF bounds = cachedSelectionWorldBounds();
    if (!bounds.isValid() || bounds.isEmpty()) {
        return false;
    }
    const QPointF selectionScreenCenter = worldToScreen(bounds.center());
    camera_.adjustPan(QPointF(kRulerExtent + (width() - kRulerExtent) * 0.5,
                              kRulerExtent + (height() - kRulerExtent) * 0.5) - selectionScreenCenter);
    invalidateSceneCache();
    update();
    return true;
}

QPointF ProjectCanvas::viewCenterWorld() {
    updateViewTransform();
    return screenToWorld(QPointF(kRulerExtent + (width() - kRulerExtent) * 0.5,
                                 kRulerExtent + (height() - kRulerExtent) * 0.5));
}

QRectF ProjectCanvas::projectBounds() const {
    BoundsAccumulator acc;
    forEachSceneShape([&](const fh6::scene::Shape &shape, const QTransform &world, int) {
        if (shape.visible) {
            acc.add(world, flatEntryVisualRect(shape, geometry_));
        }
        return true;
    }, /*reverse=*/false);
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        const QSizeF size = sceneNodeSize(guide, geometry_);
        if (guide.visible && size.width() > 0 && size.height() > 0 && isSectionActive(sectionGroupId)) {
            acc.add(world, sceneLocalRect(size));
        }
        return true;
    }, /*reverse=*/false);
    if (!acc.hasBounds() || acc.bounds().isEmpty()) {
        return kDefaultProjectBounds;
    }
    return acc.bounds();
}

void ProjectCanvas::updateViewTransform() {
    if (!camera_.hasViewBounds()) {
        camera_.setViewBounds(projectBounds());
    }
    const double viewportWidth = std::max(width() - kRulerExtent, 1.0);
    const double viewportHeight = std::max(height() - kRulerExtent, 1.0);
    camera_.recompute(QPointF(kRulerExtent, kRulerExtent), viewportWidth, viewportHeight);
}

QPointF ProjectCanvas::worldToScreen(const QPointF &point) const {
    return camera_.worldToScreen(point);
}

QPointF ProjectCanvas::screenToWorld(const QPointF &point) const {
    return camera_.screenToWorld(point);
}

QPolygonF ProjectCanvas::screenQuad(const QTransform &entryToWorld, const QRectF &localRect) const {
    QPolygonF polygon;
    polygon << worldToScreen(entryToWorld.map(localRect.topLeft()))
            << worldToScreen(entryToWorld.map(localRect.topRight()))
            << worldToScreen(entryToWorld.map(localRect.bottomRight()))
            << worldToScreen(entryToWorld.map(localRect.bottomLeft()));
    return polygon;
}

} // namespace gui
