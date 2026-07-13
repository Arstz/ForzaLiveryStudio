#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

using namespace pc_detail;

namespace {

const QRectF DefaultProjectBounds(-128.0, -128.0, 256.0, 256.0);

} // namespace

ProjectCanvas::ProjectCanvas(QWidget *parent)
    : QOpenGLWidget(parent)
    , canvasColor_(canvasColorForTheme(currentUiTheme(), loadCanvasColorSettings()))
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    setFormat(format);
    setMinimumSize(320, 240);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    selectionFlashClock_.start();
    selectionFlashTimer_.setInterval(SelectionFlashFrameMs);
    connect(&selectionFlashTimer_, &QTimer::timeout, this, [this]() {
        update();
        scheduleSelectionFlashTimer();
    });

    tools_.push_back(std::make_unique<SelectTool>(*this));
    tools_.push_back(std::make_unique<MoveTool>(*this));
    tools_.push_back(std::make_unique<MarqueeTool>(*this));
    tools_.push_back(std::make_unique<TransformTool>(*this));
    tools_.push_back(std::make_unique<RotateTool>(*this));
    tools_.push_back(std::make_unique<PipetteTool>(*this));
    activeTool_ = tools_.front().get();
}

ProjectCanvas::~ProjectCanvas() = default;

void ProjectCanvas::setProject(fh6::Project *project)
{
    project_ = project;
    guideImageCache_.clear();
    sectionCanvasCache_.clear();
    zoom_ = 1.0;
    pan_ = {};
    refitView();
    invalidateSceneCache();
    update();
}

void ProjectCanvas::setEditorState(EditorState *state)
{
    state_ = state;
    setProject(state_ == nullptr ? nullptr : state_->project());
}

bool ProjectCanvas::loadGeometry(QString *error)
{
    geometryLoaded_ = geometry_.loadDefault(error);
    rendererGeometryDirty_ = true;
    invalidateSceneCache();
    update();
    return geometryLoaded_;
}

QSizeF ProjectCanvas::shapeSize(int shapeId) const
{
    return geometry_.shapeSize(shapeId);
}

QRectF ProjectCanvas::shapeInkBounds(int shapeId) const
{
    return geometry_.shapeInkBounds(shapeId);
}

void ProjectCanvas::setTool(const QString &tool)
{
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
    tool_ = tool;
    activeTool_ = next;
    hoverLayerId_.clear();
    hoverPolygon_ = {};
    if (state_ != nullptr) {
        state_->setToolName(tool_);
    }
    updateCursorForPoint(mapFromGlobal(QCursor::pos()));
    update();
}

QString ProjectCanvas::tool() const
{
    return tool_;
}

void ProjectCanvas::invalidateSelectionCache()
{
    hitCacheDirty_ = true;
    selectionWorldBoundsCache_.reset();
    hoverLayerId_.clear();
    hoverPolygon_ = {};
    updateSelectionFlashState();
}

void ProjectCanvas::invalidateSceneCache()
{
    invalidateSelectionCache();
    sectionCanvasCache_.clear();
}

const fh6::scene::Group *ProjectCanvas::sceneTree() const
{
    if (state_ != nullptr) {
        return state_->sceneRoot();
    }
    if (project_ == nullptr) {
        return nullptr;
    }
    return project_->root.get();
}

bool ProjectCanvas::isSectionActive(const QString &sectionGroupId) const
{
    if (state_ == nullptr || project_ == nullptr || !project_->isLivery || state_->activeSectionId_.isEmpty()) {
        return true;
    }
    return sectionGroupId == state_->activeSectionId_;
}

void ProjectCanvas::forEachSceneShape(
    const std::function<bool(const fh6::scene::Shape &, const QTransform &, int)> &visit,
    bool reverse) const
{
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
    bool reverse) const
{
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

void ProjectCanvas::resetRelativeSelectionFrame()
{
    frameLayerSignature_.clear();
    frameGuideSignature_.clear();
    frameReferenceRotation_ = 0.0;
    invalidateSelectionCache();
}

void ProjectCanvas::setCanvasColor(const QColor &color)
{
    canvasColor_ = color.isValid() ? color : defaultCanvasColor(currentUiTheme());
    update();
}

QColor ProjectCanvas::canvasColor() const
{
    return canvasColor_;
}

void ProjectCanvas::setTransformRelativeMode(bool relative)
{
    if (transformRelativeMode_ == relative) {
        return;
    }
    transformRelativeMode_ = relative;
    frameLayerSignature_.clear();
    frameGuideSignature_.clear();
    invalidateSelectionCache();
    update();
}

void ProjectCanvas::setMoveToolAutoSelect(bool enabled)
{
    moveToolAutoSelect_ = enabled;
}

bool ProjectCanvas::moveToolAutoSelect() const
{
    return moveToolAutoSelect_;
}

void ProjectCanvas::setSelectionFlashEnabled(bool enabled)
{
    if (selectionFlashEnabled_ == enabled) {
        return;
    }
    selectionFlashEnabled_ = enabled;
    if (!selectionFlashEnabled_) {
        setFlashingLayerIds({});
    } else {
        updateSelectionFlashState();
    }
    update();
}

bool ProjectCanvas::selectionFlashEnabled() const
{
    return selectionFlashEnabled_;
}

void ProjectCanvas::setCarUnwrapOverlay(const QImage &overlay)
{
    carUnwrapOverlay_ = overlay;
    if (carUnwrapOverlay_.isNull()) {
        carUnwrapVisible_ = false;
    }
    update();
}

void ProjectCanvas::setCarUnwrapVisible(bool visible)
{
    if (carUnwrapVisible_ == visible) {
        return;
    }
    carUnwrapVisible_ = visible && !carUnwrapOverlay_.isNull();
    update();
}

bool ProjectCanvas::carUnwrapVisible() const
{
    return carUnwrapVisible_;
}

bool ProjectCanvas::hasCarUnwrap() const
{
    return !carUnwrapOverlay_.isNull();
}

void ProjectCanvas::setGuideLayersOnTop(bool enabled)
{
    if (guideLayersOnTop_ == enabled) {
        return;
    }
    guideLayersOnTop_ = enabled;
    sectionCanvasCache_.clear();
    update();
}

bool ProjectCanvas::guideLayersOnTop() const
{
    return guideLayersOnTop_;
}

void ProjectCanvas::setVisibilityBordersEnabled(bool enabled)
{
    if (visibilityBordersEnabled_ == enabled) {
        return;
    }
    visibilityBordersEnabled_ = enabled;
    update();
}

void ProjectCanvas::setPositionLimitBorderEnabled(bool enabled)
{
    if (positionLimitBorderEnabled_ == enabled) {
        return;
    }
    positionLimitBorderEnabled_ = enabled;
    update();
}

void ProjectCanvas::setVisibilityBorderResolution(const QSize &resolution)
{
    const QSize validResolution = resolution.isValid() ? resolution : QSize(1920, 1080);
    if (visibilityBorderResolution_ == validResolution) {
        return;
    }
    visibilityBorderResolution_ = validResolution;
    update();
}

void ProjectCanvas::setNudgeSteps(double normalStep, double shiftStep)
{
    nudgeStep_ = normalStep > 0.0 ? normalStep : 0.1;
    nudgeShiftStep_ = shiftStep > 0.0 ? shiftStep : 1.0;
}

void ProjectCanvas::setPipetteColorPickedCallback(std::function<void(const QColor &)> callback)
{
    pipetteColorPickedCallback_ = std::move(callback);
}

void ProjectCanvas::refitView()
{
    viewBounds_ = projectBounds();
    currentBounds_ = viewBounds_;
    updateViewTransform();
}

bool ProjectCanvas::centerViewOnSelection()
{
    if (project_ == nullptr || state_ == nullptr) {
        return false;
    }
    updateViewTransform();
    const QRectF bounds = cachedSelectionWorldBounds();
    if (!bounds.isValid() || bounds.isEmpty()) {
        return false;
    }
    const QPointF selectionScreenCenter = worldToScreen(bounds.center());
    pan_ += QPointF(width() * 0.5, height() * 0.5) - selectionScreenCenter;
    invalidateSceneCache();
    update();
    return true;
}

QPointF ProjectCanvas::viewCenterWorld()
{
    updateViewTransform();
    return screenToWorld(QPointF(width() * 0.5, height() * 0.5));
}

QRectF ProjectCanvas::projectBounds() const
{
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
        return DefaultProjectBounds;
    }
    return acc.bounds();
}

void ProjectCanvas::updateViewTransform()
{
    if (!viewBounds_.isValid() || viewBounds_.isEmpty()) {
        viewBounds_ = projectBounds();
    }
    currentBounds_ = viewBounds_;
    const double paddedWidth = std::max(currentBounds_.width() * 1.16, 1.0);
    const double paddedHeight = std::max(currentBounds_.height() * 1.16, 1.0);
    baseScale_ = std::min(width() / paddedWidth, height() / paddedHeight);
    const QPointF center = currentBounds_.center();

    worldToScreen_.reset();
    worldToScreen_.translate(width() * 0.5 + pan_.x(), height() * 0.5 + pan_.y());
    worldToScreen_.scale(baseScale_ * zoom_, -baseScale_ * zoom_);
    worldToScreen_.translate(-center.x(), -center.y());
    screenToWorld_ = worldToScreen_.inverted();
}

QPointF ProjectCanvas::worldToScreen(const QPointF &point) const
{
    return worldToScreen_.map(point);
}

QPointF ProjectCanvas::screenToWorld(const QPointF &point) const
{
    return screenToWorld_.map(point);
}

QPolygonF ProjectCanvas::screenQuad(const QTransform &entryToWorld, const QRectF &localRect) const
{
    QPolygonF polygon;
    polygon << worldToScreen(entryToWorld.map(localRect.topLeft()))
            << worldToScreen(entryToWorld.map(localRect.topRight()))
            << worldToScreen(entryToWorld.map(localRect.bottomRight()))
            << worldToScreen(entryToWorld.map(localRect.bottomLeft()));
    return polygon;
}

} // namespace gui
