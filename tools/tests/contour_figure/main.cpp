#include "editor_state.h"
#include "flat_payload.h"
#include "project_codec.h"
#include "scene_codec.h"
#include "property_panel.h"
#include "path_interaction.h"
#include "scene_view.h"

#include <QApplication>
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

void multiplePathProperties() {
    fls::Project project;
    fls::scene::ensureProjectSceneRoot(project);
    for (int index = 0; index < 2; ++index) {
        auto figure = std::make_unique<fls::scene::ContourFigure>();
        figure->id = QString::number(index);
        figure->scaleX = 0.5 + index;
        project.root->append(std::move(figure));
    }
    gui::EditorState state;
    state.setProject(std::move(project));
    state.setSelectedLayerIds({QStringLiteral("0"), QStringLiteral("1")});
    gui::PropertyPanel properties(&state);
    properties.setSelection({}, {}, {}, state.selectedContours());
    auto *scale = properties.findChild<QDoubleSpinBox *>(QStringLiteral("property.scaleX"));
    require(scale != nullptr && scale->isEnabled() && scale->value() == 1.0 && !scale->styleSheet().isEmpty(),
            "multiple saved paths lost their mixed transform value");
    scale->setValue(1.25);
    require(state.sceneNode(QStringLiteral("0"))->scaleX == 0.75
                && state.sceneNode(QStringLiteral("1"))->scaleX == 1.75,
            "multiple saved-path edits did not preserve the transform difference");
    state.undo();
    require(state.sceneNode(QStringLiteral("0"))->scaleX == 0.5
                && state.sceneNode(QStringLiteral("1"))->scaleX == 1.5,
            "undo did not restore multiple saved-path transforms");
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication application(argc, argv);

    try {
        multiplePathProperties();
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
        require(state.selectedContours().size() == 1, "selected contour cannot be edited in Properties");
        gui::PropertyPanel properties(&state);
        properties.setSelection({}, {}, {}, state.selectedContours());
        auto *scaleX = properties.findChild<QDoubleSpinBox *>(QStringLiteral("property.scaleX"));
        auto *scaleY = properties.findChild<QDoubleSpinBox *>(QStringLiteral("property.scaleY"));
        auto *rotation = properties.findChild<QDoubleSpinBox *>(QStringLiteral("property.rotation"));
        require(scaleX != nullptr && scaleY != nullptr && rotation != nullptr,
                "saved-path transform fields are unavailable");
        require(scaleX->isEnabled() && scaleY->isEnabled() && scaleX->value() == 1.0
                    && scaleY->value() == 1.0 && rotation->value() == 14.0,
                "saved-path Properties show stale transform values");
        scaleX->setValue(0.75);
        require(figureIn(state)->scaleX == 0.75 && figureIn(state)->scaleY == 1.0,
                "saved-path scale edit did not reach the stored transform");
        state.undo();
        properties.setSelection({}, {}, {}, state.selectedContours());
        require(scaleX->value() == 1.0, "undo did not restore the saved-path scale field");
        figureIn(state)->scaleY = 1.25;
        properties.refreshTransformFields();
        require(scaleY->value() == 1.25, "live refresh skipped the saved-path transform");
        figureIn(state)->scaleY = 1.0;
        properties.refreshTransformFields();
        const std::array<quint8, 4> newColor{37, 61, 219, 255};
        properties.applyColorToSelection(newColor);
        require(figureIn(state)->data.fillColor == QColor(219, 61, 37), "Properties did not set the saved fill color");
        require(properties.currentSelectionColor() == newColor, "Properties did not refresh the saved path color");
        state.undo();
        require(figureIn(state)->data == expected, "undo did not restore the saved path color");
        require(gui::contourOutlineStrokeColor(QColor(Qt::black)) == QColor(Qt::white), "dark path stroke is not white");
        require(gui::contourOutlineStrokeColor(QColor(Qt::white)) == QColor(Qt::black), "light path stroke is not black");

        gui::PathInteraction lining(fls::scene::ContourPathKind::Lining);
        require(lining.fillColor == QColor(Qt::black), "new Lining path is not black");
        lining.fillColor = QColor(Qt::red);
        lining.reset();
        require(lining.fillColor == QColor(Qt::black), "reset Lining path is not black");
        auto liningFigure = std::make_unique<fls::scene::ContourFigure>();
        liningFigure->id = QStringLiteral("lining");
        liningFigure->data.pathKind = fls::scene::ContourPathKind::Lining;
        liningFigure->data.strokeWidth = 12.5;
        liningFigure->data.closed = true;
        liningFigure->data.points = {{{0, 0}, fls::scene::ContourPointKind::Hard, {}, {}, false, 4.0},
                                     {{40, 10}, fls::scene::ContourPointKind::Soft, {}, {}, false, 9.0},
                                     {{80, 0}, fls::scene::ContourPointKind::Hard, {}, {}, false, 14.0}};
        const auto expectedLining = liningFigure->data;
        auto liningProject = *state.project();
        liningProject.root->append(std::move(liningFigure));
        auto restoredLiningProject = fls::decodeProjectDocument(fls::encodeProjectDocument(liningProject));
        const auto *restoredLining = dynamic_cast<const fls::scene::ContourFigure *>(restoredLiningProject.root->children.back().get());
        require(restoredLining != nullptr && restoredLining->data == expectedLining, "saved Lining lost its tool type or widths");
        require(fls::scene::contourFillColor(restoredLining->data) == QColor(Qt::black), "saved Lining default color is not black");

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
        properties.setSelection({}, {}, {}, state.selectedContours());
        require(!properties.currentSelectionColor().has_value(), "locked contour color can be edited");
        const auto lockedColor = figureIn(state)->data.fillColor;
        properties.applyColorToSelection(newColor);
        require(figureIn(state)->data.fillColor == lockedColor, "Properties changed a locked contour color");
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
