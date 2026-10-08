#include "editor_state.h"
#include "flat_payload.h"
#include "project_codec.h"
#include "scene_codec.h"

#include <QCoreApplication>
#include <QDebug>

#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

fls::scene::ContourFigure *figureIn(gui::EditorState &state) {
    return dynamic_cast<fls::scene::ContourFigure *>(state.sceneNode(QStringLiteral("contour")));
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);

    try {
        fls::Project project;
        fls::scene::ensureProjectSceneRoot(project);
        auto group = std::make_unique<fls::scene::Group>();
        auto figure = std::make_unique<fls::scene::ContourFigure>();
        auto shape = std::make_unique<fls::scene::Shape>();
        group->id = QStringLiteral("group");
        group->x = 25.0;
        group->scaleX = 1.7;
        group->rotation = 31.0;
        figure->id = QStringLiteral("contour");
        figure->name = QStringLiteral("Editable contour");
        figure->rotation = 14.0;
        figure->data.points = {{{0, 0}, fls::scene::ContourPointKind::Hard, {-2, 3}, {4, 5}, true},
                               {{25, 40}, fls::scene::ContourPointKind::Soft},
                               {{50, 0}, fls::scene::ContourPointKind::Hard}};
        figure->data.cutouts = {{{{15, 10}}, {{25, 20}}, {{35, 10}}},
                                {{{20, 7}}, {{22, 9}}}};
        figure->data.closed = true;
        figure->data.cutoutClosed = false;
        figure->data.fillColor = QColor(27, 88, 151, 93);
        figure->data.fillMask = true;
        const auto expected = figure->data;
        group->append(std::move(figure));
        project.root->append(std::move(group));
        shape->id = QStringLiteral("shape");
        shape->setVectorShape(101);
        project.root->append(std::move(shape));

        auto restored = fls::decodeProjectDocument(fls::encodeProjectDocument(project));
        gui::EditorState state;
        state.setProject(std::move(restored));
        require(figureIn(state) != nullptr, "project round trip lost the contour kind");
        require(figureIn(state)->data == expected, "project round trip lost anchors, handles, cutouts or fill settings");
        require(figureIn(state)->parent()->id == QStringLiteral("group"), "project round trip lost the contour parent");
        require(figureIn(state)->rotation == 14.0 && figureIn(state)->parent()->scaleX == 1.7,
                "project round trip lost contour transforms");
        auto clone = figureIn(state)->clone();
        static_cast<fls::scene::ContourFigure *>(clone.get())->data.points[0].position.setX(500);
        require(figureIn(state)->data == expected, "clone shares mutable contour data");

        state.setSelectedLayerIds({QStringLiteral("contour")});
        require(state.selectedLayerIds().contains(QStringLiteral("contour")), "contour cannot be selected");
        require(state.selectedLayers().isEmpty(), "contour is counted as a native shape");
        state.beginProjectEdit();
        figureIn(state)->data.points[1].position.setY(42);
        state.commitProjectEdit();
        require(state.isModified(), "contour edit did not mark project dirty");
        state.undo();
        require(figureIn(state)->data == expected, "undo did not restore the saved contour");
        state.redo();
        require(figureIn(state)->data.points[1].position.y() == 42, "redo lost the contour edit");

        require(state.copyEntriesToClipboard({QStringLiteral("contour")}), "contour copy failed");
        QSet<QString> copies;
        state.beginProjectEdit();
        require(state.duplicateEntriesInPlace({QStringLiteral("contour")}, &copies), "contour duplicate failed");
        state.commitProjectEdit();
        require(copies.size() == 1, "contour duplicate was omitted from selection");
        const auto *copy = dynamic_cast<const fls::scene::ContourFigure *>(state.sceneNode(*copies.constBegin()));
        require(copy != nullptr && copy->data == figureIn(state)->data, "contour duplicate lost editable data");
        state.undo();
        require(state.sceneNode(*copies.constBegin()) == nullptr, "undo did not remove the contour duplicate");
        state.setLayerVisible(QStringLiteral("contour"), false);
        require(!figureIn(state)->visible, "contour visibility toggle failed");
        state.setGroupAndDescendantLocked(QStringLiteral("group"), true);
        require(state.isLayerLocked(QStringLiteral("contour")), "contour does not inherit group locking");
        require(!state.duplicateEntriesInPlace({QStringLiteral("contour")}), "locked contour can be duplicated");

        const QByteArray withContour = fls::buildFlatPayload(*state.project());
        state.setGroupAndDescendantLocked(QStringLiteral("group"), false);
        state.setGroupDescendantVisible(QStringLiteral("group"), true);
        const QByteArray nestedWithContour = fls::buildNestedPayload(*state.project());
        auto mixedGroup = std::make_unique<fls::scene::Group>();
        mixedGroup->id = QStringLiteral("mixed");
        auto mixedContour = figureIn(state)->clone();
        auto mixedShape = state.sceneNode(QStringLiteral("shape"))->clone();
        mixedContour->id = QStringLiteral("mixed_contour");
        mixedShape->id = QStringLiteral("mixed_shape");
        mixedGroup->append(std::move(mixedContour));
        mixedGroup->append(std::move(mixedShape));
        auto mixedProject = *state.project();
        mixedProject.root->append(std::move(mixedGroup));
        require(!fls::buildNestedPayload(mixedProject).isEmpty(), "a contour makes a mixed group unexportable");
        state.removeEntries({QStringLiteral("contour")});
        require(fls::buildFlatPayload(*state.project()) == withContour, "contour changes the game shape payload");
        require(fls::buildNestedPayload(*state.project()) == nestedWithContour, "contour-only group changes the nested game payload");
        qInfo() << "Contour persistence, cloning, selection, history, duplication, locks and export checks passed";
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }

    return 0;
}
