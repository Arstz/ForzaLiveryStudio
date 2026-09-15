#include "image_import_fill.h"
#include "svg_vector_objects.h"

#include <QtCore>
#include <QtGui>
#include <QtSvg/QSvgRenderer>

#define _USE_MATH_DEFINES
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

QVector<gui::PenPrimitive> penPrimitiveCatalog(TestContext *test)
{
    gui::ShapeGeometryStore geometry;
    QString error;
    test->expect(geometry.loadDefault(&error), "Pen Primitive geometry should load");
    return gui::buildPenPrimitiveCatalog(geometry);
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

QPainterPath placementCoverage(const QVector<gui::PenPlacement> &placements,
                               const QVector<gui::PenPrimitive> &primitives)
{
    QPainterPath coverage;
    coverage.setFillRule(Qt::WindingFill);
    for (const gui::PenPlacement &placement : placements) {
        const auto primitive = std::find_if(
            primitives.cbegin(), primitives.cend(),
            [&](const gui::PenPrimitive &candidate) {
                return candidate.shapeId == placement.shapeId;
            });
        if (primitive != primitives.cend()) {
            coverage = coverage.united(placement.transform.map(primitive->silhouette));
        }
    }
    return coverage;
}

double coverageFraction(const QPainterPath &coverage, const QPainterPath &target)
{
    const double targetArea = filledPathArea(target);
    return targetArea > 0.0 ? filledPathArea(coverage.intersected(target)) / targetArea : 0.0;
}

gui::ImageImportFillRequest requestForSvg(TestContext *test,
                                          const QByteArray &svg,
                                          const QSize &size,
                                          const QVector<gui::PenPrimitive> &primitives)
{
    const gui::SvgVectorDocument document = gui::extractSvgVectorObjects(svg, size);
    test->expect(document.supportsObjectSelection(),
                 "SVG fixture should support vector object selection");
    gui::ImageImportFillRequest request;
    request.units = gui::svgImageImportUnits(document);
    request.primitives = primitives;
    return request;
}

void reportUnit(const char *label, const gui::ImageImportFillResult &result)
{
    if (result.units.isEmpty()) {
        return;
    }
    const gui::ImageImportFilledUnit &unit = result.units.front();
    std::cerr << "  " << label << ": via=" << unit.via.toStdString()
              << " components=" << unit.componentCount
              << " failedComponents=" << unit.failedComponentCount
              << " loops=" << unit.loopCount
              << " placements=" << unit.placements.size()
              << " error=" << unit.error.toStdString() << '\n';
}

void rectangleFillsCompletely(TestContext *test, const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"80\" "
        "viewBox=\"0 0 120 80\"><rect x=\"10\" y=\"10\" width=\"100\" height=\"60\" "
        "fill=\"#3366cc\"/></svg>");
    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(120, 80), primitives);
    test->expect(request.units.size() == 1, "rectangle SVG should yield one unit");
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    test->expect(result.error.isEmpty(), "rectangle import should not report an error");
    test->expect(result.filledCount == 1 && result.failedCount == 0,
                 "rectangle unit should fill");
    if (result.units.isEmpty() || !result.units.front().filled()) {
        reportUnit("rectangle", result);
        return;
    }
    const gui::ImageImportFilledUnit &unit = result.units.front();
    test->expect(unit.color == QColor(QStringLiteral("#3366cc")),
                 "rectangle unit should keep its authored color");
    test->expect(unit.componentCount == 1 && unit.loopCount == 1,
                 "rectangle should produce one component with one outer loop");
    const QPainterPath target = request.units.front().outline;
    const QPainterPath coverage = placementCoverage(unit.placements, primitives);
    const double outsideArea = filledPathArea(coverage.subtracted(target));
    test->expect(coverageFraction(coverage, target) >= 0.95,
                 "rectangle placements should cover at least 95% of the target");
    test->expect(outsideArea / filledPathArea(target) <= 0.05,
                 "rectangle placements should not spill far outside the target");
}

void ringPreservesItsHole(TestContext *test, const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" "
        "viewBox=\"0 0 100 100\"><path fill=\"#22aa44\" fill-rule=\"evenodd\" "
        "d=\"M5 50 C5 20 20 5 50 5 C80 5 95 20 95 50 "
        "C95 80 80 95 50 95 C20 95 5 80 5 50 Z "
        "M35 35 H65 V65 H35 Z\"/></svg>");
    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(100, 100), primitives);
    test->expect(request.units.size() == 1, "ring SVG should yield one unit");
    if (request.units.isEmpty()) {
        return;
    }
    const gui::ImageImportUnitLoops loops = gui::imageImportUnitLoops(request.units.front().outline);
    test->expect(loops.valid() && loops.components.size() == 1 && loops.components.front().size() == 2,
                 "ring outline should convert to one component with an outer loop and one cutout");
    test->expect(loops.via == QStringLiteral("authored-curves"),
                 "clean authored cubics should convert without resampling");
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    test->expect(result.filledCount == 1, "ring unit should fill");
    if (result.units.isEmpty() || !result.units.front().filled()) {
        reportUnit("ring", result);
        return;
    }
    QPainterPath hole;
    hole.addRect(QRectF(35.0, 35.0, 30.0, 30.0));
    const QPainterPath coverage = placementCoverage(result.units.front().placements, primitives);
    test->expect(coverageFraction(coverage, hole) <= 0.05,
                 "ring placements should leave the interior cutout empty");
}

void islandsInOnePathFillSeparately(TestContext *test,
                                    const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"60\" "
        "viewBox=\"0 0 120 60\"><path fill=\"#aa2244\" "
        "d=\"M10 10 H50 V50 H10 Z M70 10 H110 V50 H70 Z\"/></svg>");
    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(120, 60), primitives);
    test->expect(request.units.size() == 1, "two-island path should still be one object");
    if (request.units.isEmpty()) {
        return;
    }
    const QVector<QPainterPath> components =
        gui::imageImportOutlineComponents(request.units.front().outline);
    test->expect(components.size() == 2, "two disjoint squares should split into two components");
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    test->expect(result.filledCount == 1 && result.partialCount == 0,
                 "both islands of the object should fill");
    if (result.units.isEmpty() || !result.units.front().filled()) {
        reportUnit("islands", result);
        return;
    }
    QPainterPath left;
    left.addRect(QRectF(10.0, 10.0, 40.0, 40.0));
    QPainterPath right;
    right.addRect(QRectF(70.0, 10.0, 40.0, 40.0));
    const QPainterPath coverage = placementCoverage(result.units.front().placements, primitives);
    test->expect(coverageFraction(coverage, left) >= 0.95 && coverageFraction(coverage, right) >= 0.95,
                 "placements should cover both islands");
}

void islandInsideHoleBecomesItsOwnComponent(TestContext *test,
                                            const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" "
        "viewBox=\"0 0 100 100\"><path fill=\"#112233\" fill-rule=\"evenodd\" "
        "d=\"M5 5 H95 V95 H5 Z M25 25 H75 V75 H25 Z M40 40 H60 V60 H40 Z\"/></svg>");
    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(100, 100), primitives);
    test->expect(request.units.size() == 1, "nested path should be one object");
    if (request.units.isEmpty()) {
        return;
    }
    const gui::ImageImportUnitLoops loops = gui::imageImportUnitLoops(request.units.front().outline);
    test->expect(loops.valid() && loops.components.size() == 2,
                 "frame plus centre dot should split into two components");
    if (loops.components.size() == 2) {
        const int outerLoops = loops.components[0].size() + loops.components[1].size();
        test->expect(outerLoops == 3, "frame keeps its cutout and the dot has no cutout");
    }
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    test->expect(result.filledCount == 1 && result.partialCount == 0,
                 "frame and centre dot should both fill");
    if (result.units.isEmpty() || !result.units.front().filled()) {
        reportUnit("nested", result);
        return;
    }
    QPainterPath gap;
    gap.addRect(QRectF(27.0, 27.0, 46.0, 11.0));
    QPainterPath dot;
    dot.addRect(QRectF(40.0, 40.0, 20.0, 20.0));
    const QPainterPath coverage = placementCoverage(result.units.front().placements, primitives);
    test->expect(coverageFraction(coverage, gap) <= 0.05,
                 "the ring gap between frame and dot should stay empty");
    test->expect(coverageFraction(coverage, dot) >= 0.9,
                 "the centre dot should be covered");
}

void multipleObjectsKeepOrderAndColor(TestContext *test,
                                      const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\" "
        "viewBox=\"0 0 200 100\">"
        "<rect x=\"5\" y=\"5\" width=\"60\" height=\"60\" fill=\"#ff0000\"/>"
        "<circle cx=\"110\" cy=\"50\" r=\"30\" fill=\"#00ff00\"/>"
        "<rect x=\"150\" y=\"20\" width=\"40\" height=\"70\" fill=\"#0000ff\" opacity=\"0.5\"/>"
        "</svg>");
    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(200, 100), primitives);
    test->expect(request.units.size() == 3, "three-object SVG should yield three units");
    if (request.units.size() != 3) {
        return;
    }
    test->expect(request.units[0].color == QColor(255, 0, 0)
                     && request.units[1].color == QColor(0, 255, 0)
                     && request.units[2].color.rgb() == QColor(0, 0, 255).rgb(),
                 "units should keep document order and authored colors");
    test->expect(request.units[2].color.alpha() < 255,
                 "object opacity should fold into the unit color alpha");
    int progressCalls = 0;
    int lastTotal = 0;
    const gui::ImageImportFillResult result = gui::computeImageImportFills(
        request, [&](int, int total) {
            ++progressCalls;
            lastTotal = total;
        });
    test->expect(result.filledCount == 3 && result.failedCount == 0,
                 "all three objects should fill");
    test->expect(result.placementCount > 0 && !result.summary.isEmpty(),
                 "batch result should report placements and a summary");
    test->expect(progressCalls >= 3 && lastTotal == 3,
                 "progress should be reported per unit against the unit total");
}

void strokeOnlyObjectFillsAsOutline(TestContext *test,
                                    const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" "
        "viewBox=\"0 0 100 100\"><rect x=\"20\" y=\"20\" width=\"60\" height=\"60\" "
        "fill=\"none\" stroke=\"#000000\" stroke-width=\"8\"/></svg>");
    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(100, 100), primitives);
    test->expect(request.units.size() == 1, "stroked rectangle should yield one unit");
    if (request.units.isEmpty()) {
        return;
    }
    const gui::ImageImportUnitLoops loops = gui::imageImportUnitLoops(request.units.front().outline);
    test->expect(loops.valid() && loops.components.size() == 1 && loops.components.front().size() == 2,
                 "stroke outline should become one component with one cutout");
    test->expect(loops.via == QStringLiteral("simplified-rdp"),
                 "self-overlapping stroke outline should convert through the simplified path");
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    test->expect(result.filledCount == 1, "stroke outline should fill");
    if (result.units.isEmpty() || !result.units.front().filled()) {
        reportUnit("stroke", result);
        return;
    }
    QPainterPath interior;
    interior.addRect(QRectF(26.0, 26.0, 48.0, 48.0));
    const QPainterPath coverage = placementCoverage(result.units.front().placements, primitives);
    test->expect(coverageFraction(coverage, interior) <= 0.05,
                 "stroke placements should leave the unstroked interior empty");
}

void thinRingsStayRound(TestContext *test, const QVector<gui::PenPrimitive> &primitives)
{
    for (const int strokeWidth : {4, 16}) {
        const QByteArray svg = QByteArrayLiteral(
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"200\">"
            "<circle cx=\"100\" cy=\"100\" r=\"80\" fill=\"none\" stroke=\"#000\" stroke-width=\"")
            + QByteArray::number(strokeWidth) + QByteArrayLiteral("\"/></svg>");
        const gui::ImageImportFillRequest request =
            requestForSvg(test, svg, QSize(200, 200), primitives);
        const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
        test->expect(result.filledCount == 1, "a stroked circle should fill");
        if (result.units.isEmpty() || !result.units.front().filled()) {
            reportUnit("ring", result);
            continue;
        }
        const QPainterPath target = request.units.front().outline;
        const QPainterPath coverage =
            placementCoverage(result.units.front().placements, primitives);
        const double targetArea = filledPathArea(target);
        const double spill = filledPathArea(coverage.subtracted(target)) / targetArea;
        test->expect(spill <= 0.05,
                     "ring placements should not extend beyond the stroked band");
        const double minimumCoverage = strokeWidth >= 16 ? 0.85 : 0.55;
        const double covered = coverageFraction(coverage, target);
        test->expect(covered >= minimumCoverage,
                     "ring placements should cover most of the stroked band");
        std::cout << "  ring stroke=" << strokeWidth << " placements="
                  << result.units.front().placements.size() << " coverage=" << covered
                  << " spill=" << spill << '\n';
        QPainterPath hole;
        hole.addEllipse(QPointF(100.0, 100.0), 80.0 - strokeWidth * 0.5 - 1.0,
                        80.0 - strokeWidth * 0.5 - 1.0);
        test->expect(coverageFraction(coverage, hole) <= 0.02,
                     "ring placements should leave the inside of the ring empty");
    }
}

void textObjectsProduceGlyphUnits(TestContext *test,
                                  const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"80\" "
        "viewBox=\"0 0 200 80\"><text x=\"10\" y=\"60\" font-size=\"48\" "
        "fill=\"#202020\">OK</text></svg>");
    const gui::SvgVectorDocument document = gui::extractSvgVectorObjects(svg, QSize(200, 80));
    if (!document.supportsObjectSelection()) {
        std::cerr << "SKIP: text fixture unsupported on this host: "
                  << document.fallbackReason.toStdString() << '\n';
        return;
    }
    gui::ImageImportFillRequest request;
    request.units = gui::svgImageImportUnits(document);
    request.primitives = primitives;
    test->expect(request.units.size() == 2, "two glyphs should yield two units");
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    test->expect(result.filledCount >= 1, "at least one glyph should fill");
    if (result.filledCount < request.units.size()) {
        reportUnit("text", result);
    }
}

void unsupportedDocumentYieldsNoUnits(TestContext *test)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">"
        "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#f00\"/>"
        "<stop offset=\"1\" stop-color=\"#00f\"/></linearGradient></defs>"
        "<rect width=\"100\" height=\"100\" fill=\"url(#g)\"/></svg>");
    const gui::SvgVectorDocument document = gui::extractSvgVectorObjects(svg, QSize(100, 100));
    test->expect(!document.supportsObjectSelection() && !document.fallbackReason.isEmpty(),
                 "gradient document should report a fallback reason");
    test->expect(gui::svgImageImportUnits(document).isEmpty(),
                 "unsupported document should yield no units");
}

void emptyAndCancelledRequestsAreReported(TestContext *test,
                                          const QVector<gui::PenPrimitive> &primitives)
{
    gui::ImageImportFillRequest empty;
    empty.primitives = primitives;
    test->expect(!gui::computeImageImportFills(empty).error.isEmpty(),
                 "empty unit list should report an error");

    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"50\" height=\"50\">"
        "<rect width=\"50\" height=\"50\" fill=\"#123456\"/></svg>");
    gui::ImageImportFillRequest noPrimitives =
        requestForSvg(test, svg, QSize(50, 50), primitives);
    noPrimitives.primitives.clear();
    test->expect(!gui::computeImageImportFills(noPrimitives).error.isEmpty(),
                 "missing primitive catalog should report an error");

    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(50, 50), primitives);
    const gui::ImageImportFillResult cancelled =
        gui::computeImageImportFills(request, {}, []() { return true; });
    test->expect(cancelled.cancelled, "an immediately cancelled batch should report cancellation");

    gui::ImageImportFillRequest degenerate;
    degenerate.primitives = primitives;
    QPainterPath line;
    line.moveTo(0.0, 0.0);
    line.lineTo(10.0, 0.0);
    degenerate.units.push_back({line, QColor(Qt::black)});
    const gui::ImageImportFillResult degenerateResult = gui::computeImageImportFills(degenerate);
    test->expect(!degenerateResult.error.isEmpty() && degenerateResult.failedCount == 1,
                 "a zero-area outline should fail cleanly without crashing");
}

void repeatedRunsAreDeterministic(TestContext *test,
                                  const QVector<gui::PenPrimitive> &primitives)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"120\">"
        "<path fill=\"#777777\" d=\"M10 110 L60 10 L110 110 Z\"/></svg>");
    const gui::ImageImportFillRequest request =
        requestForSvg(test, svg, QSize(120, 120), primitives);
    const gui::ImageImportFillResult first = gui::computeImageImportFills(request);
    const gui::ImageImportFillResult second = gui::computeImageImportFills(request);
    test->expect(first.filledCount == 1 && second.filledCount == 1, "triangle should fill twice");
    if (first.filledCount != 1 || second.filledCount != 1) {
        return;
    }
    const QVector<gui::PenPlacement> &a = first.units.front().placements;
    const QVector<gui::PenPlacement> &b = second.units.front().placements;
    bool same = a.size() == b.size();
    for (int i = 0; same && i < a.size(); ++i) {
        same = a[i].shapeId == b[i].shapeId && a[i].transform == b[i].transform;
    }
    test->expect(same, "repeated fills of the same outline should produce identical placements");
}

// Runs the import pipeline on a real SVG and prints per-object and
// per-component outcomes, for diagnosing files that import badly.
int reportSvgImport(const QString &path,
                    const QVector<gui::PenPrimitive> &primitives,
                    int shapeLimitPerPoint,
                    qint64 componentBudgetMs)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "could not open " << path.toStdString() << '\n';
        return 2;
    }
    const QByteArray svg = file.readAll();
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        std::cerr << "not a valid SVG\n";
        return 2;
    }
    QSize size = renderer.defaultSize();
    if (size.isEmpty()) {
        size = renderer.viewBoxF().size().toSize();
    }
    const gui::SvgVectorDocument document = gui::extractSvgVectorObjects(svg, size);
    std::cout << "size " << size.width() << "x" << size.height()
              << " objects " << document.objects.size()
              << " fallback '" << document.fallbackReason.toStdString() << "'\n";
    gui::ImageImportFillRequest request;
    request.units = gui::svgImageImportUnits(document);
    request.primitives = primitives;
    request.shapeLimitPerPoint = shapeLimitPerPoint;
    request.componentBudgetMs = componentBudgetMs;
    for (int index = 0; index < request.units.size(); ++index) {
        const gui::ImageImportFillUnit &unit = request.units[index];
        const gui::ImageImportUnitLoops loops = gui::imageImportUnitLoops(unit.outline);
        int points = 0;
        int cutouts = 0;
        for (const QVector<gui::PenLoop> &component : loops.components) {
            cutouts += component.size() - 1;
            for (const gui::PenLoop &loop : component) {
                points += loop.points.size();
            }
        }
        std::cout << "unit " << index << " color " << unit.color.name().toStdString()
                  << " elements " << unit.outline.elementCount()
                  << " components " << loops.components.size()
                  << " cutouts " << cutouts << " pen points " << points
                  << " via " << loops.via.toStdString()
                  << " error '" << loops.error.toStdString() << "'\n";
    }
    QElapsedTimer clock;
    clock.start();
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    std::cout << "elapsed " << clock.elapsed() << " ms\n"
              << "summary: " << result.summary.toStdString() << '\n'
              << "error: '" << result.error.toStdString() << "'\n";
    for (int index = 0; index < result.units.size(); ++index) {
        const gui::ImageImportFilledUnit &unit = result.units[index];
        std::cout << "unit " << index << " via " << unit.via.toStdString()
                  << " components " << unit.componentCount
                  << " failed " << unit.failedComponentCount
                  << " timedOut " << unit.timedOutComponentCount
                  << " placements " << unit.placements.size()
                  << " elapsed " << unit.elapsedMs << " ms"
                  << " error '" << unit.error.toStdString() << "'\n";
        qint64 slowest = 0;
        int slowestIndex = -1;
        for (int componentIndex = 0; componentIndex < unit.components.size(); ++componentIndex) {
            const gui::ImageImportComponentResult &component = unit.components[componentIndex];
            if (component.elapsedMs > slowest) {
                slowest = component.elapsedMs;
                slowestIndex = componentIndex;
            }
            if (!component.filled()) {
                std::cout << "  component " << componentIndex << " loops " << component.loopCount
                          << " points " << component.pointCount
                          << " elapsed " << component.elapsedMs << " ms"
                          << " error '" << component.error.toStdString() << "'\n";
            }
        }
        if (slowestIndex >= 0) {
            std::cout << "  slowest component " << slowestIndex << " took " << slowest << " ms\n";
        }
    }
    std::cout << "component failure reasons:\n";
    for (auto it = result.componentFailureReasons.cbegin();
         it != result.componentFailureReasons.cend(); ++it) {
        std::cout << "  " << it.value() << " x " << it.key().toStdString() << '\n';
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Golden corpus: record the engine's placements for fixture contours and diff
// later runs against them, so engine changes can be verified output-identical.

struct GoldenFixture {
    QString name;
    QByteArray svg;
    QString path;
};

// Deterministic pseudo-random sequence so fixtures are identical on every host.
class FixtureRandom {
public:
    explicit FixtureRandom(quint32 seed) : state_(seed) {}
    double next() {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<double>(state_ >> 8) / static_cast<double>(1u << 24);
    }
    double range(double low, double high) { return low + (high - low) * next(); }

private:
    quint32 state_;
};

QByteArray blobPath(FixtureRandom &random, double centerX, double centerY,
                    double radius, double noise, int segments, bool hardCorners)
{
    QVector<QPointF> points;
    for (int i = 0; i < segments; ++i) {
        const double angle = 2.0 * M_PI * i / segments;
        const double r = radius + random.range(-noise, noise);
        points.push_back(QPointF(centerX + std::cos(angle) * r, centerY + std::sin(angle) * r));
    }
    QByteArray d = "M" + QByteArray::number(points.front().x(), 'f', 2) + " "
        + QByteArray::number(points.front().y(), 'f', 2);
    for (int i = 0; i < segments; ++i) {
        const QPointF a = points[i];
        const QPointF b = points[(i + 1) % segments];
        if (hardCorners && i % 9 == 4) {
            d += " L" + QByteArray::number(b.x(), 'f', 2) + " " + QByteArray::number(b.y(), 'f', 2);
            continue;
        }
        const QPointF c1 = a + (b - a) / 3.0 + QPointF(random.range(-noise, noise), random.range(-noise, noise)) * 0.3;
        const QPointF c2 = a + (b - a) * 2.0 / 3.0 + QPointF(random.range(-noise, noise), random.range(-noise, noise)) * 0.3;
        d += " C" + QByteArray::number(c1.x(), 'f', 2) + " " + QByteArray::number(c1.y(), 'f', 2)
            + " " + QByteArray::number(c2.x(), 'f', 2) + " " + QByteArray::number(c2.y(), 'f', 2)
            + " " + QByteArray::number(b.x(), 'f', 2) + " " + QByteArray::number(b.y(), 'f', 2);
    }
    return d + " Z";
}

QVector<GoldenFixture> goldenFixtures()
{
    QVector<GoldenFixture> fixtures;
    fixtures.push_back({QStringLiteral("ring4"), QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"200\">"
        "<circle cx=\"100\" cy=\"100\" r=\"80\" fill=\"none\" stroke=\"#000\" stroke-width=\"4\"/></svg>"), {}});
    fixtures.push_back({QStringLiteral("ring16"), QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"200\">"
        "<circle cx=\"100\" cy=\"100\" r=\"80\" fill=\"none\" stroke=\"#000\" stroke-width=\"16\"/></svg>"), {}});
    fixtures.push_back({QStringLiteral("discWithHole"), QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 100 100\">"
        "<path fill=\"#22aa44\" fill-rule=\"evenodd\" d=\"M5 50 C5 20 20 5 50 5 C80 5 95 20 95 50 "
        "C95 80 80 95 50 95 C20 95 5 80 5 50 Z M35 35 H65 V65 H35 Z\"/></svg>"), {}});
    fixtures.push_back({QStringLiteral("multiObject"), QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\" viewBox=\"0 0 200 100\">"
        "<rect x=\"5\" y=\"5\" width=\"60\" height=\"60\" fill=\"#ff0000\"/>"
        "<circle cx=\"110\" cy=\"50\" r=\"30\" fill=\"#00ff00\"/>"
        "<path d=\"M150 20 Q190 20 190 55 Q190 90 150 90 Z\" fill=\"#0000ff\"/></svg>"), {}});

    FixtureRandom random(20260914u);
    QByteArray blob = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"600\" height=\"600\">"
        "<path fill=\"#336699\" fill-rule=\"evenodd\" d=\"";
    blob += blobPath(random, 300.0, 300.0, 240.0, 28.0, 96, true);
    blob += " " + blobPath(random, 220.0, 240.0, 55.0, 9.0, 24, false);
    blob += " " + blobPath(random, 380.0, 330.0, 70.0, 12.0, 30, true);
    blob += " " + blobPath(random, 290.0, 420.0, 35.0, 6.0, 18, false);
    blob += "\"/></svg>";
    fixtures.push_back({QStringLiteral("blobWithHoles"), blob, {}});

    QByteArray tendrils = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"500\" height=\"500\">"
        "<path fill=\"none\" stroke=\"#202020\" stroke-width=\"2.5\" stroke-linecap=\"round\" d=\"";
    for (int tendril = 0; tendril < 24; ++tendril) {
        QPointF point(random.range(40.0, 460.0), random.range(40.0, 120.0));
        double heading = M_PI / 2.0 + random.range(-0.4, 0.4);
        tendrils += (tendril == 0 ? "M" : " M") + QByteArray::number(point.x(), 'f', 2) + " "
            + QByteArray::number(point.y(), 'f', 2);
        const int steps = 6 + static_cast<int>(random.range(0.0, 6.0));
        for (int step = 0; step < steps; ++step) {
            heading += random.range(-0.9, 0.9);
            const double length = random.range(18.0, 40.0);
            const QPointF control = point + QPointF(std::cos(heading), std::sin(heading)) * (length * 0.5)
                + QPointF(random.range(-12.0, 12.0), random.range(-12.0, 12.0));
            point += QPointF(std::cos(heading), std::sin(heading)) * length;
            point.setX(std::clamp(point.x(), 20.0, 480.0));
            point.setY(std::clamp(point.y(), 20.0, 480.0));
            tendrils += " Q" + QByteArray::number(control.x(), 'f', 2) + " " + QByteArray::number(control.y(), 'f', 2)
                + " " + QByteArray::number(point.x(), 'f', 2) + " " + QByteArray::number(point.y(), 'f', 2);
        }
    }
    tendrils += "\"/></svg>";
    fixtures.push_back({QStringLiteral("tendrils"), tendrils, {}});

    // Logo-class shapes: a rounded capsule with a circular hole next to a
    // rectangle merged with a half disc, and bold text with counters.
    fixtures.push_back({QStringLiteral("capsuleLogo"), QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"300\" height=\"220\">"
        "<path fill=\"#d04020\" fill-rule=\"evenodd\" d=\"M60 60 A50 50 0 0 1 160 60 L160 160 "
        "A50 50 0 0 1 60 160 Z M85 80 A25 25 0 1 0 135 80 A25 25 0 1 0 85 80 Z\"/>"
        "<path fill=\"#2060c0\" d=\"M200 60 A40 40 0 1 1 280 60 L280 200 L200 200 Z\"/></svg>"), {}});
    fixtures.push_back({QStringLiteral("textLogo"), QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"320\" height=\"220\">"
        "<text x=\"10\" y=\"180\" font-size=\"200\" font-family=\"Arial\" font-weight=\"bold\" "
        "fill=\"#123456\">SB</text></svg>"), {}});

    return fixtures;
}

QVector<GoldenFixture> goldenFixturesWithExtras(const QStringList &extraPaths)
{
    QVector<GoldenFixture> fixtures = goldenFixtures();
    for (const QString &path : extraPaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            std::cerr << "could not open " << path.toStdString() << '\n';
            continue;
        }
        fixtures.push_back({QFileInfo(path).completeBaseName(), file.readAll(), path});
    }
    return fixtures;
}

struct GoldenPlacement {
    int shapeId = 0;
    double m[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
};

struct GoldenComponent {
    QString key;
    QVector<GoldenPlacement> placements;
    QString error;
};

struct GoldenRun {
    QVector<GoldenComponent> components;
    QVector<QPainterPath> unitOutlines;
    QVector<int> componentUnit;
    qint64 elapsedMs = 0;
    qint64 slowestMs = 0;
    QString slowestKey;
    int shapes = 0;
};

QTransform goldenTransform(const GoldenPlacement &placement)
{
    return QTransform(placement.m[0], placement.m[1], placement.m[2], placement.m[3],
                      placement.m[4], placement.m[5]);
}

bool samePlacement(const GoldenPlacement &a, const GoldenPlacement &b)
{
    if (a.shapeId != b.shapeId) {
        return false;
    }
    for (int k = 0; k < 6; ++k) {
        if (std::abs(a.m[k] - b.m[k]) > 1e-6) {
            return false;
        }
    }
    return true;
}

// Covered fraction of the outline measured on a raster, which stays reliable
// for hundreds of placements where chained path unions do not.
double rasterCoveredFraction(const QVector<GoldenPlacement> &placements,
                             const QPainterPath &outline,
                             const QVector<gui::PenPrimitive> &primitives)
{
    const QRectF bounds = outline.boundingRect();
    if (bounds.isEmpty()) {
        return 0.0;
    }
    const double scale = 1200.0 / std::max(bounds.width(), bounds.height());
    const QSize size(static_cast<int>(std::ceil(bounds.width() * scale)) + 3,
                     static_cast<int>(std::ceil(bounds.height() * scale)) + 3);
    const auto render = [&](const std::function<void(QPainter &)> &draw) {
        QImage image(size, QImage::Format_Grayscale8);
        image.fill(0);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.translate(1.0, 1.0);
        painter.scale(scale, scale);
        painter.translate(-bounds.topLeft());
        draw(painter);
        painter.end();
        return image;
    };
    const QImage outlineImage = render([&](QPainter &painter) { painter.drawPath(outline); });
    const QImage coverImage = render([&](QPainter &painter) {
        for (const GoldenPlacement &placement : placements) {
            for (const gui::PenPrimitive &primitive : primitives) {
                if (primitive.shapeId == placement.shapeId) {
                    painter.drawPath(goldenTransform(placement).map(primitive.silhouette));
                    break;
                }
            }
        }
    });
    long long outlinePixels = 0;
    long long coveredPixels = 0;
    for (int y = 0; y < size.height(); ++y) {
        const uchar *outlineRow = outlineImage.constScanLine(y);
        const uchar *coverRow = coverImage.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            if (outlineRow[x] > 127) {
                ++outlinePixels;
                if (coverRow[x] > 127) {
                    ++coveredPixels;
                }
            }
        }
    }
    return outlinePixels > 0 ? static_cast<double>(coveredPixels) / outlinePixels : 0.0;
}

// Explains a placement-count diff: which placements have no counterpart on the
// other side, how much area they carry, and whether the covered fraction of
// the source outline moved.
QString explainPlacementDiff(const GoldenComponent &before,
                             const GoldenComponent &after,
                             const QPainterPath &outline,
                             const QVector<gui::PenPrimitive> &primitives)
{
    const auto primitiveFor = [&](int shapeId) -> const gui::PenPrimitive * {
        for (const gui::PenPrimitive &primitive : primitives) {
            if (primitive.shapeId == shapeId) {
                return &primitive;
            }
        }
        return nullptr;
    };
    const auto unmatched = [&](const QVector<GoldenPlacement> &left,
                               const QVector<GoldenPlacement> &right) {
        QVector<GoldenPlacement> result;
        for (const GoldenPlacement &placement : left) {
            const bool found = std::any_of(right.cbegin(), right.cend(), [&](const GoldenPlacement &other) {
                return samePlacement(placement, other);
            });
            if (!found) {
                result.push_back(placement);
            }
        }
        return result;
    };
    const auto placementArea = [&](const QVector<GoldenPlacement> &placements) {
        double area = 0.0;
        for (const GoldenPlacement &placement : placements) {
            if (const gui::PenPrimitive *primitive = primitiveFor(placement.shapeId)) {
                area += primitive->area * std::abs(goldenTransform(placement).determinant());
            }
        }
        return area;
    };
    const auto coveredFraction = [&](const QVector<GoldenPlacement> &placements) {
        return rasterCoveredFraction(placements, outline, primitives);
    };
    const QVector<GoldenPlacement> onlyBefore = unmatched(before.placements, after.placements);
    const QVector<GoldenPlacement> onlyAfter = unmatched(after.placements, before.placements);
    const double outlineArea = filledPathArea(outline);

    return QStringLiteral("only-before %1 placements (%2%% of outline area), only-after %3 placements (%4%%), "
                          "outline covered %5%% -> %6%%")
        .arg(onlyBefore.size())
        .arg(outlineArea > 0.0 ? placementArea(onlyBefore) / outlineArea * 100.0 : 0.0, 0, 'f', 4)
        .arg(onlyAfter.size())
        .arg(outlineArea > 0.0 ? placementArea(onlyAfter) / outlineArea * 100.0 : 0.0, 0, 'f', 4)
        .arg(coveredFraction(before.placements) * 100.0, 0, 'f', 3)
        .arg(coveredFraction(after.placements) * 100.0, 0, 'f', 3);
}

// Records placements per (unit, component). The engine emits placements in a
// deterministic order per component, so the list is compared positionally.
GoldenRun runGolden(const GoldenFixture &fixture, const QVector<gui::PenPrimitive> &primitives)
{
    GoldenRun run;
    QSvgRenderer renderer(fixture.svg);
    QSize size = renderer.defaultSize();
    if (size.isEmpty()) {
        size = renderer.viewBoxF().size().toSize();
    }
    const gui::SvgVectorDocument document = gui::extractSvgVectorObjects(fixture.svg, size);
    gui::ImageImportFillRequest request;
    request.units = gui::svgImageImportUnits(document);
    request.primitives = primitives;
    request.componentBudgetMs = 3600000;
    request.componentBudgetMsPerPoint = 0;
    QElapsedTimer clock;
    clock.start();
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);
    run.elapsedMs = clock.elapsed();
    for (const gui::ImageImportFillUnit &unit : request.units) {
        run.unitOutlines.push_back(unit.outline);
    }
    for (int unitIndex = 0; unitIndex < result.units.size(); ++unitIndex) {
        const gui::ImageImportFilledUnit &unit = result.units[unitIndex];
        run.shapes += unit.placements.size();
        int offset = 0;
        for (int componentIndex = 0; componentIndex < unit.components.size(); ++componentIndex) {
            const gui::ImageImportComponentResult &component = unit.components[componentIndex];
            GoldenComponent golden;
            golden.key = QStringLiteral("unit%1/component%2").arg(unitIndex).arg(componentIndex);
            golden.error = component.error;
            for (int i = 0; i < component.placementCount; ++i) {
                const gui::PenPlacement &placement = unit.placements[offset + i];
                const QTransform &t = placement.transform;
                golden.placements.push_back({placement.shapeId,
                                             {t.m11(), t.m12(), t.m21(), t.m22(), t.dx(), t.dy()}});
            }
            offset += component.placementCount;
            if (component.elapsedMs > run.slowestMs) {
                run.slowestMs = component.elapsedMs;
                run.slowestKey = golden.key;
            }
            run.componentUnit.push_back(unitIndex);
            run.components.push_back(std::move(golden));
        }
    }
    return run;
}

QString goldenFilePath(const QString &directory, const QString &name)
{
    return QDir(directory).filePath(name + QStringLiteral(".golden.txt"));
}

bool writeGolden(const QString &path, const GoldenRun &run)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setRealNumberNotation(QTextStream::FixedNotation);
    stream.setRealNumberPrecision(6);
    for (const GoldenComponent &component : run.components) {
        stream << "component " << component.key << " placements " << component.placements.size();
        if (!component.error.isEmpty()) {
            stream << " error " << component.error;
        }
        stream << '\n';
        for (const GoldenPlacement &placement : component.placements) {
            stream << "  " << placement.shapeId;
            for (const double value : placement.m) {
                stream << ' ' << value;
            }
            stream << '\n';
        }
    }
    return true;
}

bool readGolden(const QString &path, GoldenRun *run)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    GoldenComponent *current = nullptr;
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (line.startsWith(QStringLiteral("component "))) {
            const QStringList parts = line.split(QLatin1Char(' '));
            GoldenComponent component;
            component.key = parts.value(1);
            const int errorIndex = line.indexOf(QStringLiteral(" error "));
            if (errorIndex >= 0) {
                component.error = line.mid(errorIndex + 7);
            }
            run->components.push_back(component);
            current = &run->components.last();
        } else if (current != nullptr && line.startsWith(QStringLiteral("  "))) {
            const QStringList parts = line.trimmed().split(QLatin1Char(' '));
            if (parts.size() == 7) {
                GoldenPlacement placement;
                placement.shapeId = parts[0].toInt();
                for (int i = 0; i < 6; ++i) {
                    placement.m[i] = parts[i + 1].toDouble();
                }
                current->placements.push_back(placement);
            }
        }
    }
    return true;
}

// Draws an import result over its source outlines so a fill can be judged by
// eye: triangles and squares in grey, curved Primitives in blue, outlines in
// red. Runs with the unlimited budget the golden corpus uses.
int renderSvgImport(const QString &path,
                    const QString &outputPath,
                    const QVector<gui::PenPrimitive> &primitives,
                    double scale)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "could not open " << path.toStdString() << '\n';
        return 2;
    }
    const QByteArray svg = file.readAll();
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        std::cerr << "not a valid SVG\n";
        return 2;
    }
    QSize size = renderer.defaultSize();
    if (size.isEmpty()) {
        size = renderer.viewBoxF().size().toSize();
    }
    const gui::SvgVectorDocument document = gui::extractSvgVectorObjects(svg, size);
    gui::ImageImportFillRequest request;
    request.units = gui::svgImageImportUnits(document);
    request.primitives = primitives;
    request.componentBudgetMs = 3600000;
    request.componentBudgetMsPerPoint = 0;
    const gui::ImageImportFillResult result = gui::computeImageImportFills(request);

    QImage image(QSize(static_cast<int>(std::ceil(size.width() * scale)),
                       static_cast<int>(std::ceil(size.height() * scale))),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(scale, scale);
    painter.setPen(Qt::NoPen);
    int curved = 0;
    for (const gui::ImageImportFilledUnit &unit : result.units) {
        for (const gui::PenPlacement &placement : unit.placements) {
            const gui::PenPrimitive *primitive = nullptr;
            for (const gui::PenPrimitive &candidate : primitives) {
                if (candidate.shapeId == placement.shapeId) {
                    primitive = &candidate;
                    break;
                }
            }
            if (primitive == nullptr) {
                continue;
            }
            const bool isCurved = placement.shapeId != 101 && placement.shapeId != 103;
            curved += isCurved ? 1 : 0;
            painter.setBrush(isCurved ? QColor(40, 90, 220, 110) : QColor(90, 90, 90, 90));
            painter.setPen(QPen(isCurved ? QColor(20, 50, 160) : QColor(60, 60, 60), 0.4 / scale));
            painter.drawPath(placement.transform.map(primitive->silhouette));
        }
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(220, 30, 30), 1.0 / scale));
    for (const gui::ImageImportFillUnit &unit : request.units) {
        painter.drawPath(unit.outline);
    }
    painter.end();
    if (!image.save(outputPath)) {
        std::cerr << "could not write " << outputPath.toStdString() << '\n';
        return 2;
    }
    std::cout << "summary: " << result.summary.toStdString() << '\n'
              << "placements " << result.placementCount << " curved " << curved
              << " -> " << QDir::toNativeSeparators(outputPath).toStdString() << '\n';
    return 0;
}

int goldenRecord(const QString &directory, const QStringList &extraPaths,
                 const QVector<gui::PenPrimitive> &primitives)
{
    QDir().mkpath(directory);
    for (const GoldenFixture &fixture : goldenFixturesWithExtras(extraPaths)) {
        const GoldenRun run = runGolden(fixture, primitives);
        const QString path = goldenFilePath(directory, fixture.name);
        if (!writeGolden(path, run)) {
            std::cerr << "could not write " << path.toStdString() << '\n';
            return 2;
        }
        if (fixture.path.isEmpty()) {
            QFile svgFile(QDir(directory).filePath(fixture.name + QStringLiteral(".svg")));
            if (svgFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                svgFile.write(fixture.svg);
            }
        }
        std::cout << fixture.name.toStdString() << ": " << run.components.size() << " components, "
                  << run.shapes << " shapes, " << run.elapsedMs << " ms (slowest "
                  << run.slowestKey.toStdString() << " " << run.slowestMs << " ms) -> "
                  << QDir::toNativeSeparators(path).toStdString() << '\n';
    }
    return 0;
}

int goldenCheck(const QString &directory, const QStringList &extraPaths,
                const QVector<gui::PenPrimitive> &primitives)
{
    constexpr double kTransformNoise = 1e-4;
    int totalDiffs = 0;
    for (const GoldenFixture &fixture : goldenFixturesWithExtras(extraPaths)) {
        GoldenRun expected;
        const QString path = goldenFilePath(directory, fixture.name);
        if (!readGolden(path, &expected)) {
            std::cerr << fixture.name.toStdString() << ": no golden file at "
                      << QDir::toNativeSeparators(path).toStdString() << '\n';
            ++totalDiffs;
            continue;
        }
        const GoldenRun actual = runGolden(fixture, primitives);
        int diffs = 0;
        int noiseOnly = 0;
        QStringList details;
        const int count = std::max(expected.components.size(), actual.components.size());
        for (int i = 0; i < count; ++i) {
            if (i >= expected.components.size() || i >= actual.components.size()) {
                ++diffs;
                details << QStringLiteral("component count changed: %1 -> %2")
                              .arg(expected.components.size()).arg(actual.components.size());
                break;
            }
            const GoldenComponent &before = expected.components[i];
            const GoldenComponent &after = actual.components[i];
            if (before.placements.size() != after.placements.size() || before.error != after.error) {
                ++diffs;
                const int unitIndex = actual.componentUnit.value(i, -1);
                const QPainterPath outline = unitIndex >= 0 && unitIndex < actual.unitOutlines.size()
                    ? actual.unitOutlines[unitIndex] : QPainterPath();
                details << QStringLiteral("%1: placements %2 -> %3, error '%4' -> '%5'; %6")
                              .arg(before.key).arg(before.placements.size()).arg(after.placements.size())
                              .arg(before.error, after.error,
                                   explainPlacementDiff(before, after, outline, primitives));
                continue;
            }
            bool shapeChanged = false;
            double maximumDelta = 0.0;
            int firstChanged = -1;
            for (int p = 0; p < before.placements.size(); ++p) {
                if (before.placements[p].shapeId != after.placements[p].shapeId) {
                    shapeChanged = true;
                    firstChanged = firstChanged < 0 ? p : firstChanged;
                    break;
                }
                for (int k = 0; k < 6; ++k) {
                    const double delta = std::abs(before.placements[p].m[k] - after.placements[p].m[k]);
                    if (delta > maximumDelta) {
                        maximumDelta = delta;
                        if (delta > kTransformNoise) {
                            firstChanged = firstChanged < 0 ? p : firstChanged;
                        }
                    }
                }
            }
            if (shapeChanged || maximumDelta > kTransformNoise) {
                ++diffs;
                details << QStringLiteral("%1: %2 at placement %3 (max transform delta %4)")
                              .arg(before.key,
                                   shapeChanged ? QStringLiteral("shape id changed")
                                                : QStringLiteral("transform changed"))
                              .arg(firstChanged)
                              .arg(maximumDelta, 0, 'g', 6);
            } else if (maximumDelta > 1e-6) {
                ++noiseOnly;
            }
        }
        totalDiffs += diffs;
        std::cout << fixture.name.toStdString() << ": " << actual.components.size() << " components, "
                  << actual.shapes << " shapes, " << actual.elapsedMs << " ms (slowest "
                  << actual.slowestKey.toStdString() << " " << actual.slowestMs << " ms), diffs "
                  << diffs << ", sub-1e-4 numerical noise in " << noiseOnly << " components\n";
        // Per-object totals so a run can be judged on what the livery would
        // show: shape count, curved Primitive count, and rasterised coverage of
        // the object outline, before and after.
        if (diffs > 0 && expected.components.size() == actual.components.size()) {
            for (int unit = 0; unit < actual.unitOutlines.size(); ++unit) {
                QVector<GoldenPlacement> before;
                QVector<GoldenPlacement> after;
                for (int i = 0; i < actual.components.size(); ++i) {
                    if (actual.componentUnit.value(i, -1) != unit) {
                        continue;
                    }
                    before += expected.components[i].placements;
                    after += actual.components[i].placements;
                }
                const auto curved = [](const QVector<GoldenPlacement> &placements) {
                    return static_cast<int>(std::count_if(placements.begin(), placements.end(),
                        [](const GoldenPlacement &placement) {
                            return placement.shapeId != 101 && placement.shapeId != 103;
                        }));
                };
                const auto covered = [&](const QVector<GoldenPlacement> &placements) {
                    return QString::number(rasterCoveredFraction(placements, actual.unitOutlines[unit],
                                                                 primitives) * 100.0, 'f', 3).toStdString();
                };
                std::cout << "    unit" << unit << " totals: shapes " << before.size() << " -> "
                          << after.size() << " (curved " << curved(before) << " -> " << curved(after)
                          << "), outline covered " << covered(before) << "% -> " << covered(after) << "%\n";
            }
        }
        for (const QString &detail : details) {
            std::cout << "    " << detail.toStdString() << '\n';
        }
    }
    std::cout << "total diffs " << totalDiffs << '\n';
    return totalDiffs == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    TestContext test;
    const QVector<gui::PenPrimitive> primitives = penPrimitiveCatalog(&test);
    if (primitives.isEmpty()) {
        std::cerr << "FAIL: the Pen Primitive catalog is empty\n";
        return 1;
    }
    if (argc >= 3 && (QString::fromLocal8Bit(argv[1]) == QStringLiteral("--golden-record")
                      || QString::fromLocal8Bit(argv[1]) == QStringLiteral("--golden-check"))) {
        QStringList extraPaths;
        for (int i = 3; i < argc; ++i) {
            extraPaths.push_back(QString::fromLocal8Bit(argv[i]));
        }
        const QString directory = QString::fromLocal8Bit(argv[2]);
        return QString::fromLocal8Bit(argv[1]) == QStringLiteral("--golden-record")
            ? goldenRecord(directory, extraPaths, primitives)
            : goldenCheck(directory, extraPaths, primitives);
    }
    if (argc >= 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--render")) {
        const double scale = argc >= 5 ? QString::fromLocal8Bit(argv[4]).toDouble() : 4.0;
        return renderSvgImport(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]),
                               primitives, scale > 0.0 ? scale : 4.0);
    }
    if (argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--report")) {
        const int shapeLimitPerPoint = argc >= 4 ? QString::fromLocal8Bit(argv[3]).toInt() : 6;
        const qint64 componentBudgetMs = argc >= 5 ? QString::fromLocal8Bit(argv[4]).toLongLong() : 3000;
        return reportSvgImport(QString::fromLocal8Bit(argv[2]), primitives,
                               shapeLimitPerPoint, componentBudgetMs);
    }
    rectangleFillsCompletely(&test, primitives);
    ringPreservesItsHole(&test, primitives);
    islandsInOnePathFillSeparately(&test, primitives);
    islandInsideHoleBecomesItsOwnComponent(&test, primitives);
    multipleObjectsKeepOrderAndColor(&test, primitives);
    strokeOnlyObjectFillsAsOutline(&test, primitives);
    thinRingsStayRound(&test, primitives);
    textObjectsProduceGlyphUnits(&test, primitives);
    unsupportedDocumentYieldsNoUnits(&test);
    emptyAndCancelledRequestsAreReported(&test, primitives);
    repeatedRunsAreDeterministic(&test, primitives);
    if (test.failures() > 0) {
        std::cerr << test.failures() << " image import test(s) failed\n";
        return 1;
    }
    std::cout << "image import tests passed\n";

    return 0;
}
