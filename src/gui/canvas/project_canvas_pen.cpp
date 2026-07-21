#include "project_canvas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr int kCurveHitSamples = 32;

QPointF quadraticPoint(const PenBoundarySegment &segment, double t) {
    if (!segment.curved) {
        return segment.start * (1.0 - t) + segment.end * t;
    }
    const double u = 1.0 - t;
    return segment.start * (u * u)
        + segment.control * (2.0 * u * t)
        + segment.end * (t * t);
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
    if (!pen_.closed || pen_.points.size() < 3
        || pen_.points.front().kind != PenPointKind::Hard) {
        return best;
    }

    QPointF current = pen_.points.front().position;
    int index = 1;
    while (index <= pen_.points.size()) {
        const int nextIndex = index % pen_.points.size();
        const PenPoint &next = pen_.points[nextIndex];
        PenBoundarySegment segment;
        segment.start = current;
        int insertIndex = nextIndex == 0 ? pen_.points.size() : nextIndex;
        if (next.kind == PenPointKind::Hard) {
            segment.end = next.position;
            segment.curved = false;
            current = next.position;
            ++index;
        } else {
            const int afterIndex = (index + 1) % pen_.points.size();
            const PenPoint &after = pen_.points[afterIndex];
            segment.control = next.position;
            segment.end = after.kind == PenPointKind::Hard
                ? after.position
                : (next.position + after.position) * 0.5;
            segment.curved = true;
            insertIndex = std::min(nextIndex + 1, static_cast<int>(pen_.points.size()));
            current = segment.end;
            index += after.kind == PenPointKind::Hard ? 2 : 1;
        }

        accumulateCurveHit(segment, insertIndex, screenPoint, best);
    }
    if (best.screenDistance > kPenEditRadius) {
        return {};
    }
    return best;
}

void ProjectCanvas::normalizePenPointOrder() {
    if (pen_.points.isEmpty() || pen_.points.front().kind == PenPointKind::Hard) {
        return;
    }
    const auto firstHard = std::find_if(pen_.points.begin(),
                                        pen_.points.end(),
                                        [](const PenPoint &point) {
        return point.kind == PenPointKind::Hard;
    });
    if (firstHard != pen_.points.end()) {
        std::rotate(pen_.points.begin(), firstHard, pen_.points.end());
    }
}

void ProjectCanvas::validatePenInteraction() {
    pen_.crossings.clear();
    pen_.error.clear();
    if (!pen_.closed) {
        return;
    }
    const double worldPerPixel = 1.0 / std::max(camera_.scale(), 1e-8);
    const PenContour contour = buildPenContour(pen_.points, worldPerPixel * 0.25);
    if (!contour.valid()) {
        pen_.crossings = contour.crossings;
        pen_.error = contour.error.isEmpty()
            ? QStringLiteral("Invalid Pen path")
            : contour.error;
    }
}

void ProjectCanvas::refreshPenInteractionHint(const QPointF &screenPoint,
                                              Qt::KeyboardModifiers modifiers) {
    pen_.hoverWorld = screenToWorld(screenPoint);
    pen_.hoverPoint = -1;
    pen_.hoverCurve = {};
    if (!pen_.closed) {
        clearCursorHint();
        update();
        return;
    }

    pen_.hoverPoint = pointAtScreen(pen_.points, screenPoint);
    if (pen_.hoverPoint < 0) {
        pen_.hoverCurve = penCurveAtScreen(screenPoint);
    }

    QStringList lines{QStringLiteral("Press Enter to fill")};
    if (!pen_.error.isEmpty()) {
        lines.push_back(pen_.error);
    }
    appendPointEditHints(lines, pen_.points, pen_.hoverPoint, pen_.hoverCurve);
    Q_UNUSED(modifiers);
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
                      QStringLiteral("Press Enter to fill")};
    if (!lining_.error.isEmpty()) {
        lines.push_back(lining_.error);
    }
    appendPointEditHints(lines, lining_.points, lining_.hoverPoint, lining_.hoverCurve);
    Q_UNUSED(modifiers);
    setCursorHint(screenPoint, lines);
    update();
}

} // namespace gui
