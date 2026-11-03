#include "mesh_edit.h"
#include "project_canvas.h"
#include "editor_state.h"
#include "project_codec.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <stdexcept>

namespace {
using namespace fls::scene;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(const QPointF &first, const QPointF &last) {
    return QLineF(first, last).length() < 1e-7;
}

MeshData meshFixture() {
    MeshData mesh;
    mesh.vertices = {{0, 0}, {100, 0}, {100, 100}, {0, 100}, {200, 0}, {200, 100},
                     {20, 20}, {40, 20}, {40, 40}, {20, 40}};
    MeshContour left;
    left.name = "Left";
    left.style.closed = true;
    for (const int vertex : {0, 1, 2, 3}) left.points.push_back({{}, {}, ContourPointKind::Hard, vertex});
    QVector<MeshPoint> hole;
    for (const int vertex : {6, 7, 8, 9}) hole.push_back({{}, {}, ContourPointKind::Hard, vertex});
    left.cutouts.push_back(hole);
    MeshContour right;
    right.name = "Right";
    right.style.closed = true;
    for (const int vertex : {1, 4, 5, 2}) right.points.push_back({{}, {}, ContourPointKind::Hard, vertex});
    right.points[0] = {{0, 100.0 / 3.0}, {100.0 / 3.0, 0}, ContourPointKind::Soft, 1, true};
    right.points[3] = {{100.0 / 3.0, 0}, {0, -100.0 / 3.0}, ContourPointKind::Soft, 2, true};
    mesh.contours = {left, right};
    return mesh;
}

void requireSharedCurve(const MeshData &mesh, int first, int last) {
    QVector<gui::PenBoundarySegment> curves;
    for (int contour = 0; contour < mesh.contours.size(); ++contour) {
        const auto segments = gui::penSegments(mesh.contourData(contour).points);
        const auto &points = mesh.contours[contour].points;
        for (int index = 0; index < points.size(); ++index) {
            if (points[index].vertex == first && points[(index + 1) % points.size()].vertex == last) curves.push_back(segments[index]);
            else if (points[index].vertex == last && points[(index + 1) % points.size()].vertex == first) {
                auto curve = segments[index];
                std::swap(curve.start, curve.end);
                std::swap(curve.control, curve.control2);
                curves.push_back(curve);
            }
        }
    }
    require(curves.size() == 2, "a shared edge must remain referenced by both faces");
    for (const double parameter : {0.0, 0.2, 0.5, 0.8, 1.0})
        require(close(curves[0].point(parameter), curves[1].point(parameter)), "incident faces must retain the same shared curve");
}

void edgeEditing() {
    auto mesh = meshFixture();
    require(mesh.valid() && gui::meshEditEdges(mesh).size() == 11, "edge topology must deduplicate the shared seam and include holes");
    require(gui::setMeshEdgeHandle(mesh, 1, 2, 1, {130, 30}), "a shared handle must be editable");
    requireSharedCurve(mesh, 1, 2);
    const auto beforeMove = mesh;
    require(gui::translateMeshVertices(mesh, {1}, {3, 5}), "curved shared vertices must remain movable");
    requireSharedCurve(mesh, 1, 2);
    const auto movedCurve = gui::penSegments(mesh.contourData(0).points)[1];
    const auto originalCurve = gui::penSegments(beforeMove.contourData(0).points)[1];
    require(close(movedCurve.start, originalCurve.start + QPointF(3, 5))
            && close(movedCurve.control, originalCurve.control + QPointF(3, 5))
            && close(movedCurve.end, originalCurve.end) && close(movedCurve.control2, originalCurve.control2),
            "moving a shared anchor must translate its own cubic control and retain the opposite endpoint");
    mesh = beforeMove;
    const auto edges = gui::meshEditEdges(mesh);
    const auto seam = *std::find_if(edges.cbegin(), edges.cend(), [](const auto &edge) { return edge.first == 1 && edge.last == 2; });
    require(seam.curve.point(0.5).x() > 100.0, "editing a hard anchor's handle must curve its edge");
    const int middle = gui::splitMeshEdge(mesh, seam, 0.37);
    require(middle == 10 && mesh.vertices.size() == 11 && mesh.contours[0].points.size() == 5 && mesh.contours[1].points.size() == 5,
            "insertion must create one canonical point on both faces");
    requireSharedCurve(mesh, 1, middle);
    requireSharedCurve(mesh, middle, 2);
    for (const auto &edge : gui::meshEditEdges(mesh)) {
        if ((edge.first == 1 && edge.last == middle) || (edge.first == 2 && edge.last == middle)) {
            for (const double parameter : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                const auto expected = edge.first == 1 ? seam.curve.point(parameter * 0.37) : seam.curve.point(1.0 - parameter * 0.63);
                require(close(edge.curve.point(parameter), expected), "splitting a shared curve must preserve its complete geometry");
            }
        }
    }
    const auto before = mesh;
    require(!gui::removeMeshVertices(mesh, {1}) && mesh == before, "junction removal must be rejected atomically");
    require(gui::removeMeshVertices(mesh, {middle}) && mesh.valid() && mesh.vertices.size() == 10,
            "shared seam anchor removal must update every face and compact the table");
    fls::Project project;
    project.root = std::make_unique<Group>();
    auto node = std::make_unique<Mesh>();
    node->id = "mesh";
    node->data = mesh;
    project.root->append(std::move(node));
    const auto restored = fls::projectFromJson(fls::projectToJson(project));
    require(static_cast<const Mesh *>(restored.root->children.front().get())->data == mesh, "edited topology and shared handles must persist");
    auto batch = meshFixture();
    require(gui::removeMeshVertices(batch, {0, 5}) && batch.vertices.size() == 8
            && batch.contours[0].points.size() == 3 && batch.contours[1].points.size() == 3,
            "selected point deletion must update all affected loops in one topology edit");
    requireSharedCurve(batch, batch.contours[0].points[0].vertex, batch.contours[0].points[1].vertex);
}

void mouse(gui::ProjectCanvas &canvas, QEvent::Type type, const QPointF &point, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {}) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()), button, buttons, modifiers);
    QApplication::sendEvent(&canvas, &event);
}

void canvasEditing(bool transformed) {
    gui::EditorState state;
    gui::ProjectCanvas canvas;
    fls::Project project;
    project.root = std::make_unique<Group>();
    auto parent = std::make_unique<Group>();
    parent->id = "frame";
    auto mesh = std::make_unique<Mesh>();
    mesh->id = "mesh";
    mesh->data = meshFixture();
    if (transformed) {
        parent->transform = {37, -19, 1.4, -0.8, 31, 0.2};
        mesh->transform = {20, 25, -1.3, 0.6, -17, -0.25};
    }
    parent->append(std::move(mesh));
    project.root->append(std::move(parent));
    state.setProject(std::move(project));
    canvas.resize(1000, 700);
    canvas.setEditorState(&state);
    gui::EditorCommand selection;
    selection.type = gui::EditorCommandType::Selection;
    selection.entries = {"mesh"};
    require(state.executeCommand(selection) && canvas.tool() == "pen", "selecting a mesh must activate point editing");
    const auto current = [&]() -> const MeshData & { return static_cast<const Mesh *>(state.sceneNode("mesh"))->data; };
    const auto world = gui::sceneWorldTransform(*state.sceneNode("mesh"));
    const auto inverse = world.inverted();
    gui::CanvasCamera camera;
    camera.setViewBounds(world.mapRect(QRectF(0, 0, 200, 100)));
    camera.recompute({28, 28}, canvas.width() - 28, canvas.height() - 28);
    const auto screen = [&](const QPointF &local) { return camera.worldToScreen(world.map(local)); };
    const auto localDelta = [&](const QPointF &delta) { return inverse.map(delta) - inverse.map(QPointF()); };
    const auto click = [&](const QPointF &position, Qt::KeyboardModifiers modifiers = {}) {
        mouse(canvas, QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton, modifiers);
        mouse(canvas, QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton, modifiers);
    };
    const auto drag = [&](const QPointF &start, const QPointF &delta) {
        const auto end = start + camera.worldToScreen(delta) - camera.worldToScreen(QPointF());
        mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    };
    const auto undo = [&]() {
        gui::EditorCommand command;
        command.type = gui::EditorCommandType::Undo;
        require(state.executeCommand(command), "mesh edit undo must succeed");
    };
    const auto original = current();
    for (const int vertex : {1, 4, 7}) {
        const auto history = state.undoStack_.size();
        drag(screen(original.vertices[vertex]), {8, -5});
        require(state.undoStack_.size() == history + 1, "each point drag must create one project history entry");
        require(close(current().vertices[vertex], original.vertices[vertex] + localDelta({8, -5})),
                "every face and hole point must move correctly through the mesh transform");
        for (int other = 0; other < original.vertices.size(); ++other)
            if (other != vertex) require(close(current().vertices[other], original.vertices[other]), "unrelated vertices must stay fixed");
        if (vertex == 1) requireSharedCurve(current(), 1, 2);
        undo();
        require(current() == original, "undo must restore the complete mesh edit");
    }
    click(screen(original.vertices[1]));
    click(screen(original.vertices[4]), Qt::ShiftModifier);
    require(current() == original, "selecting points must leave the document unchanged");
    drag(screen(original.vertices[1]), {6, 4});
    for (const int vertex : {1, 4}) require(close(current().vertices[vertex], original.vertices[vertex] + localDelta({6, 4})),
            "multi-point movement must translate each canonical vertex exactly once");
    undo();
    click(screen(original.vertices[1]));
    const auto control = QPointF(100, 100.0 / 3.0);
    drag(screen(control), {9, 0});
    requireSharedCurve(current(), 1, 2);
    const auto edges = gui::meshEditEdges(current());
    const auto changed = *std::find_if(edges.cbegin(), edges.cend(), [](const auto &edge) { return edge.first == 1 && edge.last == 2; });
    require(close(changed.curve.control, control + localDelta({9, 0})), "shared control handles must follow the transformed pointer");
    undo();
    click(screen({100, 50}), Qt::ControlModifier);
    require(current().vertices.size() == original.vertices.size() + 1, "Ctrl-click must insert a shared point");
    require(canvas.handleKeyBinding(gui::KeyInteraction::CanvasRemovePathPoint, gui::KeyEventPhase::Press, false), "mesh removal key must be handled");
    require(current().vertices.size() == original.vertices.size(), "removing a selected shared point must compact the table");
    undo();
    undo();
    require(current() == original, "insertion and removal must have complete undo history");
    const auto start = screen(original.vertices[1]);
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, start + QPointF(20, 10), Qt::NoButton, Qt::LeftButton);
    canvas.cancelActiveInput();
    require(current() == original, "cancelling a drag must restore every face and shared handle");
    gui::EditorCommand lock;
    lock.entries = {"mesh"};
    lock.property = "locked";
    lock.value = true;
    require(state.executeCommand(lock), "mesh locking must succeed");
    drag(screen(original.vertices[1]), {8, -5});
    require(current() == original, "locked vertices must remain fixed");
    QApplication::processEvents();
}
} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("ForzaLiveryStudioTests");
    QCoreApplication::setApplicationName("MeshEditing");
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    try {
        edgeEditing();
        canvasEditing(false);
        canvasEditing(true);
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "Canonical mesh editing, shared curves, transforms and history passed\n";
    return 0;
}
