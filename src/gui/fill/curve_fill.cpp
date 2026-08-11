#include "curve_fill.h"

#include "differential_cover.h"
#include "region_extract.h"
#include "region_fill.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gui {
namespace {

constexpr double kTemplateRasterScale = 2.0;
constexpr int kTemplateRasterMargin = 2;
constexpr double kBoundaryVertexTolerance = 1e-5;
constexpr double kMaximumHardVertexSnapDistance = 4.0;

struct RasterizedTemplate {
    QImage image;
    QTransform sourceToImage;
    QTransform imageToSource;
};

RasterizedTemplate rasterizedTemplate(const QPainterPath &silhouette) {
    RasterizedTemplate result;
    const QRectF bounds = silhouette.boundingRect();
    const int width = std::max(
        1, static_cast<int>(std::ceil(bounds.width() * kTemplateRasterScale))
            + kTemplateRasterMargin * 2);
    const int height = std::max(
        1, static_cast<int>(std::ceil(bounds.height() * kTemplateRasterScale))
            + kTemplateRasterMargin * 2);
    result.sourceToImage.translate(
        kTemplateRasterMargin - bounds.left() * kTemplateRasterScale,
        kTemplateRasterMargin - bounds.top() * kTemplateRasterScale);
    result.sourceToImage.scale(kTemplateRasterScale, kTemplateRasterScale);
    bool invertible = false;
    result.imageToSource = result.sourceToImage.inverted(&invertible);
    if (!invertible) {
        return {};
    }
    result.image = QImage(width, height, QImage::Format_Grayscale8);
    result.image.fill(0);
    QPainter painter(&result.image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.setTransform(result.sourceToImage);
    painter.drawPath(silhouette);
    painter.end();

    return result;
}

QPainterPath highPrecisionTrace(const QImage &image) {
    if (image.isNull()) {
        return {};
    }
    std::vector<std::uint8_t> mask(
        static_cast<size_t>(image.width()) * image.height(), 0);
    for (int y = 0; y < image.height(); ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            mask[static_cast<size_t>(y) * image.width() + x] = line[x] > 0;
        }
    }
    RegionExtractionParams traceOptions;
    traceOptions.traceSpeckle = 0;
    traceOptions.traceOptTolerance = 0.0;

    return traceMaskToPath(mask, image.width(), image.height(),
                           image.rect(), traceOptions);
}

double polygonArea(const QPolygonF &polygon) {
    double twiceArea = 0.0;
    for (int index = 0; index < polygon.size(); ++index) {
        const QPointF &left = polygon[index];
        const QPointF &right = polygon[(index + 1) % polygon.size()];
        twiceArea += left.x() * right.y() - right.x() * left.y();
    }
    return std::abs(twiceArea) * 0.5;
}

QPainterPath largestTracedSubpath(const QPainterPath &source) {
    QVector<QPainterPath> subpaths;
    QPainterPath current;
    for (int index = 0; index < source.elementCount(); ++index) {
        const QPainterPath::Element element = source.elementAt(index);
        if (element.type == QPainterPath::MoveToElement) {
            if (!current.isEmpty()) {
                current.closeSubpath();
                subpaths.push_back(current);
            }
            current = {};
            current.moveTo(element.x, element.y);
        } else if (element.type == QPainterPath::LineToElement) {
            current.lineTo(element.x, element.y);
        } else if (element.type == QPainterPath::CurveToElement
                   && index + 2 < source.elementCount()) {
            const QPainterPath::Element control2 = source.elementAt(index + 1);
            const QPainterPath::Element end = source.elementAt(index + 2);
            current.cubicTo(element.x, element.y,
                            control2.x, control2.y,
                            end.x, end.y);
            index += 2;
        }
    }
    if (!current.isEmpty()) {
        current.closeSubpath();
        subpaths.push_back(current);
    }
    const auto area = [](const QPainterPath &path) {
        double result = 0.0;
        for (const QPolygonF &polygon : path.toFillPolygons()) {
            result += polygonArea(polygon);
        }
        return result;
    };
    const auto largest = std::max_element(
        subpaths.cbegin(), subpaths.cend(),
        [&](const QPainterPath &left, const QPainterPath &right) {
            return area(left) < area(right);
        });
    return largest == subpaths.cend() ? QPainterPath{} : *largest;
}

double pointSegmentDistance(const QPointF &point,
                            const QPointF &start,
                            const QPointF &end) {
    const QPointF segment = end - start;
    const double lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 0.0) {
        return QLineF(point, start).length();
    }
    const double parameter = std::clamp(
        QPointF::dotProduct(point - start, segment) / lengthSquared,
        0.0, 1.0);
    return QLineF(point, start + segment * parameter).length();
}

bool onPrimitiveBoundary(const QPointF &point,
                         const QVector<QPolygonF> &contours) {
    for (const QPolygonF &contour : contours) {
        for (int index = 0; index < contour.size(); ++index) {
            const QPointF &start = contour[index];
            const QPointF &end = contour[(index + 1) % contour.size()];
            if (pointSegmentDistance(point, start, end)
                <= kBoundaryVertexTolerance) {
                return true;
            }
        }
    }
    return false;
}

QVector<QPointF> boundaryTriangleVertices(
    const ShapeGeometry &geometry,
    const PenPrimitive &primitive) {
    QVector<QPointF> result;
    const auto append = [&](const QPointF &point) {
        if (!onPrimitiveBoundary(point, primitive.contours)) {
            return;
        }
        const bool duplicate = std::any_of(
            result.cbegin(), result.cend(), [&](const QPointF &candidate) {
                return QLineF(candidate, point).length()
                    <= kBoundaryVertexTolerance;
            });
        if (!duplicate) {
            result.push_back(point);
        }
    };
    for (const ShapeTriangle &triangle : geometry.triangles) {
        append(triangle.p0);
        append(triangle.p1);
        append(triangle.p2);
    }
    return result;
}

} // namespace

QVector<PenPrimitive> buildCurvePrimitiveCatalog(
    const ShapeGeometryStore &geometry,
    QString *error) {
    QString catalogError;
    const QVector<cover::ShapeMesh> meshes = cover::buildShapeCatalog(
        geometry, &catalogError);
    if (meshes.isEmpty()) {
        if (error != nullptr) {
            *error = catalogError.isEmpty()
                ? QStringLiteral("Curve shape catalog is empty")
                : catalogError;
        }
        return {};
    }

    QVector<PenPrimitive> result;
    result.reserve(meshes.size());
    for (const cover::ShapeMesh &mesh : meshes) {
        const ShapeGeometry *source = geometry.shape(mesh.id);
        if (source == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("shape %1 is unavailable").arg(mesh.id);
            }
            return {};
        }
        PenPrimitive primitive = buildPenPrimitive(mesh.id, *source);
        const RasterizedTemplate raster = rasterizedTemplate(
            primitive.silhouette);
        const QPainterPath traced = largestTracedSubpath(
            highPrecisionTrace(raster.image));
        RegionPenConversionOptions conversionOptions;
        conversionOptions.mergeTolerance = kCurveContourMergeTolerance;
        conversionOptions.maximumDssim = kCurveContourMaximumDssim;
        const QVector<QPointF> preferredVertices =
            boundaryTriangleVertices(*source, primitive);
        conversionOptions.preferredHardPoints.reserve(
            preferredVertices.size());
        for (const QPointF &point : preferredVertices) {
            conversionOptions.preferredHardPoints.push_back(
                raster.sourceToImage.map(point));
        }
        conversionOptions.maximumPreferredHardPointDistance =
            kMaximumHardVertexSnapDistance * kTemplateRasterScale;
        const RegionPenConversionResult conversion =
            optimizeCurveRegionOutline(traced, conversionOptions);
        if (!conversion.valid()) {
            if (error != nullptr) {
                *error = QStringLiteral("shape %1 curve path failed: %2")
                    .arg(mesh.id)
                    .arg(conversion.error);
            }
            return {};
        }
        primitive.curvePoints.reserve(conversion.points.size());
        for (const PenPoint &point : conversion.points) {
            primitive.curvePoints.push_back({
                raster.imageToSource.map(point.position), point.kind});
        }
        const PenContour contour = buildPenContour(primitive.curvePoints);
        if (!contour.valid()) {
            if (error != nullptr) {
                *error = QStringLiteral("shape %1 curve path is invalid")
                    .arg(mesh.id);
            }
            return {};
        }
        primitive.curveSegments = contour.segments;
        result.push_back(std::move(primitive));
    }
    if (error != nullptr) {
        error->clear();
    }

    return result;
}

} // namespace gui
