#include "optimizer.h"

#include "shape_geometry_store.h"
#include <QSvgRenderer>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace opaque {
namespace {

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

QPainterPath rect(double x, double y, double width, double height) {
    QPainterPath path;
    path.addRect(x, y, width, height);
    return path;
}

QPainterPath polygon(std::initializer_list<QPointF> points) {
    QPainterPath path;
    bool first = true;
    for (const auto &point : points) {
        if (first) path.moveTo(point);
        else path.lineTo(point);
        first = false;
    }
    path.closeSubpath();
    return path;
}

Result optimize(const QVector<gui::SvgVectorObject> &input, int chordSpans = 8, bool expectRejected = false) {
    gui::ShapeGeometryStore geometry;
    QString error;
    require(geometry.loadDefault(&error), qPrintable(error));
    const auto sources = gui::buildPolygonMeshSources(geometry);
    require(sources.valid(), "Missing native mesh sources");
    Options options;
    options.renderRmse = 12;
    options.renderMaxError = 255;
    options.silhouetteTolerance = 0.01;
    options.maximumPasses = 4;
    options.maximumChordSpans = chordSpans;
    Optimizer optimizer(makeItems(input), options, meshEncoder(sources, options),
        [](const auto &objects) { return render(objects, {64, 64}); });
    auto result = optimizer.run();
    if (expectRejected)
        require(result.error.contains("decomposition exceeds"), "Invalid native baseline was accepted");
    else
        require(result.error.isEmpty(), qPrintable(result.error));
    require(result.silhouetteDifference <= options.silhouetteTolerance, "Silhouette changed");
    require(result.after.cost <= result.before.cost, "Weighted SVG complexity increased");
    return result;
}

void holeFilling() {
    auto ring = rect(4, 4, 48, 48);
    ring.setFillRule(Qt::OddEvenFill);
    ring.addRect(16, 16, 24, 24);
    const auto result = optimize({{ring, Qt::red}, {rect(16, 16, 24, 24), Qt::blue}});
    require(result.accepted > 0 && result.after.holes == 0, "Covered hole was not filled");
    require(result.after.drawItems < result.before.drawItems, "Hole fill did not reduce actual decomposition");
    require(result.graph.error.isEmpty(), "Hole fill ordering invalid");
    const auto image = render(objects(result.items), {64, 64});
    require(image.pixelColor(24, 24) == QColor(Qt::blue), "Hole occluder drawn underneath");
}

void chordAndRectangle() {
    const auto notched = polygon({{4, 4}, {52, 4}, {52, 52}, {36, 52},
        {36, 24}, {20, 24}, {20, 52}, {4, 52}});
    const auto result = optimize({{notched, Qt::red}, {rect(20, 24, 16, 28), Qt::blue}});
    require(result.accepted > 0 && result.after.drawItems < result.before.drawItems,
            "Hidden boundary chord/rectangle did not simplify native geometry");
    require(!result.graph.edges.empty(), "Shortcut did not retain restoration dependency");
    require(render(objects(result.items), {64, 64}).pixelColor(24, 40) == QColor(Qt::blue),
            "Shortcut changed visible ownership");
    require(result.diagnostics.at(0).toObject().value("accepted_kind").toString() == "chord",
            "Hidden-chain shortcut was not exercised");
    const auto completed = optimize({{notched, Qt::red}, {rect(20, 24, 16, 28), Qt::blue}}, 0);
    require(completed.diagnostics.at(0).toObject().value("accepted_kind").toString() == "rectangle",
            "Enclosing rectangle completion was not exercised");
}

void bridgesAndAppearanceGroups() {
    const QVector<gui::SvgVectorObject> input{
        {rect(4, 4, 12, 40), Qt::red}, {rect(40, 4, 12, 40), Qt::red},
        {rect(16, 4, 24, 40), Qt::blue}};
    const auto result = optimize(input);
    require(result.items.size() == 2 && result.after.drawItems == 2,
            "Hidden same-fill bridge did not produce one rectangle");
    require(result.after.components < result.before.components, "Bridge left disconnected geometry");
    const auto repeat = optimize(input);
    require(svg(objects(result.items), {64, 64}) == svg(objects(repeat.items), {64, 64}),
            "Optimization is nondeterministic");
}

void adjacentMerge() {
    const auto result = optimize({{rect(4, 4, 24, 40), Qt::red},
                                  {rect(28, 4, 24, 40), Qt::red}});
    require(result.items.size() == 1 && result.after.drawItems == 1, "Adjacent same-fill regions did not merge");
    require(result.items.front().path.elementCount() == 5, "Merge left the shared boundary in SVG geometry");
}

void cyclesAndSeparateStackPositions() {
    const QVector<gui::SvgVectorObject> input{
        {rect(4, 4, 32, 32), Qt::red}, {rect(20, 4, 32, 32), Qt::blue},
        {rect(28, 12, 8, 16), Qt::red}};
    Options options;
    Optimizer optimizer(makeItems(input), options, {}, {});
    auto merged = makeItems(input);
    merged[0].owners.insert(2);
    merged[0].path.addPath(merged[2].path);
    merged.removeAt(2);
    const auto graph = optimizer.dependencies(merged);
    require(graph.error == "Dependency cycle" && graph.edges.size() == 2,
            "Same-color merge across incompatible layer positions should cycle");
    const auto result = optimize(input);
    require(result.items.size() == 3, "Incompatible same-fill positions were merged");
    require(result.after.paths == 3 && result.after.fills == 2
        && result.after.maximumPathsPerFill == 2, "One-path-per-fill preference became a hard constraint");
    require(result.after.orderingEdges == result.graph.edges.size()
        && result.after.orderingEdges >= 2 && result.after.orderingDepth == 3,
            "Ordering metrics lost required separate layer positions");
    require(result.graph.error.isEmpty(), "Cycle rejection left a stale dependency");
    const auto stale = topologicalOrder(3, {{0, 1}, {1, 2}, {2, 0}});
    require(stale.error == "Dependency cycle", "Explicit dependency cycle was accepted");
}

void silhouetteAndNoOp() {
    QPainterPath curve;
    curve.moveTo(4, 32);
    curve.cubicTo(4, 4, 52, 4, 52, 32);
    curve.lineTo(52, 52); curve.lineTo(4, 52); curve.closeSubpath();
    const QVector<gui::SvgVectorObject> input{{curve, Qt::red}};
    Optimizer optimizer(makeItems(input), Options{}, {}, {});
    auto expanded = makeItems(input);
    expanded[0].path = rect(4, 4, 48, 48);
    require(optimizer.validate(expanded).contains("silhouette"), "External silhouette expansion accepted");
    auto missing = makeItems(input);
    missing[0].path = rect(12, 32, 20, 20);
    require(optimizer.validate(missing).contains("territory"), "Visible territory removal accepted");
    auto invalid = makeItems(input);
    invalid[0].path = polygon({{4, 4}, {52, 52}, {4, 52}, {52, 4}});
    require(optimizer.validate(invalid).contains("Invalid"), "Self-intersection was accepted");
    const auto a = optimize({{rect(4, 4, 48, 48), Qt::red}});
    const auto b = optimize({{rect(4, 4, 48, 48), Qt::red}});
    require(a.accepted == 0 && a.before.cost == a.after.cost, "Rectangle no-op changed geometry");
    require(svg(objects(a.items), {64, 64}) == svg(objects(b.items), {64, 64}), "No-op nondeterministic");
    require(svg(objects(makeItems(input)), {64, 64}).contains("C4 4 52 4 52 32"),
            "Original visible cubic span was flattened on export");
    const auto preserved = optimize(input, 8, true);
    require(preserved.accepted == 0 && svg(objects(preserved.items), {64, 64})
        == svg(input, {64, 64}), "Curved silhouette changed in no-op optimization");
}

void exportAudit() {
    const QByteArray source =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\" viewBox=\"0 0 32 32\">"
        "<g transform=\"translate(2 2)\"><path fill=\"red\" fill-rule=\"evenodd\" "
        "d=\"M0 0H24V24H0Z M4 4H20V20H4Z\"/><rect x=\"8\" y=\"8\" width=\"8\" height=\"8\" fill=\"blue\"/>"
        "</g></svg>";
    const auto document = gui::extractSvgVectorObjects(source, {64, 64});
    require(document.supportsObjectSelection() && document.objects.size() == 2, "Export lost a path");
    const auto output = svg(document.objects, {64, 64});
    const auto restored = gui::extractSvgVectorObjects(output, {64, 64});
    require(restored.objects.size() == 2 && output.count('Z') == 3, "Closed-loop export lost a hole");
    const auto expected = render(document.objects, {64, 64});
    require(imageError(expected, render(restored.objects, {64, 64})).value("rmse").toDouble() == 0,
            "Fill rule or transform roundtrip changed appearance");
    const QVector<gui::SvgVectorObject> overlapping{
        {rect(4, 4, 40, 40), Qt::red}, {rect(20, 20, 36, 36), Qt::blue}};
    auto reversed = overlapping;
    std::reverse(reversed.begin(), reversed.end());
    require(imageError(render(overlapping, {64, 64}), render(reversed, {64, 64}))
        .value("rmse").toDouble() > 0, "Export-order error was not detected");
    QPainterPath ellipse;
    ellipse.addEllipse(4.2, 4.2, 40, 40);
    const auto aa = render({{ellipse, Qt::red}}, {64, 64});
    bool partial = false;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            partial |= aa.pixelColor(x, y).alpha() > 0 && aa.pixelColor(x, y).alpha() < 255;
    require(partial, "Boundary AA fixture needs fractional coverage with opaque fill");
    const auto transparent = render({{rect(4, 4, 40, 40), QColor(255, 0, 0, 100)}}, {64, 64});
    require(imageError(transparent, render({{rect(4, 4, 40, 40), Qt::red}}, {64, 64}))
        .value("rmse").toDouble() > 20, "Transparency-to-opaque error was hidden");
}

void pathCountAndTopology() {
    QPainterPath compound = rect(4, 4, 12, 12);
    compound.addRect(36, 4, 12, 12);
    compound.addRect(6, 6, 4, 4);
    const auto stats = pathMetrics({{compound, Qt::red}, {rect(4, 32, 12, 12), Qt::red}}, .05);
    require(stats.paths == 2 && stats.fills == 1 && stats.maximumPathsPerFill == 2,
            "Path count was confused with color count");
    require(stats.subpaths == 4 && stats.lineSegments == 16 && stats.bezierSegments == 0,
            "Subpaths were confused with selectable paths or SVG segments");
    require(stats.components == 3 && stats.holes == 1 && stats.maximumComponentsPerPath == 2
        && stats.maximumHolesPerPath == 1 && stats.maximumSegmentsPerPath == 12,
        "Compound path topology metrics are incorrect");
    const auto packed = optimize({{rect(4, 4, 12, 12), Qt::red}, {rect(36, 4, 12, 12), Qt::red}});
    require(packed.accepted == 0 && packed.after.paths == 2,
            "Disconnected SVG repacking was counted as geometry simplification");
    QPainterPath bezier;
    bezier.moveTo(4, 32); bezier.cubicTo(4, 4, 52, 4, 52, 32); bezier.closeSubpath();
    const auto curves = pathMetrics({{bezier, Qt::blue}}, .05);
    require(curves.bezierSegments == 1 && curves.lineSegments == 1,
            "Bezier commands were counted as flattened polygon edges");
}

Result stackOptimize(const QVector<gui::SvgVectorObject> &input, bool compare = true) {
    Options options;
    options.strategy = "savings";
    options.compareStrategies = compare;
    options.renderRmse = 0.5;
    options.renderMaxError = 16;
    Optimizer optimizer(makeItems(input), options, {},
        [](const auto &objects) { return render(objects, {64, 64}); });
    auto result = optimizer.run();
    require(result.error.isEmpty(), qPrintable(result.error));
    require(imageError(render(input, {64, 64}), render(objects(result.items), {64, 64}))
        .value("rmse").toDouble() <= options.renderRmse, "Stack changed baseline appearance");
    require(result.graph.error.isEmpty(), "Bottom-up stack contains a cycle");
    return result;
}

void bottomUpCompletion() {
    auto ring = rect(4, 4, 56, 56);
    ring.addRect(6, 6, 52, 52);
    const auto result = stackOptimize({{ring, Qt::red}, {rect(6, 6, 52, 52), Qt::blue}});
    require(result.accepted && result.after.holes == 0, "Large hidden rectangle completion was not accepted");
    require(result.items.front().path.contains(QPointF(32, 32)), "Large hidden area was limited by visible area");
    require(result.strategies.size() == 3, "Ordering strategies were not compared");
    const auto again = stackOptimize({{ring, Qt::red}, {rect(6, 6, 52, 52), Qt::blue}});
    require(svg(objects(result.items), {64, 64}) == svg(objects(again.items), {64, 64}),
            "Stack selection is nondeterministic");
    QPainterPath green;
    green.addEllipse(22, 20, 20, 20);
    const QVector<gui::SvgVectorObject> scene{{rect(4, 4, 28, 52), Qt::red},
        {rect(32, 4, 28, 52), Qt::blue}, {green, Qt::green}};
    const auto covered = stackOptimize(scene);
    Options options;
    options.strategy = "savings";
    Optimizer graph(makeItems(scene), options, {}, {});
    require(graph.dependencies(graph.baselineItems()).edges == std::set<std::pair<int, int>>{{0, 2}, {1, 2}},
            "Overlap covered by green imposed a false red/blue order");
    require(covered.items.size() == 3, "Independent lower regions were lost");
    const auto rectangleDifficulty = regionDifficulty(rect(4, 4, 56, 56), options);
    const auto ringDifficulty = regionDifficulty(ring, options);
    require(rectangleDifficulty.value("estimate").toDouble() < ringDifficulty.value("estimate").toDouble(),
            "Hole complexity was not reflected in difficulty");
    require(rectangleDifficulty.value("hard_points").toInt() == 4, "Rectangle hard points are not predictable");
    const auto hidden = stackOptimize({{rect(4, 4, 56, 56), Qt::red},
                                      {rect(4, 4, 56, 56), Qt::blue}});
    require(hidden.items.size() == 1 && hidden.items.front().color == QColor(Qt::blue),
            "Fully hidden owner was not removed");
    const auto layered = stackOptimize({{rect(4, 4, 32, 32), Qt::red},
        {rect(20, 4, 32, 32), Qt::blue}, {rect(28, 12, 8, 16), Qt::red}});
    require(layered.graph.error.isEmpty() && layered.after.cost <= layered.before.cost,
            "Same-fill stack conflict left stale dependencies");
}

void ownershipPrecision() {
    QVector<gui::SvgVectorObject> scene{{rect(0, 0, 1248, 1248), Qt::white}};
    for (int index = 0; index < 40; ++index) {
        QPainterPath ellipse;
        ellipse.addEllipse(100.123456789 + index * 6.123456789, 70.987654321 + index * 4.876543219,
                           400.123456789, 320.987654321);
        scene.push_back({ellipse, QColor::fromRgb(index * 5, 90, 140)});
    }
    Options options;
    options.strategy = "savings";
    options.maximumPasses = 0;
    Optimizer optimizer(makeItems(scene), options, {}, {});
    require(optimizer.validate(optimizer.baselineItems()).isEmpty(),
            "Boolean rounding rejected unchanged native-scale ownership");
}

void opaqueBoundaryRepair() {
    auto ring = rect(0, 0, 64, 64);
    QPainterPath hole;
    hole.addEllipse(10.2, 10.2, 43.6, 43.6);
    ring.setFillRule(Qt::OddEvenFill);
    ring.addPath(hole);
    const QVector<gui::SvgVectorObject> scene{{ring, Qt::red}, {hole, Qt::blue}};
    Options options;
    options.strategy = "current";
    options.renderRmse = 0;
    options.renderMaxError = 0;
    Optimizer optimizer(makeItems(scene), options, {},
        [](const auto &objects) { return render(objects, {64, 64}); });
    const auto result = optimizer.run();
    require(result.error.isEmpty(), qPrintable(result.error));
    require(result.strategies.at(0).toObject().value("render_reverted_items").toInt() > 0,
            "Opaque boundary AA mismatch did not restore the affected contour");
    require(imageError(render(scene, {64, 64}), render(objects(result.items), {64, 64}))
        .value("rmse").toDouble() == 0, "Repair loosened the opaque rendering budget");
}

void threatenedCompletion() {
    QVector<gui::SvgVectorObject> scene{{rect(0, 0, 64, 64), Qt::white}};
    for (int index = 0; index < 15; ++index) {
        const int x = 2 + (index % 5) * 12, y = 2 + (index / 5) * 16;
        scene.push_back({polygon({{double(x), double(y)}, {double(x + 8), double(y)},
            {double(x + 8), double(y + 4)}, {double(x + 4), double(y + 4)},
            {double(x + 4), double(y + 8)}, {double(x), double(y + 8)}}), QColor(200, index * 10, 40)});
        scene.push_back({rect(x + 4, y + 4, 4, 4), QColor(20, 50, 100 + index * 10)});
    }
    Options options;
    options.strategy = "savings";
    options.shortlistSize = 1;
    Optimizer optimizer(makeItems(scene), options, {},
        [](const auto &objects) { return render(objects, {64, 64}); });
    const auto result = optimizer.run();
    require(result.error.isEmpty() && result.accepted, "Threatened background completion was excluded from the shortlist");
    require(result.items.front().owners.contains(0) && result.after.lineSegments < result.before.lineSegments,
            "Foreground savings forced the simple background into a perforated contour");
    require(imageError(render(scene, {64, 64}), render(objects(result.items), {64, 64}))
        .value("rmse").toDouble() == 0, "Threatened-completion ordering changed appearance");
}

Result incremental(const QVector<gui::SvgVectorObject> &scene, Options options = {}) {
    options.strategy = "incremental";
    options.renderRmse = 0;
    options.renderMaxError = 0;
    options.checkpointEdits = 2;
    options.maximumChordSpans = 2;
    options.controlPointWeight = 1;
    Optimizer optimizer(makeItems(scene), options, {},
        [](const auto &objects) { return render(objects, {64, 64}); });
    auto result = optimizer.run();
    require(result.error.isEmpty(), qPrintable(result.error));
    require(result.graph.error.isEmpty(), "Incremental dependency graph is invalid");
    require(optimizer.validate(result.items).isEmpty(), "Cached incremental checks disagree with full geometry validation");
    require(optimizer.dependencies(result.items).edges == result.graph.edges,
            "Cached dependency rows disagree with complete ownership intersections");
    require(auditPenTargets(objects(result.items), options).value("passed").toBool(),
            "Incremental export contains an incompatible Bucket target");
    require(imageError(render(scene, {64, 64}), render(objects(result.items), {64, 64}))
        .value("rmse").toDouble() == 0, "Incremental edits changed the reference");
    require(result.silhouetteDifference <= options.silhouetteTolerance, "Incremental edits changed the silhouette");

    return result;
}

void incrementalEdits() {
    const auto zigzag = polygon({{4, 4}, {20, 4}, {28, 8}, {36, 4}, {44, 10}, {52, 4}, {56, 4},
        {56, 56}, {52, 56}, {44, 50}, {36, 56}, {28, 52}, {20, 56}, {4, 56},
        {4, 44}, {10, 40}, {4, 32}, {4, 20}, {10, 16}, {4, 8}});
    QPainterPath cover;
    cover.moveTo(24, 0); cover.lineTo(64, 0); cover.lineTo(64, 64); cover.lineTo(24, 64);
    for (int y = 60; y > 0; y -= 4) cover.lineTo(y % 8 ? 30 : 24, y);
    cover.closeSubpath();
    const QVector<gui::SvgVectorObject> scene{{zigzag, Qt::red}, {cover, Qt::blue}};
    const auto result = incremental(scene);
    require(result.accepted > 1 && result.after.lineSegments + 4 < result.before.lineSegments,
            "Hidden boundary changes did not accumulate");
    const auto again = incremental(scene);
    require(svg(objects(result.items), {64, 64}) == svg(objects(again.items), {64, 64}),
            "Incremental output is nondeterministic");

    QPainterPath curved;
    curved.moveTo(4, 4); curved.lineTo(20, 4);
    curved.cubicTo(50, 0, 50, 24, 20, 24);
    curved.lineTo(4, 24); curved.closeSubpath();
    const auto flattened = incremental({{curved, Qt::red}, {rect(20, 0, 44, 32), Qt::blue}});
    require(flattened.after.bezierSegments < flattened.before.bezierSegments,
            "Hidden cubic control points were not removed");

    auto ring = rect(0, 0, 28, 28);
    QPainterPath hole; hole.addEllipse(6.2, 6.2, 14.6, 14.6);
    ring.addPath(hole);
    const auto combined = incremental({{ring, Qt::red}, {hole, Qt::blue},
        {polygon({{34, 36}, {48, 36}, {54, 40}, {60, 36}, {62, 36}, {62, 60}, {54, 56}, {48, 60}, {34, 60}}), Qt::green},
        {rect(48, 32, 16, 32), Qt::yellow}});
    require(combined.accepted && combined.after.holes >= 1,
            "An invalid hole fill discarded independent valid shortcuts");

    auto perforated = rect(8, 8, 48, 48);
    perforated.addRect(24, 24, 16, 16);
    Options options;
    options.holeWeight = 4;
    const auto layered = incremental({{rect(0, 0, 64, 64), Qt::white}, {perforated, Qt::red}}, options);
    require(layered.accepted && layered.after.holes == 0 && layered.after.paths == 3,
            "An enclosed ownership component could not move to an upper layer");
    bool split = false;
    for (const auto &entry : layered.diagnostics) split |= entry.toObject().value("kind").toString().contains("occluder");
    require(split, "Component placement did not record the joint split transaction");

    const auto noOp = incremental({{rect(4, 4, 56, 56), Qt::red}});
    require(noOp.accepted == 0 && noOp.after.cost == noOp.before.cost, "Incremental no-op changed geometry");
}

void croppedSvgRender() {
    QPainterPath curved;
    curved.addEllipse(4.2, 3.7, 49.3, 55.8);
    const QVector<gui::SvgVectorObject> scene{{rect(0, 0, 64, 64), Qt::white}, {curved, Qt::red},
        {polygon({{18.3, 4.1}, {57.4, 42.8}, {24.2, 60.6}}), Qt::blue}};
    QSvgRenderer renderer(svg(scene, {64, 64}));
    QImage whole({64, 64}, QImage::Format_ARGB32_Premultiplied);
    whole.fill(Qt::transparent);
    { QPainter painter(&whole); painter.setRenderHint(QPainter::Antialiasing); renderer.render(&painter); }
    for (const QRect rectangle : {QRect(0, 0, 20, 20), QRect(17, 11, 32, 43), QRect(32, 32, 32, 32)})
        require(imageError(whole.copy(rectangle), renderCrop(scene, rectangle)).value("max_channel_error").toDouble() <= 1,
                "Pixel-aligned cropped SVG rendering differs from the full renderer");
}

void colorAbstraction() {
    Options options;
    options.strategy = "incremental";
    options.controlPointWeight = 1;
    options.holeWeight = 4;
    ColorOptions colors;
    const Renderer renderer = [](const auto &objects) { return render(objects, {64, 64}); };
    const auto run = [&](const QVector<gui::SvgVectorObject> &scene, ColorOptions settings) {
        const auto result = abstractColors(makeItems(scene), options, settings, renderer);
        require(result.report.value("after").toObject().value("cost").toDouble()
            <= result.report.value("before").toObject().value("cost").toDouble() + 1e-9,
            "Color abstraction increased encoded geometry cost");
        require(pathMetrics(objects(result.items), options.flattenTolerance).holes
            <= pathMetrics(scene, options.flattenTolerance).holes, "Color abstraction introduced holes");
        require(silhouetteDifference(scene, objects(result.items), options.flattenTolerance) <= options.silhouetteTolerance,
                "Color abstraction changed the external silhouette");
        const auto error = imageError(render(scene, {64, 64}), render(objects(result.items), {64, 64}));
        require(error.value("rmse").toDouble() <= settings.renderRmse
            && error.value("max_channel_error").toDouble() <= settings.renderMaxError,
            "Color abstraction exceeded its cumulative appearance limits");
        for (const auto &item : result.items) require(item.color.alpha() == 255, "Color abstraction emitted transparency");
        Options audit = options;
        audit.maximumPasses = 0;
        const auto prepared = preparePenTargets(result.items, audit, renderer);
        require(prepared.error.isEmpty(), qPrintable(prepared.error));
        Optimizer optimizer(prepared.items, audit, {}, renderer);
        require(optimizer.run().error.isEmpty(), "Abstracted paths failed the topology baseline audit");
        return result;
    };
    const QVector<gui::SvgVectorObject> shades{{rect(0, 0, 32, 64), QColor(230, 40, 30)},
        {rect(32, 0, 32, 64), QColor(233, 42, 31)}};
    const auto merged = run(shades, colors);
    require(merged.changed && pathMetrics(objects(merged.items), options.flattenTolerance).fills == 1,
        "Adjacent slight color differences retained separate appearance groups");
    require(incremental(objects(merged.items), options).items.size() == 1,
        "Same-fill source supports did not merge into actual simplified geometry");
    require(svg(objects(merged.items), {64, 64}) == svg(objects(run(shades, colors).items), {64, 64}),
            "Color abstraction changed across repeated runs");

    QPainterPath silhouette;
    silhouette.addEllipse(4, 4, 56, 56);
    const auto curved = run({{silhouette, QColor(180, 120, 60)},
        {rect(16, 14, 14, 36), QColor(183, 122, 62)}}, colors);
    require(curved.changed && curved.items.front().path.elementAt(1).type == QPainterPath::CurveToElement,
            "Color abstraction discarded the curved external silhouette");

    QPainterPath fragments;
    fragments.addRect(8.1, 10, 0.2, 40);
    fragments.addRect(48.1, 10, 0.2, 40);
    const auto absorbed = run({{rect(0, 0, 32, 64), QColor(160, 160, 160)},
        {rect(32, 0, 32, 64), QColor(60, 60, 60)}, {fragments, QColor(110, 110, 110)}}, colors);
    require(appearanceError(render({{rect(0, 0, 32, 64), QColor(160, 160, 160)},
        {rect(32, 0, 32, 64), QColor(60, 60, 60)}, {fragments, QColor(110, 110, 110)}}, {64, 64}),
        render(objects(absorbed.items), {64, 64}), 16).value("max_local_rmse").toDouble() <= colors.localRmse,
        "Subpixel color merging exceeded its actual native pixel error bound");
    const auto subpixelLine = run({{rect(0, 0, 64, 64), Qt::white},
        {rect(16.1, 16, 0.2, 32), Qt::black}}, colors);
    require(subpixelLine.items.size() == 2, "A strongly contrasting subpixel line lost its boundary protection");

    const auto insignificant = run({{rect(0, 0, 64, 64), Qt::white}, {rect(16, 16, 1, 4), Qt::black}}, colors);
    require(insignificant.items.size() == 2
        && insignificant.report.value("discarded_low_impact_components").toInt() == 0,
        "A small contrasting contour was deleted to improve the count");
    const auto coherent = run({{rect(0, 0, 64, 64), Qt::white}, {rect(16, 16, 1, 32), Qt::black}}, colors);
    require(coherent.items.size() == 2, "A coherent contrasting line was discarded by a width rule");
    const auto invisible = run({{rect(0, 0, 64, 64), Qt::white},
        {rect(16.1, 16.1, 0.01, 0.01), Qt::black}}, colors);
    require(invisible.items.size() == 1 && invisible.report.value("color_guards").toObject()
        .value("max_coverage_weighted_color_drift").toDouble() <= colors.tolerance,
        "An invisible fragment could not merge despite bounded native pixel impact");
    run({{rect(0, 0, 64, 64), Qt::white}, {rect(-12, -12, 1, 4), Qt::black}}, colors);

    auto frame = rect(8, 8, 48, 48);
    frame.addRect(22, 22, 20, 20);
    const auto layered = incremental({{rect(0, 0, 64, 64), Qt::white}, {frame, Qt::red}}, options);
    require(layered.accepted && layered.items.size() == 3
        && pathMetrics(objects(layered.items), options.flattenTolerance).holes == 0,
        "Enclosed same-color components did not move above the completed foreground");

    const auto nested = incremental({{rect(0, 0, 64, 64), Qt::white},
        {rect(4, 4, 56, 56), Qt::red}, {frame, Qt::blue}}, options);
    require(nested.after.holes == 0 && nested.items.size() == 4,
        "A non-dominant color could not move an enclosed patch to a later position");

    auto perforated = rect(4, 4, 56, 56);
    for (int i = 0; i < 10; ++i) perforated.addRect(8 + (i % 5) * 9, 12 + (i / 5) * 24, 5, 12);
    Options enclosedOptions = options;
    enclosedOptions.componentLimit = 1;
    const auto enclosed = incremental({{rect(0, 0, 64, 64), Qt::white}, {perforated, Qt::blue}}, enclosedOptions);
    require(enclosed.after.holes == 0 && enclosed.after.cost < enclosed.before.cost,
        "Enclosed hole restoration was restricted by the general component-split search limit");

    const auto reconstructed = run({{rect(0, 0, 64, 64), Qt::white},
        {rect(4, 4, 56, 56), QColor(180, 20, 20)}, {frame, Qt::blue},
        {rect(24, 24, 8, 8), QColor(183, 22, 21)}}, colors);
    require(reconstructed.report.value("reconstruction").toObject().value("accepted").toBool()
        && pathMetrics(objects(reconstructed.items), options.flattenTolerance).holes == 0,
        "Color reconstruction did not preserve separate positions for a non-dominant enclosed color");

    QVector<gui::SvgVectorObject> tones;
    for (int i = 0; i < 16; ++i) tones.push_back({rect(i * 4, 0, 4, 64), QColor(40 + i * 12, 40 + i * 12, 40 + i * 12)});
    const auto bounded = run(tones, colors);
    const auto gradient = renderer(objects(bounded.items));
    require(gradient.pixelColor(2, 32).red() + 120 < gradient.pixelColor(62, 32).red(),
        "Repeated nearby-color merges erased a broad tonal structure");

    ColorOptions strict = colors;
    strict.renderRmse = strict.renderMaxError = 0;
    require(!run(shades, strict).changed, "Failed abstraction did not retain the unchanged baseline");
    colors.tolerance = 0;
    const auto noOp = run(shades, colors);
    require(!noOp.changed && svg(objects(noOp.items), {64, 64}) == svg(shades, {64, 64}),
            "Disabled color abstraction changed the source paths");

    colors.tolerance = 5;
    colors.detailScale = 0;
    const QVector<gui::SvgVectorObject> contrasting{{rect(0, 0, 32, 64), Qt::red},
        {rect(32, 0, 32, 64), Qt::green}};
    require(!run(contrasting, colors).changed, "Distinct broad hues were merged");
}

void localAppearance() {
    const auto target = render({{rect(0, 0, 64, 64), Qt::white}, {rect(15, 15, 8, 8), Qt::black}}, {64, 64});
    const auto blank = render({{rect(0, 0, 64, 64), Qt::white}}, {64, 64});
    const auto error = appearanceError(target, blank, 16);
    require(error.value("rmse").toDouble() < 40 && error.value("max_local_rmse").toDouble() > 120,
        "Local appearance error averaged a concentrated detail loss over the whole image");
    require(appearanceError(target, target, 128).value("max_local_rmse").toDouble() == 0,
        "A window larger than the image changed the identity error");
    ColorOptions colors;
    colors.tolerance = 100;
    colors.renderRmse = colors.renderMaxError = 255;
    colors.localRmse = 2;
    colors.contrastRetention = 0;
    Options options;
    const auto result = abstractColors(makeItems({{rect(0, 0, 64, 64), Qt::white}, {rect(15, 15, 8, 8), Qt::black}}),
        options, colors, [](const auto &objects) { return render(objects, {64, 64}); });
    const auto after = render(objects(result.items), {64, 64});
    require(appearanceError(target, after, 16).value("max_local_rmse").toDouble() <= colors.localRmse
        && after.pixelColor(18, 18) == QColor(Qt::black), "Local detail guards accepted a destructive merge");
    require(result.report.value("attempts").toArray().at(0).toObject().value("rejected").toObject()
        .value("Original regional RGB error").toInt() > 0, "Regional color error did not reject an otherwise eligible merge");
}

void originalAppearanceGuard() {
    QPainterPath hole;
    hole.addEllipse(10.2, 10.2, 43.6, 43.6);
    auto ring = rect(0, 0, 64, 64);
    ring.addPath(hole);
    const QVector<gui::SvgVectorObject> scene{{ring, Qt::red}, {hole, Qt::blue}};
    const Renderer renderer = [](const auto &objects) { return render(objects, {64, 64}); };
    Options options;
    options.strategy = "incremental";
    options.maximumPasses = 1;
    options.maximumChordSpans = 0;
    options.renderRmse = options.renderMaxError = 255;
    ColorOptions strict;
    strict.renderRmse = strict.renderMaxError = strict.localRmse = 0;
    Optimizer optimizer(makeItems(scene), options, {}, renderer);
    optimizer.setAppearanceGuard(renderer(scene), strict);
    const auto result = optimizer.run();
    require(result.error.isEmpty() && result.after.holes == 1
        && imageError(renderer(scene), renderer(objects(result.items))).value("rmse").toDouble() == 0,
        "A geometry edit exceeded its original-input appearance guard");
    Optimizer invalid(makeItems({{rect(0, 0, 64, 64), Qt::white}}), options, {}, renderer);
    invalid.setAppearanceGuard(renderer(scene), strict);
    require(invalid.run().error.contains("Baseline appearance"), "An invalid appearance baseline bypassed the original-input guard");
}

void localSourceRestoration() {
    QVector<gui::SvgVectorObject> scene{{rect(0, 0, 128, 128), Qt::white},
        {rect(48, 48, 32, 32), Qt::black}, {rect(48, 48, 16.5, 32), Qt::white},
        {rect(64.5, 48, 16, 32), QColor(254, 254, 254)}};
    for (int i = 0; i < 20; ++i) scene.push_back({rect(4, 4 + i * 5, 8, 3), Qt::white});
    Options options;
    options.controlPointWeight = 1;
    options.holeWeight = 4;
    ColorOptions colors;
    const Renderer renderer = [](const auto &objects) { return render(objects, {128, 128}); };
    const auto result = abstractColors(makeItems(scene), options, colors, renderer);
    const auto reconstruction = result.report.value("reconstruction").toObject();
    require(reconstruction.value("accepted").toBool() && reconstruction.value("restored_source_paths").toInt() > 0,
        "Local source restoration did not retain antialiased detail absent from geometric ownership");
    require(result.items.size() < scene.size() && pathMetrics(objects(result.items), .05).holes == 0,
        "Local source restoration lost the geometry saving or introduced holes");
    require(appearanceError(renderer(scene), renderer(objects(result.items)), colors.localWindow)
        .value("max_local_rmse").toDouble() <= colors.localRmse, "Restored detail exceeded the native local error budget");
}

void partialColorGrouping() {
    const QVector<gui::SvgVectorObject> scene{{rect(4.25, 8, 8.5, 40), Qt::red},
        {rect(20.25, 8, 8.5, 40), Qt::red}, {rect(20.25, 8, 8.5, 40), Qt::red},
        {rect(36.25, 8, 8.5, 40), Qt::red}};
    Options options;
    options.strategy = "incremental";
    options.maximumPasses = 1;
    options.maximumChordSpans = 0;
    options.groupNeighbors = 0;
    options.componentLimit = 1;
    options.renderRmse = options.renderMaxError = 0;
    const Renderer renderer = [](const auto &objects) { return render(objects, {64, 64}); };
    const auto run = [&]() { return Optimizer(makeItems(scene), options, {}, renderer).run(); };
    const auto result = run();
    require(result.error.isEmpty() && result.after.paths == 4,
        "Disconnected contours were counted as a single Bucket target");
    require(imageError(renderer(scene), renderer(objects(result.items))).value("rmse").toDouble() == 0,
        "Partial grouping changed cumulative boundary coverage");
    require(svg(objects(result.items), {64, 64}) == svg(objects(run().items), {64, 64}),
        "Partial grouping changed across repeated runs");
}

void bucketTargets() {
    Options options;
    options.strategy = "incremental";
    options.renderRmse = 4;
    options.renderMaxError = 96;
    const Renderer renderer = [](const auto &items) { return renderCrop(items, {0, 0, 64, 64}); };
    auto disconnected = rect(4, 4, 12, 40);
    disconnected.addPath(rect(40, 4, 12, 40));
    auto nested = rect(4, 4, 48, 48);
    nested.setFillRule(Qt::WindingFill);
    nested.addPath(rect(16, 16, 24, 24));
    const auto crossing = polygon({{4, 4}, {52, 52}, {4, 52}, {52, 4}});
    const auto thin = rect(4, 4, 48, .00001);
    for (const auto &path : {disconnected, nested, crossing}) {
        const QVector<gui::SvgVectorObject> scene{{path, Qt::red}};
        require(!penTargetError(path, options).isEmpty(), "Bucket audit missed incompatible input geometry");
        const auto prepared = preparePenTargets(makeItems(scene), options, renderer);
        require(prepared.error.isEmpty() && prepared.changed, qPrintable(prepared.error));
        require(prepared.report.value("after").toObject().value("passed").toBool(),
            "Decomposition retained an incompatible Bucket contour");
        require(silhouetteDifference(scene, objects(prepared.items), options.flattenTolerance) <= options.silhouetteTolerance,
            "Bucket decomposition changed filled geometry");
        const auto encoded = svg(objects(prepared.items), {64, 64});
        const auto exported = gui::extractSvgVectorObjects(encoded, {64, 64});
        require(auditPenTargets(exported.objects, options).value("passed").toBool(),
            "Serialized Bucket targets failed native conversion");
        require(encoded == svg(objects(preparePenTargets(makeItems(scene), options, renderer).items), {64, 64}),
            "Bucket decomposition is nondeterministic");
    }
    const auto prepared = preparePenTargets(makeItems({{disconnected, Qt::red}}), options, renderer);
    require(prepared.items.size() == 2, "Independent outer loops have an incorrect target count");
    const auto corrected = preparePenTargets(makeItems({{nested, Qt::red}}), options, renderer);
    require(corrected.items.size() == 1 && pathMetrics(objects(corrected.items), .05).holes == 0,
        "Nonzero fill was silently converted into a Pen cutout");
    const auto thinTarget = preparePenTargets(makeItems({{thin, Qt::red}}), options, renderer);
    require(thinTarget.error.isEmpty() && !thinTarget.changed && thinTarget.items.size() == 1,
        "Thin valid geometry was rejected or split by the native crossing check");
    const auto spike = polygon({{52, 52}, {4, 52}, {4, 4}, {24, 4.1}, {28, 4.11999999}, {4, 4}, {52, 4}});
    const auto cleaned = preparePenTargets(makeItems({{spike, Qt::red}}), options, renderer);
    require(cleaned.error.isEmpty() && cleaned.items.size() == 1
        && cleaned.report.value("discarded_unconvertible_area").toDouble() <= options.areaTolerance,
        "A repeated-vertex excursion forced excessive native decomposition");
    auto narrowRing = rect(4, 4, 48, 48);
    narrowRing.addPath(rect(4.00001, 4.00001, 47.99998, 47.99998));
    require(penTargetError(narrowRing, options).isEmpty(), "Separate narrow cutout boundaries were reported as crossing");
    auto touchingRing = rect(4, 4, 48, 48);
    touchingRing.addPath(rect(4, 8, 20, 20));
    require(!penTargetError(touchingRing, options).isEmpty(), "A cutout touching the outer boundary was accepted");
    Options noEdits = options;
    noEdits.maximumPasses = 0;
    require(!Optimizer(makeItems({{disconnected, Qt::red}}), noEdits, {}, renderer).run().error.isEmpty(),
        "The no-op path bypassed Bucket baseline validation");
    QPainterPath curve;
    curve.addEllipse(8.2, 8.2, 40, 40);
    const auto unchanged = preparePenTargets(makeItems({{curve, Qt::red}}), options, renderer);
    require(unchanged.error.isEmpty() && !unchanged.changed
        && svg(objects(unchanged.items), {64, 64}) == svg({{curve, Qt::red}}, {64, 64}),
        "A valid curved target lost its original spans");
}

void connectedSupportRecovery() {
    QPainterPath ellipse;
    ellipse.addEllipse(8.123456789, 8.234567891, 48.345678912, 40.456789123);
    const auto curvedMerge = incremental({{ellipse, Qt::red}, {rect(20, 20, 10, 10), Qt::red}});
    require(curvedMerge.after.paths == 1 && curvedMerge.after.bezierSegments == 4 && curvedMerge.after.lineSegments == 0,
        "A Boolean merge split or flattened unchanged source Beziers");
    const QVector<gui::SvgVectorObject> scene{{rect(4, 4, 12, 28), Qt::red},
        {rect(40, 4, 12, 28), Qt::red}, {rect(4, 32, 48, 16), Qt::blue}};
    const auto result = incremental(scene);
    require(result.after.paths == 2 && result.after.components == 2 && result.after.holes == 0,
        "Hidden support outside the visible bounding box did not connect same-fill islands");
    require(result.after.lineSegments == 12 && result.graph.edges.size() == 1,
        "Connected support recovery did not retain the silhouette and cover dependency");
    bool recovered = false;
    for (const auto &entry : result.diagnostics)
        recovered |= entry.toObject().value("kind").toString() == "group-domain-support";
    require(recovered, "Domain-constrained support recovery was not exercised");

    QVector<gui::SvgVectorObject> interleaved;
    const QVector<QColor> covers{Qt::blue, Qt::green, Qt::yellow};
    for (int index = 0; index < 4; ++index) {
        interleaved.push_back({rect(4 + index * 12, 4, 6, 48), Qt::red});
        if (index < 3) interleaved.push_back({rect(10 + index * 12, 4, 10, 48), covers[index]});
    }
    Options options;
    options.maximumPasses = 1;
    options.groupNeighbors = 0;
    const auto rebuilt = incremental(interleaved, options);
    require(rebuilt.after.paths == 4 && rebuilt.after.components == 4 && rebuilt.after.holes == 0,
        "Multiple blocking supports could not be trimmed in one grouping transaction");
    bool trimmed = false;
    for (const auto &entry : rebuilt.diagnostics)
        trimmed |= entry.toObject().value("kind").toString().contains("trim-occluders");
    require(trimmed, "Joint support and order optimization was not exercised");
}

void globalComponentPlacement() {
    auto frame = rect(4, 4, 56, 56);
    frame.addPath(rect(24, 24, 16, 16));
    const QVector<gui::SvgVectorObject> scene{{rect(0, 0, 64, 64), Qt::white}, {frame, Qt::red}};
    Options options;
    options.strategy = "savings";
    options.maximumChordSpans = 0;
    options.lookaheadWeight = 2;
    options.holeWeight = 4;
    options.renderRmse = options.renderMaxError = 0;
    const Renderer renderer = [](const auto &items) { return render(items, {64, 64}); };
    Optimizer optimizer(makeItems(scene), options, {}, renderer);
    require(optimizer.baselineItems().first().owners.size() == 2,
        "Global planning still treats separated visible components as one owner");
    const auto result = optimizer.run();
    require(result.error.isEmpty() && result.accepted && result.after.paths == 3 && result.after.holes == 0,
        "Global planning could not place source components at separate depths");
    require(result.after.cost < result.before.cost && optimizer.validate(result.items).isEmpty(),
        "Split global ownership increased cost or broke fixed ownership");
    require(imageError(renderer(scene), renderer(objects(result.items))).value("rmse").toDouble() == 0,
        "Global component placement changed opaque appearance");
    require(auditPenTargets(objects(result.items), options).value("passed").toBool(),
        "Global component placement produced an invalid Bucket target");

    auto perforated = rect(0, 0, 64, 64);
    for (const QPointF point : {QPointF(16, 24), QPointF(22, 24), QPointF(16, 36), QPointF(22, 36)})
        perforated.addPath(rect(point.x(), point.y(), 2, 2));
    const QVector<gui::SvgVectorObject> touching{{perforated, Qt::white},
        {polygon({{0, 32}, {20, 12}, {40, 32}, {20, 52}}), Qt::red}};
    options.strategy = "difficulty";
    options.renderRmse = options.renderMaxError = 255;
    Optimizer junctions(makeItems(touching), options, {}, renderer);
    const auto repaired = junctions.run();
    require(repaired.error.isEmpty() && repaired.accepted && repaired.after.cost < repaired.before.cost,
        "Point-touching ownership had no legal native completion");
    require(auditPenTargets(objects(repaired.items), options).value("passed").toBool(),
        "A repaired ownership junction still fails native conversion");
    bool usedRepair = false;
    for (const auto &step : repaired.diagnostics)
        usedRepair |= step.toObject().value("kind").toString().contains("junction-completion");
    require(usedRepair, "The point-touching ownership repair was not exercised");
    options.renderRmse = options.renderMaxError = 0;
    const auto strict = Optimizer(makeItems(touching), options, {}, renderer).run();
    require(strict.error.isEmpty() && strict.accepted == 0
        && svg(objects(strict.items), {64, 64}) == svg(touching, {64, 64}),
        "A junction repair bypassed the complete rendering budget");
}

void colorSupportConstruction() {
    Options options;
    options.strategy = "supports";
    options.holeWeight = 4;
    options.maximumPasses = 2;
    options.renderRmse = options.renderMaxError = 0;
    const Renderer renderer = [](const auto &items) { return render(items, {64, 64}); };
    const auto run = [&](const QVector<gui::SvgVectorObject> &scene) {
        auto result = Optimizer(makeItems(scene), options, {}, renderer).run();
        require(result.error.isEmpty(), qPrintable(result.error));
        require(result.after.components == result.after.paths, "Support cost counted disconnected objects as one target");
        require(auditPenTargets(objects(result.items), options).value("passed").toBool(),
            "A color support is not a usable Bucket target");
        require(imageError(renderer(scene), renderer(objects(result.items))).value("rmse").toDouble() == 0,
            "Color support construction changed the final visible image");
        return result;
    };
    const QVector<gui::SvgVectorObject> islands{{rect(4, 4, 12, 28), Qt::red},
        {rect(40, 4, 12, 28), Qt::red}, {rect(4, 32, 48, 16), Qt::blue}};
    const auto joined = run(islands);
    require(joined.accepted && joined.after.paths == 2 && joined.after.singlePathFills == 2,
        "Color-wide supports did not produce one connected target per color");
    require(joined.after.lineSegments == 12 && joined.after.holes == 0,
        "Hidden color bridges failed to simplify the complete support geometry");
    require(svg(objects(joined.items), {64, 64}) == svg(objects(run(islands).items), {64, 64}),
        "Color-wide construction changed between identical runs");
    auto ring = rect(4, 4, 56, 56);
    ring.addPath(rect(24, 24, 16, 16));
    const auto repaint = run({{rect(0, 0, 64, 64), Qt::white}, {ring, Qt::red}});
    require(repaint.accepted && repaint.after.paths == 3 && repaint.after.holes == 0,
        "A cutout was not filled with its original contents repainted above");
    require(repaint.after.cost < repaint.before.cost && repaint.after.maximumPathsPerFill == 2,
        "Hole removal failed to account for the extra color position");
    const auto transparentHole = run({{ring, Qt::red}});
    require(!transparentHole.accepted && transparentHole.after.holes == 1,
        "An external transparent hole was painted over");
    QPainterPath ellipse;
    ellipse.addEllipse(8.123456789, 8.234567891, 48.345678912, 40.456789123);
    const auto curved = run({{ellipse, Qt::red}, {rect(20, 20, 10, 10), Qt::red}});
    require(curved.after.paths == 1 && curved.after.bezierSegments == 4 && curved.after.lineSegments == 0,
        "Color support reconstruction flattened unchanged visible Beziers");
    const auto separate = run({{rect(0, 0, 32, 64), QColor(255, 0, 0)},
        {rect(32, 0, 32, 64), QColor(254, 0, 0)}});
    require(separate.after.fills == 2, "Exact color support construction changed the palette");
    const auto narrowGap = run({{rect(0, 0, 64, 16), Qt::red}, {rect(0, 48, 64, 16), Qt::red},
        {rect(0, 16, 32, 32), Qt::red}, {rect(32.00000002, 16, 31.99999998, 32), Qt::red}});
    require(narrowGap.accepted && narrowGap.after.paths == 1 && narrowGap.after.holes == 0
        && narrowGap.silhouetteDifference <= options.silhouetteTolerance,
        "Sub-tolerance domain cracks were preserved as structural cutouts");
    QVector<gui::SvgVectorObject> interleaved{{rect(0, 0, 64, 64), Qt::white}};
    const QVector<QColor> palette{Qt::red, Qt::blue, Qt::green, Qt::white};
    for (int row = 0; row < 6; ++row)
        for (int column = 0; column < 6; ++column)
            interleaved.push_back({rect(8 + column * 8, 8 + row * 8, 8, 8),
                palette[(row * row + column * 3 + row * column) % palette.size()]});
    const auto fallback = run(interleaved);
    bool validatedRepaint = false;
    for (const auto &entry : fallback.strategies) {
        const auto trial = entry.toObject();
        require(!trial.value("error").toString().contains("No opaque completion"),
            "Hole completion rejected a feasible exact visible layer");
        validatedRepaint |= trial.value("strategy").toString().startsWith("repaint-")
            && trial.value("error").toString().isEmpty();
    }
    require(validatedRepaint, "No exact-ownership repaint ordering remained valid");
    options.pathWeight = 8;
    const auto cover = polygon({{16, 16}, {20, 16}, {20, 18}, {24, 18}, {24, 16}, {28, 16},
        {28, 18}, {32, 18}, {32, 16}, {36, 16}, {36, 18}, {40, 18}, {40, 16}, {44, 16},
        {44, 24}, {40, 24}, {40, 22}, {36, 22}, {36, 24}, {32, 24}, {32, 22}, {28, 22},
        {28, 24}, {24, 24}, {24, 22}, {20, 22}, {20, 24}, {16, 24}});
    const auto sparse = run({{rect(4, 12, 12, 16), Qt::red}, {rect(44, 12, 12, 16), Qt::red}, {cover, Qt::blue}});
    require(sparse.after.paths == 2 && sparse.after.cost < sparse.before.cost,
        "Sparse hidden bridges could not connect same-fill islands beneath a complex cover");
    const auto red = std::find_if(sparse.items.begin(), sparse.items.end(), [](const Item &item) { return item.color == Qt::red; });
    require(red != sparse.items.end() && red->path.contains(QPointF(30, 20)),
        "The connected target has no actual hidden bridge");
    options.pathWeight = 1;
    options.maximumPasses = 0;
    const auto noOp = run(islands);
    require(noOp.accepted == 0 && svg(objects(noOp.items), {64, 64}) == svg(islands, {64, 64}),
        "A zero-pass support run changed its baseline");
    auto disconnected = rect(0, 0, 8, 8);
    disconnected.addPath(rect(16, 16, 8, 8));
    require(!Optimizer(makeItems({{disconnected, Qt::red}}), options, {}, renderer).run().error.isEmpty(),
        "A zero-pass color-support run accepted an incompatible Bucket baseline");
}

void localColorAbsorption() {
    Options options;
    options.strategy = "incremental";
    options.maximumPasses = 3;
    options.renderRmse = 64;
    options.renderMaxError = 255;
    options.checkpointEdits = 1;
    ColorOptions limits;
    limits.tolerance = 100;
    limits.renderRmse = 64;
    limits.renderMaxError = 255;
    limits.localRmse = 64;
    const Renderer renderer = [](const auto &scene) { return render(scene, {64, 64}); };
    const QColor gray(120, 120, 120);
    const QVector<gui::SvgVectorObject> target{{rect(0, 0, 64, 64), gray}, {rect(32, 8, 1, 40), Qt::black}};
    auto noisy = target;
    noisy.push_back({rect(8, 8, 2, 2), QColor(126, 126, 126)});
    noisy.push_back({rect(16, 8, 2, 2), QColor(132, 132, 132)});
    const auto run = [&](const auto &scene, const QImage &source, double growth) {
        Optimizer optimizer(makeItems(scene), options, {}, renderer);
        optimizer.setAppearanceGuard(renderer(scene), limits);
        optimizer.setColorAbsorption(source, limits, growth, 24);
        return optimizer.run();
    };
    const auto cleaned = run(noisy, renderer(target), 100);
    require(cleaned.error.isEmpty(), qPrintable(cleaned.error));
    require(cleaned.accepted >= 2 && cleaned.after.paths == 2 && cleaned.after.fills == 2,
        "Local absorption did not remove different-color artifacts and their geometry");
    require(imageError(renderer(target), renderer(objects(cleaned.items))).value("rmse").toDouble() == 0,
        "Source-supported thin detail was lost while absorbing color fragments");
    require(auditPenTargets(objects(cleaned.items), options).value("passed").toBool()
        && cleaned.silhouetteDifference <= options.silhouetteTolerance, "Absorption exported invalid geometry");
    const auto visibleBefore = absorptionRegionMetrics(cleaned.ownershipBefore, options.flattenTolerance);
    const auto visibleAfter = absorptionRegionMetrics(cleaned.ownershipAfter, options.flattenTolerance);
    require(visibleBefore.value("components").toInt() == 4 && visibleAfter.value("components").toInt() == 2
        && visibleAfter.value("small_region_histogram").toArray().at(2).toObject().value("components").toInt() == 0,
        "Visible color fragments were confused with draw supports in absorption diagnostics");
    const auto rejections = cleaned.diagnostics.at(0).toObject().value("rejected").toObject();
    require(rejections.value("Source edge or local added-error budget exceeded").toInt() > 0,
        "The source edge guard was not exercised");
    require(svg(objects(cleaned.items), {64, 64}) == svg(objects(run(noisy, renderer(target), 100).items), {64, 64}),
        "Local color absorption changed between deterministic runs");
    const auto nonWorsening = run(noisy, renderer(target), 0);
    require(nonWorsening.accepted >= 2
        && imageError(renderer(target), renderer(objects(nonWorsening.items))).value("rmse").toDouble() == 0,
        "Non-worsening source validation blocked an actual reconstruction improvement");
    auto repeatedShade = target;
    repeatedShade.push_back({rect(8, 8, 2, 2), QColor(126, 126, 126)});
    repeatedShade.push_back({rect(40, 8, 2, 2), QColor(126, 126, 126)});
    const auto classMerged = run(repeatedShade, renderer(target), 0);
    bool classTransaction = false;
    for (const auto &value : classMerged.diagnostics) {
        const auto entry = value.toObject();
        if (entry.value("kind") != "color-class-support-group") continue;
        const auto removed = entry.value("removed_fills").toArray();
        classTransaction |= std::count(removed.cbegin(), removed.cend(), QJsonValue(QColor(126, 126, 126).name())) == 2
            && entry.value("paths_before").toInt() == 4 && entry.value("paths_after").toInt() == 2;
    }
    require(classTransaction && classMerged.after.paths == 2 && classMerged.after.fills == 2
        && imageError(renderer(target), renderer(objects(classMerged.items))).value("rmse").toDouble() == 0,
        "A joint palette-class substitution did not include separated same-color items and real geometry savings");
    const QVector<gui::SvgVectorObject> layeredTarget{{rect(0, 0, 64, 64), gray},
        {rect(16, 16, 32, 32), Qt::black}, {rect(24, 24, 4, 4), gray}};
    auto layered = layeredTarget;
    layered[2].color = QColor(126, 126, 126);
    layered.push_back({rect(8, 8, 2, 2), QColor(126, 126, 126)});
    const auto separatePositions = run(layered, renderer(layeredTarget), 0);
    require(separatePositions.error.isEmpty() && separatePositions.after.paths == 3 && separatePositions.after.fills == 2
        && imageError(renderer(layeredTarget), renderer(objects(separatePositions.items))).value("rmse").toDouble() == 0,
        "Class support grouping lost a required separate layer position");
    require(separatePositions.diagnostics.at(0).toObject().value("rejected").toObject().value("Dependency cycle").toInt() > 0,
        "The incompatible class support did not exercise dependency-cycle rejection");
    const QColor darkRed(120, 0, 0);
    const QVector<gui::SvgVectorObject> completedTarget{{rect(0, 0, 64, 64), Qt::white},
        {rect(4, 4, 56, 56), darkRed}, {rect(32, 32, 28, 28), Qt::black}};
    const QVector<gui::SvgVectorObject> incomplete{{rect(0, 0, 64, 64), Qt::white},
        {rect(4, 4, 56, 28), darkRed}, {rect(4, 32, 28, 28), QColor(124, 0, 0)},
        {rect(32, 32, 28, 28), Qt::black}};
    const auto completed = run(incomplete, renderer(completedTarget), 0);
    require(completed.error.isEmpty() && completed.after.paths == 3
        && completed.after.lineSegments == 12 && completed.after.bezierSegments == 0
        && imageError(renderer(completedTarget), renderer(objects(completed.items))).value("rmse").toDouble() == 0,
        "Encoded savings did not prefer a hidden rectangle completion over a more complex union");
    const auto ring = rect(8, 8, 48, 48).subtracted(rect(24, 24, 8, 8));
    const QVector<gui::SvgVectorObject> falseHole{{rect(0, 0, 64, 64), Qt::white}, {ring, darkRed}};
    const QVector<gui::SvgVectorObject> solidTarget{{rect(0, 0, 64, 64), Qt::white}, {rect(8, 8, 48, 48), darkRed}};
    const auto filledComponent = run(falseHole, renderer(solidTarget), 0);
    require(filledComponent.error.isEmpty() && filledComponent.after.paths == 2 && filledComponent.after.holes == 0
        && imageError(renderer(solidTarget), renderer(objects(filledComponent.items))).value("rmse").toDouble() == 0,
        "A false hole could not transfer out of a background item's disconnected ownership");
    require(std::any_of(filledComponent.diagnostics.begin(), filledComponent.diagnostics.end(), [](const auto &value) {
        return value.toObject().value("kind") == "component-absorption-group";
    }), "Fixed-component absorption was not exercised");
    const auto trueHole = run(falseHole, renderer(falseHole), 0);
    require(trueHole.error.isEmpty() && trueHole.after.holes == 1
        && imageError(renderer(falseHole), renderer(objects(trueHole.items))).value("rmse").toDouble() == 0,
        "A source-supported hole was incorrectly absorbed into its neighbor");
    const auto twoHoles = rect(8, 8, 48, 48).subtracted(rect(20, 20, 8, 8)).subtracted(rect(36, 36, 8, 8));
    const auto retainedHole = rect(8, 8, 48, 48).subtracted(rect(36, 36, 8, 8));
    const QVector<gui::SvgVectorObject> mixedHoles{{rect(0, 0, 64, 64), Qt::white}, {twoHoles, darkRed}};
    const QVector<gui::SvgVectorObject> restoredTarget{{rect(0, 0, 64, 64), Qt::white}, {retainedHole, darkRed}};
    const double previousHoleWeight = options.holeWeight;
    options.holeWeight = 4;
    const auto restored = run(mixedHoles, renderer(restoredTarget), 0);
    options.holeWeight = previousHoleWeight;
    require(restored.error.isEmpty() && restored.after.paths == 3 && restored.after.holes == 0
        && restored.after.lineSegments == 12
        && imageError(renderer(restoredTarget), renderer(objects(restored.items))).value("rmse").toDouble() == 0,
        "A retained cutout did not become a separate opaque upper restoration");
    const QColor blue(0, 0, 120);
    const QVector<gui::SvgVectorObject> separated{{rect(0, 0, 64, 64), Qt::white},
        {rect(8, 8, 22, 48), darkRed}, {rect(34, 8, 22, 48), darkRed},
        {rect(30, 8, 4, 4), blue}, {rect(30, 52, 4, 4), blue}};
    const QVector<gui::SvgVectorObject> connectedTarget{{rect(0, 0, 64, 64), Qt::white},
        {rect(8, 8, 48, 48), darkRed}, {rect(30, 8, 4, 4), blue}, {rect(30, 52, 4, 4), blue}};
    const auto connected = run(separated, renderer(connectedTarget), 0);
    require(connected.error.isEmpty() && connected.after.paths == 4 && connected.after.lineSegments == 16
        && imageError(renderer(connectedTarget), renderer(objects(connected.items))).value("rmse").toDouble() == 0,
        "A recolored ownership component did not join compatible neighboring supports");
    const QVector<gui::SvgVectorObject> paletteScene{{rect(0, 0, 32, 64), QColor(100, 100, 100)},
        {rect(32, 0, 30, 64), QColor(104, 104, 104)}, {rect(62, 0, 1, 64), Qt::black},
        {rect(63, 0, 1, 64), gray}};
    const QVector<gui::SvgVectorObject> paletteTarget{{rect(0, 0, 64, 64), gray}, {rect(62, 0, 1, 64), Qt::black}};
    const auto sharedPalette = run(paletteScene, renderer(paletteTarget), 0);
    require(sharedPalette.error.isEmpty()
        && imageError(renderer(paletteTarget), renderer(objects(sharedPalette.items))).value("rmse").toDouble() == 0,
        "Local geometry could not reuse a better source-fitting color from the existing palette");
    bool borrowed = false;
    for (const auto &value : sharedPalette.diagnostics) {
        const auto entry = value.toObject();
        if (!entry.contains("source_rmse_after")) continue;
        require(entry.value("source_rmse_after").toDouble() <= entry.value("source_rmse_before").toDouble() + 1e-9,
            "A local absorption transaction spent an earlier source-quality improvement");
        const auto removed = entry.value("removed_fills").toArray();
        const auto added = entry.value("added_fills").toArray();
        borrowed |= !removed.contains(gray.name()) && added.contains(gray.name());
    }
    require(borrowed, "The source-aware shared palette was not exercised");
    limits.localRmse = 0;
    const auto sourceCorrection = run(paletteScene, renderer(paletteTarget), 0);
    require(sourceCorrection.error.isEmpty() && sourceCorrection.after.paths == 3
        && imageError(renderer(paletteTarget), renderer(objects(sourceCorrection.items))).value("rmse").toDouble() == 0,
        "Guide-local error blocked an exact source correction with no added source damage");
    options.renderRmse = limits.renderRmse = 2.5;
    options.renderMaxError = limits.renderMaxError = limits.localRmse = 8;
    const QVector<gui::SvgVectorObject> ramp{{rect(0, 0, 21, 64), gray},
        {rect(21, 0, 22, 64), QColor(124, 124, 124)}, {rect(43, 0, 21, 64), QColor(128, 128, 128)}};
    const auto bounded = run(ramp, renderer(ramp), 100);
    require(bounded.error.isEmpty() && bounded.accepted > 0 && bounded.after.paths == 2,
        "Cumulative color error did not stop transitive shade absorption");
    require(imageError(renderer(ramp), renderer(objects(bounded.items))).value("rmse").toDouble() <= 2.5,
        "Accepted color merges drifted beyond the fixed original guide");
    QPainterPath ellipse;
    ellipse.addEllipse(4, 4, 56, 56);
    const QVector<gui::SvgVectorObject> curved{{ellipse, gray}, {rect(16, 16, 2, 2), QColor(126, 126, 126)}};
    const auto smooth = run(curved, renderer({{ellipse, gray}}), 0);
    require(smooth.error.isEmpty() && smooth.after.paths == 1 && smooth.after.bezierSegments == 4,
        "Color absorption flattened an unchanged external Bezier silhouette");
    options.maximumPasses = 0;
    const auto unchanged = run(noisy, renderer(target), 0);
    require(unchanged.accepted == 0 && svg(objects(unchanged.items), {64, 64}) == svg(noisy, {64, 64}),
        "Zero-pass absorption changed its baseline");
}

} // namespace

int selfTest() {
    std::puts("hole filling");
    holeFilling();
    std::puts("chords and rectangles");
    chordAndRectangle();
    std::puts("bridges and appearance groups");
    bridgesAndAppearanceGroups();
    adjacentMerge();
    std::puts("cycles and separate stack positions");
    cyclesAndSeparateStackPositions();
    std::puts("silhouette and no-op");
    silhouetteAndNoOp();
    std::puts("export audit");
    exportAudit();
    pathCountAndTopology();
    bottomUpCompletion();
    ownershipPrecision();
    opaqueBoundaryRepair();
    threatenedCompletion();
    croppedSvgRender();
    incrementalEdits();
    colorAbstraction();
    localAppearance();
    originalAppearanceGuard();
    localSourceRestoration();
    partialColorGrouping();
    bucketTargets();
    connectedSupportRecovery();
    globalComponentPlacement();
    colorSupportConstruction();
    std::puts("local color absorption");
    localColorAbsorption();
    std::puts("Opaque region tests passed: holes, chords, rectangles, bridges, cycles, separate stack positions, silhouette, deterministic no-op, export order, fill rules, transforms, opacity, boundary AA, Bucket conversion, thin geometry, connected supports, joint ordering and local color absorption.");
    return 0;
}

} // namespace opaque
