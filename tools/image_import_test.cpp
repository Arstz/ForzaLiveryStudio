#include "image_import_fill.h"
#include "svg_vector_objects.h"

#include <QtCore>
#include <QtGui>
#include <QtSvg/QSvgRenderer>

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
