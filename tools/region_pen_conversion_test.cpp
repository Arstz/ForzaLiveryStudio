#include "bucket_fill.h"
#include "lining_fill.h"
#include "region_extract.h"
#include "region_fill.h"

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
                     <= result.baselineDeviation + 0.5 + 1e-6,
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
    const gui::RegionPenConversionResult conversion =
        gui::regionOutlineToPenPoints(traced);
    test->expect(conversion.valid(),
                 "the traced bucket mask should become a valid optimized Pen contour");
    test->expect(gui::buildPenContour(conversion.points).valid(),
                 "the end-to-end bucket result should be directly consumable by Pen");
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

    const gui::RegionPenConversionResult overLimit =
        gui::regionOutlineToPenPoints(smoothCircularPath(65));
    test->expect(overLimit.valid(), "a contour over the optimization limit should convert");
    test->expect(overLimit.optimizationSkipped && overLimit.removedHardPoints == 0,
                 "a contour over the optimization limit should retain its baseline points");
    test->expect(overLimit.points.size() == overLimit.originalPointCount,
                 "the optimization cutoff should not discard baseline points");
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestContext test;
    alternatingCurvatureMerges(&test);
    sharpLineCornersStayHard(&test);
    excessiveDisplacementDoesNotMerge(&test);
    containedHoleIsIgnored(&test);
    invalidContoursAreRejected(&test);
    conversionIsDeterministic(&test);
    conversionOptimizationIsBounded(&test);
    arcPrimitivesFillCurvedBoundaries(&test);
    automaticRegionFillRetainsCurvedBoundary(&test);
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
    if (test.failures() == 0) {
        std::cout << "All region Pen conversion tests passed\n";
    }
    return test.failures() == 0 ? 0 : 1;
}
