#include "project_canvas.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {
namespace {

QPointF quadraticPoint(const PenBoundarySegment &segment, double t)
{
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
                          double *lineT)
{
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

int ProjectCanvas::penPointAtScreen(const QPointF &screenPoint) const
{
    int result = -1;
    double best = PenEditRadius + 1.0;
    for (int i = 0; i < penPoints_.size(); ++i) {
        const double distance = QLineF(screenPoint, worldToScreen(penPoints_[i].position)).length();
        if (distance <= PenEditRadius && distance < best) {
            best = distance;
            result = i;
        }
    }
    return result;
}

ProjectCanvas::PenCurveHit ProjectCanvas::penCurveAtScreen(const QPointF &screenPoint) const
{
    PenCurveHit best;
    if (!penLooped_ || penPoints_.size() < 3
        || penPoints_.front().kind != PenPointKind::Hard) {
        return best;
    }

    QPointF current = penPoints_.front().position;
    int index = 1;
    while (index <= penPoints_.size()) {
        const int nextIndex = index % penPoints_.size();
        const PenPoint &next = penPoints_[nextIndex];
        PenBoundarySegment segment;
        segment.start = current;
        int insertIndex = nextIndex == 0 ? penPoints_.size() : nextIndex;
        if (next.kind == PenPointKind::Hard) {
            segment.end = next.position;
            segment.curved = false;
            current = next.position;
            ++index;
        } else {
            const int afterIndex = (index + 1) % penPoints_.size();
            const PenPoint &after = penPoints_[afterIndex];
            segment.control = next.position;
            segment.end = after.kind == PenPointKind::Hard
                ? after.position
                : (next.position + after.position) * 0.5;
            segment.curved = true;
            insertIndex = std::min(nextIndex + 1, static_cast<int>(penPoints_.size()));
            current = segment.end;
            index += after.kind == PenPointKind::Hard ? 2 : 1;
        }

        constexpr int samples = 32;
        QPointF previousWorld = segment.start;
        QPointF previousScreen = worldToScreen(previousWorld);
        for (int sample = 1; sample <= samples; ++sample) {
            const double endT = static_cast<double>(sample) / samples;
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
    if (best.screenDistance > PenEditRadius) {
        return {};
    }
    return best;
}

void ProjectCanvas::normalizePenPointOrder()
{
    if (penPoints_.isEmpty() || penPoints_.front().kind == PenPointKind::Hard) {
        return;
    }
    const auto firstHard = std::find_if(penPoints_.begin(),
                                        penPoints_.end(),
                                        [](const PenPoint &point) {
        return point.kind == PenPointKind::Hard;
    });
    if (firstHard != penPoints_.end()) {
        std::rotate(penPoints_.begin(), firstHard, penPoints_.end());
    }
}

void ProjectCanvas::validatePenInteraction()
{
    penCrossings_.clear();
    penError_.clear();
    if (!penLooped_) {
        return;
    }
    const double worldPerPixel = 1.0 / std::max(baseScale_ * zoom_, 1e-8);
    const PenContour contour = buildPenContour(penPoints_, worldPerPixel * 0.25);
    if (!contour.valid()) {
        penCrossings_ = contour.crossings;
        penError_ = contour.error.isEmpty()
            ? QStringLiteral("Invalid Pen path")
            : contour.error;
    }
}

void ProjectCanvas::refreshPenInteractionHint(const QPointF &screenPoint,
                                              Qt::KeyboardModifiers modifiers)
{
    penHoverWorld_ = screenToWorld(screenPoint);
    penHoverPoint_ = -1;
    penHoverCurve_ = {};
    if (!penLooped_) {
        clearCursorHint();
        update();
        return;
    }

    penHoverPoint_ = penPointAtScreen(screenPoint);
    if (penHoverPoint_ < 0) {
        penHoverCurve_ = penCurveAtScreen(screenPoint);
    }

    QStringList lines{QStringLiteral("Press Enter to fill")};
    if (!penError_.isEmpty()) {
        lines.push_back(penError_);
    }
    if (penHoverPoint_ >= 0 || penHoverCurve_.valid()) {
        lines.push_back(QString());
        if (penHoverPoint_ >= 0) {
            if (penPoints_[penHoverPoint_].kind == PenPointKind::Soft) {
                lines.push_back(QStringLiteral("Ctrl+LMB: Make hard"));
            }
            lines.push_back(QStringLiteral("Alt+drag: Move point"));
            lines.push_back(QStringLiteral("RMB: Remove point"));
        } else {
            lines.push_back(QStringLiteral("Ctrl+LMB: Add soft point"));
        }
    }
    Q_UNUSED(modifiers);
    setCursorHint(screenPoint, lines);
    update();
}

int ProjectCanvas::liningPointAtScreen(const QPointF &screenPoint) const
{
    int result = -1;
    double best = PenEditRadius + 1.0;
    for (int i = 0; i < liningPoints_.size(); ++i) {
        const double distance = QLineF(screenPoint, worldToScreen(liningPoints_[i].position)).length();
        if (distance <= PenEditRadius && distance < best) {
            best = distance;
            result = i;
        }
    }
    return result;
}

ProjectCanvas::PenCurveHit ProjectCanvas::liningCurveAtScreen(const QPointF &screenPoint) const
{
    PenCurveHit best;
    if (!liningComplete_ || liningPoints_.size() < 2) {
        return best;
    }
    const LiningPath path = buildLiningPath(liningPoints_);
    if (!path.valid()) {
        return best;
    }

    int pointIndex = 1;
    for (const PenBoundarySegment &segment : path.segments) {
        int insertIndex = pointIndex;
        if (pointIndex < liningPoints_.size()
            && liningPoints_[pointIndex].kind == PenPointKind::Soft) {
            insertIndex = pointIndex + 1;
            pointIndex += pointIndex + 1 < liningPoints_.size()
                    && liningPoints_[pointIndex + 1].kind == PenPointKind::Hard
                ? 2
                : 1;
        } else {
            ++pointIndex;
        }

        constexpr int samples = 32;
        QPointF previousWorld = segment.start;
        QPointF previousScreen = worldToScreen(previousWorld);
        for (int sample = 1; sample <= samples; ++sample) {
            const double endT = static_cast<double>(sample) / samples;
            const QPointF nextWorld = quadraticPoint(segment, endT);
            const QPointF nextScreen = worldToScreen(nextWorld);
            double localT = 0.0;
            const double distance =
                closestPointOnLine(screenPoint, previousScreen, nextScreen, &localT);
            if (distance < best.screenDistance) {
                best.screenDistance = distance;
                best.insertIndex = std::min(insertIndex, static_cast<int>(liningPoints_.size()));
                best.worldPosition = previousWorld * (1.0 - localT) + nextWorld * localT;
            }
            previousWorld = nextWorld;
            previousScreen = nextScreen;
        }
    }
    if (best.screenDistance > PenEditRadius) {
        return {};
    }
    return best;
}

void ProjectCanvas::validateLiningInteraction()
{
    liningError_.clear();
    if (!liningComplete_) {
        return;
    }
    const LiningPath path = buildLiningPath(liningPoints_);
    if (!path.valid()) {
        liningError_ = path.error.isEmpty()
            ? QStringLiteral("Invalid lining path")
            : path.error;
    }
}

void ProjectCanvas::refreshLiningInteractionHint(const QPointF &screenPoint,
                                                 Qt::KeyboardModifiers modifiers)
{
    liningHoverWorld_ = screenToWorld(screenPoint);
    liningHoverPoint_ = -1;
    liningHoverCurve_ = {};
    if (!liningComplete_) {
        QStringList lines{QStringLiteral("Width: %1").arg(liningWidth_, 0, 'f', 2)};
        if (!liningPoints_.isEmpty()) {
            lines.push_back(QStringLiteral("RMB: Complete path"));
            lines.push_back(QStringLiteral("Double LMB: Hard point"));
        }
        if (!liningError_.isEmpty()) {
            lines.push_back(liningError_);
        }
        if (lines.isEmpty()) {
            clearCursorHint();
        } else {
            setCursorHint(screenPoint, lines);
        }
        update();
        return;
    }

    liningHoverPoint_ = liningPointAtScreen(screenPoint);
    if (liningHoverPoint_ < 0) {
        liningHoverCurve_ = liningCurveAtScreen(screenPoint);
    }
    QStringList lines{QStringLiteral("Width: %1").arg(liningWidth_, 0, 'f', 2),
                      QStringLiteral("Press Enter to fill")};
    if (!liningError_.isEmpty()) {
        lines.push_back(liningError_);
    }
    if (liningHoverPoint_ >= 0 || liningHoverCurve_.valid()) {
        lines.push_back(QString());
        if (liningHoverPoint_ >= 0) {
            if (liningPoints_[liningHoverPoint_].kind == PenPointKind::Soft) {
                lines.push_back(QStringLiteral("Ctrl+LMB: Make hard"));
            }
            lines.push_back(QStringLiteral("Alt+drag: Move point"));
            lines.push_back(QStringLiteral("RMB: Remove point"));
        } else {
            lines.push_back(QStringLiteral("Ctrl+LMB: Add soft point"));
        }
    }
    Q_UNUSED(modifiers);
    setCursorHint(screenPoint, lines);
    update();
}

} // namespace gui
