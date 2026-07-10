#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

// Shared helpers (flatEntry*, EffectiveSelection, collectGuideIds, handle axes,
// handle-box geometry constants, buildTransformTargetIds, normalizeRotation) live in
// project_canvas_internal.h so every ProjectCanvas translation unit reuses one definition.
using namespace pc_detail;

namespace {

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

// Standard logical cursor size. Cursor art is authored at 2x; keep it smaller
// than the source art so transform cursors do not obscure the handles.
constexpr int LogicalCursorSize = 21;

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


void ProjectCanvas::resolveHandleCursor(const QString &handle, const SelectionBox *box,
                                        Qt::CursorShape *shape, QString *iconFile) const
{
    const auto assign = [&](Qt::CursorShape cursorShape, const QString &icon) {
        if (shape != nullptr) {
            *shape = cursorShape;
        }
        if (iconFile != nullptr) {
            *iconFile = icon;
        }
    };
    // With a (possibly rotated) box, orient the cursor to the on-screen anchor->handle
    // direction so scale and negative-scale stay visually consistent.
    if (box != nullptr && box->valid) {
        QPointF handleLocal;
        QPointF anchorLocal;
        if (handleAnchorLocalPoints(handle, box->localRect, &handleLocal, &anchorLocal)) {
            const QPointF handleWorld = box->localToWorld.map(handleLocal);
            const QPointF anchorWorld = box->localToWorld.map(anchorLocal);
            const HandleCursorSpec spec = scaleCursorSpecForScreenDelta(
                worldToScreen_.map(anchorWorld) - worldToScreen_.map(handleWorld));
            assign(spec.shape, QLatin1String(spec.icon));
            return;
        }
    }
    const auto spec = handleCursorSpecs().constFind(handle);
    if (spec != handleCursorSpecs().constEnd()) {
        assign(spec->shape, QLatin1String(spec->icon));
    } else {
        assign(Qt::ArrowCursor, QStringLiteral("ToolbarScale.xpm"));
    }
}

Qt::CursorShape ProjectCanvas::cursorForScaleHandle(const QString &handle, const SelectionBox *box) const
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    resolveHandleCursor(handle, box, &shape, nullptr);
    return shape;
}

QCursor ProjectCanvas::cursorForTransformHandle(const QString &handle, const SelectionBox *box) const
{
    QString iconFile;
    resolveHandleCursor(handle, box, nullptr, &iconFile);
    return assetCursor(iconFile);
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
        return cursorForScaleHandle(activeHandle_, &dragStartBox_);
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
        setCursor(cursorForTransformHandle(activeHandle_, &dragStartBox_));
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

QCursor ProjectCanvas::pipetteCursor() const
{
    static const QCursor cursor = assetCursor(QStringLiteral("CursorPipette.xpm"));
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

} // namespace gui
