#include "region_shape_cost.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int kSquareShapeId = 101;
constexpr int kCircleShapeId = 102;
constexpr double kRecognitionAreaRatio = 0.005;
constexpr double kExactCoverageAreaTolerance = 1e-7;
constexpr int kLinesPerEstimatedShape = 4;

double filledArea(const QPainterPath &path) {
    double result = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        double twiceArea = 0.0;
        for (int i = 0; i < polygon.size(); ++i) {
            const QPointF &a = polygon[i];
            const QPointF &b = polygon[(i + 1) % polygon.size()];
            twiceArea += a.x() * b.y() - b.x() * a.y();
        }
        result += std::abs(twiceArea) * 0.5;
    }

    return result;
}

bool closeArea(const QPainterPath &outline, const QPainterPath &candidate,
               double targetArea) {
    const double difference = filledArea(outline.subtracted(candidate))
        + filledArea(candidate.subtracted(outline));

    return difference <= targetArea * kRecognitionAreaRatio;
}

} // namespace

int estimateRegionShapeCount(const QPainterPath &outline) {
    if (outline.isEmpty()) {
        return 0;
    }

    const QRectF bounds = outline.boundingRect();
    const double area = filledArea(outline);
    if (area <= kExactCoverageAreaTolerance) {
        return 0;
    }
    QPainterPath rectangle;
    rectangle.addRect(bounds);
    QPainterPath ellipse;
    ellipse.addEllipse(bounds);
    const int contourCount = outline.toSubpathPolygons().size();
    if (contourCount == 1 && (closeArea(outline, rectangle, area)
                             || closeArea(outline, ellipse, area))) {
        return 1;
    }

    int lines = 0;
    int curves = 0;
    int contours = 0;
    for (int i = 0; i < outline.elementCount(); ++i) {
        const QPainterPath::Element element = outline.elementAt(i);
        if (element.isMoveTo()) {
            ++contours;
        } else if (element.isLineTo()) {
            ++lines;
        } else if (element.type == QPainterPath::CurveToElement) {
            ++curves;
        }
    }
    const int straightShapes =
        (lines + kLinesPerEstimatedShape - 1) / kLinesPerEstimatedShape;

    return std::max(1, straightShapes + curves + std::max(0, contours - 1));
}

PenFillResult fitSingleRegionPrimitive(const QPainterPath &outline,
                                      const QVector<PenPrimitive> &primitives,
                                      double boundaryTolerance) {
    PenFillResult result;
    const QRectF targetBounds = outline.boundingRect();
    const double targetArea = filledArea(outline);
    if (targetBounds.isEmpty() || targetArea <= kExactCoverageAreaTolerance
        || outline.toSubpathPolygons().size() != 1) {
        return result;
    }
    QPainterPath permitted = outline;
    if (boundaryTolerance > 0.0) {
        QPainterPathStroker stroker;
        stroker.setWidth(2.0 * boundaryTolerance);
        permitted = permitted.united(stroker.createStroke(outline));
    }

    for (const PenPrimitive &primitive : primitives) {
        if ((primitive.shapeId != kSquareShapeId
             && primitive.shapeId != kCircleShapeId)
            || primitive.bounds.isEmpty()) {
            continue;
        }
        const double scaleX = targetBounds.width() / primitive.bounds.width();
        const double scaleY = targetBounds.height() / primitive.bounds.height();
        const QTransform transform(scaleX, 0.0, 0.0, scaleY,
            targetBounds.left() - primitive.bounds.left() * scaleX,
            targetBounds.top() - primitive.bounds.top() * scaleY);
        const QPainterPath candidate = transform.map(primitive.silhouette);
        const double candidateArea = filledArea(candidate);
        if (std::abs(candidateArea - targetArea) > targetArea * kRecognitionAreaRatio
            || filledArea(outline.subtracted(candidate)) > kExactCoverageAreaTolerance
            || filledArea(candidate.subtracted(permitted)) > kExactCoverageAreaTolerance
            || !closeArea(outline, candidate, targetArea)) {
            continue;
        }
        result.placements.push_back(
            PenPlacement{primitive.shapeId, transform, candidateArea});
        result.targetArea = targetArea;
        result.coveredArea = targetArea;
        result.outsideArea = filledArea(candidate.subtracted(outline));
        result.shapeLimit = 1;

        return result;
    }

    return result;
}

} // namespace gui
