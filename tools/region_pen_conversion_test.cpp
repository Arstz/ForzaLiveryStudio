#include "bucket_fill.h"
#include "region_extract.h"
#include "region_fill.h"

#include <QtCore>
#include <QtGui>

#include <algorithm>
#include <cmath>
#include <iostream>

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
    crossedCoreRetainsValidFits(&test);
    pointedCurveUsesContainedPrimitive(&test);
    bucketFloodIsContiguousAndToleranceBounded(&test);
    bucketMaskTracesIntoPenContour(&test);
    if (test.failures() == 0) {
        std::cout << "All region Pen conversion tests passed\n";
    }
    return test.failures() == 0 ? 0 : 1;
}
