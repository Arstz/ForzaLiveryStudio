#include "project_canvas.h"

#include "canvas_tools.h"
#include "editor_state.h"
#include "gui_constants.h"
#include "matrix_math.h"
#include "scene_entry.h"
#include "theme_manager.h"

#include <QKeyEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace gui {
namespace {

constexpr double HandleHalf = 6.0;            // drawn handle marker half-size
// Scale lives in a band that straddles each edge: it reaches ScaleGrabInside into the box
// (so the visible edge itself grabs Scale) and ScaleGrabOutside past it, while the interior
// beyond the band stays Move. Biased outward so the box interior reads as Move almost
// everywhere. Where two edge bands meet (the corners) Scale becomes two-axis.
constexpr double ScaleGrabInside = 12.0;
constexpr double ScaleGrabOutside = 12.0;
// Rotate is the outer-anchor affordance: it lives strictly outside the box, in the diagonal
// region past each corner, out to this reach. Scale is resolved first, so along the sides the
// scale band wins and Rotate only claims the area past the corner anchors.
constexpr double RotateCornerReach = 131.0;
constexpr double SkewHandleOffset = 30.0;
constexpr double ClickDragThreshold = 5.0;
constexpr qint64 SelectionFlashDurationMs = 750;
constexpr qint64 SelectionFlashPeriodMs = 3750;
constexpr int SelectionFlashFrameMs = 33;

// Overlay styling. These were inlined literals in drawOverlay()/drawCursorHint();
// naming them keeps the paint routines declarative and the values in one place.
const QColor SelectionAccentColor(255, 200, 50);   // hover halo + marquee accent
const QColor OverlayHaloColor(0, 0, 0);            // dark halo drawn behind bright strokes
const QColor SelectionFrameColor(255, 255, 255);   // selection box outline + handle fill
constexpr double HoverHaloWidth = 4.0;
constexpr double HoverAccentWidth = 2.0;
constexpr double SelectionFrameHaloWidth = 3.0;
constexpr double SelectionFrameLineWidth = 1.0;
constexpr double HandleBorderWidth = 2.0;
constexpr int MarqueeFillAlpha = 32;

// Cursor hint bubble styling.
const QColor CursorHintBorderColor(0, 0, 0, 180);
const QColor CursorHintFillColor(20, 20, 22, 210);
const QColor CursorHintTextColor(245, 246, 248);
constexpr double CursorHintCornerRadius = 4.0;
constexpr int CursorHintPaddingX = 8;
constexpr int CursorHintPaddingY = 6;
constexpr double CursorHintCursorOffset = 18.0;   // gap from cursor to bubble
constexpr double CursorHintScreenMargin = 4.0;    // keep bubble inside the viewport

const QColor EmptyCanvasTextColor(190, 194, 201);

// Fallback view extent when there is no project or nothing visible to fit:
// the default 256-unit vinyl canvas centred on the origin.
const QRectF DefaultProjectBounds(-128.0, -128.0, 256.0, 256.0);

QString assetPath(const QString &fileName)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    QStringList candidates;
    candidates << QDir(appDir).filePath(QStringLiteral("assets/%1").arg(fileName))
               << QDir(cwd).filePath(QStringLiteral("assets/%1").arg(fileName))
               << QDir(cwd).filePath(QStringLiteral("cpp-port/assets/%1").arg(fileName));
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return candidates.front();
}

// Standard logical cursor size; cursor art is authored at 2x (64 px) for this.
constexpr int LogicalCursorSize = 32;

// Device pixel ratio of the primary screen (>= 1). The cursor pixmap is rendered
// at this many physical pixels per logical pixel and tagged with it, so it stays
// LogicalCursorSize *logical* pixels on every display.
double cursorScaleFactor()
{
    double scale = 1.0;
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        scale = std::max(screen->devicePixelRatio(), 1.0);
    }
    return scale;
}

QCursor assetCursor(const QString &fileName)
{
    // Cursors are requested on every mouse-move; cache them so we never hit the
    // filesystem (stat + pixmap decode) on the interactive hot path.
    static QHash<QString, QCursor> cache;
    const auto cached = cache.constFind(fileName);
    if (cached != cache.constEnd()) {
        return cached.value();
    }

    QCursor cursor;
    QPixmap pixmap(assetPath(fileName));
    if (pixmap.isNull()) {
        cursor = QCursor(Qt::ArrowCursor);
        cache.insert(fileName, cursor);
        return cursor;
    }

    // Render the art at LogicalCursorSize logical pixels, tagging the pixmap with the
    // screen dpr so it stays that logical size (and crisp) on hi-dpi displays. The tool
    // cursors carry their own padding inside the art, but the arrow (Cursor.xpm) is drawn
    // edge-to-edge, so it would otherwise render about twice as large as a normal pointer;
    // halve its target to bring it back to the standard small size.
    const double scale = cursorScaleFactor();
    const double logical = LogicalCursorSize * (fileName == QStringLiteral("Cursor.xpm") ? 0.5 : 1.0);
    const int target = std::max(1, qRound(logical * scale));
    if (pixmap.width() != target || pixmap.height() != target) {
        pixmap = pixmap.scaled(target, target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    pixmap.setDevicePixelRatio(scale);

    // The default arrow points with its top-left tip, so its hot spot stays at the
    // corner. Every transform-tool cursor (scale, skew, rotate) acts around the
    // selection, so its hot spot is centred  Ekeeping the active point consistent as
    // the cursor swaps between tools.
    if (fileName == QStringLiteral("Cursor.xpm")) {
        cursor = QCursor(pixmap, 0, 0);
    } else {
        cursor = QCursor(pixmap, pixmap.width() / 2, pixmap.height() / 2);
    }
    cache.insert(fileName, cursor);
    return cursor;
}

// Reuse the canonical [0,360) wrap from core rather than shadowing it here.
using fh6::normalizeRotation;

double snapRotation(double degrees, Qt::KeyboardModifiers modifiers)
{
    if (modifiers & Qt::ShiftModifier) {
        return std::round(degrees / 15.0) * 15.0;
    }
    return degrees;
}

QPointF constrainDelta(QPointF delta, Qt::KeyboardModifiers modifiers)
{
    if (!(modifiers & Qt::ShiftModifier)) {
        return delta;
    }
    if (std::abs(delta.x()) >= std::abs(delta.y())) {
        return {delta.x(), 0.0};
    }
    return {0.0, delta.y()};
}

QString formatHintNumber(double value, int decimals = 2)
{
    if (std::abs(value) < 0.005) {
        value = 0.0;
    }
    return QString::number(value, 'f', decimals);
}

// Decomposed affine result shared by the Shape/Guide scale-drag loops. ok is false
// when the X axis collapsed (degenerate); skew falls back to fallbackSkew then.
struct ScaleDecomposition {
    bool ok = false;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    double skew = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
};

ScaleDecomposition decomposeScaleResult(const QTransform &result, double fallbackSkew)
{
    const double a = result.m11();
    const double b = result.m12();
    const double c = result.m21();
    const double d = result.m22();
    const double scaleXLen = std::hypot(a, b);
    if (scaleXLen < 1e-9) {
        return {};
    }
    const double det = a * d - b * c;
    ScaleDecomposition out;
    out.ok = true;
    out.x = result.dx();
    out.y = result.dy();
    out.rotation = normalizeRotation(std::atan2(b, a) * 180.0 / kPi);
    out.skew = std::abs(det) > 1e-9 ? (a * c + b * d) / det : fallbackSkew;
    out.scaleX = std::clamp(scaleXLen, -100.0, 100.0);
    out.scaleY = std::clamp(det / scaleXLen, -100.0, 100.0);
    return out;
}

// Which box axes a named scale handle drives ("top_left" -> left + top, ...).
struct HandleAxes {
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;
};

HandleAxes handleAxes(const QString &handle)
{
    return {handle.contains(QStringLiteral("left")), handle.contains(QStringLiteral("right")),
            handle.contains(QStringLiteral("top")), handle.contains(QStringLiteral("bottom"))};
}

// Single source for the per-handle cursor: the native shape (used while a scale/skew
// drag is active) and the themed cursor art (used when hovering the transform box).
struct HandleCursorSpec {
    Qt::CursorShape shape;
    const char *icon;
};

const QHash<QString, HandleCursorSpec> &handleCursorSpecs()
{
    static const QHash<QString, HandleCursorSpec> specs = {
        {QStringLiteral("left"), {Qt::SizeHorCursor, "ToolScaleX.xpm"}},
        {QStringLiteral("right"), {Qt::SizeHorCursor, "ToolScaleX.xpm"}},
        {QStringLiteral("top"), {Qt::SizeVerCursor, "ToolScaleY.xpm"}},
        {QStringLiteral("bottom"), {Qt::SizeVerCursor, "ToolScaleY.xpm"}},
        {QStringLiteral("top_right"), {Qt::SizeBDiagCursor, "ToolScaleNESW.xpm"}},
        {QStringLiteral("bottom_left"), {Qt::SizeBDiagCursor, "ToolScaleNESW.xpm"}},
        {QStringLiteral("top_left"), {Qt::SizeFDiagCursor, "ToolScaleNWSE.xpm"}},
        {QStringLiteral("bottom_right"), {Qt::SizeFDiagCursor, "ToolScaleNWSE.xpm"}},
        {QStringLiteral("skew"), {Qt::SizeHorCursor, "ToolTransformSkew.xpm"}},
    };
    return specs;
}

bool handleAnchorLocalPoints(const QString &handle, const QRectF &rect, QPointF *handlePoint, QPointF *anchorPoint)
{
    const HandleAxes axes = handleAxes(handle);
    if ((!axes.left && !axes.right && !axes.top && !axes.bottom) || handle == QStringLiteral("skew")) {
        return false;
    }
    const double centerX = rect.center().x();
    const double centerY = rect.center().y();
    const double handleX = axes.left ? rect.left() : (axes.right ? rect.right() : centerX);
    const double handleY = axes.top ? rect.top() : (axes.bottom ? rect.bottom() : centerY);
    const double anchorX = axes.left ? rect.right() : (axes.right ? rect.left() : centerX);
    const double anchorY = axes.top ? rect.bottom() : (axes.bottom ? rect.top() : centerY);
    *handlePoint = QPointF(handleX, handleY);
    *anchorPoint = QPointF(anchorX, anchorY);
    return QLineF(*handlePoint, *anchorPoint).length() > 1e-9;
}

QString rotateHandleForLocalPoint(const QPointF &local, const QRectF &rect)
{
    const bool west = local.x() < rect.left();
    const bool east = local.x() > rect.right();
    const bool north = local.y() < rect.top();
    const bool south = local.y() > rect.bottom();
    if (north && west) {
        return QStringLiteral("top_left");
    }
    if (north && east) {
        return QStringLiteral("top_right");
    }
    if (south && west) {
        return QStringLiteral("bottom_left");
    }
    if (south && east) {
        return QStringLiteral("bottom_right");
    }
    if (north) {
        return QStringLiteral("top");
    }
    if (south) {
        return QStringLiteral("bottom");
    }
    if (west) {
        return QStringLiteral("left");
    }
    if (east) {
        return QStringLiteral("right");
    }
    return {};
}

QString rotateCursorSuffixForScreenDelta(const QPointF &delta)
{
    if (std::hypot(delta.x(), delta.y()) < 1e-6) {
        return {};
    }
    double angle = std::atan2(-delta.y(), delta.x());
    if (angle < 0.0) {
        angle += 2.0 * kPi;
    }
    const int sector = static_cast<int>(std::floor((angle + kPi / 8.0) / (kPi / 4.0))) % 8;
    switch (sector) {
    case 0: // screen east
        return QStringLiteral("W");
    case 1: // screen north-east; rotate cursor assets have inverted vertical and horizontal names
        return QStringLiteral("SW");
    case 2: // screen north
        return QStringLiteral("S");
    case 3: // screen north-west
        return QStringLiteral("SE");
    case 4: // screen west
        return QStringLiteral("E");
    case 5: // screen south-west
        return QStringLiteral("NE");
    case 6: // screen south
        return QStringLiteral("N");
    case 7: // screen south-east
        return QStringLiteral("NW");
    default:
        return {};
    }
}

HandleCursorSpec scaleCursorSpecForScreenDelta(const QPointF &delta)
{
    const QString suffix = rotateCursorSuffixForScreenDelta(delta);
    if (suffix.isEmpty()) {
        return {Qt::ArrowCursor, "ToolbarScale.xpm"};
    }
    if (suffix == QStringLiteral("E") || suffix == QStringLiteral("W")) {
        return {Qt::SizeHorCursor, "ToolScaleX.xpm"};
    }
    if (suffix == QStringLiteral("N") || suffix == QStringLiteral("S")) {
        return {Qt::SizeVerCursor, "ToolScaleY.xpm"};
    }
    if (suffix == QStringLiteral("NE") || suffix == QStringLiteral("SW")) {
        return {Qt::SizeBDiagCursor, "ToolScaleNESW.xpm"};
    }
    return {Qt::SizeFDiagCursor, "ToolScaleNWSE.xpm"};
}

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
    setMinimumSize(640, 480);
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
    guideImageCache_.clear();
    sectionCanvasCache_.clear();
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
    // Force the multi-selection frame reference to be recaptured so the box starts axis-aligned
    // for the current selection when Relative mode is switched on.
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

void ProjectCanvas::refitView()
{
    viewBounds_ = projectBounds();
    currentBounds_ = viewBounds_;
    updateViewTransform();
}

QPointF ProjectCanvas::viewCenterWorld()
{
    updateViewTransform();
    return screenToWorld(QPointF(width() * 0.5, height() * 0.5));
}

QRectF ProjectCanvas::projectBounds() const
{
    BoundsAccumulator acc;
    if (project_ != nullptr) {
        for (const fh6::ShapeLayer &layer : project_->layers) {
            if (!layer.visible) {
                continue;
            }
            acc.add(entryTransform(layer), entryLocalRect(geometry_.shapeSize(layer.shapeId)));
        }
        for (const fh6::GuideLayer &guide : project_->guideLayers) {
            if (!guide.visible || guide.width <= 0 || guide.height <= 0) {
                continue;
            }
            acc.add(entryTransform(guide), entryLocalRect(guide));
        }
    }
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

QPolygonF ProjectCanvas::layerScreenPolygon(const fh6::ShapeLayer &layer) const
{
    return screenQuad(entryTransform(layer), entryLocalRect(geometry_.shapeSize(layer.shapeId)));
}

QPolygonF ProjectCanvas::guideScreenPolygon(const fh6::GuideLayer &guide) const
{
    return screenQuad(entryTransform(guide), entryLocalRect(guide));
}

QVector<ProjectCanvas::HitEntry> ProjectCanvas::hitEntries()
{
    if (!hitCacheDirty_) {
        return hitCache_;
    }
    updateViewTransform();
    hitCache_.clear();
    if (project_ == nullptr) {
        hitCacheDirty_ = false;
        return hitCache_;
    }
    for (int index = project_->layers.size() - 1; index >= 0; --index) {
        const fh6::ShapeLayer &layer = project_->layers[index];
        if (!layer.visible) {
            continue;  // not drawn (e.g. inactive C_livery section) => not pickable
        }
        const QPolygonF polygon = layerScreenPolygon(layer);
        hitCache_.push_back({index, layer.id, polygon, polygon.boundingRect()});
    }
    hitCacheDirty_ = false;
    return hitCache_;
}

QString ProjectCanvas::guideAtScreenPoint(const QPointF &point)
{
    if (project_ == nullptr) {
        return {};
    }
    updateViewTransform();
    for (int index = project_->guideLayers.size() - 1; index >= 0; --index) {
        const fh6::GuideLayer &guide = project_->guideLayers[index];
        if (!guide.visible) {
            continue;
        }
        const QPolygonF polygon = guideScreenPolygon(guide);
        if (polygon.boundingRect().contains(point) && pointInPolygon(point, polygon)) {
            return guide.id;
        }
    }
    return {};
}

QVector<QString> ProjectCanvas::layersAtScreenPoint(const QPointF &point)
{
    QVector<QString> ids;
    for (const HitEntry &entry : hitEntries()) {
        if (entry.screenBounds.contains(point) && pointInPolygon(point, entry.screenPolygon)) {
            ids.push_back(entry.layerId);
        }
    }
    return ids;
}

QString ProjectCanvas::selectTargetAtScreenPoint(const QPointF &point, Qt::KeyboardModifiers modifiers)
{
    const QVector<QString> hits = layersAtScreenPoint(point);
    if (hits.isEmpty()) {
        return {};
    }
    if (modifiers & Qt::ControlModifier) {
        return hits.front();
    }

    QString target = hits.front();
    if (state_ != nullptr) {
        const QSet<QString> selected = state_->selectedLayerIds();
        for (int i = 0; i < hits.size(); ++i) {
            if (selected.contains(hits[i])) {
                target = hits[(i + 1) % hits.size()];
                break;
            }
        }
    }
    return target;
}

const QRectF &ProjectCanvas::cachedSelectionWorldBounds() const
{
    // Cache the world-axis union of the selection's bounds (invalidated on any
    // selection/geometry change) so the selection-flash repaints don't rescan every layer each
    // frame. Computed inline from the selection id sets - NOT via selectedLayers(), which calls
    // lockedLayerIds() (a full buildEntryMaps) and would drop the box entirely if any selected
    // layer were locked; this path runs every paint/drag frame.
    if (!selectionWorldBoundsCache_.has_value()) {
        BoundsAccumulator acc;
        if (project_ != nullptr && state_ != nullptr) {
            const QSet<QString> selected = state_->selectedLayerIds();
            if (!selected.isEmpty()) {
                for (const fh6::ShapeLayer &layer : project_->layers) {
                    if (!selected.contains(layer.id)) {
                        continue;
                    }
                    acc.add(entryTransform(layer), entryLocalRect(geometry_.shapeSize(layer.shapeId)));
                }
            }
            const QSet<QString> selectedGuides = state_->selectedGuideLayerIds();
            if (!selectedGuides.isEmpty()) {
                for (const fh6::GuideLayer &guide : project_->guideLayers) {
                    if (!selectedGuides.contains(guide.id)) {
                        continue;
                    }
                    acc.add(entryTransform(guide), entryLocalRect(guide));
                }
            }
        }
        selectionWorldBoundsCache_ = acc.hasBounds() ? acc.bounds() : QRectF();
    }
    return *selectionWorldBoundsCache_;
}

QRectF ProjectCanvas::selectedScreenBounds() const
{
    if (project_ == nullptr || state_ == nullptr) {
        return {};
    }
    // The world-AABB box and the view have no rotation, so the screen bounds are just the world
    // bounds mapped through the current view transform.
    const QRectF worldBounds = cachedSelectionWorldBounds();
    if (!worldBounds.isValid()) {
        return {};
    }
    return worldToScreen_.mapRect(worldBounds);
}

ProjectCanvas::SelectionBox ProjectCanvas::currentSelectionBox() const
{
    SelectionBox box;
    if (project_ == nullptr || state_ == nullptr) {
        return box;
    }
    const QSet<QString> selLayers = state_->selectedLayerIds();
    const QSet<QString> selGuides = state_->selectedGuideLayerIds();
    const int count = selLayers.size() + selGuides.size();
    if (count == 0) {
        return box;
    }

    // Detect a selection change so the Relative multi-selection box's reference rotation can be
    // recaptured (lazy, so it never resets mid-drag where the selection is stable).
    const bool signatureChanged = selLayers != frameLayerSignature_ || selGuides != frameGuideSignature_;
    if (signatureChanged) {
        frameLayerSignature_ = selLayers;
        frameGuideSignature_ = selGuides;
    }

    // Absolute mode (default): identity frame over the world-axis AABB - reduces to legacy.
    if (!transformRelativeMode_) {
        const QRectF worldBounds = cachedSelectionWorldBounds();
        if (!worldBounds.isValid()) {
            return box;
        }
        box.valid = true;
        box.localRect = worldBounds;
        box.localToWorld = QTransform();
        return box;
    }

    // Relative mode, single item: the box is the shape's own transformed rect (parallelogram,
    // skew included), so scaling is a pure per-axis scale of the shape's local axes.
    if (count == 1) {
        if (!selLayers.isEmpty()) {
            const QString id = *selLayers.constBegin();
            for (const fh6::ShapeLayer &layer : project_->layers) {
                if (layer.id != id) {
                    continue;
                }
                box.valid = true;
                box.localRect = entryLocalRect(geometry_.shapeSize(layer.shapeId));
                box.localToWorld = entryTransform(layer);
                return box;
            }
        } else {
            const QString id = *selGuides.constBegin();
            for (const fh6::GuideLayer &guide : project_->guideLayers) {
                if (guide.id != id) {
                    continue;
                }
                box.valid = true;
                box.localRect = entryLocalRect(guide);
                box.localToWorld = entryTransform(guide);
                return box;
            }
        }
        return box;
    }

    // Relative mode, multi-selection/group: oriented bounding box at the derived frame angle.
    const QRectF worldBounds = cachedSelectionWorldBounds();
    if (!worldBounds.isValid()) {
        return box;
    }
    // The frame angle follows the primary selected item's rotation since selection time. Read it
    // live from layer data so undo/redo restore the box orientation along with the shapes. The
    // primary is the first selected item in project order, for deterministic, stable choice.
    double primaryRotation = 0.0;
    bool hasPrimary = false;
    for (const fh6::ShapeLayer &layer : project_->layers) {
        if (selLayers.contains(layer.id)) {
            primaryRotation = layer.rotation;
            hasPrimary = true;
            break;
        }
    }
    if (!hasPrimary) {
        for (const fh6::GuideLayer &guide : project_->guideLayers) {
            if (selGuides.contains(guide.id)) {
                primaryRotation = guide.rotation;
                hasPrimary = true;
                break;
            }
        }
    }
    if (signatureChanged) {
        frameReferenceRotation_ = primaryRotation;
    }
    double frameAngle = normalizeRotation(primaryRotation - frameReferenceRotation_);
    // A non-uniform group scale (or shear) applies a world affine to every child, and
    // decomposeScaleResult() re-extracts each child's rotation from the result - which drifts for
    // any child rotated relative to the box axes, the primary included. Deriving the frame angle
    // from the primary's live rotation would then spin the whole box while a scale/skew handle is
    // dragged (and leave it tilted afterwards): an artifact carried over from the image_transformer
    // port, not an intended rotation. While such a drag is active, pin the frame to the angle
    // captured at press (held in dragStartBox_) and rebase the reference so the pinned angle also
    // survives the release. The extent below still recomputes live, so the box keeps tightly
    // bounding the shapes as they scale; only its orientation is held steady.
    if ((dragMode_ == DragMode::Scale || dragMode_ == DragMode::Skew) && dragStartBox_.valid) {
        frameAngle = normalizeRotation(std::atan2(dragStartBox_.localToWorld.m12(),
                                                  dragStartBox_.localToWorld.m11())
                                       * 180.0 / kPi);
        frameReferenceRotation_ = normalizeRotation(primaryRotation - frameAngle);
    }
    const QPointF center = worldBounds.center();
    const double theta = frameAngle * kPi / 180.0;
    const QPointF axisU(std::cos(theta), std::sin(theta));
    const QPointF axisV(-std::sin(theta), std::cos(theta));
    bool hasExtent = false;
    double minA = 0.0;
    double maxA = 0.0;
    double minB = 0.0;
    double maxB = 0.0;
    const auto accumulate = [&](const QTransform &xform, const QRectF &local) {
        const QPointF corners[4] = {
            xform.map(local.topLeft()), xform.map(local.topRight()),
            xform.map(local.bottomRight()), xform.map(local.bottomLeft()),
        };
        for (const QPointF &corner : corners) {
            const QPointF rel = corner - center;
            const double a = rel.x() * axisU.x() + rel.y() * axisU.y();
            const double b = rel.x() * axisV.x() + rel.y() * axisV.y();
            if (!hasExtent) {
                minA = maxA = a;
                minB = maxB = b;
                hasExtent = true;
            } else {
                minA = std::min(minA, a);
                maxA = std::max(maxA, a);
                minB = std::min(minB, b);
                maxB = std::max(maxB, b);
            }
        }
    };
    for (const fh6::ShapeLayer &layer : project_->layers) {
        if (!selLayers.contains(layer.id)) {
            continue;
        }
        accumulate(entryTransform(layer), entryLocalRect(geometry_.shapeSize(layer.shapeId)));
    }
    for (const fh6::GuideLayer &guide : project_->guideLayers) {
        if (!selGuides.contains(guide.id)) {
            continue;
        }
        accumulate(entryTransform(guide), entryLocalRect(guide));
    }
    if (!hasExtent) {
        return box;
    }
    box.valid = true;
    box.localRect = QRectF(minA, minB, maxA - minA, maxB - minB);
    QTransform frame;
    frame.translate(center.x(), center.y());
    frame.rotate(frameAngle);
    box.localToWorld = frame;
    return box;
}

QTransform ProjectCanvas::boxToScreen(const SelectionBox &box) const
{
    return box.localToWorld * worldToScreen_;
}

bool ProjectCanvas::boxContainsScreenPoint(const SelectionBox &box, const QPointF &screenPoint) const
{
    if (!box.valid) {
        return false;
    }
    bool invertible = false;
    const QTransform inv = boxToScreen(box).inverted(&invertible);
    if (!invertible) {
        return false;
    }
    return box.localRect.contains(inv.map(screenPoint));
}

QRectF ProjectCanvas::selectedWorldBounds() const
{
    BoundsAccumulator acc;
    for (const fh6::ShapeLayer *layer : selectedLayers()) {
        acc.add(entryTransform(*layer), entryLocalRect(geometry_.shapeSize(layer->shapeId)));
    }
    for (const fh6::GuideLayer *guide : selectedGuideLayers()) {
        acc.add(entryTransform(*guide), entryLocalRect(*guide));
    }
    return acc.bounds();
}

// Tab flips the selection, cycling through the four scale-sign states in the order
// (+x +y) -> (+x -y) -> (-x -y) -> (-x +y) -> (+x +y). Each press mirrors the whole
// selection about its combined centre, so a multi-shape group mirrors as a unit
// (and a single shape flips in place).
void ProjectCanvas::cycleFlipSelection()
{
    if (state_ == nullptr || project_ == nullptr) {
        return;
    }
    QVector<fh6::ShapeLayer *> layers = selectedLayers();
    QVector<fh6::GuideLayer *> guides = selectedGuideLayers();
    if (layers.isEmpty() && guides.isEmpty()) {
        return;
    }

    // Derive the current state from a representative item, then advance one step.
    // flipV mirrors x positions and is represented by negative scaleY after
    // the rotation complement; flipH mirrors y positions and is represented by
    // negative scaleX. This matches decomposing a world-axis reflection back
    // into the editor's rotate-then-shear-then-scale layer fields.
    // Toggling either axis also complements the rotation and negates the skew:
    // with the rotate-then-scale convention chosen here, the rotation complement
    // alone does not absorb the shear, so the shear sign must be flipped too or
    // skewed shapes reflect incorrectly. (In a group the per-flip skew error
    // cancels every other press, so the offset only shows up on odd presses.)
    const double repScaleX = layers.isEmpty() ? guides.front()->scaleX : layers.front()->scaleX;
    const double repScaleY = layers.isEmpty() ? guides.front()->scaleY : layers.front()->scaleY;
    const int currentState = (repScaleY < 0 ? 1 : 0) + (repScaleX < 0 ? 2 : 0);
    const QVector<int> cycle = {0, 1, 3, 2};
    const int cycleIndex = cycle.contains(currentState) ? cycle.indexOf(currentState) : 0;
    const int nextState = cycle[(cycleIndex + 1) % cycle.size()];
    const bool toggleVertical = (repScaleY < 0) != ((nextState & 1) != 0);
    const bool toggleHorizontal = (repScaleX < 0) != ((nextState & 2) != 0);
    if (!toggleVertical && !toggleHorizontal) {
        return;
    }

    const QPointF center = selectedWorldBounds().center();
    const auto complementRotation = [](double rotation) {
        return normalizeRotation(180.0 - rotation);
    };

    state_->beginTransformCommand();
    // Detach from the undo snapshot before mutating through resolved pointers, then
    // re-resolve the selection into the now-owned buffers (mirrors captureDragStarts).
    project_->layers.data();
    project_->guideLayers.data();
    layers = selectedLayers();
    guides = selectedGuideLayers();

    for (fh6::ShapeLayer *layer : layers) {
        if (toggleVertical) {
            layer->x = 2.0 * center.x() - layer->x;
            layer->scaleY = -layer->scaleY;
            layer->skew = -layer->skew;
            layer->rotation = complementRotation(layer->rotation);
        }
        if (toggleHorizontal) {
            layer->y = 2.0 * center.y() - layer->y;
            layer->scaleX = -layer->scaleX;
            layer->skew = -layer->skew;
            layer->rotation = complementRotation(layer->rotation);
        }
    }
    for (fh6::GuideLayer *guide : guides) {
        if (toggleVertical) {
            guide->x = 2.0 * center.x() - guide->x;
            guide->scaleY = -guide->scaleY;
            guide->rotation = complementRotation(guide->rotation);
        }
        if (toggleHorizontal) {
            guide->y = 2.0 * center.y() - guide->y;
            guide->scaleX = -guide->scaleX;
            guide->rotation = complementRotation(guide->rotation);
        }
    }

    state_->commitTransformCommand();
    state_->noteProjectGeometryChanged();
    invalidateSceneCache();
    update();
}

QVector<fh6::ShapeLayer *> ProjectCanvas::selectedLayers() const
{
    if (state_ == nullptr) {
        return {};
    }
    const QSet<QString> locked = state_->lockedLayerIds();
    for (const QString &id : state_->selectedLayerIds()) {
        if (locked.contains(id)) {
            return {};
        }
    }
    QVector<fh6::ShapeLayer *> result;
    for (fh6::ShapeLayer *layer : state_->selectedLayers()) {
        if (!locked.contains(layer->id)) {
            result.push_back(layer);
        }
    }
    return result;
}

QVector<fh6::GuideLayer *> ProjectCanvas::selectedGuideLayers() const
{
    if (state_ == nullptr) {
        return {};
    }
    for (fh6::GuideLayer *guide : state_->selectedGuideLayers()) {
        if (guide->locked) {
            return {};
        }
    }
    return state_->selectedGuideLayers();
}

void ProjectCanvas::captureDragStarts()
{
    dragStarts_.clear();
    dragGuideStarts_.clear();
    // Force project_.layers to detach from any shared (undo snapshot) buffer before we
    // cache raw pointers into it. Mutating those pointers during the drag must not write
    // through copy-on-write aliasing into a history snapshot. The non-const data() call
    // triggers the detach; selectedLayers() below then resolves pointers into the owned buffer.
    if (project_ != nullptr) {
        project_->layers.data();
        project_->guideLayers.data();
    }
    dragLayers_ = selectedLayers();
    dragGuides_ = selectedGuideLayers();
    for (fh6::ShapeLayer *layer : dragLayers_) {
        dragStarts_.insert(layer->id, {layer->x, layer->y, layer->scaleX, layer->scaleY, layer->rotation, layer->skew});
    }
    for (fh6::GuideLayer *guide : dragGuides_) {
        dragGuideStarts_.insert(guide->id, {guide->x, guide->y, guide->scaleX, guide->scaleY, guide->rotation});
    }
}

void ProjectCanvas::applyMoveDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers)
{
    updateCursorForPoint(screenPoint);
    const QPointF delta = constrainDelta(screenToWorld(screenPoint) - dragStartWorld_, modifiers);
    const auto moveItem = [&delta](auto *item, const EntryStart &start) {
        item->x = start.x + delta.x();
        item->y = start.y + delta.y();
    };
    for (fh6::ShapeLayer *layer : dragLayers_) {
        moveItem(layer, dragStarts_.value(layer->id));
    }
    for (fh6::GuideLayer *guide : dragGuides_) {
        moveItem(guide, dragGuideStarts_.value(guide->id));
    }
    setCursorHint(screenPoint, {QStringLiteral("X: %1").arg(formatHintNumber(delta.x())),
                                QStringLiteral("Y: %1").arg(formatHintNumber(delta.y()))});
    invalidateSceneCache();
    update();
}

void ProjectCanvas::captureScaleReference()
{
    const QRectF &lr = dragStartBox_.localRect;
    const HandleAxes axes = handleAxes(activeHandle_);

    // The grabbed handle (named side/corner) and the anchor (opposite side/corner) are resolved
    // once in the box's local frame, then mapped to world via the drag-start frame so they
    // survive any pan/zoom during the drag. In Absolute mode localToWorld is identity, so the
    // local coords equal world coords and this matches the legacy behaviour.
    QPointF handleLocal = lr.center();
    QPointF anchorLocal = lr.center();
    if (axes.left) {
        handleLocal.setX(lr.left());
        anchorLocal.setX(lr.right());
    } else if (axes.right) {
        handleLocal.setX(lr.right());
        anchorLocal.setX(lr.left());
    }
    if (axes.top) {
        handleLocal.setY(lr.top());
        anchorLocal.setY(lr.bottom());
    } else if (axes.bottom) {
        handleLocal.setY(lr.bottom());
        anchorLocal.setY(lr.top());
    }

    scaleHandleLocal_ = handleLocal;
    scaleAnchorLocal_ = anchorLocal;
    scaleCenterLocal_ = lr.center();
    const QTransform &M = dragStartBox_.localToWorld;
    scaleHandleStartWorld_ = M.map(handleLocal);
    scaleAnchorWorld_ = M.map(anchorLocal);
    scaleCenterWorld_ = M.map(lr.center());
}

void ProjectCanvas::applyScaleDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers)
{
    updateCursorForPoint(screenPoint);
    if (!dragStartBox_.valid || dragStartBox_.localRect.isEmpty()) {
        return;
    }

    const HandleAxes axes = handleAxes(activeHandle_);

    // Everything is computed in the drag-start box's local frame so scaling runs along the
    // box's own axes. In Absolute mode the frame is the identity, so local == world and this
    // reduces exactly to the legacy world-axis behaviour.
    const QTransform &M = dragStartBox_.localToWorld;
    bool invertible = false;
    const QTransform Minv = M.inverted(&invertible);
    if (!invertible) {
        return;
    }

    // Alt anchors the scale at the box centre so it grows/shrinks in place (centre fixed)
    // rather than pinning the opposite side/corner. Resolved per-move so Alt can be toggled
    // mid-drag.
    const QPointF anchorLocal = (modifiers & Qt::AltModifier) ? scaleCenterLocal_ : scaleAnchorLocal_;

    // Track the grabbed handle, not the raw press point: preserving the initial grab offset
    // (in local coords) keeps the handle exactly under the cursor with no error accumulating
    // over distance, and holds under pan/zoom mid-drag.
    const QPointF pressLocal = Minv.map(dragStartWorld_);
    const QPointF grabOffsetLocal = pressLocal - scaleHandleLocal_;
    const QPointF cursorLocal = Minv.map(screenToWorld(screenPoint));
    const QPointF currentLocal = cursorLocal - grabOffsetLocal;

    const auto axisScale = [](double current, double start, double anchor) {
        const double span = start - anchor;
        if (std::abs(span) < 1e-6) {
            return 1.0;
        }
        return (current - anchor) / span;
    };

    double sx = (axes.left || axes.right) ? axisScale(currentLocal.x(), scaleHandleLocal_.x(), anchorLocal.x()) : 1.0;
    double sy = (axes.top || axes.bottom) ? axisScale(currentLocal.y(), scaleHandleLocal_.y(), anchorLocal.y()) : 1.0;

    // Groups and single shapes scale the same way: edge handles drive one axis, corners track
    // the cursor on both axes freely, and Shift locks the aspect ratio. For a multi-selection
    // the non-uniform factors run in the group's oriented frame (worldScale below), so each
    // child keeps its relative position and its form round-trips through the per-shape skew
    // field - the same math the image_transformer app applies to a whole image.
    const bool uniform = modifiers & Qt::ShiftModifier;
    if (uniform) {
        // Project the cursor onto the anchor->handle diagonal for a single continuous
        // factor. Picking the dominant axis instead snaps when the axes cross (e.g. while
        // scaling down past 1x); projection is smooth and rotation-independent.
        const QPointF anchorToHandle = scaleHandleLocal_ - anchorLocal;
        const QPointF anchorToCurrent = currentLocal - anchorLocal;
        const double denom = anchorToHandle.x() * anchorToHandle.x() + anchorToHandle.y() * anchorToHandle.y();
        const double s = denom > 1e-9
            ? (anchorToCurrent.x() * anchorToHandle.x() + anchorToCurrent.y() * anchorToHandle.y()) / denom
            : 1.0;
        sx = s;
        sy = s;
    }

    // Build the scale about the anchor in the box's local frame.
    QTransform localScale;
    localScale.translate(anchorLocal.x(), anchorLocal.y());
    localScale.scale(sx, sy);
    localScale.translate(-anchorLocal.x(), -anchorLocal.y());

    // Relative single-item mode applies the scale on the shape's pre-image side
    // (result = localScale * start), which only changes scaleX/scaleY and the position while
    // preserving rotation/skew - the pure scale the Relative mode exists for. All other cases
    // (Absolute, or Relative multi) scale about the anchor in the box frame on the world side
    // (result = start * (Minv * localScale * M)); for Absolute that frame is the identity, so
    // this is byte-for-byte the legacy path.
    const bool relativeSingle = transformRelativeMode_ && (dragLayers_.size() + dragGuides_.size() == 1);
    if (relativeSingle) {
        applyDragTransform(localScale, /*preMultiply=*/true);
    } else {
        applyDragTransform(Minv * localScale * M, /*preMultiply=*/false);
    }
    if (uniform) {
        setCursorHint(screenPoint, {QStringLiteral("Scale X/Y: %1x").arg(formatHintNumber(sx, 3))});
    } else {
        setCursorHint(screenPoint, {QStringLiteral("Scale X: %1x").arg(formatHintNumber(sx, 3)),
                                    QStringLiteral("Scale Y: %1x").arg(formatHintNumber(sy, 3))});
    }
    invalidateSceneCache();
    update();
}

void ProjectCanvas::applyDragTransform(const QTransform &transform, bool preMultiply)
{
    const auto apply = [&](auto *item, const EntryStart &start) {
        // Same field order as entryTransform(); a guide's skew stays 0 (shear(0, 0) is identity).
        QTransform startTransform;
        startTransform.translate(start.x, start.y);
        startTransform.rotate(start.rotation);
        startTransform.shear(start.skew, 0.0);
        startTransform.scale(start.scaleX, start.scaleY);
        const QTransform result = preMultiply ? (transform * startTransform)
                                              : (startTransform * transform);
        const ScaleDecomposition dec = decomposeScaleResult(result, start.skew);
        if (!dec.ok) {
            return;
        }
        item->x = dec.x;
        item->y = dec.y;
        item->rotation = dec.rotation;
        item->scaleX = dec.scaleX;
        item->scaleY = dec.scaleY;
        if constexpr (std::is_same_v<std::decay_t<decltype(*item)>, fh6::ShapeLayer>) {
            item->skew = dec.skew;
        }
    };
    for (fh6::ShapeLayer *layer : dragLayers_) {
        apply(layer, dragStarts_.value(layer->id));
    }
    for (fh6::GuideLayer *guide : dragGuides_) {
        apply(guide, dragGuideStarts_.value(guide->id));
    }
}

void ProjectCanvas::applySkewDrag(const QPointF &screenPoint)
{
    updateCursorForPoint(screenPoint);

    // Multi-selection: shear the whole group as a unit in the drag-start box's local frame, so
    // every child keeps its relative position and its induced shear/rotation round-trips through
    // the per-shape fields - the same box-frame treatment the group scale gesture uses. A lone
    // shape keeps the direct width-normalised skew below.
    if (dragLayers_.size() + dragGuides_.size() > 1) {
        if (!dragStartBox_.valid || dragStartBox_.localRect.isEmpty()) {
            return;
        }
        const QTransform &M = dragStartBox_.localToWorld;
        bool invertible = false;
        const QTransform Minv = M.inverted(&invertible);
        if (!invertible) {
            return;
        }
        const QRectF &lr = dragStartBox_.localRect;
        const QPointF centerLocal = lr.center();
        const double pressLocalX = Minv.map(dragStartWorld_).x();
        const double cursorLocalX = Minv.map(screenToWorld(screenPoint)).x();
        const double deltaLocalX = cursorLocalX - pressLocalX;
        // The skew handle sits at the top edge. Shearing about the box centre by
        // k = deltaLocalX / halfHeight makes the top edge track the cursor's horizontal motion
        // in the same rotational sense as a single shape's skew (Qt shear maps x' = x + k*(y-cy),
        // and the top is -halfHeight from the centre).
        const double halfHeight = lr.height() * 0.5;
        const double k = halfHeight > 1e-6 ? deltaLocalX / halfHeight : 0.0;
        QTransform localSkew;
        localSkew.translate(centerLocal.x(), centerLocal.y());
        localSkew.shear(k, 0.0);
        localSkew.translate(-centerLocal.x(), -centerLocal.y());
        applyDragTransform(Minv * localSkew * M, /*preMultiply=*/false);
        setCursorHint(screenPoint, {QStringLiteral("Skew: %1").arg(formatHintNumber(k, 3))});
        invalidateSceneCache();
        update();
        return;
    }

    const QPointF current = screenToWorld(screenPoint);
    // dragStartWorld_ is the world point captured at press; using it (instead of
    // re-mapping the stale start screen point) keeps skew stable across pan/zoom.
    const double delta = current.x() - dragStartWorld_.x();
    for (fh6::ShapeLayer *layer : dragLayers_) {
        const EntryStart startState = dragStarts_.value(layer->id);
        const QSizeF size = geometry_.shapeSize(layer->shapeId);
        layer->skew = startState.skew + delta / std::max(size.width(), 1.0);
    }
    setCursorHint(screenPoint, {QStringLiteral("Skew: %1").arg(formatHintNumber(delta, 2))});
    invalidateSceneCache();
    update();
}

void ProjectCanvas::applyRotateDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers)
{
    updateCursorForPoint(screenPoint);
    const QPointF current = screenToWorld(screenPoint);
    const double angle = std::atan2(current.y() - rotateCenterWorld_.y(), current.x() - rotateCenterWorld_.x());
    double deltaDegrees = (angle - rotateStartAngle_) * 180.0 / kPi;
    deltaDegrees = snapRotation(deltaDegrees, modifiers);
    const double radians = deltaDegrees * kPi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    // ShapeLayer and GuideLayer both expose x/y/rotation, so one generic step
    // rotates either kind about the pivot.
    const auto rotateItem = [&](auto *item, const EntryStart &startState) {
        const QPointF offset(startState.x - rotateCenterWorld_.x(), startState.y - rotateCenterWorld_.y());
        item->x = rotateCenterWorld_.x() + c * offset.x() - s * offset.y();
        item->y = rotateCenterWorld_.y() + s * offset.x() + c * offset.y();
        item->rotation = normalizeRotation(startState.rotation + deltaDegrees);
    };
    for (fh6::ShapeLayer *layer : dragLayers_) {
        rotateItem(layer, dragStarts_.value(layer->id));
    }
    for (fh6::GuideLayer *guide : dragGuides_) {
        rotateItem(guide, dragGuideStarts_.value(guide->id));
    }
    double displayedRotation = normalizeRotation(deltaDegrees);
    if (!dragLayers_.isEmpty()) {
        displayedRotation = dragLayers_.front()->rotation;
    } else if (!dragGuides_.isEmpty()) {
        displayedRotation = dragGuides_.front()->rotation;
    }
    setCursorHint(screenPoint, {QStringLiteral("Rotation: %1 deg").arg(formatHintNumber(displayedRotation, 1))});
    invalidateSceneCache();
    update();
}

void ProjectCanvas::clearCursorHint()
{
    cursorHintLines_.clear();
}

void ProjectCanvas::setCursorHint(const QPointF &point, const QStringList &lines)
{
    cursorHintPoint_ = point;
    cursorHintLines_ = lines;
}

void ProjectCanvas::drawCursorHint(QPainter &painter)
{
    if (cursorHintLines_.isEmpty()) {
        return;
    }

    painter.save();
    painter.resetTransform();
    const QFontMetrics metrics(painter.font());
    int textWidth = 0;
    for (const QString &line : cursorHintLines_) {
        textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
    }
    const int paddingX = CursorHintPaddingX;
    const int paddingY = CursorHintPaddingY;
    const int lineHeight = metrics.height();
    const QSize hintSize(textWidth + paddingX * 2,
                         cursorHintLines_.size() * lineHeight + paddingY * 2);
    QPointF topLeft = cursorHintPoint_ + QPointF(CursorHintCursorOffset, CursorHintCursorOffset);
    topLeft.setX(std::min(topLeft.x(), static_cast<double>(width() - hintSize.width() - CursorHintScreenMargin)));
    topLeft.setY(std::min(topLeft.y(), static_cast<double>(height() - hintSize.height() - CursorHintScreenMargin)));
    topLeft.setX(std::max(topLeft.x(), CursorHintScreenMargin));
    topLeft.setY(std::max(topLeft.y(), CursorHintScreenMargin));

    const QRectF background(topLeft, hintSize);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(CursorHintBorderColor, 1));
    painter.setBrush(CursorHintFillColor);
    painter.drawRoundedRect(background, CursorHintCornerRadius, CursorHintCornerRadius);
    painter.setPen(CursorHintTextColor);
    double baseline = topLeft.y() + paddingY + metrics.ascent();
    for (const QString &line : cursorHintLines_) {
        painter.drawText(QPointF(topLeft.x() + paddingX, baseline), line);
        baseline += lineHeight;
    }
    painter.restore();
}

bool ProjectCanvas::isTransformDrag() const
{
    switch (dragMode_) {
    case DragMode::Move:
    case DragMode::TransformMove:
    case DragMode::Scale:
    case DragMode::Skew:
    case DragMode::Rotate:
        return true;
    case DragMode::None:
    case DragMode::Pan:
    case DragMode::Marquee:
        break;
    }
    return false;
}

void ProjectCanvas::resetDragState()
{
    dragDuplicated_ = false;
    dragMode_ = DragMode::None;
    marqueeRect_ = {};
    clearCursorHint();
    activeHandle_.clear();
    dragStarts_.clear();
    dragGuideStarts_.clear();
    dragLayers_.clear();
    dragGuides_.clear();
    updateCursorForPoint(mapFromGlobal(QCursor::pos()));
    update();
}

void ProjectCanvas::finishDrag()
{
    if (isTransformDrag() && state_ != nullptr) {
        state_->commitTransformCommand();
        // An Alt-duplicate added layers/groups, so rebuild the tree to show the clones;
        // a plain transform only needs a geometry refresh.
        if (dragDuplicated_) {
            state_->noteProjectStructureChanged();
        } else {
            state_->noteProjectGeometryChanged();
        }
    }
    resetDragState();
}

void ProjectCanvas::cancelDrag()
{
    if (isTransformDrag() && state_ != nullptr) {
        // cancelTransformCommand restores the pre-drag snapshot, which removes any
        // Alt-duplicated clones and refreshes the tree via noteProjectStructureChanged.
        state_->cancelTransformCommand();
    }
    resetDragState();
}

QString ProjectCanvas::transformHandleAt(const QPointF &point, const SelectionBox &box) const
{
    if (!box.valid) {
        return {};
    }
    bool invertible = false;
    const QTransform toScreen = boxToScreen(box);
    const QTransform toLocal = toScreen.inverted(&invertible);
    if (!invertible) {
        return {};
    }
    // Work in the box's local frame: the box is its axis-aligned localRect there, so the legacy
    // band logic applies verbatim once the screen-pixel reach constants are converted to local
    // units using the per-axis screen length of each local unit vector (handles rotation/skew).
    const QPointF local = toLocal.map(point);
    const QPointF originScreen = toScreen.map(box.localRect.center());
    const double lenX = QLineF(originScreen, toScreen.map(box.localRect.center() + QPointF(1.0, 0.0))).length();
    const double lenY = QLineF(originScreen, toScreen.map(box.localRect.center() + QPointF(0.0, 1.0))).length();
    if (lenX < 1e-9 || lenY < 1e-9) {
        return {};
    }
    const double insideX = ScaleGrabInside / lenX;
    const double outsideX = ScaleGrabOutside / lenX;
    const double insideY = ScaleGrabInside / lenY;
    const double outsideY = ScaleGrabOutside / lenY;
    const double handleHalfX = HandleHalf / lenX;
    const double handleHalfY = HandleHalf / lenY;

    // Skew handle sits just above the top edge, for single shapes and groups alike.
    {
        const QPointF skew(box.localRect.center().x(), box.localRect.top() - SkewHandleOffset / lenY);
        if (std::abs(local.x() - skew.x()) <= handleHalfX && std::abs(local.y() - skew.y()) <= handleHalfY) {
            return QStringLiteral("skew");
        }
    }

    // Scale band straddling each edge: it reaches ScaleGrabInside into the box and
    // ScaleGrabOutside past it. The interior beyond the band is left to Move; the area past a
    // corner is left to Rotate (rotateZoneAt). Where two edge bands meet, Scale is two-axis.
    const double left = box.localRect.left();
    const double right = box.localRect.right();
    const double top = box.localRect.top();
    const double bottom = box.localRect.bottom();
    const bool nearLeft = local.x() >= left - outsideX && local.x() <= left + insideX;
    const bool nearRight = local.x() >= right - insideX && local.x() <= right + outsideX;
    const bool nearTop = local.y() >= top - outsideY && local.y() <= top + insideY;
    const bool nearBottom = local.y() >= bottom - insideY && local.y() <= bottom + outsideY;
    // Single-axis scale only counts when the orthogonal coordinate lies within the box's
    // span, so it never bleeds into the diagonal corner-proximity (rotate) regions.
    const bool spanX = local.x() >= left && local.x() <= right;
    const bool spanY = local.y() >= top && local.y() <= bottom;

    // Corners (two-axis) take priority over the sides.
    if (nearLeft && nearTop) {
        return QStringLiteral("top_left");
    }
    if (nearRight && nearTop) {
        return QStringLiteral("top_right");
    }
    if (nearLeft && nearBottom) {
        return QStringLiteral("bottom_left");
    }
    if (nearRight && nearBottom) {
        return QStringLiteral("bottom_right");
    }
    // Sides (single-axis).
    if (nearLeft && spanY) {
        return QStringLiteral("left");
    }
    if (nearRight && spanY) {
        return QStringLiteral("right");
    }
    if (nearTop && spanX) {
        return QStringLiteral("top");
    }
    if (nearBottom && spanX) {
        return QStringLiteral("bottom");
    }
    return {};
}

bool ProjectCanvas::rotateZoneAt(const QPointF &point, const SelectionBox &box) const
{
    // Rotate is the outer-anchor affordance: strictly outside the box, in the diagonal region
    // past a corner. Move owns the interior and Scale owns the edges and corner anchors
    // (resolved by transformHandleAt before this is consulted), so anything reaching here near
    // an edge is already past the scale band  Ewe only claim past the corner. Reach stays in
    // screen pixels; the inside/outward tests run in the box's (possibly rotated) local frame.
    if (!box.valid) {
        return false;
    }
    bool invertible = false;
    const QTransform toScreen = boxToScreen(box);
    const QTransform toLocal = toScreen.inverted(&invertible);
    if (!invertible) {
        return false;
    }
    const QPointF local = toLocal.map(point);
    if (box.localRect.contains(local)) {
        return false;
    }
    const QPointF centerLocal = box.localRect.center();
    const QPointF cornersLocal[4] = {
        box.localRect.topLeft(), box.localRect.topRight(), box.localRect.bottomLeft(), box.localRect.bottomRight(),
    };
    for (const QPointF &cornerLocal : cornersLocal) {
        const double dist = QLineF(point, toScreen.map(cornerLocal)).length();
        if (dist > RotateCornerReach) {
            continue;
        }
        const bool outwardX = (cornerLocal.x() < centerLocal.x()) ? (local.x() < cornerLocal.x()) : (local.x() > cornerLocal.x());
        const bool outwardY = (cornerLocal.y() < centerLocal.y()) ? (local.y() < cornerLocal.y()) : (local.y() > cornerLocal.y());
        if (outwardX && outwardY) {
            return true;
        }
    }
    return false;
}

Qt::CursorShape ProjectCanvas::cursorForScaleHandle(const QString &handle, const SelectionBox &box) const
{
    HandleCursorSpec resolvedSpec{Qt::ArrowCursor, "ToolbarScale.xpm"};
    if (box.valid) {
        QPointF handleLocal;
        QPointF anchorLocal;
        if (handleAnchorLocalPoints(handle, box.localRect, &handleLocal, &anchorLocal)) {
            const QPointF handleWorld = box.localToWorld.map(handleLocal);
            const QPointF anchorWorld = box.localToWorld.map(anchorLocal);
            resolvedSpec = scaleCursorSpecForScreenDelta(worldToScreen_.map(anchorWorld) - worldToScreen_.map(handleWorld));
            return resolvedSpec.shape;
        }
    }
    const auto spec = handleCursorSpecs().constFind(handle);
    return spec != handleCursorSpecs().constEnd() ? spec->shape : Qt::ArrowCursor;
}

QCursor ProjectCanvas::cursorForTransformHandle(const QString &handle, const SelectionBox &box) const
{
    HandleCursorSpec resolvedSpec{Qt::ArrowCursor, "ToolbarScale.xpm"};
    if (box.valid) {
        QPointF handleLocal;
        QPointF anchorLocal;
        if (handleAnchorLocalPoints(handle, box.localRect, &handleLocal, &anchorLocal)) {
            const QPointF handleWorld = box.localToWorld.map(handleLocal);
            const QPointF anchorWorld = box.localToWorld.map(anchorLocal);
            resolvedSpec = scaleCursorSpecForScreenDelta(worldToScreen_.map(anchorWorld) - worldToScreen_.map(handleWorld));
            return assetCursor(QLatin1String(resolvedSpec.icon));
        }
    }
    const auto spec = handleCursorSpecs().constFind(handle);
    return assetCursor(QLatin1String(spec != handleCursorSpecs().constEnd() ? spec->icon : resolvedSpec.icon));
}

Qt::CursorShape ProjectCanvas::cursorForPoint(const QPointF &point)
{
    switch (dragMode_) {
    case DragMode::Pan:
        return Qt::ClosedHandCursor;
    case DragMode::Move:
    case DragMode::TransformMove:
        return Qt::SizeAllCursor;
    case DragMode::Marquee:
        return Qt::CrossCursor;
    case DragMode::Scale:
    case DragMode::Skew:
        return cursorForScaleHandle(activeHandle_, dragStartBox_);
    case DragMode::Rotate:
        return Qt::ArrowCursor;
    case DragMode::None:
        break;
    }

    if (spaceDown_) {
        return Qt::OpenHandCursor;
    }
    if (activeTool_ != nullptr) {
        return activeTool_->idleCursorShape(point);
    }
    return Qt::ArrowCursor;
}

void ProjectCanvas::updateCursorForPoint(const QPointF &point)
{
    if (dragMode_ == DragMode::None && !rect().contains(point.toPoint())) {
        unsetCursor();
        return;
    }

    if (dragMode_ == DragMode::Rotate) {
        const SelectionBox box = dragStartBox_.valid ? dragStartBox_ : currentSelectionBox();
        setCursor(box.valid ? rotateCursorForPoint(point, box) : rotateCursor());
        return;
    }
    if (dragMode_ == DragMode::Scale || dragMode_ == DragMode::Skew) {
        setCursor(cursorForTransformHandle(activeHandle_, dragStartBox_));
        return;
    }
    if (activeTool_ != nullptr) {
        QCursor toolCursor;
        if (activeTool_->hoverCursor(point, &toolCursor)) {
            setCursor(toolCursor);
            return;
        }
    }
    const Qt::CursorShape nativeCursor = cursorForPoint(point);
    setCursor(nativeCursor == Qt::ArrowCursor ? assetCursor(QStringLiteral("Cursor.xpm")) : QCursor(nativeCursor));
}

QCursor ProjectCanvas::rotateCursor() const
{
    static const QCursor cursor = assetCursor(QStringLiteral("Cursor.xpm"));
    return cursor;
}

QCursor ProjectCanvas::rotateCursorForPoint(const QPointF &point, const SelectionBox &box) const
{
    if (!box.valid) {
        return rotateCursor();
    }
    // Classify the hovered rotate side/corner in local box space, then choose the cursor from
    // the transformed opposing-anchor vector. This keeps rotation and negative scale in sync.
    bool invertible = false;
    const QTransform toScreen = boxToScreen(box);
    const QTransform toLocal = toScreen.inverted(&invertible);
    if (!invertible) {
        return rotateCursor();
    }
    const QString handle = rotateHandleForLocalPoint(toLocal.map(point), box.localRect);
    QPointF handleLocal;
    QPointF anchorLocal;
    if (!handleAnchorLocalPoints(handle, box.localRect, &handleLocal, &anchorLocal)) {
        return rotateCursor();
    }
    const QPointF handleWorld = box.localToWorld.map(handleLocal);
    const QPointF anchorWorld = box.localToWorld.map(anchorLocal);
    const QPointF screenDelta = worldToScreen_.map(anchorWorld) - worldToScreen_.map(handleWorld);
    const QString suffix = rotateCursorSuffixForScreenDelta(screenDelta);
    return suffix.isEmpty() ? rotateCursor() : assetCursor(QStringLiteral("ToolRotate%1.xpm").arg(suffix));
}

void ProjectCanvas::drawOverlay(QPainter &painter)
{
    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (!hoverPolygon_.isEmpty()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(OverlayHaloColor, HoverHaloWidth));
        painter.drawPolygon(hoverPolygon_);
        painter.setPen(QPen(SelectionAccentColor, HoverAccentWidth));
        painter.drawPolygon(hoverPolygon_);
    }

    const SelectionBox box = currentSelectionBox();
    if (box.valid && !box.localRect.isEmpty()) {
        const QTransform toScreen = boxToScreen(box);
        const QRectF &lr = box.localRect;
        const QPointF topLeft = toScreen.map(lr.topLeft());
        const QPointF topRight = toScreen.map(lr.topRight());
        const QPointF bottomRight = toScreen.map(lr.bottomRight());
        const QPointF bottomLeft = toScreen.map(lr.bottomLeft());
        const QPolygonF boxPolygon({topLeft, topRight, bottomRight, bottomLeft});

        if (!isTransformDrag()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(OverlayHaloColor, SelectionFrameHaloWidth));
            painter.drawPolygon(boxPolygon);
            painter.setPen(QPen(SelectionFrameColor, SelectionFrameLineWidth));
            painter.drawPolygon(boxPolygon);
        }
        if (tool_ == QStringLiteral("transform")) {
            QVector<QPointF> handles = {
                topLeft, topRight, bottomLeft, bottomRight,
                toScreen.map(QPointF(lr.left(), lr.center().y())),
                toScreen.map(QPointF(lr.right(), lr.center().y())),
                toScreen.map(QPointF(lr.center().x(), lr.top())),
                toScreen.map(QPointF(lr.center().x(), lr.bottom())),
            };
            {
                // Skew handle sits SkewHandleOffset screen pixels outward from the top edge,
                // perpendicular to it (matching transformHandleAt's local-frame offset).
                const QPointF topCenter = toScreen.map(QPointF(lr.center().x(), lr.top()));
                const QPointF inward = toScreen.map(QPointF(lr.center().x(), lr.top() + 1.0));
                QPointF up = topCenter - inward;
                const double upLen = std::hypot(up.x(), up.y());
                if (upLen > 1e-9) {
                    up /= upLen;
                    handles.push_back(topCenter + up * SkewHandleOffset);
                }
            }
            for (const QPointF &handle : handles) {
                QRectF rect(handle.x() - HandleHalf, handle.y() - HandleHalf, HandleHalf * 2.0, HandleHalf * 2.0);
                painter.fillRect(rect, SelectionFrameColor);
                painter.setPen(QPen(OverlayHaloColor, HandleBorderWidth));
                painter.drawRect(rect);
            }
        }
    }

    if (dragMode_ == DragMode::Marquee && marqueeRect_.isValid()) {
        QColor marqueeFill = SelectionAccentColor;
        marqueeFill.setAlpha(MarqueeFillAlpha);
        painter.setBrush(marqueeFill);
        painter.setPen(QPen(SelectionAccentColor, 1, Qt::DashLine));
        painter.drawRect(marqueeRect_);
    }
    drawCursorHint(painter);
    painter.restore();
}

bool ProjectCanvas::selectionIsGroupLike() const
{
    if (state_ == nullptr) {
        return false;
    }
    // Counting the selection id sets is O(1) and avoids resolving every selected pointer each
    // paint (this runs inside drawOverlay, including during the selection-flash repaints).
    return state_->selectedLayerIds().size() + state_->selectedGuideLayerIds().size() > 1;
}

void ProjectCanvas::updateSelectionFlashState()
{
    if (!selectionFlashEnabled_) {
        setFlashingLayerIds({});
        return;
    }
    QSet<QString> selected;
    if (state_ != nullptr) {
        selected = state_->selectedLayerIds();
    }
    setFlashingLayerIds(selected);
}

void ProjectCanvas::setFlashingLayerIds(const QSet<QString> &ids)
{
    const QSet<QString> selected = selectionFlashEnabled_ ? ids : QSet<QString>{};
    const bool selectionChanged = selected != flashingLayerIds_;
    if (selectionChanged) {
        flashingLayerIds_ = selected;
        selectionFlashClock_.restart();
    }
    if (flashingLayerIds_.isEmpty()) {
        selectionFlashTimer_.stop();
    } else if (selectionChanged || !selectionFlashTimer_.isActive()) {
        scheduleSelectionFlashTimer();
    }
}

void ProjectCanvas::scheduleSelectionFlashTimer()
{
    if (flashingLayerIds_.isEmpty()) {
        selectionFlashTimer_.stop();
        return;
    }
    const qint64 elapsed = selectionFlashClock_.elapsed() % SelectionFlashPeriodMs;
    if (elapsed < SelectionFlashDurationMs) {
        selectionFlashTimer_.start(SelectionFlashFrameMs);
        return;
    }
    selectionFlashTimer_.start(static_cast<int>(SelectionFlashPeriodMs - elapsed));
}

std::optional<double> ProjectCanvas::selectionFlashProgress() const
{
    if (flashingLayerIds_.isEmpty() || !selectionFlashClock_.isValid()) {
        return std::nullopt;
    }
    const qint64 elapsed = selectionFlashClock_.elapsed() % SelectionFlashPeriodMs;
    if (elapsed >= SelectionFlashDurationMs) {
        return std::nullopt;
    }
    return static_cast<double>(elapsed) / static_cast<double>(SelectionFlashDurationMs);
}

double ProjectCanvas::selectionFlashHue() const
{
    return selectionFlashProgress().value_or(-1.0);
}

double ProjectCanvas::selectionFlashStrength() const
{
    const std::optional<double> progress = selectionFlashProgress();
    return progress.has_value() ? 0.18 + 0.72 * std::sin(*progress * kPi) : 0.0;
}

void ProjectCanvas::refreshHover(const QPointF &point, Qt::KeyboardModifiers modifiers)
{
    if (tool_ != QStringLiteral("select")) {
        return;
    }
    const QString id = selectTargetAtScreenPoint(point, modifiers);
    if (id == hoverLayerId_) {
        return;
    }
    hoverLayerId_ = id;
    hoverPolygon_ = {};
    for (const HitEntry &entry : hitEntries()) {
        if (entry.layerId == id) {
            hoverPolygon_ = entry.screenPolygon;
            break;
        }
    }
    update();
}

void ProjectCanvas::selectByMarquee(Qt::KeyboardModifiers modifiers)
{
    if (project_ == nullptr) {
        return;
    }
    QSet<QString> ids = (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) && state_ != nullptr
        ? state_->selectedLayerIds()
        : QSet<QString>{};
    for (const HitEntry &entry : hitEntries()) {
        if (marqueeRect_.contains(entry.screenBounds.center())) {
            ids.insert(entry.layerId);
        }
    }
    if (state_ != nullptr) {
        QSet<QString> guideIds;
        for (const fh6::GuideLayer &guide : project_->guideLayers) {
            if (!guide.visible) {
                continue;
            }
            const QRectF bounds = guideScreenPolygon(guide).boundingRect();
            if (marqueeRect_.contains(bounds.center())) {
                guideIds.insert(guide.id);
            }
        }
        state_->setSelectionIds(ids, guideIds);
    }
}

bool ProjectCanvas::pointInPolygon(const QPointF &point, const QPolygonF &polygon) const
{
    if (polygon.size() < 3) {
        return false;
    }
    bool signSet = false;
    double sign = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF a = polygon[i];
        const QPointF b = polygon[(i + 1) % polygon.size()];
        const double cross = (b.x() - a.x()) * (point.y() - a.y()) - (b.y() - a.y()) * (point.x() - a.x());
        if (std::abs(cross) < 1e-8) {
            continue;
        }
        const double current = cross > 0.0 ? 1.0 : -1.0;
        if (!signSet) {
            sign = current;
            signSet = true;
        } else if (current != sign) {
            return false;
        }
    }
    return true;
}

bool ProjectCanvas::movedPastClickThreshold(const QPointF &point) const
{
    const QPointF delta = point - dragStartScreen_;
    return delta.x() * delta.x() + delta.y() * delta.y() > ClickDragThreshold * ClickDragThreshold;
}

QImage ProjectCanvas::guideImage(const fh6::GuideLayer &guide) const
{
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5")
        .arg(guide.id)
        .arg(guide.width)
        .arg(guide.height)
        .arg(guide.imageFormat)
        .arg(QString::number(qHash(guide.imageBytes)));
    const auto cached = guideImageCache_.constFind(cacheKey);
    if (cached != guideImageCache_.constEnd()) {
        return cached.value();
    }
    QImage image;
    if (!guide.pixelBytes.isEmpty() && guide.width > 0 && guide.height > 0) {
        image = QImage(reinterpret_cast<const uchar *>(guide.pixelBytes.constData()),
                       guide.width,
                       guide.height,
                       guide.width * 4,
                       QImage::Format_ARGB32_Premultiplied).copy();
    } else {
        image.loadFromData(guide.imageBytes, guide.imageFormat.toLatin1().constData());
    }
    if (!image.isNull()) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    guideImageCache_.insert(cacheKey, image);
    return image;
}

QString ProjectCanvas::sectionCanvasCacheKey() const
{
    if (project_ == nullptr || state_ == nullptr || !project_->isLivery || state_->activeSectionId_.isEmpty() || size().isEmpty()) {
        return {};
    }
    return QStringLiteral("%1|%2x%3|%4|%5,%6,%7,%8,%9,%10|%11")
        .arg(state_->activeSectionId_)
        .arg(width())
        .arg(height())
        .arg(QString::number(devicePixelRatioF(), 'g', 12))
        .arg(QString::number(worldToScreen_.m11(), 'g', 17))
        .arg(QString::number(worldToScreen_.m12(), 'g', 17))
        .arg(QString::number(worldToScreen_.m21(), 'g', 17))
        .arg(QString::number(worldToScreen_.m22(), 'g', 17))
        .arg(QString::number(worldToScreen_.dx(), 'g', 17))
        .arg(QString::number(worldToScreen_.dy(), 'g', 17))
        .arg(canvasColor_.rgba());
}

void ProjectCanvas::storeSectionCanvasCache(const QString &key)
{
    if (key.isEmpty()) {
        return;
    }
    QImage image = grabFramebuffer();
    if (image.isNull()) {
        return;
    }
    constexpr int SectionCanvasCacheCap = 16;
    if (sectionCanvasCache_.size() >= SectionCanvasCacheCap) {
        sectionCanvasCache_.clear();
    }
    sectionCanvasCache_.insert(key, image);
}

void ProjectCanvas::drawGuideLayers(QPainter &painter)
{
    if (project_ == nullptr) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const fh6::GuideLayer &guide : project_->guideLayers) {
        if (!guide.visible || guide.opacity <= 0.0) {
            continue;
        }
        const QImage image = guideImage(guide);
        if (image.isNull()) {
            continue;
        }
        painter.save();
        painter.setOpacity(std::clamp(guide.opacity, 0.0, 1.0));
        painter.setTransform(entryTransform(guide) * worldToScreen_, true);
        painter.drawImage(QRectF(-guide.width * 0.5, -guide.height * 0.5, guide.width, guide.height), image);
        painter.restore();
    }
    painter.restore();
}

void ProjectCanvas::initializeGL()
{
    renderer_.initialize();
    rendererGeometryDirty_ = true;
}

void ProjectCanvas::paintGL()
{
    if (project_ == nullptr) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), canvasColor_);
        painter.setPen(EmptyCanvasTextColor);
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter, QStringLiteral("Open a C_group file or folder"));
        return;
    }

    updateViewTransform();
    updateSelectionFlashState();
    if (rendererGeometryDirty_ && renderer_.isInitialized()) {
        renderer_.uploadGeometry(geometry_);
        rendererGeometryDirty_ = false;
    }

    const std::optional<double> flashProgress = selectionFlashProgress();
    const bool flashActive = flashProgress.has_value() && !flashingLayerIds_.isEmpty();
    const QString sectionCacheKey = flashActive ? QString() : sectionCanvasCacheKey();
    if (!sectionCacheKey.isEmpty()) {
        const auto cached = sectionCanvasCache_.constFind(sectionCacheKey);
        if (cached != sectionCanvasCache_.constEnd()) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.drawImage(rect(), cached.value());
            drawOverlay(painter);
            return;
        }
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Paint the background and guide layers first so guides sit *behind* the shapes,
    // then composite the shapes over them (clearBackground=false keeps this content).
    painter.fillRect(rect(), canvasColor_);
    drawGuideLayers(painter);

    painter.beginNativePainting();
    renderer_.render(*project_, geometry_, worldToScreen_, size(), flashingLayerIds_, selectionFlashHue(), selectionFlashStrength(), false);
    painter.endNativePainting();

    if (!sectionCacheKey.isEmpty()) {
        painter.end();
        storeSectionCanvasCache(sectionCacheKey);
        painter.begin(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
    }

    drawOverlay(painter);
}

void ProjectCanvas::beginToolDrag(const QPointF &screenPos, const QPointF &boxCenterWorld)
{
    if (activeTool_ != nullptr) {
        activeTool_->beginDrag(screenPos, boxCenterWorld);
    }
}

void ProjectCanvas::beginRotateDrag(const QPointF &boxCenterWorld)
{
    dragMode_ = DragMode::Rotate;
    rotateCenterWorld_ = boxCenterWorld;
    rotateStartAngle_ = std::atan2(dragStartWorld_.y() - rotateCenterWorld_.y(),
                                   dragStartWorld_.x() - rotateCenterWorld_.x());
}

void ProjectCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    updateViewTransform();
    dragStartScreen_ = event->position();
    dragLastScreen_ = event->position();
    dragStartWorld_ = screenToWorld(dragStartScreen_);
    dragStartSelectionBounds_ = selectedScreenBounds();

    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && spaceDown_)) {
        dragMode_ = DragMode::Pan;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || project_ == nullptr) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    if (activeTool_ != nullptr && activeTool_->handlePress(event)) {
        return;
    }

    if (state_ == nullptr) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    // Rotate joins move/transform in picking the layer under the cursor: it rotates from
    // anywhere on the canvas like the move tool, with no anchor handle to grab.
    const bool picksUnderCursor = activeTool_ != nullptr && activeTool_->picksUnderCursor();

    auto selectMoveAutoTarget = [this, event]() -> bool {
        const QString target = selectTargetAtScreenPoint(event->position(), {});
        if (!target.isEmpty()) {
            const QString groupId = state_->topmostGroupForEntry(target);
            if (!groupId.isEmpty()) {
                QSet<QString> leafIds;
                for (const QString &id : state_->leafLayerIdsForEntry(groupId)) {
                    leafIds.insert(id);
                }
                state_->setSelectionIds(leafIds, {});
            } else {
                state_->setSelectionIds({target}, {});
            }
            dragStartSelectionBounds_ = selectedScreenBounds();
            return true;
        }
        const QString guideTarget = guideAtScreenPoint(event->position());
        if (!guideTarget.isEmpty()) {
            state_->setSelectionIds({}, {guideTarget});
            dragStartSelectionBounds_ = selectedScreenBounds();
            return true;
        }
        state_->clearSelection();
        return false;
    };

    if (tool_ == QStringLiteral("move")
        && moveToolAutoSelect_
        && !selectedScreenBounds().contains(event->position())) {
        if (!selectMoveAutoTarget()) {
            event->accept();
            return;
        }
    }

    if (picksUnderCursor
        && state_->selectedLayerIds().isEmpty()
        && !state_->selectedGuideLayerIds().isEmpty()) {
        const QString target = selectTargetAtScreenPoint(event->position(), {});
        if (!target.isEmpty()) {
            state_->selectLayerAtPoint(target, {});
            dragStartSelectionBounds_ = selectedScreenBounds();
        }
    }

    // (Move-tool auto-select cannot reach this branch: with an empty selection the
    // auto-select block above already ran  Ea null selection bounds contains nothing  E
    // and either selected a target or returned.)
    if (state_->selectedLayerIds().isEmpty() && state_->selectedGuideLayerIds().isEmpty()
        && picksUnderCursor) {
        const QString target = selectTargetAtScreenPoint(event->position(), {});
        const QString guideTarget = target.isEmpty() ? guideAtScreenPoint(event->position()) : QString();
        if (target.isEmpty() && guideTarget.isEmpty()) {
            QOpenGLWidget::mousePressEvent(event);
            return;
        }
        if (!target.isEmpty()) {
            state_->selectLayerAtPoint(target, {});
        } else {
            state_->setSelectionIds({}, {guideTarget});
        }
        dragStartSelectionBounds_ = selectedScreenBounds();
    }
    if (state_->selectedLayerIds().isEmpty() && state_->selectedGuideLayerIds().isEmpty()) {
        QOpenGLWidget::mousePressEvent(event);
        return;
    }

    // Capture the full selection box once (after any selection mutations above), so the drag
    // references follow the box's local frame.
    dragStartBox_ = currentSelectionBox();
    const QPointF boxCenterWorld = dragStartBox_.valid
        ? dragStartBox_.localToWorld.map(dragStartBox_.localRect.center())
        : screenToWorld(dragStartSelectionBounds_.center());
    beginToolDrag(event->position(), boxCenterWorld);

    if (dragMode_ != DragMode::None) {
        // Snapshot first: beginTransformCommand() copies project_.layers into the
        // undo "before" state, which shares storage via copy-on-write. captureDragStarts()
        // must run afterwards so it detaches project_.layers from that snapshot before we
        // cache raw layer pointers; otherwise the live drag would mutate the snapshot too.
        state_->beginTransformCommand();
        // Alt while beginning a plain move (Move tool, or dragging the body in Transform)
        // duplicates the selection in place and drags the clones, leaving the originals
        // untouched. The duplication and the move share one undo step (the "before"
        // snapshot above predates the clones), so undo removes the clones too.
        dragDuplicated_ = false;
        if ((event->modifiers() & Qt::AltModifier)
            && (dragMode_ == DragMode::Move || dragMode_ == DragMode::TransformMove)) {
            QVector<QString> entries;
            for (const QString &id : state_->selectedLayerIds()) {
                entries.push_back(id);
            }
            for (const QString &id : state_->selectedGuideLayerIds()) {
                entries.push_back(id);
            }
            QSet<QString> newLayerSel;
            QSet<QString> newGuideSel;
            if (state_->duplicateEntriesInPlace(entries, &newLayerSel, &newGuideSel)
                && (!newLayerSel.isEmpty() || !newGuideSel.isEmpty())) {
                state_->setSelectionIds(newLayerSel, newGuideSel);
                dragDuplicated_ = true;
            }
        }
        captureDragStarts();
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void ProjectCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (dragMode_ == DragMode::Marquee) {
        updateCursorForPoint(event->position());
        const QRectF nextRect = QRectF(dragStartScreen_, event->position()).normalized();
        if (nextRect != marqueeRect_) {
            marqueeRect_ = nextRect;
            update();
        }
        event->accept();
        return;
    }

    updateViewTransform();
    if (dragMode_ == DragMode::Pan) {
        updateCursorForPoint(event->position());
        const QPointF delta = event->position() - dragLastScreen_;
        pan_ += delta;
        dragLastScreen_ = event->position();
        invalidateSceneCache();
        update();
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Move || dragMode_ == DragMode::TransformMove) {
        applyMoveDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Scale) {
        applyScaleDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Skew) {
        applySkewDrag(event->position());
        event->accept();
        return;
    }
    if (dragMode_ == DragMode::Rotate) {
        applyRotateDrag(event->position(), event->modifiers());
        event->accept();
        return;
    }
    updateCursorForPoint(event->position());
    refreshHover(event->position(), event->modifiers());
    QOpenGLWidget::mouseMoveEvent(event);
}

void ProjectCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    // Marquee consumes its release before the shared view-transform refresh;
    // Select resolves click-selection when no drag is active. Both guard on
    // dragMode_, so a pan release still falls through to the pan block below.
    if (activeTool_ != nullptr && activeTool_->handleRelease(event)) {
        return;
    }

    updateViewTransform();
    if (dragMode_ == DragMode::Pan && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        dragMode_ = DragMode::None;
        updateCursorForPoint(event->position());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && dragMode_ != DragMode::None) {
        finishDrag();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void ProjectCanvas::wheelEvent(QWheelEvent *event)
{
    updateViewTransform();
    const QPointF anchorWorld = screenToWorld(event->position());
    // Zoom on whichever axis carries the wheel notch. Holding Alt makes some platforms
    // deliver the notch as a horizontal delta (angleDelta().x()), leaving y() == 0; reading
    // y() alone would then always take the zoom-out branch, so Alt appeared to force zoom-out.
    // Falling back to x() makes Alt a no-op for zooming, as intended.
    const QPoint wheelDelta = event->angleDelta();
    const int notch = wheelDelta.y() != 0 ? wheelDelta.y() : wheelDelta.x();
    if (notch == 0) {
        event->accept();
        return;
    }
    const double factor = notch > 0 ? 1.15 : 1.0 / 1.15;
    zoom_ = std::clamp(zoom_ * factor, 0.1, 100.0);
    updateViewTransform();
    pan_ += event->position() - worldToScreen(anchorWorld);
    invalidateSceneCache();
    update();
    event->accept();
}

void ProjectCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceDown_ = true;
        updateCursorForPoint(mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        cancelDrag();
        if (tool_ == QStringLiteral("transform")) {
            setTool(QStringLiteral("select"));
        }
        event->accept();
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && tool_ == QStringLiteral("transform")) {
        setTool(QStringLiteral("select"));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) {
        if (!event->isAutoRepeat()) {
            cycleFlipSelection();
        }
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

bool ProjectCanvas::focusNextPrevChild(bool next)
{
    // Keep Tab/Backtab on the canvas so keyPressEvent can use them to flip the selection.
    Q_UNUSED(next);
    return false;
}

void ProjectCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        spaceDown_ = false;
        if (dragMode_ == DragMode::Pan) {
            dragMode_ = DragMode::None;
        }
        updateCursorForPoint(mapFromGlobal(QCursor::pos()));
        event->accept();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void ProjectCanvas::leaveEvent(QEvent *event)
{
    hoverLayerId_.clear();
    hoverPolygon_ = {};
    clearCursorHint();
    unsetCursor();
    update();
    QOpenGLWidget::leaveEvent(event);
}

} // namespace gui
