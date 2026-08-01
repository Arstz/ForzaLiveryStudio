#include "project_canvas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr int kCurveHitSamples = 32;
constexpr double kPenHitCellSize = 32.0;
constexpr double kPenAngleIntervalDegrees = 15.0;
constexpr double kRadiansToDegrees =
    180.0 / 3.14159265358979323846;
constexpr double kDegreesToRadians =
    3.14159265358979323846 / 180.0;

QPointF quadraticPoint(const PenBoundarySegment &segment, double t) {
    if (!segment.curved) {
        return segment.start * (1.0 - t) + segment.end * t;
    }
    const double u = 1.0 - t;
    return segment.start * (u * u)
        + segment.control * (2.0 * u * t)
        + segment.end * (t * t);
}

quint64 penHitCellKey(int x, int y) {
    return (static_cast<quint64>(static_cast<quint32>(x)) << 32)
        | static_cast<quint32>(y);
}

int penHitCellCoordinate(double value) {
    return static_cast<int>(std::floor(value / kPenHitCellSize));
}

double closestPointOnLine(const QPointF &point,
                          const QPointF &a,
                          const QPointF &b,
                          double *lineT) {
    const QPointF ab = b - a;
    const double lengthSquared = QPointF::dotProduct(ab, ab);
    const double t = lengthSquared <= 1e-12
        ? 0.0
        : std::clamp(QPointF::dotProduct(point - a, ab) / lengthSquared, 0.0, 1.0);
    if (lineT != nullptr) {
        *lineT = t;
    }
    return QLineF(point, a + ab * t).length();
}

} // namespace

void ProjectCanvas::invalidatePenGeometryCache() {
    ++penGeometryRevision_;
    if (penGeometryRevision_ == std::numeric_limits<quint64>::max()) {
        penGeometryRevision_ = 0;
    }
}

const ProjectCanvas::PenGeometryCache &ProjectCanvas::penGeometryCache() const {
    if (penGeometryCache_.revision == penGeometryRevision_) {
        return penGeometryCache_;
    }

    PenGeometryCache cache;
    cache.revision = penGeometryRevision_;
    cache.worldPath.setFillRule(Qt::OddEvenFill);
    cache.completedWorldPath.setFillRule(Qt::OddEvenFill);
    const bool drawingCutout = pen_.closed && !pen_.cutoutClosed;
    const auto appendLoop = [&cache, drawingCutout,
                             activeCutout = pen_.activeCutout](
                                const QVector<PenPoint> &points,
                                int loopIndex) {
        if (points.isEmpty()) {
            return;
        }
        const auto firstHard = std::find_if(
            points.cbegin(), points.cend(), [](const PenPoint &point) {
                return point.kind == PenPointKind::Hard;
            });
        if (firstHard == points.cend()) {
            return;
        }
        const int offset = static_cast<int>(firstHard - points.cbegin());
        CachedPenLoop loop;
        loop.loopIndex = loopIndex;
        loop.openPath.moveTo(firstHard->position);
        int openIndex = 1;
        while (openIndex < points.size()) {
            const int nextIndex = (offset + openIndex) % points.size();
            const PenPoint &next = points[nextIndex];
            if (next.kind == PenPointKind::Hard) {
                loop.openPath.lineTo(next.position);
                ++openIndex;
                continue;
            }
            if (openIndex + 1 >= points.size()) {
                break;
            }
            const PenPoint &after = points[(offset + openIndex + 1) % points.size()];
            const QPointF end = after.kind == PenPointKind::Hard
                ? after.position
                : (next.position + after.position) * 0.5;
            loop.openPath.quadTo(next.position, end);
            openIndex += after.kind == PenPointKind::Hard ? 2 : 1;
        }
        if (points.size() < 3) {
            cache.loops.push_back(std::move(loop));
            return;
        }
        loop.path.setFillRule(Qt::WindingFill);
        QPointF current = firstHard->position;
        loop.path.moveTo(current);
        int index = 1;
        while (index <= points.size()) {
            const int nextIndex = (offset + index) % points.size();
            const PenPoint &next = points[nextIndex];
            int insertIndex = nextIndex == 0 ? points.size() : nextIndex;
            if (next.kind == PenPointKind::Hard) {
                loop.segments.push_back({{current, {}, next.position, false},
                                         insertIndex});
                loop.path.lineTo(next.position);
                current = next.position;
                ++index;
                continue;
            }
            const int afterIndex = (offset + index + 1) % points.size();
            const PenPoint &after = points[afterIndex];
            const QPointF end = after.kind == PenPointKind::Hard
                ? after.position
                : (next.position + after.position) * 0.5;
            insertIndex = std::min(nextIndex + 1,
                                   static_cast<int>(points.size()));
            loop.segments.push_back({{current, next.position, end, true},
                                     insertIndex});
            loop.path.quadTo(next.position, end);
            current = end;
            index += after.kind == PenPointKind::Hard ? 2 : 1;
        }
        loop.path.closeSubpath();
        cache.worldPath.addPath(loop.path);
        if (!drawingCutout || loopIndex != activeCutout) {
            cache.completedWorldPath.addPath(loop.path);
        }
        cache.loops.push_back(std::move(loop));
    };

    appendLoop(pen_.points, -1);
    for (int loopIndex = 0; loopIndex < pen_.cutouts.size(); ++loopIndex) {
        appendLoop(pen_.cutouts[loopIndex], loopIndex);
    }
    penGeometryCache_ = std::move(cache);
    return penGeometryCache_;
}

const QPainterPath &ProjectCanvas::penScreenPath() const {
    penGeometryCache();
    const QTransform matrix = camera_.matrix();
    if (penGeometryCache_.screenRevision != penGeometryRevision_
        || penGeometryCache_.screenCamera != matrix) {
        penGeometryCache_.screenPath = matrix.map(penGeometryCache_.worldPath);
        penGeometryCache_.completedScreenPath =
            matrix.map(penGeometryCache_.completedWorldPath);
        penGeometryCache_.screenCamera = matrix;
        penGeometryCache_.screenRevision = penGeometryRevision_;
    }
    return penGeometryCache_.screenPath;
}

const QPainterPath &ProjectCanvas::penCompletedScreenPath() const {
    penScreenPath();
    return penGeometryCache_.completedScreenPath;
}

void ProjectCanvas::rebuildPenHitCache() const {
    const QTransform matrix = camera_.matrix();
    if (penHitCache_.revision == penGeometryRevision_
        && penHitCache_.camera == matrix) {
        return;
    }

    PenHitCache cache;
    cache.revision = penGeometryRevision_;
    cache.camera = matrix;
    const auto appendPoints = [&cache, &matrix](const QVector<PenPoint> &points,
                                                int loopIndex) {
        for (int pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            const QPointF screen = matrix.map(points[pointIndex].position);
            const int entryIndex = cache.points.size();
            cache.points.push_back({screen, pointIndex, loopIndex});
            cache.pointCells[penHitCellKey(
                penHitCellCoordinate(screen.x()),
                penHitCellCoordinate(screen.y()))].push_back(entryIndex);
        }
    };
    appendPoints(pen_.points, -1);
    for (int loopIndex = 0; loopIndex < pen_.cutouts.size(); ++loopIndex) {
        appendPoints(pen_.cutouts[loopIndex], loopIndex);
    }

    for (const CachedPenLoop &loop : penGeometryCache().loops) {
        for (const CachedPenSegment &cachedSegment : loop.segments) {
            const PenBoundarySegment &segment = cachedSegment.segment;
            const int samples = segment.curved ? kCurveHitSamples : 1;
            QPointF previousWorld = segment.start;
            QPointF previousScreen = matrix.map(previousWorld);
            for (int sample = 1; sample <= samples; ++sample) {
                const double t = static_cast<double>(sample) / samples;
                const QPointF nextWorld = quadraticPoint(segment, t);
                const QPointF nextScreen = matrix.map(nextWorld);
                const int edgeIndex = cache.edges.size();
                cache.edges.push_back({previousScreen, nextScreen,
                                       previousWorld, nextWorld,
                                       cachedSegment.insertIndex,
                                       loop.loopIndex});
                const QRectF bounds = QRectF(previousScreen, nextScreen)
                                          .normalized()
                                          .adjusted(-kPenEditRadius,
                                                    -kPenEditRadius,
                                                    kPenEditRadius,
                                                    kPenEditRadius);
                const int minimumX = penHitCellCoordinate(bounds.left());
                const int maximumX = penHitCellCoordinate(bounds.right());
                const int minimumY = penHitCellCoordinate(bounds.top());
                const int maximumY = penHitCellCoordinate(bounds.bottom());
                for (int cellY = minimumY; cellY <= maximumY; ++cellY) {
                    for (int cellX = minimumX; cellX <= maximumX; ++cellX) {
                        cache.edgeCells[penHitCellKey(cellX, cellY)]
                            .push_back(edgeIndex);
                    }
                }
                previousWorld = nextWorld;
                previousScreen = nextScreen;
            }
        }
    }
    penHitCache_ = std::move(cache);
}

ProjectCanvas::PenPointHit ProjectCanvas::penPointAtScreen(
    const QPointF &screenPoint) const {
    rebuildPenHitCache();
    PenPointHit result;
    const int centerX = penHitCellCoordinate(screenPoint.x());
    const int centerY = penHitCellCoordinate(screenPoint.y());
    for (int cellY = centerY - 1; cellY <= centerY + 1; ++cellY) {
        for (int cellX = centerX - 1; cellX <= centerX + 1; ++cellX) {
            const auto found = penHitCache_.pointCells.constFind(
                penHitCellKey(cellX, cellY));
            if (found == penHitCache_.pointCells.constEnd()) {
                continue;
            }
            for (const int entryIndex : found.value()) {
                const PenHitPointEntry &entry = penHitCache_.points[entryIndex];
                const double distance = QLineF(
                    screenPoint, entry.screenPosition).length();
                if (distance <= kPenEditRadius
                    && distance < result.screenDistance) {
                    result.pointIndex = entry.pointIndex;
                    result.loopIndex = entry.loopIndex;
                    result.screenDistance = distance;
                }
            }
        }
    }
    return result;
}

int ProjectCanvas::pointAtScreen(const QVector<PenPoint> &points, const QPointF &screenPoint) const {
    int result = -1;
    double best = kPenEditRadius + 1.0;
    for (int i = 0; i < points.size(); ++i) {
        const double distance = QLineF(screenPoint, worldToScreen(points[i].position)).length();
        if (distance <= kPenEditRadius && distance < best) {
            best = distance;
            result = i;
        }
    }
    return result;
}

void ProjectCanvas::accumulateCurveHit(const PenBoundarySegment &segment,
                                       int insertIndex,
                                       const QPointF &screenPoint,
                                       PenCurveHit &best) const {
    QPointF previousWorld = segment.start;
    QPointF previousScreen = worldToScreen(previousWorld);
    for (int sample = 1; sample <= kCurveHitSamples; ++sample) {
        const double endT = static_cast<double>(sample) / kCurveHitSamples;
        const QPointF nextWorld = quadraticPoint(segment, endT);
        const QPointF nextScreen = worldToScreen(nextWorld);
        double localT = 0.0;
        const double distance =
            closestPointOnLine(screenPoint, previousScreen, nextScreen, &localT);
        if (distance < best.screenDistance) {
            best.screenDistance = distance;
            best.insertIndex = insertIndex;
            best.worldPosition = previousWorld * (1.0 - localT) + nextWorld * localT;
        }
        previousWorld = nextWorld;
        previousScreen = nextScreen;
    }
}

void ProjectCanvas::appendPointEditHints(QStringList &lines,
                                         const QVector<PenPoint> &points,
                                         int hoverPoint,
                                         const PenCurveHit &hoverCurve) const {
    if (hoverPoint < 0 && !hoverCurve.valid()) {
        return;
    }
    lines.push_back(QString());
    if (hoverPoint >= 0) {
        if (points[hoverPoint].kind == PenPointKind::Soft) {
            lines.push_back(QStringLiteral("Ctrl+LMB: Make hard"));
        }
        lines.push_back(QStringLiteral("Alt+drag: Move point"));
        lines.push_back(QStringLiteral("RMB: Remove point"));
    } else {
        lines.push_back(QStringLiteral("Ctrl+LMB: Add soft point"));
    }
}

PenCurveHit ProjectCanvas::penCurveAtScreen(const QPointF &screenPoint) const {
    PenCurveHit best;
    if (!pen_.closed || !pen_.cutoutClosed) {
        return best;
    }
    rebuildPenHitCache();
    const int centerX = penHitCellCoordinate(screenPoint.x());
    const int centerY = penHitCellCoordinate(screenPoint.y());
    QSet<int> candidates;
    for (int cellY = centerY - 1; cellY <= centerY + 1; ++cellY) {
        for (int cellX = centerX - 1; cellX <= centerX + 1; ++cellX) {
            const auto found = penHitCache_.edgeCells.constFind(
                penHitCellKey(cellX, cellY));
            if (found != penHitCache_.edgeCells.constEnd()) {
                for (const int entryIndex : found.value()) {
                    candidates.insert(entryIndex);
                }
            }
        }
    }
    for (const int entryIndex : candidates) {
        const PenHitEdgeEntry &entry = penHitCache_.edges[entryIndex];
        double lineT = 0.0;
        const double distance = closestPointOnLine(
            screenPoint, entry.screenStart, entry.screenEnd, &lineT);
        if (distance < best.screenDistance) {
            best.screenDistance = distance;
            best.insertIndex = entry.insertIndex;
            best.loopIndex = entry.loopIndex;
            best.worldPosition = entry.worldStart * (1.0 - lineT)
                + entry.worldEnd * lineT;
        }
    }
    if (best.screenDistance > kPenEditRadius) {
        return {};
    }
    return best;
}

QPointF ProjectCanvas::snappedPenPosition(
    const QPointF &worldPosition,
    Qt::KeyboardModifiers modifiers,
    double *angleDegrees) const {
    if (angleDegrees != nullptr) {
        *angleDegrees =
            std::numeric_limits<double>::quiet_NaN();
    }
    const QVector<PenPoint> &points = pen_.cutoutClosed
        ? pen_.points
        : pen_.cutouts[pen_.activeCutout];
    if (!(modifiers & Qt::ShiftModifier)
        || points.isEmpty()
        || (pen_.closed && pen_.cutoutClosed)) {
        return worldPosition;
    }

    return snappedPenPosition(
        worldPosition,
        points.back().position,
        modifiers,
        angleDegrees);
}

QPointF ProjectCanvas::snappedPenPosition(
    const QPointF &worldPosition,
    const QPointF &origin,
    Qt::KeyboardModifiers modifiers,
    double *angleDegrees) const {
    if (angleDegrees != nullptr) {
        *angleDegrees =
            std::numeric_limits<double>::quiet_NaN();
    }
    if (!(modifiers & Qt::ShiftModifier)) {
        return worldPosition;
    }

    const QPointF delta =
        worldPosition - origin;
    const double length =
        std::hypot(delta.x(), delta.y());
    if (length <= std::numeric_limits<double>::epsilon()) {
        return origin;
    }
    const double rawDegrees =
        std::atan2(delta.y(), delta.x())
        * kRadiansToDegrees;
    double snappedDegrees =
        std::round(
            rawDegrees
            / kPenAngleIntervalDegrees)
        * kPenAngleIntervalDegrees;
    snappedDegrees =
        std::fmod(
            snappedDegrees + 360.0,
            360.0);
    if (angleDegrees != nullptr) {
        *angleDegrees = snappedDegrees;
    }
    const double radians =
        snappedDegrees * kDegreesToRadians;

    return origin
        + QPointF(
            std::cos(radians) * length,
            std::sin(radians) * length);
}

void ProjectCanvas::normalizePenPointOrder() {
    normalizePenPointOrder(pen_.points);
    for (QVector<PenPoint> &cutout : pen_.cutouts) {
        normalizePenPointOrder(cutout);
    }
}

void ProjectCanvas::normalizePenPointOrder(QVector<PenPoint> &points) {
    if (points.isEmpty() || points.front().kind == PenPointKind::Hard) {
        return;
    }
    const auto firstHard = std::find_if(points.begin(),
                                        points.end(),
                                        [](const PenPoint &point) {
        return point.kind == PenPointKind::Hard;
    });
    if (firstHard != points.end()) {
        std::rotate(points.begin(), firstHard, points.end());
    }
}

QVector<PenPoint> &ProjectCanvas::penPointsForLoop(int loopIndex) {
    return loopIndex < 0 ? pen_.points : pen_.cutouts[loopIndex];
}

const QVector<PenPoint> &ProjectCanvas::penPointsForLoop(int loopIndex) const {
    return loopIndex < 0 ? pen_.points : pen_.cutouts[loopIndex];
}

QVector<PenLoop> ProjectCanvas::currentPenLoops() const {
    QVector<PenLoop> result;
    if (!pen_.points.isEmpty()) {
        result.push_back({pen_.points, PenLoopKind::Outer});
    }
    for (const QVector<PenPoint> &cutout : pen_.cutouts) {
        result.push_back({cutout, PenLoopKind::Cutout});
    }

    return result;
}

void ProjectCanvas::validatePenInteraction() {
    pen_.crossings.clear();
    pen_.error.clear();
    if (!pen_.closed || !pen_.cutoutClosed) {
        return;
    }
    const double worldPerPixel = 1.0 / std::max(camera_.scale(), 1e-8);
    const PenContour contour = buildPenContour(currentPenLoops(), worldPerPixel * 0.25);
    if (!contour.valid()) {
        pen_.crossings = contour.crossings;
        pen_.error = contour.error.isEmpty()
            ? QStringLiteral("Invalid Pen path")
            : contour.error;
    }
}

void ProjectCanvas::refreshPenInteractionHint(const QPointF &screenPoint,
                                              Qt::KeyboardModifiers modifiers) {
    const bool drawingCutout = pen_.closed && !pen_.cutoutClosed;
    const QVector<PenPoint> &drawingPoints = drawingCutout
        ? pen_.cutouts[pen_.activeCutout]
        : pen_.points;
    const bool nearStart =
        (!pen_.closed || drawingCutout)
        && drawingPoints.size() >= 3
        && QLineF(
               screenPoint,
               worldToScreen(
                   drawingPoints.front().position))
               .length()
            <= kPenCloseRadius;
    double angleDegrees =
        std::numeric_limits<double>::quiet_NaN();
    pen_.hoverWorld = nearStart
        ? drawingPoints.front().position
        : snappedPenPosition(
              screenToWorld(screenPoint),
              modifiers,
              &angleDegrees);
    pen_.hoverPoint = -1;
    pen_.hoverLoop = -1;
    pen_.hoverCurve = {};
    if (!pen_.closed || drawingCutout) {
        if (std::isfinite(angleDegrees)) {
            setCursorHint(
                screenPoint,
                {QStringLiteral("Angle: %1 deg")
                     .arg(angleDegrees, 0, 'f', 0)});
        } else {
            clearCursorHint();
        }
        update();
        return;
    }

    if (pen_.dragPoint >= 0) {
        pen_.hoverPoint = pen_.dragPoint;
        pen_.hoverLoop = pen_.dragLoop;
        QStringList lines{QStringLiteral("Press %1 to fill")
                              .arg(interactionShortcutText(
                                  KeyInteraction::CanvasCommitInteraction))};
        if (!pen_.error.isEmpty()) {
            lines.push_back(pen_.error);
        }
        appendPointEditHints(lines,
                             penPointsForLoop(pen_.hoverLoop),
                             pen_.hoverPoint,
                             pen_.hoverCurve);
        setCursorHint(screenPoint, lines);
        update();
        return;
    }

    const PenPointHit pointHit = penPointAtScreen(screenPoint);
    if (pointHit.valid()) {
        pen_.hoverPoint = pointHit.pointIndex;
        pen_.hoverLoop = pointHit.loopIndex;
    }
    if (pen_.hoverPoint < 0) {
        pen_.hoverCurve = penCurveAtScreen(screenPoint);
        pen_.hoverLoop = pen_.hoverCurve.loopIndex;
    }

    QStringList lines{QStringLiteral("Press %1 to fill")
                          .arg(interactionShortcutText(KeyInteraction::CanvasCommitInteraction))};
    if (!pen_.error.isEmpty()) {
        lines.push_back(pen_.error);
    }
    if (pen_.hoverPoint < 0 && !pen_.hoverCurve.valid()) {
        lines.push_back(QString());
        lines.push_back(QStringLiteral("Ctrl+LMB inside: Add cutout"));
    } else {
        appendPointEditHints(lines,
                             penPointsForLoop(pen_.hoverLoop),
                             pen_.hoverPoint,
                             pen_.hoverCurve);
    }
    setCursorHint(screenPoint, lines);
    update();
}

PenCurveHit ProjectCanvas::liningCurveAtScreen(const QPointF &screenPoint) const {
    PenCurveHit best;
    if (!lining_.closed || lining_.points.size() < 2) {
        return best;
    }
    const LiningPath path = buildLiningPath(lining_.points);
    if (!path.valid()) {
        return best;
    }

    int pointIndex = 1;
    for (const PenBoundarySegment &segment : path.segments) {
        int insertIndex = pointIndex;
        if (pointIndex < lining_.points.size()
            && lining_.points[pointIndex].kind == PenPointKind::Soft) {
            insertIndex = pointIndex + 1;
            pointIndex += pointIndex + 1 < lining_.points.size()
                    && lining_.points[pointIndex + 1].kind == PenPointKind::Hard
                ? 2
                : 1;
        } else {
            ++pointIndex;
        }

        accumulateCurveHit(segment, std::min(insertIndex, static_cast<int>(lining_.points.size())),
                           screenPoint, best);
    }
    if (best.screenDistance > kPenEditRadius) {
        return {};
    }
    return best;
}

void ProjectCanvas::validateLiningInteraction() {
    lining_.error.clear();
    if (!lining_.closed) {
        return;
    }
    const LiningPath path = buildLiningPath(lining_.points);
    if (!path.valid()) {
        lining_.error = path.error.isEmpty()
            ? QStringLiteral("Invalid lining path")
            : path.error;
    }
}

void ProjectCanvas::refreshLiningInteractionHint(const QPointF &screenPoint,
                                                 Qt::KeyboardModifiers modifiers) {
    lining_.hoverWorld = screenToWorld(screenPoint);
    lining_.hoverPoint = -1;
    lining_.hoverCurve = {};
    if (!lining_.closed) {
        QStringList lines{QStringLiteral("Width: %1").arg(liningWidth_, 0, 'f', 2)};
        if (!lining_.points.isEmpty()) {
            lines.push_back(QStringLiteral("RMB: Complete path"));
            lines.push_back(QStringLiteral("Double LMB: Hard point"));
        }
        if (!lining_.error.isEmpty()) {
            lines.push_back(lining_.error);
        }
        if (lines.isEmpty()) {
            clearCursorHint();
        } else {
            setCursorHint(screenPoint, lines);
        }
        update();
        return;
    }

    lining_.hoverPoint = pointAtScreen(lining_.points, screenPoint);
    if (lining_.hoverPoint < 0) {
        lining_.hoverCurve = liningCurveAtScreen(screenPoint);
    }
    QStringList lines{QStringLiteral("Width: %1").arg(liningWidth_, 0, 'f', 2),
                      QStringLiteral("Press %1 to fill")
                          .arg(interactionShortcutText(KeyInteraction::CanvasCommitInteraction))};
    if (!lining_.error.isEmpty()) {
        lines.push_back(lining_.error);
    }
    appendPointEditHints(lines, lining_.points, lining_.hoverPoint, lining_.hoverCurve);
    Q_UNUSED(modifiers);
    setCursorHint(screenPoint, lines);
    update();
}

} // namespace gui
