#include "advancing_front.h"
#include "bucket_fill.h"
#include "lining_fill.h"
#include "region_extract.h"
#include "region_fill.h"
#include "region_layer_plan.h"

#include <QtCore>
#include <QtGui>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <vector>

namespace {

class TestContext {
public:
    void expect(bool condition, const char *message)
    {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAIL: " << message << '\n';
    }

    int failures() const { return failures_; }

private:
    int failures_ = 0;
};

void quadraticTo(QPainterPath *path, const QPointF &control, const QPointF &end)
{
    const QPointF start = path->currentPosition();
    const QPointF cubic1 = start + (control - start) * (2.0 / 3.0);
    const QPointF cubic2 = end + (control - end) * (2.0 / 3.0);
    path->cubicTo(cubic1, cubic2, end);
}

bool hasPoint(const QVector<gui::PenPoint> &points,
              const QPointF &position,
              gui::PenPointKind kind)
{
    return std::any_of(points.cbegin(), points.cend(), [&](const gui::PenPoint &point) {
        return point.kind == kind && QLineF(point.position, position).length() <= 1e-6;
    });
}

bool hasConsecutiveSoftPoints(const QVector<gui::PenPoint> &points)
{
    for (int i = 0; i < points.size(); ++i) {
        if (points[i].kind == gui::PenPointKind::Soft
            && points[(i + 1) % points.size()].kind == gui::PenPointKind::Soft) {
            return true;
        }
    }
    return false;
}

bool hasPlacement(const gui::PenFillResult &result, int shapeId)
{
    return std::any_of(result.placements.cbegin(),
                       result.placements.cend(),
                       [&](const gui::PenPlacement &placement) {
        return placement.shapeId == shapeId;
    });
}

QVector<gui::PenPrimitive> penPrimitiveCatalog(TestContext *test)
{
    gui::ShapeGeometryStore geometry;
    QString error;
    test->expect(geometry.loadDefault(&error), "Pen Primitive geometry should load");
    const QVector<gui::PenPrimitive> primitives = gui::buildPenPrimitiveCatalog(geometry);
    for (const int shapeId : {101, 102, 103, 109, 127, 129, 130, 139, 2123}) {
        test->expect(std::any_of(primitives.cbegin(),
                                primitives.cend(),
                                [&](const gui::PenPrimitive &primitive) {
                                    return primitive.shapeId == shapeId;
                                }),
                     "the Pen Primitive catalog should include every fill shape");
    }
    return primitives;
}

double filledPathArea(const QPainterPath &path)
{
    double result = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        double twiceArea = 0.0;
        for (int i = 0; i < polygon.size(); ++i) {
            const QPointF &a = polygon[i];
            const QPointF &b = polygon[(i + 1) % polygon.size()];
            twiceArea += a.x() * b.y() - a.y() * b.x();
        }
        result += std::abs(twiceArea * 0.5);
    }
    return result;
}

QPainterPath placementCoverage(const gui::PenFillResult &result,
                               const QVector<gui::PenPrimitive> &primitives)
{
    QPainterPath coverage;
    coverage.setFillRule(Qt::WindingFill);
    for (const gui::PenPlacement &placement : result.placements) {
        const auto primitive = std::find_if(
            primitives.cbegin(), primitives.cend(),
            [&](const gui::PenPrimitive &candidate) {
                return candidate.shapeId == placement.shapeId;
            });
        if (primitive != primitives.cend()) {
            coverage = coverage.united(
                placement.transform.map(primitive->silhouette));
        }
    }
    return coverage;
}

gui::ExtractedRegion rasterRegion(const std::vector<int> &labels,
                                  const QSize &size,
                                  int label,
                                  const QColor &color)
{
    std::vector<std::uint8_t> mask(labels.size(), 0);
    QRect bounds;
    int area = 0;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const int pixel = y * size.width() + x;
            if (labels[static_cast<size_t>(pixel)] != label) {
                continue;
            }
            mask[static_cast<size_t>(pixel)] = 1;
            const QRect pixelBounds(x, y, 1, 1);
            bounds = bounds.isNull() ? pixelBounds : bounds.united(pixelBounds);
            ++area;
        }
    }
    gui::RegionExtractionParams params;
    params.traceSpeckle = 0;
    gui::ExtractedRegion region;
    region.id = label;
    region.color = color;
    region.outline = gui::traceMaskToPath(mask, size.width(), size.height(), bounds, params);
    region.bounds = bounds;
    region.area = area;

    return region;
}

gui::RegionExtractionResult rasterExtraction(const std::vector<int> &labels,
                                              const QSize &size,
                                              QVector<gui::ExtractedRegion> regions)
{
    QSet<int> lineartLabels;
    for (const gui::ExtractedRegion &region : regions) {
        if (region.lineart) {
            lineartLabels.insert(region.id);
        }
    }
    auto raster = QSharedPointer<gui::RegionRasterData>::create();
    raster->labels = labels;
    raster->foreground.resize(labels.size(), 0);
    raster->lineart.resize(labels.size(), 0);
    for (size_t pixel = 0; pixel < labels.size(); ++pixel) {
        raster->foreground[pixel] = labels[pixel] >= 0 ? 1 : 0;
        raster->lineart[pixel] = lineartLabels.contains(labels[pixel]) ? 1 : 0;
    }
    raster->traceParams.traceSpeckle = 0;
    gui::RegionExtractionResult result;
    result.imageSize = size;
    result.colorRegionCount = regions.size();
    result.regions = std::move(regions);
    result.raster = raster;

    return result;
}

struct PrimitiveEnds {
    QPointF tip;
    QPointF base;
    QPointF middle;
};

std::optional<PrimitiveEnds> primitiveEnds(const gui::PenPrimitive &primitive)
{
    QVector<QPointF> points;
    for (const QPolygonF &contour : primitive.contours) {
        points += contour;
    }
    if (points.size() < 3) {
        return std::nullopt;
    }
    QPointF centroid;
    for (const QPointF &point : points) {
        centroid += point;
    }
    centroid /= points.size();

    double xx = 0.0;
    double xy = 0.0;
    double yy = 0.0;
    for (const QPointF &point : points) {
        const QPointF delta = point - centroid;
        xx += delta.x() * delta.x();
        xy += delta.x() * delta.y();
        yy += delta.y() * delta.y();
    }
    const double angle = 0.5 * std::atan2(2.0 * xy, xx - yy);
    const QPointF axis(std::cos(angle), std::sin(angle));
    const QPointF normal(-axis.y(), axis.x());
    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    for (const QPointF &point : points) {
        const double along = QPointF::dotProduct(point - centroid, axis);
        minimum = std::min(minimum, along);
        maximum = std::max(maximum, along);
    }
    const double radius = (maximum - minimum) * 0.04;
    const auto endpoint = [&](double position) {
        QPointF center;
        int count = 0;
        double acrossMinimum = std::numeric_limits<double>::max();
        double acrossMaximum = std::numeric_limits<double>::lowest();
        for (const QPointF &point : points) {
            const QPointF delta = point - centroid;
            if (std::abs(QPointF::dotProduct(delta, axis) - position) > radius) {
                continue;
            }
            const double across = QPointF::dotProduct(delta, normal);
            acrossMinimum = std::min(acrossMinimum, across);
            acrossMaximum = std::max(acrossMaximum, across);
            center += point;
            ++count;
        }
        return QPair<QPointF, double>(count > 0 ? center / count : QPointF(),
                                      acrossMaximum - acrossMinimum);
    };
    const auto first = endpoint(minimum);
    const auto second = endpoint(maximum);
    const double middlePosition = (minimum + maximum) * 0.5;
    const double middleRadius = (maximum - minimum) * 0.08;
    double middleMinimum = std::numeric_limits<double>::max();
    double middleMaximum = std::numeric_limits<double>::lowest();
    for (const QPointF &point : points) {
        const QPointF delta = point - centroid;
        if (std::abs(QPointF::dotProduct(delta, axis) - middlePosition)
            > middleRadius) {
            continue;
        }
        const double across = QPointF::dotProduct(delta, normal);
        middleMinimum = std::min(middleMinimum, across);
        middleMaximum = std::max(middleMaximum, across);
    }
    const QPointF middle = centroid + axis * middlePosition
        + normal * ((middleMinimum + middleMaximum) * 0.5);
    return first.second < second.second
        ? std::optional<PrimitiveEnds>(PrimitiveEnds{first.first, second.first, middle})
        : std::optional<PrimitiveEnds>(PrimitiveEnds{second.first, first.first, middle});
}

std::optional<QPointF> primitiveTip(const gui::PenPrimitive &primitive)
{
    const std::optional<PrimitiveEnds> ends = primitiveEnds(primitive);
    return ends.has_value() ? std::optional<QPointF>(ends->tip) : std::nullopt;
}

std::optional<QPointF> placedHairTip(const gui::PenPlacement &placement,
                                     const QVector<gui::PenPrimitive> &primitives)
{
    if (placement.shapeId != 2112) {
        return std::nullopt;
    }
    const auto primitive = std::find_if(primitives.cbegin(),
                                        primitives.cend(),
                                        [](const gui::PenPrimitive &candidate) {
        return candidate.shapeId == 2112;
    });
    if (primitive == primitives.cend()) {
        return std::nullopt;
    }
    const std::optional<QPointF> tip = primitiveTip(*primitive);
    return tip.has_value() ? std::optional<QPointF>(placement.transform.map(*tip))
                           : std::nullopt;
}

bool placementsConnected(const gui::PenFillResult &result,
                         const QVector<gui::PenPrimitive> &primitives)
{
    QVector<QPainterPath> placedPaths;
    for (const gui::PenPlacement &placement : result.placements) {
        const auto primitive = std::find_if(primitives.cbegin(),
                                            primitives.cend(),
                                            [&](const gui::PenPrimitive &candidate) {
            return candidate.shapeId == placement.shapeId;
        });
        if (primitive != primitives.cend()) {
            placedPaths.push_back(placement.transform.map(primitive->silhouette));
        }
    }
    if (placedPaths.isEmpty()) {
        return false;
    }
    QSet<int> connected{0};
    bool changed = true;
    while (changed) {
        changed = false;
        for (int candidate = 0; candidate < placedPaths.size(); ++candidate) {
            if (connected.contains(candidate)) {
                continue;
            }
            for (const int existing : std::as_const(connected)) {
                if (filledPathArea(placedPaths[existing].intersected(placedPaths[candidate]))
                    > 1e-4) {
                    connected.insert(candidate);
                    changed = true;
                    break;
                }
            }
        }
    }
    return connected.size() == placedPaths.size();
}

QVector<gui::PenPrimitive> liningPrimitiveCatalog(TestContext *test)
{
    gui::ShapeGeometryStore geometry;
    QString error;
    test->expect(geometry.loadDefault(&error), "Lining Primitive geometry should load");
    const QVector<gui::PenPrimitive> primitives = gui::buildLiningPrimitiveCatalog(geometry);
    for (const int shapeId : {2112, 2131, 136, 2133}) {
        test->expect(std::any_of(primitives.cbegin(),
                                primitives.cend(),
                                [&](const gui::PenPrimitive &primitive) {
                                    return primitive.shapeId == shapeId;
                                }),
                     "the Lining Primitive catalog should include every lining shape");
    }
    test->expect(std::none_of(primitives.cbegin(),
                              primitives.cend(),
                              [](const gui::PenPrimitive &primitive) {
        return primitive.shapeId == 139 || primitive.shapeId == 2123
            || primitive.shapeId == 2128;
    }),
                 "the Lining Primitive catalog should exclude unsuitable fill shapes");
    return primitives;
}

void alternatingCurvatureMerges(TestContext *test)
{
    QPainterPath path;
    path.moveTo(0.0, 0.0);
    quadraticTo(&path, QPointF(5.0, 5.0), QPointF(10.0, 0.0));
    quadraticTo(&path, QPointF(15.0, -5.0), QPointF(20.0, 0.0));
    quadraticTo(&path, QPointF(22.0, 14.0), QPointF(10.0, 20.0));
    quadraticTo(&path, QPointF(-2.0, 14.0), QPointF(0.0, 0.0));
    path.closeSubpath();

    const gui::RegionPenConversionResult result = gui::regionOutlineToPenPoints(path);
    test->expect(result.valid(), "alternating-curvature contour should convert");
    test->expect(result.removedHardPoints >= 1,
                 "alternating outer/inner sectors should remove a shared hard point");
    test->expect(hasConsecutiveSoftPoints(result.points),
                 "a merged sector should be represented by consecutive soft points");
    test->expect(result.maximumDeviation
                     <= result.baselineDeviation + 1.1 + 1e-6,
                 "optimized contour should stay inside the cumulative deviation budget");
    test->expect(gui::buildPenContour(result.points).valid(),
                 "optimized alternating contour should remain valid for Pen");
}

void sharpLineCornersStayHard(TestContext *test)
{
    QPainterPath path;
    path.moveTo(0.0, 0.0);
    path.lineTo(20.0, 0.0);
    quadraticTo(&path, QPointF(25.0, 10.0), QPointF(20.0, 20.0));
    path.lineTo(0.0, 20.0);
    path.lineTo(0.0, 0.0);
    path.closeSubpath();

    const gui::RegionPenConversionResult result = gui::regionOutlineToPenPoints(path);
    test->expect(result.valid(), "mixed line/curve contour should convert");
    test->expect(hasPoint(result.points, QPointF(20.0, 0.0), gui::PenPointKind::Hard),
                 "Potrace line-to-curve corner should stay hard");
    test->expect(hasPoint(result.points, QPointF(20.0, 20.0), gui::PenPointKind::Hard),
                 "Potrace curve-to-line corner should stay hard");
}

void excessiveDisplacementDoesNotMerge(TestContext *test)
{
    QPainterPath path;
    path.moveTo(0.0, 0.0);
    path.lineTo(0.0, 10.0);
    quadraticTo(&path, QPointF(5.0, 15.0), QPointF(10.0, 10.0));
    quadraticTo(&path, QPointF(15.0, 0.0), QPointF(20.0, 10.0));
    path.lineTo(20.0, 0.0);
    path.lineTo(0.0, 0.0);
    path.closeSubpath();

    gui::RegionPenConversionOptions options;
    options.mergeTolerance = 0.1;
    const gui::RegionPenConversionResult result =
        gui::regionOutlineToPenPoints(path, options);
    test->expect(result.valid(), "strict-tolerance contour should still convert");
    test->expect(hasPoint(result.points, QPointF(10.0, 10.0), gui::PenPointKind::Hard),
                 "a hard junction beyond the merge tolerance should remain");
}

void negligibleSoftRunBecomesLine(TestContext *test)
{
    QPainterPath path;
    path.moveTo(0.0, 0.0);
    path.lineTo(0.0, 20.0);
    quadraticTo(&path, QPointF(5.0, 20.4), QPointF(10.0, 20.0));
    quadraticTo(&path, QPointF(15.0, 19.6), QPointF(20.0, 20.0));
    path.lineTo(20.0, 0.0);
    path.lineTo(0.0, 0.0);
    path.closeSubpath();

    const gui::RegionPenConversionResult result = gui::regionOutlineToPenPoints(path);
    test->expect(result.valid() && result.points.size() == 4,
                 "a negligible soft run should reduce to its hardpoint chord");
    test->expect(result.removedHardPoints == 1 && result.removedSoftPoints == 2,
                 "soft-run reduction should report hard and soft removals separately");
    test->expect(gui::buildPenContour(result.points).valid(),
                 "a straightened soft run should retain a valid contour");

    gui::RegionPenConversionOptions hardOnlyOptions;
    hardOnlyOptions.straightenSoftRuns = false;
    const gui::RegionPenConversionResult hardOnly =
        gui::regionOutlineToPenPoints(path, hardOnlyOptions);
    test->expect(hardOnly.valid() && hardOnly.removedHardPoints == 1
                     && hardOnly.removedSoftPoints == 0 && hardOnly.points.size() == 6,
                 "hard-only reduction should retain a recoverable curved contour");

    gui::RegionFillContourStats fallbackStats;
    QVector<gui::PenPoint> fallbackPoints;
    const gui::PenFillResult unavailableFill = gui::fillRegionOutline(
        path, {}, 0.5, {}, nullptr, &fallbackStats, &fallbackPoints);
    test->expect(!unavailableFill.error.isEmpty()
                     && fallbackStats.removedSoftPoints == 0
                     && fallbackPoints.size() == hardOnly.points.size(),
                 "a failed soft-run fit should expose hard-only geometry to fallback");
}

void visibleSoftRunRemainsCurved(TestContext *test)
{
    QPainterPath path;
    path.moveTo(0.0, 0.0);
    path.lineTo(0.0, 20.0);
    quadraticTo(&path, QPointF(5.0, 24.0), QPointF(10.0, 20.0));
    quadraticTo(&path, QPointF(15.0, 16.0), QPointF(20.0, 20.0));
    path.lineTo(20.0, 0.0);
    path.lineTo(0.0, 0.0);
    path.closeSubpath();

    const gui::RegionPenConversionResult result = gui::regionOutlineToPenPoints(path);
    test->expect(result.valid() && result.removedSoftPoints == 0,
                 "a visible soft run should remain curved");
    test->expect(hasConsecutiveSoftPoints(result.points),
                 "retained curvature should preserve the intermediate soft controls");
}

void rasterDssimGuardsSoftRunReduction(TestContext *test)
{
    QPainterPath path;
    path.moveTo(5.0, 5.0);
    path.lineTo(5.0, 25.0);
    quadraticTo(&path, QPointF(10.0, 25.8), QPointF(15.0, 25.0));
    quadraticTo(&path, QPointF(20.0, 24.2), QPointF(25.0, 25.0));
    path.lineTo(25.0, 5.0);
    path.lineTo(5.0, 5.0);
    path.closeSubpath();

    gui::RegionPenConversionOptions permissive;
    permissive.comparisonImageSize = QSize(32, 32);
    permissive.maximumDssim = 1.0;
    const gui::RegionPenConversionResult reduced =
        gui::regionOutlineToPenPoints(path, permissive);
    gui::RegionPenConversionOptions exact = permissive;
    exact.maximumDssim = 0.0;
    const gui::RegionPenConversionResult retained =
        gui::regionOutlineToPenPoints(path, exact);

    test->expect(reduced.valid() && reduced.removedSoftPoints == 2,
                 "a permissive raster guard should accept a negligible soft run");
    test->expect(retained.valid() && retained.removedSoftPoints == 0,
                 "an exact raster guard should retain a raster-visible soft run");
}

void containedHoleIsIgnored(TestContext *test)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addRect(QRectF(0.0, 0.0, 30.0, 30.0));
    QPainterPath hole;
    hole.addRect(QRectF(10.0, 10.0, 10.0, 10.0));
    path.addPath(hole);

    const gui::RegionPenConversionResult result = gui::regionOutlineToPenPoints(path);
    test->expect(result.valid(), "one closed outer with a contained hole should convert");
    test->expect(result.points.size() == 4,
                 "contained hole points should not be emitted to the single-contour Pen model");
}

void invalidContoursAreRejected(TestContext *test)
{
    QPainterPath open;
    open.moveTo(0.0, 0.0);
    open.lineTo(10.0, 0.0);
    open.lineTo(10.0, 10.0);
    const gui::RegionPenConversionResult openResult = gui::regionOutlineToPenPoints(open);
    test->expect(!openResult.valid() && openResult.error.contains("open"),
                 "open contours should be rejected explicitly");

    QPainterPath multiple;
    multiple.addRect(QRectF(0.0, 0.0, 10.0, 10.0));
    multiple.addRect(QRectF(20.0, 0.0, 10.0, 10.0));
    const gui::RegionPenConversionResult multipleResult =
        gui::regionOutlineToPenPoints(multiple);
    test->expect(!multipleResult.valid() && multipleResult.error.contains("multiple"),
                 "multiple independent outer contours should be rejected");

    QPainterPath crossed;
    crossed.moveTo(0.0, 0.0);
    crossed.lineTo(10.0, 10.0);
    crossed.lineTo(0.0, 10.0);
    crossed.lineTo(10.0, 0.0);
    crossed.lineTo(0.0, 0.0);
    crossed.closeSubpath();
    const gui::RegionPenConversionResult crossedResult =
        gui::regionOutlineToPenPoints(crossed);
    test->expect(!crossedResult.valid(), "self-intersecting contours should be rejected");
}

void conversionIsDeterministic(TestContext *test)
{
    QPainterPath path;
    path.moveTo(0.0, 0.0);
    quadraticTo(&path, QPointF(5.0, 5.0), QPointF(10.0, 0.0));
    quadraticTo(&path, QPointF(15.0, -5.0), QPointF(20.0, 0.0));
    quadraticTo(&path, QPointF(22.0, 14.0), QPointF(10.0, 20.0));
    quadraticTo(&path, QPointF(-2.0, 14.0), QPointF(0.0, 0.0));
    path.closeSubpath();

    const gui::RegionPenConversionResult first = gui::regionOutlineToPenPoints(path);
    const gui::RegionPenConversionResult second = gui::regionOutlineToPenPoints(path);
    test->expect(first.points.size() == second.points.size(),
                 "repeated conversions should have the same point count");
    if (first.points.size() != second.points.size()) {
        return;
    }
    for (int i = 0; i < first.points.size(); ++i) {
        test->expect(first.points[i].kind == second.points[i].kind
                         && first.points[i].position == second.points[i].position,
                     "repeated conversions should produce identical Pen points");
    }
}

void bucketFloodIsContiguousAndToleranceBounded(TestContext *test)
{
    QImage image(6, 3, QImage::Format_ARGB32);
    image.fill(QColor(20, 40, 60));
    for (int y = 0; y < image.height(); ++y) {
        image.setPixelColor(2, y, QColor(25, 45, 65));
        image.setPixelColor(3, y, QColor(180, 40, 60));
        image.setPixelColor(4, y, QColor(20, 40, 60));
        image.setPixelColor(5, y, QColor(20, 40, 60));
    }

    const gui::BucketFillResult exact =
        gui::floodGuideRegion(image, QPoint(0, 1), 0);
    test->expect(exact.valid() && exact.area == 6,
                 "zero tolerance should select only the exact contiguous color");

    const gui::BucketFillResult tolerant =
        gui::floodGuideRegion(image, QPoint(0, 1), 5);
    test->expect(tolerant.valid() && tolerant.area == 9,
                 "tolerance should include nearby RGBA values");
    test->expect(tolerant.averageColor == QColor(21, 41, 61, 255),
                 "bucket fill should report the average RGBA color of selected pixels");
    test->expect(tolerant.mask[4] == 0,
                 "matching pixels beyond a nonmatching barrier should stay unselected");

    image.setPixelColor(0, 1, Qt::transparent);
    const gui::BucketFillResult transparent =
        gui::floodGuideRegion(image, QPoint(0, 1), 255);
    test->expect(!transparent.valid() && transparent.error.contains("transparent"),
                 "transparent seed pixels should be rejected");

    const QImage preview = gui::bucketMaskPreview(tolerant);
    test->expect(!preview.isNull() && qAlpha(preview.pixel(0, 0)) > 0
                     && qAlpha(preview.pixel(4, 0)) == 0,
                 "bucket preview should rasterize only selected mask pixels");
}

void bucketMaskTracesIntoPenContour(TestContext *test)
{
    QImage image(20, 20, QImage::Format_ARGB32);
    image.fill(QColor(30, 40, 180));
    for (int y = 4; y <= 15; ++y) {
        for (int x = 3; x <= 16; ++x) {
            image.setPixelColor(x, y, QColor(220, 50, 40));
        }
    }
    for (int y = 8; y <= 11; ++y) {
        for (int x = 8; x <= 11; ++x) {
            image.setPixelColor(x, y, QColor(30, 40, 180));
        }
    }

    const gui::BucketFillResult fill =
        gui::floodGuideRegion(image, QPoint(5, 5), 0);
    test->expect(fill.valid(), "bucket mask should be valid before tracing");
    test->expect(fill.averageColor == QColor(220, 50, 40),
                 "end-to-end bucket fill should retain its region average color");
    gui::RegionExtractionParams traceOptions;
    traceOptions.traceSpeckle = 0;
    const QPainterPath traced = gui::traceMaskToPath(fill.mask,
                                                     image.width(),
                                                     image.height(),
                                                     fill.bounds,
                                                     traceOptions);
    test->expect(!traced.isEmpty(), "Potrace should vectorize the bucket mask");
    constexpr double kBucketRdpEpsilon = 2.0;
    constexpr double kBucketMinimumCurveBow = 0.75;
    constexpr int kBucketRdpCurveSamples = 32;
    const QPolygonF sourceContour =
        gui::regionOuterContour(traced, kBucketRdpCurveSamples);
    const QVector<gui::PenPoint> points =
        gui::simplifyClosedPolygonRdpHybridQuadratic(
            sourceContour, kBucketRdpEpsilon, kBucketMinimumCurveBow);
    test->expect(points.size() < sourceContour.size(),
                 "Bucket hybrid quadratic RDP should reduce the traced contour");
    test->expect(gui::buildPenContour(points).valid(),
                 "the hybrid quadratic Bucket contour should be consumable by Pen");
}

void rdpHybridQuadraticMatchesAnalyzer(TestContext *test) {
    QPolygonF circle;
    constexpr int kPointCount = 64;
    constexpr double kRadius = 20.0;
    constexpr double kEpsilon = 2.0;
    constexpr double kMinimumCurveBow = 0.75;

    circle.reserve(kPointCount);
    for (int index = 0; index < kPointCount; ++index) {
        const double angle = 2.0 * std::acos(-1.0) * index / kPointCount;
        circle.push_back({kRadius * std::cos(angle), kRadius * std::sin(angle)});
    }

    const QVector<gui::PenPoint> hybrid =
        gui::simplifyClosedPolygonRdpHybridQuadratic(
            circle, kEpsilon, kMinimumCurveBow);
    test->expect(hybrid.size() == 16,
                 "the hybrid quadratic port should match the analyzer point count");
    test->expect(!hybrid.isEmpty()
                     && hybrid.front().kind == gui::PenPointKind::Hard
                     && QLineF(hybrid.front().position, QPointF(20.0, 0.0)).length() <= 1e-9,
                 "the hybrid quadratic port should retain the analyzer seam anchor");
    test->expect(hybrid.size() > 1
                     && hybrid[1].kind == gui::PenPointKind::Soft
                     && QLineF(hybrid[1].position,
                               QPointF(19.878942791656183,
                                       8.234127709942868)).length() <= 1e-9,
                 "the hybrid quadratic port should match the analyzer control fit");
    test->expect(gui::buildPenContour(hybrid).valid(),
                 "the analyzer-matched hybrid contour should remain valid for Pen");

    const QVector<gui::PenPoint> straight =
        gui::simplifyClosedPolygonRdpHybridQuadratic(circle, kEpsilon, kRadius);
    test->expect(straight.size() == 8
                     && std::all_of(straight.cbegin(), straight.cend(),
                                    [](const gui::PenPoint &point) {
                                        return point.kind == gui::PenPointKind::Hard;
                                    }),
                 "the bow threshold should retain the same anchors as straight spans");
}

void advancingFrontFillsSimpleEllipse(TestContext *test)
{
    gui::ShapeGeometryStore geometry;
    QString error;
    test->expect(geometry.loadDefault(&error),
                 "Advancing Front Primitive geometry should load");
    const QVector<gui::PenPrimitive> primitives =
        gui::buildPenPrimitiveCatalog(geometry);
    const gui::PolygonMeshSources meshSources =
        gui::buildPolygonMeshSources(geometry);
    QPainterPath target;
    target.addEllipse(QRectF(8.0, 12.0, 48.0, 24.0));
    gui::AdvancingFrontOptions options = gui::defaultAdvancingFrontOptions();
    options.workerCount = 2;
    options.seedEdgeCount = 4;
    options.finalistCount = 8;
    options.cheapBudgetCap = 4096;
    options.dssimBudgetCap = 256;
    const gui::AdvancingFrontResult result = gui::fillRegionAdvancingFront(
        target, primitives, meshSources, QSize(64, 48), options);
    test->expect(result.valid(),
                 "Advancing Front should fill a simple ellipse");
    test->expect(!result.placements.isEmpty(),
                 "Advancing Front should emit at least one placement");
    test->expect(std::isfinite(result.stats.finalDssim),
                 "Advancing Front should report a finite final DSSIM");
    test->expect(result.structurallyComplete
                     && result.stats.targetCoverage
                         >= options.minimumTargetCoverage,
                 "Advancing Front should verify explicit target coverage");
}

void advancingFrontHonorsSafeShapeCeiling(TestContext *test)
{
    gui::ShapeGeometryStore geometry;
    QString error;
    test->expect(geometry.loadDefault(&error),
                 "Advancing Front Primitive geometry should load for shape ceiling");
    const QVector<gui::PenPrimitive> primitives =
        gui::buildPenPrimitiveCatalog(geometry);
    const gui::PolygonMeshSources meshSources =
        gui::buildPolygonMeshSources(geometry);
    QPainterPath target;
    target.addEllipse(QRectF(8.0, 12.0, 48.0, 24.0));
    gui::AdvancingFrontOptions options = gui::defaultAdvancingFrontOptions();
    test->expect(options.fallbackContourEpsilon
                     == options.originalContourEpsilon,
                 "Advancing Front fallback should use the Safe contour epsilon");
    options.maximumOutputShapes = 0;
    const gui::AdvancingFrontResult result = gui::fillRegionAdvancingFront(
        target, primitives, meshSources, QSize(64, 48), options);
    test->expect(!result.valid() && result.placements.isEmpty(),
                 "Advancing Front should not replace a one-shape Safe result");
    test->expect(result.stats.countLimitReached,
                 "Advancing Front should report the Safe shape ceiling");
}

void advancingFrontPreflightRejectsUnprofitableSearch(TestContext *test)
{
    gui::ShapeGeometryStore geometry;
    QString error;
    test->expect(geometry.loadDefault(&error),
                 "Advancing Front Primitive geometry should load for preflight");
    const QVector<gui::PenPrimitive> primitives =
        gui::buildPenPrimitiveCatalog(geometry);
    const gui::PolygonMeshSources meshSources =
        gui::buildPolygonMeshSources(geometry);
    const QPolygonF polygon{{8.0, 8.0}, {42.0, 6.0}, {58.0, 22.0},
                            {48.0, 42.0}, {24.0, 38.0}, {6.0, 24.0}};
    const gui::PenFillResult baseline = gui::fillPolygonMesh(polygon, meshSources);
    test->expect(baseline.error.isEmpty() && baseline.placements.size() > 1,
                 "Advancing Front preflight fixture should have multiple Safe shapes");
    QPainterPath target;
    target.addPolygon(polygon);
    target.closeSubpath();
    gui::AdvancingFrontOptions options = gui::defaultAdvancingFrontOptions();
    options.baselinePlacements = baseline.placements;
    options.maximumOutputShapes = baseline.placements.size() - 1;
    options.preflightCandidateBudget = 1;
    options.minimumBaselineShapesReplaced = baseline.placements.size() + 1;
    const gui::AdvancingFrontResult result = gui::fillRegionAdvancingFront(
        target, primitives, meshSources, QSize(64, 48), options);
    test->expect(!result.valid() && result.placements.isEmpty(),
                 "An unprofitable preflight should reuse Safe without remeshing");
    test->expect(result.stats.preflightRejected
                     && result.stats.preflightCandidates <= 1,
                 "Advancing Front preflight should honor its detached work budget");
    test->expect(result.error.contains(QStringLiteral("Preflight")),
                 "Advancing Front should preserve the preflight rejection reason");
}

QPainterPath smoothCircularPath(int curveCount)
{
    constexpr double radius = 1000.0;
    QPainterPath path;
    path.moveTo(radius, 0.0);
    const double step = 2.0 * std::acos(-1.0) / curveCount;
    for (int i = 0; i < curveCount; ++i) {
        const double middle = (i + 0.5) * step;
        const double end = (i + 1.0) * step;
        quadraticTo(&path,
                    QPointF(radius * std::cos(middle) / std::cos(step * 0.5),
                            radius * std::sin(middle) / std::cos(step * 0.5)),
                    QPointF(radius * std::cos(end), radius * std::sin(end)));
    }
    path.closeSubpath();
    return path;
}

void conversionOptimizationIsBounded(TestContext *test)
{
    const gui::RegionPenConversionResult atLimit =
        gui::regionOutlineToPenPoints(smoothCircularPath(64));
    test->expect(atLimit.valid(), "a contour at the optimization limit should convert");
    test->expect(!atLimit.optimizationSkipped,
                 "a contour at the optimization limit should be simplified");
    test->expect(atLimit.originalPointCount == 128 && atLimit.removedHardPoints == 63,
                 "local simplification should merge every redundant circular junction");

    const gui::RegionPenConversionResult overLegacyLimit =
        gui::regionOutlineToPenPoints(smoothCircularPath(65));
    test->expect(overLegacyLimit.valid(),
                 "a contour over the former optimization limit should convert");
    test->expect(!overLegacyLimit.optimizationSkipped
                     && overLegacyLimit.removedHardPoints > 0,
                 "dense contours should use the topology-safe optimizer");
    test->expect(overLegacyLimit.points.size() < overLegacyLimit.originalPointCount,
                 "dense contour optimization should reduce its Pen point count");

    gui::RegionPenConversionOptions capped;
    capped.maxOptimizedPointCount = 128;
    const gui::RegionPenConversionResult explicitlyCapped =
        gui::regionOutlineToPenPoints(smoothCircularPath(65), capped);
    test->expect(explicitlyCapped.valid() && explicitlyCapped.optimizationSkipped,
                 "an explicit caller cutoff should still skip dense optimization");
    test->expect(explicitlyCapped.points.size() == explicitlyCapped.originalPointCount,
                 "an explicit cutoff should retain all baseline points");
}

void rasterDssimGuardsCurveSimplification(TestContext *test)
{
    QPainterPath path;
    path.moveTo(65.0, 40.0);
    quadraticTo(&path, QPointF(65.0, 15.0), QPointF(40.0, 15.1));
    quadraticTo(&path, QPointF(15.0, 15.0), QPointF(15.0, 40.0));
    quadraticTo(&path, QPointF(15.0, 65.0), QPointF(40.0, 65.0));
    quadraticTo(&path, QPointF(65.0, 65.0), QPointF(65.0, 40.0));
    path.closeSubpath();

    gui::RegionPenConversionOptions permissive;
    permissive.comparisonImageSize = QSize(80, 80);
    const gui::RegionPenConversionResult accepted =
        gui::regionOutlineToPenPoints(path, permissive);
    test->expect(accepted.valid() && accepted.dssim <= permissive.maximumDssim + 1e-9,
                 "accepted raster-guarded optimization should stay below its DSSIM cap");

    gui::RegionPenConversionOptions exact = permissive;
    exact.maximumDssim = 0.0;
    const gui::RegionPenConversionResult guarded =
        gui::regionOutlineToPenPoints(path, exact);
    test->expect(guarded.valid() && guarded.dssim <= 1e-9,
                 "a zero DSSIM cap should retain an identical raster contour");
    test->expect(guarded.points.size() >= accepted.points.size(),
                 "tightening DSSIM must not produce a more aggressive simplification");
}

void arcPrimitivesFillCurvedBoundaries(TestContext *test)
{
    const QVector<gui::PenPrimitive> primitives = penPrimitiveCatalog(test);

    gui::PenFillRequest halfCircle;
    halfCircle.primitives = primitives;
    halfCircle.boundaryTolerance = 0.5;
    halfCircle.points = {{{-50.0, 0.0}, gui::PenPointKind::Hard},
                         {{-50.0, -50.0}, gui::PenPointKind::Soft},
                         {{0.0, -50.0}, gui::PenPointKind::Hard},
                         {{50.0, -50.0}, gui::PenPointKind::Soft},
                         {{50.0, 0.0}, gui::PenPointKind::Hard},
                         {{50.0, 80.0}, gui::PenPointKind::Hard},
                         {{-50.0, 80.0}, gui::PenPointKind::Hard}};
    const gui::PenFillResult halfCircleResult = gui::fillPenPath(halfCircle);
    test->expect(halfCircleResult.error.isEmpty(), "a semicircular exterior should fill");
    test->expect(hasPlacement(halfCircleResult, 102) || hasPlacement(halfCircleResult, 109),
                 "a semicircular exterior should use a contained curve Primitive");

    gui::PenFillRequest quarterCircle;
    quarterCircle.primitives = primitives;
    quarterCircle.boundaryTolerance = 0.5;
    quarterCircle.points = {{{0.0, -50.0}, gui::PenPointKind::Hard},
                            {{50.0, -50.0}, gui::PenPointKind::Soft},
                            {{50.0, 0.0}, gui::PenPointKind::Hard},
                            {{50.0, 80.0}, gui::PenPointKind::Hard},
                            {{-50.0, 80.0}, gui::PenPointKind::Hard},
                            {{-50.0, -50.0}, gui::PenPointKind::Hard}};
    const gui::PenFillResult quarterCircleResult = gui::fillPenPath(quarterCircle);
    test->expect(quarterCircleResult.error.isEmpty(), "a quarter-circle exterior should fill");
    test->expect(hasPlacement(quarterCircleResult, 130),
                 "a quarter-circle exterior should prefer the quarter-circle Primitive");
    test->expect(!hasPlacement(quarterCircleResult, 102),
                 "a fitted quarter-circle should not fall back to a full circle");

    gui::PenFillRequest inwardArc;
    inwardArc.primitives = primitives;
    inwardArc.boundaryTolerance = 0.5;
    inwardArc.points = {{{0.0, -50.0}, gui::PenPointKind::Hard},
                        {{0.0, 0.0}, gui::PenPointKind::Soft},
                        {{50.0, 0.0}, gui::PenPointKind::Hard},
                        {{50.0, 100.0}, gui::PenPointKind::Hard},
                        {{-50.0, 100.0}, gui::PenPointKind::Hard},
                        {{-50.0, -50.0}, gui::PenPointKind::Hard}};
    const gui::PenFillResult inwardArcResult = gui::fillPenPath(inwardArc);
    test->expect(inwardArcResult.error.isEmpty(), "an internal arc should fill");
    test->expect(hasPlacement(inwardArcResult, 127)
                     || hasPlacement(inwardArcResult, 129)
                     || hasPlacement(inwardArcResult, 139)
                     || hasPlacement(inwardArcResult, 2123),
                 "an internal arc should use a contained curve Primitive");
}

void concaveCoreFallbackStaysContained(TestContext *test)
{
    const QVector<gui::PenPrimitive> catalog = penPrimitiveCatalog(test);
    QVector<gui::PenPrimitive> primitives;
    for (const gui::PenPrimitive &primitive : catalog) {
        if (primitive.shapeId == 101 || primitive.shapeId == 103) {
            primitives.push_back(primitive);
        }
    }
    gui::PenPrimitive unavailableCurve;
    unavailableCurve.shapeId = 127;
    unavailableCurve.silhouette.addRect(QRectF(-1.0, -1.0, 2.0, 2.0));
    unavailableCurve.bounds = unavailableCurve.silhouette.boundingRect();
    unavailableCurve.area = 4.0;
    primitives.push_back(unavailableCurve);

    gui::PenFillRequest request;
    request.primitives = primitives;
    request.boundaryTolerance = 0.5;
    request.points = {{{0.0, -50.0}, gui::PenPointKind::Hard},
                      {{0.0, 0.0}, gui::PenPointKind::Soft},
                      {{50.0, 0.0}, gui::PenPointKind::Hard},
                      {{50.0, 100.0}, gui::PenPointKind::Hard},
                      {{-50.0, 100.0}, gui::PenPointKind::Hard},
                      {{-50.0, -50.0}, gui::PenPointKind::Hard}};
    const gui::PenContour contour = gui::buildPenContour(request.points);
    const gui::PenFillResult result = gui::fillPenPath(request);
    const QPainterPath coreCoverage = placementCoverage(result, primitives);
    const double outsideArea =
        filledPathArea(coreCoverage.subtracted(contour.path));
    test->expect(result.error.isEmpty(),
                 "a concave core should fill without a curve Primitive");
    test->expect(outsideArea <= result.targetArea * 1e-5 + 1e-6,
                 "fallback core shapes should remain within a concave boundary");
}

void negligibleCorePlacementsAreDiscarded(TestContext *test)
{
    gui::PenFillRequest baselineRequest;
    baselineRequest.primitives = penPrimitiveCatalog(test);
    baselineRequest.boundaryTolerance = 0.1;
    baselineRequest.discardNegligiblePlacements = false;
    baselineRequest.points = {{{0.0, 0.0}, gui::PenPointKind::Hard},
                              {{100.0, 0.0}, gui::PenPointKind::Hard},
                              {{100.0, 100.0}, gui::PenPointKind::Hard},
                              {{0.0, 100.0}, gui::PenPointKind::Hard},
                              {{0.0, 50.1}, gui::PenPointKind::Hard},
                              {{-0.1, 50.0}, gui::PenPointKind::Hard},
                              {{0.0, 49.9}, gui::PenPointKind::Hard}};
    const gui::PenFillResult baseline = gui::fillPenPath(baselineRequest);
    gui::PenFillRequest optimizedRequest = baselineRequest;
    optimizedRequest.discardNegligiblePlacements = true;
    const gui::PenFillResult optimized = gui::fillPenPath(optimizedRequest);
    const double baselineMissing = filledPathArea(baseline.unfilled);
    const double optimizedMissing = filledPathArea(optimized.unfilled);
    test->expect(baseline.error.isEmpty() && optimized.error.isEmpty(),
                 "a contour with a negligible core ear should fill");
    test->expect(optimized.placements.size() < baseline.placements.size(),
                 "the cleanup pass should discard a negligible core placement");
    test->expect(optimizedMissing - baselineMissing
                     <= optimized.targetArea * 1e-3 + 1e-6,
                 "discarded placements should stay within the cleanup area budget");
    test->expect(optimized.outsideArea <= baseline.outsideArea + 1e-6,
                 "discarding a placement should not increase leakage");
}

void automaticRegionFillRetainsCurvedBoundary(TestContext *test)
{
    QPainterPath outline;
    outline.moveTo(0.0, -50.0);
    quadraticTo(&outline, QPointF(50.0, -50.0), QPointF(50.0, 0.0));
    outline.lineTo(50.0, 80.0);
    outline.lineTo(-50.0, 80.0);
    outline.lineTo(-50.0, -50.0);
    outline.closeSubpath();

    const QVector<gui::PenPrimitive> primitives = penPrimitiveCatalog(test);
    const gui::PenFillResult result =
        gui::fillRegionOutline(outline, primitives, 0.5);
    test->expect(result.error.isEmpty(),
                 "automatic region outline should use the curve-aware Pen fitter");
    test->expect(hasPlacement(result, 130),
                 "automatic region outline should retain its quarter-circle Primitive");

    QPolygonF optimizedContour;
    gui::RegionFillContourStats contourStats;
    QVector<gui::PenPoint> optimizedPenPoints;
    const gui::PenFillResult interrupted = gui::fillRegionOutline(
        outline, primitives, 0.5, []() { return true; }, &optimizedContour,
        &contourStats, &optimizedPenPoints);
    test->expect(interrupted.cancelled,
                 "an interrupted region fit should report cancellation");
    test->expect(optimizedContour.size() >= 3,
                 "an interrupted region fit should retain its optimized contour for fallback");
    test->expect(contourStats.originalPointCount >= contourStats.optimizedPointCount
                     && contourStats.optimizedPointCount >= 3
                     && contourStats.flattenedPointCount == optimizedContour.size(),
                 "region fill diagnostics should report original, optimized, and flattened points");
    test->expect(optimizedPenPoints.size() == contourStats.optimizedPointCount,
                 "region fill diagnostics should retain every optimized Pen point");
    const QPolygonF simplifiedFallback =
        gui::simplifyClosedPolygon(optimizedContour, 0.45);
    test->expect(gui::buildPolygonContour(simplifiedFallback).valid(),
                 "fallback RDP should retain a valid non-crossing contour");
    test->expect(simplifiedFallback.size() <= optimizedContour.size(),
                 "fallback RDP must not increase the mesh point count");

    constexpr int width = 96;
    constexpr int height = 96;
    std::vector<std::uint8_t> circleMask(static_cast<size_t>(width) * height, 0);
    const QPointF center(width * 0.5, height * 0.5);
    constexpr double radius = 30.0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const QPointF delta(x + 0.5 - center.x(), y + 0.5 - center.y());
            if (QPointF::dotProduct(delta, delta) <= radius * radius) {
                circleMask[static_cast<size_t>(y) * width + x] = 1;
            }
        }
    }
    gui::RegionExtractionParams traceOptions;
    traceOptions.traceSpeckle = 0;
    const QPainterPath tracedCircle = gui::traceMaskToPath(
        circleMask, width, height, QRect(0, 0, width, height), traceOptions);
    const gui::PenFillResult tracedResult =
        gui::fillRegionOutline(tracedCircle, primitives, 1.0);
    const bool hasCurvePlacement = std::any_of(
        tracedResult.placements.cbegin(), tracedResult.placements.cend(),
        [](const gui::PenPlacement &placement) {
            return placement.shapeId != 101 && placement.shapeId != 103;
        });
    test->expect(tracedResult.error.isEmpty(),
                 "a Potrace-generated region should pass through the automatic Pen fitter");
    test->expect(hasCurvePlacement,
                 "a Potrace-generated circle should not flatten to only Squares/Triangles");
}

void denseValidPolygonTriangulates(TestContext *test)
{
    gui::PolygonMeshRequest request;
    constexpr int pointCount = 4000;
    constexpr double radius = 500.0;
    request.points.reserve(pointCount);
    for (int i = 0; i < pointCount; ++i) {
        const double angle = 2.0 * 3.14159265358979323846 * i / pointCount;
        request.points.push_back({radius * std::cos(angle), radius * std::sin(angle)});
    }
    request.sources.triangle = {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};
    request.mergeSquares = false;
    const gui::PolygonMeshResult result = gui::meshPolygon(request);
    if (!result.error.isEmpty()) {
        std::cerr << "Dense polygon mesh error: "
                  << result.error.toStdString() << '\n';
    }
    test->expect(result.error.isEmpty(),
                 "a dense valid polygon should not lose every shallow ear to scale tolerance");
    test->expect(result.placements.size() == pointCount - 2,
                 "a dense valid polygon should produce one triangle per clipped ear");
}

void smallRegionMergesIntoClosestColorNeighbor(TestContext *test)
{
    QImage image(9, 5, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, x < 4 ? QColor(200, 0, 0)
                                             : QColor(220, 100, 0));
        }
    }
    image.setPixelColor(4, 2, QColor(216, 82, 0));

    gui::RegionExtractionParams params;
    params.maxColorCount = 3;
    params.colorMergeDistance = 0.0;
    params.colorFrequencyFloor = 0.0;
    params.minRegionArea = 1;
    params.smallRegionMergeArea = 2;
    params.blurPasses = 0;
    params.traceSpeckle = 0;
    const gui::RegionExtractionResult result = gui::extractRegions(image, params);
    test->expect(result.valid(), "small-region merge fixture should extract");
    test->expect(result.mergedSmallRegionCount == 1,
                 "one sub-threshold region should be merged");
    test->expect(result.colorRegionCount == 2,
                 "merged small region should not survive as a third region");
    const auto orange = std::find_if(
        result.regions.cbegin(), result.regions.cend(), [](const gui::ExtractedRegion &region) {
            return !region.lineart && region.color.green() > 50;
        });
    test->expect(orange != result.regions.cend(),
                 "closest orange neighbor should remain after merging");
    if (orange != result.regions.cend()) {
        test->expect(orange->area == 25,
                     "small region pixels should be reassigned to the closest-color neighbor");
    }
    QFile diagnosticFile(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("region_extract.log")));
    test->expect(diagnosticFile.open(QIODevice::ReadOnly | QIODevice::Text),
                 "region extraction should write its diagnostic log");
    if (diagnosticFile.isOpen()) {
        const QByteArray diagnostic = diagnosticFile.readAll();
        test->expect(diagnostic.contains("pass requested_colors="),
                     "region extraction diagnostics should include progressive pass metrics");
        test->expect(diagnostic.contains("votes sum="),
                     "region extraction diagnostics should include progressive line votes");
        test->expect(diagnostic.contains("cleanup narrow="),
                     "region extraction diagnostics should include fringe cleanup decisions");
        test->expect(diagnostic.contains("neighbor label="),
                     "region extraction diagnostics should include component adjacency");
    }
}

void lineartDiagnosticCapturesComponentTopology(TestContext *test) {
    QImage image(23, 9, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QColor color(60, 120, 220);
            if (x < 10) {
                color = QColor(220, 80, 60);
            } else if (x == 10) {
                color = QColor(105, 42, 33);
            } else if (x == 11) {
                color = QColor(10, 10, 10);
            } else if (x == 12) {
                color = QColor(33, 60, 105);
            }
            image.setPixelColor(x, y, color);
        }
    }

    gui::RegionExtractionParams params;
    params.maxColorCount = 5;
    params.colorMergeDistance = 0.0;
    params.colorFrequencyFloor = 0.0;
    params.minRegionArea = 1;
    params.smallRegionMergeArea = 0;
    params.blurPasses = 0;
    params.traceSpeckle = 0;
    const gui::RegionExtractionResult result = gui::extractRegions(image, params);
    test->expect(result.valid() && result.colorRegionCount == 2,
                  "lineart cleanup fixture should absorb both fringe regions into their fills");
    test->expect(result.lineartRegionCount == 1,
                  "lineart diagnostic fixture should detect its separating stroke");
    const bool expandedFills = std::all_of(
        result.regions.cbegin(), result.regions.cend(), [](const gui::ExtractedRegion &region) {
            return region.lineart || region.area == 99;
        });
    test->expect(expandedFills,
                 "lineart fringe pixels should be retained in neighboring color regions");

    QFile diagnosticFile(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("region_extract.log")));
    test->expect(diagnosticFile.open(QIODevice::ReadOnly | QIODevice::Text),
                 "lineart extraction should write its diagnostic log");
    if (diagnosticFile.isOpen()) {
        const QByteArray diagnostic = diagnosticFile.readAll();
        test->expect(diagnostic.contains("core_lineart_pixels=9"),
                     "lineart diagnostics should measure the final lineart mask");
        test->expect(diagnostic.contains("final_lineart=yes"),
                     "lineart diagnostics should identify final lineart components");
        test->expect(diagnostic.contains("boundary_edges=9"),
                     "lineart diagnostics should measure color-to-line boundary contact");
        test->expect(diagnostic.contains("cleanup_merged_labels=2"),
                     "lineart diagnostics should record both merged fringe labels");
        test->expect(diagnostic.count("action=merged-color") == 2,
                     "lineart diagnostics should record each fringe merge target");
    }
}

void blendWithoutLineVotesRemainsAColorRegion(TestContext *test) {
    QImage image(28, 9, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QColor color(60, 120, 220);
            if (x < 10) {
                color = QColor(220, 80, 60);
            } else if (x == 10 || x == 11) {
                color = QColor(199, 73, 55);
            } else if (x == 12 || x == 13) {
                color = QColor(10, 10, 10);
            } else if (x == 14 || x == 15) {
                color = QColor(55, 109, 199);
            }
            image.setPixelColor(x, y, color);
        }
    }
    image.setPixelColor(0, 0, QColor(10, 10, 10));

    gui::RegionExtractionParams params;
    params.maxColorCount = 5;
    params.colorMergeDistance = 0.0;
    params.colorFrequencyFloor = 0.0;
    params.minRegionArea = 2;
    params.smallRegionMergeArea = 0;
    params.blurPasses = 0;
    params.traceSpeckle = 0;
    const gui::RegionExtractionResult result = gui::extractRegions(image, params);
    test->expect(result.valid() && result.colorRegionCount == 4,
                 "color blending without progressive line votes should not trigger cleanup");
    test->expect(result.lineartRegionCount == 1,
                 "the separating stroke should remain lineart when blend regions are retained");
}

void residualLineHistoryMergesMissedComponent(TestContext *test) {
    QImage image(23, 9, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QColor color(60, 120, 220);
            if (x < 10) {
                color = QColor(220, 80, 60);
            } else if (x == 10 || x == 11) {
                color = QColor(180, 100, 100);
            } else if (x == 12) {
                color = QColor(10, 10, 10);
            }
            image.setPixelColor(x, y, color);
        }
    }

    gui::RegionExtractionParams params;
    params.maxColorCount = 4;
    params.colorMergeDistance = 0.0;
    params.colorFrequencyFloor = 0.0;
    params.minRegionArea = 1;
    params.smallRegionMergeArea = 0;
    params.blurPasses = 0;
    params.traceSpeckle = 0;
    const gui::RegionExtractionResult result = gui::extractRegions(image, params);
    test->expect(result.valid() && result.colorRegionCount == 2,
                 "residual line history should absorb a component missed by the strict gate");
    test->expect(result.lineartRegionCount == 1,
                 "residual cleanup should retain the recognized separating stroke");

    QFile diagnosticFile(QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("region_extract.log")));
    test->expect(diagnosticFile.open(QIODevice::ReadOnly | QIODevice::Text),
                 "residual cleanup should write its diagnostic log");
    if (diagnosticFile.isOpen()) {
        const QByteArray diagnostic = diagnosticFile.readAll();
        test->expect(diagnostic.contains("cleanup_residual_evidence_labels=1"),
                     "residual cleanup diagnostics should count the missed component");
        test->expect(diagnostic.contains("evidence_reason=residual-line-history"),
                     "residual cleanup diagnostics should record historical evidence");
    }
}

void regionLayerPlanAbsorbsBehindEnclosedRegion(TestContext *test)
{
    const QSize size(24, 24);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 0);
    for (int y = 11; y <= 12; ++y) {
        for (int x = 11; x <= 12; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 1, QColor(30, 110, 220)));
    sourceRegions.push_back(rasterRegion(labels, size, 0, QColor(220, 60, 40)));
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlan plan = gui::buildRegionLayerPlan(extraction);
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "an enclosed-region plan should pass exact raster validation");
    test->expect(plan.units.size() == 2 && plan.absorbedRegionCount == 1,
                 "a small enclosed region should retain an overlay over one backdrop");
    if (plan.units.size() == 2) {
        test->expect(plan.units[0].sourceRegionIndices == QVector<int>{1}
                         && plan.units[0].absorbedRegionIndices == QVector<int>{0},
                     "the surrounding region should absorb the enclosed footprint");
        test->expect(plan.units[1].sourceRegionIndices == QVector<int>{0},
                     "the enclosed region should draw after its surrounding region");
    }
}

void regionLayerPlanMergesAdjacentExactColors(TestContext *test)
{
    const QSize size(16, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 0);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = size.width() / 2; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor color(180, 70, 50);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, color));
    sourceRegions.push_back(rasterRegion(labels, size, 1, color));
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlan plan = gui::buildRegionLayerPlan(extraction);
    test->expect(!plan.fallback && plan.units.size() == 1,
                 "adjacent regions with an exact color match should form one fill unit");
    test->expect(plan.sameColorMergeCount == 1
                     && plan.units.front().sourceRegionIndices == QVector<int>{0, 1},
                 "a same-color unit should retain both source mappings");
}

void regionLayerPlanBridgesNearbyExactColors(TestContext *test)
{
    const QSize size(14, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 2);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < 5; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
        for (int x = 7; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor backdrop(180, 70, 50);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, backdrop));
    sourceRegions.push_back(rasterRegion(labels, size, 1, backdrop));
    sourceRegions.push_back(rasterRegion(labels, size, 2, QColor(40, 100, 190)));
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlan plan = gui::buildRegionLayerPlan(extraction);
    if (plan.fallback || plan.units.size() != 2) {
        std::cerr << "nearby-plan: "
                  << plan.diagnostics.join(QStringLiteral(" | ")).toStdString() << '\n';
    }
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "a nearby-color bridge should preserve the baseline rendering");
    test->expect(plan.units.size() == 2 && plan.nearbySameColorMergeCount == 1,
                 "nearby exact colors should share one bridged backdrop unit");
    if (plan.units.size() == 2) {
        test->expect(plan.units[0].sourceRegionIndices == QVector<int>{0, 1}
                         && plan.units[1].sourceRegionIndices == QVector<int>{2},
                     "intervening color should remain above the bridged backdrop");
    }
}

void regionLayerPlanDoesNotBridgeBackground(TestContext *test)
{
    const QSize size(12, 6);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), -1);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < 5; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
        for (int x = 7; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor color(180, 70, 50);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, color));
    sourceRegions.push_back(rasterRegion(labels, size, 1, color));
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    const gui::RegionLayerPlan &plan = variants.safe;
    if (plan.fallback || plan.units.size() != 2) {
        std::cerr << "background-plan: "
                  << plan.diagnostics.join(QStringLiteral(" | ")).toStdString() << '\n';
    }
    test->expect(!plan.fallback && plan.units.size() == 2
                     && plan.nearbySameColorMergeCount == 0,
                 "nearby colors should remain separate across image background");
    test->expect(variants.dangerous.units.size() == 2
                     && variants.dangerous.hierarchicalContourMergeCount == 0,
                 "dangerous contour bridges should never cross transparent background");
}

void regionLayerPlanUsesBaselineOverlapOrder(TestContext *test)
{
    const QSize size(10, 6);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 0);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = size.width() / 2; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, QColor(190, 70, 40)));
    sourceRegions.push_back(rasterRegion(labels, size, 1, QColor(40, 90, 190)));
    QPainterPath sharedOutline;
    sharedOutline.addRect(QRectF(QPointF(0.0, 0.0), QSizeF(size)));
    sourceRegions[0].outline = sharedOutline;
    sourceRegions[1].outline = sharedOutline;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlan plan = gui::buildRegionLayerPlan(extraction);
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "overlapping source contours should preserve baseline rendering");
    test->expect(plan.units.size() == 2 && plan.orderingEdgeCount == 1
                     && plan.units[0].sourceRegionIndices == QVector<int>{0}
                     && plan.units[1].sourceRegionIndices == QVector<int>{1},
                 "overlap dependencies should follow the baseline top owner");
}

void regionLayerPlanRejectsOnlyCyclicNearbyBridge(TestContext *test)
{
    const QSize size(14, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 1);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < 5; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
        for (int x = 7; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 2;
        }
    }
    const QColor repeatedColor(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, QColor(40, 90, 190)));
    sourceRegions.push_back(rasterRegion(labels, size, 2, repeatedColor));
    QPainterPath overlappingMiddle;
    overlappingMiddle.addRect(QRectF(5.0, 0.0, 5.0, size.height()));
    sourceRegions[1].outline = overlappingMiddle;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlan plan = gui::buildRegionLayerPlan(extraction);
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "a cyclic nearby merge should retain a valid layer plan");
    test->expect(plan.units.size() == 3 && plan.nearbySameColorMergeCount == 0
                     && plan.nearbyConflictRejectCount == 1
                     && plan.suppressedOperationCount == 1
                     && plan.conflictIsolatedSourceCount == 0,
                 "only the nearby bridge responsible for a dependency cycle should be rejected");
    test->expect(plan.diagnostics.join(QLatin1Char('\n')).contains(
                     QStringLiteral("action=suppress-nearby-merge")),
                 "cycle diagnostics should identify the suppressed nearby operation");
}

void ellipseLikeCoreUsesOneCircle(TestContext *test)
{
    gui::ShapeGeometryStore geometry;
    QString error;
    test->expect(geometry.loadDefault(&error),
                 "ellipse core Primitive geometry should load");
    const gui::PolygonMeshSources sources = gui::buildPolygonMeshSources(geometry);
    QPainterPath corePath;
    corePath.addPolygon(sources.circle);
    corePath.closeSubpath();
    QVector<gui::PolygonMeshPlacement> placements;
    for (int i = 0; i < 12; ++i) {
        placements.push_back({103, QTransform()});
    }
    const QVector<gui::PolygonMeshPlacement> optimized =
        gui::optimizePolygonMeshWithEllipses(
            placements, sources, corePath);

    test->expect(optimized.size() == 1 && optimized.front().shapeId == 102,
                 "an ellipse-like large core should replace its mesh with one Circle");
}

void regionLayerPlanRetainsUnrelatedSafeBridge(TestContext *test)
{
    const QSize size(40, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), -1);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < 5; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
        for (int x = 5; x < 7; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
        for (int x = 7; x < 12; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 2;
        }
        for (int x = 20; x < 25; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 3;
        }
        for (int x = 25; x < 27; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 4;
        }
        for (int x = 27; x < 34; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 5;
        }
    }
    const QColor cyclicColor(190, 70, 40);
    const QColor safeColor(70, 180, 90);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, cyclicColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, QColor(40, 90, 190)));
    sourceRegions.push_back(rasterRegion(labels, size, 2, cyclicColor));
    sourceRegions.push_back(rasterRegion(labels, size, 3, safeColor));
    sourceRegions.push_back(rasterRegion(labels, size, 4, QColor(170, 80, 180)));
    sourceRegions.push_back(rasterRegion(labels, size, 5, safeColor));
    QPainterPath overlappingMiddle;
    overlappingMiddle.addRect(QRectF(5.0, 0.0, 5.0, size.height()));
    sourceRegions[1].outline = overlappingMiddle;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlan plan = gui::buildRegionLayerPlan(extraction);
    if (plan.units.size() != 5 || plan.nearbySameColorMergeCount != 1
        || plan.nearbyConflictRejectCount != 1) {
        std::cerr << "independent-bridge-plan: "
                  << plan.diagnostics.join(QStringLiteral(" | ")).toStdString() << '\n';
    }
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "independent safe and cyclic bridges should retain an exact plan");
    test->expect(plan.units.size() == 5 && plan.nearbySameColorMergeCount == 1
                     && plan.nearbyConflictRejectCount == 1
                     && plan.suppressedOperationCount == 1
                     && plan.conflictIsolatedSourceCount == 0,
                 "cycle rollback should preserve an unrelated safe nearby merge");
}

void regionLayerPlanRejectsComplexForeignBridge(TestContext *test)
{
    const QSize size(14, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 0);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 5; x < 6; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 2;
        }
        for (int x = 6; x < 8; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 3;
        }
        for (int x = 8; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor repeatedColor(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 2, QColor(40, 90, 190)));
    sourceRegions.push_back(rasterRegion(labels, size, 3, QColor(80, 170, 100)));
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    const gui::RegionLayerPlan &plan = variants.safe;
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "a complex foreign-owner bridge should retain the source plan");
    test->expect(plan.units.size() == 4 && plan.nearbySameColorMergeCount == 0
                     && plan.nearbyConflictRejectCount == 1
                     && plan.nearbyForeignOwnerRejectCount == 1,
                 "nearby bridging should reject paths crossing multiple foreign owners");
}

void regionLayerPlanSuppressesResidualCycleMerge(TestContext *test)
{
    const QSize size(20, 20);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 0);
    for (int y = size.height() / 2; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 2;
        }
    }
    for (int y = 8; y < 12; ++y) {
        for (int x = 8; x < 12; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor repeatedColor(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, QColor(40, 90, 190)));
    sourceRegions.push_back(rasterRegion(labels, size, 2, repeatedColor));
    QPainterPath overlappingCenter;
    overlappingCenter.addRect(QRectF(8.0, 8.0, 4.0, 6.0));
    sourceRegions[1].outline = overlappingCenter;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    const gui::RegionLayerPlan &plan = variants.safe;
    if (plan.adjacentConflictRejectCount != 1
        || plan.suppressedOperationCount != 2) {
        std::cerr << "residual-cycle-plan: "
                  << plan.diagnostics.join(QStringLiteral(" | ")).toStdString() << '\n';
    }
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "a residual dependency cycle should retain a valid source plan");
    test->expect(plan.units.size() == 3 && plan.sameColorMergeCount == 0
                     && plan.adjacentConflictRejectCount == 1
                     && plan.suppressedOperationCount == 2
                     && plan.conflictIsolatedSourceCount == 0,
                 "a residual cycle should suppress only its absorption and adjacent operations");
    const QString diagnostics = plan.diagnostics.join(QLatin1Char('\n'));
    test->expect(diagnostics.contains(QStringLiteral("action=suppress-absorption"))
                     && diagnostics.contains(
                         QStringLiteral("action=suppress-adjacent-merge")),
                 "residual-cycle diagnostics should identify both suppressed operations");
    test->expect(!variants.dangerous.fallback
                     && variants.dangerous.dangerousCycleBreakCount > 0
                     && variants.dangerous.suppressedOperationCount == 0,
                 "dangerous cycle recovery should force an order without suppressing operations");
    const QString dangerousDiagnostics =
        variants.dangerous.diagnostics.join(QLatin1Char('\n'));
    test->expect(dangerousDiagnostics.contains(QStringLiteral(
                     "dangerous_safety_policy=accept-any-output"))
                     && dangerousDiagnostics.contains(QStringLiteral(
                         "WARNING=Dangerous safety checks are disabled")),
                 "dangerous cycle recovery should log its unsafe acceptance policy");
}

void regionLayerPlanSuppressesValidationMismatchMerge(TestContext *test)
{
    const QSize size(16, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 0);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = size.width() / 2; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor color(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, color));
    sourceRegions.push_back(rasterRegion(labels, size, 1, color));
    QPainterPath leftOutline;
    leftOutline.addRect(QRectF(1.0, 0.0, 7.0, size.height()));
    QPainterPath rightOutline;
    rightOutline.addRect(QRectF(8.0, 0.0, 7.0, size.height()));
    sourceRegions[0].outline = leftOutline;
    sourceRegions[1].outline = rightOutline;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    const gui::RegionLayerPlan &plan = variants.safe;
    if (plan.adjacentConflictRejectCount != 1
        || plan.suppressedOperationCount != 1) {
        std::cerr << "validation-mismatch-plan: "
                  << plan.diagnostics.join(QStringLiteral(" | ")).toStdString() << '\n';
    }
    test->expect(!plan.fallback && plan.validationMismatchPixels == 0,
                 "a changed merged contour should retain the baseline rendering");
    test->expect(plan.units.size() == 2 && plan.sameColorMergeCount == 0
                     && plan.adjacentConflictRejectCount == 1
                     && plan.suppressedOperationCount == 1
                     && plan.conflictIsolatedSourceCount == 0,
                 "a validation mismatch should suppress only its adjacent merge operation");
    test->expect(variants.dangerous.units.size() == 1
                     && variants.dangerous.sameColorMergeCount == 1
                     && variants.dangerous.validationMismatchPixels > 0
                     && variants.dangerous.suppressedOperationCount == 0
                     && variants.dangerous.diagnostics.join(QLatin1Char('\n')).contains(
                         QStringLiteral("dangerous_variant=unsafe-output-accepted")),
                 "the dangerous variant should retain validation-mismatching optimization");
}

void dangerousRegionLayerPlanBuildsHierarchicalContourBridge(TestContext *test)
{
    const QSize size(30, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 2);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < 5; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
        for (int x = 20; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor repeatedColor(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 2, QColor(30, 30, 35)));
    sourceRegions.back().lineart = true;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    test->expect(variants.safe.units.size() == 2
                     && variants.safe.sameColorMergeCount == 0,
                 "the safe plan should retain regions separated by a wide edge region");
    test->expect(variants.dangerous.units.size() == 1
                     && variants.dangerous.nearbySameColorMergeCount == 1
                     && variants.dangerous.edgeComponentMergeCount == 0
                     && variants.dangerous.hierarchicalContourMergeCount == 1
                     && variants.dangerous.validationMismatchPixels > 0,
                 "the dangerous plan should bridge same-colour regions across edge-only pixels");
    const QString diagnostics =
        variants.dangerous.diagnostics.join(QLatin1Char('\n'));
    const bool hierarchyDiagnostics = diagnostics.contains(QStringLiteral(
        "bridge_kind=hierarchical-contour"))
        && diagnostics.contains(QStringLiteral(
            "bridge_fill=straight-edge-corridor"))
        && diagnostics.contains(QStringLiteral("hierarchy_level=1"))
        && diagnostics.contains(QStringLiteral(
            "hierarchical_route_policy=outer-boundary edge-only"))
        && diagnostics.contains(QStringLiteral("straight_visibility=yes"))
        && diagnostics.contains(QStringLiteral("estimated_shapes="));
    if (!hierarchyDiagnostics) {
        std::cerr << "hierarchical-bridge-plan: "
                  << diagnostics.toStdString() << '\n';
    }
    test->expect(hierarchyDiagnostics,
                 "dangerous bridge diagnostics should identify its contour hierarchy");
    test->expect(variants.dangerous.diagnostics.join(QLatin1Char('\n')).contains(
                     QStringLiteral("nearby_candidate_cache=hit")),
                 "dangerous finalization should reuse its scored bridge candidates");

    bool cancelRequested = false;
    const gui::RegionLayerPlanVariants cancelledVariants =
        gui::buildRegionLayerPlanVariants(
            extraction,
            [&](const QString &phase, int completed, int) {
                if (phase == QStringLiteral("Planning Dangerous contour hierarchy")
                    && completed > 0) {
                    cancelRequested = true;
                }
            },
            [&]() { return cancelRequested; });
    test->expect(cancelRequested && cancelledVariants.dangerous.cancelled,
                 "dangerous bridge planning should honor cancellation");
}

void dangerousRegionLayerPlanDoesNotCrossColor(TestContext *test)
{
    const QSize size(30, 8);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 2);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < 5; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
        labels[static_cast<size_t>(y) * size.width() + 14] = 3;
        for (int x = 25; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor repeatedColor(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 2, QColor(30, 30, 35)));
    sourceRegions.back().lineart = true;
    sourceRegions.push_back(rasterRegion(labels, size, 3, QColor(40, 100, 190)));
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    test->expect(variants.dangerous.units.size() == 3
                     && variants.dangerous.hierarchicalContourMergeCount == 0,
                 "dangerous contour bridges should not cross a colored barrier");
}

void dangerousRegionLayerPlanRequiresStraightEdgeVisibility(TestContext *test)
{
    const QSize size(30, 20);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), -1);
    for (int y = 1; y < 6; ++y) {
        for (int x = 1; x < 6; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
    }
    for (int y = 14; y < 19; ++y) {
        for (int x = 24; x < 29; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    for (int y = 2; y < 5; ++y) {
        for (int x = 5; x < 16; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 2;
        }
    }
    for (int y = 2; y < 17; ++y) {
        for (int x = 13; x < 16; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 2;
        }
    }
    for (int y = 14; y < 17; ++y) {
        for (int x = 14; x < 25; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 2;
        }
    }
    const QColor repeatedColor(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 2, QColor(30, 30, 35)));
    sourceRegions.back().lineart = true;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    test->expect(variants.dangerous.units.size() == 2
                     && variants.dangerous.hierarchicalContourMergeCount == 0,
                 "a winding edge component should not substitute for straight visibility");
}

void dangerousRegionLayerPlanUsesMultipleStraightAttachments(TestContext *test)
{
    const QSize size(30, 32);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 2);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < 8; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 0;
        }
        for (int x = 18; x < size.width(); ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    const QColor repeatedColor(190, 70, 40);
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 1, repeatedColor));
    sourceRegions.push_back(rasterRegion(labels, size, 2, QColor(30, 30, 35)));
    sourceRegions.back().lineart = true;
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    const QString diagnostics =
        variants.dangerous.diagnostics.join(QLatin1Char('\n'));
    bool multipleAttachments = false;
    for (int count = 2; count <= 8; ++count) {
        multipleAttachments = multipleAttachments
            || diagnostics.contains(QStringLiteral("straight_attachments=%1").arg(count));
    }
    if (variants.dangerous.units.size() != 1 || !multipleAttachments) {
        std::cerr << "multiple-straight-attachments-plan: "
                  << diagnostics.toStdString() << '\n';
    }
    test->expect(variants.dangerous.units.size() == 1 && multipleAttachments,
                 "facing outer boundaries should use multiple simplifying attachments");
}

void dangerousRegionLayerPlanAbsorbsLargeContainedRegion(TestContext *test)
{
    const QSize size(20, 20);
    std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), 0);
    for (int y = 5; y < 15; ++y) {
        for (int x = 5; x < 15; ++x) {
            labels[static_cast<size_t>(y) * size.width() + x] = 1;
        }
    }
    QVector<gui::ExtractedRegion> sourceRegions;
    sourceRegions.push_back(rasterRegion(labels, size, 0, QColor(190, 70, 40)));
    sourceRegions.push_back(rasterRegion(labels, size, 1, QColor(40, 90, 190)));
    const gui::RegionExtractionResult extraction =
        rasterExtraction(labels, size, std::move(sourceRegions));

    const gui::RegionLayerPlanVariants variants =
        gui::buildRegionLayerPlanVariants(extraction);
    test->expect(variants.safe.absorbedRegionCount == 0,
                 "the safe plan should retain the area limit for containment absorption");
    test->expect(variants.dangerous.absorbedRegionCount == 1
                     && variants.dangerous.largeContainedAbsorptionCount == 1
                     && variants.dangerous.units.size() == 2
                     && variants.dangerous.units.front().absorbedRegionIndices.contains(1)
                     && variants.dangerous.units.back().sourceRegionIndices.contains(1),
                 "the dangerous plan should fill through a large enclosed region and redraw it");
}

void dangerousGeometryUsesOnlySameColorOrLineart(TestContext *test)
{
    const QSize size(18, 14);
    const QColor sourceColor(190, 70, 40);
    const QPointF notchCenter(8.5, 4.5);
    const auto labelsWithNotch = [&](int notchLabel) {
        std::vector<int> labels(static_cast<size_t>(size.width()) * size.height(), -1);
        for (int y = 2; y <= 11; ++y) {
            for (int x = 2; x <= 15; ++x) {
                labels[static_cast<size_t>(y) * size.width() + x] = 0;
            }
        }
        for (int y = 2; y <= 7; ++y) {
            for (int x = 7; x <= 10; ++x) {
                labels[static_cast<size_t>(y) * size.width() + x] = notchLabel;
            }
        }

        return labels;
    };

    std::vector<int> lineartLabels = labelsWithNotch(1);
    QVector<gui::ExtractedRegion> lineartRegions;
    lineartRegions.push_back(rasterRegion(lineartLabels, size, 0, sourceColor));
    lineartRegions.push_back(
        rasterRegion(lineartLabels, size, 1, QColor(30, 30, 35)));
    lineartRegions.back().lineart = true;
    const gui::RegionLayerPlanVariants lineartVariants =
        gui::buildRegionLayerPlanVariants(
            rasterExtraction(lineartLabels, size, std::move(lineartRegions)));
    const bool lineartNotchFilled = lineartVariants.dangerous.units.size() == 1
        && lineartVariants.dangerous.units.front().outline.contains(notchCenter);
    if (lineartVariants.dangerous.geometryPointReductionCount <= 0
        || (lineartVariants.dangerous.morphologicalClosingCount
            + lineartVariants.dangerous.convexSimplificationCount) != 1
        || lineartVariants.dangerous.units.size() != 1
        || !lineartNotchFilled) {
        std::cerr << "dangerous-geometry-lineart: "
                  << "reduction="
                  << lineartVariants.dangerous.geometryPointReductionCount
                  << " closing="
                  << lineartVariants.dangerous.morphologicalClosingCount
                  << " convex="
                  << lineartVariants.dangerous.convexSimplificationCount
                  << " units=" << lineartVariants.dangerous.units.size()
                  << " contains=" << lineartNotchFilled
                  << " | "
                  << lineartVariants.dangerous.diagnostics.join(
                         QStringLiteral(" | ")).toStdString()
                  << '\n';
    }
    test->expect(lineartVariants.dangerous.geometryPointReductionCount > 0
                     && (lineartVariants.dangerous.morphologicalClosingCount
                         + lineartVariants.dangerous.convexSimplificationCount) == 1
                     && lineartVariants.dangerous.units.size() == 1
                     && lineartNotchFilled && !lineartVariants.dangerous.fallback,
                 "dangerous geometry should simplify a concavity made only of lineart pixels");

    std::vector<int> foreignColorLabels = labelsWithNotch(1);
    QVector<gui::ExtractedRegion> foreignColorRegions;
    foreignColorRegions.push_back(
        rasterRegion(foreignColorLabels, size, 0, sourceColor));
    foreignColorRegions.push_back(
        rasterRegion(foreignColorLabels, size, 1, QColor(40, 90, 190)));
    const gui::RegionLayerPlanVariants foreignColorVariants =
        gui::buildRegionLayerPlanVariants(rasterExtraction(
            foreignColorLabels, size, std::move(foreignColorRegions)));
    const auto sourceUnit = std::find_if(
        foreignColorVariants.dangerous.units.cbegin(),
        foreignColorVariants.dangerous.units.cend(),
        [](const gui::RegionLayerUnit &unit) {
            return unit.sourceRegionIndices.contains(0);
        });
    test->expect(foreignColorVariants.dangerous.morphologicalClosingCount == 0
                     && foreignColorVariants.dangerous.convexSimplificationCount == 0
                     && sourceUnit != foreignColorVariants.dangerous.units.cend()
                     && !sourceUnit->outline.contains(notchCenter),
                 "dangerous geometry should not simplify through another colour");

    std::vector<int> transparentLabels = labelsWithNotch(-1);
    QVector<gui::ExtractedRegion> transparentRegions;
    transparentRegions.push_back(
        rasterRegion(transparentLabels, size, 0, sourceColor));
    const gui::RegionLayerPlanVariants transparentVariants =
        gui::buildRegionLayerPlanVariants(
            rasterExtraction(transparentLabels, size, std::move(transparentRegions)));
    test->expect(transparentVariants.dangerous.morphologicalClosingCount == 0
                     && transparentVariants.dangerous.convexSimplificationCount == 0
                     && transparentVariants.dangerous.units.size() == 1
                     && !transparentVariants.dangerous.units.front().outline.contains(
                         notchCenter),
                 "dangerous geometry should not simplify through transparent background");
}

void completedRegionLayersUsePlannedDrawOrder(TestContext *test)
{
    QVector<gui::RegionFillLayer> layers(5);
    layers[0].drawOrder = 2;
    layers[1].drawOrder = 1;
    layers[1].variant = gui::RegionFillVariant::Dangerous;
    layers[2].drawOrder = 1;
    layers[3].drawOrder = 0;
    layers[3].variant = gui::RegionFillVariant::Dangerous;
    layers[4].drawOrder = 0;
    gui::sortRegionFillLayersByDrawOrder(&layers);

    test->expect(layers[0].drawOrder == 0 && layers[1].drawOrder == 1
                     && layers[2].drawOrder == 2
                     && layers[3].variant == gui::RegionFillVariant::Dangerous
                     && layers[3].drawOrder == 0 && layers[4].drawOrder == 1,
                 "completed parallel layers should return to each variant's draw order");
}

void crossedCoreRetainsValidFits(TestContext *test)
{
    const QVector<gui::PenPrimitive> primitives = penPrimitiveCatalog(test);
    gui::PenFillRequest request;
    request.primitives = primitives;
    request.boundaryTolerance = 0.5;
    request.points = {{{113.734, 0.0}, gui::PenPointKind::Hard},
                      {{32.6528, 11.8846}, gui::PenPointKind::Soft},
                      {{95.7775, 80.3669}, gui::PenPointKind::Hard},
                      {{14.3184, 24.8003}, gui::PenPointKind::Soft},
                      {{21.5951, 122.472}, gui::PenPointKind::Hard},
                      {{-26.0475, 147.723}, gui::PenPointKind::Soft},
                      {{-61.0773, 105.789}, gui::PenPointKind::Hard},
                      {{-34.5291, 28.9734}, gui::PenPointKind::Soft},
                      {{-91.7899, 33.4088}, gui::PenPointKind::Hard},
                      {{-181.514, 0.0}, gui::PenPointKind::Soft},
                      {{-116.816, -42.5177}, gui::PenPointKind::Hard},
                      {{-70.0474, -58.7767}, gui::PenPointKind::Soft},
                      {{-62.194, -107.723}, gui::PenPointKind::Hard},
                      {{-4.40167, -24.9631}, gui::PenPointKind::Soft},
                      {{17.1126, -97.0503}, gui::PenPointKind::Hard},
                      {{75.438, -130.662}, gui::PenPointKind::Soft},
                      {{59.969, -50.3199}, gui::PenPointKind::Hard},
                      {{101.037, -36.7745}, gui::PenPointKind::Soft}};
    test->expect(gui::buildPenContour(request.points).valid(),
                 "the local-repair contour should be a simple Pen path");
    const gui::PenFillResult result = gui::fillPenPath(request);
    if (!result.error.isEmpty()) {
        std::cerr << "Crossed core fill error: "
                  << result.error.toStdString() << '\n';
    }
    test->expect(result.error.isEmpty(),
                 "a crossed generated core should be repaired before meshing");
    test->expect(hasPlacement(result, 109) || hasPlacement(result, 127)
                     || hasPlacement(result, 129) || hasPlacement(result, 130)
                     || hasPlacement(result, 139) || hasPlacement(result, 2123),
                 "local core repair should retain unaffected curve placements");
}

void pointedCurveUsesContainedPrimitive(TestContext *test)
{
    gui::PenFillRequest request;
    request.primitives = penPrimitiveCatalog(test);
    request.boundaryTolerance = 0.5;
    request.points = {{{-50.0, 0.0}, gui::PenPointKind::Hard},
                      {{0.0, -60.0}, gui::PenPointKind::Soft},
                      {{50.0, 0.0}, gui::PenPointKind::Hard},
                      {{150.0, 200.0}, gui::PenPointKind::Hard},
                      {{20.0, 340.0}, gui::PenPointKind::Hard},
                      {{-20.0, 340.0}, gui::PenPointKind::Hard},
                      {{-150.0, 200.0}, gui::PenPointKind::Hard}};
    const gui::PenFillResult result = gui::fillPenPath(request);
    test->expect(result.error.isEmpty(), "a pointed curve region should fill");
    test->expect(hasPlacement(result, 127),
                 "a pointed curve region should select the largest contained Primitive");
    test->expect(result.outsideArea <= result.targetArea * 1e-5,
                 "a pointed curve Primitive should remain within the fill border");
}

void openCenterlineBuildsAndFills(TestContext *test)
{
    const QVector<gui::PenPoint> points = {
        {{-80.0, 0.0}, gui::PenPointKind::Hard},
        {{80.0, 0.0}, gui::PenPointKind::Hard},
    };
    const gui::LiningPath path = gui::buildLiningPath(points);
    test->expect(path.valid(), "a completed open centerline should be valid");
    test->expect(!gui::buildLiningRibbon(path.centerline, 8.0).isEmpty(),
                 "a completed centerline should produce a width ribbon");

    gui::LiningFillRequest request;
    request.points = points;
    request.width = 8.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    test->expect(result.error.isEmpty(), "a straight centerline should fit lining Primitives");
    test->expect(!result.placements.isEmpty(), "a lining fill should generate placements");
    test->expect(result.placements.size() == 1 && hasPlacement(result, 2133),
                 "one straight authored span should use one Pill Primitive");
    test->expect(result.coveredArea >= result.targetArea * 0.2,
                 "a lining fill should cover a substantial part of a straight width ribbon");
    test->expect(std::all_of(result.placements.cbegin(),
                             result.placements.cend(),
                             [](const gui::PenPlacement &placement) {
        return placement.shapeId == 2112 || placement.shapeId == 2131
            || placement.shapeId == 136
            || placement.shapeId == 2133;
    }),
                 "a lining fill should use only the lining catalog");
    test->expect(result.outsideArea <= result.targetArea * 1e-5,
                 "lining placements should remain within the width ribbon");
}

void curvedCenterlineBuildsAndFills(TestContext *test)
{
    gui::LiningFillRequest request;
    request.points = {
        {{-60.0, 0.0}, gui::PenPointKind::Hard},
        {{0.0, -55.0}, gui::PenPointKind::Soft},
        {{60.0, 0.0}, gui::PenPointKind::Hard},
    };
    request.width = 10.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    test->expect(result.error.isEmpty(), "a quadratic centerline should fit lining Primitives");
    test->expect(!result.placements.isEmpty(), "a curved lining fill should generate placements");
    test->expect(result.placements.size() == 1,
                 "one angled authored curve should use one curve Primitive");
    test->expect(std::all_of(result.placements.cbegin(),
                             result.placements.cend(),
                             [](const gui::PenPlacement &placement) {
        return placement.shapeId == 2131;
    }),
                 "an angled curve should use curve-matching Primitives");
    test->expect(result.coveredArea >= result.targetArea * 0.4,
                 "a curved lining fill should retain substantial width coverage");
    test->expect(result.outsideArea <= result.targetArea * 1e-5,
                 "curved lining placements should remain within the width ribbon");
}

void adjacentLiningPlacementsOverlap(TestContext *test)
{
    gui::LiningFillRequest request;
    request.points = {
        {{-150.0, 0.0}, gui::PenPointKind::Hard},
        {{-75.0, -55.0}, gui::PenPointKind::Soft},
        {{0.0, 0.0}, gui::PenPointKind::Hard},
        {{75.0, 55.0}, gui::PenPointKind::Soft},
        {{150.0, 0.0}, gui::PenPointKind::Hard},
    };
    request.width = 10.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    test->expect(result.error.isEmpty(), "a multi-curve centerline should fit lining Primitives");
    test->expect(result.placements.size() == 2,
                 "each authored curve should use one curve Primitive");
    test->expect(hasPlacement(result, 2131),
                 "an angled lining should use a curve-matching Primitive");

    test->expect(placementsConnected(result, request.primitives),
                 "lining Primitive overlaps should form one connected chain");
    const int pointCount = static_cast<int>(request.points.size());
    const int expectedBudget = pointCount + std::max(1, pointCount / 2);
    test->expect(result.placements.size() <= expectedBudget,
                 "lining shape count should remain tied to authored point count");
}

void shallowCurveCanUseComplementaryHairs(TestContext *test)
{
    gui::LiningFillRequest request;
    request.points = {
        {{-100.0, 0.0}, gui::PenPointKind::Hard},
        {{0.0, -20.0}, gui::PenPointKind::Soft},
        {{100.0, 0.0}, gui::PenPointKind::Hard},
    };
    request.width = 10.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    const int hairCount = static_cast<int>(std::count_if(
        result.placements.cbegin(),
        result.placements.cend(),
        [](const gui::PenPlacement &placement) { return placement.shapeId == 2112; }));
    test->expect(result.error.isEmpty(), "a shallow curve should fit lining Primitives");
    test->expect(hairCount == 2,
                 "a single shallow curve should use complementary Hair Primitives");
    if (result.placements.size() == 2) {
        test->expect(result.placements[0].transform != result.placements[1].transform,
                     "complementary Hair placements should use distinct orientations");
        const std::optional<QPointF> firstTip = placedHairTip(result.placements[0],
                                                              request.primitives);
        const std::optional<QPointF> secondTip = placedHairTip(result.placements[1],
                                                               request.primitives);
        const QPointF start = request.points.front().position;
        const QPointF end = request.points.back().position;
        const bool faceAway = firstTip.has_value() && secondTip.has_value()
            && ((QLineF(*firstTip, start).length() < QLineF(*firstTip, end).length()
                 && QLineF(*secondTip, end).length() < QLineF(*secondTip, start).length())
                || (QLineF(*firstTip, end).length() < QLineF(*firstTip, start).length()
                    && QLineF(*secondTip, start).length()
                        < QLineF(*secondTip, end).length()));
        test->expect(faceAway,
                     "complementary Hair tips should face away from each other");
    }
    test->expect(placementsConnected(result, request.primitives),
                 "complementary Hair placements should overlap");
}

void sharpHardJoinUsesOverlappingStrokeShapes(TestContext *test)
{
    gui::LiningFillRequest request;
    request.points = {
        {{-100.0, 40.0}, gui::PenPointKind::Hard},
        {{0.0, -40.0}, gui::PenPointKind::Hard},
        {{100.0, 40.0}, gui::PenPointKind::Hard},
    };
    request.width = 10.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    test->expect(result.error.isEmpty(), "a sharp hard join should fit lining Primitives");
    test->expect(std::all_of(result.placements.cbegin(),
                             result.placements.cend(),
                             [](const gui::PenPlacement &placement) {
        return placement.shapeId == 2112 || placement.shapeId == 2133;
    }),
                 "a sharp hard join should use Hair or Pill Primitives");
    int hairCount = 0;
    bool hairsFaceOutward = true;
    const QPointF join = request.points[1].position;
    for (const gui::PenPlacement &placement : result.placements) {
        const std::optional<QPointF> tip = placedHairTip(placement, request.primitives);
        if (!tip.has_value()) {
            continue;
        }
        ++hairCount;
        const double outerDistance = std::min(QLineF(*tip,
                                                     request.points.front().position).length(),
                                              QLineF(*tip,
                                                     request.points.back().position).length());
        hairsFaceOutward = hairsFaceOutward
            && outerDistance < QLineF(*tip, join).length();
    }
    test->expect(hairCount == 0 || hairsFaceOutward,
                 "Hair tips at a sharp hard join should face outward");
    test->expect(placementsConnected(result, request.primitives),
                 "sharp hard-join placements should overlap");
}

void wideCenterlineCanUsePill(TestContext *test)
{
    gui::LiningFillRequest request;
    request.points = {
        {{-100.0, 0.0}, gui::PenPointKind::Hard},
        {{100.0, 0.0}, gui::PenPointKind::Hard},
    };
    request.width = 32.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    test->expect(result.error.isEmpty(), "a wide centerline should fit lining Primitives");
    test->expect(hasPlacement(result, 2133),
                 "a wide centerline should be able to select the Pill Primitive");
    test->expect(result.placements.size() == 1,
                 "a wide centerline should avoid redundant overlays");
}

void hairOrientationFollowsPathSide(TestContext *test)
{
    const QVector<gui::PenPoint> source = {
        {{-180.0, 0.0}, gui::PenPointKind::Hard},
        {{-135.0, -15.0}, gui::PenPointKind::Soft},
        {{-90.0, 0.0}, gui::PenPointKind::Hard},
        {{90.0, 0.0}, gui::PenPointKind::Hard},
        {{135.0, -15.0}, gui::PenPointKind::Soft},
        {{180.0, 0.0}, gui::PenPointKind::Hard},
    };
    for (const double rotation : {0.0, 67.0}) {
        for (const bool reverse : {false, true}) {
            QTransform transform;
            transform.rotate(rotation);
            QVector<gui::PenPoint> points = source;
            for (gui::PenPoint &point : points) {
                point.position = transform.map(point.position);
            }
            if (reverse) {
                std::reverse(points.begin(), points.end());
            }
            gui::LiningFillRequest request;
            request.points = points;
            request.width = 10.0;
            request.primitives = liningPrimitiveCatalog(test);
            const gui::PenFillResult result = gui::fillLiningPath(request);
            test->expect(result.error.isEmpty(),
                         "rotated mixed lining paths should produce placements");
            const bool primarySequence = result.placements.size() >= 3
                && result.placements[0].shapeId == 2112
                && result.placements[1].shapeId == 2133
                && result.placements[2].shapeId == 2112;
            test->expect(primarySequence,
                         "mixed lining paths should retain their Hair-Pill-Hair sequence");
            if (!primarySequence) {
                continue;
            }
            const QPointF pathChord = points.back().position - points.front().position;
            double pathSide = 0.0;
            for (int index = 1; index + 1 < points.size(); ++index) {
                const QPointF delta = points[index].position - points.front().position;
                pathSide += pathChord.x() * delta.y() - pathChord.y() * delta.x();
            }
            const auto hair = std::find_if(request.primitives.cbegin(),
                                           request.primitives.cend(),
                                           [](const gui::PenPrimitive &primitive) {
                return primitive.shapeId == 2112;
            });
            const std::optional<PrimitiveEnds> ends = hair != request.primitives.cend()
                ? primitiveEnds(*hair)
                : std::nullopt;
            const QPointF desiredDirection(-pathChord.y() * pathSide,
                                           pathChord.x() * pathSide);
            const QPointF firstDirection = ends.has_value()
                ? result.placements[0].transform.map(ends->tip)
                    - result.placements[0].transform.map(ends->base)
                : QPointF();
            const QPointF secondDirection = ends.has_value()
                ? result.placements[2].transform.map(ends->tip)
                    - result.placements[2].transform.map(ends->base)
                : QPointF();
            test->expect(ends.has_value()
                             && QPointF::dotProduct(firstDirection, desiredDirection) > 0.0
                             && QPointF::dotProduct(secondDirection, desiredDirection) > 0.0,
                         "every rotated Hair should retain one path-wide direction");
        }
    }
}

void longSoftRunUsesOverlappingArcs(TestContext *test)
{
    gui::LiningFillRequest request;
    request.points = {
        {{-218.1588, 456.7003}, gui::PenPointKind::Hard},
        {{-169.7437, 502.7537}, gui::PenPointKind::Soft},
        {{-100.4668, 548.8070}, gui::PenPointKind::Soft},
        {{-54.4135, 569.6688}, gui::PenPointKind::Soft},
        {{-13.0835, 583.8391}, gui::PenPointKind::Soft},
        {{49.5018, 594.8604}, gui::PenPointKind::Hard},
    };
    request.width = 4.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    test->expect(result.error.isEmpty(), "a long soft run should produce placements");
    test->expect(result.placements.size() == 4,
                 "a four-span soft run should use four placements");
    test->expect(std::all_of(result.placements.cbegin(),
                             result.placements.cend(),
                             [](const gui::PenPlacement &placement) {
        return placement.shapeId == 136;
    }),
                 "a long soft run should use Arc placements");
    test->expect(placementsConnected(result, request.primitives),
                 "Arc placements in a long soft run should overlap");
}

void denseLiningPathKeepsHairDirectionAndCurve(TestContext *test)
{
    gui::LiningFillRequest request;
    request.points = {
        {{-215.1029, 457.9713}, gui::PenPointKind::Hard},
        {{-232.8158, 454.8224}, gui::PenPointKind::Hard},
        {{-213.9221, 472.1416}, gui::PenPointKind::Hard},
        {{-233.9966, 465.4501}, gui::PenPointKind::Hard},
        {{-207.2306, 500.4821}, gui::PenPointKind::Hard},
        {{-220.2200, 495.7587}, gui::PenPointKind::Hard},
        {{-204.0816, 511.5034}, gui::PenPointKind::Hard},
        {{-221.0072, 512.6843}, gui::PenPointKind::Soft},
        {{-244.2307, 502.8438}, gui::PenPointKind::Hard},
        {{-222.5817, 524.4928}, gui::PenPointKind::Soft},
        {{-201.3263, 531.1844}, gui::PenPointKind::Hard},
        {{-224.9434, 539.4503}, gui::PenPointKind::Hard},
        {{-209.5923, 552.8334}, gui::PenPointKind::Soft},
        {{-189.5177, 549.2908}, gui::PenPointKind::Hard},
        {{-206.4433, 561.8866}, gui::PenPointKind::Soft},
        {{-216.6774, 582.7484}, gui::PenPointKind::Hard},
        {{-197.3901, 577.6313}, gui::PenPointKind::Hard},
        {{-202.1135, 589.8335}, gui::PenPointKind::Hard},
        {{-191.8794, 585.8973}, gui::PenPointKind::Hard},
        {{-205.6561, 622.5038}, gui::PenPointKind::Soft},
        {{-208.4114, 664.2274}, gui::PenPointKind::Hard},
        {{-175.7411, 631.9507}, gui::PenPointKind::Hard},
        {{-177.7092, 642.5784}, gui::PenPointKind::Soft},
        {{-172.5921, 652.0252}, gui::PenPointKind::Hard},
        {{-165.9006, 637.4613}, gui::PenPointKind::Hard},
        {{-163.9325, 675.6423}, gui::PenPointKind::Soft},
        {{-147.0069, 705.1637}, gui::PenPointKind::Hard},
        {{-128.9005, 648.4827}, gui::PenPointKind::Hard},
        {{-122.6026, 668.1636}, gui::PenPointKind::Soft},
        {{-112.7621, 681.9402}, gui::PenPointKind::Hard},
        {{-109.6132, 668.5572}, gui::PenPointKind::Soft},
        {{-98.1982, 660.6848}, gui::PenPointKind::Hard},
        {{-96.2301, 665.8019}, gui::PenPointKind::Hard},
        {{-90.7195, 657.9295}, gui::PenPointKind::Hard},
        {{-87.1769, 675.2487}, gui::PenPointKind::Hard},
        {{-83.6344, 665.0146}, gui::PenPointKind::Soft},
        {{-75.3684, 659.1103}, gui::PenPointKind::Hard},
    };
    request.width = 4.0;
    request.primitives = liningPrimitiveCatalog(test);
    const gui::PenFillResult result = gui::fillLiningPath(request);
    const gui::LiningPath path = gui::buildLiningPath(request.points);
    test->expect(result.error.isEmpty(), "a dense lining path should produce placements");
    test->expect(result.placements.size() >= path.segments.size(),
                 "a dense lining path should retain its primary placements");
    if (!result.error.isEmpty() || result.placements.size() < path.segments.size()) {
        return;
    }
    const auto hair = std::find_if(request.primitives.cbegin(),
                                   request.primitives.cend(),
                                   [](const gui::PenPrimitive &primitive) {
        return primitive.shapeId == 2112;
    });
    const std::optional<PrimitiveEnds> ends = hair != request.primitives.cend()
        ? primitiveEnds(*hair)
        : std::nullopt;
    int hairCount = 0;
    bool directionMatches = ends.has_value();
    bool curveMatches = ends.has_value();
    const QPointF pathChord = request.points.back().position
        - request.points.front().position;
    double pathSide = 0.0;
    for (int index = 1; index + 1 < request.points.size(); ++index) {
        pathSide += pathChord.x()
                * (request.points[index].position.y() - request.points.front().position.y())
            - pathChord.y()
                * (request.points[index].position.x() - request.points.front().position.x());
    }
    const QPointF desiredDirection(-pathChord.y() * pathSide,
                                   pathChord.x() * pathSide);
    for (int index = 0; index < path.segments.size(); ++index) {
        const gui::PenPlacement &placement = result.placements[index];
        if (placement.shapeId != 2112 || !ends.has_value()) {
            continue;
        }
        ++hairCount;
        const gui::PenBoundarySegment &segment = path.segments[index];
        const QPointF tip = placement.transform.map(ends->tip);
        const QPointF base = placement.transform.map(ends->base);
        directionMatches = directionMatches
            && QPointF::dotProduct(tip - base, desiredDirection) > 0.0;
        if (segment.curved) {
            const QPointF targetMiddle = segment.start * 0.25
                + segment.control * 0.5 + segment.end * 0.25;
            const QPointF placedMiddle = placement.transform.map(ends->middle);
            curveMatches = curveMatches
                && QLineF(placedMiddle, targetMiddle).length() <= request.width;
        }
    }
    test->expect(hairCount >= 2, "a dense lining path should exercise multiple Hairs");
    test->expect(directionMatches, "dense lining Hairs should retain one direction");
    test->expect(curveMatches, "dense lining Hairs should retain their fitted curves");
}

int compareLoggedPen(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "Could not open Pen fill log\n";
        return 2;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        std::cerr << "Could not parse Pen fill log: "
                  << parseError.errorString().toStdString() << '\n';
        return 2;
    }
    const QJsonObject root = document.object();
    const QJsonObject loggedRequest = root.value(QStringLiteral("request")).toObject();
    gui::PenFillRequest request;
    request.boundaryTolerance =
        loggedRequest.value(QStringLiteral("boundaryTolerance")).toDouble(0.1);
    for (const QJsonValue &value :
         loggedRequest.value(QStringLiteral("points")).toArray()) {
        const QJsonObject pointObject = value.toObject();
        const QJsonArray position =
            pointObject.value(QStringLiteral("position")).toArray();
        if (position.size() != 2) {
            std::cerr << "Pen fill log contains an invalid point\n";
            return 2;
        }
        const gui::PenPointKind kind =
            pointObject.value(QStringLiteral("kind")).toString()
                    == QStringLiteral("hard")
            ? gui::PenPointKind::Hard
            : gui::PenPointKind::Soft;
        request.points.push_back(
            {{position[0].toDouble(), position[1].toDouble()}, kind});
    }
    gui::ShapeGeometryStore geometry;
    QString geometryError;
    if (!geometry.loadDefault(&geometryError)) {
        std::cerr << "Could not load Primitive geometry: "
                  << geometryError.toStdString() << '\n';
        return 2;
    }
    request.primitives = gui::buildPenPrimitiveCatalog(geometry);
    request.discardNegligiblePlacements = false;
    QElapsedTimer timer;
    timer.start();
    const gui::PenFillResult baseline = gui::fillPenPath(request);
    const qint64 baselineMilliseconds = timer.elapsed();
    request.discardNegligiblePlacements = true;
    timer.restart();
    const gui::PenFillResult optimized = gui::fillPenPath(request);
    const qint64 optimizedMilliseconds = timer.elapsed();
    if (!baseline.error.isEmpty() || !optimized.error.isEmpty()) {
        std::cerr << "Pen fill comparison failed: baseline="
                  << baseline.error.toStdString()
                  << " optimized=" << optimized.error.toStdString() << '\n';
        return 1;
    }
    const double comparisonThreshold = baseline.targetArea * 1e-3;
    const int eligiblePlacements = std::count_if(
        baseline.placements.cbegin(), baseline.placements.cend(),
        [&](const gui::PenPlacement &placement) {
            return placement.area <= comparisonThreshold;
        });
    const auto minimumPlacement = std::min_element(
        baseline.placements.cbegin(), baseline.placements.cend(),
        [](const gui::PenPlacement &left, const gui::PenPlacement &right) {
            return left.area < right.area;
        });
    const QJsonObject loggedResult =
        root.value(QStringLiteral("result")).toObject();
    std::cout << "points=" << request.points.size()
              << " logged_strategy="
              << loggedRequest.value(QStringLiteral("strategy"))
                     .toString().toStdString()
              << " logged_shapes="
              << loggedResult.value(QStringLiteral("shapeCount")).toInt()
              << " unpruned_shapes=" << baseline.placements.size()
              << " pruned_shapes=" << optimized.placements.size()
              << " eligible_shapes=" << eligiblePlacements
              << " minimum_shape_area="
              << (minimumPlacement != baseline.placements.cend()
                      ? minimumPlacement->area : 0.0)
              << " unpruned_missing="
              << baseline.targetArea - baseline.coveredArea
              << " pruned_missing="
              << optimized.targetArea - optimized.coveredArea
              << " unpruned_residual=" << filledPathArea(baseline.unfilled)
              << " pruned_residual=" << filledPathArea(optimized.unfilled)
              << " unpruned_outside=" << baseline.outsideArea
              << " pruned_outside=" << optimized.outsideArea
              << " unpruned_ms=" << baselineMilliseconds
              << " pruned_ms=" << optimizedMilliseconds << '\n';
    return 0;
}

int checkLoggedRegion(const QString &path, const QSize &imageSize)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "Could not open region point log\n";
        return 2;
    }
    QPainterPath outline;
    bool inSourcePath = false;
    int curveStage = 0;
    QPointF control1;
    QPointF control2;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line == QStringLiteral("[source_qpainter_path]")) {
            inSourcePath = true;
            continue;
        }
        if (inSourcePath && line.startsWith(QLatin1Char('['))) {
            break;
        }
        if (!inSourcePath || line.isEmpty() || line.startsWith(QStringLiteral("count="))
            || line.startsWith(QStringLiteral("index,"))) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char(','));
        if (fields.size() != 4) {
            continue;
        }
        bool xOk = false;
        bool yOk = false;
        const QPointF point(fields[2].toDouble(&xOk), fields[3].toDouble(&yOk));
        if (!xOk || !yOk) {
            continue;
        }
        const QString &type = fields[1];
        if (type == QStringLiteral("move")) {
            outline.moveTo(point);
            curveStage = 0;
        } else if (type == QStringLiteral("line")) {
            outline.lineTo(point);
            curveStage = 0;
        } else if (type == QStringLiteral("curve-control-1")) {
            control1 = point;
            curveStage = 1;
        } else if (type == QStringLiteral("curve-data") && curveStage == 1) {
            control2 = point;
            curveStage = 2;
        } else if (type == QStringLiteral("curve-data") && curveStage == 2) {
            outline.cubicTo(control1, control2, point);
            curveStage = 0;
        }
    }
    gui::RegionPenConversionOptions options;
    options.comparisonImageSize = imageSize;
    QElapsedTimer timer;
    timer.start();
    const gui::RegionPenConversionResult result =
        gui::regionOutlineToPenPoints(outline, options);
    std::cout << "valid=" << (result.valid() ? "yes" : "no")
              << " original=" << result.originalPointCount
              << " optimized=" << result.points.size()
              << " removed_hard=" << result.removedHardPoints
              << " removed_soft=" << result.removedSoftPoints
              << " dssim=" << result.dssim
              << " elapsed_ms=" << timer.elapsed() << '\n';
    if (!result.error.isEmpty()) {
        std::cerr << result.error.toStdString() << '\n';
    }
    if (!result.valid()) {
        return 1;
    }
    constexpr double kDangerousRdpEpsilon = 1.9;
    constexpr int kDangerousRdpCurveSamples = 32;
    const QPolygonF dangerousSource =
        gui::regionOuterContour(outline, kDangerousRdpCurveSamples);
    const QPolygonF dangerousContour =
        gui::simplifyClosedPolygonCyclic(dangerousSource, kDangerousRdpEpsilon);
    std::cout << "dangerous_rdp_input=" << dangerousSource.size()
              << " dangerous_rdp_output=" << dangerousContour.size()
              << " polygon_valid="
              << (gui::buildPolygonContour(dangerousContour).valid() ? "yes" : "no")
              << '\n';

    gui::ShapeGeometryStore geometry;
    QString geometryError;
    if (!geometry.loadDefault(&geometryError)) {
        std::cerr << geometryError.toStdString() << '\n';
        return 2;
    }
    const QVector<gui::PenPrimitive> primitives = gui::buildPenPrimitiveCatalog(geometry);
    const gui::PolygonMeshSources meshSources = gui::buildPolygonMeshSources(geometry);
    QElapsedTimer dangerousMeshTimer;
    dangerousMeshTimer.start();
    const gui::PenFillResult dangerousMesh =
        gui::fillPolygonMesh(dangerousContour, meshSources);
    std::cout << "dangerous_mesh_placements=" << dangerousMesh.placements.size()
              << " dangerous_mesh_elapsed_ms=" << dangerousMeshTimer.elapsed();
    if (!dangerousMesh.error.isEmpty()) {
        std::cout << " error=" << dangerousMesh.error.toStdString();
    }
    std::cout << '\n';
    if (!dangerousMesh.error.isEmpty() || dangerousMesh.placements.isEmpty()) {
        return 1;
    }
    QElapsedTimer fillTimer;
    fillTimer.start();
    const auto timedOut = [&fillTimer]() { return fillTimer.elapsed() > 3000; };
    gui::RegionFillContourStats stats;
    QPolygonF optimizedContour;
    const gui::PenFillResult fill = gui::fillRegionOutline(outline,
                                                            primitives,
                                                            4.224,
                                                            timedOut,
                                                            &optimizedContour,
                                                            &stats,
                                                            nullptr,
                                                            imageSize);
    std::cout << "fill_cancelled=" << (fill.cancelled ? "yes" : "no")
              << " placements=" << fill.placements.size()
              << " optimized=" << stats.optimizedPointCount
              << " fill_elapsed_ms=" << fillTimer.elapsed();
    if (!fill.error.isEmpty()) {
        std::cout << " error=" << fill.error.toStdString();
    }
    std::cout << '\n';

    QPolygonF simplifiedMesh = gui::simplifyClosedPolygon(optimizedContour, 0.45);
    if (!gui::buildPolygonContour(simplifiedMesh).valid()) {
        simplifiedMesh = optimizedContour;
    }
    QElapsedTimer meshTimer;
    meshTimer.start();
    const gui::PenFillResult mesh =
        gui::fillPolygonMesh(simplifiedMesh, meshSources);
    std::cout << "fallback_points=" << optimizedContour.size()
              << " simplified_points=" << simplifiedMesh.size()
              << " mesh_placements=" << mesh.placements.size()
              << " mesh_elapsed_ms=" << meshTimer.elapsed();
    if (!mesh.error.isEmpty()) {
        std::cout << " error=" << mesh.error.toStdString();
    }
    std::cout << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 3
        && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--compare-pen-log")) {
        return compareLoggedPen(QString::fromLocal8Bit(argv[2]));
    }
    if (argc == 5 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--check-region-log")) {
        bool widthOk = false;
        bool heightOk = false;
        const int width = QString::fromLocal8Bit(argv[3]).toInt(&widthOk);
        const int height = QString::fromLocal8Bit(argv[4]).toInt(&heightOk);
        if (!widthOk || !heightOk || width <= 0 || height <= 0) {
            std::cerr << "Invalid region image dimensions\n";
            return 2;
        }
        return checkLoggedRegion(QString::fromLocal8Bit(argv[2]), QSize(width, height));
    }
    TestContext test;
    advancingFrontFillsSimpleEllipse(&test);
    advancingFrontHonorsSafeShapeCeiling(&test);
    advancingFrontPreflightRejectsUnprofitableSearch(&test);
    alternatingCurvatureMerges(&test);
    sharpLineCornersStayHard(&test);
    excessiveDisplacementDoesNotMerge(&test);
    negligibleSoftRunBecomesLine(&test);
    visibleSoftRunRemainsCurved(&test);
    rasterDssimGuardsSoftRunReduction(&test);
    containedHoleIsIgnored(&test);
    invalidContoursAreRejected(&test);
    conversionIsDeterministic(&test);
    conversionOptimizationIsBounded(&test);
    rasterDssimGuardsCurveSimplification(&test);
    denseValidPolygonTriangulates(&test);
    ellipseLikeCoreUsesOneCircle(&test);
    arcPrimitivesFillCurvedBoundaries(&test);
    concaveCoreFallbackStaysContained(&test);
    negligibleCorePlacementsAreDiscarded(&test);
    automaticRegionFillRetainsCurvedBoundary(&test);
    smallRegionMergesIntoClosestColorNeighbor(&test);
    lineartDiagnosticCapturesComponentTopology(&test);
    blendWithoutLineVotesRemainsAColorRegion(&test);
    residualLineHistoryMergesMissedComponent(&test);
    regionLayerPlanAbsorbsBehindEnclosedRegion(&test);
    regionLayerPlanMergesAdjacentExactColors(&test);
    regionLayerPlanBridgesNearbyExactColors(&test);
    regionLayerPlanDoesNotBridgeBackground(&test);
    regionLayerPlanUsesBaselineOverlapOrder(&test);
    regionLayerPlanRejectsOnlyCyclicNearbyBridge(&test);
    regionLayerPlanRetainsUnrelatedSafeBridge(&test);
    regionLayerPlanRejectsComplexForeignBridge(&test);
    regionLayerPlanSuppressesResidualCycleMerge(&test);
    regionLayerPlanSuppressesValidationMismatchMerge(&test);
    dangerousRegionLayerPlanBuildsHierarchicalContourBridge(&test);
    dangerousRegionLayerPlanDoesNotCrossColor(&test);
    dangerousRegionLayerPlanRequiresStraightEdgeVisibility(&test);
    dangerousRegionLayerPlanUsesMultipleStraightAttachments(&test);
    dangerousRegionLayerPlanAbsorbsLargeContainedRegion(&test);
    dangerousGeometryUsesOnlySameColorOrLineart(&test);
    completedRegionLayersUsePlannedDrawOrder(&test);
    crossedCoreRetainsValidFits(&test);
    pointedCurveUsesContainedPrimitive(&test);
    openCenterlineBuildsAndFills(&test);
    curvedCenterlineBuildsAndFills(&test);
    adjacentLiningPlacementsOverlap(&test);
    shallowCurveCanUseComplementaryHairs(&test);
    sharpHardJoinUsesOverlappingStrokeShapes(&test);
    wideCenterlineCanUsePill(&test);
    hairOrientationFollowsPathSide(&test);
    denseLiningPathKeepsHairDirectionAndCurve(&test);
    longSoftRunUsesOverlappingArcs(&test);
    bucketFloodIsContiguousAndToleranceBounded(&test);
    bucketMaskTracesIntoPenContour(&test);
    rdpHybridQuadraticMatchesAnalyzer(&test);
    if (test.failures() == 0) {
        std::cout << "All region Pen conversion tests passed\n";
    }
    return test.failures() == 0 ? 0 : 1;
}
