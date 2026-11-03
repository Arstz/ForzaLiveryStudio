#include "editor_state.h"
#include "scene_codec.h"

#include <QCoreApplication>
#include <QTextStream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void appendShape(fls::scene::Group &group, const QString &id) {
    auto shape = std::make_unique<fls::scene::Shape>();
    shape->id = id;
    shape->setVectorShape(101);
    group.append(std::move(shape));
}

fls::Project nestedProject() {
    fls::Project project;
    fls::scene::ensureProjectSceneRoot(project);
    auto outer = std::make_unique<fls::scene::Group>();
    auto middle = std::make_unique<fls::scene::Group>();
    auto inner = std::make_unique<fls::scene::Group>();
    auto sibling = std::make_unique<fls::scene::Group>();
    outer->id = QStringLiteral("outer");
    middle->id = QStringLiteral("middle");
    inner->id = QStringLiteral("inner");
    sibling->id = QStringLiteral("sibling");
    appendShape(*inner, QStringLiteral("a"));
    appendShape(*inner, QStringLiteral("b"));
    appendShape(*sibling, QStringLiteral("c"));
    middle->append(std::move(inner));
    middle->append(std::move(sibling));
    outer->append(std::move(middle));
    appendShape(*outer, QStringLiteral("d"));
    project.root->append(std::move(outer));
    appendShape(*project.root, QStringLiteral("loose"));

    return project;
}

void testGroupSelection() {
    gui::EditorState state;
    state.setProject(nestedProject());
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::NoModifier);
    require(state.selectedLayerIds() == QSet<QString>{"a"}, "plain click must select one shape");
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"a", "b"}, "Shift click must select only the immediate group");
    require(state.selectedTransformTargetIds() == QVector<QString>{"inner"}, "the immediate group must retain its identity");
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"a", "b", "c"}, "the next Shift click must climb one parent");
    state.selectLayerAtPoint(QStringLiteral("c"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"a", "b", "c", "d"}, "clicking another member must climb the selected group's parent");
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"a", "b", "c", "d"}, "repeated Shift clicks at the outermost group must stay there");
    require(state.selectedTransformTargetIds() == QVector<QString>{"outer"}, "selection must stop at the outermost group");
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::NoModifier);
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"a", "b"}, "a normal click must reset the hierarchy sequence");
    state.selectLayerAtPoint(QStringLiteral("c"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"c"}, "Shift click must replace selection with the designated group");
    state.selectLayerAtPoint(QStringLiteral("loose"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"loose"}, "Shift click on an ungrouped shape must stay local");
    state.selectLayerAtPoint(QStringLiteral("loose"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"loose"}, "repeated Shift clicks must not select the scene root");
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::ControlModifier);
    require(state.selectedLayerIds() == QSet<QString>{"a", "loose"}, "Ctrl click must add a shape");
    state.selectLayerAtPoint(QStringLiteral("a"), Qt::ControlModifier);
    require(state.selectedLayerIds() == QSet<QString>{"loose"}, "Ctrl click must remove a shape");
}

void testSectionSelection() {
    fls::Project project = nestedProject();
    auto section = std::make_unique<fls::scene::Group>();
    section->id = QStringLiteral("section");
    section->isLiverySection = true;
    section->liverySectionSlot = 0;
    while (!project.root->children.empty()) {
        section->append(project.root->takeAt(0));
    }
    project.root->append(std::move(section));
    project.isLivery = true;
    gui::EditorState state;
    state.setProject(std::move(project));
    for (int click = 0; click < 5; ++click) {
        state.selectLayerAtPoint(QStringLiteral("a"), Qt::ShiftModifier);
    }
    require(state.selectedLayerIds() == QSet<QString>{"a", "b", "c", "d"}, "the hierarchy must stop before a livery section");
    state.selectLayerAtPoint(QStringLiteral("loose"), Qt::ShiftModifier);
    state.selectLayerAtPoint(QStringLiteral("loose"), Qt::ShiftModifier);
    require(state.selectedLayerIds() == QSet<QString>{"loose"}, "an ungrouped shape must not select its section container");
}

void testGroupsWithIdenticalLeaves() {
    fls::Project project;
    fls::scene::ensureProjectSceneRoot(project);
    auto outer = std::make_unique<fls::scene::Group>();
    auto middle = std::make_unique<fls::scene::Group>();
    auto inner = std::make_unique<fls::scene::Group>();
    outer->id = QStringLiteral("outer");
    middle->id = QStringLiteral("middle");
    inner->id = QStringLiteral("inner");
    appendShape(*inner, QStringLiteral("a"));
    auto guide = std::make_unique<fls::scene::GuideLayer>();
    guide->id = QStringLiteral("guide");
    inner->append(std::move(guide));
    middle->append(std::move(inner));
    outer->append(std::move(middle));
    project.root->append(std::move(outer));
    gui::EditorState state;
    state.setProject(std::move(project));
    for (const QString &group : {QStringLiteral("inner"), QStringLiteral("middle"), QStringLiteral("outer"), QStringLiteral("outer")}) {
        state.selectLayerAtPoint(QStringLiteral("a"), Qt::ShiftModifier);
        require(state.selectedLayerIds() == QSet<QString>{"a"}, "climbing identical leaf sets must keep the same shape selected");
        require(state.selectedGuideLayerIds() == QSet<QString>{"guide"}, "group selection must include its guides");
        require(state.selectedTransformTargetIds() == QVector<QString>{group}, "identical leaf sets must still advance exactly one group per click");
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        testGroupSelection();
        testSectionSelection();
        testGroupsWithIdenticalLeaves();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "Group selection checks passed\n";
    return 0;
}
