#include "contour_transform.h"

#include "pen_fill.h"
#include "lining_fill.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

bool penTransformNeedsExplicitHandles(const QTransform &transform) {
    constexpr double kSimilarityTolerance = 1e-10;
    const double horizontalLengthSquared = transform.m11() * transform.m11() + transform.m12() * transform.m12();
    const double verticalLengthSquared = transform.m21() * transform.m21() + transform.m22() * transform.m22();
    const double axisProduct = transform.m11() * transform.m21() + transform.m12() * transform.m22();
    const double tolerance = kSimilarityTolerance * std::max(horizontalLengthSquared, verticalLengthSquared);

    return std::abs(horizontalLengthSquared - verticalLengthSquared) > tolerance
        || std::abs(axisProduct) > tolerance;
}

} // namespace

fls::scene::ContourData transformedContourData(fls::scene::ContourData state, const QTransform &transform) {
    const auto transformPoints = [&transform, needsExplicitHandles = penTransformNeedsExplicitHandles(transform)](
                                     QVector<PenPoint> &points, bool closed) {
        if (needsExplicitHandles) {
            materializePenHandles(points, closed);
        }
        for (PenPoint &point : points) {
            transformPenPoint(point, transform);
        }
    };

    if (state.pathKind == fls::scene::ContourPathKind::Lining) {
        const auto segments = penSegments(state.points, false);
        const double determinant = std::abs(transform.determinant());
        const double widthScale = std::sqrt(determinant);
        for (int pointIndex = 0; pointIndex < state.points.size(); ++pointIndex) {
            QPointF tangent;
            for (int segmentIndex : {pointIndex - 1, pointIndex}) {
                if (segmentIndex >= 0 && segmentIndex < segments.size()) {
                    const QPointF direction = segments[segmentIndex].derivative(segmentIndex == pointIndex ? 0.0 : 1.0);
                    const double length = QLineF({}, direction).length();
                    if (length > 0.0) {
                        tangent += direction / length;
                    }
                }
            }
            const double length = QLineF({}, tangent).length();
            const double mappedLength = length > 0.0
                ? QLineF(transform.map(QPointF()), transform.map(tangent / length)).length() : 0.0;
            auto &point = state.points[pointIndex];
            const double normalScale = mappedLength > 0.0 ? determinant / mappedLength : widthScale;
            point.strokeWidth = liningPointWidth(point, state.strokeWidth) * normalScale;
            if (normalScale > 0.0) point.endCap *= mappedLength / normalScale;
        }
        state.strokeWidth *= widthScale;
    }
    transformPoints(state.points, state.pathKind == fls::scene::ContourPathKind::Pen && state.closed);
    for (int index = 0; index < state.cutouts.size(); ++index) {
        transformPoints(state.cutouts[index], state.cutoutClosed || index + 1 < state.cutouts.size());
    }

    return state;
}

} // namespace gui
