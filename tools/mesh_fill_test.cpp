#include "mesh_fill.h"
#include "mesh_edit.h"
#include "polygon_mesh.h"
#include "compact_fit_catalog_internal.h"
#include "main_window.h"
#include "editor_state.h"
#include "project_canvas.h"
#include "scene_codec.h"
#include "profile_fit.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QProgressBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>
#include <stdexcept>

namespace {
using namespace fls::scene;
using namespace gui;

void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

MeshData fixture(bool hole = false) {
    MeshData mesh;
    mesh.vertices = {{0, 0}, {100, 0}, {100, 100}, {0, 100}, {200, 0}, {200, 100}};
    MeshContour left, right;
    left.name = "Left";
    right.name = "Right";
    left.style.closed = right.style.closed = true;
    left.style.fillColor = Qt::red;
    right.style.fillColor = Qt::blue;
    for (const int vertex : {0, 1, 2, 3}) left.points.push_back({{}, {}, ContourPointKind::Hard, vertex});
    for (const int vertex : {1, 4, 5, 2}) right.points.push_back({{}, {}, ContourPointKind::Hard, vertex});
    if (hole) {
        mesh.vertices += QVector<QPointF>{{20, 20}, {40, 20}, {40, 40}, {20, 40}};
        QVector<MeshPoint> cutout;
        for (const int vertex : {6, 7, 8, 9}) cutout.push_back({{}, {}, ContourPointKind::Hard, vertex});
        left.cutouts.push_back(cutout);
    }
    mesh.contours = {left, right};
    require(mesh.valid(), "invalid test mesh");
    return mesh;
}

QVector<catalog::Primitive> primitives() {
    QVector<catalog::Primitive> result;
    for (const int id : {101, 103}) {
        catalog::Primitive primitive;
        primitive.taskOrder.fill(-1);
        primitive.taskOrder[static_cast<int>(catalog::ShapeTask::Interior)] = 0;
        primitive.taskOrder[static_cast<int>(catalog::ShapeTask::StraightEdges)] = 0;
        primitive.taskOrder[static_cast<int>(catalog::ShapeTask::CornerTriangles)] = 0;
        primitive.taskOrder[static_cast<int>(catalog::ShapeTask::WholeRegion)] = 0;
        primitive.taskOrder[static_cast<int>(catalog::ShapeTask::Replacements)] = 0;
        primitive.shape.shapeId = id;
        QPolygonF polygon;
        if (id == 101) polygon = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        else polygon = {{0, 0}, {1, 0}, {0, 1}};
        primitive.shape.contours = {polygon};
        primitive.shape.silhouette.addPolygon(polygon);
        primitive.shape.silhouette.closeSubpath();
        primitive.shape.bounds = polygon.boundingRect();
        primitive.shape.area = catalog::area({polygon});
        result.push_back(primitive);
    }
    return result;
}

catalog::Polygons support(const MeshFillLayer &layer, const QVector<catalog::Primitive> &shapes) {
    catalog::Polygons polygons;
    for (const auto &placement : layer.placements) {
        const auto found = std::find_if(shapes.cbegin(), shapes.cend(), [&](const auto &primitive) {
            return primitive.shape.shapeId == placement.shapeId;
        });
        require(found != shapes.cend(), "unknown emitted shape");
        polygons += catalog::mapped(found->shape, placement.transform);
    }
    return catalog::unite(polygons);
}

void deterministicFill() {
    MeshFillRequest request{fixture(), primitives()};
    const auto first = fillMesh(request);
    require(first.error.isEmpty(), first.error);
    require(first.completedFaces == 2 && first.shapeCount == 2 && !first.stopped, "rectangular faces should each need one shape");
    require(first.layers[0].face == 0 && first.layers[1].face == 1, "equal costs must have stable source ordering");
    require(first.layers[0].color == Qt::red && first.layers[1].color == Qt::blue, "face colors must be preserved");
    const auto foreground = support(first.layers[0], request.primitives);
    const auto background = support(first.layers[1], request.primitives);
    require(catalog::area(catalog::intersect(foreground, background)) > 9999.0, "background shape should use certified neighboring foreground support");
    const auto again = fillMesh(request);
    require(again.error.isEmpty() && again.shapeCount == first.shapeCount, "repeat fill failed");
    for (int face = 0; face < first.layers.size(); ++face) {
        require(first.layers[face].face == again.layers[face].face, "face order changed between runs");
        for (int shape = 0; shape < first.layers[face].placements.size(); ++shape) {
            require(first.layers[face].placements[shape].transform == again.layers[face].placements[shape].transform,
                    "emitted placement changed between runs");
        }
    }
    bool stop = false;
    const auto partial = fillMesh(request, [&] { return stop; }, [&](const MeshFillProgress &progress) {
        if (progress.shapeCount == 1) stop = true;
    });
    require(partial.stopped && partial.error.isEmpty() && partial.shapeCount == 1 && partial.completedFaces == 1,
            "stopping must retain and count the generated foreground checkpoint");
    require(partial.layers.size() == 1 && partial.layers.front().placements.size() == 1, "stop discarded generated placements");

    auto touching = request;
    touching.mesh.vertices += QVector<QPointF>{{200, 100}, {200, 200}, {100, 200}};
    touching.mesh.contours[1].points.clear();
    for (const int vertex : {2, 6, 7, 8}) touching.mesh.contours[1].points.push_back({{}, {}, ContourPointKind::Hard, vertex});
    const auto pointContact = fillMesh(touching);
    require(pointContact.error.isEmpty(), pointContact.error);
    require(pointContact.diagnostics["faces"].toArray()[1].toObject()["leewayArea"].toDouble() == 0.0,
            "point contact must not authorize shared-edge leeway");

    request.mesh = fixture(true);
    request.mesh.contours[1].visible = false;
    stop = false;
    const auto unfinished = fillMesh(request, [&] { return stop; }, [&](const MeshFillProgress &progress) {
        if (progress.shapeCount == 1) stop = true;
    });
    require(unfinished.stopped && unfinished.error.isEmpty() && unfinished.shapeCount >= 1 && unfinished.completedFaces == 0,
            QStringLiteral("stopping during repair must retain an unfinished face: %1").arg(QString::fromUtf8(QJsonDocument(unfinished.diagnostics).toJson(QJsonDocument::Compact))));
}

void boundariesAndHoles() {
    for (const bool curved : {false, true}) {
        MeshFillRequest request{fixture(true), primitives()};
        if (curved) require(setMeshEdgeHandle(request.mesh, 1, 2, 1, {130, 30}), "shared seam handle edit failed");
        const auto result = fillMesh(request);
        require(result.completedFaces == 2, "boundary fill left incomplete faces");
        const catalog::Polygons hole{QPolygonF{{20, 20}, {40, 20}, {40, 40}, {20, 40}}};
        catalog::Polygons coverage;
        for (const auto &layer : result.layers) {
            const auto painted = support(layer, request.primitives);
            require(catalog::area(catalog::intersect(painted, hole)) == 0.0, "native shapes painted an empty mesh hole");
            coverage += painted;
        }
        const catalog::Polygons envelope{QPolygonF{{0, 0}, {200, 0}, {200, 100}, {0, 100}}};
        const auto expected = catalog::subtract(envelope, hole);
        require(catalog::area(catalog::subtract(expected, catalog::expanded(catalog::unite(coverage), compact::kDefaultInwardAllowance))) == 0.0,
                "native shapes left a material gap");
        require(catalog::area(catalog::subtract(catalog::unite(coverage), catalog::expanded(expected, compact::kDefaultBoundaryAllowance))) == 0.0,
                "native shapes escaped the mesh boundary");
    }
}

void partialPolygonRepair() {
    PolygonMeshRequest request;
    request.points = {{0, 0}, {50, 0}, {70, 40}, {30, 80}, {0, 40}};
    request.sources.square = primitives()[0].shape.contours.front();
    request.sources.triangle = primitives()[1].shape.contours.front();
    request.mergeSquares = false;
    request.retainOnCancel = true;
    bool stop = false;
    request.placementAccepted = [&](const PolygonMeshPlacement &) { stop = true; };
    const auto partial = meshPolygon(request, [&] { return stop; });
    require(partial.cancelled && partial.placements.size() == 1, "polygon repair must retain its emitted checkpoint on stop");
}

void windowFill() {
    MainWindow window;
    auto *state = window.findChild<EditorState *>();
    auto *canvas = dynamic_cast<ProjectCanvas *>(window.centralWidget());
    auto *progress = window.findChild<QProgressBar *>("GeneratedFillProgress");
    require(state && canvas && progress, "missing mesh fill GUI components");
    fls::Project project;
    ensureProjectSceneRoot(project);
    auto parent = std::make_unique<Group>();
    parent->id = "parent";
    parent->transform = {100, -50, -1.2, 0.7, 18, 0.1};
    auto mesh = std::make_unique<Mesh>();
    mesh->id = "mesh";
    mesh->data = fixture();
    mesh->transform = {-20, 40, 0.8, 1.3, -22, -0.15};
    const auto data = mesh->data;
    const auto frame = mesh->transform;
    parent->append(std::move(mesh));
    project.root->append(std::move(parent));
    state->setProject(std::move(project));
    state->noteProjectStructureChanged();
    EditorCommand select;
    select.type = EditorCommandType::Selection;
    select.entries = {"mesh"};
    require(state->executeCommand(select), "mesh selection failed");
    QApplication::setActiveWindow(&window);
    canvas->setFocus();
    bool escape = false;
    bool displayedCounts = false;
    QObject::connect(progress, &QProgressBar::valueChanged, &window, [&](int value) {
        displayedCounts |= progress->format().contains("faces") && progress->format().contains("shapes");
        if (value <= 0 || value >= progress->maximum() || escape) return;
        escape = true;
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &cancel);
    });
    require(canvas->handleKeyBinding(KeyInteraction::CanvasCommitInteraction, KeyEventPhase::Press, false), "Enter did not start mesh filling");
    require(!progress->isHidden(), "mesh progress was not displayed");
    QElapsedTimer timer;
    timer.start();
    while (!progress->isHidden() && timer.elapsed() < 30000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    require(progress->isHidden() && escape && displayedCounts, "mesh fill controls or progress failed");
    const auto *source = dynamic_cast<const Mesh *>(state->sceneNode("mesh"));
    require(source && source->data == data, "mesh filling changed the editable source");
    const auto *owner = dynamic_cast<const Group *>(source->parent());
    require(owner && owner->children.size() == 2, "Escape failed to insert retained shapes alongside the mesh");
    const auto *output = dynamic_cast<const Group *>(owner->children.back().get());
    require(output && output->name == "Mesh Fill" && output->x == frame.x && output->y == frame.y
            && output->scaleX == frame.scaleX && output->scaleY == frame.scaleY
            && output->rotation == frame.rotation && output->skew == frame.skew && !output->children.empty(),
            "fill output lost the source frame or generated shapes");
    const auto *front = dynamic_cast<const Group *>(output->children.back().get());
    require(front && front->name == "Left", "fill groups must be inserted back to front");
    const auto *shape = dynamic_cast<const Shape *>(front->children.front().get());
    require(shape && shape->color[2] == 255 && shape->color[0] == 0, "foreground native color changed");
    EditorCommand undo;
    undo.type = EditorCommandType::Undo;
    require(state->executeCommand(undo), "mesh fill undo failed");
    source = dynamic_cast<const Mesh *>(state->sceneNode("mesh"));
    require(source && dynamic_cast<const Group *>(source->parent())->children.size() == 1, "mesh fill must be one undo operation");
    QThreadPool::globalInstance()->waitForDone();

    QString catalogError;
    MeshFillRequest curved{fixture(), canvas->compactFillPrimitives(&catalogError)};
    require(!curved.primitives.isEmpty(), catalogError);
    curved.profileTrialBudget = 64;
    require(setMeshEdgeHandle(curved.mesh, 1, 2, 1, {130, 30}), "native curved fixture failed");
    const auto native = fillMesh(curved);
    if (!native.error.isEmpty()) {
        QFile report(QCoreApplication::applicationDirPath() + "/mesh_fill_test_failure.json");
        if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(native.diagnostics).toJson());
    }
    require(native.error.isEmpty() && native.completedFaces == 2,
            QStringLiteral("native profile mesh fill: %1").arg(native.error));
    require(native.shapeCount < 80, "curved mesh filling regressed to a polygon cover");
}

void loggedFill(bool full) {
    QFile fixtureFile(QStringLiteral(FLS_MESH_FIXTURE_DIR "/mesh_fill_logged.json"));
    require(fixtureFile.open(QIODevice::ReadOnly), "missing logged mesh fill fixture");
    const auto scene = sceneTreeFromJson(QJsonDocument::fromJson(fixtureFile.readAll()).object());
    const auto *mesh = dynamic_cast<const Mesh *>(scene->children.front().get());
    require(mesh && mesh->data.valid(), "invalid logged mesh");
    ShapeGeometryStore geometry;
    QString error;
    require(geometry.loadDefault(&error), error);
    MeshFillRequest request{mesh->data, catalog::buildCatalog(geometry, &error)};
    require(!request.primitives.isEmpty(), error);
    if (!full) for (int face = 0; face < request.mesh.contours.size(); ++face) request.mesh.contours[face].visible = face == 21;
    int completed = 0;
    const auto result = fillMesh(request, {}, [&](const MeshFillProgress &progress) {
        if (progress.completedFaces <= completed) return;
        completed = progress.completedFaces;
        QTextStream(stdout) << "Processed " << completed << '/' << progress.totalFaces << " faces, " << progress.shapeCount << " shapes\n";
    });
    QFile report(QCoreApplication::applicationDirPath() + "/mesh_fill_replay.json");
    if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(result.diagnostics).toJson());
    QTextStream(stdout) << "Logged mesh: " << result.completedFaces << '/' << result.totalFaces << " faces, "
        << result.shapeCount << " shapes, " << result.error << '\n';
    require(result.completedFaces == (full ? request.mesh.contours.size() : 1), "logged mesh fill stopped before processing all faces");
    if (!full) {
        PenFillRequest contour;
        const auto data = request.mesh.contourData(21);
        contour.loops.push_back({data.points, PenLoopKind::Outer});
        for (const auto &hole : data.cutouts) contour.loops.push_back({hole, PenLoopKind::Cutout});
        compact::FillOptions options;
        options.useGpu = false;
        options.timeBudgetMilliseconds = compact::kNoTimeBudget;
        options.retainFailedFill = true;
        const auto regular = profile::fillRegion(contour, request.primitives, options);
        QTextStream(stdout) << "Regular Compact Fill: " << regular.fill.placements.size() << " shapes, " << regular.fill.error << '\n';
        require(result.shapeCount <= regular.fill.placements.size() + 2 && result.shapeCount < 80,
                "mesh face shape count regressed against regular Compact Fill");
        bool stop = false;
        const auto partial = fillMesh(request, [&] { return stop; }, [&](const MeshFillProgress &progress) {
            if (progress.shapeCount >= 2) stop = true;
        });
        require(partial.stopped && partial.shapeCount >= 2 && partial.completedFaces == 0,
                "stopping native profile fitting must retain its current placements");
    } else require(result.shapeCount < 1500, "logged mesh shape count remains excessive");
}
} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("ForzaLiveryStudioTests");
    QCoreApplication::setApplicationName("MeshFilling");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        if (application.arguments().contains("--logged") || application.arguments().contains("--logged-full")) {
            loggedFill(application.arguments().contains("--logged-full"));
            return 0;
        }
        deterministicFill();
        boundariesAndHoles();
        partialPolygonRepair();
        windowFill();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "Mesh filling, topology support, holes, determinism, progress and stop retention passed\n";
    return 0;
}
