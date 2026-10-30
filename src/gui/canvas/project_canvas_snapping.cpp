#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {
namespace {

constexpr double kSnapRadiusPixels = 6.0;
constexpr double kSnapIndicatorWidth = 1.0;
constexpr double kSnapContactWidth = 1.5;
constexpr double kSnapMarkerExtent = 3.0;
constexpr double kSnapLineExtension = 8.0;
const QColor kSnapIndicatorColor(255, 75, 115);
const QVector<double> kNoGuidelines;

bool visibleForSnapping(const fls::scene::Layer &layer) {
    for (const auto *node = &layer; node != nullptr; node = node->parent()) {
        if (!node->visible) return false;
    }

    return true;
}

QTransform scaleAroundAnchor(const QTransform &frame, const QPointF &anchor, const QPointF &factors) {
    QTransform scale;

    scale.translate(anchor.x(), anchor.y());
    scale.scale(factors.x(), factors.y());
    scale.translate(-anchor.x(), -anchor.y());

    return frame.inverted() * scale * frame;
}

} // namespace

void ProjectCanvas::captureDragSnapGeometry() {
    const auto targets = dragTransformTargetIds();
    const QSet<QString> movingIds(targets.cbegin(), targets.cend());
    QVector<GeometrySnapOutline> stationaryOutlines;

    drag_.snapOutlines.clear();
    drag_.snapTargets = {};
    drag_.snapIndicators.clear();
    if (state_ == nullptr || !options_.automaticSnapping
        || (drag_.mode != DragMode::Move && drag_.mode != DragMode::TransformMove
                             && drag_.mode != DragMode::Scale)) {
        return;
    }
    for (const auto &entry : state_->activeRenderEntries()) {
        QPainterPath body;
        bool moving = false;

        if (!visibleForSnapping(*entry.node)) continue;
        for (const auto *node = entry.node; node != nullptr; node = node->parent()) {
            if (movingIds.contains(node->id)) {
                moving = true;
                break;
            }
        }
        if (entry.shape != nullptr) {
            if (entry.shape->color[ColorByteAlpha] == 0) continue;
            body = shapeSelectionBody(*entry.shape);
        } else if (moving && entry.guide != nullptr) {
            body.addRect(sceneLocalRect(*entry.guide, geometry_));
        } else if (moving && entry.kind == fls::scene::LayerKind::Contour) {
            const auto &figure = static_cast<const fls::scene::ContourFigure &>(*entry.node);
            body = figure.data.pathKind == fls::scene::ContourPathKind::Lining
                ? buildLiningRibbon(figure.data.points, figure.data.strokeWidth) : contourFigurePath(figure);
        }
        if (body.isEmpty()) continue;
        auto outline = geometrySnapOutline(entry.worldTransform.map(body), camera_.scale());
        if (moving) {
            drag_.snapOutlines.push_back(std::move(outline));
        } else {
            stationaryOutlines.push_back(std::move(outline));
        }
    }
    drag_.snapTargets.setOutlines(stationaryOutlines);
}

QPointF ProjectCanvas::snappedMoveDelta(const QPointF &delta, Qt::KeyboardModifiers modifiers) {
    const bool guidelines = guidelines_.visible && project_ != nullptr;
    const bool constrained = modifiers & Qt::ShiftModifier;
    const bool horizontal = std::abs(delta.x()) >= std::abs(delta.y());
    const QPointF firstDirection = !constrained || horizontal ? QPointF(1.0, 0.0) : QPointF(0.0, 1.0);
    const QPointF firstStep = delta + firstDirection;
    GeometrySnapMotion motion;

    drag_.snapIndicators.clear();
    if (!options_.automaticSnapping || drag_.snapOutlines.isEmpty()) return delta;
    motion.transform = QTransform::fromTranslate(delta.x(), delta.y());
    motion.firstStep = QTransform::fromTranslate(firstStep.x(), firstStep.y());
    motion.secondStep = constrained ? motion.transform : QTransform::fromTranslate(delta.x(), delta.y() + 1.0);
    motion.controlPoint = drag_.startWorld;
    motion.twoDimensions = !constrained;
    auto result = snapGeometry(drag_.snapOutlines, &drag_.snapTargets, motion,
        guidelines ? project_->verticalGuidelines : kNoGuidelines,
        guidelines ? project_->horizontalGuidelines : kNoGuidelines,
        kSnapRadiusPixels * camera_.worldPerPixel());
    drag_.snapIndicators = std::move(result.indicators);

    return delta + (constrained ? firstDirection * result.adjustment.x() : result.adjustment);
}

QPointF ProjectCanvas::snappedScaleFactors(const QPointF &factors, const QPointF &anchorLocal,
                                          bool scaleX, bool scaleY, bool uniform) {
    const QTransform &frame = drag_.startBox.localToWorld;
    const bool guidelines = guidelines_.visible && project_ != nullptr;
    const bool twoDimensions = scaleX && scaleY && !uniform;
    const QPointF firstDirection(uniform || scaleX ? 1.0 : 0.0, !twoDimensions && (uniform || scaleY) ? 1.0 : 0.0);
    GeometrySnapMotion motion;

    drag_.snapIndicators.clear();
    if (!options_.automaticSnapping || drag_.snapOutlines.isEmpty()) return factors;
    motion.transform = scaleAroundAnchor(frame, anchorLocal, factors);
    motion.firstStep = scaleAroundAnchor(frame, anchorLocal, factors + firstDirection);
    motion.secondStep = twoDimensions ? scaleAroundAnchor(frame, anchorLocal, factors + QPointF(0.0, 1.0)) : motion.transform;
    motion.controlPoint = frame.map(drag_.scaleHandleLocal);
    motion.twoDimensions = twoDimensions;
    auto result = snapGeometry(drag_.snapOutlines, &drag_.snapTargets, motion,
        guidelines ? project_->verticalGuidelines : kNoGuidelines,
        guidelines ? project_->horizontalGuidelines : kNoGuidelines,
        kSnapRadiusPixels * camera_.worldPerPixel());
    drag_.snapIndicators = std::move(result.indicators);

    return factors + (twoDimensions ? result.adjustment : firstDirection * result.adjustment.x());
}

void ProjectCanvas::drawSnapIndicators(QPainter &painter) const {
    const auto drawCross = [&](const QPointF &point) {
        const QPointF first(kSnapMarkerExtent, kSnapMarkerExtent);
        const QPointF second(kSnapMarkerExtent, -kSnapMarkerExtent);

        painter.drawLine(point - first, point + first);
        painter.drawLine(point - second, point + second);
    };

    if (!options_.automaticSnapping || drag_.snapIndicators.isEmpty()
        || (drag_.mode != DragMode::Move && drag_.mode != DragMode::TransformMove && drag_.mode != DragMode::Scale)) {
        return;
    }
    painter.save();
    painter.resetTransform();
    painter.setBrush(Qt::NoBrush);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const auto &indicator : drag_.snapIndicators) {
        const QPointF point = worldToScreen(indicator.point);
        const QLineF reference(worldToScreen(indicator.line.p1()), worldToScreen(indicator.line.p2()));
        QLineF line = reference;
        QPen pen(kSnapIndicatorColor, indicator.kind == GeometrySnapIndicatorKind::Contact
            ? kSnapContactWidth : kSnapIndicatorWidth);

        pen.setCosmetic(true);
        painter.setPen(pen);
        if (indicator.infiniteLine) {
            line = indicator.line.dx() != 0.0
                ? QLineF(pc_detail::kRulerExtent, point.y(), width(), point.y())
                : QLineF(point.x(), pc_detail::kRulerExtent, point.x(), height());
        } else if (indicator.kind == GeometrySnapIndicatorKind::Alignment && line.length() > 0.0) {
            const QPointF extension = (line.p2() - line.p1()) * (kSnapLineExtension / line.length());
            line = QLineF(line.p1() - extension, line.p2() + extension);
        }
        painter.drawLine(line);
        if (indicator.kind == GeometrySnapIndicatorKind::Contact) {
            painter.drawEllipse(point, kSnapMarkerExtent, kSnapMarkerExtent);
        } else {
            drawCross(point);
            if (!indicator.infiniteLine) {
                drawCross(reference.p1());
                drawCross(reference.p2());
            }
        }
    }
    painter.restore();
}

} // namespace gui
