#include "contour_mesh.h"
#include "contour_mesh_diagnostics.h"
#include "editor_state.h"
#include "project_codec.h"
#include "scene_codec.h"
#include <clipper2/clipper.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <stdexcept>

namespace {

using namespace fls::scene;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

ContourData rectangle(double left, double top, double size) {
    ContourData data;
    data.closed = true;
    for (const QPointF position : {QPointF(left, top), QPointF(left + size, top),
                                  QPointF(left + size, top + size), QPointF(left, top + size)}) {
        data.points.push_back({position, ContourPointKind::Hard});
    }
    return data;
}

ContourData box(double left, double top, double width, double height) {
    auto data = rectangle(0, 0, 1);
    for (auto &point : data.points) point.position = QPointF(left + point.position.x() * width, top + point.position.y() * height);
    return data;
}

QPainterPath filledContour(const ContourData &data) {
    auto path = gui::penPath(data.points, true);
    path.setFillRule(Qt::OddEvenFill);
    for (const auto &cutout : data.cutouts) path.addPath(gui::penPath(cutout, true));
    return path;
}

void requireMaterialRetained(const ContourData &before, const ContourData &after) {
    const auto original = filledContour(before);
    const auto result = filledContour(after);
    double area = 0.0;
    for (const auto &polygon : original.subtracted(result).toFillPolygons()) {
        for (int index = 0; index < polygon.size(); ++index) {
            const auto first = polygon[index];
            const auto second = polygon[(index + 1) % polygon.size()];
            area += first.x() * second.y() - first.y() * second.x();
        }
    }
    require(std::abs(area) < 1e-6, "region reconstruction must retain the original filled material");
    require(gui::buildPenContour(after.points).valid(), "reconstructed outer boundary must remain simple with positive area");
}

void regionTopology() {
    for (const bool reverse : {false, true}) {
        MeshData mesh;
        auto first = box(0, 0, 2, 100);
        auto second = box(2.2, 0, 2, 100);
        if (reverse) std::reverse(second.points.begin(), second.points.end());
        mesh.appendContour(first, QStringLiteral("Left"), true, 1.0);
        mesh.appendContour(second, QStringLiteral("Right"), true, 1.0);
        require(gui::connectMeshContours(mesh), "facing boundaries must reconstruct their empty gap region");
        requireMaterialRetained(first, mesh.contourData(0));
        requireMaterialRetained(second, mesh.contourData(1));
        for (const auto &a : mesh.contours[0].points) {
            for (const auto &b : mesh.contours[1].points) {
                if (a.vertex == b.vertex) require(std::abs(mesh.vertices[a.vertex].x() - 2.1) < 1e-8,
                    "opposite sides must not participate in the reconstructed seam");
            }
        }
    }

    MeshData nested;
    nested.appendContour(box(0, 0, 10, 100), QStringLiteral("Outer"), true, 1.0);
    nested.appendContour(box(1, 1, 8, 98), QStringLiteral("Inner"), true, 1.0);
    const auto before = nested;
    require(!gui::connectMeshContours(nested), "occupied regions must not be reinterpreted as repairable gaps");
    require(nested == before, "rejected reconstruction must retain all source geometry");

    MeshData holes;
    auto frame = rectangle(0, 0, 100);
    frame.cutouts.push_back(rectangle(20, 20, 60).points);
    auto neighbor = rectangle(102, 0, 100);
    holes.appendContour(frame, QStringLiteral("Frame"), true, 1.0);
    holes.appendContour(neighbor, QStringLiteral("Neighbor"), true, 1.0);
    require(gui::connectMeshContours(holes), "region repair must support contours with holes");
    requireMaterialRetained(frame, holes.contourData(0));
    requireMaterialRetained(neighbor, holes.contourData(1));
    require(holes.contourData(0).cutouts.size() == 1 && !filledContour(holes.contourData(0)).contains(QPointF(50, 50)),
            "reconstruction must retain the hole and its containment");

    MeshData partition;
    const QVector<ContourData> originals{rectangle(0, 0, 100), rectangle(102, 0, 100), box(100.5, 0, 1, 100)};
    for (int index = 0; index < originals.size(); ++index) partition.appendContour(originals[index], QString::number(index), true, 1.0);
    require(gui::connectMeshContours(partition), "gap regions must respect intervening labelled faces");
    for (int index = 0; index < originals.size(); ++index) requireMaterialRetained(originals[index], partition.contourData(index));

    MeshData curved;
    ContourData left;
    ContourData right;
    left.closed = right.closed = true;
    left.points = {{{0, 0}, ContourPointKind::Hard, {0, 100.0 / 3.0}, {100.0 / 3.0, 0}, true},
                   {{100, 0}, ContourPointKind::Soft, {-100.0 / 3.0, 0}, {10, 100.0 / 3.0}, true},
                   {{100, 100}, ContourPointKind::Soft, {10, -100.0 / 3.0}, {-100.0 / 3.0, 0}, true},
                   {{0, 100}, ContourPointKind::Hard, {100.0 / 3.0, 0}, {0, -100.0 / 3.0}, true}};
    right.points = {{{102, 0}, ContourPointKind::Soft, {10, 100.0 / 3.0}, {98.0 / 3.0, 0}, true},
                    {{200, 0}, ContourPointKind::Hard, {-98.0 / 3.0, 0}, {0, 100.0 / 3.0}, true},
                    {{200, 100}, ContourPointKind::Hard, {0, -100.0 / 3.0}, {-98.0 / 3.0, 0}, true},
                    {{102, 100}, ContourPointKind::Soft, {98.0 / 3.0, 0}, {10, -100.0 / 3.0}, true}};
    curved.appendContour(left, QStringLiteral("Curved left"), true, 1.0);
    curved.appendContour(right, QStringLiteral("Curved right"), true, 1.0);
    const auto curvedInput = curved;
    QJsonObject curvedDiagnostics;
    QString curvedError;
    if (!gui::connectMeshContours(curved, &curvedError, &curvedDiagnostics)) {
        gui::writeMeshReconstructionDiagnostics(curved, nullptr, curvedDiagnostics, {QStringLiteral("curved_partition")});
        throw std::runtime_error(curvedError.toStdString());
    }
    double leftArea = 0.0;
    double rightArea = 0.0;
    for (const auto &segment : gui::penSegments(curved.contourData(0).points)) leftArea += segment.signedArea();
    for (const auto &segment : gui::penSegments(curved.contourData(1).points)) rightArea += segment.signedArea();
    require(leftArea >= 10500.0 && rightArea >= 9300.0
            && std::abs((leftArea - 10500.0) - (rightArea - 9300.0)) < 1.0,
            "closed-curve reconstruction must distribute growth between both neighbors");
    require(gui::buildPenContour(curved.contourData(0).points).valid() && gui::buildPenContour(curved.contourData(1).points).valid(),
            "reconstructed curved boundaries must remain simple");
    const auto leftRegion = filledContour(curved.contourData(0));
    const auto rightRegion = filledContour(curved.contourData(1));
    for (const double y : {1.0, 10.0, 25.0, 50.0, 75.0, 90.0, 99.0}) {
        const double parameter = y / 100.0;
        const double boundary = 101.0 + 30.0 * parameter * (1.0 - parameter);
        if (!(leftRegion.contains(QPointF(0.25, y)) && leftRegion.contains(QPointF(boundary - 0.25, y))
                && !leftRegion.contains(QPointF(boundary + 0.25, y)))) {
            gui::writeMeshReconstructionDiagnostics(curvedInput, &curved, curvedDiagnostics, {QStringLiteral("parallel_curves")},
                QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("parallel_diagnostics")));
            throw std::runtime_error(QStringLiteral("curved region containment y=%1, inner=%2, left=%3, right=%4")
                .arg(y).arg(leftRegion.contains(QPointF(0.25, y))).arg(leftRegion.contains(QPointF(boundary - 0.25, y)))
                .arg(leftRegion.contains(QPointF(boundary + 0.25, y))).toStdString());
        }
        require(rightRegion.contains(QPointF(boundary + 2.25, y)) && rightRegion.contains(QPointF(199.75, y)),
                "recipient material must retain its curved region");
    }
}

double contourArea(const ContourData &data) {
    double area = 0.0;
    for (const auto &segment : gui::penSegments(data.points)) area += segment.signedArea();
    return std::abs(area);
}

void overlappingRegions() {
    for (const double scale : {0.001, 1.0, 1000.0}) {
        for (const bool reversed : {false, true}) {
            MeshData mesh;
            auto first = rectangle(0, 0, 100 * scale);
            auto second = rectangle(98 * scale, 0, 100 * scale);
            if (reversed) std::reverse(second.points.begin(), second.points.end());
            mesh.appendContour(first, QStringLiteral("First"), true, 1.0);
            mesh.appendContour(second, QStringLiteral("Second"), true, 1.0);
            QJsonObject diagnostics;
            QString error;
            if (!gui::connectMeshContours(mesh, &error, &diagnostics)) {
                gui::writeMeshReconstructionDiagnostics(mesh, nullptr, diagnostics, {QStringLiteral("overlap_regression")});
                throw std::runtime_error(QStringLiteral("overlap scale=%1 reversed=%2: %3").arg(scale).arg(reversed).arg(error).toStdString());
            }
            const double normalization = scale * scale;
            require(std::abs(contourArea(mesh.contourData(0)) / normalization - 10000.0) < 1e-6,
                    "overlap ownership must retain the source contour");
            require(std::abs(contourArea(mesh.contourData(1)) / normalization - 9800.0) < 1e-6,
                    "only the assigned overlapping material should leave the recipient");
            require(diagnostics.value(QStringLiteral("accepted")).toBool()
                    && diagnostics.value(QStringLiteral("decisionCounts")).toObject().value(QStringLiteral("assigned_overlap_region")).toInt() > 0,
                    "diagnostics must record overlap inference");
            require(diagnostics.value(QStringLiteral("inputOverlaps")).toArray().size() == 1,
                    "diagnostics must measure the input overlap");
            require(diagnostics.value(QStringLiteral("lostUnionArea")).toDouble(-1) < 1e-6 * normalization,
                    "combined material must be retained when resolving overlap");
        }
    }
}

void mixedBoundaryRegions() {
    for (const bool curved : {false, true}) {
        MeshData mesh;
        auto first = rectangle(0, 0, 100);
        ContourData second;
        second.closed = true;
        second.points = {{{98, 0}, ContourPointKind::Soft, {curved ? 0.0 : 4.0 / 3.0, 100.0 / 3.0}, {102.0 / 3.0, 0}, true},
                         {{200, 0}, ContourPointKind::Hard, {-102.0 / 3.0, 0}, {0, 100.0 / 3.0}, true},
                         {{200, 100}, ContourPointKind::Hard, {0, -100.0 / 3.0}, {-98.0 / 3.0, 0}, true},
                         {{102, 100}, ContourPointKind::Soft, {98.0 / 3.0, 0}, {curved ? 0.0 : -4.0 / 3.0, -100.0 / 3.0}, true}};
        mesh.appendContour(first, QStringLiteral("First"), true, 1.0);
        mesh.appendContour(second, QStringLiteral("Second"), true, 1.0);
        const auto input = mesh;
        QJsonObject diagnostics;
        QString error;
        if (!gui::connectMeshContours(mesh, &error, &diagnostics)) {
            gui::writeMeshReconstructionDiagnostics(mesh, nullptr, diagnostics, {QStringLiteral("mixed_first"), QStringLiteral("mixed_second")});
            throw std::runtime_error(QStringLiteral("mixed region curved=%1: %2").arg(curved).arg(error).toStdString());
        }
        const auto counts = diagnostics.value(QStringLiteral("decisionCounts")).toObject();
        require(counts.value(QStringLiteral("balanced_gap_growth")).toInt() > 0 && counts.value(QStringLiteral("assigned_overlap_region")).toInt() > 0,
                "both material assignments must be inferred along a crossing arc");
        const double firstArea = contourArea(mesh.contourData(0));
        const double secondArea = contourArea(mesh.contourData(1));
        if (!(firstArea > 10000.0 && secondArea < 10000.0 && std::abs(firstArea + secondArea - 20000.0) < 0.1)) {
            gui::writeMeshReconstructionDiagnostics(input, &mesh, diagnostics, {QStringLiteral("mixed_boundary")},
                QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mixed_diagnostics")));
            throw std::runtime_error(QStringLiteral("crossing arc area curved=%1: first=%2 second=%3")
                                         .arg(curved).arg(firstArea, 0, 'g', 12).arg(secondArea, 0, 'g', 12).toStdString());
        }
        require(gui::buildPenContour(mesh.contourData(0).points).valid() && gui::buildPenContour(mesh.contourData(1).points).valid(),
                "mixed reconstruction must retain simple boundaries");
    }
}

void diagnosticReports() {
    MeshData mesh;
    mesh.appendContour(rectangle(0, 0, 100), QStringLiteral("First"), true, 1.0);
    mesh.appendContour(rectangle(1000, 0, 100), QStringLiteral("Second"), true, 1.0);
    const auto input = mesh;
    QJsonObject diagnostics;
    QString error;
    require(!gui::connectMeshContours(mesh, &error, &diagnostics), "disconnected inputs must be rejected with diagnostics");
    require(!diagnostics.value(QStringLiteral("accepted")).toBool(true)
            && !diagnostics.value(QStringLiteral("stage")).toString().isEmpty()
            && !diagnostics.value(QStringLiteral("reason")).toString().isEmpty(),
            "rejections must include their stage and reason");
    QTemporaryDir directory(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("mesh-log-XXXXXX")));
    require(directory.isValid(), "diagnostics test directory must be writable");
    const auto path = gui::writeMeshReconstructionDiagnostics(input, nullptr, diagnostics, {QStringLiteral("first"), QStringLiteral("second")}, directory.path());
    QFile report(path);
    require(report.open(QIODevice::ReadOnly), "latest reconstruction report must be written");
    const auto saved = QJsonDocument::fromJson(report.readAll()).object();
    const auto children = saved.value(QStringLiteral("inputScene")).toObject().value(QStringLiteral("children")).toArray();
    require(children.size() == 1 && children[0].toObject().value(QStringLiteral("vertices")).toArray().size() == input.vertices.size(),
            "the report must include the full input geometry");
    require(saved.value(QStringLiteral("sourceEntries")).toArray().size() == 2 && !saved.value(QStringLiteral("runId")).toString().isEmpty(),
            "the report must identify the run and source entries");
    QFile log(QDir(directory.path()).filePath(QStringLiteral("mesh_reconstruction.log")));
    require(log.open(QIODevice::ReadOnly) && !QJsonDocument::fromJson(log.readLine()).isNull(),
            "the summary log must contain a structured run record");
}

void balancedGrowth() {
    MeshData mesh;
    mesh.appendContour(box(0, 0, 20, 100), QStringLiteral("Small"), true, 1.0);
    mesh.appendContour(box(22, 0, 200, 100), QStringLiteral("Large"), true, 1.0);
    QString error;
    QJsonObject diagnostics;
    const auto input = mesh;
    if (!gui::connectMeshContours(mesh, &error, &diagnostics)) {
        gui::writeMeshReconstructionDiagnostics(input, nullptr, diagnostics, {QStringLiteral("balanced_growth")});
        throw std::runtime_error(QStringLiteral("balanced growth: %1").arg(error).toStdString());
    }
    require(std::abs(contourArea(mesh.contourData(0)) - 2100.0) < 1e-5
            && std::abs(contourArea(mesh.contourData(1)) - 20100.0) < 1e-5,
            "opposing regions must contribute equal displacement regardless of their size");
    for (const auto &first : mesh.contours[0].points) {
        for (const auto &second : mesh.contours[1].points) {
            if (first.vertex == second.vertex) require(std::abs(mesh.vertices[first.vertex].x() - 21.0) < 1e-8,
                "the collision boundary must lie midway between equally advancing fronts");
        }
    }
    MeshData remote;
    remote.appendContour(box(0, 0, 20, 20), QStringLiteral("First"), true, 1.0);
    remote.appendContour(box(22, 0, 20, 20), QStringLiteral("Second"), true, 1.0);
    remote.appendContour(box(200, 0, 20, 20), QStringLiteral("Remote"), true, 1.0);
    const auto before = remote;
    require(!gui::connectMeshContours(remote) && remote == before,
            "nearby neighbors must not be extended into a bridge to a remote component");
    MeshData holes;
    auto frame = rectangle(0, 0, 100);
    frame.cutouts.push_back(rectangle(49, 49, 2).points);
    holes.appendContour(frame, QStringLiteral("Frame"), true, 1.0);
    holes.appendContour(rectangle(110, 0, 100), QStringLiteral("Neighbor"), true, 1.0);
    require(gui::connectMeshContours(holes), "external neighbors must remain connectable during internal collapse");
    require(holes.contours.front().cutouts.isEmpty(), "an internal hole must collapse into its own region before a later external collision");
    MeshData occupied;
    frame.cutouts = {rectangle(20, 20, 60).points};
    occupied.appendContour(frame, QStringLiteral("Frame"), true, 1.0);
    occupied.appendContour(rectangle(30, 30, 20), QStringLiteral("Inside"), true, 1.0);
    occupied.appendContour(rectangle(102, 0, 100), QStringLiteral("Outside"), true, 1.0);
    const auto intact = occupied;
    require(!gui::connectMeshContours(occupied) && occupied == intact,
            "a hole-to-exterior collision cannot establish a region neighbor");
}

void multiRegionPartitions() {
    for (const bool overlap : {false, true}) {
        for (const double scale : {0.001, 1.0, 1000.0}) {
            for (const bool reverseOrder : {false, true}) {
                MeshData mesh;
                QVector<ContourData> inputs;
                const double spacing = overlap ? 98.0 : 102.0;
                for (int row = 0; row < 2; ++row) {
                    for (int column = 0; column < 2; ++column) {
                        inputs.push_back(rectangle(column * spacing * scale, row * spacing * scale, 100.0 * scale));
                    }
                }
                if (reverseOrder) std::reverse(inputs.begin(), inputs.end());
                QPainterPath original;
                for (int index = 0; index < inputs.size(); ++index) {
                    mesh.appendContour(inputs[index], QString::number(index), true, 1.0);
                    original = original.united(filledContour(inputs[index]));
                }
                const auto input = mesh;
                QJsonObject diagnostics;
                QString error;
                if (!gui::connectMeshContours(mesh, &error, &diagnostics)) {
                    gui::writeMeshReconstructionDiagnostics(input, nullptr, diagnostics, {QStringLiteral("multi_region")});
                    throw std::runtime_error(QStringLiteral("multi-region overlap=%1 scale=%2: %3").arg(overlap).arg(scale).arg(error).toStdString());
                }
                require(mesh.valid(), "all regions must participate in one valid Mesh");
                require(diagnostics.value(QStringLiteral("pipeline")).toString() == QStringLiteral("topology_wavefront"),
                        "closed regions must use the global subdivision pipeline");
                QPainterPath rebuilt;
                QVector<QPainterPath> regions;
                const auto polygons = [&](const QPainterPath &path) {
                    Clipper2Lib::PathsD paths;
                    for (const auto &polygon : path.toSubpathPolygons(QTransform::fromScale(1.0 / scale, 1.0 / scale))) {
                        Clipper2Lib::PathD vertices;
                        for (const auto &point : polygon) vertices.emplace_back(point.x(), point.y());
                        paths.push_back(vertices);
                    }
                    return Clipper2Lib::Union(paths, Clipper2Lib::FillRule::EvenOdd, 8);
                };
                for (int index = 0; index < mesh.contours.size(); ++index) {
                    const auto data = mesh.contourData(index);
                    require(gui::buildPenContour(data.points).valid(), "every reconstructed region must retain a simple exterior");
                    const auto material = filledContour(data);
                    for (const auto &other : regions) {
                        require(std::abs(Clipper2Lib::Area(Clipper2Lib::Intersect(polygons(material), polygons(other),
                                                                          Clipper2Lib::FillRule::NonZero, 8))) < 1e-6,
                                "a reconstructed partition cannot contain multiply owned material");
                    }
                    regions.push_back(material);
                    rebuilt = rebuilt.united(material);
                    if (!overlap) {
                        require(std::abs(Clipper2Lib::Area(Clipper2Lib::Difference(polygons(filledContour(inputs[index])), polygons(material),
                                                                           Clipper2Lib::FillRule::NonZero, 8))) < 1e-6,
                                "gap assignment must retain each source region's material");
                    }
                }
                require(std::abs(Clipper2Lib::Area(Clipper2Lib::Difference(polygons(original), polygons(rebuilt),
                                                                   Clipper2Lib::FillRule::NonZero, 8))) < 1e-6,
                        "global reconstruction must retain the combined material");
                if (overlap) {
                    require(diagnostics.value(QStringLiteral("maximumCoverage")).toInt() == 4,
                            "the subdivision must retain the complete coverage set at multi-region intersections");
                } else {
                    for (int index = 0; index < mesh.contours.size(); ++index) {
                        require(contourArea(mesh.contourData(index)) / (scale * scale) > 10000.0,
                                "all neighboring regions must participate in growth at a shared junction");
                    }
                }
            }
        }
    }
}

void disconnectedRegionPieces() {
    MeshData mesh;
    const auto horizontal = box(0, 40, 100, 20);
    const auto vertical = box(40, 0, 20, 100);
    mesh.appendContour(horizontal, QStringLiteral("Horizontal"), true, 1.0);
    mesh.appendContour(vertical, QStringLiteral("Vertical"), true, 0.75);
    QJsonObject diagnostics;
    QString error;
    const auto input = mesh;
    if (!gui::connectMeshContours(mesh, &error, &diagnostics)) {
        gui::writeMeshReconstructionDiagnostics(input, nullptr, diagnostics, {QStringLiteral("crossing_regions")});
        throw std::runtime_error(error.toStdString());
    }
    require(mesh.valid() && mesh.contours.size() == 3, "a region split by ownership must retain all of its connected pieces");
    require(mesh.contours[1].name == QStringLiteral("Vertical") && mesh.contours[2].name == QStringLiteral("Vertical")
            && mesh.contours[1].opacity == 0.75 && mesh.contours[2].opacity == 0.75,
            "each reconstructed piece must inherit its source region attributes");
    require(std::abs(contourArea(mesh.contourData(0)) - 2000.0) < 1e-6
            && std::abs(contourArea(mesh.contourData(1)) + contourArea(mesh.contourData(2)) - 1600.0) < 1e-6,
            "multi-component reconstruction must retain the complete material partition");
}

void curvedRegionJunction() {
    ContourData left;
    left.closed = true;
    left.points = {{{0, 0}, ContourPointKind::Hard, {0, 100.0 / 3.0}, {100.0 / 3.0, 0}, true},
                   {{100, 0}, ContourPointKind::Soft, {-100.0 / 3.0, 0}, {10, 100.0 / 3.0}, true},
                   {{100, 100}, ContourPointKind::Soft, {10, -100.0 / 3.0}, {-100.0 / 3.0, 0}, true},
                   {{0, 100}, ContourPointKind::Hard, {100.0 / 3.0, 0}, {0, -100.0 / 3.0}, true}};
    MeshData mesh;
    mesh.appendContour(left, QStringLiteral("Curved"), true, 1.0);
    mesh.appendContour(box(102, 0, 98, 51), QStringLiteral("Upper"), true, 1.0);
    mesh.appendContour(box(104, 49, 96, 51), QStringLiteral("Lower"), true, 1.0);
    const auto input = mesh;
    QJsonObject diagnostics;
    QString error;
    if (!gui::connectMeshContours(mesh, &error, &diagnostics)) {
        gui::writeMeshReconstructionDiagnostics(input, nullptr, diagnostics, {QStringLiteral("curved_junction")});
        throw std::runtime_error(QStringLiteral("curved junction: %1").arg(error).toStdString());
    }
    require(mesh.valid() && diagnostics.value(QStringLiteral("maximumCoverage")).toInt() == 3,
            "a curved junction must resolve all covering regions in one subdivision");
    require(contourArea(mesh.contourData(0)) >= 10500.0 - 1e-4,
            "multi-region overlap must retain the canonical owner's original material");
    const auto retained = filledContour(mesh.contourData(0));
    for (const double y : {1.0, 10.0, 25.0, 50.0, 75.0, 90.0, 99.0}) {
        const double parameter = y / 100.0;
        require(retained.contains(QPointF(100.0 + 30.0 * parameter * (1.0 - parameter) - 0.01, y)),
                "the shared partition must preserve material along the original curved boundary");
    }
    for (int index = 0; index < mesh.contours.size(); ++index) {
        require(gui::buildPenContour(mesh.contourData(index).points).valid(), "curved junction boundaries must remain geometrically valid");
    }
    MeshData invalid;
    auto frame = rectangle(0, 0, 100);
    frame.cutouts.push_back(rectangle(120, 20, 10).points);
    invalid.appendContour(frame, QStringLiteral("Invalid"), true, 1.0);
    invalid.appendContour(rectangle(102, 0, 100), QStringLiteral("Other"), true, 1.0);
    const auto before = invalid;
    require(!gui::connectMeshContours(invalid) && invalid == before, "invalid source hole containment must be rejected atomically");
}

std::unique_ptr<ContourFigure> figure(const QString &id, const ContourData &data) {
    auto result = std::make_unique<ContourFigure>();
    result->id = id;
    result->name = id;
    result->data = data;
    return result;
}

MeshData adjacentMesh(double scale, const QPointF &origin) {
    MeshData mesh;
    mesh.appendContour(rectangle(origin.x(), origin.y(), 100.0 * scale), QStringLiteral("Left"), true, 1.0);
    mesh.appendContour(rectangle(origin.x() + 102.0 * scale, origin.y(), 100.0 * scale), QStringLiteral("Right"), true, 1.0);
    require(gui::connectMeshContours(mesh), "near contours should connect");
    require(mesh.valid(), "connected mesh should be valid");
    require(mesh.vertices.size() >= 8, "balanced reconstruction must retain the input boundary anchors");
    QSet<int> shared;
    for (const auto &left : mesh.contours[0].points) {
        for (const auto &right : mesh.contours[1].points) if (left.vertex == right.vertex) shared.insert(left.vertex);
    }
    require(shared.size() >= 2, "the complete facing boundary should be shared");
    for (const int vertex : shared) {
        require(std::abs((mesh.vertices[vertex].x() - origin.x()) / scale - 101.0) < 1e-6,
                "the shared boundary must follow the equal-arrival collision line");
    }
    const auto left = mesh.contourData(0);
    require(std::any_of(left.points.cbegin(), left.points.cend(), [&](const auto &point) {
                return point.position == QPointF(origin.x() + 100.0 * scale, origin.y());
            }), "reconstruction must retain original anchors on unchanged exterior edges");
    return mesh;
}

void adaptiveConnections() {
    for (const double scale : {0.0001, 1.0, 10000.0}) adjacentMesh(scale, QPointF(17.0, -23.0));
    MeshData remote;
    remote.appendContour(rectangle(0, 0, 100), QStringLiteral("A"), true, 1.0);
    remote.appendContour(rectangle(1000, 0, 100), QStringLiteral("B"), true, 1.0);
    require(!gui::connectMeshContours(remote), "disconnected contours should be rejected");
}

void middleAnchors() {
    MeshData mesh;
    ContourData horizontal;
    ContourData vertical;
    horizontal.points = {{{0, 0}, ContourPointKind::Hard}, {{200, 0}, ContourPointKind::Hard}};
    vertical.points = {{{100, 0}, ContourPointKind::Hard}, {{100, 102}, ContourPointKind::Hard}};
    vertical.pathKind = ContourPathKind::Lining;
    vertical.points.back().endCap = -1.25;
    mesh.appendContour(horizontal, QStringLiteral("Horizontal"), true, 1.0);
    mesh.appendContour(vertical, QStringLiteral("Vertical"), true, 1.0);
    require(gui::connectMeshContours(mesh), "endpoint to span projection should connect");
    require(mesh.contours[0].points.size() == 3, "projection should split the receiving span");
    require(mesh.contours[0].points[1].vertex == mesh.contours[1].points[0].vertex,
            "split span should share the projected anchor");
    require(mesh.contourData(0).points[1].position == QPointF(100, 0), "exact junction should retain its position");
    require(mesh.contourData(1).points.back().endCap == -1.25, "mesh should preserve signed Lining caps");

    auto edited = mesh.contourData(1);
    edited.points[0].position = QPointF(105, 4);
    require(mesh.replaceContour(1, edited), "shared anchor movement should succeed");
    require(mesh.contourData(0).points[1].position == edited.points[0].position,
            "moving a shared vertex should move every incident anchor");
    const auto before = mesh;
    edited.points.removeFirst();
    require(!mesh.replaceContour(1, edited), "deleting a shared connection should be rejected");
    require(mesh == before, "rejected point edits should be atomic");
}

void intersections() {
    MeshData mesh;
    ContourData horizontal;
    ContourData vertical;
    horizontal.points = {{{-100, 0}, ContourPointKind::Hard}, {{100, 0}, ContourPointKind::Hard}};
    vertical.points = {{{0, -100}, ContourPointKind::Hard}, {{0, 100}, ContourPointKind::Hard}};
    mesh.appendContour(horizontal, QStringLiteral("Horizontal"), true, 1.0);
    mesh.appendContour(vertical, QStringLiteral("Vertical"), true, 1.0);
    require(gui::connectMeshContours(mesh), "crossing spans should connect");
    require(mesh.contours[0].points.size() == 3 && mesh.contours[1].points.size() == 3,
            "crossing should create both middle anchors");
    require(mesh.contours[0].points[1].vertex == mesh.contours[1].points[1].vertex,
            "crossing anchors should share a vertex");
    require(mesh.contourData(0).points.front().position == horizontal.points.front().position,
            "connected contours should retain unrelated anchors");
}

void curvedMiddleConnection() {
    MeshData mesh;
    ContourData arc;
    ContourData line;
    arc.points = {{{-100, 0}, ContourPointKind::Soft, {}, {200.0 / 3.0, 100}, true},
                  {{100, 0}, ContourPointKind::Soft, {-200.0 / 3.0, 100}, {}, true}};
    line.points = {{{-100, 77}, ContourPointKind::Hard}, {{100, 77}, ContourPointKind::Hard}};
    mesh.appendContour(arc, QStringLiteral("Arc"), true, 1.0);
    mesh.appendContour(line, QStringLiteral("Line"), true, 1.0);
    const auto before = mesh;
    require(!gui::connectMeshContours(mesh), "an isolated near contact must not become an arbitrary vertex connection");
    require(mesh == before, "rejected region inference must be atomic");
}

void requireSharedBoundary(const MeshData &mesh, int firstIndex, int secondIndex, bool complete = true) {
    const auto &first = mesh.contours[firstIndex].points;
    const auto &second = mesh.contours[secondIndex].points;
    QSet<int> firstVertices;
    QSet<int> secondVertices;
    for (const auto &point : first) firstVertices.insert(point.vertex);
    for (const auto &point : second) secondVertices.insert(point.vertex);
    if (complete && !(firstVertices - secondVertices).isEmpty()) {
        for (const int vertex : firstVertices - secondVertices) {
            QTextStream(stderr) << "Unshared source anchor " << mesh.vertices[vertex].x() << ", " << mesh.vertices[vertex].y()
                                << "; boundary sizes " << first.size() << "/" << second.size() << '\n';
        }
        throw std::runtime_error("the reconstructed seam must retain every source boundary anchor");
    }
    require((firstVertices & secondVertices).size() >= 2, "a reconstructed arc must share more than an isolated vertex");
    const auto firstSegments = gui::penSegments(mesh.contourData(firstIndex).points, false);
    const auto secondSegments = gui::penSegments(mesh.contourData(secondIndex).points, false);
    int checked = 0;
    for (int index = 0; index < firstSegments.size(); ++index) {
        if (!complete && (!secondVertices.contains(first[index].vertex) || !secondVertices.contains(first[index + 1].vertex))) continue;
        bool found = false;
        for (int other = 0; other < secondSegments.size(); ++other) {
            const bool forward = first[index].vertex == second[other].vertex && first[index + 1].vertex == second[other + 1].vertex;
            const bool reverse = first[index].vertex == second[other + 1].vertex && first[index + 1].vertex == second[other].vertex;
            if (!forward && !reverse) continue;
            for (const double t : {0.0, 0.2, 0.5, 0.8, 1.0}) {
                const double scale = firstSegments[index].controlLength();
                require(QLineF(firstSegments[index].point(t), secondSegments[other].point(reverse ? 1.0 - t : t)).length()
                            <= std::max(1.0, scale) * 1e-8,
                        "shared arc segments must have identical geometry between their anchors");
            }
            found = true;
        }
        require(found, "matching arcs must retain a common segment partition");
        ++checked;
    }
    require(checked > 0, "a reconstructed region must retain a common boundary arc");
}

ContourData sampledArc(gui::PenBoundarySegment curve, const QVector<double> &parameters, bool reversed = false) {
    ContourData data;
    double previous = 0.0;
    for (const double parameter : parameters) {
        auto halves = curve.split((parameter - previous) / (1.0 - previous));
        if (data.points.isEmpty()) data.points.push_back({halves.first.start, ContourPointKind::Soft, {}, {}, true});
        data.points.back().outgoing = halves.first.control - halves.first.start;
        data.points.push_back({halves.first.end, ContourPointKind::Soft, halves.first.control2 - halves.first.end, {}, true});
        curve = halves.second;
        previous = parameter;
    }
    if (reversed) {
        std::reverse(data.points.begin(), data.points.end());
        for (auto &point : data.points) std::swap(point.incoming, point.outgoing);
    }
    return data;
}

void divergingArcs() {
    for (const double scale : {0.001, 1.0, 1000.0}) {
        double sparseCut = 0.0;
        for (const bool dense : {false, true}) {
            gui::PenBoundarySegment curve{{180, 100}, {102, 200.0 / 3.0}, {102, 0}, true, {102, 100.0 / 3.0}};
            const auto boundary = sampledArc(curve, dense ? QVector<double>{0.2, 0.7, 1.0} : QVector<double>{1.0});
            ContourData right;
            right.closed = true;
            auto top = boundary.points.back();
            top.outgoing = {118.0 / 3.0, 0};
            right.points = {top, {{220, 0}, ContourPointKind::Hard, {-118.0 / 3.0, 0}, {0, 100.0 / 3.0}, true},
                            {{220, 100}, ContourPointKind::Hard, {0, -100.0 / 3.0}, {-40.0 / 3.0, 0}, true}};
            for (int index = 0; index + 1 < boundary.points.size(); ++index) right.points.push_back(boundary.points[index]);
            right.points[3].incoming = {40.0 / 3.0, 0};
            auto left = rectangle(0, 0, 100);
            const QPointF origin(17, -23);
            const QTransform transform(scale, 0.0, 0.0, scale, origin.x(), origin.y());
            for (auto *contour : {&left, &right}) for (auto &point : contour->points) gui::transformPenPoint(point, transform);
            MeshData mesh;
            mesh.appendContour(left, QStringLiteral("Left"), true, 1.0);
            mesh.appendContour(right, QStringLiteral("Flared"), true, 1.0);
            const auto input = mesh;
            QJsonObject diagnostics;
            QString error;
            if (!gui::connectMeshContours(mesh, &error, &diagnostics)) {
                gui::writeMeshReconstructionDiagnostics(input, nullptr, diagnostics, {QStringLiteral("diverging_arc")},
                    QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("arc_diagnostics")));
                throw std::runtime_error(QStringLiteral("divergence scale=%1 dense=%2: %3").arg(scale).arg(dense).arg(error).toStdString());
            }
            QSet<int> shared;
            for (const auto &first : mesh.contours[0].points) {
                for (const auto &second : mesh.contours[1].points) if (first.vertex == second.vertex) shared.insert(first.vertex);
            }
            require(shared.size() >= 2, "the short supported corridor must remain connected");
            double last = 0.0;
            for (const int vertex : shared) last = std::max(last, (mesh.vertices[vertex].y() - origin.y()) / scale);
            require(last > 40.0 && last < 90.0, "the shared arc must stop inside the geometric span as separation diverges");
            if (!dense) sparseCut = last;
            else if (std::abs(last - sparseCut) >= 0.1) {
                throw std::runtime_error(QStringLiteral("arc cutoff differs with anchor density at scale %1: sparse=%2 dense=%3")
                                             .arg(scale).arg(sparseCut).arg(last).toStdString());
            }
            require(!filledContour(mesh.contourData(0)).contains(transform.map(QPointF(101, 95)))
                    && !filledContour(mesh.contourData(1)).contains(transform.map(QPointF(140, 95))),
                    "both unconnected extensions must return to the original source boundaries");
            require(diagnostics.value(QStringLiteral("interiorSourceArcCuts")).toInt() > 0,
                    "cutoff diagnostics must record source parameters inside existing spans");
        }
    }
    MeshData restarted;
    restarted.appendContour(rectangle(0, 0, 100), QStringLiteral("Left"), true, 1.0);
    ContourData right;
    right.closed = true;
    for (const QPointF point : {QPointF(102, 0), QPointF(220, 0), QPointF(220, 100), QPointF(140, 100), QPointF(140, 60), QPointF(102, 40)}) {
        right.points.push_back({point, ContourPointKind::Hard});
    }
    restarted.appendContour(right, QStringLiteral("Plateau"), true, 1.0);
    require(gui::connectMeshContours(restarted), "the local shortest corridor must remain usable after a flare");
    require(!filledContour(restarted.contourData(0)).contains(QPointF(110, 80))
            && !filledContour(restarted.contourData(1)).contains(QPointF(130, 80)),
            "a remote parallel tail cannot restart the connection after a divergent interval");
}

void matchingArcs() {
    for (const double scale : {0.001, 1.0, 1000.0}) {
        for (const bool reversed : {false, true}) {
            MeshData mesh;
            gui::PenBoundarySegment first{{0, 0}, {33, 35}, {100, 0}, true, {67, 35}};
            gui::PenBoundarySegment second{{0, 2}, {33, 39}, {100, 6}, true, {67, 40}};
            const auto map = [&](gui::PenBoundarySegment &curve) {
                for (auto *point : {&curve.start, &curve.control, &curve.control2, &curve.end}) *point = *point * scale + QPointF(17, -23);
            };
            map(first);
            map(second);
            mesh.appendContour(sampledArc(first, {0.5, 1.0}), QStringLiteral("First arc"), true, 1.0);
            mesh.appendContour(sampledArc(second, {0.2, 0.7, 1.0}, reversed), QStringLiteral("Second arc"), true, 1.0);
            require(gui::connectMeshContours(mesh), "similar arcs should connect along their overlapping boundaries");
            requireSharedBoundary(mesh, 0, 1);
            const auto before = mesh;
            require(gui::connectMeshContours(mesh), "existing arc topology should remain connectable");
            require(mesh.vertices.size() == before.vertices.size(), "reconnecting matched arcs should retain the anchor count");
            requireSharedBoundary(mesh, 0, 1);
        }
    }
}

void connectedArcCompletion() {
    MeshData mesh;
    ContourData first;
    ContourData second;
    first.points = {{{0, 0}, ContourPointKind::Hard}, {{0, 25}, ContourPointKind::Hard}, {{0, 50}, ContourPointKind::Hard},
                    {{0, 75}, ContourPointKind::Hard}, {{0, 100}, ContourPointKind::Hard}};
    second.points = {{{2, 0}, ContourPointKind::Hard}, {{1, 25}, ContourPointKind::Hard}, {{0, 50}, ContourPointKind::Hard},
                     {{3, 75}, ContourPointKind::Hard}, {{4, 100}, ContourPointKind::Hard}};
    mesh.appendContour(first, QStringLiteral("First"), true, 1.0);
    mesh.appendContour(second, QStringLiteral("Second"), true, 1.0);
    require(gui::connectMeshContours(mesh), "touching contours should complete their matching boundary");
    requireSharedBoundary(mesh, 0, 1);
}

void partialAndDenseBoundaries() {
    MeshData partial;
    ContourData first;
    ContourData second;
    first.points = {{{0, 0}, ContourPointKind::Hard}, {{100, 0}, ContourPointKind::Hard}};
    second.points = {{{50, 2}, ContourPointKind::Hard}, {{150, 2}, ContourPointKind::Hard}};
    partial.appendContour(first, QStringLiteral("First"), true, 1.0);
    partial.appendContour(second, QStringLiteral("Second"), true, 1.0);
    require(gui::connectMeshContours(partial), "partially overlapping boundaries should share their overlap");
    require(partial.contourData(0).points.front().position == QPointF(0, 0)
            && partial.contourData(1).points.front().position == QPointF(50, 2)
            && partial.contourData(1).points.back().position == QPointF(150, 2),
            "reconstruction must retain the open boundary endpoints");
    QSet<int> shared;
    for (const auto &a : partial.contours[0].points) {
        for (const auto &b : partial.contours[1].points) if (a.vertex == b.vertex) shared.insert(a.vertex);
    }
    require(shared.size() == 2, "the complete overlapping boundary region should be reconstructed");

    MeshData dense;
    first.points.clear();
    for (int x = 0; x <= 100; x += 5) first.points.push_back({QPointF(x, 0), ContourPointKind::Hard});
    second.points = {{{0, 6}, ContourPointKind::Hard}, {{100, 6}, ContourPointKind::Hard}};
    dense.appendContour(first, QStringLiteral("Dense"), true, 1.0);
    dense.appendContour(second, QStringLiteral("Sparse"), true, 1.0);
    const auto input = dense;
    QJsonObject diagnostics;
    require(gui::connectMeshContours(dense, nullptr, &diagnostics), "smooth boundary support should be independent of anchor spacing");
    try {
        requireSharedBoundary(dense, 0, 1);
    } catch (...) {
        gui::writeMeshReconstructionDiagnostics(input, &dense, diagnostics, {QStringLiteral("dense"), QStringLiteral("sparse")});
        throw;
    }
}

void appendMatchingArc() {
    Mesh existing;
    gui::PenBoundarySegment curve{{0, 0}, {33, 35}, {100, 0}, true, {67, 35}};
    existing.data.appendContour(sampledArc(curve, {0.5, 1.0}), QStringLiteral("First"), true, 1.0);
    for (auto *point : {&curve.start, &curve.control, &curve.control2, &curve.end}) *point += QPointF(0, 2);
    existing.data.appendContour(sampledArc(curve, {0.2, 0.7, 1.0}), QStringLiteral("Second"), true, 1.0);
    require(gui::connectMeshContours(existing.data), "matching arcs should form the initial mesh");
    MeshData extended;
    gui::appendMeshGeometry(extended, existing, QTransform());
    for (auto *point : {&curve.start, &curve.control, &curve.control2, &curve.end}) *point += QPointF(0, 2);
    extended.appendContour(sampledArc(curve, {0.3, 0.8, 1.0}, true), QStringLiteral("Third"), true, 1.0);
    require(gui::connectMeshContours(extended), "a matching arc should extend an existing mesh");
    requireSharedBoundary(extended, 0, 1);
    requireSharedBoundary(extended, 0, 2, false);
    for (const auto &point : existing.data.contourData(0).points) {
        const auto rebuilt = extended.contourData(0).points;
        require(std::any_of(rebuilt.cbegin(), rebuilt.cend(), [&](const ContourPoint &other) {
            return QLineF(point.position, other.position).length() < 1e-8;
        }), "appending must retain the existing source anchors");
    }
}

void stateAndPersistence() {
    fls::Project project;
    project.root = std::make_unique<Group>();
    project.root->id = QStringLiteral("root");
    project.root->append(figure(QStringLiteral("left"), rectangle(0, 0, 100)));
    project.root->append(figure(QStringLiteral("right"), rectangle(102, 0, 100)));
    project.root->append(figure(QStringLiteral("next"), rectangle(204, 0, 100)));
    auto shape = std::make_unique<Shape>();
    shape->id = QStringLiteral("shape");
    project.root->append(std::move(shape));
    gui::EditorState state;
    state.setProject(std::move(project));
    gui::EditorCommand command;
    command.type = gui::EditorCommandType::Group;
    command.entries = {QStringLiteral("left"), QStringLiteral("right")};
    require(state.executeCommand(command), "contours should group into a mesh");
    const auto id = *state.selectedLayerIds().cbegin();
    const auto *mesh = dynamic_cast<const Mesh *>(state.sceneNode(id));
    require(mesh != nullptr, "grouping contours should create a distinct mesh");
    require(state.leafLayerIdsForEntry(id) == QVector<QString>{id}, "mesh should be one selectable leaf");
    state.undo();
    require(state.sceneNode(QStringLiteral("left")) != nullptr, "undo should restore the contours");
    state.redo();
    require(state.sceneNode(id) != nullptr, "redo should restore the mesh");

    command.entries = {id, QStringLiteral("next")};
    require(state.executeCommand(command), "grouping should append contours to an existing mesh");
    mesh = dynamic_cast<const Mesh *>(state.sceneNode(id));
    require(mesh != nullptr && mesh->data.contours.size() == 3, "appending should retain mesh identity");
    const auto document = fls::projectToJson(*state.project());
    const auto restored = fls::projectFromJson(document);
    const auto *restoredMesh = dynamic_cast<const Mesh *>(restored.root->children.front().get());
    require(restoredMesh != nullptr && restoredMesh->data == mesh->data, "project persistence should retain shared topology");
    auto corrupted = document;
    auto root = corrupted.value(QStringLiteral("root")).toObject();
    auto children = root.value(QStringLiteral("children")).toArray();
    auto meshObject = children[0].toObject();
    auto meshContours = meshObject.value(QStringLiteral("contours")).toArray();
    auto contourObject = meshContours[0].toObject();
    contourObject.insert(QStringLiteral("vertices"), QJsonArray{-1, 0, 0, 0});
    meshContours[0] = contourObject;
    meshObject.insert(QStringLiteral("contours"), meshContours);
    children[0] = meshObject;
    root.insert(QStringLiteral("children"), children);
    corrupted.insert(QStringLiteral("root"), root);
    bool rejected = false;
    try { fls::projectFromJson(corrupted); } catch (const std::exception &) { rejected = true; }
    require(rejected, "invalid shared vertex references should be rejected on load");
    const auto before = sceneTreeToJson(*state.sceneRoot());
    command.entries = {id, QStringLiteral("shape")};
    require(!state.executeCommand(command), "mesh mixed with other objects should be rejected");
    require(sceneTreeToJson(*state.sceneRoot()) == before, "rejected grouping should retain the scene");
    command.type = gui::EditorCommandType::Ungroup;
    command.entries = {id};
    require(state.executeCommand(command), "mesh should dissolve into standalone contours");
    require(state.project()->root->children.size() == 4, "dissolving should restore each contour");
}

} // namespace

void loggedGrowthFixture(bool divergence = false) {
    QFile fixture(QStringLiteral(FLS_CONTOUR_FIXTURE_DIR) + (divergence ? QStringLiteral("/contour_mesh_diverging_input.json")
                                                                     : QStringLiteral("/contour_mesh_growth_input.json")));
    require(fixture.open(QIODevice::ReadOnly), "the logged source fixture must be readable");
    const auto record = QJsonDocument::fromJson(fixture.readAll()).object();
    auto scene = record.value(QStringLiteral("inputScene")).toObject();
    const auto encoded = scene.value(QStringLiteral("children")).toArray()[0].toObject();
    auto contours = encoded.value(QStringLiteral("contours")).toArray();
    for (int index = 0; index < contours.size(); ++index) {
        auto contour = contours[index].toObject();
        contour.insert(QStringLiteral("id"), QStringLiteral("fixture_%1").arg(index));
        contours[index] = contour;
    }
    scene.insert(QStringLiteral("children"), contours);
    const auto root = fls::scene::sceneTreeFromJson(scene);
    MeshData mesh;
    for (const auto &node : root->children) {
        const auto *contour = dynamic_cast<const ContourFigure *>(node.get());
        require(contour != nullptr, "fixture sources must decode as saved contours");
        mesh.appendContour(contour->data, contour->name, contour->visible, contour->opacity);
    }
    const auto input = mesh;
    QJsonObject diagnostics;
    QString error;
    const bool connected = gui::connectMeshContours(mesh, &error, &diagnostics);
    gui::writeMeshReconstructionDiagnostics(input, connected ? &mesh : nullptr, diagnostics, {QStringLiteral("logged_growth_fixture")},
        QDir(QCoreApplication::applicationDirPath()).filePath(divergence ? QStringLiteral("divergence_diagnostics") : QStringLiteral("wavefront_diagnostics")));
    QTextStream(stdout) << "Logged growth fixture: " << connected << ", stage=" << diagnostics.value(QStringLiteral("stage")).toString()
                        << ", vertices=" << mesh.vertices.size() << ", elapsed=" << diagnostics.value(QStringLiteral("elapsedMs")).toInt() << '\n';
    if (!connected) throw std::runtime_error(error.toStdString());
    require(mesh.valid(), "the logged regions must form a connected, valid Mesh");
    const auto growth = diagnostics.value(QStringLiteral("regionGrowth")).toArray();
    require(growth.size() == input.contours.size(), "growth diagnostics must account for every source region");
    double total = 0.0;
    for (const auto &entry : growth) total += entry.toObject().value(QStringLiteral("growthArea")).toDouble();
    if (divergence) {
        require(diagnostics.value(QStringLiteral("discardedCorridorLength")).toDouble() > 0.0
                && diagnostics.value(QStringLiteral("interiorSourceArcCuts")).toInt() > 0,
                "the logged divergent corridors must be trimmed with geometric source cuts");
        return;
    }
    const auto red = growth[23].toObject();
    require(red.value(QStringLiteral("growthArea")).toDouble() < red.value(QStringLiteral("beforeArea")).toDouble() * 0.2
            && red.value(QStringLiteral("growthArea")).toDouble() < total * 0.5,
            "the red region cannot take ownership of the shared gap network");
    for (const auto &entry : diagnostics.value(QStringLiteral("decisions")).toArray()) {
        const auto decision = entry.toObject();
        require(!(decision.value(QStringLiteral("decision")).toString() == QStringLiteral("external_neighbors")
                  && decision.value(QStringLiteral("firstContour")).toInt(-1) == 4
                  && decision.value(QStringLiteral("secondContour")).toInt(-1) == 27),
                "the logged exterior bridge must be superseded by intervening neighbors");
    }
    const auto labels = diagnostics.value(QStringLiteral("outputSourceLabels")).toArray();
    require(labels.size() == mesh.contours.size(), "every reconstructed piece must identify its source region");
    for (int index = 0; index < mesh.contours.size(); ++index) {
        require(mesh.contours[index].style.fillColor == input.contours[labels[index].toInt()].style.fillColor,
                "reconstructed pieces must retain their source color");
    }
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        if (application.arguments().contains(QStringLiteral("--logged-growth"))) {
            loggedGrowthFixture();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--logged-divergence"))) {
            loggedGrowthFixture(true);
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--diverging-arcs"))) {
            divergingArcs();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--mixed-boundaries"))) {
            mixedBoundaryRegions();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--region-topology"))) {
            regionTopology();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--balanced-growth"))) {
            balancedGrowth();
            return 0;
        }
        balancedGrowth();
        divergingArcs();
        overlappingRegions();
        mixedBoundaryRegions();
        diagnosticReports();
        multiRegionPartitions();
        disconnectedRegionPieces();
        curvedRegionJunction();
        regionTopology();
        adaptiveConnections();
        middleAnchors();
        intersections();
        curvedMiddleConnection();
        matchingArcs();
        connectedArcCompletion();
        partialAndDenseBoundaries();
        appendMatchingArc();
        stateAndPersistence();
        QTextStream(stdout) << "Contour mesh tests passed\n";
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    return 0;
}
