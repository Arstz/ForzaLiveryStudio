#include "compact_fit_catalog.h"
#include "compact_fit_catalog_internal.h"
#include "project_codec.h"
#include "layer.h"
#include "compact_fit.h"
#include "profile_fit.h"
#include "thin_fit.h"
#include "lining_extract.h"
#include "cubic_contour.h"
#include "lining_fill.h"
#include "profile_fit_selection.h"
#include "compact_fit_quality.h"
#include "compact_fit_gpu_rank.h"
#include "compact_fit_budget.h"
#include "compact_fit_reduction.h"
#include "greedy_cover.h"
#include "matrix_math.h"
#include "shape_registry.h"

#include <clipper2/clipper.engine.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
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
    request.discardNegligiblePlacements = object.value(QStringLiteral("discardNegligiblePlacements")).toBool(true);
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
            auto &anchor = loop.points.back();
            const auto incoming = point.value(QStringLiteral("incoming")).toArray();
            const auto outgoing = point.value(QStringLiteral("outgoing")).toArray();
            anchor.explicitHandles = point.value(QStringLiteral("explicitHandles")).toBool();
            if (anchor.explicitHandles) {
                require(incoming.size() == 2 && outgoing.size() == 2,
                        QStringLiteral("Missing cubic replay handles"));
                anchor.incoming = {incoming[0].toDouble(), incoming[1].toDouble()};
                anchor.outgoing = {outgoing[0].toDouble(), outgoing[1].toDouble()};
            }
        }
        if (object.value(QStringLiteral("curveModel")).toString() != QStringLiteral("cubic-anchors-v1")) {
            gui::splitCurvedHardSpans(loop.points);
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
                    const QVector<gui::catalog::Primitive> &catalog, const QString &group = {}) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto project = fls::decodeProjectDocument(file.readAll());
    const auto region = gui::catalog::buildRegion(request, {});
    QTextStream output(stdout);
    const gui::compact::BoundaryModel boundary(region.required, 1.0);
    gui::catalog::Polygons corridor;
    for (const auto &polygon : region.required) {
        for (int index = 0; index < polygon.size(); ++index) {
            QPolygonF points;
            for (const QPointF &point : {polygon[index], polygon[(index + 1) % polygon.size()]}) {
                points += QPolygonF({point + QPointF(-0.5, -0.5), point + QPointF(0.5, -0.5),
                    point + QPointF(0.5, 0.5), point + QPointF(-0.5, 0.5)});
            }
            corridor.push_back(gui::catalog::convexHull(points));
        }
    }
    const auto inner = gui::catalog::subtract(region.required, gui::catalog::unite(corridor));
    for (const auto &node : project.root->children) {
        if (node->kind() != fls::scene::LayerKind::Group
            || (!group.isEmpty() && node->name != group)) {
            continue;
        }
        QVector<gui::catalog::Polygons> placements;
        gui::catalog::Polygons polygons;
        QJsonObject histogram;
        QJsonArray records;
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
            records.push_back(QJsonObject{{"shapeId", shape.shapeId}, {"transform", QJsonArray{
                transform.m11(), transform.m12(), transform.m21(), transform.m22(), transform.dx(), transform.dy()}}});
        };
        for (const auto &child : static_cast<const fls::scene::Group &>(*node).children) {
            collect(*child, QTransform());
        }
        const auto coverage = gui::catalog::unite(polygons);
        QJsonArray interiorGaps;
        for (const auto &polygon : gui::catalog::subtract(inner, coverage)) {
            QJsonArray points;
            for (const auto &point : polygon) {
                points.push_back(QJsonArray{point.x(), point.y()});
            }
            interiorGaps.push_back(QJsonObject{{"area", gui::catalog::signedArea(polygon)}, {"points", points}});
        }
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
            const auto privateSupport = gui::catalog::subtract(placements[index], gui::catalog::unite(others));
            exclusive.push_back(gui::catalog::area(privateSupport));
            auto record = records[index].toObject();
            record.insert("footprintArea", gui::catalog::area(placements[index]));
            record.insert("privateArea", gui::catalog::area(privateSupport));
            record.insert("privateInteriorArea", gui::catalog::area(gui::catalog::intersect(privateSupport, inner)));
            records[index] = record;
        }
        std::sort(exclusive.begin(), exclusive.end());
        QJsonArray contribution;
        for (double value : exclusive) {
            contribution.push_back(value);
        }
        output << QJsonDocument(QJsonObject{{"group", node->name}, {"count", placements.size()},
            {"missingInteriorArea", gui::catalog::area(gui::catalog::subtract(inner, coverage))},
            {"interiorGaps", interiorGaps},
            {"placements", records},
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

std::unique_ptr<fls::scene::Group> comparisonGroup(const gui::PenFillResult &fill, const QString &name) {
    auto group = std::make_unique<fls::scene::Group>();
    group->id = QStringLiteral("group_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    group->name = name;
    group->transform.x = -750;
    for (const auto &placement : fill.placements) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->id = QStringLiteral("layer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        shape->name = QStringLiteral("%1 [%2] #%3").arg(fls::detail::shapeName(placement.shapeId))
            .arg(placement.shapeId).arg(group->children.size() + 1, 3, 10, QLatin1Char('0'));
        shape->setVectorShape(placement.shapeId);
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

    return group;
}

void saveComparison(const QString &source, const QString &destination, const gui::PenFillResult &fill,
                     const QString &name = QStringLiteral("Compact Fit - continuity"),
                     const std::optional<QRectF> &reviewBounds = {},
                     const QVector<gui::PenPlacement> &exceptionPlacements = {}) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    auto project = fls::decodeProjectDocument(file.readAll());
    auto group = comparisonGroup(fill, name);
    if (reviewBounds.has_value()) {
        group->transform.x = reviewBounds->width() * 1.2;
        for (const auto &child : group->children)
            static_cast<fls::scene::Shape &>(*child).color = {0, 0, 0, 255};
    }
    gui::PenFillResult exceptions;
    exceptions.placements = exceptionPlacements;
    auto overlays = comparisonGroup(exceptions, QStringLiteral("Above lining"));
    for (auto &child : overlays->children) {
        static_cast<fls::scene::Shape &>(*child).color = {255, 255, 255, 255};
        child->name = QStringLiteral("Above lining: %1").arg(child->name);
        group->append(std::move(child));
    }
    project.root->append(std::move(group));
    QFile output(destination);
    require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), output.errorString());
    const auto encoded = fls::encodeProjectDocument(project);
    require(output.write(encoded) == encoded.size(), output.errorString());
}

void comparisonExportTests() {
    fls::Project project;
    gui::PenFillResult fill;
    fill.placements = {{101, QTransform(2, 0, 0.25, 3, 20, 30)},
                       {101, QTransform(-1, 0.5, 0, 2, -20, 30)},
                       {2133, QTransform(1, 0, 0, 1, 5, -7)}};
    project.root->append(comparisonGroup(fill, QStringLiteral("Comparison")));
    const auto loaded = fls::decodeProjectDocument(fls::encodeProjectDocument(project));
    require(loaded.root->children.size() == 1, QStringLiteral("Comparison export lost its group"));
    const auto &group = static_cast<const fls::scene::Group &>(*loaded.root->children.front());
    QSet<QString> ids{group.id};
    require(!group.id.isEmpty() && group.children.size() == fill.placements.size(),
        QStringLiteral("Comparison export lost selectable layers"));
    for (int index = 0; index < fill.placements.size(); ++index) {
        const auto &shape = static_cast<const fls::scene::Shape &>(*group.children[index]);
        require(!shape.id.isEmpty() && !ids.contains(shape.id) && shape.name.contains(QString::number(shape.shapeId))
            && shape.shapeId == fill.placements[index].shapeId,
            QStringLiteral("Comparison export has missing or duplicate shape identities"));
        ids.insert(shape.id);
        const auto matrix = shape.transform.matrix();
        const auto &expected = fill.placements[index].transform;
        require(std::abs(matrix.m[0][0] - expected.m11()) < 1e-8
            && std::abs(matrix.m[1][0] - expected.m12()) < 1e-8
            && std::abs(matrix.m[0][1] - expected.m21()) < 1e-8
            && std::abs(matrix.m[1][1] - expected.m22()) < 1e-8
            && std::abs(matrix.m[0][2] - expected.dx()) < 1e-8
            && std::abs(matrix.m[1][2] - expected.dy()) < 1e-8,
            QStringLiteral("Comparison export changed a placement transform"));
    }
    QTextStream(stdout) << "Comparison export preserves separate shape identities, names and transforms\n";
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
    const auto observedCorner = cornerModel.observationSupport(cutCorner);
    const auto globalCornerMetrics = cornerModel.measure(cutCorner, observedCorner);
    const auto fullWindowMetrics = cornerModel.measure(cutCorner, observedCorner, QRectF(-100, -100, 200, 200));
    require(cornerModel.diagnostics(globalCornerMetrics) == cornerModel.diagnostics(fullWindowMetrics),
        QStringLiteral("A complete measurement window changed boundary metrics"));
    const auto leftWindowMetrics = cornerModel.measure(cutCorner, observedCorner, QRectF(-45, -35, 20, 70));
    require(leftWindowMetrics.maximumCornerDistance < 1e-6
        && leftWindowMetrics.samples < globalCornerMetrics.samples,
        QStringLiteral("Local boundary scoring includes remote corner defects"));
    const gui::catalog::Polygons smallIsland{QPolygonF(QRectF(200, 200, 1, 1))};
    const gui::compact::BoundaryModel islandModel(squareRegion.required + smallIsland, 1.0);
    require(islandModel.measure(squareRegion.required + smallIsland).components == 2,
        QStringLiteral("Local evaluation silently ignores a small intended component"));
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
                point.incoming *= 20.0;
                point.outgoing *= 20.0;
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
            require(count >= 0 && evaluated >= reportedWork && budget == options.evaluationBudget + options.evaluationBudget / 5,
                QStringLiteral("Profile allowance selection progress is invalid"));
            reportedWork = evaluated;
        };
        const auto widerResult = fit(square, catalog, wider);
        const auto tighterResult = fit(square, catalog, options);
        requireApproximation(square, widerResult, catalog, wider);
        require(widerResult.fill.placements.size() == tighterResult.fill.placements.size(), QStringLiteral("Wider margin changed a feasible tight count"));
        require(widerResult.diagnostics.value("attempts").toArray().size() == 1
            && widerResult.diagnostics.value("profileSeed").toObject().value("allowanceSelection")
                .toObject().value("candidateSearches").toInt() == 1,
            QStringLiteral("Wider margin repeated the full search"));
        output << "Large-region and wider-margin regressions passed\n" << Qt::flush;
        gui::PenFillRequest thinBand;
        QTransform outerBand, innerBand;
        outerBand.scale(1.5, 1.5);
        innerBand.scale(1.375, 1.375);
        thinBand.loops = {polygonLoop(outerBand.map(circle->shape.contours.front())),
            polygonLoop(innerBand.map(circle->shape.contours.front()), gui::PenLoopKind::Cutout)};
        auto thinOptions = options;
        thinOptions.evaluationBudget = 1200;
        thinOptions.retainFailedFill = true;
        const auto thinResult = fit(thinBand, catalog, thinOptions);
        const auto thinSeed = thinResult.diagnostics.value("profileSeed").toObject();
        require(!thinResult.fill.placements.isEmpty() && thinSeed.value("thinRegionCandidates").toBool()
            && thinSeed.value("bodyCenters").toInt() > 24 && thinSeed.value("bodyCandidates").toInt() > 0,
            QStringLiteral("Thin-band seed did not use clearance-spaced catalog candidates"));
        require(std::abs(thinResult.diagnostics.value("areaErrorLimit").toDouble()
                - thinResult.fill.targetArea * thinOptions.areaErrorRatio) < 1e-6
            && thinResult.diagnostics.value("maximumExcessTurnLimit").toDouble() == 0.6,
            QStringLiteral("Thin-band seeding relaxed verification limits"));
        const auto thinRepeat = fit(thinBand, catalog, thinOptions);
        require(thinRepeat.fill.placements.size() == thinResult.fill.placements.size(),
            QStringLiteral("Thin-band repeat count changed"));
        for (int index = 0; index < thinResult.fill.placements.size(); ++index) {
            require(thinRepeat.fill.placements[index].shapeId == thinResult.fill.placements[index].shapeId
                && thinRepeat.fill.placements[index].transform == thinResult.fill.placements[index].transform,
                QStringLiteral("Thin-band repeat transform changed"));
        }
        output << "Thin band " << QJsonDocument(thinResult.diagnostics).toJson(QJsonDocument::Compact) << '\n';
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

void benchmarkExactReduction(const QString &path, const QVector<gui::catalog::Primitive> &catalog) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto recorded = QJsonDocument::fromJson(file.readAll()).object().value("result").toObject().value("placements").toArray();
    QVector<gui::PenPlacement> placements;
    for (const auto &entry : recorded) {
        const auto fields = entry.toObject().value("transform").toArray();
        require(fields.size() == 6, QStringLiteral("Invalid recorded transform"));
        gui::PenPlacement placement;
        placement.shapeId = entry.toObject().value("shapeId").toInt();
        placement.transform = QTransform(fields[0].toDouble(), fields[1].toDouble(), fields[2].toDouble(),
            fields[3].toDouble(), fields[4].toDouble(), fields[5].toDouble());
        placements.push_back(placement);
    }
    require(!placements.isEmpty(), QStringLiteral("Exact benchmark needs recorded placements"));
    const auto coverage = [&](const QVector<gui::PenPlacement> &pieces) {
        gui::catalog::Polygons polygons;
        for (const auto &piece : pieces) {
            const auto primitive = std::find_if(catalog.cbegin(), catalog.cend(), [&](const auto &entry) {
                return entry.shape.shapeId == piece.shapeId;
            });
            require(primitive != catalog.cend(), QStringLiteral("Recorded primitive is unavailable"));
            polygons += gui::catalog::mapped(primitive->shape, piece.transform);
        }
        return gui::catalog::unite(polygons);
    };
    QElapsedTimer timer;
    timer.start();
    const auto result = gui::compact::reduceExactCoverage(placements, catalog, {});
    const auto elapsed = timer.elapsed();
    const auto before = coverage(placements);
    const auto after = coverage(result.placements);
    require(gui::catalog::subtract(before, after).isEmpty() && gui::catalog::subtract(after, before).isEmpty(),
        QStringLiteral("Exact benchmark changed coverage"));
    const auto repeat = gui::compact::reduceExactCoverage(placements, catalog, {});
    require(repeat.placements.size() == result.placements.size(), QStringLiteral("Exact benchmark repeat count differs"));
    for (int index = 0; index < result.placements.size(); ++index) {
        require(result.placements[index].shapeId == repeat.placements[index].shapeId
            && result.placements[index].transform == repeat.placements[index].transform,
            QStringLiteral("Exact benchmark repeat transform differs"));
    }
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"elapsedMilliseconds", elapsed},
        {"diagnostics", result.diagnostics}, {"sameCoverage", true}, {"deterministic", true}}).toJson(QJsonDocument::Compact) << '\n';
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
    CoverageOwnership ownership;
    QVector<Polygons> footprints{target, target, hole};
    ownership.synchronize(footprints);
    require(ownership.exclusive({0}).isEmpty() && ownership.exclusive({1}).isEmpty(),
        QStringLiteral("Duplicate support was not recognized"));
    const auto joint = ownership.exclusive({0, 1});
    require(std::abs(gui::catalog::area(joint) - 9900) < 1e-6,
        QStringLiteral("Group ownership was incorrectly reduced to individual private areas"));
    ownership.exclusive({0});
    require(ownership.diagnostics().value("cacheHits").toInt() > 0,
        QStringLiteral("Ownership cache did not reuse a footprint"));
    ownership.rememberFailure({0, 1}, false);
    require(ownership.failed({1, 0}, false) && !ownership.failed({0, 1}, true),
        QStringLiteral("Failed merge cache ignored group order or search mode"));
    ownership.erase(1);
    footprints.removeAt(1);
    ownership.synchronize(footprints);
    require(!ownership.failed({0, 1}, false) && std::abs(gui::catalog::area(ownership.exclusive({0})) - 9900) < 1e-6,
        QStringLiteral("Deletion retained stale ownership or failed-merge data"));
    QTransform remoteShift;
    remoteShift.translate(300, 0);
    footprints.push_back({remoteShift.map(target.front())});
    ownership.synchronize(footprints);
    ownership.exclusive({2});
    const int hitsBeforeErase = ownership.diagnostics().value("cacheHits").toInt();
    ownership.erase(1);
    footprints.removeAt(1);
    ownership.synchronize(footprints);
    ownership.exclusive({1});
    require(ownership.diagnostics().value("cacheHits").toInt() == hitsBeforeErase + 1,
        QStringLiteral("Remote ownership was invalidated by a local deletion"));
    for (int trial = 0; trial < 12; ++trial) {
        footprints.clear();
        for (int index = 0; index < 8; ++index) {
            QTransform placement;
            placement.translate(index * 13 + trial, index % 3 * 17);
            placement.rotate(index * 9 + trial);
            placement.shear(trial * 0.01, 0);
            footprints.push_back({placement.map(target.front())});
        }
        footprints[3] = gui::catalog::subtract(footprints[3], hole);
        ownership.synchronize(footprints);
        for (int index = 0; index < footprints.size(); ++index) {
            for (const QVector<int> &members : {QVector<int>{index}, QVector<int>{index, (index + 1) % 8}}) {
                Polygons chosen, remaining;
                for (int member = 0; member < footprints.size(); ++member) {
                    (members.contains(member) ? chosen : remaining) += footprints[member];
                }
                const auto expected = gui::catalog::subtract(gui::catalog::unite(chosen), gui::catalog::unite(remaining));
                const auto actual = ownership.exclusive(members);
                require(gui::catalog::area(gui::catalog::subtract(expected, actual))
                    + gui::catalog::area(gui::catalog::subtract(actual, expected)) < 1e-4,
                    QStringLiteral("Local ownership differs from full-union subtraction"));
            }
        }
    }
    const BoundaryModel model(target, 1);
    const auto targetMetrics = model.measure(target);
    const auto exact = reductionState(target, target, target, {}, model, 0.5);
    const auto broken = reductionState(gui::catalog::subtract(target, hole), target, target, {}, model, 0.5);
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
    options.useGpu = false;
    const gui::PenFillRequest request{{}, {polygonLoop({{0, 0}, {20, 0}, {20, 8}, {100, 8},
        {100, 0}, {120, 0}, {120, 20}, {100, 20}, {100, 12}, {20, 12}, {20, 20}, {0, 20}})}};
    const auto result = gui::compact::fillRegion(request, {*square}, options);
    if (result.fill.placements.size() >= options.initialPlacements.size()) {
        QTextStream(stdout) << result.fill.error << '\n' << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    }
    require(!result.fill.cancelled && !result.fill.error.isEmpty() && !result.fill.placements.isEmpty()
        && result.fill.placements.size() < options.initialPlacements.size(),
        QStringLiteral("Remote quality failure prevented duplicate removal"));
    require(result.diagnostics.value("approximateReductions").toInt()
        + result.diagnostics.value("exactReductions").toInt() > 0, QStringLiteral("Reduction was not recorded"));
    require(result.diagnostics.value("exactReductions").toInt() > 0,
        QStringLiteral("Duplicate removal did not use exact-union verification"));
    const auto stages = result.diagnostics.value("stageWork").toObject();
    int used = 0;
    for (const auto &name : {"recognition", "repair", "spatialReduction", "exposedReduction", "polish",
             "smallSupportCompaction", "clusterCompaction", "contourPolish", "neighborCompaction"}) {
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
    options.useGpu = true;
    const auto gpuResult = gui::compact::fillRegion(request, {*square}, options);
    require(gpuResult.fill.placements.size() < options.initialPlacements.size()
        && gpuResult.diagnostics.value("exactReductions").toInt() > 0,
        QStringLiteral("Rejected GPU refinement retained duplicate seed placements"));
    int gpuUsed = 0;
    const auto gpuStages = gpuResult.diagnostics.value("stageWork").toObject();
    for (const auto &name : {"recognition", "gpuPipeline", "repair", "spatialReduction", "exposedReduction", "polish",
             "smallSupportCompaction", "clusterCompaction", "contourPolish", "neighborCompaction"}) {
        const auto stage = gpuStages.value(name).toObject();
        if (stage.isEmpty()) {
            continue;
        }
        require(stage.value("start").toInt() == gpuUsed && stage.value("evaluations").toInt() >= 0,
            QStringLiteral("GPU work accounting is discontinuous"));
        gpuUsed += stage.value("evaluations").toInt();
        require(gpuUsed <= stage.value("ceiling").toInt(), QStringLiteral("GPU stage budget overrun"));
    }
    require(gpuUsed == gpuResult.diagnostics.value("evaluations").toInt()
        && gpuUsed <= options.evaluationBudget, QStringLiteral("GPU total work accounting differs"));
    QTextStream(stdout) << "Stage budgets, local reductions, topology and baseline checks passed\n";
}

void interiorCoverageTests(const QVector<gui::catalog::Primitive> &catalog) {
    const auto square = std::find_if(catalog.cbegin(), catalog.cend(), [](const auto &entry) {
        return entry.shape.shapeId == 101;
    });
    require(square != catalog.cend(), QStringLiteral("Missing interior repair square"));
    const auto rectangle = [&](double left, double top, double right, double bottom) {
        gui::PenPlacement placement;
        placement.shapeId = 101;
        placement.transform.translate(left, top);
        placement.transform.scale((right - left) / square->shape.bounds.width(),
            (bottom - top) / square->shape.bounds.height());
        placement.transform.translate(-square->shape.bounds.left(), -square->shape.bounds.top());
        return placement;
    };
    const gui::PenFillRequest request{{}, {polygonLoop({{0, 0}, {100, 0}, {100, 100}, {0, 100}}),
        polygonLoop({{60, 60}, {80, 60}, {80, 80}, {60, 80}}, gui::PenLoopKind::Cutout)}};
    for (bool useGpu : {false, true}) {
        gui::compact::FillOptions options;
        options.initialPlacements = {rectangle(0, 0, 100, 29.9), rectangle(0, 30.1, 100, 60),
            rectangle(0, 29.9, 29.9, 30.1), rectangle(30.1, 29.9, 100, 30.1),
            rectangle(0, 80, 100, 100), rectangle(0, 60, 60, 80), rectangle(80, 60, 100, 80)};
        options.evaluationBudget = 1600;
        options.shapeBudget = options.initialPlacements.size();
        options.retainFailedFill = true;
        options.useGpu = useGpu;
        const auto result = gui::compact::fillRegion(request, {*square}, options);
        QTextStream(stdout) << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n';
        require(!result.fill.cancelled && result.diagnostics.contains("missingInteriorArea")
            && result.diagnostics.value("missingInteriorArea").toDouble() == 0.0,
            QStringLiteral("Interior repair left a hole smaller than the inward allowance"));
        require(result.diagnostics.value("missingBeyondInward").toDouble() == 0.0
            && result.diagnostics.value("evaluations").toInt() <= options.evaluationBudget
            && result.fill.placements.size() <= options.initialPlacements.size(),
            QStringLiteral("Interior repair exceeded the work or placement budget"));
        gui::catalog::Polygons coverage;
        for (const auto &placement : result.fill.placements) {
            coverage += gui::catalog::mapped(square->shape, gui::catalog::emittedTransform(placement.transform));
        }
        require(gui::catalog::intersect(gui::catalog::unite(coverage),
            {QPolygonF({{62.1, 62.1}, {77.9, 62.1}, {77.9, 77.9}, {62.1, 77.9}})}).isEmpty(),
            QStringLiteral("Interior repair filled an intentional cutout beyond the outward allowance"));
    }
    QTextStream(stdout) << "Exact interior coverage, nearest-shape repair and cutout checks passed\n";
}

void qualityExamples() {
    using gui::catalog::Polygons;
    const Polygons target{QPolygonF({{0, 0}, {400, 0}, {400, 400}, {0, 400}})};
    const gui::compact::BoundaryModel boundary(target, 1.0);
    const auto reference = boundary.measure(target);
    const auto serialize = [](const Polygons &polygons) {
        QJsonArray result;
        for (const auto &polygon : polygons) {
            QJsonArray points;
            for (const auto &point : polygon) {
                points.push_back(QJsonArray{point.x(), point.y()});
            }
            result.push_back(points);
        }
        return result;
    };
    QJsonArray examples;
    const QVector<Polygons> additions{{}, {QPolygonF({{-1.5, 0.625}, {0.625, -1.5}, {0.625, 0.625}})},
        {QPolygonF({{198, 1}, {200, -1}, {202, 1}})}};
    const QStringList names{"All checks passed", "Sharp-corner position failed", "Contour continuity failed"};
    for (int index = 0; index < additions.size(); ++index) {
        const auto coverage = gui::catalog::unite(target + additions[index]);
        const auto metrics = boundary.measure(coverage);
        QStringList failures;
        if (boundary.energy(metrics) > boundary.perimeter() * 0.025
            || metrics.maximumExcessTurn > 0.6 || metrics.cornerDefects > boundary.perimeter() / 80) {
            failures.push_back("contour continuity");
        }
        if (metrics.maximumCornerDistance > 0.5) {
            failures.push_back("sharp-corner position");
        }
        const QVector<QStringList> expected{{}, {"sharp-corner position"}, {"contour continuity"}};
        require(failures == expected[index], QStringLiteral("Quality example does not isolate its named failure"));
        require(gui::catalog::subtract(target, coverage).isEmpty()
            && gui::catalog::subtract(coverage, gui::catalog::expanded(target, 2.0)).isEmpty()
            && metrics.components == reference.components && metrics.holes == reference.holes,
            QStringLiteral("Quality example damaged coverage, the envelope or topology"));
        examples.push_back(QJsonObject{{"name", names[index]}, {"target", serialize(target)},
            {"coverage", serialize(coverage)}, {"metrics", boundary.diagnostics(metrics)},
            {"failedChecks", QJsonArray::fromStringList(failures)}});
    }
    QTextStream(stdout) << QJsonDocument(examples).toJson(QJsonDocument::Compact) << '\n';
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
    const auto exact = reductionState(target, target, target, {}, reference, 0.5);
    const Polygons hole{QPolygonF({{20, 20}, {30, 20}, {30, 30}, {20, 30}})};
    const auto broken = reductionState(gui::catalog::subtract(target, hole), target, target, {}, reference, 0.5);
    require(!preservesCoverage(broken, exact, metrics, 0.5), QStringLiteral("Polishing can open an interior hole"));
    require(preservesCoverage(exact, broken, metrics, 0.5), QStringLiteral("Coverage repair was rejected"));
    const Polygons rectangle{QPolygonF({{0, 0}, {100, 0}, {100, 100}, {0, 100}})};
    const Polygons interior{QPolygonF({{1, 1}, {99, 1}, {99, 99}, {1, 99}})};
    const BoundaryModel rectangleReference(rectangle, 1);
    const auto rectangleMetrics = rectangleReference.measure(rectangle);
    const Polygons slit{QPolygonF({{49.7, 0}, {50.3, 0}, {50.3, 100}, {49.7, 100}})};
    const auto split = reductionState(gui::catalog::subtract(rectangle, slit), rectangle, rectangle, {}, rectangleReference, 0.5);
    require(gui::catalog::area(split.deepMissing) < 1e-6 && split.metrics.components == 2,
        QStringLiteral("Shallow crack fixture did not separate observed components"));
    require(gui::catalog::area(repairResidual(split, rectangle, interior, rectangleMetrics)) > 50,
        QStringLiteral("Deep-coverage tolerance hid a visible disconnected crack"));
    const Polygons smallHole{QPolygonF({{49.7, 40}, {50.3, 40}, {50.3, 60}, {49.7, 60}})};
    const auto perforated = reductionState(gui::catalog::subtract(rectangle, smallHole), rectangle, rectangle, {}, rectangleReference, 0.5);
    require(gui::catalog::area(perforated.deepMissing) < 1e-6 && perforated.metrics.holes == 1
        && gui::catalog::area(repairResidual(perforated, rectangle, interior, rectangleMetrics)) > 10,
        QStringLiteral("Shallow unintended hole was not scheduled for repair"));
    const Polygons notch{QPolygonF({{49.7, 0}, {50.3, 0}, {50.3, 60}, {49.7, 60}})};
    const auto cracked = reductionState(gui::catalog::subtract(rectangle, notch), rectangle, rectangle, {}, rectangleReference, 0.5);
    require(cracked.metrics.components == 1 && cracked.metrics.holes == 0
        && gui::catalog::area(repairResidual(cracked, rectangle, interior, rectangleMetrics)) > 30,
        QStringLiteral("Open interior crack was ignored because topology counts matched"));
    const Polygons subpixelSlit{QPolygonF({{49.95, 0}, {50.05, 0}, {50.05, 100}, {49.95, 100}})};
    const auto subpixel = reductionState(gui::catalog::subtract(rectangle, subpixelSlit), rectangle, rectangle, {}, rectangleReference, 0.5);
    require(gui::catalog::area(repairResidual(subpixel, rectangle, interior, rectangleMetrics)) < 1e-6,
        QStringLiteral("Observation-invisible crack generated a repair task"));
    const auto intendedTarget = gui::catalog::subtract(rectangle, smallHole);
    const BoundaryModel intendedReference(intendedTarget, 1);
    const auto intended = reductionState(intendedTarget, intendedTarget, intendedTarget, {}, intendedReference, 0.5);
    require(repairResidual(intended, intendedTarget, interior, intendedReference.measure(intendedTarget)).isEmpty(),
        QStringLiteral("Intentional cutout generated a repair task"));
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
    require(!result.diagnostics.value("connectorDiagnostics").toObject().isEmpty()
        && result.diagnostics.contains("feasibleCheckpoints") && result.diagnostics.contains("feasibleRestores"),
        QStringLiteral("Repair diagnostics are missing"));
    require(result.diagnostics.value("residualConnectors").toInt() > 0,
        QStringLiteral("Connected compound repair was not exercised"));
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
    options.shapeBudget = 1;
    const auto limited = gui::compact::fillRegion(request, {*square}, options);
    require(!limited.fill.placements.isEmpty() && limited.fill.placements.size() <= options.shapeBudget
        && limited.diagnostics.value("evaluations").toInt() <= options.evaluationBudget,
        QStringLiteral("Shape-limited repair lost the retained cover or exceeded work"));
    const auto placedRectangle = [&](const QRectF &frame) {
        gui::PenPlacement piece;
        piece.shapeId = 101;
        piece.transform.translate(frame.left(), frame.top());
        piece.transform.scale(frame.width() / bounds.width(), frame.height() / bounds.height());
        piece.transform.translate(-bounds.left(), -bounds.top());
        return piece;
    };
    FillOptions leewayOptions;
    leewayOptions.initialPlacements = {placedRectangle({0, 0, 100, 100})};
    leewayOptions.leeway = {QPolygonF({{40, -10}, {60, -10}, {60, 110}, {40, 110}})};
    leewayOptions.evaluationBudget = 1200;
    leewayOptions.retainFailedFill = true;
    const gui::PenFillRequest leewayRequest{{}, {polygonLoop({{0, 0}, {100, 0}, {100, 100}, {0, 100}})}};
    const auto leewayRegion = gui::catalog::leewayAdjustedRegion(gui::catalog::buildRegion(leewayRequest, {}),
        leewayOptions.leeway, leewayOptions.boundaryAllowance);
    const BoundaryModel leewayBoundary(leewayRegion.visible, 1.0);
    const auto leewayState = reductionState(rectangle, leewayRegion.required, leewayRegion.visible,
        leewayOptions.leeway, leewayBoundary, 0.5);
    require(leewayState.metrics.components == 2 && leewayState.missingArea == 0.0
        && leewayState.spillArea == 0.0 && leewayState.deepMissing.isEmpty(),
        QStringLiteral("Leeway must affect visible metrics without losing required coverage"));
    const auto leewayFill = gui::compact::fillRegion(leewayRequest, {*square}, leewayOptions);
    require(leewayFill.diagnostics.value("approximationVerified").toBool()
        && leewayFill.diagnostics.value("boundaryQuality").toObject().value("components").toInt() == 2,
        QStringLiteral("Merged Compact Fit lost leeway-aware acceptance"));
    auto leewaySeedOptions = leewayOptions;
    leewaySeedOptions.initialPlacements.clear();
    leewaySeedOptions.useGpu = false;
    const auto leewaySeeded = gui::profile::fillRegion(leewayRequest, {*square}, leewaySeedOptions);
    require(!leewaySeeded.fill.placements.isEmpty()
        && leewaySeeded.diagnostics.value("missingBeyondInward").toDouble() < 1e-6
        && leewaySeeded.diagnostics.value("outsideEnvelope").toDouble() < 1e-6,
        QStringLiteral("Profile initialization lost required coverage with leeway"));
    const gui::PenFillRequest separatedRequest{{}, {polygonLoop({{0, 0}, {1240, 0}, {1240, 40}, {0, 40}}),
        polygonLoop({{600, 10}, {620, 10}, {620, 30}, {600, 30}}, gui::PenLoopKind::Cutout)}};
    FillOptions separatedOptions;
    separatedOptions.useGpu = false;
    for (int index = 0; index < 31; ++index) {
        const double x = index * 40.0;
        if (index == 15) {
            separatedOptions.initialPlacements += {placedRectangle({600, 0, 40, 10}),
                placedRectangle({600, 30, 40, 10}), placedRectangle({620, 10, 20, 20})};
        } else {
            separatedOptions.initialPlacements.push_back(placedRectangle({x, 0, 40, 40}));
        }
    }
    separatedOptions.shapeBudget = separatedOptions.initialPlacements.size();
    separatedOptions.evaluationBudget = 1200;
    separatedOptions.retainFailedFill = true;
    const auto separated = gui::compact::fillRegion(separatedRequest, {*square}, separatedOptions);
    QTextStream(stdout) << "Separated scoring " << QJsonDocument(separated.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    require(separated.diagnostics.value("localContexts").toInt() > 0
        && separated.diagnostics.value("localEvaluations").toInt() > 0
        && separated.diagnostics.value("boundaryQuality").toObject().value("components").toInt() == 1
        && separated.diagnostics.value("boundaryQuality").toObject().value("holes").toInt() == 1
        && separated.diagnostics.value("missingBeyondInward").toDouble() < 1e-6
        && separated.diagnostics.value("outsideEnvelope").toDouble() < 1e-6,
        QStringLiteral("Local scoring lost remote coverage or did not run"));
    const auto separatedRepeat = gui::compact::fillRegion(separatedRequest, {*square}, separatedOptions);
    require(separatedRepeat.fill.placements.size() == separated.fill.placements.size(),
        QStringLiteral("Local scoring count is not deterministic"));
    for (int index = 0; index < separated.fill.placements.size(); ++index) {
        require(separatedRepeat.fill.placements[index].shapeId == separated.fill.placements[index].shapeId
            && separatedRepeat.fill.placements[index].transform == separated.fill.placements[index].transform,
            QStringLiteral("Local scoring transforms are not deterministic"));
    }
    const gui::PenFillRequest pairedRequest{{}, {polygonLoop({{0, 0}, {100, 0}, {100, 100}, {0, 100}}),
        polygonLoop({{20, 20}, {80, 20}, {80, 80}, {20, 80}}, gui::PenLoopKind::Cutout)}};
    FillOptions pairedOptions;
    pairedOptions.useGpu = false;
    pairedOptions.initialPlacements = {placedRectangle({0, 0, 100, 20}), placedRectangle({0, 80, 100, 20}),
        placedRectangle({80, 20, 20, 60}), placedRectangle({0, 20, 20, 29}), placedRectangle({0, 51, 20, 29})};
    pairedOptions.shapeBudget = pairedOptions.initialPlacements.size();
    pairedOptions.evaluationBudget = 160;
    pairedOptions.boundaryAllowance = 0.01;
    pairedOptions.retainFailedFill = true;
    const auto paired = gui::compact::fillRegion(pairedRequest, {*square}, pairedOptions);
    QTextStream(stdout) << "Paired repair " << QJsonDocument(paired.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    require(paired.diagnostics.value("repairGroupReplacements").toInt() > 0
        && paired.diagnostics.value("residualInsertions").toInt() == 0
        && paired.diagnostics.value("approximationVerified").toBool()
        && paired.diagnostics.value("evaluations").toInt() <= pairedOptions.evaluationBudget
        && paired.fill.placements.size() < pairedOptions.shapeBudget
        && paired.diagnostics.value("missingBeyondInward").toDouble() < 1e-6
        && paired.diagnostics.value("outsideEnvelope").toDouble() < 1e-6,
        QStringLiteral("Neighborhood replacement failed to close a gap while reducing shapes"));
    const auto pairedRepeat = gui::compact::fillRegion(pairedRequest, {*square}, pairedOptions);
    require(pairedRepeat.fill.placements.size() == paired.fill.placements.size(),
        QStringLiteral("Paired repair count is not deterministic"));
    for (int index = 0; index < paired.fill.placements.size(); ++index) {
        require(pairedRepeat.fill.placements[index].shapeId == paired.fill.placements[index].shapeId
            && pairedRepeat.fill.placements[index].transform == paired.fill.placements[index].transform,
            QStringLiteral("Paired repair transforms are not deterministic"));
    }
    const auto circle = std::find_if(catalog.cbegin(), catalog.cend(), [](const auto &entry) {
        return entry.shape.shapeId == 102;
    });
    require(circle != catalog.cend(), QStringLiteral("Missing broad repair shape"));
    FillOptions patchOptions;
    patchOptions.useGpu = false;
    patchOptions.initialPlacements = {placedRectangle({0, 0, 100, 20}),
        placedRectangle({0, 80, 100, 20}), placedRectangle({80, 20, 20, 60}),
        placedRectangle({0, 20, 20, 60})};
    for (int index = 0; index < 8; ++index) {
        patchOptions.initialPlacements.push_back(placedRectangle({5.0 + 4.0 * (index % 2),
            25.0 + 4.0 * (index / 2), 2.0, 2.0}));
    }
    patchOptions.evaluationBudget = 6000;
    patchOptions.retainFailedFill = true;
    const auto consolidated = gui::compact::fillRegion(pairedRequest, {*square, *circle}, patchOptions);
    require(consolidated.diagnostics.value("broadRepairReplacements").toInt() > 0
        && consolidated.fill.placements.size() == 4
        && consolidated.diagnostics.value("approximationVerified").toBool(),
        QStringLiteral("Broad patch consolidation did not replace a cluster with one shape"));
    const gui::PenFillRequest splitRingRequest{{}, {polygonLoop({{0, 0}, {100, 0}, {100, 100}, {0, 100}}),
        polygonLoop({{20, 20}, {80, 20}, {80, 80}, {20, 80}}, gui::PenLoopKind::Cutout)}};
    FillOptions splitOptions;
    splitOptions.useGpu = false;
    splitOptions.initialPlacements = {placedRectangle({0, 0, 100, 20}), placedRectangle({0, 80, 100, 20}),
        placedRectangle({80, 20, 20, 60}), placedRectangle({0, 20, 20, 29.7}), placedRectangle({0, 50.3, 20, 29.7})};
    splitOptions.shapeBudget = splitOptions.initialPlacements.size();
    splitOptions.evaluationBudget = 6000;
    splitOptions.retainFailedFill = true;
    const auto joinedRing = gui::compact::fillRegion(splitRingRequest, {*square}, splitOptions);
    QTextStream(stdout) << "Joined ring " << QJsonDocument(joinedRing.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    const auto joinedMetrics = joinedRing.diagnostics.value("boundaryQuality").toObject();
    require(!joinedRing.fill.placements.isEmpty() && joinedRing.fill.placements.size() <= splitOptions.shapeBudget
        && joinedRing.diagnostics.value("evaluations").toInt() <= splitOptions.evaluationBudget,
        QStringLiteral("Connected repair exceeded an exhausted shape budget"));
    require(joinedMetrics.value("components").toInt() == 1 && joinedMetrics.value("holes").toInt() == 1
        && joinedRing.diagnostics.value("repairResidualArea").toDouble() < 1e-6,
        QStringLiteral("Shallow ring crack remained after shape-limited repair"));
    require(joinedRing.diagnostics.value("feasibleCheckpoints").toInt() > 0
        && joinedRing.diagnostics.value("feasibleRestores").toInt() > 0,
        QStringLiteral("Feasible intermediate restoration was not exercised"));
    auto poolOptions = splitOptions;
    poolOptions.initialPlacements = {placedRectangle({0, 0, 50, 20}), placedRectangle({50, 0, 50, 20}),
        placedRectangle({0, 80, 50, 20}), placedRectangle({50, 80, 50, 20}),
        placedRectangle({0, 20, 20, 30}), placedRectangle({0, 50, 20, 30}),
        placedRectangle({80, 20, 20, 30}), placedRectangle({80, 50, 20, 30})};
    poolOptions.shapeBudget = poolOptions.initialPlacements.size();
    auto reusePool = std::make_shared<QVector<ReusableCandidate>>();
    for (const QRectF &frame : {QRectF(0, 0, 100, 20), QRectF(0, 80, 100, 20),
            QRectF(0, 20, 20, 60), QRectF(80, 20, 20, 60)}) {
        const auto placement = placedRectangle(frame);
        reusePool->push_back({placement, gui::catalog::mapped(square->shape, placement.transform), frame});
    }
    poolOptions.replacementCandidates = reusePool;
    const auto reusedRing = gui::compact::fillRegion(splitRingRequest, {*square}, poolOptions);
    QTextStream(stdout) << "Reused ring " << QJsonDocument(reusedRing.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    require(reusedRing.diagnostics.value("reusedMerges").toInt() > 0
        && reusedRing.diagnostics.value("reusedPairs").toInt() > 0
        && reusedRing.fill.placements.size() < poolOptions.initialPlacements.size()
        && reusedRing.diagnostics.value("approximationVerified").toBool(),
        QStringLiteral("Candidate reuse failed to reduce a verified cover"));
    const auto reuseRepeat = gui::compact::fillRegion(splitRingRequest, {*square}, poolOptions);
    require(reuseRepeat.fill.placements.size() == reusedRing.fill.placements.size(),
        QStringLiteral("Candidate reuse count is not deterministic"));
    for (int index = 0; index < reusedRing.fill.placements.size(); ++index) {
        require(reuseRepeat.fill.placements[index].transform == reusedRing.fill.placements[index].transform,
            QStringLiteral("Candidate reuse transform is not deterministic"));
    }
    auto uncachedOptions = poolOptions;
    uncachedOptions.replacementCandidates.reset();
    const auto uncachedRing = gui::compact::fillRegion(splitRingRequest, {*square}, uncachedOptions);
    QTextStream(stdout) << "Uncached ring " << QJsonDocument(uncachedRing.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    require(uncachedRing.diagnostics.value("approximationVerified").toBool()
        && uncachedRing.fill.placements.size() < uncachedOptions.initialPlacements.size()
        && uncachedRing.diagnostics.value("boundaryQuality").toObject().value("energy").toDouble() == 0,
        QStringLiteral("Exact final reduction failed to preserve the uncached cover"));
    const auto exactRing = reduceExactCoverage(uncachedOptions.initialPlacements, {*square}, {});
    require(exactRing.placements.size() == 4, QStringLiteral("Exact envelope reduction failed to join split sides"));
    const auto exactCoverage = [&](const QVector<gui::PenPlacement> &placements) {
        Polygons polygons;
        for (const auto &piece : placements) {
            polygons += gui::catalog::mapped(square->shape, piece.transform);
        }
        return gui::catalog::unite(polygons);
    };
    const auto ringBefore = exactCoverage(uncachedOptions.initialPlacements);
    const auto ringAfter = exactCoverage(exactRing.placements);
    require(gui::catalog::subtract(ringBefore, ringAfter).isEmpty()
        && gui::catalog::subtract(ringAfter, ringBefore).isEmpty(),
        QStringLiteral("Exact reduction changed the union or filled an intended hole"));
    const auto exactRepeat = reduceExactCoverage(uncachedOptions.initialPlacements, {*square}, {});
    require(exactRepeat.placements.size() == exactRing.placements.size(), QStringLiteral("Exact reduction count changed"));
    for (int index = 0; index < exactRing.placements.size(); ++index) {
        require(exactRepeat.placements[index].transform == exactRing.placements[index].transform,
            QStringLiteral("Exact reduction transforms changed"));
    }
    const auto exactCancelled = reduceExactCoverage(uncachedOptions.initialPlacements, {*square}, {}, [] { return true; });
    require(exactCancelled.placements.size() == uncachedOptions.initialPlacements.size(),
        QStringLiteral("Cancelled exact reduction changed the cover"));
    QTextStream(stdout) << "Exact ring " << QJsonDocument(exactRing.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    const auto native = std::find_if(catalog.cbegin(), catalog.cend(), [](const auto &entry) { return entry.shape.shapeId == 2117; });
    require(native != catalog.cend(), QStringLiteral("Missing curved exact-reduction shape"));
    const auto primitive = [&](int id, const Polygons &polygons) {
        gui::catalog::Primitive entry;
        entry.shape.shapeId = id;
        entry.shape.contours = polygons;
        entry.shape.silhouette = gui::catalog::painterPath(polygons);
        entry.shape.bounds = entry.shape.silhouette.boundingRect();
        entry.shape.area = gui::catalog::area(polygons);
        return entry;
    };
    const auto nativeCoverage = gui::catalog::mapped(native->shape, QTransform());
    const auto nativeBounds = native->shape.bounds;
    const Polygons leftHalf{QPolygonF(QRectF(nativeBounds.left(), nativeBounds.top(),
        nativeBounds.width() / 2, nativeBounds.height()))};
    const auto left = primitive(900001, gui::catalog::intersect(nativeCoverage, leftHalf));
    const auto right = primitive(900002, gui::catalog::subtract(nativeCoverage, leftHalf));
    gui::PenPlacement leftPlacement, rightPlacement, nativePlacement;
    leftPlacement.shapeId = left.shape.shapeId;
    rightPlacement.shapeId = right.shape.shapeId;
    nativePlacement.shapeId = native->shape.shapeId;
    const QVector<ReusableCandidate> curvedPool{{nativePlacement, nativeCoverage, nativeBounds}};
    const auto curvedExact = reduceExactCoverage({leftPlacement, rightPlacement}, {left, right, *native}, curvedPool);
    require(curvedExact.placements.size() == 1 && curvedExact.placements.front().shapeId == native->shape.shapeId,
        QStringLiteral("Exact candidate replacement could not use a non-basic catalog shape"));
    const auto firstShared = primitive(900003, {QPolygonF(QRectF(0, 0, 60, 20))});
    const auto secondShared = primitive(900004, {QPolygonF(QRectF(40, 0, 60, 20))});
    const auto sharedHole = primitive(900005, {QPolygonF(QRectF(0, 0, 40, 20)), QPolygonF(QRectF(60, 0, 40, 20))});
    leftPlacement.shapeId = firstShared.shape.shapeId;
    rightPlacement.shapeId = secondShared.shape.shapeId;
    nativePlacement.shapeId = sharedHole.shape.shapeId;
    const auto sharedExact = reduceExactCoverage({leftPlacement, rightPlacement}, {firstShared, secondShared, sharedHole},
        {{nativePlacement, sharedHole.shape.contours, sharedHole.shape.bounds}});
    require(sharedExact.placements.size() == 2 && sharedExact.diagnostics.value("merges").toInt() == 0,
        QStringLiteral("Individual ownership allowed loss of jointly owned support"));
    const auto remote = primitive(900006, {QPolygonF(QRectF(120, 0, 20, 20))});
    const auto sharedReplacement = primitive(900007, sharedHole.shape.contours + remote.shape.contours);
    gui::PenPlacement remotePlacement;
    remotePlacement.shapeId = remote.shape.shapeId;
    nativePlacement.shapeId = sharedReplacement.shape.shapeId;
    const auto restoredExact = reduceExactCoverage({leftPlacement, rightPlacement, remotePlacement},
        {firstShared, secondShared, remote, sharedReplacement},
        {{nativePlacement, sharedReplacement.shape.contours, sharedReplacement.shape.bounds}});
    require(restoredExact.placements.size() == 2 && restoredExact.diagnostics.value("merges").toInt() == 1,
        QStringLiteral("Shared-support restoration could not retain a smaller exact replacement"));
    for (int trial = 0; trial < 16; ++trial) {
        auto transformed = uncachedOptions.initialPlacements;
        QTransform frame;
        frame.translate(trial * 7.25, trial * -3.5);
        frame.rotate(trial * 11.25);
        frame.shear(trial * 0.03125, 0);
        frame.scale(trial % 2 == 0 ? -1 : 1, 0.5 + trial * 0.125);
        for (auto &piece : transformed) {
            piece.transform = gui::catalog::emittedTransform(piece.transform * frame);
        }
        const auto reduced = reduceExactCoverage(transformed, {*square}, {});
        const auto transformedBefore = exactCoverage(transformed);
        const auto transformedAfter = exactCoverage(reduced.placements);
        require(reduced.placements.size() <= transformed.size()
            && gui::catalog::subtract(transformedBefore, transformedAfter).isEmpty()
            && gui::catalog::subtract(transformedAfter, transformedBefore).isEmpty(),
            QStringLiteral("Exact reduction changed a reflected or affine-transformed union"));
    }
    auto stalePool = curvedPool;
    stalePool.front().placement.transform.translate(10000, 10000);
    leftPlacement.shapeId = left.shape.shapeId;
    rightPlacement.shapeId = right.shape.shapeId;
    const auto staleExact = reduceExactCoverage({leftPlacement, rightPlacement}, {left, right, *native}, stalePool);
    require(staleExact.placements.size() == 2, QStringLiteral("Exact reduction trusted stale cached geometry"));
    auto forgedPool = std::make_shared<QVector<ReusableCandidate>>(*reusePool);
    for (auto &candidate : *forgedPool) {
        candidate.placement.transform.translate(10000, 10000);
    }
    poolOptions.replacementCandidates = forgedPool;
    const auto forgedResult = gui::compact::fillRegion(splitRingRequest, {*square}, poolOptions);
    require(forgedResult.diagnostics.value("reusedMerges").toInt() == 0
        && forgedResult.diagnostics.value("outsideEnvelope").toDouble() < 1e-6,
        QStringLiteral("Cached geometry bypassed emitted-transform verification"));
    QTextStream(stdout) << "Residual repair, coverage guards, reference calibration and boundary retention passed\n";
}

#ifdef FLS_HAS_CUDA
void gpuRasterRankTests() {
    const std::vector<gui::compact::gpu::MaskWord> maskWords{
        {0, 0b0011}, {1, 0b0001}, {0, 0b0110}, {1, 0b0010}};
    const std::vector<int> maskOffsets{0, 2, 4};
    const std::vector<std::uint64_t> missing{0b0111, 0b0011};
    auto bitmask = gui::compact::gpu::createBitmaskCover(maskWords, maskOffsets, missing, 1);
    std::vector<gui::compact::gpu::MaskCounts> maskCounts;
    require(bitmask && bitmask->error().empty() && bitmask->score(&maskCounts)
        && maskCounts.size() == 2 && maskCounts[0].cells == 2
        && maskCounts[0].boundary == 1 && maskCounts[1].cells == 2
        && maskCounts[1].boundary == 1 && bitmask->remove(0)
        && bitmask->score(&maskCounts) && maskCounts[0].cells == 0
        && maskCounts[0].boundary == 0 && maskCounts[1].cells == 1
        && maskCounts[1].boundary == 1,
        QStringLiteral("CUDA bitmask marginal counts or coverage update failed"));
    gui::compact::gpu::MaskGeometry maskGeometry;
    maskGeometry.points = {{0, 0}, {4, 0}, {4, 4}, {0, 4},
                           {4, 0}, {8, 0}, {8, 4}, {4, 4}};
    maskGeometry.loops = {{0, 4}, {4, 4}};
    maskGeometry.pieces = {{0, 1}, {1, 1}};
    maskGeometry.bounds = {{0, 0, 4, 4}, {4, 0, 8, 4}};
    std::vector<std::uint64_t> rasterMasks;
    std::vector<std::uint64_t> witnessMasks;
    std::string rasterError;
    require(gui::compact::gpu::rasterizeBitmasks(maskGeometry,
        {0, 0, 1, 8, 4}, {{0.5, 0.5}, {4.5, 0.5}, {7.5, 3.5}},
        &rasterMasks, &witnessMasks, &rasterError)
        && rasterMasks == std::vector<std::uint64_t>{0x0f0f0f0f, 0xf0f0f0f0}
        && witnessMasks == std::vector<std::uint64_t>{0b001, 0b110},
        QStringLiteral("CUDA profile grid rasterization changed pixel coverage"));
    require(gui::compact::gpu::rasterizeBitmasks(maskGeometry,
        {0, 0, 1, 8, 4}, {{7.5, 3.5}, {0.5, 0.5}, {4.5, 0.5}},
        &rasterMasks, &witnessMasks, &rasterError)
        && rasterMasks == std::vector<std::uint64_t>{0x0f0f0f0f, 0xf0f0f0f0}
        && witnessMasks == std::vector<std::uint64_t>{0b010, 0b101},
        QStringLiteral("CUDA witness ordering changed pixel coverage"));
    const gui::catalog::Polygons envelopePolygons{
        QPolygonF{{0, 0}, {20, 0}, {20, 20}, {0, 20}},
        QPolygonF{{6, 6}, {6, 14}, {14, 14}, {14, 6}}};
    const QPolygonF probePolygon{{0, 0}, {2, 0}, {2, 1}, {0, 1}};
    const auto gpuMask = [](const gui::catalog::Polygons &polygons) {
        gui::compact::gpu::MaskGeometry geometry;
        for (const QPolygonF &polygon : polygons) {
            const int offset = static_cast<int>(geometry.points.size());
            for (const QPointF &point : polygon) {
                geometry.points.push_back({point.x(), point.y()});
            }
            geometry.loops.push_back({offset, static_cast<int>(polygon.size())});
        }

        return geometry;
    };
    std::vector<QTransform> probeTransforms;
    std::vector<gui::compact::gpu::MaskAffine> probeAffines;
    for (int y = -2; y <= 20; ++y) {
        for (int x = -2; x <= 20; ++x) {
            for (double angle : {0.0, 23.0, 71.0}) {
                QTransform transform;
                transform.translate(x + 0.25, y + 0.25);
                transform.rotate(angle);
                probeTransforms.push_back(transform);
                probeAffines.push_back({transform.m11(), transform.m12(),
                    transform.m21(), transform.m22(), transform.dx(), transform.dy()});
            }
        }
    }
    std::vector<std::uint8_t> proven;
    std::string proofError;
    require(gui::compact::gpu::proveTransformContainment(gpuMask(envelopePolygons),
        gpuMask({probePolygon}), probeAffines, 0.0001, &proven, &proofError),
        QStringLiteral("CUDA containment proof failed"));
    int provenCount = 0;
    for (int index = 0; index < static_cast<int>(proven.size()); ++index) {
        if (!proven[index]) {
            continue;
        }
        ++provenCount;
        require(gui::catalog::subtract({probeTransforms[index].map(probePolygon)},
            envelopePolygons).isEmpty(),
            QStringLiteral("CUDA containment proof accepted a clipped placement"));
    }
    require(provenCount > 0 && provenCount < static_cast<int>(proven.size()),
        QStringLiteral("CUDA containment proof did not distinguish placements"));
    const QPolygonF enclosingHole{{0, 0}, {12, 0}, {12, 12}, {0, 12}};
    require(gui::compact::gpu::proveTransformContainment(gpuMask(envelopePolygons),
        gpuMask({enclosingHole}), {{1, 0, 0, 1, 4, 4}}, 0.0001,
        &proven, &proofError) && proven == std::vector<std::uint8_t>{0},
        QStringLiteral("CUDA containment proof overlooked an interior hole"));
    const std::vector<gui::compact::gpu::MaskPoint> radiusProbes{
        {-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    std::vector<gui::compact::gpu::MaskPoint> radiusCenters;
    std::vector<gui::compact::gpu::MaskAffine> radiusTransforms;
    for (int y = 1; y <= 18; y += 2) {
        for (int x = 1; x <= 18; x += 2) {
            radiusCenters.push_back({static_cast<double>(x), static_cast<double>(y)});
            radiusTransforms.push_back({1, 0, 0, 1,
                static_cast<double>(x), static_cast<double>(y)});
        }
    }
    std::vector<double> parallelRadii;
    require(gui::compact::gpu::fitContainmentRadii(gpuMask(envelopePolygons),
        radiusProbes, radiusCenters, radiusTransforms, 8.0, 12, 0.01,
        &parallelRadii, &proofError),
        QStringLiteral("CUDA parallel radius search failed"));
    for (int index = static_cast<int>(radiusCenters.size()); index < 2048; ++index) {
        radiusCenters.push_back(radiusCenters[index % parallelRadii.size()]);
        radiusTransforms.push_back(radiusTransforms[index % parallelRadii.size()]);
    }
    std::vector<double> serialRadii;
    require(gui::compact::gpu::fitContainmentRadii(gpuMask(envelopePolygons),
        radiusProbes, radiusCenters, radiusTransforms, 8.0, 12, 0.01,
        &serialRadii, &proofError)
        && std::equal(parallelRadii.begin(), parallelRadii.end(), serialRadii.begin()),
        QStringLiteral("CUDA parallel and serial radius searches disagree"));
    const auto appendRectangle = [](gui::compact::gpu::Geometry *geometry,
                                    float left, float top, float right, float bottom) {
        const int pointOffset = static_cast<int>(geometry->points.size());
        const int loopOffset = static_cast<int>(geometry->loops.size());
        geometry->points.insert(geometry->points.end(), {
            {left, top}, {right, top}, {right, bottom}, {left, bottom}});
        geometry->loops.push_back({pointOffset, 4});
        geometry->pieces.push_back({loopOffset, 1});
    };
    gui::compact::gpu::Geometry target;
    appendRectangle(&target, 0, 0, 100, 100);
    gui::compact::gpu::Geometry pieces;
    appendRectangle(&pieces, 0, 0, 60, 100);
    appendRectangle(&pieces, 40, 0, 100, 100);
    appendRectangle(&pieces, 0, 0, 60, 100);
    auto ranker = gui::compact::gpu::createRasterRanker(
        target, target, target, target, target, 1.0);
    std::vector<double> first;
    std::vector<double> second;
    require(ranker && ranker->evaluate(pieces, &first)
        && ranker->evaluate(pieces, &second),
        QStringLiteral("CUDA ownership ranking failed"));
    require(first == second && first.size() == 3
        && first[0] == first[2] && first[1] > first[0],
        QStringLiteral("CUDA ownership ranking is unstable or incorrect"));
    gui::compact::gpu::Geometry coverage;
    appendRectangle(&coverage, 0, 0, 45, 100);
    gui::compact::gpu::Geometry candidates;
    appendRectangle(&candidates, 35, 0, 55, 100);
    appendRectangle(&candidates, 35, 0, 75, 100);
    gui::compact::gpu::Geometry primitive;
    appendRectangle(&primitive, 0, 0, 20, 100);
    gui::compact::gpu::Geometry current;
    appendRectangle(&current, 35, 0, 55, 100);
    const std::vector<gui::compact::gpu::Affine> transforms{
        {1, 0, 0, 1, 35, 0}, {2, 0, 0, 1, 35, 0}};
    require(ranker->prepareAdditionCoverage(coverage)
        && ranker->evaluateAdditions(candidates, &first)
        && ranker->evaluateTransforms(primitive, transforms, &second)
        && first == second && first.size() == 2 && first[1] > first[0],
        QStringLiteral("CUDA refinement ranking is unstable or incorrect"));
    gui::compact::gpu::Geometry splitCoverage;
    appendRectangle(&splitCoverage, 0, 0, 49, 100);
    appendRectangle(&splitCoverage, 50, 0, 100, 100);
    gui::compact::gpu::Geometry joinCandidates;
    appendRectangle(&joinCandidates, 49, 0, 50, 100);
    appendRectangle(&joinCandidates, 20, 0, 21, 100);
    const gui::compact::gpu::AdditionWeights joinWeights{
        0.0, 0.0, 0.0, 1.0, 0.0};
    require(ranker->prepareAdditionCoverage(splitCoverage)
        && ranker->evaluateAdditions(joinCandidates, &first, joinWeights)
        && first.size() == 2 && first[0] > first[1],
        QStringLiteral("CUDA refinement ranking did not prefer a closed crack"));
    require(ranker->preparePlacementCoverage(candidates)
        && ranker->prepareReplacementCoverage(current)
        && ranker->commitReplacement(current, coverage),
        QStringLiteral("CUDA persistent coverage update failed"));
    const auto stats = ranker->stats();
    require(stats.error.empty() && stats.calls == 2
        && stats.refinementPreparations == 3 && stats.refinementCalls == 3
        && stats.persistentPreparations == 1 && stats.persistentCommits == 1
        && stats.columns > 0
        && stats.rows > 0 && stats.cellSize >= 1.0,
        QStringLiteral("CUDA ownership ranking diagnostics are incomplete"));
    const gui::compact::gpu::AdditionWeights boundaryWeights{0.0, 0.0, 0.0, 0.0, 1.0};
    require(ranker->prepareAdditionCoverage(splitCoverage)
        && ranker->evaluateAdditions(joinCandidates, &first, boundaryWeights)
        && first[0] > 0.0 && first[1] == 0.0,
        QStringLiteral("CUDA boundary ranking did not credit removed crack edges"));
    gui::compact::gpu::Geometry outer;
    appendRectangle(&outer, -10, -10, 110, 110);
    gui::compact::gpu::Geometry outwardCandidates;
    appendRectangle(&outwardCandidates, 100, 10, 102, 90);
    appendRectangle(&outwardCandidates, 106, 10, 108, 90);
    auto outwardRanker = gui::compact::gpu::createRasterRanker(target, target, target,
        outer, target, 1.0);
    require(outwardRanker && outwardRanker->prepareAdditionCoverage({})
        && outwardRanker->evaluateAdditions(outwardCandidates, &first, boundaryWeights)
        && first[0] < 0.0 && first[1] < first[0],
        QStringLiteral("CUDA boundary ranking ignored exposed edges outside the target"));
    gui::compact::gpu::Geometry outwardPrimitive;
    appendRectangle(&outwardPrimitive, 0, 0, 2, 80);
    require(outwardRanker->evaluateTransforms(outwardPrimitive,
        {{1, 0, 0, 1, 100, 10}, {1, 0, 0, 1, 106, 10}}, &second, boundaryWeights)
        && first == second,
        QStringLiteral("CUDA transformed boundary ranking disagrees with emitted geometry"));

}
#endif

gui::PenFillRequest thinReferenceRequest(const QString &source, const gui::ShapeGeometryStore &geometry,
                                        gui::catalog::Polygons *exceptions = nullptr,
                                        QVector<gui::PenPlacement> *exceptionPlacements = nullptr) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto project = fls::decodeProjectDocument(file.readAll());
    gui::catalog::Polygons visible;
    QJsonObject shapeCounts;
    bool found = false;
    std::function<void(const fls::scene::Layer &, const QTransform &, bool)> collect;
    collect = [&](const auto &layer, const QTransform &parent, bool lining) {
        if (!layer.visible)
            return;
        const auto matrix = layer.transform.matrix();
        const QTransform transform = QTransform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
            matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]) * parent;
        if (layer.name == QStringLiteral("Lining")) {
            lining = true;
            found = true;
        }
        if (layer.kind() == fls::scene::LayerKind::Group) {
            for (const auto &child : static_cast<const fls::scene::Group &>(layer).children)
                collect(*child, transform, lining);
        } else if (lining && layer.kind() == fls::scene::LayerKind::Shape) {
            const auto &shape = static_cast<const fls::scene::Shape &>(layer);
            const auto key = QString::number(shape.shapeId);
            shapeCounts.insert(key, shapeCounts.value(key).toInt() + 1);
            require(!shape.mask && !shape.isRaster() && shape.color[3] == 255 && layer.opacity == 1,
                QStringLiteral("Lining reference requires opaque vectors"));
            const auto *sourceGeometry = geometry.shape(shape.shapeId);
            require(sourceGeometry != nullptr, QStringLiteral("Lining reference shape geometry is missing"));
            gui::PenPrimitive primitive;
            primitive.shapeId = shape.shapeId;
            for (const auto &triangle : sourceGeometry->triangles) {
                QPolygonF polygon({triangle.p0, triangle.p1, triangle.p2});
                if (gui::catalog::signedArea(polygon) < 0.0)
                    std::reverse(polygon.begin(), polygon.end());
                primitive.contours.push_back(polygon);
            }
            primitive.contours = gui::catalog::unite(primitive.contours);
            const auto polygons = gui::catalog::mapped(primitive, transform);
            if (shape.color[0] == 255 && shape.color[1] == 255 && shape.color[2] == 255) {
                visible = gui::catalog::subtract(visible, polygons);
                if (exceptions)
                    *exceptions += polygons;
                if (exceptionPlacements)
                    exceptionPlacements->push_back({shape.shapeId, transform});
            } else {
                require(shape.color[0] == 0 && shape.color[1] == 0 && shape.color[2] == 0,
                    QStringLiteral("Lining reference contains an unsupported color"));
                visible = gui::catalog::unite(visible + polygons);
            }
        }
    };
    collect(*project.root, {}, false);
    require(found && !visible.isEmpty(), QStringLiteral("Lining reference group is unavailable"));
    QTextStream(stdout) << "Reference shapes " << QJsonDocument(shapeCounts).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
    gui::PenFillRequest request;
    for (const auto &polygon : visible) {
        gui::PenLoop loop;
        loop.kind = gui::catalog::signedArea(polygon) > 0 ? gui::PenLoopKind::Outer : gui::PenLoopKind::Cutout;
        for (const auto &point : polygon)
            loop.points.push_back({point, gui::PenPointKind::Hard});
        request.loops.push_back(loop);
    }
    return request;
}

void thinFitTests(const QVector<gui::catalog::Primitive> &catalog, bool qualityPreset = false) {
    auto options = qualityPreset ? gui::thin::qualityOptions() : gui::thin::FillOptions{};
    options.useGpu = false;
    const auto check = [&](const gui::catalog::FillResult &result, const QString &name) {
        QTextStream(stdout) << name << ": " << result.fill.placements.size() << " shapes "
            << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
        require(result.fill.error.isEmpty() && !result.fill.cancelled && !result.fill.placements.isEmpty(),
            name + QStringLiteral(": ") + result.fill.error);
        require(result.diagnostics.value("coverageRatio").toDouble(-1) >= options.minimumCoverage
            && result.diagnostics.value("outsideEnvelope").toDouble(-1) == 0
            && result.diagnostics.value("newInteriorHoleArea").toDouble(-1) == 0,
            name + QStringLiteral(" lost coverage or exceeded the envelope"));
        const auto quality = result.diagnostics.value("boundaryQuality").toObject();
        const auto target = result.diagnostics.value("targetBoundary").toObject();
        require(quality.value("holes") == target.value("holes")
            && quality.value("components") == target.value("components"), name + QStringLiteral(" changed topology"));

    };
    const QVector<gui::PenPoint> straight{{{0, 0}, gui::PenPointKind::Hard},
        {{70, 0}, gui::PenPointKind::Hard}};
    const auto straightResult = gui::thin::fillLiningPath(straight, 0.4, catalog, options);
    check(straightResult, QStringLiteral("Straight span"));
    require(straightResult.fill.placements.size() == 1, QStringLiteral("Straight span was split"));
    const double straightThickness = outputPath(straightResult.fill, catalog).boundingRect().height();
    require(straightThickness >= 0.4 * options.preferredThicknessRatio * 0.995
        && straightThickness <= 0.4 * options.maximumThicknessRatio + 0.015,
        QStringLiteral("Straight lining lost its protective overlap or exceeded its width limit"));
    const QVector<gui::PenPoint> quadratic{{{0, 0}, gui::PenPointKind::Hard, {}, {70.0 / 3, 20}, true},
        {{70, 0}, gui::PenPointKind::Hard, {-70.0 / 3, 20}, {}, true}};
    for (double width : {0.4, 2.0, 8.0}) {
        const auto result = gui::thin::fillLiningPath(quadratic, width, catalog, options);
        check(result, QStringLiteral("Single bend %1").arg(width));
        require(result.fill.placements.size() <= 20, QStringLiteral("Single bend exceeded its span budget"));
    }
    for (double width : {0.4, 2.0, 8.0}) {
        QVector<gui::PenPoint> points{{{0, 0}, gui::PenPointKind::Hard},
            {{30, 15}, gui::PenPointKind::Soft}, {{70, 0}, gui::PenPointKind::Hard}};
        const auto result = gui::thin::fillLiningPath(points, width, catalog, options);
        check(result, QStringLiteral("Curved lining %1").arg(width));
        QPainterPathStroker stroker;
        stroker.setWidth(width);
        stroker.setCurveThreshold(0.0001);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        const auto target = stroker.createStroke(gui::buildLiningPath(points).centerline);
        const auto generated = outputPath(result.fill, catalog);
        stroker.setWidth(width * options.maximumThicknessRatio + 0.015);
        const auto maximumWidth = stroker.createStroke(gui::buildLiningPath(points).centerline);
        gui::catalog::Polygons maximumPolygons;
        gui::catalog::Polygons generatedPolygons;
        const auto inverseStrokeScale = QTransform::fromScale(1.0 / 1024, 1.0 / 1024);
        for (const auto &polygon : maximumWidth.toSubpathPolygons(QTransform::fromScale(1024, 1024)))
            maximumPolygons.push_back(inverseStrokeScale.map(polygon));
        for (const auto &polygon : generated.toSubpathPolygons())
            generatedPolygons.push_back(polygon);
        require(gui::catalog::subtract(gui::catalog::unite(generatedPolygons),
            gui::catalog::unite(maximumPolygons)).isEmpty(),
            QStringLiteral("Lining exceeded its independently measured stroke thickness"));
        int selectedPixels = 0;
        int coveredPixels = 0;
        for (int row = 0; row < 100; ++row)
            for (int column = 0; column < 400; ++column) {
                const auto bounds = target.boundingRect();
                const QPointF point(bounds.left() + bounds.width() * (column + 0.5) / 400.0,
                    bounds.top() + bounds.height() * (row + 0.5) / 100.0);
                if (target.contains(point)) {
                    ++selectedPixels;
                    coveredPixels += generated.contains(point);
                }
            }
        require(coveredPixels >= selectedPixels * options.minimumCoverage,
            QStringLiteral("Lining missed too many independent stroke pixels"));
    }
    for (int shapeId : gui::catalog::shapeIdsForTask(catalog, gui::catalog::ShapeTask::WholeRegion)) {
        const auto primitive = std::find_if(catalog.cbegin(), catalog.cend(), [shapeId](const auto &entry) {
            return entry.shape.shapeId == shapeId;
        });
        require(primitive != catalog.cend(), QStringLiteral("Missing native curved span"));
        const auto transform = QTransform::fromScale(0.5, 0.1);
        const auto result = gui::thin::fillPolygons(gui::catalog::mapped(primitive->shape, transform), catalog, options);
        check(result, QStringLiteral("Native curved region %1").arg(shapeId));
        require(result.fill.placements.size() == 1, QStringLiteral("Native curved region was split"));
        if (shapeId == 103 || shapeId == 104)
            require(result.fill.placements.front().shapeId == 103 || result.fill.placements.front().shapeId == 104,
                QStringLiteral("Native triangle was replaced with an unsuitable shape"));
        QTransform skewed;
        skewed.rotate(37.0);
        skewed.shear(2.5, 0.0);
        skewed.scale(0.5, 0.06);
        const auto affine = gui::thin::fillPolygons(gui::catalog::mapped(primitive->shape, skewed), catalog, options);
        check(affine, QStringLiteral("Skewed native region %1").arg(shapeId));
        require(affine.fill.placements.size() == 1, QStringLiteral("Skewed native region was split"));
    }
    gui::catalog::Polygons nativeComponents;
    for (int shapeId : {103, 136}) {
        const auto primitive = std::find_if(catalog.cbegin(), catalog.cend(), [shapeId](const auto &entry) {
            return entry.shape.shapeId == shapeId;
        });
        QTransform transform;
        transform.translate(0, shapeId == 103 ? 0 : 100);
        transform.rotate(19);
        transform.shear(1.5, 0);
        transform.scale(0.5, 0.06);
        nativeComponents += gui::catalog::mapped(primitive->shape, transform);
    }
    const auto componentFit = gui::thin::fillPolygons(nativeComponents, catalog, options);
    check(componentFit, QStringLiteral("Separate native spans"));
    require(componentFit.fill.placements.size() == 2,
        QStringLiteral("Separate native spans were split into patches"));
    if (qualityPreset) {
        const auto curve = std::find_if(catalog.cbegin(), catalog.cend(), [](const auto &entry) {
            return entry.shape.shapeId == 136;
        });
        QTransform transform;
        transform.rotate(19);
        transform.shear(1.5, 0);
        transform.scale(0.06, 0.5);
        auto connected = gui::catalog::mapped(curve->shape, transform);
        const auto contact = connected.front()[connected.front().size() / 4];
        connected.push_back(QPolygonF(QRectF(contact - QPointF(8, 0.5), QSizeF(16, 1))));
        const auto junction = gui::thin::fillPolygons(connected, catalog, options);
        check(junction, QStringLiteral("Connected native strokes"));
        require(junction.fill.placements.size() <= 3,
            QStringLiteral("Connected native strokes were fragmented"));
        require(junction.diagnostics.value(QStringLiteral("profileSeed")).toObject()
            .value(QStringLiteral("nativeArcCandidates")).toInt() > 0,
            QStringLiteral("An intact native curve was not recovered at a junction"));
    }
    QVector<gui::PenPoint> corner{{{0, 0}, gui::PenPointKind::Hard},
        {{40, 0}, gui::PenPointKind::Hard}, {{40, 40}, gui::PenPointKind::Hard}};
    check(gui::thin::fillLiningPath(corner, 2.0, catalog, options), QStringLiteral("Hard join"));
    gui::PenFillRequest taper;
    taper.points = {{{0, 0}, gui::PenPointKind::Hard}, {{70, 0}, gui::PenPointKind::Hard},
        {{55, 2}, gui::PenPointKind::Hard}, {{0, 8}, gui::PenPointKind::Hard}};
    const auto tapered = gui::thin::fillRegion(taper, catalog, options);
    check(tapered, QStringLiteral("Tapered region"));
    require(tapered.fill.placements.size() == 1, QStringLiteral("A native taper was split into patches"));
    QPainterPath ring;
    ring.setFillRule(Qt::WindingFill);
    ring.addEllipse(QRectF(0, 0, 50, 30));
    auto hole = QPainterPath();
    hole.addEllipse(QRectF(1, 1, 48, 28));
    ring.addPath(hole.toReversed());
    gui::PenFillRequest closed;
    closed.loops = gui::cubicPathLoops(ring);
    check(gui::thin::fillRegion(closed, catalog, options), QStringLiteral("Closed lining cutout"));
    const gui::catalog::Polygons disconnected{
        QPolygonF({{0, 0}, {40, 0}, {40, 2}, {0, 2}}),
        QPolygonF({{0, 10}, {40, 10}, {40, 12}, {0, 12}})};
    const auto separated = gui::thin::fillPolygons(disconnected, catalog, options);
    check(separated, QStringLiteral("Disconnected regions"));
    require(separated.diagnostics.value("boundaryQuality").toObject().value("components").toInt() == 2,
        QStringLiteral("Disconnected input merged components"));
    gui::catalog::Polygons reconstructed;
    for (const auto &placement : separated.fill.placements) {
        const auto primitive = std::find_if(catalog.cbegin(), catalog.cend(), [&](const auto &p) {
            return p.shape.shapeId == placement.shapeId;
        });
        require(primitive != catalog.cend(), QStringLiteral("Thin output escaped its catalog"));
        reconstructed += gui::catalog::mapped(primitive->shape, gui::catalog::emittedTransform(placement.transform));
    }
    reconstructed = gui::catalog::unite(reconstructed);
    require(gui::catalog::area(gui::catalog::subtract(disconnected, reconstructed))
            <= gui::catalog::area(disconnected) * (1.0 - options.minimumCoverage)
        && gui::catalog::subtract(reconstructed, gui::catalog::expanded(disconnected, options.boundaryAllowance)).isEmpty(),
        QStringLiteral("Serialized thin placements failed independent coverage or spill checks"));
    gui::catalog::Polygons mixedWidths;
    QPainterPath mixedEnvelope;
    for (int index = 0; index < 3; ++index) {
        const double width = index + 1.0;
        const double top = index * 10.0;
        mixedWidths.push_back(QPolygonF(QRectF(0, top, 60, width)));
        const double allowance = width * (options.maximumThicknessRatio - 1.0) * 0.5 + 0.001;
        mixedEnvelope.addRect(QRectF(0, top, 60, width).adjusted(-allowance, -allowance, allowance, allowance));
    }
    const auto mixed = gui::thin::fillPolygons(mixedWidths, catalog, options);
    check(mixed, QStringLiteral("Local widths 1-3"));
    require(mixed.fill.placements.size() == 3
        && outputPath(mixed.fill, catalog).subtracted(mixedEnvelope).isEmpty(),
        QStringLiteral("Mixed-width lining exceeded local thickness or split straight spans"));
    QPainterPath exception;
    exception.addEllipse(QRectF(27, -3, 6, 6));
    const auto inverseException = QTransform::fromScale(1.0 / 1024, 1.0 / 1024);
    auto occluded = options;
    for (const auto &polygon : exception.toSubpathPolygons(QTransform::fromScale(1024, 1024)))
        occluded.leeway.push_back(inverseException.map(polygon));
    const gui::catalog::Polygons ribbon{QPolygonF(QRectF(0, -1, 60, 2))};
    auto referenced = options;
    referenced.thicknessReference = ribbon;
    referenced.preferredThicknessRatio = referenced.maximumThicknessRatio;
    const auto paddedRibbon = gui::thin::fillPolygons({QPolygonF(QRectF(0, -1.2, 60, 2.4))}, catalog, referenced);
    check(paddedRibbon, QStringLiteral("Original thickness reference"));
    require(outputPath(paddedRibbon.fill, catalog).boundingRect().height() <= 2.0 * referenced.maximumThicknessRatio + 0.001,
        QStringLiteral("Padded target increased the original stroke thickness limit"));
    require(!gui::thin::fillPolygons({QPolygonF(QRectF(0, -2.5, 60, 5))}, catalog, referenced).fill.error.isEmpty(),
        QStringLiteral("Fitting accepted a target outside its original stroke thickness limit"));
    const auto hiddenSpan = gui::thin::fillPolygons(ribbon, catalog, occluded);
    check(hiddenSpan, QStringLiteral("Lining beneath exception layer"));
    require(hiddenSpan.fill.placements.size() == 1
        && hiddenSpan.diagnostics.value("boundaryQuality").toObject().value("components").toInt() == 2,
        QStringLiteral("Lining traced a hidden edge or covered an exception layer"));
    const auto hiddenOutput = outputPath(hiddenSpan.fill, catalog);
    require(hiddenOutput.contains(QPointF(30, 0))
        && !hiddenOutput.subtracted(exception).contains(QPointF(30, 0)),
        QStringLiteral("Exception masking did not preserve an underlying span"));
    auto protectedOptions = options;
    protectedOptions.preferredThicknessRatio = protectedOptions.maximumThicknessRatio;
    protectedOptions.protectedEmpty = {QPolygonF(QRectF(-52, -2, 104, 1.75))};
    const auto protectedStroke = gui::thin::fillLiningPath(straight, 0.4, catalog, protectedOptions);
    check(protectedStroke, QStringLiteral("Protected empty pixels"));
    gui::catalog::Polygons protectedCoverage;
    for (const auto &polygon : outputPath(protectedStroke.fill, catalog).toSubpathPolygons())
        protectedCoverage.push_back(polygon);
    require(gui::catalog::area(gui::catalog::intersect(protectedCoverage, protectedOptions.protectedEmpty)) == 0.0,
        QStringLiteral("Lining padding covered protected empty pixels"));
    auto excessiveThickness = options;
    excessiveThickness.maximumThicknessRatio = 2.01;
    require(!gui::thin::fillPolygons(mixedWidths, catalog, excessiveThickness).fill.error.isEmpty(),
        QStringLiteral("Thin fit accepted a thickness ratio above two"));
    bool stopAfterSeed = false;
    auto interrupt = options;
    interrupt.workProgress = [&](int, int, int) { stopAfterSeed = true; };
    const auto interrupted = gui::thin::fillRegion(taper, catalog, interrupt, [&] { return stopAfterSeed; });
    require(stopAfterSeed && interrupted.fill.cancelled && interrupted.fill.placements.isEmpty(),
        QStringLiteral("In-flight cancellation leaked shapes"));
    auto limited = options;
    limited.shapeBudget = 1;
    const auto overBudget = gui::thin::fillPolygons(disconnected, catalog, limited);
    require(!overBudget.fill.error.isEmpty(), QStringLiteral("Thin completion ignored its shape budget"));
    const auto cancelled = gui::thin::fillRegion(taper, catalog, options, [] { return true; });
    require(cancelled.fill.cancelled && cancelled.fill.placements.isEmpty(), QStringLiteral("Cancelled thin fill leaked shapes"));
    QTextStream(stdout) << "Thin fitting coverage, topology, taper, joins and cancellation passed\n";
}

struct PaintedPlacement {
    gui::PenPlacement placement;
    std::array<quint8, 4> color;
    double opacity = 1.0;
};

struct ColoredLiningRegion {
    gui::catalog::Polygons polygons;
    gui::catalog::Polygons leeway;
    QVector<PaintedPlacement> translucent;
    std::array<quint8, 4> color;
    int referenceCount = 0;
};

std::vector<ColoredLiningRegion> coloredLiningReference(const QString &source,
                                                       const gui::ShapeGeometryStore &geometry);

fls::scene::Transform2D sceneTransform(const QTransform &transform) {
    fls::Matrix3 matrix;
    matrix.m[0][0] = transform.m11();
    matrix.m[1][0] = transform.m12();
    matrix.m[0][1] = transform.m21();
    matrix.m[1][1] = transform.m22();
    matrix.m[0][2] = transform.dx();
    matrix.m[1][2] = transform.dy();

    return fls::decomposeTransform2D(matrix);
}

QTransform estimateSourceAlignment(const gui::LiningExtractionResult &detection,
                                    const std::vector<ColoredLiningRegion> &reference,
                                    const QTransform &initial, QJsonObject *diagnostics) {
    const int width = detection.pixels.width();
    const int height = detection.pixels.height();
    const int infinity = (width + height) * 3;
    std::vector<int> distance(width * height, infinity);
    QRect sourceBounds;
    QRectF humanBounds;
    QPolygonF points;
    for (const auto &region : detection.regions.regions)
        sourceBounds = sourceBounds.united(region.bounds);
    for (const auto &region : reference) {
        humanBounds = humanBounds.united(gui::catalog::painterPath(region.polygons).boundingRect());
        for (const auto &polygon : region.polygons)
            points += polygon;
    }
    if (sourceBounds.isEmpty() || humanBounds.isEmpty() || points.isEmpty())
        return initial;
    QPolygonF sampled;
    const int stride = std::max(1, int(points.size() / 2048));
    for (int index = 0; index < points.size(); index += stride)
        sampled.push_back(points[index]);
    for (int index = 0; index < width * height; ++index)
        if (detection.regions.raster->lineart[index])
            distance[index] = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto &value = distance[y * width + x];
            if (x > 0) value = std::min(value, distance[y * width + x - 1] + 3);
            if (y > 0) {
                value = std::min(value, distance[(y - 1) * width + x] + 3);
                if (x > 0) value = std::min(value, distance[(y - 1) * width + x - 1] + 4);
                if (x + 1 < width) value = std::min(value, distance[(y - 1) * width + x + 1] + 4);
            }
        }
    for (int y = height - 1; y >= 0; --y)
        for (int x = width - 1; x >= 0; --x) {
            auto &value = distance[y * width + x];
            if (x + 1 < width) value = std::min(value, distance[y * width + x + 1] + 3);
            if (y + 1 < height) {
                value = std::min(value, distance[(y + 1) * width + x] + 3);
                if (x > 0) value = std::min(value, distance[(y + 1) * width + x - 1] + 4);
                if (x + 1 < width) value = std::min(value, distance[(y + 1) * width + x + 1] + 4);
            }
        }
    const auto cost = [&](const QTransform &humanToPixel) {
        double sum = 0.0;
        for (const auto &point : sampled) {
            const QPointF mapped = humanToPixel.map(point);
            const int x = qRound(mapped.x());
            const int y = qRound(mapped.y());
            sum += x < 0 || y < 0 || x >= width || y >= height
                ? 12.0 : std::min(12.0, distance[y * width + x] / 3.0);
        }
        return sum / sampled.size();
    };
    const auto initialInverse = initial.inverted();
    QTransform best = initialInverse;
    double bestCost = cost(best);
    const double initialCost = bestCost;
    for (int rotation : {0, 90, 180, 270}) {
        const bool swapped = rotation == 90 || rotation == 270;
        for (int mirror : {-1, 1}) {
            std::array<double, 4> parameters = {double(sourceBounds.center().x()), double(sourceBounds.center().y()),
                sourceBounds.width() / (swapped ? humanBounds.height() : humanBounds.width()),
                sourceBounds.height() / (swapped ? humanBounds.width() : humanBounds.height())};
            const auto anchor = parameters;
            const auto transform = [&](const std::array<double, 4> &values) {
                QTransform trial;
                trial.translate(values[0], values[1]);
                trial.scale(values[2] * mirror, values[3]);
                trial.rotate(rotation);
                trial.translate(-humanBounds.center().x(), -humanBounds.center().y());
                return trial;
            };
            const auto score = [&](const std::array<double, 4> &values) {
                return cost(transform(values)) + 0.75 * (std::abs(std::log(values[2] / anchor[2]))
                    + std::abs(std::log(values[3] / anchor[3])));
            };
            double currentCost = score(parameters);
            for (double step : {64.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5}) {
                for (int round = 0; round < 16; ++round) {
                    bool improved = false;
                    for (int parameter = 0; parameter < 4; ++parameter)
                        for (int direction : {-1, 1}) {
                            auto trial = parameters;
                            if (parameter < 2)
                                trial[parameter] += direction * step;
                            else {
                                trial[parameter] *= std::exp(direction * step / 512.0);
                                if (trial[parameter] < anchor[parameter] * 0.65 || trial[parameter] > anchor[parameter] * 1.5)
                                    continue;
                            }
                            const double trialCost = score(trial);
                            if (trialCost + 1e-6 < currentCost) {
                                parameters = trial;
                                currentCost = trialCost;
                                improved = true;
                            }
                        }
                    if (!improved)
                        break;
                }
            }
            if (cost(transform(parameters)) < bestCost) {
                best = transform(parameters);
                bestCost = cost(best);
            }
        }
    }
    diagnostics->insert("initialAlignmentMeanDistancePixels", initialCost);
    diagnostics->insert("estimatedAlignmentMeanDistancePixels", bestCost);
    diagnostics->insert("alignmentDiagnosticOnly", true);

    return best.inverted();
}

QTransform layerTransform(const fls::scene::Layer &layer) {
    const auto matrix = layer.worldMatrix();
    return QTransform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
        matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]);
}

void liningDetectionTests() {
    QImage image(192, 128, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.fillRect(QRect(12, 20, 140, 2), Qt::black);
    painter.fillRect(QRect(12, 45, 140, 2), QColor(155, 155, 155));
    painter.fillRect(QRect(40, 70, 60, 40), Qt::black);
    painter.end();
    gui::LiningExtractionOptions options;
    options.maximumWidth = 3;
    auto result = gui::extractLining(image, options);
    require(result.regions.error.isEmpty() && !result.cancelled, result.regions.error);
    const auto selected = [&](int x, int y) { return result.regions.raster->lineart[y * image.width() + x] != 0; };
    for (int x = 14; x < 150; ++x) {
        require(selected(x, 20) && selected(x, 45), QStringLiteral("Detection lost a black line or an interior grey stripe"));
        require(qRed(result.pixels.pixel(x, 45)) > 120, QStringLiteral("Detection recolored a grey stripe as black"));
    }
    for (int y = 74; y < 106; ++y)
        for (int x = 44; x < 96; ++x)
            require(!selected(x, y), QStringLiteral("Detection classified a broad fill interior as lining"));
    QImage transparent(80, 48, QImage::Format_ARGB32);
    transparent.fill(Qt::transparent);
    QPainter lines(&transparent);
    lines.fillRect(QRect(10, 10, 60, 2), Qt::black);
    lines.fillRect(QRect(10, 30, 60, 2), Qt::white);
    lines.end();
    auto alpha = gui::extractLining(transparent, options);
    require(alpha.regions.error.isEmpty() && alpha.regions.lineartRegionCount == 2,
        QStringLiteral("Detection lost dark or white strokes on a transparent source"));
    for (int y = 0; y < transparent.height(); ++y)
        for (int x = 0; x < transparent.width(); ++x)
            require(qAlpha(transparent.pixel(x, y)) != 0 || qAlpha(alpha.pixels.pixel(x, y)) == 0,
                QStringLiteral("Detection painted an intentional transparent pixel"));
    require(gui::extractLining(image, options, [] { return true; }).cancelled,
        QStringLiteral("Lining detection ignored cancellation"));
    options.maximumWidth = -1;
    require(!gui::extractLining(image, options).regions.error.isEmpty(), QStringLiteral("Detection accepted an invalid width"));
    QTextStream(stdout) << "Lining detection covers black and grey stripes, preserves transparency and rejects broad interiors\n";
}

std::unique_ptr<fls::scene::GuideLayer> detectionGuide(const QImage &image, const QString &name,
                                                     const fls::scene::Transform2D &transform) {
    auto guide = std::make_unique<fls::scene::GuideLayer>();
    guide->id = QStringLiteral("guide_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    guide->name = name;
    guide->transform = transform;
    guide->opacity = 1.0;
    guide->image = std::make_unique<fls::scene::RasterContainer>();
    guide->image->width = image.width();
    guide->image->height = image.height();
    guide->image->format = QStringLiteral("webp");
    QBuffer buffer(&guide->image->encoded);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "WEBP", 100), QStringLiteral("Cannot encode detected lining"));

    return guide;
}

void sourceLiningTrial(const QString &source, const QString &destination,
                       const gui::ShapeGeometryStore &geometry,
                       const QVector<gui::catalog::Primitive> &catalog, bool fit) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    auto project = fls::decodeProjectDocument(file.readAll());
    const fls::scene::GuideLayer *guide = nullptr;
    std::vector<const fls::scene::Layer *> humanGroups;
    bool referenceAvailable = false;
    std::function<void(const fls::scene::Layer &)> collect;
    collect = [&](const auto &layer) {
        referenceAvailable = referenceAvailable || layer.name == QStringLiteral("Lining");
        if (layer.name == QStringLiteral("Lining"))
            humanGroups.push_back(&layer);
        if (layer.kind() == fls::scene::LayerKind::Guide && !guide)
            guide = &static_cast<const fls::scene::GuideLayer &>(layer);
        if (layer.kind() == fls::scene::LayerKind::Group)
            for (const auto &child : static_cast<const fls::scene::Group &>(layer).children)
                collect(*child);
    };
    collect(*project.root);
    require(guide && guide->image, QStringLiteral("Source project has no embedded guide image"));
    QImage image = QImage::fromData(guide->image->encoded).convertToFormat(QImage::Format_ARGB32);
    if (!guide->imageTopDown)
        image = image.mirrored(false, true);
    require(!image.isNull(), QStringLiteral("Cannot decode embedded guide image"));
    QTextStream output(stdout);
    const auto detection = gui::extractLining(image, {}, {}, [&](const QString &phase, int done, int total) {
        if (done == 0 || done + 1 == total)
            output << phase << ' ' << done << '/' << total << '\n' << Qt::flush;
    });
    require(detection.regions.error.isEmpty() && !detection.cancelled, detection.regions.error);
    auto diagnostics = detection.diagnostics;
    const auto world = layerTransform(*guide);
    QTransform pixelToLocal;
    pixelToLocal.translate(-image.width() * 0.5, -image.height() * 0.5);
    QTransform pixelToWorld = pixelToLocal * world;
    const auto boundsJson = [](const QRectF &bounds) {
        return QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height()};
    };
    diagnostics.insert("sourceWorldBounds", boundsJson(pixelToWorld.mapRect(QRectF(QPointF(), image.size()))));
    if (referenceAvailable) {
        const auto reference = coloredLiningReference(source, geometry);
        pixelToWorld = estimateSourceAlignment(detection, reference, pixelToWorld, &diagnostics);
        QRectF referenceBounds;
        for (const auto &region : reference)
            referenceBounds = referenceBounds.united(gui::catalog::painterPath(region.polygons).boundingRect());
        diagnostics.insert("humanWorldBounds", boundsJson(referenceBounds));
        QImage target(image.size(), QImage::Format_ARGB32);
        target.fill(Qt::black);
        QPainter painter(&target);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.setTransform(pixelToWorld.inverted());
        for (const auto &region : reference)
            painter.drawPath(gui::catalog::painterPath(region.polygons));
        painter.end();
        int humanPixels = 0;
        int overlapping = 0;
        int nearHuman = 0;
        int nearDetected = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const int index = y * image.width() + x;
                const bool human = qRed(target.pixel(x, y)) != 0;
                const bool detected = detection.regions.raster->lineart[index];
                humanPixels += human;
                overlapping += human && detected;
                if (!human && !detected)
                    continue;
                bool matchedHuman = false;
                bool matchedDetected = false;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= image.width() || ny >= image.height())
                            continue;
                        matchedHuman = matchedHuman || qRed(target.pixel(nx, ny)) != 0;
                        matchedDetected = matchedDetected || detection.regions.raster->lineart[ny * image.width() + nx];
                    }
                nearHuman += detected && matchedHuman;
                nearDetected += human && matchedDetected;
            }
        diagnostics.insert("humanPixels", humanPixels);
        diagnostics.insert("overlappingHumanPixels", overlapping);
        diagnostics.insert("detectedWithinTwoPixelsOfHuman", nearHuman);
        diagnostics.insert("humanWithinTwoPixelsOfDetected", nearDetected);
    }
    output << QJsonDocument(diagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
    auto comparison = std::make_unique<fls::scene::Group>();
    comparison->id = QStringLiteral("group_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    comparison->name = QStringLiteral("Source image lining trial");
    comparison->transform.x = pixelToWorld.mapRect(QRectF(QPointF(), image.size())).width() * 1.2;
    auto imageCopy = guide->clone();
    imageCopy->id = QStringLiteral("guide_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QTransform localToPixel;
    localToPixel.translate(image.width() * 0.5, image.height() * 0.5);
    const auto alignedGuideTransform = sceneTransform(localToPixel * pixelToWorld);
    imageCopy->transform = alignedGuideTransform;
    imageCopy->name = referenceAvailable
        ? QStringLiteral("Source image — estimated alignment (toggle)")
        : QStringLiteral("Source image (toggle for comparison)");
    imageCopy->visible = false;
    imageCopy->opacity = 1.0;
    comparison->append(std::move(imageCopy));
    for (const auto *human : humanGroups) {
        auto copy = human->clone();
        std::function<void(fls::scene::Layer &)> assignIds;
        assignIds = [&](auto &layer) {
            layer.id = QStringLiteral("node_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            if (layer.kind() == fls::scene::LayerKind::Group)
                for (auto &child : static_cast<fls::scene::Group &>(layer).children)
                    assignIds(*child);
        };
        assignIds(*copy);
        copy->transform = fls::decomposeTransform2D(human->worldMatrix());
        copy->name = QStringLiteral("Human lining reference (toggle)");
        copy->visible = false;
        comparison->append(std::move(copy));
    }
    auto mask = detectionGuide(detection.pixels, QStringLiteral("Detected lining pixels"),
        alignedGuideTransform);
    mask->visible = !fit;
    comparison->append(std::move(mask));
    QElapsedTimer timer;
    timer.start();
    int totalShapes = 0;
    std::vector<std::uint8_t> transparentPixels(image.width() * image.height(), 0);
    bool hasTransparentPixels = false;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) == 0) {
                transparentPixels[y * image.width() + x] = 1;
                hasTransparentPixels = true;
            }
    gui::catalog::Polygons transparentGeometry;
    if (hasTransparentPixels)
        for (const auto &polygon : gui::pixelBoundaryLoops(transparentPixels, image.size(), image.rect()))
            transparentGeometry.push_back(pixelToWorld.map(polygon));
    transparentGeometry = gui::catalog::unite(transparentGeometry);
    for (const auto &sourceRegion : detection.regions.regions) {
        const auto color = sourceRegion.color;
        const auto &path = sourceRegion.outline;
        if (!fit)
            continue;
        auto options = gui::thin::qualityOptions();
        options.shapeBudget = 100000;
        options.minimumCoverage = 0.98;
        const double worldPixelWidth = std::max(
            QLineF(pixelToWorld.map(QPointF()), pixelToWorld.map(QPointF(1.0, 0.0))).length(),
            QLineF(pixelToWorld.map(QPointF()), pixelToWorld.map(QPointF(0.0, 1.0))).length());
        options.boundaryAllowance = detection.diagnostics.value("maximumWidthPixels").toDouble() * worldPixelWidth * 0.5;
        options.qualityTimeBudgetMilliseconds = 30000;
        options.qualityEvaluationBudget = 30000;
        options.phaseProgress = [&](const QString &phase) {
            output << "Phase " << color.name() << '/' << phase << ' ' << timer.elapsed() << " ms\n" << Qt::flush;
        };
        options.workProgress = [&](int count, int done, int total) {
            if (count > 0 && done == 0 && total == 0)
                output << "Seed " << color.name() << ' ' << count << " shapes " << timer.elapsed() << " ms\n" << Qt::flush;
        };
        std::vector<std::uint8_t> requiredPixels(image.width() * image.height(), 0);
        QRect pixelBounds;
        int sourceCount = 0;
        int tracedCount = 0;
        for (int y = sourceRegion.bounds.top(); y <= sourceRegion.bounds.bottom(); ++y)
            for (int x = sourceRegion.bounds.left(); x <= sourceRegion.bounds.right(); ++x)
                if (detection.regions.raster->labels[y * image.width() + x] == sourceRegion.id) {
                    requiredPixels[y * image.width() + x] = 1;
                    ++sourceCount;
                    tracedCount += path.contains(QPointF(x + 0.5, y + 0.5));
                    pixelBounds = pixelBounds.united(QRect(x, y, 1, 1));
                }
        gui::catalog::Polygons pixelGeometry;
        for (const auto &polygon : gui::pixelBoundaryLoops(requiredPixels, image.size(), pixelBounds))
            pixelGeometry.push_back(pixelToWorld.map(polygon));
        pixelGeometry = gui::catalog::unite(pixelGeometry);
        const auto flatten = QTransform::fromScale(16.0, 16.0);
        const auto flattenedToWorld = flatten.inverted() * pixelToWorld;
        for (const auto &polygon : path.toSubpathPolygons(flatten))
            options.thicknessReference.push_back(flattenedToWorld.map(polygon));
        options.thicknessReference = gui::catalog::unite(options.thicknessReference);
        options.protectedEmpty = transparentGeometry;
        for (auto hole : pixelGeometry)
            if (gui::catalog::signedArea(hole) < 0.0) {
                std::reverse(hole.begin(), hole.end());
                options.protectedEmpty += gui::catalog::interiorSupport({hole},
                    std::sqrt(std::abs(pixelToWorld.determinant())) * 0.15);
            }
        QPainterPathStroker padding;
        padding.setWidth(0.6);
        padding.setJoinStyle(Qt::RoundJoin);
        const auto paddedPath = path.united(padding.createStroke(path));
        gui::catalog::Polygons paddedPolygons;
        for (const auto &polygon : paddedPath.toSubpathPolygons(flatten))
            paddedPolygons.push_back(flattenedToWorld.map(polygon));
        const auto polygons = gui::catalog::subtract(gui::catalog::unite(paddedPolygons), options.protectedEmpty);
        const auto result = gui::thin::fillPolygons(polygons, catalog, options);
        auto fitDiagnostics = result.diagnostics;
        fitDiagnostics.insert("color", color.name());
        fitDiagnostics.insert("sourceRegion", sourceRegion.id);
        fitDiagnostics.insert("count", result.fill.placements.size());
        fitDiagnostics.insert("tracePixelCenterCoverageRatio", sourceCount > 0 ? double(tracedCount) / sourceCount : 1.0);
        fitDiagnostics.insert("error", result.fill.error);
        output << QJsonDocument(fitDiagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
        auto group = comparisonGroup(result.fill, QStringLiteral("%1 — region %2").arg(color.name()).arg(sourceRegion.id + 1));
        group->transform.x = 0.0;
        if (!result.fill.error.isEmpty())
            group->name += QStringLiteral(" — incomplete");
        for (auto &child : group->children)
            static_cast<fls::scene::Shape &>(*child).color = {quint8(color.red()), quint8(color.green()), quint8(color.blue()), 255};
        totalShapes += result.fill.placements.size();
        comparison->append(std::move(group));
    }
    output << QJsonDocument(QJsonObject{{"generatedCount", totalShapes},
        {"fitMilliseconds", timer.elapsed()}}).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
    project.root->append(std::move(comparison));
    QFile outputFile(destination);
    require(outputFile.open(QIODevice::WriteOnly | QIODevice::NewOnly), outputFile.errorString());
    const auto encoded = fls::encodeProjectDocument(project);
    require(outputFile.write(encoded) == encoded.size(), outputFile.errorString());
}

void sourceLiningRasterCheck(const QString &source, const gui::ShapeGeometryStore &geometry) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    auto project = fls::decodeProjectDocument(file.readAll());
    require(!project.root->children.empty() && project.root->children.back()->kind() == fls::scene::LayerKind::Group,
        QStringLiteral("Source comparison group is missing"));
    const auto &trial = static_cast<const fls::scene::Group &>(*project.root->children.back());
    const fls::scene::GuideLayer *sourceGuide = nullptr;
    const fls::scene::GuideLayer *maskGuide = nullptr;
    for (const auto &node : trial.children)
        if (node->kind() == fls::scene::LayerKind::Guide) {
            const auto *guide = &static_cast<const fls::scene::GuideLayer &>(*node);
            if (guide->name.startsWith(QStringLiteral("Source image"))) sourceGuide = guide;
            if (guide->name == QStringLiteral("Detected lining pixels")) maskGuide = guide;
        }
    require(sourceGuide && sourceGuide->image && maskGuide && maskGuide->image,
        QStringLiteral("Source and detected pixel guides are missing"));
    QImage sourceImage = QImage::fromData(sourceGuide->image->encoded).convertToFormat(QImage::Format_ARGB32);
    const QImage mask = QImage::fromData(maskGuide->image->encoded).convertToFormat(QImage::Format_ARGB32);
    if (!sourceGuide->imageTopDown)
        sourceImage = sourceImage.mirrored(false, true);
    require(!mask.isNull() && sourceImage.size() == mask.size(), QStringLiteral("Source mask dimensions are invalid"));
    QImage painted(mask.size(), QImage::Format_ARGB32);
    painted.fill(Qt::transparent);
    QTransform localToPixel;
    localToPixel.translate(mask.width() * 0.5, mask.height() * 0.5);
    const auto worldToPixel = layerTransform(*maskGuide).inverted() * localToPixel;
    QPainter painter(&painted);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    int count = 0;
    int warnings = 0;
    gui::catalog::Polygons nativeCoverage;
    std::function<void(const fls::scene::Layer &)> render;
    render = [&](const auto &layer) {
        if (!layer.visible)
            return;
        if (layer.kind() == fls::scene::LayerKind::Group) {
            warnings += layer.name.contains(QStringLiteral("incomplete"));
            for (const auto &child : static_cast<const fls::scene::Group &>(layer).children)
                render(*child);
        } else if (layer.kind() == fls::scene::LayerKind::Shape) {
            const auto &shape = static_cast<const fls::scene::Shape &>(layer);
            const auto *native = geometry.shape(shape.shapeId);
            require(native && !shape.mask && !shape.isRaster(), QStringLiteral("Unexpected source fit geometry"));
            painter.setTransform(layerTransform(shape) * worldToPixel);
            gui::catalog::Polygons triangles;
            for (const auto &triangle : native->triangles) {
                QPolygonF polygon{triangle.p0, triangle.p1, triangle.p2};
                if (gui::catalog::signedArea(polygon) < 0.0)
                    std::reverse(polygon.begin(), polygon.end());
                triangles.push_back(std::move(polygon));
            }
            for (const auto &polygon : gui::catalog::unite(triangles)) {
                painter.drawPolygon(polygon);
                const auto transform = layerTransform(shape) * worldToPixel;
                auto mapped = transform.map(polygon);
                if (transform.determinant() < 0.0)
                    std::reverse(mapped.begin(), mapped.end());
                nativeCoverage.push_back(std::move(mapped));
            }
            ++count;
        }
    };
    render(trial);
    painter.end();
    int selected = 0;
    int covered = 0;
    int added = 0;
    int transparent = 0;
    int centerCovered = 0;
    const gui::catalog::PointContainment nativeContains(gui::catalog::unite(nativeCoverage));
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x) {
            const bool required = qAlpha(mask.pixel(x, y)) >= 128;
            const bool ink = qAlpha(painted.pixel(x, y)) >= 128;
            selected += required;
            covered += required && ink;
            centerCovered += required && nativeContains.contains(QPointF(x + 0.5, y + 0.5));
            added += !required && ink;
            transparent += ink && qAlpha(sourceImage.pixel(x, y)) == 0;
        }
    const double coverage = selected > 0 ? double(covered) / selected : 1.0;
    QTextStream(stdout) << QJsonDocument(QJsonObject{{"count", count}, {"selectedPixels", selected},
        {"coveredPixels", covered}, {"pixelCoverageRatio", coverage}, {"additionalPixels", added},
        {"pixelCenterCoverageRatio", selected > 0 ? double(centerCovered) / selected : 1.0},
        {"paintedAreaRatio", selected > 0 ? double(covered + added) / selected : 0.0},
        {"paintedTransparentSourcePixels", transparent}, {"fittingWarnings", warnings}})
        .toJson(QJsonDocument::Compact) << '\n';
    require(coverage >= 0.98 && warnings == 0 && transparent == 0,
        QStringLiteral("Saved native fit failed source pixel coverage, transparency or geometric checks"));
}

std::vector<ColoredLiningRegion> coloredLiningReference(const QString &source,
                                                       const gui::ShapeGeometryStore &geometry) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto project = fls::decodeProjectDocument(file.readAll());
    std::vector<ColoredLiningRegion> regions;
    std::function<void(const fls::scene::Layer &, const QTransform &, double, bool)> collect;
    collect = [&](const auto &layer, const QTransform &parent, double opacity, bool lining) {
        if (!layer.visible)
            return;
        const auto matrix = layer.transform.matrix();
        const QTransform transform = QTransform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
            matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]) * parent;
        opacity *= layer.opacity;
        lining = lining || layer.name == QStringLiteral("Lining");
        if (layer.kind() == fls::scene::LayerKind::Group) {
            for (const auto &child : static_cast<const fls::scene::Group &>(layer).children)
                collect(*child, transform, opacity, lining);
        } else if (lining && layer.kind() == fls::scene::LayerKind::Shape) {
            const auto &shape = static_cast<const fls::scene::Shape &>(layer);
            require(!shape.mask && !shape.isRaster(), QStringLiteral("Lining reference requires vectors"));
            auto color = shape.color;
            color[3] = 255;
            if (regions.empty() || regions.back().color != color) {
                ColoredLiningRegion region;
                region.color = color;
                regions.push_back(std::move(region));
            }
            auto &region = regions.back();
            ++region.referenceCount;
            if (shape.color[3] != 255 || opacity != 1.0) {
                region.translucent.push_back({{shape.shapeId, transform}, shape.color, opacity});
                return;
            }
            const auto *sourceGeometry = geometry.shape(shape.shapeId);
            require(sourceGeometry != nullptr, QStringLiteral("Lining reference geometry is missing"));
            gui::PenPrimitive primitive;
            primitive.shapeId = shape.shapeId;
            for (const auto &triangle : sourceGeometry->triangles) {
                QPolygonF polygon({triangle.p0, triangle.p1, triangle.p2});
                if (gui::catalog::signedArea(polygon) < 0)
                    std::reverse(polygon.begin(), polygon.end());
                primitive.contours.push_back(polygon);
            }
            primitive.contours = gui::catalog::unite(primitive.contours);
            region.polygons += gui::catalog::mapped(primitive, transform);
        }
    };
    collect(*project.root, {}, 1.0, false);
    require(!regions.empty(), QStringLiteral("Lining reference group is unavailable"));
    gui::catalog::Polygons above;
    for (auto it = regions.rbegin(); it != regions.rend(); ++it) {
        it->polygons = gui::catalog::unite(it->polygons);
        it->leeway = above;
        above = gui::catalog::unite(above + it->polygons);
    }

    return regions;
}

void fitColoredLiningReference(const QString &source, const std::optional<QString> &destination,
                               const gui::ShapeGeometryStore &geometry,
                               const QVector<gui::catalog::Primitive> &catalog) {
    QFile file(source);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    auto project = fls::decodeProjectDocument(file.readAll());
    const auto regions = coloredLiningReference(source, geometry);
    auto comparison = std::make_unique<fls::scene::Group>();
    comparison->id = QStringLiteral("group_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    comparison->name = QStringLiteral("Thin Region Fit — colors");
    QTextStream output(stdout);
    QElapsedTimer timer;
    QRectF bounds;
    QJsonArray measurements;
    int referenceCount = 0;
    int generatedCount = 0;
    int retainedCount = 0;
    timer.start();
    for (const auto &region : regions) {
        const auto name = QColor(region.color[0], region.color[1], region.color[2]).name();
        auto options = gui::thin::qualityOptions();
        options.leeway = region.leeway;
        options.phaseProgress = [&](const QString &phase) {
            output << "Phase " << name << '/' << phase << ' ' << timer.elapsed() << " ms\n" << Qt::flush;
        };
        gui::catalog::FillResult result;
        if (!region.polygons.isEmpty()) {
            result = gui::thin::fillPolygons(region.polygons, catalog, options);
            result.diagnostics.insert("color", name);
            result.diagnostics.insert("count", result.fill.placements.size());
            result.diagnostics.insert("error", result.fill.error);
            output << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
            require(result.fill.error.isEmpty(), name + QStringLiteral(": ") + result.fill.error);
            measurements.push_back(result.diagnostics);
            bounds = bounds.united(gui::catalog::painterPath(region.polygons).boundingRect());
        }
        auto group = comparisonGroup(result.fill, name);
        group->transform.x = 0.0;
        for (auto &child : group->children)
            static_cast<fls::scene::Shape &>(*child).color = region.color;
        gui::PenFillResult originalTranslucent;
        for (const auto &stroke : region.translucent)
            originalTranslucent.placements.push_back(stroke.placement);
        auto translucent = comparisonGroup(originalTranslucent, QStringLiteral("Original translucent strokes"));
        translucent->transform.x = 0.0;
        for (int index = 0; index < int(translucent->children.size()); ++index) {
            auto &shape = static_cast<fls::scene::Shape &>(*translucent->children[index]);
            shape.color = region.translucent[index].color;
            shape.opacity = region.translucent[index].opacity;
        }
        group->append(std::move(translucent));
        comparison->append(std::move(group));
        referenceCount += region.referenceCount;
        generatedCount += result.fill.placements.size();
        retainedCount += region.translucent.size();
    }
    comparison->transform.x = bounds.width() * 1.2;
    output << QJsonDocument(QJsonObject{{"referenceCount", referenceCount},
        {"generatedCount", generatedCount + retainedCount}, {"generatedOpaqueCount", generatedCount},
        {"retainedTranslucent", retainedCount}, {"elapsedMilliseconds", timer.elapsed()},
        {"regions", measurements}}).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
    if (destination) {
        project.root->append(std::move(comparison));
        QFile outputFile(*destination);
        require(outputFile.open(QIODevice::WriteOnly | QIODevice::NewOnly), outputFile.errorString());
        const auto encoded = fls::encodeProjectDocument(project);
        require(outputFile.write(encoded) == encoded.size(), outputFile.errorString());
    }
}

void convexUnionTests(const QVector<gui::catalog::Primitive> &catalog) {
    using gui::catalog::Polygons;
    const auto integerPaths = [](const Polygons &polygons) {
        Clipper2Lib::Paths64 paths;
        for (const auto &polygon : polygons) {
            Clipper2Lib::Path64 path;
            for (const auto &point : polygon)
                path.emplace_back(std::llround(point.x() * gui::catalog::kCoordinateScale),
                    std::llround(point.y() * gui::catalog::kCoordinateScale));
            paths.push_back(std::move(path));
        }
        return paths;
    };
    const auto canonical = [](const Clipper2Lib::Paths64 &paths) {
        std::vector<std::vector<std::pair<int64_t, int64_t>>> result;
        for (const auto &path : paths) {
            std::vector<std::pair<int64_t, int64_t>> points;
            for (const auto &point : path)
                points.push_back({point.x, point.y});
            if (!points.empty())
                std::rotate(points.begin(), std::min_element(points.begin(), points.end()), points.end());
            result.push_back(std::move(points));
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    int checks = 0;
    const auto check = [&](const Polygons &polygons) {
        Clipper2Lib::Clipper64 reference;
        Clipper2Lib::Paths64 output;
        reference.PreserveCollinear(false);
        reference.AddSubject(integerPaths(polygons));
        require(reference.Execute(Clipper2Lib::ClipType::Union,
            Clipper2Lib::FillRule::NonZero, output), QStringLiteral("Reference union failed"));
        require(canonical(integerPaths(gui::catalog::unite(polygons))) == canonical(output),
            QStringLiteral("Convex union changed the exact boundary at comparison %1").arg(checks));
        require(canonical(integerPaths(gui::catalog::subtract(polygons, {}))) == canonical(output),
            QStringLiteral("Convex empty-clip difference changed the exact boundary"));
        ++checks;
    };
    for (const auto &primitive : catalog) {
        for (int index = 0; index < 32; ++index) {
            QTransform transform;
            transform.translate(index % 8 == 0 ? 50000000.0 : index * 16.01 - 30, -index * 13.7);
            transform.rotateRadians(index * 0.17);
            transform.shear(index % 4 * 0.3, (index % 3 - 1) * 0.17);
            transform.scale(std::pow(10.0, index % 6 - 4) * (index % 2 ? -1 : 1),
                index % 8 == 0 ? 1e-8 : std::pow(10.0, index % 5 - 3));
            Polygons polygons;
            for (const auto &polygon : primitive.shape.contours)
                polygons.push_back(transform.map(polygon));
            check(polygons);
        }
    }
    for (int vertices : {3, 5, 7, 9, 16, 32, 65}) {
        for (int stride = 1; stride < vertices; ++stride) {
            QPolygonF polygon;
            for (int index = 0; index < vertices; ++index) {
                const double angle = index * stride * 2.0 * std::acos(-1.0) / vertices;
                polygon.push_back({std::cos(angle) * 19, std::sin(angle) * 13});
            }
            check({polygon});
            std::reverse(polygon.begin(), polygon.end());
            check({polygon});
            const auto repeated = polygon;
            polygon += repeated;
            check({polygon});
        }
    }
    check({QPolygonF{{0, 0}, {10, 0}, {20, 0}, {20, 20}, {0, 20}}});
    check({QPolygonF{{0, 0}, {10, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}}});
    QTextStream(stdout) << "Convex normalization: " << checks << " exact union comparisons passed\n";
}

void expansionGeometryTests(const QVector<gui::catalog::Primitive> &catalog) {
    using gui::catalog::Polygons;
    const auto integerPaths = [](const Polygons &polygons) {
        Clipper2Lib::Paths64 paths;
        for (const auto &polygon : polygons) {
            Clipper2Lib::Path64 path;
            for (const auto &point : polygon)
                path.emplace_back(std::llround(point.x() * gui::catalog::kCoordinateScale),
                    std::llround(point.y() * gui::catalog::kCoordinateScale));
            if (path.size() >= 3)
                paths.push_back(std::move(path));
        }
        return paths;
    };
    const auto canonical = [](const Clipper2Lib::Paths64 &paths) {
        std::vector<std::vector<std::pair<int64_t, int64_t>>> result;
        for (const auto &path : paths) {
            std::vector<std::pair<int64_t, int64_t>> points;
            for (const auto &point : path)
                points.push_back({point.x, point.y});
            if (!points.empty())
                std::rotate(points.begin(), std::min_element(points.begin(), points.end()), points.end());
            result.push_back(std::move(points));
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    int checks = 0;
    const auto check = [&](const Polygons &polygons, double radius) {
        Polygons parts = polygons, boundary;
        const std::array<QPointF, 4> offsets{{{-radius, -radius}, {radius, -radius},
            {radius, radius}, {-radius, radius}}};
        for (const auto &polygon : polygons)
            for (int index = 0; index < polygon.size(); ++index) {
                QPolygonF points;
                for (const auto &offset : offsets) {
                    points.push_back(polygon[index] + offset);
                    points.push_back(polygon[(index + 1) % polygon.size()] + offset);
                }
                boundary.push_back(gui::catalog::convexHull(points));
            }
        parts += boundary;
        Clipper2Lib::Clipper64 reference;
        Clipper2Lib::Paths64 output, interior;
        reference.PreserveCollinear(false);
        reference.AddSubject(integerPaths(parts));
        require(reference.Execute(Clipper2Lib::ClipType::Union,
            Clipper2Lib::FillRule::NonZero, output), QStringLiteral("Reference expansion failed"));
        require(canonical(integerPaths(gui::catalog::expanded(polygons, radius))) == canonical(output),
            QStringLiteral("Expansion changed the exact boundary at comparison %1, radius %2")
                .arg(checks).arg(radius, 0, 'g', 16));
        const auto boundaryUnion = gui::catalog::unite(boundary);
        reference.Clear();
        reference.AddSubject(integerPaths(polygons));
        reference.AddClip(integerPaths(boundaryUnion));
        require(reference.Execute(Clipper2Lib::ClipType::Difference,
            Clipper2Lib::FillRule::NonZero, interior), QStringLiteral("Reference erosion failed"));
        require(canonical(integerPaths(gui::catalog::interiorSupport(polygons, radius))) == canonical(interior),
            QStringLiteral("Interior support changed the exact boundary at comparison %1").arg(checks));
        ++checks;
    };
    for (const auto &primitive : catalog)
        for (int index = 0; index < 12; ++index) {
            QTransform transform;
            transform.translate(index % 6 == 0 ? 50000000.0 : index * 16.01 - 30, -index * 13.7);
            transform.rotateRadians(index * 0.17);
            transform.shear(index % 4 * 0.3, (index % 3 - 1) * 0.17);
            transform.scale(std::pow(10.0, index % 6 - 4) * (index % 2 ? -1 : 1),
                index % 6 == 0 ? 1e-8 : std::pow(10.0, index % 5 - 3));
            Polygons polygons;
            for (const auto &polygon : primitive.shape.contours)
                polygons.push_back(transform.map(polygon));
            for (double radius : {-2.0, 0.0, 1e-7, 1e-6, 0.15, 0.5, 2.0})
                check(polygons, radius);
        }
    for (const QString &fixture : {QStringLiteral("tools/fixtures/compact_fit_cornered_interior.json"),
             QStringLiteral("tools/fixtures/compact_fit_small_contour.json")}) {
        const auto region = gui::catalog::buildRegion(readRequest(fixture), {});
        for (double radius : {0.15, 0.5, 2.0})
            check(region.required, radius);
    }
    QTextStream(stdout) << "Expansion: " << checks << " exact expansion and erosion comparisons passed\n";
}

void polygonContainmentProofTests() {
    using gui::catalog::Polygons;
    const auto integerPaths = [](const Polygons &polygons) {
        Clipper2Lib::Paths64 paths;
        for (const auto &polygon : polygons) {
            Clipper2Lib::Path64 path;
            for (const auto &point : polygon)
                path.emplace_back(std::llround(point.x() * gui::catalog::kCoordinateScale),
                    std::llround(point.y() * gui::catalog::kCoordinateScale));
            paths.push_back(std::move(path));
        }
        return paths;
    };
    const auto canonical = [](const Clipper2Lib::Paths64 &paths) {
        std::vector<std::vector<std::pair<int64_t, int64_t>>> result;
        for (const auto &path : paths) {
            std::vector<std::pair<int64_t, int64_t>> points;
            for (const auto &point : path)
                points.push_back({point.x, point.y});
            if (!points.empty())
                std::rotate(points.begin(), std::min_element(points.begin(), points.end()), points.end());
            result.push_back(std::move(points));
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    const auto ellipse = [](QPointF center, double width, double height, int count) {
        QPolygonF polygon;
        for (int index = 0; index < count; ++index) {
            const double angle = index * 2.0 * std::acos(-1.0) / count;
            polygon.push_back(center + QPointF(std::cos(angle) * width, std::sin(angle) * height));
        }
        return polygon;
    };
    const QPolygonF outer = ellipse({}, 100, 75, 256);
    QPolygonF hole = ellipse({15, 0}, 12, 18, 64);
    std::reverse(hole.begin(), hole.end());
    QPolygonF star;
    for (int index = 0; index < 160; ++index) {
        const double angle = index * 2.0 * std::acos(-1.0) / 160;
        const double radius = index % 2 ? 80 : 100;
        star.push_back({std::cos(angle) * radius, std::sin(angle) * radius});
    }
    int checks = 0;
    for (const Polygons &raw : {Polygons{outer}, Polygons{outer, hole}, Polygons{star},
             Polygons{outer, ellipse({220, 0}, 40, 40, 128)}}) {
        const auto envelope = gui::catalog::unite(raw);
        const auto clip = integerPaths(envelope);
        for (int y = -10; y <= 10; ++y)
            for (int x = -12; x <= 12; ++x)
                for (int kind = 0; kind < 4; ++kind) {
                    const QPointF center(x * 10.0, y * 8.0);
                    Polygons subject{kind == 0
                        ? QPolygonF{center + QPointF(-20, -10), center + QPointF(20, -10),
                            center + QPointF(20, 10), center + QPointF(-20, 10)}
                        : ellipse(center, 35, 25, 32)};
                    if (kind == 2) {
                        auto cutout = ellipse(center, 18, 20, 24);
                        std::reverse(cutout.begin(), cutout.end());
                        subject.push_back(cutout);
                    }
                    if (kind == 3) {
                        const auto polygon = subject.front();
                        subject.front() += polygon;
                    }
                    Clipper2Lib::Clipper64 reference;
                    Clipper2Lib::Paths64 output;
                    reference.PreserveCollinear(false);
                    reference.AddSubject(integerPaths(subject));
                    reference.AddClip(clip);
                    require(reference.Execute(Clipper2Lib::ClipType::Difference,
                        Clipper2Lib::FillRule::NonZero, output), QStringLiteral("Reference clipping failed"));
                    require(gui::catalog::subtract(subject, envelope).isEmpty() == output.empty(),
                        QStringLiteral("Prepared containment disagrees with exact clipping at %1,%2, kind %3")
                            .arg(x).arg(y).arg(kind));
                    require(canonical(integerPaths(gui::catalog::subtract(subject, envelope))) == canonical(output),
                        QStringLiteral("Prepared difference changed the exact polygon boundary"));
                    require(reference.Execute(Clipper2Lib::ClipType::Intersection,
                        Clipper2Lib::FillRule::NonZero, output), QStringLiteral("Reference intersection failed"));
                    require(canonical(integerPaths(gui::catalog::intersect(subject, envelope))) == canonical(output),
                        QStringLiteral("Prepared intersection changed the exact polygon boundary"));
                    ++checks;
                }
        for (double offset : {-1e-5, -1e-6, -1e-7, 0.0, 1e-7, 1e-6, 1e-5}) {
            const Polygons subject{QPolygonF{{80, -1}, {100 + offset, -1},
                {100 + offset, 1}, {80, 1}}};
            Clipper2Lib::Clipper64 reference;
            Clipper2Lib::Paths64 output;
            reference.AddSubject(integerPaths(subject));
            reference.AddClip(clip);
            require(reference.Execute(Clipper2Lib::ClipType::Difference,
                Clipper2Lib::FillRule::NonZero, output), QStringLiteral("Reference clipping failed"));
            require(gui::catalog::subtract(subject, envelope).isEmpty() == output.empty(),
                QStringLiteral("Prepared containment changed a subpixel boundary"));
            ++checks;
        }
    }
    for (const QString &fixture : {QStringLiteral("tools/fixtures/compact_fit_cornered_interior.json"),
             QStringLiteral("tools/fixtures/compact_fit_small_contour.json")}) {
        const auto region = gui::catalog::buildRegion(readRequest(fixture), {});
        for (double translation : {0.0, 50000000.0}) {
            Polygons translated;
            const auto transform = QTransform::fromTranslate(translation, -translation);
            for (const auto &polygon : region.required)
                translated.push_back(transform.map(polygon));
            const auto clipPolygons = gui::catalog::unite(translated);
            const auto clip = integerPaths(clipPolygons);
            for (const auto &polygon : clipPolygons)
                for (int index = 0; index < polygon.size(); index += std::max(1, int(polygon.size()) / 32))
                    for (double radius : {0.25, 2.0, 12.0})
                        for (double shift : {-1e-6, 0.0, 1e-6}) {
                            const auto center = polygon[index] + QPointF(shift, -shift);
                            const Polygons subject{QPolygonF{center + QPointF(-radius, -radius),
                                center + QPointF(radius, -radius), center + QPointF(radius, radius),
                                center + QPointF(-radius, radius)}};
                            Clipper2Lib::Clipper64 reference;
                            Clipper2Lib::Paths64 output;
                            reference.PreserveCollinear(false);
                            reference.AddSubject(integerPaths(subject));
                            reference.AddClip(clip);
                            for (const auto operation : {Clipper2Lib::ClipType::Difference,
                                     Clipper2Lib::ClipType::Intersection}) {
                                require(reference.Execute(operation, Clipper2Lib::FillRule::NonZero, output),
                                    QStringLiteral("Reference local clipping failed"));
                                const auto actual = operation == Clipper2Lib::ClipType::Difference
                                    ? gui::catalog::subtract(subject, clipPolygons)
                                    : gui::catalog::intersect(subject, clipPolygons);
                                require(canonical(integerPaths(actual)) == canonical(output),
                                    QStringLiteral("Local clipping changed the exact boundary at comparison %1")
                                        .arg(checks));
                            }
                            ++checks;
                        }
        }
    }
    QTextStream(stdout) << "Polygon containment: " << checks << " exact clipping comparisons passed\n";
}

void preparedGeometryTests() {
    polygonContainmentProofTests();
    int samples = 0;
    for (const QString &fixture : {QStringLiteral("tools/fixtures/compact_fit_cornered_interior.json"),
             QStringLiteral("tools/fixtures/compact_fit_small_contour.json")}) {
        const auto region = gui::catalog::buildRegion(readRequest(fixture), {});
        for (const auto &polygons : {region.required, region.permitted,
                 gui::catalog::expanded(region.required, 2.0)}) {
            const gui::catalog::PointContainment prepared(polygons);
            const auto path = gui::catalog::painterPath(polygons);
            const auto bounds = path.boundingRect().adjusted(-1, -1, 1, 1);
            const auto check = [&](const QPointF &point) {
                require(prepared.contains(point) == path.contains(point),
                    QStringLiteral("Prepared point containment differs at %1,%2")
                        .arg(point.x(), 0, 'g', 16).arg(point.y(), 0, 'g', 16));
                ++samples;
            };
            for (int y = 0; y <= 160; ++y)
                for (int x = 0; x <= 160; ++x)
                    check(bounds.topLeft() + QPointF(bounds.width() * x / 160,
                        bounds.height() * y / 160));
            for (const auto &polygon : polygons)
                for (int index = 0; index < polygon.size(); ++index)
                    for (double fraction : {0.0, 0.5})
                        for (double offset : {-1e-5, -1e-7, 0.0, 1e-7, 1e-5}) {
                            const auto point = polygon[index]
                                + (polygon[(index + 1) % polygon.size()] - polygon[index]) * fraction;
                            check(point + QPointF(offset, 0));
                            check(point + QPointF(0, offset));
                        }
        }
    }
    QTextStream(stdout) << "Prepared geometry: " << samples << " containment comparisons passed\n";
}

void rasterMaskIndexTests() {
#ifdef FLS_HAS_CUDA
    for (const QString &fixture : {QStringLiteral("tools/fixtures/compact_fit_cornered_interior.json"),
             QStringLiteral("tools/fixtures/compact_fit_small_contour.json")}) {
        const auto region = gui::catalog::buildRegion(readRequest(fixture), {});
        for (double translation : {0.0, 50000000.0}) {
            for (double scale : {0.25, 0.371}) {
                gui::compact::gpu::Geometry geometry;
                for (const auto &polygon : region.required) {
                    const int first = static_cast<int>(geometry.points.size());
                    for (const auto &point : polygon)
                        geometry.points.push_back({static_cast<float>(point.x() + translation),
                            static_cast<float>(point.y() + translation)});
                    geometry.loops.push_back({first, static_cast<int>(polygon.size())});
                }
                geometry.pieces.push_back({0, static_cast<int>(geometry.loops.size())});
                std::string error;
                require(gui::compact::gpu::verifyRasterMaskIndex(geometry, scale, &error),
                    QString::fromStdString(error));
            }
        }
    }
    QTextStream(stdout) << "Indexed raster masks match complete edge traversal\n";
#endif
}

void smallContourTests(const QVector<gui::catalog::Primitive> &catalog) {
    const auto request = readRequest(QStringLiteral(FLS_SOURCE_DIR "/tools/fixtures/compact_fit_small_contour.json"));
    gui::compact::FillOptions options;
    options.retainFailedFill = true;
    int previous = 0;
    options.workProgress = [&](int, int evaluated, int budget) {
        require(evaluated >= previous && evaluated <= budget,
            QStringLiteral("Small-contour progress moved backwards or exceeded its budget"));
        previous = evaluated;
    };
    QElapsedTimer timer;
    timer.start();
    const auto result = gui::profile::fillRegion(request, catalog, options);
    require(!result.fill.placements.isEmpty() && result.fill.placements.size() <= 33,
        QStringLiteral("Small-contour repair increased the shape count"));
    require(result.diagnostics.value("missingInteriorArea").toDouble(-1) == 0
        && result.diagnostics.value("missingBeyondInward").toDouble(-1) == 0
        && result.diagnostics.value("outsideEnvelope").toDouble(-1) == 0,
        QStringLiteral("Small-contour repair lost interior coverage or exceeded the envelope"));
    const auto quality = result.diagnostics.value("boundaryQuality").toObject();
    require(quality.value("energy").toDouble() <= 115.181671
        && quality.value("maximumCornerDistance").toDouble() <= 0.5,
        QStringLiteral("Small-contour repair regressed boundary quality"));
    for (const auto &stage : result.diagnostics.value("stageWork").toObject())
        require(stage.toObject().value("evaluations").toInt() >= 0,
            QStringLiteral("Small-contour stage lost evaluation accounting"));
    QTextStream(stdout) << "Small contour: " << result.fill.placements.size() << " shapes, "
        << timer.elapsed() << " ms; coverage and work accounting passed\n";
}

void contourPolishTests(const QVector<gui::catalog::Primitive> &catalog) {
    const auto request = readRequest(QStringLiteral(FLS_SOURCE_DIR "/tools/fixtures/compact_fit_cornered_interior.json"));
    gui::compact::FillOptions options;
    options.retainFailedFill = true;
    const auto result = gui::profile::fillRegion(request, catalog, options);
    const auto quality = result.diagnostics.value("boundaryQuality").toObject();
    const auto polish = result.diagnostics.value("contourPolish").toObject();
    QTextStream(stdout) << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n';
    require(result.diagnostics.value("missingInteriorArea").toDouble(-1) == 0
        && result.diagnostics.value("missingBeyondInward").toDouble(-1) == 0
        && result.diagnostics.value("outsideEnvelope").toDouble(-1) == 0,
        QStringLiteral("Contour polishing lost coverage or exceeded the outer envelope"));
    require(!result.fill.cancelled && result.fill.placements.size() <= 127
        && quality.value("tangentEnergy").toDouble() <= 75
        && quality.value("turnEnergy").toDouble() <= 51
        && quality.value("maximumExcessTurnDegrees").toDouble() <= 125,
        QStringLiteral("Contour polishing regressed count or exposed contour quality"));
    require(polish.value("commits").toInt() > 0 && polish.value("evaluations").toInt() <= 512
        && polish.value("energyAfter").toDouble() < polish.value("energyBefore").toDouble()
        && result.diagnostics.value("evaluations").toInt() <= options.evaluationBudget,
        QStringLiteral("Contour polishing exceeded its work budget or failed to improve quality"));
    require(quality.value("holes").toInt() == 6 && quality.value("components").toInt() == 1
        && quality.value("maximumCornerDistance").toDouble() <= 5.294104,
        QStringLiteral("Contour polishing changed topology or worsened a protected corner"));
    QTextStream(stdout) << "Contour polishing, neighbor growth, count and exact coverage passed\n";
}

void clusterCompactionTests(const QVector<gui::catalog::Primitive> &catalog) {
    const auto request = readRequest(QStringLiteral(FLS_SOURCE_DIR "/tools/fixtures/compact_fit_clustered_boundary.json"));
    gui::compact::FillOptions options;
    options.retainFailedFill = true;
    QElapsedTimer timer;
    timer.start();
    const auto result = gui::profile::fillRegion(request, catalog, options);
    const auto quality = result.diagnostics.value("boundaryQuality").toObject();
    const auto cluster = result.diagnostics.value("clusterCompaction").toObject();
    const auto neighbor = result.diagnostics.value("stageWork").toObject().value("neighborCompaction").toObject();
    require(!result.fill.cancelled && result.fill.placements.size() <= 128,
        QStringLiteral("Cluster compaction did not retain its shape-count improvement"));
    require(result.diagnostics.value("missingInteriorArea").toDouble(-1) == 0
        && result.diagnostics.value("missingBeyondInward").toDouble(-1) == 0
        && result.diagnostics.value("outsideEnvelope").toDouble(-1) == 0,
        QStringLiteral("Cluster compaction lost interior coverage or exceeded the envelope"));
    require(quality.value("tangentEnergy").toDouble() <= 35
        && quality.value("turnEnergy").toDouble() <= 14
        && quality.value("energy").toDouble() <= 724
        && quality.value("maximumCornerDistance").toDouble() <= 2.737574
        && quality.value("maximumExcessTurnDegrees").toDouble() <= 111.545174,
        QStringLiteral("Cluster compaction regressed exposed contour quality"));
    require(quality.value("holes").toInt() == 6 && quality.value("components").toInt() == 1
        && quality.value("protectedCorners").toInt() == 26,
        QStringLiteral("Cluster compaction changed contour topology or corner protection"));
    require(cluster.value("removals").toInt() > 0 && cluster.value("evaluations").toInt() <= 768
        && neighbor.value("evaluations").toInt() <= 1536
        && result.diagnostics.value("evaluations").toInt() <= options.evaluationBudget,
        QStringLiteral("Cluster compaction exceeded its work limit or accepted no replacement"));
    QTextStream(stdout) << "Cluster compaction: " << result.fill.placements.size() << " shapes, "
        << timer.elapsed() << " ms; exact coverage, topology and contour quality passed\n";
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
        QVector<gui::compact::BoundaryDefect> defects;
        const auto sampled = model.measure(coverage, global, QRectF(), &defects);
        double defectEnergy = 0;
        for (const auto &defect : defects) {
            require(std::isfinite(defect.energy) && defect.energy > 0,
                QStringLiteral("Contour defect has an invalid score"));
            defectEnergy += defect.energy;
        }
        require(std::abs(defectEnergy - reference.tangentEnergy - 2 * reference.turnEnergy) < 1e-5
            && std::abs(model.energy(sampled) - model.energy(reference)) < 1e-7,
            QStringLiteral("Contour defect ranking differs from measured continuity"));
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
#ifdef FLS_HAS_CUDA
    gpuRasterRankTests();
#endif
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

void shapeConfigurationTests(const gui::ShapeGeometryStore &geometry) {
    using gui::catalog::ShapeTask;
    const auto directory = QStringLiteral(FLS_SOURCE_DIR "/build/compact-fit-config-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(QDir().mkpath(directory), QStringLiteral("Configuration test directory is unavailable"));
    const auto cleanup = qScopeGuard([&] { QDir(directory).removeRecursively(); });
    QFile source(QStringLiteral(FLS_SOURCE_DIR "/assets/compact_fit_shapes.json"));
    require(source.open(QIODevice::ReadOnly), QStringLiteral("Cannot read source configuration: ") + source.fileName());
    const auto original = QJsonDocument::fromJson(source.readAll()).object();
    const auto path = QDir(directory).filePath(QStringLiteral("compact_fit_shapes.json"));
    const auto write = [&](const QByteArray &bytes) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), QStringLiteral("Cannot write configuration: ") + path);
        require(file.write(bytes) == bytes.size(), QStringLiteral("Incomplete configuration write: ") + path);
    };
    QString error;
    write(QJsonDocument(original).toJson());
    const auto baseline = gui::catalog::buildCatalog(geometry, path, &error);
    require(error.isEmpty() && baseline.size() == 96,
        QStringLiteral("Baseline catalog has %1 shapes: %2").arg(baseline.size()).arg(error));
    require(gui::catalog::shapeIdsForTask(baseline, ShapeTask::Interior) == QVector<int>({102, 101, 109, 110, 124, 2117}),
        QStringLiteral("Configuration changed interior seed order"));
    require(gui::catalog::shapeIdsForTask(baseline, ShapeTask::Curves).size() == 25
        && gui::catalog::shapeIdsForTask(baseline, ShapeTask::Curves).contains(2133),
        QStringLiteral("Configuration changed the default curve families"));
    for (const auto &primitive : baseline)
        require(gui::catalog::usesTask(primitive, ShapeTask::CutoutReplacements) == (primitive.shape.contours.size() > 1),
            QStringLiteral("Configuration changed cutout replacement eligibility"));
    auto modified = original;
    auto tasks = original.value("tasks").toObject();
    tasks.insert("group_replacements", QJsonObject{{"all", true}, {"exclude", QJsonArray{111, 112, 113, 114, 115, 116, 117, 118, 119}}});
    modified.insert("tasks", tasks);
    write(QJsonDocument(modified).toJson());
    const auto filtered = gui::catalog::buildCatalog(geometry, path, &error);
    require(error.isEmpty() && filtered.size() == baseline.size(), error);
    require(gui::catalog::shapeIdsForTask(filtered, ShapeTask::GroupReplacements).size() == baseline.size() - 9,
        QStringLiteral("Task exclusions changed the global catalog or retained frames"));
    require(gui::catalog::shapeIdsForTask(baseline, ShapeTask::GroupReplacements).size() == baseline.size()
        && baseline.front().configuration->value("sha256") != filtered.front().configuration->value("sha256"),
        QStringLiteral("Reload changed the previous configuration snapshot"));
    for (auto entry = tasks.begin(); entry != tasks.end(); ++entry)
        entry.value() = QJsonArray{};
    tasks.insert("interior", QJsonArray{102});
    modified.insert("tasks", tasks);
    write(QJsonDocument(modified).toJson());
    const auto circles = gui::catalog::buildCatalog(geometry, path, &error);
    require(error.isEmpty(), error);
    gui::compact::FillOptions options;
    options.seedOnly = true;
    options.useGpu = false;
    options.shapeBudget = 64;
    options.evaluationBudget = 1200;
    const gui::PenFillRequest request{{}, {polygonLoop({{0, 0}, {40, 0}, {40, 30}, {0, 30}})}};
    const auto result = gui::profile::fillRegion(request, circles, options);
    require(!result.fill.placements.isEmpty()
        && std::all_of(result.fill.placements.cbegin(), result.fill.placements.cend(),
            [](const gui::PenPlacement &placement) { return placement.shapeId == 102; }),
        QStringLiteral("The profile seed ignored reloaded task eligibility"));
    require(result.diagnostics.value("profiles").toInt(-1) == 0,
        QStringLiteral("Disabled curve fitting still generated profiles"));
    auto disabled = original;
    auto ids = original.value("shape_ids").toArray();
    ids.removeAt(0);
    disabled.insert("shape_ids", ids);
    write(QJsonDocument(disabled).toJson());
    const auto withoutSquare = gui::catalog::buildCatalog(geometry, path, &error);
    require(error.isEmpty() && withoutSquare.size() == baseline.size() - 1
        && !gui::catalog::shapeIdsForTask(withoutSquare, ShapeTask::Interior).contains(101),
        QStringLiteral("Global shape disabling did not override task membership"));
    const auto reject = [&](const QJsonObject &configuration, const QString &message) {
        write(QJsonDocument(configuration).toJson());
        require(gui::catalog::buildCatalog(geometry, path, &error).isEmpty() && error.contains(message)
            && error.contains(path), QStringLiteral("Invalid configuration was accepted: ") + error);
    };
    modified = original;
    modified.insert("schema_version", 2);
    reject(modified, "schema_version");
    modified = original;
    tasks = original.value("tasks").toObject();
    tasks.insert("curve", QJsonArray{102});
    modified.insert("tasks", tasks);
    reject(modified, "Unknown tasks field");
    tasks = original.value("tasks").toObject();
    tasks.insert("interior", QJsonArray{99999999});
    modified.insert("tasks", tasks);
    reject(modified, "unavailable shape ID");
    tasks.insert("interior", QJsonArray{102, 102});
    modified.insert("tasks", tasks);
    reject(modified, "duplicate shape ID");
    tasks.insert("interior", QJsonArray{102.5});
    modified.insert("tasks", tasks);
    reject(modified, "invalid shape ID");
    tasks = original.value("tasks").toObject();
    tasks.insert("straight_edges", QJsonArray{102});
    modified.insert("tasks", tasks);
    reject(modified, "supports only shape ID 101");
    tasks = original.value("tasks").toObject();
    tasks.insert("group_replacements", QJsonObject{{"all", "false"}});
    modified.insert("tasks", tasks);
    reject(modified, "must be a boolean");
    write("{invalid");
    require(gui::catalog::buildCatalog(geometry, path, &error).isEmpty() && error.contains("Invalid JSON"),
        QStringLiteral("Malformed JSON silently reused an earlier configuration"));
    QTextStream(stdout) << "Shape configuration reload, snapshots, task exclusions and validation passed\n";
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTextStream output(stdout);
    auto arguments = application.arguments();
    QString configurationPath;
    QDir::setCurrent(QStringLiteral(FLS_SOURCE_DIR));
    try {
        const int configurationArgument = arguments.indexOf(QStringLiteral("--shape-config"));
        if (configurationArgument >= 0) {
            require(configurationArgument + 1 < arguments.size(), QStringLiteral("--shape-config requires a JSON path"));
            configurationPath = arguments[configurationArgument + 1];
            arguments.removeAt(configurationArgument + 1);
            arguments.removeAt(configurationArgument);
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--comparison-project-tests")) {
            comparisonExportTests();
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--release-catalog-test")) {
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
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--shape-config-tests")) {
            shapeConfigurationTests(geometry);
            return 0;
        }
        if (arguments.size() == 3
            && arguments[1] == QStringLiteral("--audit")) {
            auditCatalog(geometry, arguments[2]);
            output << "All visible catalog triangle unions audited\n";
            return 0;
        }
        const QVector<gui::catalog::Primitive> fullCatalog = configurationPath.isEmpty()
            ? gui::catalog::buildCatalog(geometry, &error)
            : gui::catalog::buildCatalog(geometry, configurationPath, &error);
        require(error.isEmpty(), error);
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--small-contour-tests")) {
            smallContourTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 3 && arguments[1] == QStringLiteral("--exact-reduction")) {
            benchmarkExactReduction(arguments[2], fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--compact-search-tests")) {
            compactSearchTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--coverage-repair-tests")) {
            coverageRepairTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--interior-coverage-tests")) {
            interiorCoverageTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--contour-polish-tests")) {
            contourPolishTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--cluster-compaction-tests")) {
            clusterCompactionTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && (arguments[1] == QStringLiteral("--cubic-interior-tests")
            || arguments[1] == QStringLiteral("--cornered-interior-tests"))) {
            const bool cornered = arguments[1] == QStringLiteral("--cornered-interior-tests");
            const QString fixture = cornered
                ? QStringLiteral(FLS_SOURCE_DIR "/tools/fixtures/compact_fit_cornered_interior.json")
                : QStringLiteral(FLS_SOURCE_DIR "/tools/fixtures/compact_fit_cubic_interior.json");
            const auto request = readRequest(fixture);
            const double expectedArea = cornered ? 222460.76653281302 : 222085.6647436226;
            require(std::abs(gui::catalog::buildRegion(request, {}).area - expectedArea) < 1e-5,
                QStringLiteral("Replay changed the logged cubic contour"));
            gui::compact::FillOptions options;
            options.retainFailedFill = true;
            QElapsedTimer timer;
            timer.start();
            const auto result = gui::profile::fillRegion(
                request,
                fullCatalog, options);
            output << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n';
            require(!result.fill.cancelled && result.diagnostics.contains("missingInteriorArea")
                && result.diagnostics.value("missingInteriorArea").toDouble() == 0.0
                && result.diagnostics.value("missingBeyondInward").toDouble() == 0.0
                && result.diagnostics.value("outsideEnvelope").toDouble() == 0.0,
                QStringLiteral("Cubic contour repair left missing interior support or exceeded the envelope"));
            require(result.diagnostics.value("targetBoundary").toObject().value("protectedCorners").toInt()
                    == (cornered ? 24 : 18), QStringLiteral("Replay changed the protected corners"));
            require(result.fill.placements.size() <= (cornered ? 151 : 174)
                && result.diagnostics.value("evaluations").toInt() <= options.evaluationBudget,
                QStringLiteral("Cubic contour repair exceeded its count or work allowance"));
            require(result.diagnostics.value("areaError").toDouble()
                <= result.diagnostics.value("areaErrorLimit").toDouble(),
                QStringLiteral("Cubic contour repair exceeded the area error allowance"));
            if (cornered) {
                const auto quality = result.diagnostics.value("boundaryQuality").toObject();
                require(quality.value("tangentEnergy").toDouble() <= 83.54
                    && quality.value("turnEnergy").toDouble() <= 63.83
                    && quality.value("cornerDefects").toInt() <= 85,
                    QStringLiteral("Cornered compaction regressed contour continuity"));
            }
            output << "Cubic interior coverage regression passed: " << result.fill.placements.size()
                << " placements, " << timer.elapsed() << " ms\n";
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--quality-examples")) {
            qualityExamples();
            return 0;
        }
        if (arguments.size() >= 2 && (arguments[1] == QStringLiteral("--thin-tests")
            || arguments[1] == QStringLiteral("--lining-detection-tests")
            || arguments[1] == QStringLiteral("--thin-source-detect")
            || arguments[1] == QStringLiteral("--thin-source-quality")
            || arguments[1] == QStringLiteral("--thin-source-raster-check")
            || arguments[1] == QStringLiteral("--thin-quality-tests")
            || arguments[1] == QStringLiteral("--thin-color-reference-quality")
            || arguments[1] == QStringLiteral("--thin-fit") || arguments[1] == QStringLiteral("--thin-reference")
            || arguments[1] == QStringLiteral("--thin-reference-quality-seed")
            || arguments[1] == QStringLiteral("--thin-reference-quality"))) {
            gui::ShapeGeometryStore geometry;
            QString error;
            require(geometry.loadDefault(&error), error);
            const auto thinCatalog = gui::thin::buildCatalog(geometry, &error);
            require(!thinCatalog.isEmpty(), error);
            if (arguments[1] == QStringLiteral("--thin-source-raster-check")) {
                require(arguments.size() == 3, QStringLiteral("Expected a saved source fit project"));
                sourceLiningRasterCheck(arguments[2], geometry);
                return 0;
            }
            if (arguments[1] == QStringLiteral("--lining-detection-tests")) {
                liningDetectionTests();
                return 0;
            }
            if (arguments[1] == QStringLiteral("--thin-source-detect")
                || arguments[1] == QStringLiteral("--thin-source-quality")) {
                require(arguments.size() == 4, QStringLiteral("Expected a source project and comparison output project"));
                sourceLiningTrial(arguments[2], arguments[3], geometry, thinCatalog,
                    arguments[1] == QStringLiteral("--thin-source-quality"));
                return 0;
            }
            if (arguments[1] == QStringLiteral("--thin-color-reference-quality")) {
                require(arguments.size() == 3 || arguments.size() == 4, QStringLiteral("Expected a lining project and optional output project"));
                fitColoredLiningReference(arguments[2], arguments.size() == 4
                    ? std::optional<QString>{arguments[3]} : std::nullopt, geometry, thinCatalog);
                return 0;
            }
            if (arguments[1] == QStringLiteral("--thin-tests") || arguments[1] == QStringLiteral("--thin-quality-tests")) {
                thinFitTests(thinCatalog, arguments[1] == QStringLiteral("--thin-quality-tests"));
                return 0;
            }
            require(arguments.size() >= 3, QStringLiteral("Expected thin contour or reference project"));
            gui::thin::FillOptions options;
            const bool seedOnly = arguments[1] == QStringLiteral("--thin-reference-quality-seed");
            bool seedComplete = false;
            int seedPlacementCount = 0;
            if (arguments[1] == QStringLiteral("--thin-reference-quality") || seedOnly)
                options = gui::thin::qualityOptions();
            const bool reference = arguments[1].startsWith(QStringLiteral("--thin-reference"));
            QVector<gui::PenPlacement> exceptionPlacements;
            const auto request = reference
                ? thinReferenceRequest(arguments[2], geometry, &options.leeway, &exceptionPlacements)
                : readRequest(arguments[2]);
            QElapsedTimer timer;
            timer.start();
            options.phaseProgress = [&](const QString &name) {
                output << "Phase " << name << ' ' << timer.elapsed() << " ms\n" << Qt::flush;
                seedComplete = seedComplete || name == QStringLiteral("growth");
            };
            options.workProgress = [&](int count, int evaluated, int budget) {
                if (count > 0 && evaluated == 0 && budget == 0) {
                    seedPlacementCount = count;
                    output << "Seed " << count << " shapes " << timer.elapsed() << " ms\n" << Qt::flush;
                }
            };
            gui::catalog::Polygons polygons;
            if (reference)
                for (const auto &loop : request.loops) {
                    QPolygonF polygon;
                    for (const auto &point : loop.points)
                        polygon.push_back(point.position);
                    polygons.push_back(polygon);
                }
            const auto cancelled = [&] { return seedOnly && seedComplete; };
            auto result = polygons.isEmpty() ? gui::thin::fillRegion(request, thinCatalog, options, cancelled)
                : gui::thin::fillPolygons(polygons, thinCatalog, options, cancelled);
            result.diagnostics.insert("elapsedMilliseconds", timer.elapsed());
            result.diagnostics.insert("count", seedOnly ? seedPlacementCount : result.fill.placements.size());
            result.diagnostics.insert("seedOnly", seedOnly);
            result.diagnostics.insert("error", result.fill.error);
            result.diagnostics.insert("exceptionShapes", exceptionPlacements.size());
            output << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
            if (seedOnly) {
                require(seedComplete, QStringLiteral("Lining seed did not complete"));
                return 0;
            }
            require(result.fill.error.isEmpty(), result.fill.error);
            if (arguments.size() == 4 && reference)
                saveComparison(arguments[2], arguments[3], result.fill, QStringLiteral("Thin Region Fit"),
                    outputPath(result.fill, thinCatalog).boundingRect(), exceptionPlacements);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--convex-union-tests")) {
            convexUnionTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--expansion-geometry-tests")) {
            expansionGeometryTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--prepared-geometry-tests")) {
            preparedGeometryTests();
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--raster-mask-index-tests")) {
            rasterMaskIndexTests();
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--fast-quality-tests")) {
            fastQualityTests();
            return 0;
        }
        if (arguments.size() == 3 && arguments[1] == QStringLiteral("--benchmark-boundary")) {
            benchmarkBoundary(arguments[2], fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--failed-fill-tests")) {
            failedFillTests(fullCatalog);
            return 0;
        }
        if (arguments.size() == 2 && (arguments[1] == QStringLiteral("--compact-tests")
            || arguments[1] == QStringLiteral("--profile-tests"))) {
            compactTests(fullCatalog, arguments[1] == QStringLiteral("--profile-tests"));
            return 0;
        }
        if (arguments.size() >= 3 && (arguments[1] == QStringLiteral("--compact-fit")
            || arguments[1] == QStringLiteral("--profile-fit")
            || arguments[1] == QStringLiteral("--profile-project")
            || arguments[1] == QStringLiteral("--profile-retained")
            || arguments[1] == QStringLiteral("--profile-polish")
            || arguments[1] == QStringLiteral("--profile-repeat")
            || arguments[1] == QStringLiteral("--compact-project")
            || arguments[1] == QStringLiteral("--compact-polish")
            || arguments[1] == QStringLiteral("--compact-repeat"))) {
            gui::compact::FillOptions options;
            options.retainFailedFill = arguments[1] == QStringLiteral("--profile-retained");
            if (arguments.size() > 3) {
                options.boundaryAllowance = arguments[3].toDouble();
            }
            if (arguments[1] == QStringLiteral("--compact-polish")) {
                require(arguments.size() == 6, QStringLiteral("Expected request, allowance, project and seed group"));
                options.initialPlacements = projectSeed(arguments[4], arguments[5]);
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
            const bool profile = arguments[1].startsWith(QStringLiteral("--profile-"));
            auto result = profile ? gui::profile::fillRegion(readRequest(arguments[2]), fullCatalog, options)
                : gui::compact::fillRegion(readRequest(arguments[2]), fullCatalog, options, {},
                [&](int count, double missing, double spill) {
                    output << count << " shapes, missing " << missing << ", spill " << spill
                           << ", " << timer.elapsed() << " ms\n" << Qt::flush;
                });
            QJsonObject footprintCounts;
            for (double threshold : {1.0, 10.0, 25.0, 100.0}) {
                int count = 0;
                double area = 0.0;
                for (const auto &placement : result.fill.placements) {
                    const auto primitive = std::find_if(fullCatalog.cbegin(), fullCatalog.cend(), [&](const auto &entry) {
                        return entry.shape.shapeId == placement.shapeId;
                    });
                    require(primitive != fullCatalog.cend(), QStringLiteral("Replay placement missing from catalog"));
                    const double footprint = gui::catalog::area(gui::catalog::mapped(primitive->shape, placement.transform));
                    if (footprint < threshold) {
                        ++count;
                        area += footprint;
                    }
                }
                footprintCounts.insert(QString::number(threshold), QJsonObject{{"count", count}, {"area", area}});
            }
            result.diagnostics.insert(QStringLiteral("replayFootprintsBelow"), footprintCounts);
            QJsonObject shapeCounts;
            for (const auto &placement : result.fill.placements) {
                const auto id = QString::number(placement.shapeId);
                shapeCounts.insert(id, shapeCounts.value(id).toInt() + 1);
            }
            result.diagnostics.insert(QStringLiteral("replayShapeCounts"), shapeCounts);
            result.diagnostics.insert(QStringLiteral("replayCount"), result.fill.placements.size());
            result.diagnostics.insert(QStringLiteral("replayError"), result.fill.error);
            if (qEnvironmentVariableIsSet("FLS_PROFILE_GEOMETRY"))
                result.diagnostics.insert(QStringLiteral("geometryPerformance"), gui::catalog::geometryPerformance());
            result.diagnostics.insert(QStringLiteral("replayElapsedMilliseconds"), timer.elapsed());
            output << QJsonDocument(result.diagnostics).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
            output << "Completed in " << timer.elapsed() << " ms\n" << Qt::flush;
            if (options.retainFailedFill) {
                require(!result.fill.cancelled && !result.fill.placements.isEmpty(), QStringLiteral("Replay returned no retained fill"));
                output << "Retained " << result.fill.placements.size() << " shapes: " << result.fill.error << '\n' << Qt::flush;
                if (arguments.size() == 6) {
                    saveComparison(arguments[4], arguments[5], result.fill,
                        QStringLiteral("Compact Fit - local quality"));
                }
            } else {
                require(result.fill.error.isEmpty(), result.fill.error);
                requireApproximation(readRequest(arguments[2]), result, fullCatalog, options);
            }
            if (arguments[1] == QStringLiteral("--compact-project") || arguments[1] == QStringLiteral("--profile-project")) {
                require(arguments.size() == 6, QStringLiteral("Expected request, allowance, source project and new destination"));
                saveComparison(arguments[4], arguments[5], result.fill,
                    profile ? QStringLiteral("Compact Fit - curve first") : QStringLiteral("Compact Fit - continuity"));
            }
            if (arguments[1] == QStringLiteral("--compact-repeat") || arguments[1] == QStringLiteral("--profile-repeat")) {
                const auto repeat = profile ? gui::profile::fillRegion(readRequest(arguments[2]), fullCatalog, options)
                    : gui::compact::fillRegion(readRequest(arguments[2]), fullCatalog, options);
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
        if ((arguments.size() == 4 || arguments.size() == 5)
            && arguments[1] == QStringLiteral("--compare-project")) {
            compareProject(arguments[2], readRequest(arguments[3]), fullCatalog,
                arguments.size() == 5 ? arguments[4] : QString());
            return 0;
        }
        const bool budgetedProfileReplay = arguments.size() == 4
            && (arguments[1] == QStringLiteral("--replay-profile")
                || arguments[1] == QStringLiteral("--replay-profile-cpu"));
        if ((arguments.size() == 3 || budgetedProfileReplay)
            && arguments[1].startsWith(QStringLiteral("--replay"))) {
            gui::catalog::FillOptions replayOptions;
            if (arguments[1] == QStringLiteral("--replay-mesh")) {
                replayOptions.candidateLimit = 0;
                replayOptions.searchNodes = 0;
            } else if (arguments[1] == QStringLiteral("--replay-seed")
                       || arguments[1] == QStringLiteral("--replay-seed-cpu")) {
                // Seed-only replay keeps large contour comparisons practical.
            } else if (arguments[1] == QStringLiteral("--replay-compact")) {
                replayOptions.candidateLimit = 1;
                replayOptions.searchNodes = 0;
            } else {
                require(arguments[1] == QStringLiteral("--replay")
                        || arguments[1] == QStringLiteral("--replay-profile")
                        || arguments[1] == QStringLiteral("--replay-profile-cpu"),
                        QStringLiteral("Unknown replay mode"));
            }
            const auto request = readRequest(arguments[2]);
            QFile log(arguments[2]);
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
            gui::compact::FillOptions profileOptions;
            if (budgetedProfileReplay) {
                bool validBudget = false;
                const int budget = arguments[3].toInt(&validBudget);
                require(validBudget && budget > 0, QStringLiteral("Invalid profile evaluation budget"));
                profileOptions.evaluationBudget = budget;
            }
            profileOptions.retainFailedFill = true;
            profileOptions.useGpu = arguments[1] != QStringLiteral("--replay-profile-cpu")
                && arguments[1] != QStringLiteral("--replay-seed-cpu");
            profileOptions.seedOnly = arguments[1] == QStringLiteral("--replay-seed")
                || arguments[1] == QStringLiteral("--replay-seed-cpu");
            const auto replay = arguments[1] == QStringLiteral("--replay-seed")
                || arguments[1] == QStringLiteral("--replay-seed-cpu")
                || arguments[1] == QStringLiteral("--replay-profile")
                || arguments[1] == QStringLiteral("--replay-profile-cpu")
                ? gui::profile::fillRegion(request, fullCatalog, profileOptions)
                : gui::catalog::fillRegion(request, fullCatalog, replayOptions);
            if ((arguments[1] == QStringLiteral("--replay-profile")
                 || arguments[1] == QStringLiteral("--replay-profile-cpu"))
                && !replay.fill.placements.isEmpty()) {
                const auto region = gui::catalog::buildRegion(request, {});
                const QRectF bounds = region.bounds.adjusted(-20, -20, 20, 20);
                QImage preview(std::max(1, static_cast<int>(std::ceil(bounds.width()))),
                               std::max(1, static_cast<int>(std::ceil(bounds.height()))),
                               QImage::Format_ARGB32_Premultiplied);
                preview.fill(QColor(143, 143, 143));
                QPainter painter(&preview);
                painter.setTransform(QTransform(1, 0, 0, -1, -bounds.left(), bounds.bottom()));
                painter.setPen(Qt::NoPen);
                painter.fillPath(region.requiredPath, QColor(170, 75, 112));
                painter.fillPath(outputPath(replay.fill, fullCatalog), QColor(39, 182, 235, 160));
                painter.end();
                preview.save(QStringLiteral("build/profile-replay.bmp"));
            }
            output << QJsonDocument(replay.diagnostics).toJson(QJsonDocument::Compact) << '\n';
            if (arguments[1] != QStringLiteral("--replay-seed")
                && arguments[1] != QStringLiteral("--replay-seed-cpu"))
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
            {{-40, -20},
             gui::PenPointKind::Hard,
             {-13.3333333333333, 13.3333333333333},
             {26.6666666666667, -16.6666666666667},
             true},
            {{40, -20},
             gui::PenPointKind::Hard,
             {-26.6666666666667, -16.6666666666667},
             {13.3333333333333, 13.3333333333333},
             true},
            {{40, 20},
             gui::PenPointKind::Hard,
             {13.3333333333333, -13.3333333333333},
             {-26.6666666666667, 16.6666666666667},
             true},
            {{-40, 20},
             gui::PenPointKind::Hard,
             {26.6666666666667, 16.6666666666667},
             {-13.3333333333333, -13.3333333333333},
             true},
        };
        // These unequal handles cannot be represented by a single quadratic span.
        curved.points[0].outgoing = {18, -24};
        curved.points[1].incoming = {-34, -12};
        const auto cubicRegion = gui::catalog::buildRegion(curved, {});
        double exactArea = 0;
        for (const auto &segment : gui::penSegments(curved.points))
            exactArea += segment.signedArea();
        require(std::abs(cubicRegion.originalArea - std::abs(exactArea)) < 1e-8,
                QStringLiteral("Catalog region lost the native cubic area"));
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
                gui::transformPenPoint(point, transform);
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
