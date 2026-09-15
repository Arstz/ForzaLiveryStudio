#include "catalog_cover.h"
#include "catalog_cover_internal.h"
#include "project_codec.h"
#include "layer.h"
#include "compact_fit.h"
#include "profile_fit.h"
#include "profile_fit_selection.h"
#include "compact_fit_quality.h"
#include "compact_fit_budget.h"
#include "compact_fit_reduction.h"
#include "greedy_cover.h"
#include "matrix_math.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <stdexcept>

namespace {

void require(bool condition, const QString &message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

gui::PenLoop polygonLoop(const QPolygonF &polygon, gui::PenLoopKind kind = gui::PenLoopKind::Outer) {
    gui::PenLoop loop;
    loop.kind = kind;
    for (const QPointF &point : polygon) {
        loop.points.push_back({point, gui::PenPointKind::Hard});
    }
    return loop;
}

QPainterPath outputPath(const gui::PenFillResult &fill, const QVector<gui::catalog::Primitive> &catalog) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    for (const gui::PenPlacement &placement : fill.placements) {
        const auto shape = std::find_if(catalog.begin(), catalog.end(), [&](const auto &primitive) {
            return primitive.shape.shapeId == placement.shapeId;
        });
        require(shape != catalog.end(), QStringLiteral("Output uses a shape outside the dictionary"));
        for (const QPolygonF &contour : shape->shape.contours) {
            QPolygonF polygon = placement.transform.map(contour);
            if (placement.transform.determinant() < 0) {
                std::reverse(polygon.begin(), polygon.end());
            }
            result.addPolygon(polygon);
            result.closeSubpath();
        }
    }
    return result;
}

void requireComplete(const gui::catalog::FillResult &result) {
    require(result.fill.error.isEmpty(), result.fill.error);
    require(!result.fill.cancelled && !result.fill.placements.isEmpty(), QStringLiteral("No completed fill"));
    require(result.fill.unfilled.isEmpty(), QStringLiteral("Fill contains an uncovered residual"));
    require(result.diagnostics.value(QStringLiteral("coverageVerifiedOnGrid")).toBool(),
            QStringLiteral("Final coverage was not verified"));
}

void requireSame(const gui::catalog::FillResult &first, const gui::catalog::FillResult &second) {
    requireComplete(first);
    requireComplete(second);
    require(first.fill.placements.size() == second.fill.placements.size(), QStringLiteral("Repeat count differs"));
    for (int index = 0; index < first.fill.placements.size(); ++index) {
        require(first.fill.placements[index].shapeId == second.fill.placements[index].shapeId
                && first.fill.placements[index].transform == second.fill.placements[index].transform,
                QStringLiteral("Repeat transform differs"));
    }
}

void checkCompletion(const gui::PenFillRequest &request,
                     const QVector<gui::catalog::Primitive> &catalog) {
    const auto region = gui::catalog::buildRegion(request, {});
    const auto first = gui::catalog::completeCover(region, catalog, {});
    const auto second = gui::catalog::completeCover(region, catalog, {});
    require(!first.isEmpty() && first.size() == second.size(), QStringLiteral("Mesh repeat count differs"));
    gui::catalog::Polygons polygons;
    gui::catalog::Polygons original;
    for (int index = 0; index < first.size(); ++index) {
        require(first[index].placement.shapeId == second[index].placement.shapeId
                && first[index].placement.transform == second[index].placement.transform,
                QStringLiteral("Mesh repeat transform differs"));
        const auto shape = std::find_if(catalog.begin(), catalog.end(), [&](const auto &primitive) {
            return primitive.shape.shapeId == first[index].placement.shapeId;
        });
        require(shape != catalog.end(), QStringLiteral("Unknown completion shape"));
        polygons += gui::catalog::mapped(shape->shape,
            gui::catalog::emittedTransform(first[index].placement.transform));
        original += first[index].polygons;
    }
    const auto coverage = gui::catalog::unite(polygons);
    require(gui::catalog::subtract(region.required, coverage).isEmpty(), QStringLiteral("Mesh coverage failed"));
    const auto outside = gui::catalog::subtract(coverage, region.permitted);
    require(outside.isEmpty(), QStringLiteral("Mesh spill failed: %1, first reconstruction %2")
        .arg(gui::catalog::area(outside), 0, 'g', 12)
        .arg(gui::catalog::area(gui::catalog::subtract(gui::catalog::unite(original), region.permitted)), 0, 'g', 12));
}

gui::PenFillRequest readRequest(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), error.errorString());
    const auto object = document.object().value(QStringLiteral("request")).toObject();
    gui::PenFillRequest request;
    request.boundaryTolerance = object.value(QStringLiteral("boundaryTolerance")).toDouble(0.1);
    for (const auto &value : object.value(QStringLiteral("loops")).toArray()) {
        const auto source = value.toObject();
        gui::PenLoop loop;
        loop.kind = source.value(QStringLiteral("kind")).toString() == QStringLiteral("cutout")
            ? gui::PenLoopKind::Cutout : gui::PenLoopKind::Outer;
        for (const auto &entry : source.value(QStringLiteral("points")).toArray()) {
            const auto point = entry.toObject();
            const auto position = point.value(QStringLiteral("position")).toArray();
            require(position.size() == 2, QStringLiteral("Invalid replay point"));
            loop.points.push_back({QPointF(position[0].toDouble(), position[1].toDouble()),
                point.value(QStringLiteral("kind")).toString() == QStringLiteral("soft")
                    ? gui::PenPointKind::Soft : gui::PenPointKind::Hard});
        }
        request.loops.push_back(loop);
    }
    require(!request.loops.isEmpty(), QStringLiteral("Replay log has no contour loops"));

    return request;
}

void auditCatalog(const gui::ShapeGeometryStore &geometry, const QString &destination) {
    QJsonArray records;
    QVector<int> ids = geometry.shapeIds();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        gui::catalog::Polygons triangles;
        const auto *shape = geometry.shape(id);
        bool opaque = true;
        for (const auto &triangle : shape->triangles) {
            QPolygonF polygon({triangle.p0, triangle.p1, triangle.p2});
            if (gui::catalog::signedArea(polygon) < 0) {
                std::reverse(polygon.begin(), polygon.end());
            }
            triangles.push_back(polygon);
            opaque = opaque && triangle.alpha0 == 1 && triangle.alpha1 == 1 && triangle.alpha2 == 1;
        }
        const auto silhouette = gui::catalog::unite(triangles);
        QJsonArray contours;
        for (const QPolygonF &polygon : silhouette) {
            QJsonArray points;
            for (const QPointF &point : polygon) {
                points.push_back(QJsonArray{point.x(), point.y()});
            }
            contours.push_back(points);
        }
        records.push_back(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("opaque"), opaque},
            {QStringLiteral("area"), gui::catalog::area(silhouette)},
            {QStringLiteral("contours"), contours},
        });
    }
    QFile report(destination);
    require(report.open(QIODevice::WriteOnly | QIODevice::Truncate), report.errorString());
    report.write(QJsonDocument(QJsonObject{
        {QStringLiteral("geometry_model"), QStringLiteral("visible triangle union on a 1e-6 coordinate grid")},
        {QStringLiteral("records"), records},
    }).toJson(QJsonDocument::Compact));
}

void compareProject(const QString &path, const gui::PenFillRequest &request,
                    const QVector<gui::catalog::Primitive> &catalog) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto project = fls::decodeProjectDocument(file.readAll());
    const auto region = gui::catalog::buildRegion(request, {});
    QTextStream output(stdout);
    const gui::compact::BoundaryModel boundary(region.required, 1.0);
    for (const auto &node : project.root->children) {
        if (node->kind() != fls::scene::LayerKind::Group) {
            continue;
        }
        QVector<gui::catalog::Polygons> placements;
        gui::catalog::Polygons polygons;
        QJsonObject histogram;
        std::function<void(const fls::scene::Layer &, const QTransform &)> collect;
        collect = [&](const auto &layer, const QTransform &parent) {
            require(layer.visible && layer.opacity == 1, QStringLiteral("Comparison expects opaque visible layers"));
            const auto matrix = layer.transform.matrix();
            const QTransform transform = QTransform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
                matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]) * parent;
            if (layer.kind() == fls::scene::LayerKind::Group) {
                for (const auto &child : static_cast<const fls::scene::Group &>(layer).children) {
                    collect(*child, transform);
                }
                return;
            }
            require(layer.kind() == fls::scene::LayerKind::Shape, QStringLiteral("Unexpected group leaf"));
            const auto &shape = static_cast<const fls::scene::Shape &>(layer);
            require(!shape.mask && !shape.isRaster() && shape.color[3] == 255,
                    QStringLiteral("Comparison requires opaque vector shapes"));
            const auto primitive = std::find_if(catalog.begin(), catalog.end(), [&](const auto &entry) {
                return entry.shape.shapeId == shape.shapeId;
            });
            require(primitive != catalog.end(), QStringLiteral("Shape missing from catalog"));
            const auto support = gui::catalog::mapped(primitive->shape, gui::catalog::emittedTransform(transform));
            placements.push_back(support);
            polygons += support;
            const auto key = QString::number(shape.shapeId);
            histogram[key] = histogram[key].toInt() + 1;
        };
        for (const auto &child : static_cast<const fls::scene::Group &>(*node).children) {
            collect(*child, QTransform());
        }
        const auto coverage = gui::catalog::unite(polygons);
        QJsonArray bands;
        for (double radius : {0.0, 0.25, 0.5, 1.0, 2.0}) {
            const auto expandedTarget = radius > 0 ? gui::catalog::expanded(region.required, radius) : region.required;
            const auto expandedCoverage = radius > 0 ? gui::catalog::expanded(coverage, radius) : coverage;
            bands.push_back(QJsonObject{{"radius", radius},
                {"missingBeyondBand", gui::catalog::area(gui::catalog::subtract(region.required, expandedCoverage))},
                {"spillBeyondBand", gui::catalog::area(gui::catalog::subtract(coverage, expandedTarget))}});
        }
        QVector<double> exclusive;
        for (int index = 0; index < placements.size(); ++index) {
            gui::catalog::Polygons others;
            for (int other = 0; other < placements.size(); ++other) {
                if (other != index) {
                    others += placements[other];
                }
            }
            exclusive.push_back(gui::catalog::area(gui::catalog::subtract(placements[index], gui::catalog::unite(others))));
        }
        std::sort(exclusive.begin(), exclusive.end());
        QJsonArray contribution;
        for (double value : exclusive) {
            contribution.push_back(value);
        }
        output << QJsonDocument(QJsonObject{{"group", node->name}, {"count", placements.size()},
            {"targetArea", region.area}, {"shapeCounts", histogram}, {"bands", bands},
            {"boundaryQuality", boundary.diagnostics(boundary.measure(coverage))},
            {"exclusiveAreasSorted", contribution}}).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
    }
}

QVector<gui::PenPlacement> projectSeed(const QString &source, const QString &name) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto project = fls::decodeProjectDocument(file.readAll());
    QVector<gui::PenPlacement> result;
    std::function<void(const fls::scene::Layer &, const QTransform &)> collect;
    collect = [&](const auto &layer, const QTransform &parent) {
        require(layer.visible && layer.opacity == 1, QStringLiteral("Seed must be opaque and visible"));
        const auto matrix = layer.transform.matrix();
        const QTransform transform = QTransform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
            matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]) * parent;
        if (layer.kind() == fls::scene::LayerKind::Group) {
            for (const auto &child : static_cast<const fls::scene::Group &>(layer).children) {
                collect(*child, transform);
            }
            return;
        }
        require(layer.kind() == fls::scene::LayerKind::Shape, QStringLiteral("Seed contains a non-shape"));
        const auto &shape = static_cast<const fls::scene::Shape &>(layer);
        require(!shape.mask && !shape.isRaster() && shape.color[3] == 255, QStringLiteral("Seed must use opaque vectors"));
        result.push_back({shape.shapeId, transform});
    };
    for (const auto &node : project.root->children) {
        if (node->kind() == fls::scene::LayerKind::Group && node->name == name) {
            require(result.isEmpty(), QStringLiteral("Ambiguous seed group"));
            for (const auto &child : static_cast<const fls::scene::Group &>(*node).children) {
                collect(*child, QTransform());
            }
        }
    }
    require(!result.isEmpty(), QStringLiteral("Seed group was not found"));
    return result;
}

void saveComparison(const QString &source, const QString &destination, const gui::PenFillResult &fill,
                     const QString &name = QStringLiteral("Compact Fit - continuity")) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    auto project = fls::decodeProjectDocument(file.readAll());
    auto group = std::make_unique<fls::scene::Group>();
    group->name = name;
    group->transform.x = -750;
    for (const auto &placement : fill.placements) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->shapeId = placement.shapeId;
        shape->color = {252, 252, 252, 255};
        const auto &transform = placement.transform;
        fls::Matrix3 matrix;
        matrix.m[0][0] = transform.m11();
        matrix.m[1][0] = transform.m12();
        matrix.m[0][1] = transform.m21();
        matrix.m[1][1] = transform.m22();
        matrix.m[0][2] = transform.dx();
        matrix.m[1][2] = transform.dy();
        shape->transform = fls::decomposeTransform2D(matrix);
        group->append(std::move(shape));
    }
    project.root->append(std::move(group));
    QFile output(destination);
    require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), output.errorString());
    const auto encoded = fls::encodeProjectDocument(project);
    require(output.write(encoded) == encoded.size(), output.errorString());
}

void requireApproximation(const gui::PenFillRequest &request, const gui::catalog::FillResult &result,
                          const QVector<gui::catalog::Primitive> &catalog, const gui::compact::FillOptions &options) {
    if (!result.fill.error.isEmpty()) {
        QTextStream(stdout) << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    }
    require(result.fill.error.isEmpty() && !result.fill.cancelled && !result.fill.placements.isEmpty(), result.fill.error);
    const auto region = gui::catalog::buildRegion(request, {});
    gui::catalog::Polygons polygons;
    for (const auto &placement : result.fill.placements) {
        const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto &primitive) {
            return primitive.shape.shapeId == placement.shapeId;
        });
        require(found != catalog.end(), QStringLiteral("Compact output escaped dictionary"));
        polygons += gui::catalog::mapped(found->shape, gui::catalog::emittedTransform(placement.transform));
    }
    const auto coverage = gui::catalog::unite(polygons);
    const double error = gui::catalog::area(gui::catalog::subtract(region.required, coverage))
        + gui::catalog::area(gui::catalog::subtract(coverage, region.required));
    require(error <= region.area * options.areaErrorRatio, QStringLiteral("Compact area error failed"));
    require(gui::catalog::subtract(coverage, gui::catalog::expanded(region.required, options.boundaryAllowance)).isEmpty(),
            QStringLiteral("Compact spill allowance failed after transform reconstruction"));
    require(gui::catalog::subtract(region.required, gui::catalog::expanded(coverage, options.inwardAllowance)).isEmpty(),
            QStringLiteral("Compact missing allowance failed after transform reconstruction"));
    const gui::compact::BoundaryModel boundary(region.required, options.observationScale);
    const auto expected = boundary.measure(region.required);
    const auto measured = boundary.measure(coverage);
    require(measured.components == expected.components && measured.holes == expected.holes,
            QStringLiteral("Compact visible topology changed"));
    require(measured.maximumCornerDistance <= options.observationScale * 0.5,
            QStringLiteral("Compact displaced a protected corner"));
    require(measured.maximumExcessTurn <= 0.6, QStringLiteral("Compact introduced a sharp turn on a smooth boundary"));
    require(boundary.energy(measured) <= std::max(1.0, boundary.perimeter() * 0.025),
            QStringLiteral("Compact boundary roughness exceeded its limit"));
}

void compactTests(const QVector<gui::catalog::Primitive> &catalog, bool profile = false) {
    QTextStream output(stdout);
    const auto fit = [profile](const gui::PenFillRequest &request, const QVector<gui::catalog::Primitive> &primitives,
                              const gui::compact::FillOptions &options, const std::function<bool()> &cancelled = {}) {
        return profile ? gui::profile::fillRegion(request, primitives, options, cancelled)
            : gui::compact::fillRegion(request, primitives, options, cancelled);
    };
    gui::compact::FillOptions options;
    options.evaluationBudget = 12000;
    int reportedWork = 0;
    int workCalls = 0;
    options.workProgress = [&](int count, int evaluated, int budget) {
        require(count >= 0 && evaluated >= reportedWork && budget == options.evaluationBudget + (profile ? options.evaluationBudget / 5 : 0),
                QStringLiteral("Compact work progress is invalid"));
        reportedWork = evaluated;
        ++workCalls;
    };
    const auto circle = std::find_if(catalog.begin(), catalog.end(), [](const auto &primitive) {
        return primitive.shape.shapeId == 102;
    });
    require(circle != catalog.end(), QStringLiteral("Missing circle fixture"));
    const gui::compact::BoundaryModel smoothModel(circle->shape.contours, 1.0);
    const auto smoothMetrics = smoothModel.measure(circle->shape.contours);
    const QPointF rim(circle->shape.bounds.right(), circle->shape.bounds.center().y());
    const auto bump = gui::catalog::unite(circle->shape.contours + gui::catalog::Polygons{
        QPolygonF({rim + QPointF(-0.5, -2), rim + QPointF(3, 0), rim + QPointF(-0.5, 2)})});
    require(smoothModel.energy(smoothModel.measure(bump)) > smoothModel.energy(smoothMetrics) + 0.1,
            QStringLiteral("Smooth boundary bump was not detected"));
    QTransform smoothSpill;
    smoothSpill.scale(1.03, 1.03);
    const auto smoothlyExpanded = gui::catalog::mapped(circle->shape, smoothSpill);
    require(smoothModel.energy(smoothModel.measure(smoothlyExpanded)) < smoothModel.energy(smoothModel.measure(bump)),
            QStringLiteral("Smooth outward displacement was ranked below a sharp bump"));
    const auto punctured = gui::catalog::subtract(circle->shape.contours,
        {QPolygonF({{-2, -2}, {2, -2}, {2, 2}, {-2, 2}})});
    require(smoothModel.measure(punctured).holes > smoothMetrics.holes, QStringLiteral("Visible hole was ignored"));
    const auto square = gui::PenFillRequest{{}, {polygonLoop({{-40, -30}, {40, -30}, {40, 30}, {-40, 30}})}};
    auto ring = square;
    const auto squareRegion = gui::catalog::buildRegion(square, {});
    const gui::compact::BoundaryModel cornerModel(squareRegion.required, 1.0);
    const auto exactCorner = cornerModel.measure(squareRegion.required);
    require(exactCorner.maximumCornerDistance < 1e-6 && exactCorner.cornerDefects == 0,
        QStringLiteral("Genuine sharp corners were penalized"));
    const auto cutCorner = gui::catalog::subtract(squareRegion.required,
        {QPolygonF({{36, 30}, {40, 26}, {40, 30}})});
    require(cornerModel.measure(cutCorner).maximumCornerDistance > 1,
        QStringLiteral("Loss of a genuine corner was ignored"));
    QPolygonF star;
    for (int index = 0; index < 16; ++index) {
        QTransform rotation;
        rotation.rotate(index * 22.5);
        star.push_back(rotation.map(QPointF(index % 2 ? 40 : 80, 0)));
    }
    QTransform displacement;
    displacement.translate(2.3, -1.7);
    const auto shiftedStar = displacement.map(star);
    double expectedCornerDistance = 0;
    for (const auto &corner : star) {
        double closest = std::numeric_limits<double>::infinity();
        for (int edge = 0; edge < shiftedStar.size(); ++edge) {
            const auto delta = shiftedStar[(edge + 1) % shiftedStar.size()] - shiftedStar[edge];
            const double parameter = std::clamp(QPointF::dotProduct(corner - shiftedStar[edge], delta)
                / QPointF::dotProduct(delta, delta), 0.0, 1.0);
            closest = std::min(closest, QLineF(corner, shiftedStar[edge] + delta * parameter).length());
        }
        expectedCornerDistance = std::max(expectedCornerDistance, closest);
    }
    const gui::compact::BoundaryModel starModel({star}, 1.0);
    require(std::abs(starModel.measure({shiftedStar}).maximumCornerDistance - expectedCornerDistance) < 1e-8,
        QStringLiteral("Indexed corner distance differs from exhaustive measurement"));
    ring.loops.push_back(polygonLoop({{-10, -10}, {10, -10}, {10, 10}, {-10, 10}}, gui::PenLoopKind::Cutout));
    const auto nativePrimitive = std::find_if(catalog.begin(), catalog.end(), [](const auto &primitive) {
        return primitive.shape.shapeId == 2117;
    });
    require(nativePrimitive != catalog.end(), QStringLiteral("Native fixture missing"));
    QTransform transform;
    transform.translate(317.123, -713.579);
    transform.rotate(31.7);
    transform.shear(0.21, 0);
    transform.scale(-1.21, 0.73);
    gui::PenFillRequest native;
    native.loops = {polygonLoop(transform.map(nativePrimitive->shape.contours.front()))};
    for (const auto &request : {square, ring, native}) {
        reportedWork = 0;
        const auto result = fit(request, catalog, options);
        requireApproximation(request, result, catalog, options);
        if (profile) {
            require(result.diagnostics.value("profileSeed").toObject().value("selectionShapeLimit").toInt() == options.shapeBudget,
                QStringLiteral("Profile seed has a separate shape limit"));
        }
        output << "Compact fixture: " << result.fill.placements.size() << " shapes\n" << Qt::flush;
        reportedWork = 0;
        const auto repeat = fit(request, catalog, options);
        requireApproximation(request, repeat, catalog, options);
        require(repeat.fill.placements.size() == result.fill.placements.size(), QStringLiteral("Compact repeat count failed"));
        for (int index = 0; index < result.fill.placements.size(); ++index) {
            require(repeat.fill.placements[index].shapeId == result.fill.placements[index].shapeId
                && repeat.fill.placements[index].transform == result.fill.placements[index].transform,
                QStringLiteral("Compact repeat transform failed"));
        }
    }
    require(workCalls > 0, QStringLiteral("Compact fit did not report bounded work"));
    options.workProgress = {};
    if (profile) {
        const gui::PenFillRequest triangle{{}, {polygonLoop({{-40, -30}, {40, -30}, {-40, 30}})}};
        const auto triangleResult = fit(triangle, catalog, options);
        requireApproximation(triangle, triangleResult, catalog, options);
        require(triangleResult.fill.placements.size() == 1 && triangleResult.fill.placements.front().shapeId == 103,
            QStringLiteral("A triangular region should select one Triangle"));
        auto enlarged = ring;
        for (auto &loop : enlarged.loops) {
            for (auto &point : loop.points) {
                point.position *= 20.0;
            }
        }
        const auto enlargedResult = fit(enlarged, catalog, options);
        requireApproximation(enlarged, enlargedResult, catalog, options);
        const auto jobs = enlargedResult.diagnostics.value(QStringLiteral("profileSeed")).toObject();
        require(jobs.value(QStringLiteral("processedCurveJobs")).toInt() == jobs.value(QStringLiteral("curveJobs")).toInt(),
            QStringLiteral("Large-region search skipped contour spans"));
        auto wider = options;
        wider.boundaryAllowance = 8.0;
        reportedWork = 0;
        wider.workProgress = [&](int count, int evaluated, int budget) {
            require(count >= 0 && evaluated >= reportedWork && budget == 2 * (options.evaluationBudget + options.evaluationBudget / 5),
                QStringLiteral("Profile retry progress is invalid"));
            reportedWork = evaluated;
        };
        const auto widerResult = fit(square, catalog, wider);
        const auto tighterResult = fit(square, catalog, options);
        requireApproximation(square, widerResult, catalog, wider);
        require(widerResult.fill.placements.size() == tighterResult.fill.placements.size(), QStringLiteral("Wider margin changed a feasible tight count"));
        for (int index = 0; index < widerResult.fill.placements.size(); ++index) {
            require(widerResult.fill.placements[index].shapeId == tighterResult.fill.placements[index].shapeId
                && widerResult.fill.placements[index].transform == tighterResult.fill.placements[index].transform,
                QStringLiteral("Wider margin changed a feasible tight placement"));
        }
        output << "Large-region and wider-margin regressions passed\n" << Qt::flush;
    }
    const auto nativeResult = fit(native, catalog, options);
    if (nativeResult.fill.placements.size() != 1) {
        output << QJsonDocument(nativeResult.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    }
    require(nativeResult.fill.placements.size() == 1 && nativeResult.fill.placements.front().shapeId != 101
        && nativeResult.fill.placements.front().shapeId != 103, QStringLiteral("Native curved fit is not one catalog shape"));
    auto initialized = options;
    initialized.initialPlacements = nativeResult.fill.placements;
    requireApproximation(native, fit(native, catalog, initialized), catalog, initialized);
    initialized.initialPlacements.front().shapeId = -1;
    require(!fit(native, catalog, initialized).fill.error.isEmpty(),
        QStringLiteral("Unknown warm-start shape was accepted"));
    auto restricted = options;
    restricted.shapeBudget = 1;
    const auto budget = fit(ring, catalog, restricted);
    require(!budget.fill.error.isEmpty() && budget.fill.placements.isEmpty(), QStringLiteral("Compact shape limit ignored"));
    const auto canceled = fit(square, catalog, options, [] { return true; });
    require(canceled.fill.cancelled && canceled.fill.placements.isEmpty(), QStringLiteral("Compact cancellation ignored"));
    int calls = 0;
    const auto midCancel = fit(ring, catalog, options, [&] { return ++calls > 200; });
    require(midCancel.fill.cancelled && midCancel.fill.placements.isEmpty(), QStringLiteral("Compact in-flight cancellation ignored"));
    auto invalid = options;
    invalid.boundaryAllowance = std::numeric_limits<double>::quiet_NaN();
    require(!fit(square, catalog, invalid).fill.error.isEmpty(), QStringLiteral("Invalid compact allowance accepted"));
    invalid.boundaryAllowance = std::numeric_limits<double>::infinity();
    require(!fit(square, catalog, invalid).fill.error.isEmpty(), QStringLiteral("Infinite compact allowance accepted"));
    output << "Compact approximation, native shape, repeatability, budget and cancellation tests passed\n" << Qt::flush;
}

void benchmarkBoundary(const QString &path, const QVector<gui::catalog::Primitive> &catalog) {
    const auto region = gui::catalog::buildRegion(readRequest(path), {});
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto placements = object.value(QStringLiteral("result")).toObject().value(QStringLiteral("placements")).toArray();
    gui::catalog::Polygons coverage;
    QVector<gui::catalog::Polygons> pieces;
    for (const auto &entry : placements) {
        const auto placement = entry.toObject();
        const auto fields = placement.value(QStringLiteral("transform")).toArray();
        const int id = placement.value(QStringLiteral("shapeId")).toInt();
        const auto found = std::find_if(catalog.begin(), catalog.end(), [id](const auto &shape) { return shape.shape.shapeId == id; });
        require(found != catalog.end() && fields.size() == 6, QStringLiteral("Invalid benchmark placement"));
        pieces.push_back(gui::catalog::mapped(found->shape, QTransform(fields[0].toDouble(), fields[1].toDouble(),
            fields[2].toDouble(), fields[3].toDouble(), fields[4].toDouble(), fields[5].toDouble())));
        coverage += pieces.back();
    }
    require(!coverage.isEmpty(), QStringLiteral("Benchmark needs recorded placements"));
    coverage = gui::catalog::unite(coverage);
    const gui::compact::BoundaryModel model(region.required, 1.0);
    gui::compact::BoundaryMetrics metrics;
    for (int index = 0; index < 100; ++index) {
        metrics = model.measure(coverage);
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{{QStringLiteral("quality"), model.diagnostics(metrics)},
        {QStringLiteral("timing"), model.performance()}}).toJson(QJsonDocument::Compact) << '\n';
    const gui::compact::BoundaryModel localModel(region.required, 1.0);
    double maximumDifference = 0;
    double maximumEnergyDifference = 0;
    int topologyDifferences = 0;
    for (int piece = 0; piece < pieces.size(); ++piece) {
        gui::catalog::Polygons others;
        for (int index = 0; index < pieces.size(); ++index) {
            if (index != piece) {
                others += pieces[index];
            }
        }
        others = gui::catalog::unite(others);
        const auto observedOthers = model.observationSupport(others);
        const auto bounds = gui::catalog::painterPath(pieces[piece]).boundingRect();
        const auto window = model.observationWindow(others, observedOthers, bounds);
        const auto local = localModel.observationSupport(pieces[piece], window);
        const auto global = model.observationSupport(coverage);
        const double difference = gui::catalog::area(gui::catalog::subtract(local, global))
            + gui::catalog::area(gui::catalog::subtract(global, local));
        maximumDifference = std::max(maximumDifference, difference);
        const auto localMetrics = localModel.measure(coverage, local);
        maximumEnergyDifference = std::max(maximumEnergyDifference, std::abs(localModel.energy(localMetrics) - model.energy(metrics)));
        topologyDifferences += localMetrics.components != metrics.components || localMetrics.holes != metrics.holes;
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{{QStringLiteral("localTiming"), localModel.performance()},
        {QStringLiteral("maximumSymmetricDifference"), maximumDifference},
        {QStringLiteral("maximumEnergyDifference"), maximumEnergyDifference},
        {QStringLiteral("topologyDifferences"), topologyDifferences}}).toJson(QJsonDocument::Compact) << '\n';
}

void compactSearchTests(const QVector<gui::catalog::Primitive> &catalog) {
    using namespace gui::compact;
    using gui::catalog::Polygons;
    require(FillOptions{}.shapeBudget == 3000, QStringLiteral("Default shape budget changed"));
    require(gui::profile::greedyCover(180, FillOptions{}.shapeBudget, [](int) { return 1.0; }, [](int) {}, [] { return false; }).size() == 180,
        QStringLiteral("Selection stopped at the removed seed ceiling"));
    for (int total : {1, 3, 99, 12000, 60000, std::numeric_limits<int>::max()}) {
        int previous = 0;
        for (auto stage : {WorkStage::Recognition, WorkStage::Repair, WorkStage::SpatialReduction,
                WorkStage::ExposedReduction, WorkStage::Polish}) {
            const int ceiling = stageLimit(total, stage);
            require(ceiling >= previous && ceiling <= total, QStringLiteral("Invalid stage allocation"));
            previous = ceiling;
        }
        require(previous == total, QStringLiteral("Stage allocation lost work"));
    }
    const Polygons target{QPolygonF({{0, 0}, {100, 0}, {100, 100}, {0, 100}})};
    const Polygons hole{QPolygonF({{10, 10}, {20, 10}, {20, 20}, {10, 20}})};
    const BoundaryModel model(target, 1);
    const auto targetMetrics = model.measure(target);
    const auto exact = reductionState(target, target, model, 0.5);
    const auto broken = reductionState(gui::catalog::subtract(target, hole), target, model, 0.5);
    require(nonWorseningReduction(broken, broken, targetMetrics), QStringLiteral("Unchanged remote defect rejected"));
    require(!nonWorseningReduction(broken, exact, targetMetrics), QStringLiteral("New interior hole accepted"));
    auto relocated = broken;
    QTransform shift;
    shift.translate(60, 60);
    relocated.deepMissing = {shift.map(broken.deepMissing.front())};
    require(!nonWorseningReduction(relocated, broken, targetMetrics), QStringLiteral("Relocated defect accepted"));
    relocated = broken;
    relocated.observedHoles = {shift.map(broken.observedHoles.front())};
    require(!nonWorseningReduction(relocated, broken, targetMetrics), QStringLiteral("Relocated observed hole accepted"));
    auto worse = broken;
    worse.cornerDistances.front() += 1;
    require(!nonWorseningReduction(worse, broken, targetMetrics), QStringLiteral("Aggregate metrics hid a damaged corner"));
    worse = broken;
    worse.metrics.maximumExcessTurn += 0.1;
    require(!nonWorseningReduction(worse, broken, targetMetrics), QStringLiteral("New contour kink accepted"));
    auto first = broken;
    auto second = broken;
    first.missingArea += 0.75e-7;
    second.missingArea += 1.5e-7;
    require(nonWorseningReduction(second, first, targetMetrics)
        && !nonWorseningReduction(second, broken, targetMetrics), QStringLiteral("Fixed baseline does not stop tolerance creep"));
    const auto square = std::find_if(catalog.cbegin(), catalog.cend(), [](const auto &entry) { return entry.shape.shapeId == 101; });
    require(square != catalog.cend(), QStringLiteral("Missing square"));
    const auto &bounds = square->shape.bounds;
    QTransform transform;
    transform.scale(20 / bounds.width(), 20 / bounds.height());
    transform.translate(-bounds.left(), -bounds.top());
    gui::PenPlacement placement;
    placement.shapeId = 101;
    placement.transform = transform;
    FillOptions options;
    options.initialPlacements = {placement, placement, placement};
    options.shapeBudget = options.initialPlacements.size();
    options.evaluationBudget = 400;
    options.retainFailedFill = true;
    const gui::PenFillRequest request{{}, {polygonLoop({{0, 0}, {20, 0}, {20, 8}, {100, 8},
        {100, 0}, {120, 0}, {120, 20}, {100, 20}, {100, 12}, {20, 12}, {20, 20}, {0, 20}})}};
    const auto result = gui::compact::fillRegion(request, {*square}, options);
    if (result.fill.placements.size() >= options.initialPlacements.size()) {
        QTextStream(stdout) << result.fill.error << '\n' << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    }
    require(!result.fill.cancelled && !result.fill.error.isEmpty() && !result.fill.placements.isEmpty()
        && result.fill.placements.size() < options.initialPlacements.size(),
        QStringLiteral("Remote quality failure prevented duplicate removal"));
    require(result.diagnostics.value("approximateReductions").toInt() > 0, QStringLiteral("Approximate reduction was not recorded"));
    const auto stages = result.diagnostics.value("stageWork").toObject();
    int used = 0;
    for (const auto &name : {"recognition", "repair", "spatialReduction", "exposedReduction", "polish"}) {
        const auto stage = stages.value(name).toObject();
        require(!stage.isEmpty() && stage.value("start").toInt() == used,
            QStringLiteral("Stage work accounting is discontinuous"));
        used += stage.value("evaluations").toInt();
        require(used <= stage.value("ceiling").toInt(), QStringLiteral("Stage budget overrun"));
    }
    require(used == result.diagnostics.value("evaluations").toInt() && used <= options.evaluationBudget,
        QStringLiteral("Total work accounting differs"));
    require(stages.value("polish").toObject().value("evaluations").toInt() > 0,
        QStringLiteral("Polishing was starved"));
    require(stages.value("repair").toObject().value("refitPlacements").toInt() >= options.initialPlacements.size(),
        QStringLiteral("Repair did not reach all placements"));
    QTextStream(stdout) << "Stage budgets, local reductions, topology and baseline checks passed\n";
}

void coverageRepairTests(const QVector<gui::catalog::Primitive> &catalog) {
    using namespace gui::compact;
    using gui::catalog::Polygons;
    const std::vector<gui::profile::ProfileAlternative> alternatives{
        {100, 0.3, 0.1, 0.01}, {20, 0.001, 0.01, 0.0}, {10, 0.5, 0.2, 0.02}, {30, 0.1, 0.001, 0.0}};
    const auto order = gui::profile::profileAlternativeOrder(alternatives);
    require(order.size() == alternatives.size() && order[0] == 0 && order[1] == 1
        && order[2] == 3 && order.back() == 2, QStringLiteral("Boundary alternatives lost the accuracy or tangent extreme"));
    require(order == gui::profile::profileAlternativeOrder(alternatives), QStringLiteral("Alternative order is unstable"));
    const Polygons target{QPolygonF({{0, 0}, {100, 0}, {100, 100}, {51, 100},
        {51, 20}, {50.8, 20}, {50.8, 100}, {0, 100}})};
    const BoundaryModel reference(target, 1);
    const auto metrics = reference.measure(target);
    require(reference.energy(metrics) < 1e-6 && metrics.maximumExcessTurn < 1e-6,
        QStringLiteral("Observation changes are incorrectly attributed to the generated contour"));
    const auto exact = reductionState(target, target, reference, 0.5);
    const Polygons hole{QPolygonF({{20, 20}, {30, 20}, {30, 30}, {20, 30}})};
    const auto broken = reductionState(gui::catalog::subtract(target, hole), target, reference, 0.5);
    require(!preservesCoverage(broken, exact, metrics, 0.5), QStringLiteral("Polishing can open an interior hole"));
    require(preservesCoverage(exact, broken, metrics, 0.5), QStringLiteral("Coverage repair was rejected"));
    auto damaged = exact;
    damaged.cornerDistances.front() = 2;
    require(!preservesCoverage(damaged, exact, metrics, 0.5), QStringLiteral("Coverage repair can damage a protected corner"));
    const auto square = std::find_if(catalog.cbegin(), catalog.cend(), [](const auto &entry) { return entry.shape.shapeId == 101; });
    require(square != catalog.cend(), QStringLiteral("Missing repair square"));
    const auto &bounds = square->shape.bounds;
    gui::PenPlacement placement;
    placement.shapeId = 101;
    placement.transform.scale(20 / bounds.width(), 20 / bounds.height());
    placement.transform.translate(-bounds.left(), -bounds.top());
    FillOptions options;
    options.initialPlacements = {placement};
    options.evaluationBudget = 6000;
    options.shapeBudget = 30;
    options.retainFailedFill = true;
    const gui::PenFillRequest request{{}, {polygonLoop({{0, 0}, {100, 0}, {100, 100}, {0, 100}}),
        polygonLoop({{60, 60}, {80, 60}, {80, 80}, {60, 80}}, gui::PenLoopKind::Cutout)}};
    const auto region = gui::catalog::buildRegion(request, {});
    const double originalMissing = gui::catalog::area(gui::catalog::subtract(region.required,
        gui::catalog::expanded(gui::catalog::mapped(square->shape, placement.transform), options.inwardAllowance)));
    const auto result = gui::compact::fillRegion(request, {*square}, options);
    QTextStream(stdout) << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    require(!result.fill.cancelled && result.diagnostics.value("residualInsertions").toInt() > 0,
        QStringLiteral("Residual repair did not insert any shapes"));
    require(result.fill.placements.size() <= options.shapeBudget
        && result.diagnostics.value("evaluations").toInt() <= options.evaluationBudget,
        QStringLiteral("Repair exceeded a caller budget"));
    require(result.diagnostics.value("missingBeyondInward").toDouble() < originalMissing * 0.5,
        QStringLiteral("Residual repair failed to recover most missing coverage"));
    require(result.diagnostics.value("boundaryQuality").toObject().value("components").toInt() == 1,
        QStringLiteral("Residual repair left isolated patches"));
    double previousDeep = originalMissing;
    for (const auto &checkpoint : result.diagnostics.value("history").toArray()) {
        const double deep = checkpoint.toObject().value("deepMissing").toDouble();
        require(deep <= previousDeep + 1e-5, QStringLiteral("Refinement reopened deep missing area"));
        previousDeep = deep;
    }
    Polygons coverage;
    for (const auto &entry : result.fill.placements) {
        coverage += gui::catalog::mapped(square->shape, entry.transform);
    }
    const Polygons cutout{QPolygonF({{61, 61}, {79, 61}, {79, 79}, {61, 79}})};
    require(gui::catalog::intersect(gui::catalog::unite(coverage), cutout).isEmpty(),
        QStringLiteral("Repair filled an intended hole"));
    const auto repeated = gui::compact::fillRegion(request, {*square}, options);
    require(repeated.fill.placements.size() == result.fill.placements.size(), QStringLiteral("Repair count is not deterministic"));
    for (int index = 0; index < result.fill.placements.size(); ++index) {
        require(result.fill.placements[index].transform == repeated.fill.placements[index].transform,
            QStringLiteral("Repair transforms are not deterministic"));
    }
    const auto cancelled = gui::compact::fillRegion(request, {*square}, options, [] { return true; });
    require(cancelled.fill.cancelled, QStringLiteral("Repair ignored cancellation"));
    options.evaluationBudget = 2000;
    const auto expandedCatalog = gui::compact::fillRegion(request, catalog, options);
    require(expandedCatalog.diagnostics.value("missingBeyondInward").toDouble() < originalMissing * 0.5,
        QStringLiteral("Catalog expansion starved the interior clearance proposals"));
    QTextStream(stdout) << "Residual repair, coverage guards, reference calibration and boundary retention passed\n";
}

void fastQualityTests() {
    quint64 random = 7812387;
    for (int trial = 0; trial < 64; ++trial) {
        QVector<quint64> masks;
        for (int index = 0; index < 48; ++index) {
            random = random * 6364136223846793005ULL + 1442695040888963407ULL;
            masks.push_back(random & (random >> (trial % 5)));
        }
        quint64 missing = ~quint64(0);
        const auto score = [&](int index) { return std::popcount(masks[index] & missing) / (1.0 + index % 7 * 0.25); };
        const auto accept = [&](int index) { missing &= ~masks[index]; };
        QVector<int> expected;
        for (int iteration = 0; iteration < 16; ++iteration) {
            int best = -1;
            double bestScore = 0;
            for (int index = 0; index < masks.size(); ++index) {
                if (score(index) > bestScore) {
                    best = index;
                    bestScore = score(index);
                }
            }
            if (best < 0) {
                break;
            }
            expected.push_back(best);
            accept(best);
        }
        missing = ~quint64(0);
        const auto actual = gui::profile::greedyCover(masks.size(), 16, score, accept, [] { return false; });
        require(actual == expected, QStringLiteral("Lazy cover changed exhaustive greedy selection"));
    }
    require(gui::profile::greedyCover(5, 5, [](int) { return 1.0; }, [](int) {}, [] { return true; }).isEmpty(),
        QStringLiteral("Lazy cover ignored cancellation"));
    const gui::catalog::Polygons target{QPolygonF({{-50, -40}, {50, -40}, {50, 40}, {-50, 40}})};
    const auto others = gui::catalog::subtract(target, {QPolygonF({{-10, -40}, {10, -40}, {10, 20}, {-10, 20}}),
        QPolygonF({{25, -10}, {35, -10}, {35, 10}, {25, 10}})});
    const gui::compact::BoundaryModel model(target, 1.0);
    const auto base = model.observationSupport(others);
    std::optional<gui::compact::BoundaryModel::ObservationWindow> cachedWindow;
    int reusedWindows = 0;
    for (int index = 0; index < 48; ++index) {
        QTransform movement;
        movement.translate(index * 0.13 - 3, index * 0.09 - 2);
        movement.rotate(index * 7.3);
        const gui::catalog::Polygons addition{movement.map(QPolygonF({{-14, -41}, {14, -41}, {8, 23}, {-8, 23}}))};
        const auto coverage = gui::catalog::unite(others + addition);
        const auto bounds = gui::catalog::painterPath(addition).boundingRect();
        const auto window = model.observationWindow(others, base, bounds);
        const auto local = model.observationSupport(addition, window);
        const auto global = model.observationSupport(coverage);
        const double difference = gui::catalog::area(gui::catalog::subtract(local, global))
            + gui::catalog::area(gui::catalog::subtract(global, local));
        require(difference < 0.001, QStringLiteral("Local closing changed support beyond grid rounding"));
        const auto measured = model.measure(coverage, local);
        const auto reference = model.measure(coverage, global);
        require(measured.components == reference.components && measured.holes == reference.holes,
            QStringLiteral("Local closing changed observed topology"));
        const double oldSpill = gui::catalog::area(gui::catalog::subtract(gui::catalog::subtract(addition, others), target));
        const double newSpill = gui::catalog::area(gui::catalog::subtract(addition, gui::catalog::unite(others + target)));
        require(std::abs(oldSpill - newSpill) < 0.001, QStringLiteral("Cached spill exclusion changed area beyond grid rounding"));
        if (!cachedWindow || !cachedWindow->additionBounds.contains(bounds)) {
            cachedWindow = model.observationWindow(others, base, bounds);
        } else {
            ++reusedWindows;
        }
        const auto cached = model.observationSupport(addition, *cachedWindow);
        const double cachedDifference = gui::catalog::area(gui::catalog::subtract(cached, global))
            + gui::catalog::area(gui::catalog::subtract(global, cached));
        require(cachedDifference < 0.001, QStringLiteral("Reusing an observation window changed support beyond grid rounding"));
        const auto cachedMetrics = model.measure(coverage, cached);
        require(cachedMetrics.components == reference.components && cachedMetrics.holes == reference.holes,
            QStringLiteral("Reusing an observation window changed topology"));
    }
    require(reusedWindows > 0, QStringLiteral("Local quality test did not exercise window reuse"));
    QTextStream(stdout) << "Lazy greedy equivalence, local quality support, spill cache and cancellation passed\n";
}

void failedFillTests(const QVector<gui::catalog::Primitive> &catalog) {
    const gui::PenFillRequest square{{}, {polygonLoop({{-40, -30}, {40, -30}, {40, 30}, {-40, 30}})}};
    auto ring = square;
    ring.loops.push_back(polygonLoop({{-10, -10}, {10, -10}, {10, 10}, {-10, 10}}, gui::PenLoopKind::Cutout));
    gui::compact::FillOptions strict;
    strict.evaluationBudget = 1;
    const auto exact = gui::profile::fillRegion(square, catalog, strict);
    requireApproximation(square, exact, catalog, strict);
    strict.initialPlacements = exact.fill.placements;
    QTransform offset;
    offset.translate(3, 0);
    for (auto &placement : strict.initialPlacements) {
        placement.transform *= offset;
    }
    const auto rejected = gui::profile::fillRegion(square, catalog, strict);
    require(!rejected.fill.error.isEmpty() && rejected.fill.placements.isEmpty(), QStringLiteral("Strict failure returned shapes"));
    auto review = strict;
    review.retainFailedFill = true;
    const auto retained = gui::profile::fillRegion(square, catalog, review);
    require(retained.fill.error == rejected.fill.error && !retained.fill.placements.isEmpty(),
        QStringLiteral("Failed approximation lost its shapes or error"));
    require(!retained.diagnostics.value(QStringLiteral("approximationVerified")).toBool()
        && retained.diagnostics.value(QStringLiteral("retainedAfterError")).toBool()
        && retained.diagnostics.value(QStringLiteral("failedChecks")).toArray().contains(QStringLiteral("sharp-corner position")),
        QStringLiteral("Retained approximation lost its failed verification"));
    require(!outputPath(retained.fill, catalog).isEmpty(), QStringLiteral("Retained placements have no drawable geometry"));
    const auto repeated = gui::profile::fillRegion(square, catalog, review);
    require(repeated.fill.placements.size() == retained.fill.placements.size(), QStringLiteral("Retained count changed on repeat"));
    for (int index = 0; index < retained.fill.placements.size(); ++index) {
        require(retained.fill.placements[index].shapeId == repeated.fill.placements[index].shapeId
            && retained.fill.placements[index].transform == repeated.fill.placements[index].transform,
            QStringLiteral("Retained geometry changed on repeat"));
    }
    const auto cancelled = gui::profile::fillRegion(square, catalog, review, [] { return true; });
    require(cancelled.fill.cancelled && cancelled.fill.placements.isEmpty(), QStringLiteral("Cancelled review retained shapes"));
    review.initialPlacements.clear();
    const auto recovered = gui::profile::fillRegion(square, catalog, review, {},
        [](int, double, double) { throw std::runtime_error("Injected refinement failure"); });
    require(recovered.fill.error == QStringLiteral("Injected refinement failure") && !recovered.fill.placements.isEmpty()
        && recovered.diagnostics.value(QStringLiteral("retainedProfileSeed")).toBool(),
        QStringLiteral("Refinement exception discarded the profile seed"));
    const auto accepted = gui::profile::fillRegion(square, catalog, review);
    requireApproximation(square, accepted, catalog, review);
    require(accepted.fill.placements.size() == exact.fill.placements.size()
        && accepted.fill.placements.front().transform == exact.fill.placements.front().transform,
        QStringLiteral("Retention changed a successful fill"));
    review.boundaryAllowance = std::numeric_limits<double>::quiet_NaN();
    const auto invalid = gui::profile::fillRegion(square, catalog, review);
    require(!invalid.fill.error.isEmpty() && invalid.fill.placements.isEmpty(), QStringLiteral("Invalid input produced fallback shapes"));
    gui::catalog::FillOptions mesh;
    mesh.candidateLimit = 0;
    mesh.searchNodes = 0;
    mesh.shapeBudget = 1;
    mesh.retainFailedFill = true;
    const auto overBudget = gui::catalog::fillRegion(ring, catalog, mesh);
    require(!overBudget.fill.error.isEmpty() && overBudget.fill.placements.size() > mesh.shapeBudget
        && overBudget.diagnostics.value(QStringLiteral("retainedAfterError")).toBool(),
        QStringLiteral("Catalog failure discarded the completed cover"));
    const auto failedSearch = gui::catalog::fillRegion(square, catalog, mesh, {},
        [](int, double, double) { throw std::runtime_error("Injected catalog failure"); });
    require(failedSearch.fill.error == QStringLiteral("Injected catalog failure") && !failedSearch.fill.placements.isEmpty(),
        QStringLiteral("Catalog exception discarded its completed mesh"));
    bool cancelAfterMesh = false;
    const auto cancelledSearch = gui::catalog::fillRegion(square, catalog, mesh,
        [&] { return cancelAfterMesh; }, [&](int, double, double) { cancelAfterMesh = true; });
    require(cancelledSearch.fill.cancelled && cancelledSearch.fill.placements.isEmpty(),
        QStringLiteral("Catalog cancellation retained its fallback mesh"));
    QTextStream(stdout) << "Failed verification retention, exception recovery, repeatability and cancellation passed\n";
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTextStream output(stdout);
    QDir::setCurrent(QStringLiteral(FLS_SOURCE_DIR));
    try {
        if (application.arguments().size() == 2 && application.arguments()[1] == QStringLiteral("--release-catalog-test")) {
            QTemporaryDir directory;
            require(directory.isValid() && QDir::setCurrent(directory.path()), QStringLiteral("Cannot isolate the catalog test working directory"));
            gui::ShapeGeometryStore geometry;
            QString error;
            require(gui::catalog::buildCatalog(geometry, &error).isEmpty() && error.contains(QStringLiteral("Shape geometry is not loaded")),
                QStringLiteral("An unloaded geometry store reported a duplicate shape ID"));
            error.clear();
            require(geometry.loadDefault(&error), error);
            const auto catalog = gui::catalog::buildCatalog(geometry, &error);
            require(error.isEmpty() && !catalog.isEmpty(), error);
            gui::ShapeGeometryStore source;
            require(source.loadFromFile(QStringLiteral(FLS_SOURCE_DIR "/assets/vector/shape_geometry.json.gz"), &error), error);
            auto loadedIds = geometry.shapeIds();
            auto sourceIds = source.shapeIds();
            std::sort(loadedIds.begin(), loadedIds.end());
            std::sort(sourceIds.begin(), sourceIds.end());
            require(!loadedIds.isEmpty() && loadedIds == sourceIds, QStringLiteral("Deployed geometry differs from the source shape catalog"));
            const gui::PenFillRequest square{{}, {polygonLoop({{-40, -30}, {40, -30}, {40, 30}, {-40, 30}})}};
            gui::compact::FillOptions options;
            options.evaluationBudget = 12000;
            const auto result = gui::profile::fillRegion(square, catalog, options);
            requireApproximation(square, result, catalog, options);
            output << "Release assets loaded " << loadedIds.size() << " shape geometries and " << catalog.size()
                << " fill primitives without source-directory fallback; Compact Fit produced " << result.fill.placements.size() << " verified shapes\n";
            QDir::setCurrent(QStringLiteral(FLS_SOURCE_DIR));
            return 0;
        }
        gui::ShapeGeometryStore geometry;
        QString error;
        require(geometry.loadDefault(&error), error);
        if (application.arguments().size() == 3
            && application.arguments()[1] == QStringLiteral("--audit")) {
            auditCatalog(geometry, application.arguments()[2]);
            output << "All visible catalog triangle unions audited\n";
            return 0;
        }
        const QVector<gui::catalog::Primitive> fullCatalog = gui::catalog::buildCatalog(geometry, &error);
        require(error.isEmpty(), error);
        if (application.arguments().size() == 2 && application.arguments()[1] == QStringLiteral("--compact-search-tests")) {
            compactSearchTests(fullCatalog);
            return 0;
        }
        if (application.arguments().size() == 2 && application.arguments()[1] == QStringLiteral("--coverage-repair-tests")) {
            coverageRepairTests(fullCatalog);
            return 0;
        }
        if (application.arguments().size() == 2 && application.arguments()[1] == QStringLiteral("--fast-quality-tests")) {
            fastQualityTests();
            return 0;
        }
        if (application.arguments().size() == 3 && application.arguments()[1] == QStringLiteral("--benchmark-boundary")) {
            benchmarkBoundary(application.arguments()[2], fullCatalog);
            return 0;
        }
        if (application.arguments().size() == 2 && application.arguments()[1] == QStringLiteral("--failed-fill-tests")) {
            failedFillTests(fullCatalog);
            return 0;
        }
        if (application.arguments().size() == 2 && (application.arguments()[1] == QStringLiteral("--compact-tests")
            || application.arguments()[1] == QStringLiteral("--profile-tests"))) {
            compactTests(fullCatalog, application.arguments()[1] == QStringLiteral("--profile-tests"));
            return 0;
        }
        if (application.arguments().size() >= 3 && (application.arguments()[1] == QStringLiteral("--compact-fit")
            || application.arguments()[1] == QStringLiteral("--profile-fit")
            || application.arguments()[1] == QStringLiteral("--profile-project")
            || application.arguments()[1] == QStringLiteral("--profile-retained")
            || application.arguments()[1] == QStringLiteral("--profile-polish")
            || application.arguments()[1] == QStringLiteral("--profile-repeat")
            || application.arguments()[1] == QStringLiteral("--compact-project")
            || application.arguments()[1] == QStringLiteral("--compact-polish")
            || application.arguments()[1] == QStringLiteral("--compact-repeat"))) {
            gui::compact::FillOptions options;
            options.retainFailedFill = application.arguments()[1] == QStringLiteral("--profile-retained");
            if (application.arguments().size() > 3) {
                options.boundaryAllowance = application.arguments()[3].toDouble();
            }
            if (application.arguments()[1] == QStringLiteral("--compact-polish")) {
                require(application.arguments().size() == 6, QStringLiteral("Expected request, allowance, project and seed group"));
                options.initialPlacements = projectSeed(application.arguments()[4], application.arguments()[5]);
            }
            QElapsedTimer timer;
            timer.start();
            int reported = 0;
            options.workProgress = [&](int count, int evaluated, int budget) {
                if (evaluated - reported >= budget / 10) {
                    output << "Work " << evaluated << '/' << budget << ", " << count << " shapes, " << timer.elapsed() << " ms\n" << Qt::flush;
                    reported = evaluated;
                }
            };
            const bool profile = application.arguments()[1].startsWith(QStringLiteral("--profile-"));
            auto result = profile ? gui::profile::fillRegion(readRequest(application.arguments()[2]), fullCatalog, options)
                : gui::compact::fillRegion(readRequest(application.arguments()[2]), fullCatalog, options, {},
                [&](int count, double missing, double spill) {
                    output << count << " shapes, missing " << missing << ", spill " << spill
                           << ", " << timer.elapsed() << " ms\n" << Qt::flush;
                });
            output << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
            output << "Completed in " << timer.elapsed() << " ms\n" << Qt::flush;
            if (options.retainFailedFill) {
                require(!result.fill.cancelled && !result.fill.placements.isEmpty(), QStringLiteral("Replay returned no retained fill"));
                output << "Retained " << result.fill.placements.size() << " shapes: " << result.fill.error << '\n' << Qt::flush;
                if (application.arguments().size() == 6) {
                    saveComparison(application.arguments()[4], application.arguments()[5], result.fill,
                        QStringLiteral("Compact Fit - local quality"));
                }
            } else {
                require(result.fill.error.isEmpty(), result.fill.error);
                requireApproximation(readRequest(application.arguments()[2]), result, fullCatalog, options);
            }
            if (application.arguments()[1] == QStringLiteral("--compact-project") || application.arguments()[1] == QStringLiteral("--profile-project")) {
                require(application.arguments().size() == 6, QStringLiteral("Expected request, allowance, source project and new destination"));
                saveComparison(application.arguments()[4], application.arguments()[5], result.fill,
                    profile ? QStringLiteral("Compact Fit - curve first") : QStringLiteral("Compact Fit - continuity"));
            }
            if (application.arguments()[1] == QStringLiteral("--compact-repeat") || application.arguments()[1] == QStringLiteral("--profile-repeat")) {
                const auto repeat = profile ? gui::profile::fillRegion(readRequest(application.arguments()[2]), fullCatalog, options)
                    : gui::compact::fillRegion(readRequest(application.arguments()[2]), fullCatalog, options);
                require(repeat.fill.error.isEmpty(), repeat.fill.error);
                require(repeat.fill.placements.size() == result.fill.placements.size(), QStringLiteral("Compact repeat count differs"));
                for (int index = 0; index < result.fill.placements.size(); ++index) {
                    require(repeat.fill.placements[index].shapeId == result.fill.placements[index].shapeId
                        && repeat.fill.placements[index].transform == result.fill.placements[index].transform,
                        QStringLiteral("Compact repeat geometry differs"));
                }
                output << "Compact repeat matched every ID and transform\n" << Qt::flush;
            }
            return 0;
        }
        if (application.arguments().size() == 4 && application.arguments()[1] == QStringLiteral("--compare-project")) {
            compareProject(application.arguments()[2], readRequest(application.arguments()[3]), fullCatalog);
            return 0;
        }
        if (application.arguments().size() == 3
            && application.arguments()[1].startsWith(QStringLiteral("--replay"))) {
            gui::catalog::FillOptions replayOptions;
            if (application.arguments()[1] == QStringLiteral("--replay-mesh")) {
                replayOptions.candidateLimit = 0;
                replayOptions.searchNodes = 0;
            } else if (application.arguments()[1] == QStringLiteral("--replay-compact")) {
                replayOptions.candidateLimit = 1;
                replayOptions.searchNodes = 0;
            } else {
                require(application.arguments()[1] == QStringLiteral("--replay"),
                        QStringLiteral("Unknown replay mode"));
            }
            const auto request = readRequest(application.arguments()[2]);
            QFile log(application.arguments()[2]);
            if (log.open(QIODevice::ReadOnly)) {
                const auto recorded = QJsonDocument::fromJson(log.readAll()).object()
                    .value(QStringLiteral("result")).toObject().value(QStringLiteral("placements")).toArray();
                if (!recorded.isEmpty()) {
                    gui::catalog::Polygons supports;
                    for (const auto &entry : recorded) {
                        const auto object = entry.toObject();
                        const auto transform = object.value(QStringLiteral("transform")).toArray();
                        const int id = object.value(QStringLiteral("shapeId")).toInt();
                        const auto shape = std::find_if(fullCatalog.begin(), fullCatalog.end(),
                            [id](const auto &primitive) { return primitive.shape.shapeId == id; });
                        require(shape != fullCatalog.end() && transform.size() == 6, QStringLiteral("Invalid recorded placement"));
                        supports += gui::catalog::mapped(shape->shape, gui::catalog::emittedTransform(QTransform(
                            transform[0].toDouble(), transform[1].toDouble(), transform[2].toDouble(),
                            transform[3].toDouble(), transform[4].toDouble(), transform[5].toDouble())));
                    }
                    const auto coverage = gui::catalog::unite(supports);
                    const auto region = gui::catalog::buildRegion(request, {});
                    output << "Recorded result: " << recorded.size() << " placements, grid missing area "
                           << gui::catalog::area(gui::catalog::subtract(region.required, coverage))
                           << ", outside envelope " << gui::catalog::area(gui::catalog::subtract(coverage, region.permitted))
                           << '\n' << Qt::flush;
                }
            }
            QElapsedTimer timer;
            timer.start();
            const auto replay = gui::catalog::fillRegion(request, fullCatalog, replayOptions);
            output << QJsonDocument(replay.diagnostics).toJson(QJsonDocument::Compact) << '\n';
            requireComplete(replay);
            output << "Replay completed: " << replay.fill.placements.size() << " placements, "
                   << timer.elapsed() << " ms\n";
            return 0;
        }
        require(fullCatalog.size() == 96, QStringLiteral("Unexpected curated catalog size"));
        require(std::any_of(fullCatalog.begin(), fullCatalog.end(), [](const auto &primitive) {
            return primitive.shape.shapeId == 2124 && primitive.shape.contours.size() > 1;
        }), QStringLiteral("Catalog lost a shape's hole"));
        require(std::any_of(fullCatalog.begin(), fullCatalog.end(), [](const auto &primitive) {
            return primitive.shape.shapeId == 324 && primitive.shape.contours.size() > 1;
        }), QStringLiteral("Catalog lost a disconnected shape component"));

        QVector<gui::catalog::Primitive> smallCatalog;
        for (const auto &primitive : fullCatalog) {
            if (primitive.shape.shapeId == 101 || primitive.shape.shapeId == 103
                || primitive.shape.shapeId == 2117) {
                smallCatalog.push_back(primitive);
            }
        }
        gui::catalog::FillOptions options;
        options.refinementSteps = 3;
        options.searchNodes = 1500;
        gui::PenFillRequest square;
        square.loops = {polygonLoop({{-40, -30}, {40, -30}, {40, 30}, {-40, 30}})};
        const auto squareFill = gui::catalog::fillRegion(square, smallCatalog, options);
        requireComplete(squareFill);
        require(squareFill.fill.placements.size() == 1, QStringLiteral("Square did not reduce to one placement"));
        output << "Square compression passed\n" << Qt::flush;

        gui::PenFillRequest ring = square;
        ring.loops.push_back(polygonLoop({{-10, -10}, {10, -10}, {10, 10}, {-10, 10}},
                                        gui::PenLoopKind::Cutout));
        gui::catalog::FillOptions meshOptions = options;
        meshOptions.candidateLimit = 0;
        meshOptions.searchNodes = 0;
        const auto ringFill = gui::catalog::fillRegion(ring, smallCatalog, meshOptions);
        requireComplete(ringFill);
        const QPainterPath ringOutput = outputPath(ringFill.fill, smallCatalog);
        for (int y = -25; y <= 25; y += 5) {
            for (int x = -35; x <= 35; x += 5) {
                if (std::abs(x) > 10 || std::abs(y) > 10) {
                    require(ringOutput.contains(QPointF(x, y)), QStringLiteral("Ring interior is uncovered"));
                } else if (std::abs(x) < 10 && std::abs(y) < 10) {
                    require(!ringOutput.contains(QPointF(x, y)), QStringLiteral("Ring hole was filled"));
                }
            }
        }
        output << "Hole preservation passed\n" << Qt::flush;

        gui::PenFillRequest curved;
        curved.points = {
            {{-40, -20}, gui::PenPointKind::Hard}, {{0, -45}, gui::PenPointKind::Soft},
            {{40, -20}, gui::PenPointKind::Hard}, {{60, 0}, gui::PenPointKind::Soft},
            {{40, 20}, gui::PenPointKind::Hard}, {{0, 45}, gui::PenPointKind::Soft},
            {{-40, 20}, gui::PenPointKind::Hard}, {{-60, 0}, gui::PenPointKind::Soft},
        };
        const auto curvedFill = gui::catalog::fillRegion(curved, smallCatalog, meshOptions);
        requireComplete(curvedFill);
        output << "Bezier fallback verified with " << curvedFill.fill.placements.size() << " placements\n" << Qt::flush;
        const QPainterPath curvedOutput = outputPath(curvedFill.fill, smallCatalog);
        const QPainterPath target = gui::buildPenContour(curved.points).path;
        for (int y = -40; y <= 40; ++y) {
            for (int x = -60; x <= 60; ++x) {
                const QPointF point(x + 0.173, y + 0.287);
                if (target.contains(point)) {
                    require(curvedOutput.contains(point), QStringLiteral("Bezier interior is uncovered"));
                }
            }
        }
        output << "Bezier enclosure passed\n" << Qt::flush;

        for (int index = 0; index < 12; ++index) {
            QTransform transform;
            transform.translate(-1900.317 + 371.293 * index, 2100.731 - 419.117 * index);
            transform.rotate(17.371 + index * 29.113);
            transform.scale(index % 2 ? -1.3 : 0.71, index % 3 ? 2.19 : 0.037);
            gui::PenFillRequest transformed = curved;
            transformed.boundaryTolerance = index % 4 ? 0.1 : 0.01;
            for (auto &point : transformed.points) {
                point.position = transform.map(point.position);
            }
            output << "Checking transformed completion " << index << '\n' << Qt::flush;
            checkCompletion(transformed, smallCatalog);
        }
        output << "Translated, rotated, mirrored and thin Bezier completion passed\n" << Qt::flush;

        gui::PenFillRequest narrow;
        narrow.loops = {polygonLoop({{15.631, -23.713}, {35.631, -23.713}, {25.631, -23.710}})};
        narrow.boundaryTolerance = 0.001;
        const auto narrowRegion = gui::catalog::buildRegion(narrow, {});
        const auto narrowMesh = gui::catalog::completeCover(narrowRegion, smallCatalog, {});
        require(narrowMesh.size() > 1, QStringLiteral("Narrow completion did not exercise subdivision"));
        checkCompletion(narrow, smallCatalog);
        int completionChecks = 0;
        const auto canceledMesh = gui::catalog::completeCover(narrowRegion, smallCatalog,
            [&] { return ++completionChecks > 20; });
        require(canceledMesh.isEmpty() && completionChecks > 20,
                QStringLiteral("Completion cancellation was ignored"));
        output << "Subdivision coverage and completion cancellation passed\n" << Qt::flush;

        const auto irregular = readRequest(QStringLiteral("tools/fixtures/catalog_cover_irregular.json"));
        const auto irregularFill = gui::catalog::fillRegion(irregular, fullCatalog, meshOptions);
        requireSame(irregularFill, gui::catalog::fillRegion(irregular, fullCatalog, meshOptions));
        require(std::any_of(irregularFill.fill.placements.begin(), irregularFill.fill.placements.end(),
            [](const auto &placement) { return placement.shapeId == 101; }),
            QStringLiteral("Irregular contour did not exercise rectangle completion"));
        output << "Irregular contour replay and deterministic completion passed\n" << Qt::flush;

        const auto lemon = std::find_if(smallCatalog.begin(), smallCatalog.end(), [](const auto &primitive) {
            return primitive.shape.shapeId == 2117;
        });
        require(lemon != smallCatalog.end(), QStringLiteral("Missing non-basic catalog primitive"));
        gui::PenFillRequest native;
        native.loops = {polygonLoop(lemon->shape.contours.front())};
        const auto first = gui::catalog::fillRegion(native, smallCatalog, options);
        const auto second = gui::catalog::fillRegion(native, smallCatalog, options);
        requireComplete(first);
        requireComplete(second);
        require(first.fill.placements.size() == 1 && first.fill.placements.front().shapeId == 2117,
                QStringLiteral("Native non-basic shape was not selected as a single placement"));
        requireSame(first, second);
        output << "Non-basic shape selection and determinism passed\n" << Qt::flush;

        meshOptions.shapeBudget = 1;
        const auto overBudget = gui::catalog::fillRegion(ring, smallCatalog, meshOptions);
        require(!overBudget.fill.error.isEmpty() && overBudget.fill.placements.isEmpty(),
                QStringLiteral("Over-budget fill was accepted"));
        const auto canceled = gui::catalog::fillRegion(square, smallCatalog, options, [] { return true; });
        require(canceled.fill.cancelled && canceled.fill.placements.isEmpty(), QStringLiteral("Cancellation failed"));
        output << "Budget and cancellation passed\n" << Qt::flush;

        QElapsedTimer timer;
        timer.start();
        const auto full = gui::catalog::fillRegion(native, fullCatalog, options);
        requireComplete(full);
        require(full.fill.placements.size() == 1, QStringLiteral("Curated dictionary lost the one-shape cover"));
        output << "Full curated catalog passed in " << timer.elapsed() << " ms\n";
        output << QJsonDocument(full.diagnostics).toJson(QJsonDocument::Compact) << '\n';
        timer.restart();
        const auto optimizedCurve = gui::catalog::fillRegion(curved, fullCatalog);
        requireComplete(optimizedCurve);
        require(optimizedCurve.fill.placements.size() <= curvedFill.fill.placements.size(),
                QStringLiteral("Catalog candidates made the curved fallback worse"));
        output << "Curved cover: fallback " << curvedFill.fill.placements.size() << ", selected "
               << optimizedCurve.fill.placements.size() << ", " << timer.elapsed() << " ms\n" << Qt::flush;
        const auto repeatedCurve = gui::catalog::fillRegion(curved, fullCatalog);
        requireSame(optimizedCurve, repeatedCurve);

        const auto countRequest = readRequest(QStringLiteral("tools/fixtures/catalog_cover_count.json"));
        timer.restart();
        const auto compact = gui::catalog::fillRegion(countRequest, fullCatalog);
        requireComplete(compact);
        require(compact.fill.placements.size() * 3
                < compact.diagnostics.value(QStringLiteral("meshPlacements")).toInt(),
                QStringLiteral("Boundary-first cover did not substantially reduce the mesh"));
        require(std::any_of(compact.fill.placements.begin(), compact.fill.placements.end(), [](const auto &placement) {
            return placement.shapeId == 812 || placement.shapeId == 930 || placement.shapeId == 2131;
        }), QStringLiteral("Extended catalog shapes were not used"));
        const auto repeatedCompact = gui::catalog::fillRegion(countRequest, fullCatalog);
        requireSame(compact, repeatedCompact);
        output << "Count regression: " << compact.diagnostics.value(QStringLiteral("meshPlacements")).toInt()
               << " to " << compact.fill.placements.size() << ", repeated in " << timer.elapsed() << " ms\n";
        output << QJsonDocument(compact.diagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
    } catch (const std::exception &failure) {
        output << "FAILED: " << failure.what() << '\n';
        return 1;
    }
    output << "Catalog cover tests passed\n";

    return 0;
}
