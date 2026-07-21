#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

using namespace pc_detail;

namespace {

QString assetPath(const QString &fileName) {
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

constexpr int kLogicalCursorSize = 21;

double cursorScaleFactor() {
    double scale = 1.0;
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        scale = std::max(screen->devicePixelRatio(), 1.0);
    }
    return scale;
}

QCursor assetCursor(const QString &fileName) {
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

    const double scale = cursorScaleFactor();
    const double logical = kLogicalCursorSize * (fileName == QStringLiteral("Cursor.xpm") ? 0.5 : 1.0);
    const int target = std::max(1, qRound(logical * scale));
    if (pixmap.width() != target || pixmap.height() != target) {
        pixmap = pixmap.scaled(target, target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    pixmap.setDevicePixelRatio(scale);

    if (fileName == QStringLiteral("Cursor.xpm")) {
        cursor = QCursor(pixmap, 0, 0);
    } else {
        cursor = QCursor(pixmap, pixmap.width() / 2, pixmap.height() / 2);
    }
    cache.insert(fileName, cursor);
    return cursor;
}

struct HandleCursorSpec {
    Qt::CursorShape shape;
    const char *icon;
};

const QHash<QString, HandleCursorSpec> &handleCursorSpecs() {
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

QString rotateHandleForLocalPoint(const QPointF &local, const QRectF &rect) {
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

QString rotateCursorSuffixForScreenDelta(const QPointF &delta) {
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

HandleCursorSpec scaleCursorSpecForScreenDelta(const QPointF &delta) {
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
                                        Qt::CursorShape *shape, QString *iconFile) const {
    const auto assign = [&](Qt::CursorShape cursorShape, const QString &icon) {
        if (shape != nullptr) {
            *shape = cursorShape;
        }
        if (iconFile != nullptr) {
            *iconFile = icon;
        }
    };
    if (box != nullptr && box->valid) {
        QPointF handleLocal;
        QPointF anchorLocal;
        if (handleAnchorLocalPoints(handle, box->localRect, &handleLocal, &anchorLocal)) {
            const QPointF handleWorld = box->localToWorld.map(handleLocal);
            const QPointF anchorWorld = box->localToWorld.map(anchorLocal);
            const HandleCursorSpec spec = scaleCursorSpecForScreenDelta(
                camera_.matrix().map(anchorWorld) - camera_.matrix().map(handleWorld));
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

Qt::CursorShape ProjectCanvas::cursorForScaleHandle(const QString &handle, const SelectionBox *box) const {
    Qt::CursorShape shape = Qt::ArrowCursor;
    resolveHandleCursor(handle, box, &shape, nullptr);
    return shape;
}

QCursor ProjectCanvas::cursorForTransformHandle(const QString &handle, const SelectionBox *box) const {
    QString iconFile;
    resolveHandleCursor(handle, box, nullptr, &iconFile);
    return assetCursor(iconFile);
}

Qt::CursorShape ProjectCanvas::cursorForPoint(const QPointF &point) {
    if (guidelines_.draggedOrientation == GuidelineOrientation::Vertical) {
        return Qt::SizeHorCursor;
    }
    if (guidelines_.draggedOrientation == GuidelineOrientation::Horizontal) {
        return Qt::SizeVerCursor;
    }
    switch (drag_.mode) {
    case DragMode::Pan:
        return Qt::ClosedHandCursor;
    case DragMode::Move:
    case DragMode::TransformMove:
        return Qt::SizeAllCursor;
    case DragMode::Marquee:
        return Qt::CrossCursor;
    case DragMode::Scale:
    case DragMode::Skew:
        return cursorForScaleHandle(drag_.activeHandle, &drag_.startBox);
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

void ProjectCanvas::updateCursorForPoint(const QPointF &point) {
    if (drag_.mode == DragMode::None && !rect().contains(point.toPoint())) {
        unsetCursor();
        return;
    }

    if (drag_.mode == DragMode::Rotate) {
        const SelectionBox box = drag_.startBox.valid ? drag_.startBox : currentSelectionBox();
        setCursor(box.valid ? rotateCursorForPoint(point, box) : rotateCursor());
        return;
    }
    if (drag_.mode == DragMode::Scale || drag_.mode == DragMode::Skew) {
        setCursor(cursorForTransformHandle(drag_.activeHandle, &drag_.startBox));
        return;
    }
    const GuidelineOrientation ruler = rulerAt(point);
    const bool rulerArea = project_ != nullptr && point.x() >= 0.0 && point.y() >= 0.0
        && point.x() < width() && point.y() < height()
        && (point.x() < kRulerExtent || point.y() < kRulerExtent);
    if (guidelines_.draggedOrientation != GuidelineOrientation::None || rulerArea) {
        Qt::CursorShape shape = Qt::ArrowCursor;
        const GuidelineOrientation orientation = guidelines_.draggedOrientation != GuidelineOrientation::None
            ? guidelines_.draggedOrientation
            : ruler;
        if (!guidelines_.locked && guidelines_.draggedOrientation != GuidelineOrientation::None) {
            shape = orientation == GuidelineOrientation::Vertical ? Qt::SizeHorCursor : Qt::SizeVerCursor;
        } else if (orientation != GuidelineOrientation::None
                   && !guidelines_.locked && guidelineAtRuler(point, orientation) >= 0) {
            shape = orientation == GuidelineOrientation::Vertical ? Qt::SizeHorCursor : Qt::SizeVerCursor;
        } else if (!guidelines_.locked && (QGuiApplication::keyboardModifiers() & Qt::AltModifier)) {
            shape = Qt::CrossCursor;
        }
        setCursor(QCursor(shape));
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

QCursor ProjectCanvas::rotateCursor() const {
    static const QCursor cursor = assetCursor(QStringLiteral("Cursor.xpm"));
    return cursor;
}

QCursor ProjectCanvas::pipetteCursor() const {
    static const QCursor cursor = assetCursor(QStringLiteral("CursorPipette.xpm"));
    return cursor;
}

QCursor ProjectCanvas::rotateCursorForPoint(const QPointF &point, const SelectionBox &box) const {
    if (!box.valid) {
        return rotateCursor();
    }
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
    const QPointF screenDelta = camera_.matrix().map(anchorWorld) - camera_.matrix().map(handleWorld);
    const QString suffix = rotateCursorSuffixForScreenDelta(screenDelta);
    return suffix.isEmpty() ? rotateCursor() : assetCursor(QStringLiteral("ToolRotate%1.xpm").arg(suffix));
}

} // namespace gui
