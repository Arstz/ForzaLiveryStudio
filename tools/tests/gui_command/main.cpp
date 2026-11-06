#include "main_window.h"
#include "editor_state.h"
#include "layer_state_delegate.h"
#include "property_panel.h"
#include "scene_codec.h"
#include "project_codec.h"
#include "game_export.h"
#include "cgroup_codec.h"
#include "vinyl_decoder.h"
#include "project_canvas.h"
#include "canvas_camera.h"
#include "tool_settings_widget.h"
#include "lining_fill.h"
#include "cubic_contour.h"
#include "image_io.h"

#include <limits>

#include <QApplication>
#include <QColorDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <cmath>
#include <stdexcept>

namespace {

void require(bool condition, const QString &message) {
    if (!condition) throw std::runtime_error(message.toStdString());
}

fls::scene::Layer *nodeIn(fls::scene::Group &group, const QString &id) {
    if (group.id == id) return &group;
    for (const auto &child : group.children) {
        if (child->id == id) return child.get();
        if (auto *nested = dynamic_cast<fls::scene::Group *>(child.get())) {
            if (auto *node = nodeIn(*nested, id)) return node;
        }
    }
    return nullptr;
}

std::unique_ptr<fls::scene::Shape> shape(const QString &id, double x) {
    auto node = std::make_unique<fls::scene::Shape>();
    node->id = id;
    node->name = id;
    node->setVectorShape(101);
    node->x = x;
    node->y = x * 0.6;
    node->scaleX = 0.7;
    node->scaleY = 1.3;
    node->rotation = 13;
    node->skew = -0.15;
    node->color = {31, 67, 139, 255};
    return node;
}

fls::Project fixture() {
    fls::Project project;
    fls::scene::ensureProjectSceneRoot(project);
    auto source = std::make_unique<fls::scene::Group>();
    source->id = source->name = QStringLiteral("source");
    source->transform = {-30, 60, -1.8, 0.7, 37, 0.3};
    auto inner = std::make_unique<fls::scene::Group>();
    inner->id = inner->name = QStringLiteral("inner");
    inner->transform = {20, -11, 1.4, 0.8, -17, -0.2};
    auto a = shape(QStringLiteral("a"), -20);
    a->mask = true;
    inner->append(std::move(a));
    inner->append(shape(QStringLiteral("b"), 20));
    auto path = std::make_unique<fls::scene::ContourFigure>();
    path->id = path->name = QStringLiteral("path");
    path->data.points = {{{0, 0}}, {{15, 30}}, {{40, 0}}};
    path->data.closed = true;
    path->data.fillColor = QColor(40, 80, 120, 190);
    inner->append(std::move(path));
    auto guide = std::make_unique<fls::scene::GuideLayer>();
    guide->id = guide->name = QStringLiteral("guide");
    inner->append(std::move(guide));
    source->append(std::move(inner));
    source->append(shape(QStringLiteral("c"), 55));
    auto destination = std::make_unique<fls::scene::Group>();
    destination->id = destination->name = QStringLiteral("destination");
    destination->transform = {100, -40, 0.6, 2.4, -22, -0.4};
    destination->append(shape(QStringLiteral("d"), 70));
    destination->append(shape(QStringLiteral("e"), 95));
    project.root->append(std::move(source));
    project.root->append(std::move(destination));
    project.root->append(shape(QStringLiteral("loose"), 80));
    project.root->append(shape(QStringLiteral("untouched"), -90));
    return project;
}

QByteArray fingerprint(const fls::scene::Layer &node) {
    fls::Project holder;
    fls::scene::ensureProjectSceneRoot(holder);
    auto copy = node.clone();
    copy->transform = {};
    holder.root->append(std::move(copy));
    return fls::encodeProjectDocument(holder);
}

void sameWorld(const QTransform &expected, const QTransform &actual, const QString &label) {
    for (const QPointF point : {QPointF(), QPointF(1, 0), QPointF(0, 1), QPointF(43, -29)}) {
        require(QLineF(expected.map(point), actual.map(point)).length() < 1e-7,
                QStringLiteral("%1 changed its world transform: expected (%2, %3), actual (%4, %5)")
                    .arg(label).arg(expected.map(point).x()).arg(expected.map(point).y())
                    .arg(actual.map(point).x()).arg(actual.map(point).y()));
    }
}

void click(QWidget *widget, QPoint point, Qt::KeyboardModifiers modifiers = {}) {
    const QPoint global = widget->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(widget, &release);
    QApplication::processEvents();
}

struct Harness {
    gui::MainWindow window;
    gui::EditorState *state;
    gui::LayerTreeView *tree;
    gui::LayerTreeModel *model;
    gui::PropertyPanel *properties = nullptr;
    QVector<gui::EditorCommandType> commands;
    QStringList actions;

    Harness() {
        state = window.findChild<gui::EditorState *>();
        tree = window.findChild<gui::LayerTreeView *>();
        require(state && tree, "main window is missing its state or Layers view");
        model = dynamic_cast<gui::LayerTreeModel *>(tree->model());
        for (QWidget *widget : window.findChildren<QWidget *>()) {
            if (auto *panel = dynamic_cast<gui::PropertyPanel *>(widget)) properties = panel;
        }
        require(model && properties, "main window is missing its Layers model or Properties panel");
        QObject::connect(state, &gui::EditorState::commandExecuted, state,
            [this](gui::EditorCommandType type, const QVector<QString> &) { commands.push_back(type); });
        QObject::connect(state, &gui::EditorState::editActionRequested, state,
            [this](const QString &id) { actions.push_back(id); });
    }

    void reset(fls::Project project = fixture()) {
        state->setProject(std::move(project));
        state->noteProjectStructureChanged();
        tree->resize(680, 740);
        tree->expandAll();
        tree->doItemsLayout();
        QApplication::processEvents();
        commands.clear();
        actions.clear();
        validate();
    }

    void select(const QVector<QString> &ids) {
        gui::EditorCommand command;
        command.type = gui::EditorCommandType::Selection;
        command.entries = ids;
        require(state->executeCommand(command), "selection command failed");
        QApplication::processEvents();
        commands.clear();
    }

    QModelIndex index(const QString &id, const QModelIndex &parent = {}) const {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const QModelIndex item = model->index(row, 0, parent);
            if (item.data(gui::LayerTreeModel::EntryIdRole).toString() == id) return item;
            const QModelIndex nested = index(id, item);
            if (nested.isValid()) return nested;
        }
        return {};
    }

    void trigger(const QString &id) {
        QAction *action = window.findChild<QAction *>(id);
        require(action && action->property("editorCommandAction").toString() == id, "Edit action bypasses command routing: " + id);
        action->trigger();
        QApplication::processEvents();
        require(actions.contains(id), "Edit action did not reach editor state: " + id);
    }

    QCheckBox *flag(const QString &property) {
        auto *box = properties->findChild<QCheckBox *>("property." + property);
        require(box && box->isEnabled(), "property flag is unavailable: " + property);
        return box;
    }

    void clickFlag(const QString &property) {
        QCheckBox *box = flag(property);
        click(box, QPoint(8, box->height() / 2));
        require(commands.contains(gui::EditorCommandType::Property), "property checkbox bypassed editor commands: " + property);
        validate();
    }

    void badge(const QString &id, int slot) {
        const QModelIndex item = index(id);
        require(item.isValid(), "missing badge row: " + id);
        const QRect rect = tree->visualRect(item);
        const int right = rect.right() - gui::kLayerBadgeRightMargin
            - slot * (gui::kLayerBadgeSize + gui::kLayerBadgeGap);
        click(tree->viewport(), QPoint(right - gui::kLayerBadgeSize / 2, rect.center().y()));
        require(commands.contains(gui::EditorCommandType::Property), "Layers badge bypassed editor commands: " + id);
        validate();
    }

    bool drop(const QVector<QString> &ids, const QString &target, bool inside = true) {
        select(ids);
        tree->expandAll();
        tree->doItemsLayout();
        const QModelIndex item = index(target);
        require(item.isValid(), "missing drop target: " + target);
        const QRect rect = tree->visualRect(item);
        const QPoint position(rect.left() + 36, inside ? rect.center().y() : rect.top() + 1);
        QModelIndexList indexes;
        for (const QString &id : ids) indexes.push_back(index(id));
        std::unique_ptr<QMimeData> mime(model->mimeData(indexes));
        QDragEnterEvent enter(position, Qt::MoveAction, mime.get(), Qt::LeftButton, {});
        QApplication::sendEvent(tree->viewport(), &enter);
        QDragMoveEvent move(position, Qt::MoveAction, mime.get(), Qt::LeftButton, {});
        QApplication::sendEvent(tree->viewport(), &move);
        QDropEvent event(position, Qt::MoveAction, mime.get(), Qt::LeftButton, {});
        QApplication::sendEvent(tree->viewport(), &event);
        QApplication::processEvents();
        validate();
        if (event.isAccepted()) require(commands.contains(gui::EditorCommandType::MoveEntries), "drop bypassed editor commands");
        return event.isAccepted();
    }

    void validate() const {
        QSet<QString> seen;
        const auto check = [&](auto &&self, const fls::scene::Group &group, const QModelIndex &parent) -> void {
            require(model->rowCount(parent) == static_cast<int>(group.children.size()), "tree row count differs from state");
            for (int row = 0; row < model->rowCount(parent); ++row) {
                const QModelIndex item = model->index(row, 0, parent);
                const auto *node = group.children[group.children.size() - 1 - row].get();
                require(item.isValid() && item.parent() == parent, "invalid model parent/index relationship");
                require(item.data(gui::LayerTreeModel::EntryIdRole).toString() == node->id, "tree order differs from state draw order");
                require(!seen.contains(node->id), "tree contains a duplicate node");
                require(node->parent() == &group && state->sceneNode(node->id) == node, "scene parent or index cache is stale");
                seen.insert(node->id);
                if (const auto *nested = dynamic_cast<const fls::scene::Group *>(node)) {
                    require(nested->isLiverySection || nested->children.size() >= 2, "Forza group has fewer than two children");
                    self(self, *nested, item);
                } else require(model->rowCount(item) == 0, "leaf item has tree children");
            }
        };
        const fls::scene::Group *displayed = state->project()->isLivery
            ? state->groupForId(state->activeSectionId_) : state->project()->root.get();
        require(displayed != nullptr, "displayed tree root is missing");
        check(check, *displayed, QModelIndex());
    }
};

void flags(Harness &h) {
    h.reset();
    h.select({"source", "loose"});
    require(h.flag("mask")->checkState() == Qt::PartiallyChecked, "Properties did not display a mixed mask selection");
    h.clickFlag("mask");
    for (const QString &id : {"a", "b", "c", "loose"})
        require(static_cast<const fls::scene::Shape *>(h.state->sceneNode(id))->mask, "mask did not propagate to " + id);
    require(!static_cast<const fls::scene::Shape *>(h.state->sceneNode("d"))->mask, "mask changed an unselected shape");
    require(h.index("source").data(gui::LayerTreeModel::MaskRole).toBool(), "guide/path incorrectly keeps the group mask badge off");
    h.trigger("undo");
    require(!static_cast<const fls::scene::Shape *>(h.state->sceneNode("b"))->mask, "mask undo failed");
    h.trigger("redo");
    require(static_cast<const fls::scene::Shape *>(h.state->sceneNode("b"))->mask, "mask redo failed");
    h.badge("source", 1);
    for (const QString &id : {"a", "b", "c", "loose"})
        require(!static_cast<const fls::scene::Shape *>(h.state->sceneNode(id))->mask, "selected mask badge skipped " + id);
    require(h.state->selectedLayerIds().contains("loose"), "badge click collapsed the selection");
    h.clickFlag("visible");
    for (const QString &id : {"source", "inner", "a", "b", "c", "path", "guide", "loose"})
        require(!h.state->sceneNode(id)->visible, "visibility did not propagate to " + id);
    h.badge("source", 2);
    require(h.state->sceneNode("loose")->visible && h.state->sceneNode("guide")->visible, "selected visibility badge skipped selected items");
    h.clickFlag("locked");
    require(h.state->isLayerLocked("loose") && h.state->isLayerLocked("a"), "lock did not propagate");
    require(!h.state->isLayerLocked("untouched"), "lock changed an unselected shape");
    h.badge("source", 0);
    require(!h.state->isLayerLocked("loose") && !h.state->isLayerLocked("a"), "bulk unlock failed");
    h.badge("untouched", 1);
    require(static_cast<const fls::scene::Shape *>(h.state->sceneNode("untouched"))->mask, "unselected badge did not affect its own row");
    require(!static_cast<const fls::scene::Shape *>(h.state->sceneNode("loose"))->mask, "unselected badge unexpectedly changed the selection");
    h.reset();
    const QRect row = h.tree->visualRect(h.index("loose"));
    click(h.tree->viewport(), QPoint(row.left() + 44, row.center().y()));
    require(h.commands.contains(gui::EditorCommandType::Selection) && h.state->selectedLayerIds() == QSet<QString>{"loose"},
            "Layers row click bypassed state selection commands");
}

void properties(Harness &h) {
    const QStringList names{"x", "y", "scaleX", "scaleY", "rotation", "skew", "opacity"};
    const QList<double> values{44.5, -19.25, -1.3, 2.2, 128.0, 0.42, 0.37};
    for (int index = 0; index < names.size(); ++index) {
        h.reset();
        h.select({"loose"});
        auto expected = *h.state->project();
        auto *node = static_cast<fls::scene::Shape *>(nodeIn(*expected.root, "loose"));
        const QString name = names[index];
        const double value = values[index];
        if (name == "x") node->x = value;
        else if (name == "y") node->y = value;
        else if (name == "scaleX") node->scaleX = value;
        else if (name == "scaleY") node->scaleY = value;
        else if (name == "rotation") node->rotation = value;
        else if (name == "skew") node->skew = value;
        else { node->opacity = value; node->color[3] = static_cast<quint8>(std::lround(value * 255.0)); }
        auto *control = h.properties->findChild<QDoubleSpinBox *>("property." + name);
        require(control && control->isEnabled(), "numeric property is unavailable: " + name);
        const QByteArray before = fls::encodeProjectDocument(*h.state->project());
        control->setValue(value);
        QApplication::processEvents();
        require(h.commands.contains(gui::EditorCommandType::Property), "numeric property bypassed editor commands: " + name);
        require(fls::encodeProjectDocument(expected) == fls::encodeProjectDocument(*h.state->project()), "numeric property changed unrelated data: " + name);
        h.trigger("undo");
        require(before == fls::encodeProjectDocument(*h.state->project()), "numeric property undo failed: " + name);
        h.trigger("redo");
        require(fls::encodeProjectDocument(expected) == fls::encodeProjectDocument(*h.state->project()), "numeric property redo failed: " + name);
    }
    h.reset(); h.select({"loose"});
    auto *name = h.properties->findChild<QLineEdit *>("property.name");
    name->setText("Renamed");
    QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection);
    require(h.state->sceneNode("loose")->name == "Renamed", "name edit did not propagate");
    auto *shapeId = h.properties->findChild<QSpinBox *>("property.shapeId");
    shapeId->setValue(102);
    require(static_cast<fls::scene::Shape *>(h.state->sceneNode("loose"))->shapeId == 102, "shape ID edit did not propagate");
    h.reset(); h.select({"source", "loose"});
    require(name->text().isEmpty(), "mixed group/shape names were not displayed as mixed");
    name->setText("Selected artwork");
    QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection);
    require(h.state->sceneNode("source")->name == "Selected artwork" && h.state->sceneNode("loose")->name == "Selected artwork",
            "name edit skipped part of a mixed selection");
    require(h.state->sceneNode("a")->name == "a", "group rename also renamed its contents");
    h.reset(); h.select({"loose"});
    const QByteArray beforeDrag = fls::encodeProjectDocument(*h.state->project());
    QWidget *label = h.properties->findChild<QWidget *>("property.label.x");
    require(label != nullptr, "position drag label is missing");
    const QPoint start(8, 8), end(8, -12);
    QMouseEvent press(QEvent::MouseButtonPress, start, label->mapToGlobal(start), Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(label, &press);
    QMouseEvent move(QEvent::MouseMove, end, label->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, {});
    QApplication::sendEvent(label, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, end, label->mapToGlobal(end), Qt::LeftButton, Qt::NoButton, {});
    QApplication::sendEvent(label, &release);
    require(h.commands.contains(gui::EditorCommandType::Property) && h.state->sceneNode("loose")->x != 80,
            "numeric label drag bypassed state commands");
    require(h.state->undoStack_.size() == 1, "numeric drag produced multiple undo entries");
    h.trigger("undo");
    require(beforeDrag == fls::encodeProjectDocument(*h.state->project()), "numeric drag undo failed");
    for (bool accept : {false, true}) {
        h.reset(); h.select({"source", "loose"});
        const QByteArray before = fls::encodeProjectDocument(*h.state->project());
        bool visited = false;
        QTimer::singleShot(0, [&]() {
            auto *dialog = dynamic_cast<QColorDialog *>(QApplication::activeModalWidget());
            require(dialog != nullptr, "color button did not open its dialog");
            visited = true;
            dialog->setCurrentColor(QColor(110, 170, 210, 151));
            if (accept) dialog->accept(); else dialog->reject();
        });
        auto *color = h.properties->findChild<QPushButton *>("property.color");
        color->click();
        require(visited, "color dialog test did not execute");
        if (accept) {
            for (const QString &id : {"a", "b", "c", "loose"})
                require(static_cast<const fls::scene::Shape *>(h.state->sceneNode(id))->color == std::array<quint8, 4>{210, 170, 110, 151}, "color edit skipped " + id);
            require(h.state->undoStack_.size() == 1, "color gesture produced multiple undo entries");
            h.trigger("undo");
        }
        require(before == fls::encodeProjectDocument(*h.state->project()), "color cancel/undo changed the project");
    }
}

void reparent(Harness &h) {
    h.reset();
    const QTransform innerWorld = gui::sceneWorldTransform(*h.state->sceneNode("inner"));
    const QByteArray innerProperties = fingerprint(*h.state->sceneNode("inner"));
    require(h.drop({"inner"}, "destination"), "group reparent was rejected");
    require(h.state->parentGroupForEntry("inner") == "destination", "group was not added to the new parent");
    sameWorld(innerWorld, gui::sceneWorldTransform(*h.state->sceneNode("inner")), "reparented group");
    require(fingerprint(*h.state->sceneNode("inner")) == innerProperties, "reparent changed authored properties or child transforms");
    require(h.index("inner").row() < h.index("d").row(), "group dropped above a shape did not draw above it");
    require(h.drop({"inner"}, "untouched", false), "moving group to root was rejected");
    require(h.state->parentGroupForEntry("inner").isEmpty(), "group was not moved to root");
    require(h.index("inner").row() == 0, "moving to the top did not change draw order");
    sameWorld(innerWorld, gui::sceneWorldTransform(*h.state->sceneNode("inner")), "root group");
    h.trigger("undo");
    require(h.state->parentGroupForEntry("inner") == "destination", "undo did not restore the parent");
    h.trigger("redo");
    require(h.state->parentGroupForEntry("inner").isEmpty(), "redo did not restore root placement");
    const QByteArray beforeCycle = fls::encodeProjectDocument(*h.state->project());
    require(!h.drop({"inner"}, "inner"), "group accepted itself as parent");
    require(beforeCycle == fls::encodeProjectDocument(*h.state->project()), "invalid drop changed the project");
    h.reset();
    require(!h.drop({"source"}, "inner"), "group accepted a descendant as parent");
    require(h.drop({"a", "c"}, "destination"), "selection from different parents was rejected");
    require(h.state->parentGroupForEntry("a") == "destination" && h.state->parentGroupForEntry("c") == "destination", "multi-parent move skipped an item");
    require(h.index("c").row() < h.index("a").row(), "multi-parent move reversed relative draw order");
    h.reset();
    const QTransform original = gui::sceneWorldTransform(*h.state->sceneNode("c"));
    const QByteArray authored = fingerprint(*h.state->sceneNode("c"));
    require(h.drop({"c"}, "destination"), "shape reparent was rejected");
    require(h.drop({"inner"}, "destination"), "last group reparent was rejected");
    require(h.state->sceneNode("source") == nullptr, "empty source group was retained");
    for (int step = 0; step < 12; ++step) {
        require(h.drop({"c"}, step % 2 == 0 ? "inner" : "destination"), "repeated reparent failed");
        sameWorld(original, gui::sceneWorldTransform(*h.state->sceneNode("c")), "repeated shape move");
        require(fingerprint(*h.state->sceneNode("c")) == authored, "repeated move changed authored properties");
    }
    QTemporaryDir exported;
    const QString folder = exported.filePath("group");
    fls::exportGameGroup(*h.state->project(), folder, fls::ExportGameVersion::Horizon5);
    auto imported = fls::importCGroupNested(folder);
    const auto native = fls::VinylTreeDecoder().decodeGroup(fls::readCGroupPayload(folder));
    require(fls::validateTree(native).isEmpty(), "reparented export contains an invalid native tree");
    QVector<bool> nativeMasks;
    const auto collectMasks = [&](auto &&self, const fls::VinylGroup &group, bool inherited) -> void {
        for (const auto &item : group.items) {
            if (item.isShape()) nativeMasks.push_back(inherited || group.isMask || std::get<fls::VinylShape>(item.value).isMask);
            else self(self, *std::get<fls::VinylGroupPtr>(item.value), inherited || group.isMask);
        }
    };
    collectMasks(collectMasks, native, false);
    const auto originalShapes = gui::sceneShapeLeaves(*h.state->project()->root);
    const auto importedShapes = gui::sceneShapeLeaves(*imported.root);
    require(originalShapes.size() == importedShapes.size(), "reparented export lost shapes");
    const QPointF originalOrigin = gui::sceneWorldTransform(*originalShapes.front()).map(QPointF());
    const QPointF importedOrigin = gui::sceneWorldTransform(*importedShapes.front()).map(QPointF());
    for (int index = 0; index < originalShapes.size(); ++index) {
        const auto *before = originalShapes[index];
        const auto *after = importedShapes[index];
        require(index < nativeMasks.size() && before->mask == nativeMasks[index], "reparented export changed mask flags");
        require(before->color == after->color && before->shapeId == after->shapeId,
                "reparented export changed colors or draw order");
        for (const QPointF point : {QPointF(), QPointF(1, 0), QPointF(0, 1)}) {
            const QPointF expected = gui::sceneWorldTransform(*before).map(point) - originalOrigin;
            const QPointF actual = gui::sceneWorldTransform(*after).map(point) - importedOrigin;
            require(QLineF(expected, actual).length() < 0.03, "reparented export changed artwork placement");
        }
    }
    auto restored = fls::decodeProjectDocument(fls::encodeProjectDocument(*h.state->project()));
    sameWorld(original, gui::sceneWorldTransform(*nodeIn(*restored.root, "c")), "saved/reloaded shape");
    require(fingerprint(*nodeIn(*restored.root, "c")) == authored, "save/reload changed authored properties");
}

void singleChildGroups(Harness &h) {
    for (const QString &action : {QString("delete_selected"), QString("cut")}) {
        h.reset();
        const QByteArray before = fls::encodeProjectDocument(*h.state->project());
        const QTransform world = gui::sceneWorldTransform(*h.state->sceneNode("e"));
        const QByteArray authored = fingerprint(*h.state->sceneNode("e"));
        const int row = h.index("destination").row();
        h.select({"d"}); h.trigger(action);
        require(h.state->sceneNode("destination") == nullptr && h.state->sceneNode("d") == nullptr,
                "single-child group was retained after " + action);
        require(h.state->parentGroupForEntry("e").isEmpty() && h.index("e").row() == row,
                "survivor changed parent position or draw order after " + action);
        sameWorld(world, gui::sceneWorldTransform(*h.state->sceneNode("e")), "deleted group survivor");
        require(fingerprint(*h.state->sceneNode("e")) == authored, "group collapse changed child properties");
        require(h.state->undoStack_.size() == 1, "group collapse created a separate undo entry");
        h.validate();
        h.trigger("undo");
        require(before == fls::encodeProjectDocument(*h.state->project()), "group collapse undo failed");
        h.trigger("redo");
        require(h.state->sceneNode("destination") == nullptr, "group collapse redo failed");
        h.validate();
    }
    h.reset();
    const QTransform world = gui::sceneWorldTransform(*h.state->sceneNode("c"));
    require(h.drop({"inner"}, "destination"), "move that collapses the source was rejected");
    require(h.state->sceneNode("source") == nullptr && h.state->parentGroupForEntry("c").isEmpty(),
            "reparenting retained a source with one child");
    sameWorld(world, gui::sceneWorldTransform(*h.state->sceneNode("c")), "move survivor");
    require(h.state->selectionEntries() == QVector<QString>{"inner"}, "collapse lost the moved group selection");
    h.reset();
    const QTransform innerWorld = gui::sceneWorldTransform(*h.state->sceneNode("inner"));
    const QByteArray innerProperties = fingerprint(*h.state->sceneNode("inner"));
    h.select({"c"}); h.trigger("delete_selected");
    require(h.state->sceneNode("source") == nullptr && h.state->parentGroupForEntry("inner").isEmpty(),
            "surviving group was not promoted");
    sameWorld(innerWorld, gui::sceneWorldTransform(*h.state->sceneNode("inner")), "promoted group");
    require(fingerprint(*h.state->sceneNode("inner")) == innerProperties, "promoting a group changed its descendants");
    h.validate();

    auto project = fixture();
    auto *source = static_cast<fls::scene::Group *>(nodeIn(*project.root, "source"));
    auto *inner = static_cast<fls::scene::Group *>(nodeIn(*project.root, "inner"));
    inner->children.clear();
    inner->append(shape("survivor", 10));
    inner->append(shape("remove-inner", -10));
    source->visible = false;
    source->locked = true;
    source->flags = 64;
    auto *survivor = nodeIn(*project.root, "survivor");
    const QTransform cascadeWorld = gui::sceneWorldTransform(*survivor);
    h.reset(std::move(project));
    const QByteArray before = fls::encodeProjectDocument(*h.state->project());
    h.select({"source"});
    h.state->beginProjectEdit();
    h.state->removeEntries({"c", "remove-inner"});
    h.state->commitProjectEdit();
    h.state->noteProjectStructureChanged();
    require(h.state->sceneNode("source") == nullptr && h.state->sceneNode("inner") == nullptr,
            "nested singleton groups were retained");
    sameWorld(cascadeWorld, gui::sceneWorldTransform(*h.state->sceneNode("survivor")), "nested collapse");
    require(h.state->selectionEntries() == QVector<QString>{"survivor"}, "collapsed group selection was not remapped");
    require(!h.state->sceneNode("survivor")->visible && h.state->isLayerLocked("survivor")
            && static_cast<const fls::scene::Shape *>(h.state->sceneNode("survivor"))->mask,
            "collapsed group lost inherited visibility, lock, or mask state");
    h.validate();
    h.trigger("undo");
    require(before == fls::encodeProjectDocument(*h.state->project()), "nested collapse undo failed");
    require(h.state->selectionEntries() == QVector<QString>{"source"}, "undo did not restore the selected group");
    h.trigger("redo");
    h.validate();
}

void rejectedMoves(Harness &h) {
    for (bool locked : {false, true}) {
        auto project = fixture();
        auto *destination = nodeIn(*project.root, "destination");
        if (locked) destination->locked = true;
        else destination->scaleX = 0;
        h.reset(std::move(project));
        const QByteArray before = fls::encodeProjectDocument(*h.state->project());
        require(!h.drop({"inner", "loose"}, "destination"), "invalid destination accepted a drop");
        require(before == fls::encodeProjectDocument(*h.state->project()) && h.state->undoStack_.isEmpty(),
                "rejected drop changed the document or history");
    }
    h.reset();
    const QByteArray before = fls::encodeProjectDocument(*h.state->project());
    gui::EditorCommand command;
    command.type = gui::EditorCommandType::LocalTransforms;
    command.entries = {"loose"};
    command.transforms.insert("untouched", {});
    require(!h.state->executeCommand(command), "transform command edited a node outside its targets");
    command.transforms.clear();
    auto invalid = h.state->sceneNode("loose")->transform;
    invalid.y = std::numeric_limits<double>::quiet_NaN();
    command.transforms.insert("loose", invalid);
    require(!h.state->executeCommand(command), "transform command accepted a nonfinite value");
    require(before == fls::encodeProjectDocument(*h.state->project()) && h.state->undoStack_.isEmpty(),
            "invalid transform command changed the document or history");
    require(h.drop({"loose"}, "untouched", false), "same-parent reorder was rejected");
    require(h.index("loose").row() == 0, "same-parent reorder did not update draw order");
}

void liveryMoves(Harness &h) {
    auto project = fixture();
    auto section = std::make_unique<fls::scene::Group>();
    section->id = "section";
    section->isLiverySection = true;
    section->liverySectionSlot = 0;
    section->transform = {15, -12, 1.2, 0.8, 20, -0.15};
    while (!project.root->children.empty()) section->append(project.root->takeAt(0));
    project.root->append(std::move(section));
    auto empty = std::make_unique<fls::scene::Group>();
    empty->id = "empty-section";
    empty->isLiverySection = true;
    empty->liverySectionSlot = 1;
    project.root->append(std::move(empty));
    project.isLivery = true;
    h.reset(std::move(project));
    const QTransform before = gui::sceneWorldTransform(*h.state->sceneNode("inner"));
    require(h.drop({"inner"}, "untouched", false), "moving group to section root was rejected");
    require(h.state->parentGroupForEntry("inner") == "section", "drop escaped the section into the livery project root");
    sameWorld(before, gui::sceneWorldTransform(*h.state->sceneNode("inner")), "section root move");
    require(h.drop({"c"}, "inner"), "section shape reparent was rejected");
    require(h.state->sceneNode("source") == nullptr, "empty group was retained in a livery section");
    require(h.state->sceneNode("empty-section") != nullptr, "empty livery section was pruned");
    h.trigger("undo"); h.trigger("redo");
    h.validate();
    h.reset();
    QApplication::setActiveWindow(&h.window);
    h.select({"loose"});
    QKeyEvent remove(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(h.tree, &remove);
    require(h.actions.contains("delete_selected") && h.commands.contains(gui::EditorCommandType::Remove)
            && h.state->sceneNode("loose") == nullptr, "Delete shortcut bypassed state commands");
    QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    QApplication::sendEvent(h.tree, &undo);
    require(h.commands.contains(gui::EditorCommandType::Undo) && h.state->sceneNode("loose") != nullptr,
            "Undo shortcut bypassed state commands");
    h.validate();
    QApplication::setActiveWindow(nullptr);
}

void moveAutoSelection(Harness &h) {
    gui::ProjectCanvas *canvas = nullptr;
    for (QWidget *widget : h.window.findChildren<QWidget *>()) {
        if (auto *found = dynamic_cast<gui::ProjectCanvas *>(widget)) canvas = found;
    }
    require(canvas != nullptr, "canvas is missing");
    canvas->resize(1000, 600);
    const auto reset = [&]() {
        fls::Project project;
        fls::scene::ensureProjectSceneRoot(project);
        const auto make = [](const QString &id, double x, double y = 0.0) {
            auto node = shape(id, x);
            node->transform = {x, y, 0.25, 0.25, 0, 0};
            return node;
        };
        auto group = std::make_unique<fls::scene::Group>();
        group->id = "selection";
        group->append(make("left", -200));
        group->append(make("right", 200));
        project.root->append(std::move(group));
        project.root->append(make("overlap", 0));
        project.root->append(make("foreground", -200));
        auto outside = std::make_unique<fls::scene::Group>();
        outside->id = "outside-group";
        outside->append(make("outside-a", 800));
        outside->append(make("outside-b", 800, 100));
        project.root->append(std::move(outside));
        h.reset(std::move(project));
        canvas->setTool("move");
        canvas->setMoveToolAutoSelect(true);
        canvas->setAutomaticSnapping(false);
        canvas->setTransformRelativeMode(false);
        h.select({"selection"});
    };
    reset();
    gui::BoundsAccumulator bounds;
    const QRectF ink = canvas->shapeInkBounds(101);
    require(ink.isValid(), "Move selection test needs square geometry");
    for (const auto *node : gui::sceneShapeLeaves(*h.state->project()->root))
        bounds.add(gui::sceneWorldTransform(*node), ink);
    gui::CanvasCamera camera;
    camera.setViewBounds(bounds.bounds());
    constexpr double kRulerExtent = 28;
    camera.recompute(QPointF(kRulerExtent, kRulerExtent), canvas->width() - kRulerExtent, canvas->height() - kRulerExtent);
    const auto screen = [&](const QPointF &world) { return camera.worldToScreen(world); };
    for (auto modifiers : {Qt::KeyboardModifiers(), Qt::KeyboardModifiers(Qt::ShiftModifier), Qt::KeyboardModifiers(Qt::ControlModifier)}) {
        for (const QPointF point : {QPointF(), QPointF(-200, 0), QPointF(100, 0)}) {
            click(canvas, screen(point).toPoint(), modifiers);
            require(h.state->selectionEntries() == QVector<QString>{"selection"}
                    && h.state->selectedLayerIds() == QSet<QString>{"left", "right"},
                    "Move click inside the selection box changed the selection");
        }
    }
    for (const QPointF origin : {QPointF(), QPointF(100, 0)}) {
        reset();
        const QTransform before = gui::sceneWorldTransform(*h.state->sceneNode("selection"));
        const QTransform other = gui::sceneWorldTransform(*h.state->sceneNode("overlap"));
        const QPointF start = screen(origin), end = screen(origin + QPointF(30, 15));
        QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(canvas, &press);
        require(h.state->selectionEntries() == QVector<QString>{"selection"}, "Move press in a gap cleared the selection");
        QMouseEvent move(QEvent::MouseMove, end, canvas->mapToGlobal(end.toPoint()), Qt::NoButton, Qt::LeftButton, {});
        QApplication::sendEvent(canvas, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end.toPoint()), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(canvas, &release);
        require(h.state->selectionEntries() == QVector<QString>{"selection"}, "Move drag inside the box reselected another shape");
        sameWorld(before * QTransform::fromTranslate(30, 15), gui::sceneWorldTransform(*h.state->sceneNode("selection")), "Move protected selection drag");
        sameWorld(other, gui::sceneWorldTransform(*h.state->sceneNode("overlap")), "unselected overlap");
        require(h.state->undoStack_.size() == 1, "protected Move drag did not create one undo step");
        h.trigger("undo");
        sameWorld(before, gui::sceneWorldTransform(*h.state->sceneNode("selection")), "Move protected selection undo");
    }
    reset();
    click(canvas, screen(QPointF(800, 0)).toPoint());
    require(h.state->selectedLayerIds() == QSet<QString>{"outside-a"}, "Move did not select a shape outside the box");
    reset();
    click(canvas, screen(QPointF(800, 0)).toPoint(), Qt::ShiftModifier);
    require(h.state->selectionEntries() == QVector<QString>{"outside-group"}, "Move Shift-click did not select an outside group");
    reset();
    click(canvas, screen(QPointF(500, 0)).toPoint());
    require(h.state->selectedLayerIds().isEmpty(), "empty click outside the box did not deselect");
    reset();
    h.select({});
    click(canvas, screen(QPointF()).toPoint());
    require(h.state->selectedLayerIds() == QSet<QString>{"overlap"}, "Move without a selection failed to auto-select");
    reset();
    gui::EditorCommand rotate;
    rotate.type = gui::EditorCommandType::LocalTransforms;
    rotate.entries = {"selection", "overlap"};
    auto groupFrame = h.state->sceneNode("selection")->transform;
    groupFrame.rotation = 45;
    auto overlapFrame = h.state->sceneNode("overlap")->transform;
    overlapFrame.x = 100;
    rotate.transforms.insert("selection", groupFrame);
    rotate.transforms.insert("overlap", overlapFrame);
    require(h.state->executeCommand(rotate), "rotated selection fixture failed");
    canvas->setTransformRelativeMode(true);
    click(canvas, screen(QPointF()).toPoint());
    require(h.state->selectionEntries() == QVector<QString>{"selection"}, "rotated selection box did not protect its interior");
    click(canvas, screen(QPointF(100, 0)).toPoint());
    require(h.state->selectedLayerIds() == QSet<QString>{"overlap"}, "Move protected space outside the rotated selection box");
    h.validate();
}

void gradientSnapping(Harness &h) {
    gui::ProjectCanvas *canvas = nullptr;
    for (QWidget *widget : h.window.findChildren<QWidget *>()) {
        if (auto *found = dynamic_cast<gui::ProjectCanvas *>(widget)) canvas = found;
    }
    require(canvas != nullptr, "snapping test needs a canvas");
    canvas->resize(1000, 600);
    canvas->setTool("move");
    canvas->setMoveToolAutoSelect(false);
    canvas->setAutomaticSnapping(true);
    canvas->setTransformRelativeMode(false);
    const auto drag = [&](int movingShape, int targetShape, quint8 movingAlpha, quint8 targetAlpha, int count) {
        fls::Project project;
        fls::scene::ensureProjectSceneRoot(project);
        QVector<QString> selected;
        for (int index = 0; index < count; ++index) {
            auto moving = shape(QStringLiteral("moving-%1").arg(index), 0);
            moving->transform = {};
            moving->setVectorShape(movingShape);
            moving->color[3] = movingAlpha;
            selected.push_back(moving->id);
            project.root->append(std::move(moving));
        }
        auto target = shape("target", 150);
        target->transform = {150, 0, 1, 1, 0, 0};
        target->setVectorShape(targetShape);
        target->color[3] = targetAlpha;
        project.root->append(std::move(target));
        h.reset(std::move(project));
        h.select(selected);
        canvas->refitView();
        gui::BoundsAccumulator bounds;
        for (const auto *node : gui::sceneShapeLeaves(*h.state->project()->root))
            bounds.add(gui::sceneWorldTransform(*node), canvas->shapeInkBounds(node->shapeId));
        gui::CanvasCamera camera;
        camera.setViewBounds(bounds.bounds());
        constexpr double kRulerExtent = 28;
        camera.recompute({kRulerExtent, kRulerExtent}, canvas->width() - kRulerExtent, canvas->height() - kRulerExtent);
        const QPointF start = camera.worldToScreen({0, 0});
        const QPointF end = camera.worldToScreen({21.5, 0});
        QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(canvas, &press);
        QMouseEvent move(QEvent::MouseMove, end, canvas->mapToGlobal(end.toPoint()), Qt::NoButton, Qt::LeftButton, {});
        QApplication::sendEvent(canvas, &move);
        const double x = h.state->sceneNode(selected.front())->x;
        QMouseEvent release(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end.toPoint()), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(canvas, &release);
        return x;
    };
    require(std::abs(drag(101, 101, 255, 255, 1) - 22) < 1e-6, "opaque geometry lost edge snapping");
    require(std::abs(drag(211, 101, 255, 255, 1) - 21.5) < 1e-6, "moving gradient participated in snapping");
    require(std::abs(drag(101, 211, 255, 255, 1) - 21.5) < 1e-6, "stationary gradient became a snap target");
    require(std::abs(drag(101, 101, 128, 255, 1) - 21.5) < 1e-6, "moving translucent layer participated in snapping");
    require(std::abs(drag(101, 101, 255, 128, 1) - 21.5) < 1e-6, "stationary translucent layer became a snap target");
    require(std::abs(drag(211, 101, 255, 255, 1000) - 21.5) < 1e-6, "gradient selection was snapped");
    canvas->setAutomaticSnapping(false);
}

void liveRenderEntries(Harness &h) {
    auto project = fixture();
    project.isLivery = true;
    auto *source = static_cast<fls::scene::Group *>(project.root->children[0].get());
    auto *destination = static_cast<fls::scene::Group *>(project.root->children[1].get());
    source->isLiverySection = true;
    source->liverySectionSlot = 0;
    destination->isLiverySection = true;
    destination->liverySectionSlot = 1;
    h.reset(std::move(project));
    const auto check = [&]() {
        for (const auto &entry : h.state->renderEntries())
            sameWorld(gui::sceneWorldTransform(*entry.node), entry.worldTransform, "live full entry " + entry.nodeId);
        for (const auto &entry : h.state->activeRenderEntries()) {
            require(h.state->activeSectionId_.isEmpty() || entry.sectionGroupId == h.state->activeSectionId_,
                    "active entry section changed");
            sameWorld(gui::sceneWorldTransform(*entry.node), entry.worldTransform, "live active entry " + entry.nodeId);
        }
    };
    for (const QString &section : {QStringLiteral("source"), QStringLiteral("destination"), QString()}) {
        h.state->setActiveSectionId(section);
        h.state->activeRenderEntries();
        h.state->sceneNode("a")->x += 3;
        h.state->noteTransformLiveChanged({"a"});
        check();
        h.state->sceneNode("source")->rotation += 7;
        h.state->noteTransformLiveChanged({"source", "a"});
        check();
        h.state->sceneNode("d")->y -= 5;
        h.state->noteTransformLiveChanged({"d"});
        check();
    }
    h.state->setActiveSectionId("source");
    h.state->noteProjectGeometryChanged(false);
    check();
}

void cachedTransformDrags(Harness &h) {
    gui::ProjectCanvas *canvas = nullptr;
    for (QWidget *widget : h.window.findChildren<QWidget *>()) {
        if (auto *found = dynamic_cast<gui::ProjectCanvas *>(widget)) canvas = found;
    }
    require(canvas != nullptr, "transform checks need a canvas");
    gui::ShapeGeometryStore geometry;
    require(geometry.loadDefault(), "transform checks need geometry");
    canvas->resize(1000, 600);
    canvas->setAutomaticSnapping(false);
    canvas->setMoveToolAutoSelect(false);
    canvas->setTransformRelativeMode(true);
    for (bool grouped : {false, true}) {
        for (const QString &tool : {QStringLiteral("move"), QStringLiteral("rotate"), QStringLiteral("transform")}) {
            h.reset();
            h.select(grouped ? QVector<QString>{"source", "loose"} : QVector<QString>{"a", "b", "loose"});
            canvas->setTool(tool);
            canvas->refitView();
            gui::BoundsAccumulator bounds;
            for (const auto &entry : h.state->renderEntries()) {
                const QRectF local = entry.shape != nullptr ? canvas->shapeInkBounds(entry.shape->shapeId)
                    : gui::sceneLocalRect(*entry.node, geometry);
                bounds.add(entry.worldTransform, local);
            }
            gui::CanvasCamera camera;
            camera.setViewBounds(bounds.bounds());
            camera.recompute({28, 28}, canvas->width() - 28, canvas->height() - 28);
            QPointF start(620, 290);
            if (tool == "transform") {
                QPointF center;
                double width, height;
                QTransform frame;
                require(canvas->currentTransformBox(&center, &width, &height, &frame), "missing transform box");
                const QPointF corner = frame.inverted().map(center) + QPointF(width * 0.5, height * 0.5);
                start = camera.worldToScreen(frame.map(corner));
            }
            QHash<QString, QTransform> before;
            for (const auto *node : h.state->selectedLayers()) before.insert(node->id, gui::sceneWorldTransform(*node));
            const QByteArray original = fls::encodeProjectDocument(*h.state->project());
            QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
            QApplication::sendEvent(canvas, &press);
            for (const QPointF delta : {QPointF(12, 5), QPointF(25, 11)}) {
                const QPointF point = start + delta;
                QMouseEvent move(QEvent::MouseMove, point, canvas->mapToGlobal(point.toPoint()), Qt::NoButton, Qt::LeftButton, {});
                QApplication::sendEvent(canvas, &move);
                const QString first = before.cbegin().key();
                const QTransform motion = before.value(first).inverted() * gui::sceneWorldTransform(*h.state->sceneNode(first));
                require(!motion.isIdentity(), "drag did not change geometry");
                for (auto it = before.cbegin(); it != before.cend(); ++it)
                    sameWorld(it.value() * motion, gui::sceneWorldTransform(*h.state->sceneNode(it.key())), tool + " " + it.key());
                for (const auto &entry : h.state->renderEntries())
                    sameWorld(gui::sceneWorldTransform(*entry.node), entry.worldTransform, "drag render entry " + entry.nodeId);
                QPointF center;
                double width, height;
                QTransform frame;
                require(canvas->currentTransformBox(&center, &width, &height, &frame), "drag lost selection box");
                QPointF repeatedCenter;
                double repeatedWidth, repeatedHeight;
                QTransform repeatedFrame;
                canvas->currentTransformBox(&repeatedCenter, &repeatedWidth, &repeatedHeight, &repeatedFrame);
                require(center == repeatedCenter && width == repeatedWidth && height == repeatedHeight && frame == repeatedFrame,
                        "selection box changed between consumers");
            }
            QMouseEvent release(QEvent::MouseButtonRelease, start + QPointF(25, 11),
                                canvas->mapToGlobal((start + QPointF(25, 11)).toPoint()), Qt::LeftButton, Qt::NoButton, {});
            QApplication::sendEvent(canvas, &release);
            const QByteArray edited = fls::encodeProjectDocument(*h.state->project());
            h.trigger("undo");
            require(fls::encodeProjectDocument(*h.state->project()) == original, "cached drag undo changed the document");
            h.trigger("redo");
            require(fls::encodeProjectDocument(*h.state->project()) == edited, "cached drag redo changed the document");
        }
    }
}

void profileTransforms(Harness &h, const QString &outputPath) {
    gui::ProjectCanvas *canvas = nullptr;
    for (QWidget *widget : h.window.findChildren<QWidget *>()) {
        if (auto *found = dynamic_cast<gui::ProjectCanvas *>(widget)) canvas = found;
    }
    require(canvas != nullptr, "transform profile needs a canvas");
    canvas->resize(1000, 600);
    canvas->setAutomaticSnapping(false);
    canvas->setMoveToolAutoSelect(false);
    QJsonArray reports;
    for (const QString &scenario : {QStringLiteral("loose-1000"), QStringLiteral("loose-5000"),
                                    QStringLiteral("group-5000"), QStringLiteral("livery-1000")}) {
        for (const QString &tool : {QStringLiteral("move"), QStringLiteral("rotate")}) {
            const int count = scenario.contains("5000") ? 5000 : 1000;
            const bool livery = scenario.startsWith("livery");
            const bool grouped = scenario.startsWith("group");
            fls::Project project;
            fls::scene::ensureProjectSceneRoot(project);
            project.isLivery = livery;
            auto parent = std::make_unique<fls::scene::Group>();
            parent->id = "moving-parent";
            parent->transform = {10, 20, 1.2, 0.8, 23, 0.15};
            parent->isLiverySection = livery;
            parent->liverySectionSlot = 0;
            QVector<QString> selected;
            for (int index = 0; index < count; ++index) {
                auto node = shape(QStringLiteral("moving-%1").arg(index), index % 100 * 4);
                node->y = index / 100 * 4;
                selected.push_back(node->id);
                parent->append(std::move(node));
            }
            project.root->append(std::move(parent));
            if (livery) {
                auto inactive = std::make_unique<fls::scene::Group>();
                inactive->id = "inactive";
                inactive->isLiverySection = true;
                inactive->liverySectionSlot = 1;
                for (int index = 0; index < 9000; ++index)
                    inactive->append(shape(QStringLiteral("inactive-%1").arg(index), index));
                project.root->append(std::move(inactive));
            }
            h.reset(std::move(project));
            h.select(grouped ? QVector<QString>{"moving-parent"} : selected);
            canvas->setTool(tool);
            canvas->setTransformRelativeMode(true);
            canvas->refitView();
            const QPointF start(620, 290);
            QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
            QApplication::sendEvent(canvas, &press);
            const QTransform before = gui::sceneWorldTransform(*h.state->sceneNode(selected.front()));
            QElapsedTimer timer;
            timer.start();
            constexpr int samples = 30;
            for (int sample = 0; sample < samples; ++sample) {
                const QPointF point = start + QPointF(sample + 1, sample * 0.3 + 1);
                QMouseEvent move(QEvent::MouseMove, point, canvas->mapToGlobal(point.toPoint()), Qt::NoButton, Qt::LeftButton, {});
                QApplication::sendEvent(canvas, &move);
                h.state->activeRenderEntries();
                canvas->currentTransformBox(nullptr, nullptr, nullptr, nullptr);
                canvas->currentTransformBox(nullptr, nullptr, nullptr, nullptr);
            }
            const double milliseconds = timer.nsecsElapsed() / (1000000.0 * samples);
            require(gui::sceneWorldTransform(*h.state->sceneNode(selected.front())) != before,
                    "profile input did not transform the selection");
            QMouseEvent release(QEvent::MouseButtonRelease, start + QPointF(samples, 10),
                                canvas->mapToGlobal((start + QPointF(samples, 10)).toPoint()), Qt::LeftButton, Qt::NoButton, {});
            QApplication::sendEvent(canvas, &release);
            reports.append(QJsonObject{{"scenario", scenario}, {"tool", tool}, {"layers", count},
                                       {"milliseconds_per_update", milliseconds}});
        }
    }
    QFile output(outputPath);
    require(output.open(QIODevice::WriteOnly), "profile output creation");
    output.write(QJsonDocument(QJsonObject{{"cpu_transform_updates", reports}}).toJson());
}

void liningCaps() {
    QVector<gui::PenPoint> points{{{0, 0}, gui::PenPointKind::Hard}, {{100, 0}, gui::PenPointKind::Hard}};
    points.front().endCap = 0;
    points.back().endCap = 0;
    auto ribbon = gui::buildLiningRibbon(points, 20);
    require(std::abs(ribbon.boundingRect().left()) < 1e-5 && std::abs(ribbon.boundingRect().right() - 100) < 1e-5,
            "flat Lining caps extend beyond the endpoints");
    points.back().endCap = 2;
    ribbon = gui::buildLiningRibbon(points, 20);
    require(ribbon.contains(QPointF(115, 0)) && std::abs(ribbon.boundingRect().right() - 120) < 1e-5,
            "convex Lining cap has the wrong shape");
    points.back().endCap = -1;
    ribbon = gui::buildLiningRibbon(points, 20);
    require(!ribbon.contains(QPointF(99, 0)) && ribbon.contains(QPointF(95, 9)), "concave Lining cap did not remove the center");
    QString error;
    const auto loops = gui::cubicPathLoops(ribbon, &error);
    const auto contour = gui::buildPenContour(loops);
    require(error.isEmpty() && contour.valid(), QString("Lining cap conversion: %1; contour: %2; loops: %3; crossings: %4").arg(error, contour.error).arg(loops.size()).arg(contour.crossings.size()));
}

void toolSettingsControls(Harness &h) {
    gui::ProjectCanvas *canvas = nullptr;
    for (QWidget *widget : h.window.findChildren<QWidget *>()) {
        if (auto *found = dynamic_cast<gui::ProjectCanvas *>(widget)) canvas = found;
    }
    auto *panel = h.window.findChild<QScrollArea *>("ToolSettingsPanel");
    auto *toolbar = h.window.findChild<QToolBar *>("MainToolBar");
    auto *settingsBar = h.window.findChild<QToolBar *>("ToolSettingsBar");
    require(canvas && panel && toolbar && settingsBar, "Tool Settings strip is missing");
    require(!settingsBar->isHidden(), "Tool Settings was hidden while positioning its row");
    require(h.window.centralWidget() == canvas && h.window.toolBarArea(settingsBar) == Qt::TopToolBarArea
            && h.window.toolBarBreak(settingsBar), "Tool Settings must occupy a full window row below the toolbar");
    require(panel->minimumSizeHint().width() == 0 && panel->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding,
            "Tool Settings must fill its row independently of the controls' width");
    QApplication::processEvents();
    const auto compactHeight = [&]() {
        const auto *bar = panel->horizontalScrollBar();
        return panel->widget()->sizeHint().height() + (bar->maximum() > bar->minimum() ? bar->sizeHint().height() : 0);
    };
    require(panel->sizeHint().height() == compactHeight() && panel->sizePolicy().verticalPolicy() == QSizePolicy::Fixed,
            "Tool Settings reserves excess row height");
    const QByteArray originalLayout = h.window.saveState();
    QMainWindow legacyWindow;
    auto *legacyToolbar = legacyWindow.addToolBar("Tools");
    legacyToolbar->setObjectName("MainToolBar");
    auto *legacyPanel = legacyWindow.addToolBar("Tool Settings");
    legacyPanel->setObjectName("ToolSettingsBar");
    legacyPanel->hide();
    QSettings().setValue("layout/state", legacyWindow.saveState());
    {
        gui::MainWindow restored;
        auto *restoredSettings = restored.findChild<QToolBar *>("ToolSettingsBar");
        require(restoredSettings && restored.toolBarArea(restoredSettings) == Qt::TopToolBarArea
                && restored.toolBarBreak(restoredSettings) && !restoredSettings->isHidden(),
                "restoring an old layout misplaced or hid Tool Settings");
    }
    QSettings().remove("layout/state");
    require(h.window.restoreState(legacyWindow.saveState()), "legacy toolbar layout could not be restored");
    h.window.findChild<QAction *>("reset_layout")->trigger();
    require(h.window.toolBarBreak(settingsBar) && !settingsBar->isHidden(), "resetting the layout misplaced or hid Tool Settings");
    require(h.window.restoreState(originalLayout), "original toolbar layout could not be restored");
    const QSize toolbarSize = toolbar->sizeHint();
    canvas->setTool("lining");
    QApplication::processEvents();
    require(toolbar->sizeHint() == toolbarSize && !toolbar->isAncestorOf(panel),
            "Lining settings changed the tool toolbar size");
    require(panel->sizeHint().height() == compactHeight(), "Lining settings reserve excess row height");
    require(panel->widget()->layout()->spacing() <= 6, "Tool Settings controls have excessive spacing");
    for (auto *slider : panel->findChildren<QSlider *>()) {
        require(slider->maximumWidth() <= 60, "Tool Settings slider is too wide");
    }
    for (auto *field : panel->findChildren<QDoubleSpinBox *>()) {
        require(field->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed
                && field->parentWidget()->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed,
                "Tool Settings numeric fields must retain their natural width");
    }
    for (const QString &id : {"toggle_move_auto_select", "toggle_selection_flash", "toggle_pipette_auto_return",
            "toggle_auto_save_pen_paths", "toggle_auto_save_lining_paths", "toggle_transform_relative",
            "select_analytic_contour_fill", "select_compact_contour_fill"}) {
        auto *action = h.window.findChild<QAction *>(id);
        require(action && action->parent() == panel->widget(), "Tool Settings shortcut is missing: " + id);
        for (auto *menu : h.window.findChildren<QMenu *>()) {
            require(!menu->actions().contains(action), "Tool Settings option is duplicated in a menu: " + id);
        }
    }
    for (auto *menu : h.window.findChildren<QMenu *>()) {
        require(menu->title() != "Contour Fill", "Contour Fill menu still duplicates Tool Settings");
    }
    for (bool enabled : {false, true}) {
        auto behavior = gui::loadBehaviorSettings();
        behavior.selectShapesByBoundingBox = enabled;
        behavior.showSelectedShapeOutlines = enabled;
        behavior.displayAnchorsDuringTransformDrag = enabled;
        gui::SettingsDialog dialog(gui::loadUiTheme(), gui::loadCanvasColorSettings(), gui::loadPreviewBackgroundSettings(), behavior, {});
        for (auto *label : dialog.findChildren<QLabel *>()) {
            require(!QStringList{"Select shapes by bounding box", "Show selected-shape outlines", "Display anchors during transform drag"}.contains(label->text()),
                    "Settings dialog still duplicates a Tool Settings option: " + label->text());
        }
        const auto after = dialog.selectedBehaviorSettings();
        require(after.selectShapesByBoundingBox == enabled && after.showSelectedShapeOutlines == enabled
                && after.displayAnchorsDuringTransformDrag == enabled, "Settings dialog overwrites Tool Settings values");
    }
    const auto check = [&](const QString &key) {
        auto *field = h.window.findChild<QCheckBox *>("toolSettings." + key);
        require(field != nullptr, "Tool Settings checkbox is missing: " + key);
        return field;
    };
    const auto number = [&](const QString &key) {
        auto *field = h.window.findChild<QDoubleSpinBox *>("toolSettings." + key);
        require(field != nullptr, "Tool Settings number is missing: " + key);
        return field;
    };
    const auto choice = [&](const QString &key) {
        auto *field = h.window.findChild<QComboBox *>("toolSettings." + key);
        require(field != nullptr, "Tool Settings choice is missing: " + key);
        return field;
    };
    const auto figures = [&]() { return gui::sceneLeaves<fls::scene::ContourFigure, fls::scene::LayerKind::Contour>(*h.state->project()->root); };
    const auto newFigure = [&]() -> const fls::scene::ContourFigure * {
        for (const auto *figure : figures()) if (figure->id != "path") return figure;
        return nullptr;
    };
    h.reset();
    canvas->setTool("move");
    auto *move = check("moveToolAutoSelect");
    move->click();
    require(gui::loadBehaviorSettings().moveToolAutoSelect == move->isChecked()
            && canvas->moveToolAutoSelect() == move->isChecked()
            && h.window.findChild<QAction *>("toggle_move_auto_select")->isChecked() == move->isChecked(), "Move setting did not propagate");
    h.window.findChild<QAction *>("toggle_move_auto_select")->trigger();
    require(move->isChecked() == canvas->moveToolAutoSelect(), "Move shortcut did not update Tool Settings");
    canvas->setTool("select");
    require(!check("selectionFlashEnabled")->isHidden() && move->isHidden(), "Tool Settings shows controls for another tool");
    check("selectionFlashEnabled")->click();
    require(canvas->selectionFlashEnabled() == check("selectionFlashEnabled")->isChecked(), "selection flash did not propagate");
    canvas->setTool("transform");
    check("relativeMode")->click();
    require(gui::loadTransformModeSettings().relativeMode == check("relativeMode")->isChecked()
            && h.window.findChild<QAction *>("toggle_transform_relative")->isChecked() == check("relativeMode")->isChecked(), "relative mode did not synchronize");
    canvas->setTool("pipette");
    check("pipetteAutoReturn")->click();
    require(gui::loadBehaviorSettings().pipetteAutoReturn == check("pipetteAutoReturn")->isChecked(), "Pipette setting did not persist");
    canvas->setTool("bucket");
    auto *slider = h.window.findChild<QSlider *>("toolSettings.bucketTolerance.slider");
    slider->setValue(47);
    require(number("bucketTolerance")->value() == 47 && canvas->bucketTolerance() == 47, "Tolerance slider did not update the field and canvas");
    number("bucketTolerance")->setValue(61);
    require(slider->value() == 61 && canvas->bucketTolerance() == 61, "Tolerance field did not update the slider and canvas");
    QWheelEvent wheel(QPointF(200, 200), canvas->mapToGlobal(QPoint(200, 200)), {}, QPoint(0, 120), {}, {}, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas, &wheel);
    require(canvas->bucketTolerance() == 62 && number("bucketTolerance")->value() == 62 && slider->value() == 62,
            "canvas scrolling did not update Tool Settings");
    require(!h.window.findChild<QComboBox *>("toolSettings.bucketRegionMode"), "Bucket still exposes a region mode");
    canvas->setTool("pen");
    choice("contourFillMode")->setCurrentIndex(1);
    require(gui::loadBehaviorSettings().contourFillMode == gui::ContourFillMode::CompactFit, "Pen mode did not persist");
    h.window.findChild<QAction *>("select_analytic_contour_fill")->trigger();
    require(choice("contourFillMode")->currentIndex() == 0, "Pen mode shortcut did not update Tool Settings");
    if (!check("autoSavePenPaths")->isChecked()) check("autoSavePenPaths")->click();
    require(!gui::loadBehaviorSettings().autoSaveLiningPaths, "Pen Save Path changed Lining Save Path");
    h.reset(); canvas->setTool("pen"); canvas->resize(1000, 600);
    click(canvas, QPoint(200, 200)); click(canvas, QPoint(350, 200)); click(canvas, QPoint(350, 350));
    require(figures().size() == 1, "unfinished Pen path was saved");
    click(canvas, QPoint(200, 200));
    require(figures().size() == 2 && newFigure()->data.closed && h.commands.contains(gui::EditorCommandType::Insert), "Pen did not save on contour completion");
    h.trigger("undo");
    require(figures().size() == 1, "automatic Pen save undo failed");
    h.trigger("redo");
    require(figures().size() == 2, "automatic Pen save redo failed");
    h.reset(); canvas->setTool("lining");
    require(!h.window.findChild<QComboBox *>("toolSettings.liningFillMode"), "Lining still exposes alternative fill modes");
    number("liningWidth")->setValue(20);
    require(canvas->liningWidth() == 20, "Lining width did not propagate");
    number("liningOutsideMargin")->setValue(35);
    require(std::abs(gui::loadBehaviorSettings().liningOutsideMargin - 0.35) < 1e-9, "Lining margin did not propagate");
    const auto completeLine = [&]() {
        click(canvas, QPoint(200, 200)); click(canvas, QPoint(400, 200));
        const QPoint point(400, 200);
        QMouseEvent press(QEvent::MouseButtonPress, point, canvas->mapToGlobal(point), Qt::RightButton, Qt::RightButton, {});
        QApplication::sendEvent(canvas, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, point, canvas->mapToGlobal(point), Qt::RightButton, Qt::NoButton, {});
        QApplication::sendEvent(canvas, &release);
    };
    completeLine();
    require(figures().size() == 1, "disabled Lining Save Path still saved a line");
    check("autoSaveLiningPaths")->click();
    h.reset(); canvas->setTool("lining"); completeLine();
    require(figures().size() == 2 && newFigure()->data.pathKind == fls::scene::ContourPathKind::Lining, "Lining did not save on line completion");
    const QString id = newFigure()->id;
    const auto data = newFigure()->data;
    const double scale = 200.0 / QLineF(data.points.front().position, data.points.back().position).length();
    const double radius = gui::liningPointWidth(data.points.back(), data.strokeWidth) * scale * 0.5;
    const QPointF start = QPointF(400, 200) + QPointF(radius + 12, 0);
    const QPointF end = start - QPointF(radius * 2, 0);
    QMouseEvent press(QEvent::MouseButtonPress, start, canvas->mapToGlobal(start.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(canvas, &press);
    QMouseEvent moveCap(QEvent::MouseMove, end, canvas->mapToGlobal(end.toPoint()), Qt::NoButton, Qt::LeftButton, {});
    QApplication::sendEvent(canvas, &moveCap);
    QMouseEvent release(QEvent::MouseButtonRelease, end, canvas->mapToGlobal(end.toPoint()), Qt::LeftButton, Qt::NoButton, {});
    QApplication::sendEvent(canvas, &release);
    auto *figure = static_cast<const fls::scene::ContourFigure *>(h.state->sceneNode(id));
    require(figure->data.points.back().endCap < -0.9 && figure->data.points.back().position == data.points.back().position,
            "Lining cap handle did not change the cap independently of its endpoint");
    h.trigger("undo");
    require(static_cast<const fls::scene::ContourFigure *>(h.state->sceneNode(id))->data == data, "Lining cap undo failed");
    h.trigger("redo");
    auto restored = fls::decodeProjectDocument(fls::encodeProjectDocument(*h.state->project()));
    require(static_cast<const fls::scene::ContourFigure *>(nodeIn(*restored.root, id))->data.points.back().endCap < -0.9,
            "Lining cap was not saved in the project");
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, end, canvas->mapToGlobal(end.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(canvas, &doubleClick);
    require(static_cast<const fls::scene::ContourFigure *>(h.state->sceneNode(id))->data.points.back().endCap == 0,
            "double-click did not reset the Lining cap to flat");
    QImage image(256, 256, QImage::Format_ARGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawEllipse(QPointF(128, 128), 80, 80);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(128, 128), 15, 15);
    }
    fls::Project guideProject;
    fls::scene::ensureProjectSceneRoot(guideProject);
    auto guide = std::make_unique<fls::scene::GuideLayer>();
    guide->id = "bucket-guide";
    guide->image = std::make_unique<fls::scene::RasterContainer>();
    guide->image->width = guide->image->height = 256;
    guide->image->encoded = gui::encodeGuideImage(image, &guide->image->format);
    guideProject.root->append(std::move(guide));
    {
        h.reset(guideProject);
        h.select({"bucket-guide"});
        canvas->setTool("bucket");
        QSettings().setValue("ui/behavior/bucketRegionMode", 1);
        gui::CanvasCamera camera;
        camera.setViewBounds(QRectF(-128, -128, 256, 256));
        camera.recompute(QPointF(28, 28), canvas->width() - 28, canvas->height() - 28);
        click(canvas, camera.worldToScreen(QPointF(32, 0)).toPoint());
        const auto contours = figures();
        require(canvas->tool() == "pen" && contours.size() == 1, "Bucket did not produce a saved Pen contour");
        const auto &converted = contours.front()->data;
        require(converted.closed && converted.cutouts.size() == 1, "Bucket mode lost its contour or cutout");
        const auto hard = [](const gui::PenPoint &point) { return point.kind == gui::PenPointKind::Hard; };
        const bool polygonal = std::all_of(converted.points.begin(), converted.points.end(), hard);
        require(!polygonal, "Bucket retained the removed Polygon preference");
    }
}

void actions(Harness &h) {
    const QStringList expected{"undo", "redo", "copy", "cut", "paste", "group_ungroup", "ungroup_flat",
        "fold_all_groups", "delete_selected", "stamp", "flip_selection", "center_view_on_selection",
        "fit_view_to_visible_area", "save_pen_figure", "new_pen_contour", "align_top", "align_bottom",
        "align_left", "align_right", "align_centre", "align_vertical_centre", "distribute_vertical", "distribute_horizontal"};
    QStringList registered;
    for (QAction *action : h.window.findChildren<QAction *>()) {
        if (action->property("editorCommandAction").isValid()) registered.push_back(action->objectName());
    }
    QStringList sorted = expected;
    registered.sort(); sorted.sort();
    require(registered == sorted, "Edit action coverage is incomplete");
    for (const QString &id : expected) {
        h.reset();
        h.select(id.startsWith("align_") || id.startsWith("distribute_") ? QVector<QString>{"loose", "untouched", "d"}
            : id == "group_ungroup" ? QVector<QString>{"a", "b"}
            : id == "ungroup_flat" ? QVector<QString>{"source"} : QVector<QString>{"loose"});
        if (id == "paste") {
            gui::EditorCommand copy;
            copy.type = gui::EditorCommandType::Copy;
            copy.entries = {"loose"};
            require(h.state->executeCommand(copy), "copy before paste failed");
        }
        h.trigger(id);
        require(h.commands.contains(gui::EditorCommandType::Action), "Edit action did not execute its state event: " + id);
        if (id.startsWith("align_") || id.startsWith("distribute_") || id == "flip_selection")
            require(h.commands.contains(gui::EditorCommandType::LocalTransforms) && h.state->undoStack_.size() == 1,
                    "canvas edit bypassed state transform commands: " + id);
        if (id == "copy") require(h.state->clipboard() != nullptr, "Copy did not update state clipboard");
        if (id == "cut" || id == "delete_selected") require(h.state->sceneNode("loose") == nullptr, "edit action did not remove selected shape: " + id);
        if (id == "stamp" || id == "paste") require(gui::sceneShapeLeaves(*h.state->project()->root).size() == 8, "edit action did not insert a shape: " + id);
        if (id == "group_ungroup") require(h.state->parentGroupForEntry("a") == h.state->parentGroupForEntry("b")
            && h.state->parentGroupForEntry("a") != "inner", "Group did not create a new group");
        if (id == "ungroup_flat") require(h.state->sceneNode("source") == nullptr && h.state->parentGroupForEntry("a").isEmpty(), "flat ungroup did not rebuild parent structure");
        h.validate();
    }
    h.reset(); h.select({"loose"});
    const QByteArray beforeFlip = fls::encodeProjectDocument(*h.state->project());
    for (int step = 0; step < 4; ++step) h.trigger("flip_selection");
    require(beforeFlip == fls::encodeProjectDocument(*h.state->project()), "four flips did not restore the exact original transform");
    h.reset();
    gui::ProjectCanvas *canvas = nullptr;
    for (QWidget *widget : h.window.findChildren<QWidget *>()) {
        if (auto *found = dynamic_cast<gui::ProjectCanvas *>(widget)) canvas = found;
    }
    require(canvas != nullptr, "canvas is missing");
    h.trigger("new_pen_contour");
    canvas->resize(800, 600);
    click(canvas, QPoint(200, 200));
    click(canvas, QPoint(350, 280));
    click(canvas, QPoint(400, 180));
    const QByteArray beforeSave = fls::encodeProjectDocument(*h.state->project());
    h.trigger("save_pen_figure");
    require(h.commands.contains(gui::EditorCommandType::Insert), "Save path bypassed state insertion commands");
    h.validate();
    h.trigger("undo");
    require(beforeSave == fls::encodeProjectDocument(*h.state->project()), "Save path undo did not restore the document");
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("ForzaLiveryStudioTests");
    QCoreApplication::setApplicationName("GuiCommands-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QSettings().setValue("ui/behavior/autosaveIntervalMinutes", 0);
    QSettings().setValue("ui/behavior/pasteInPlace", true);
    gui::applyUiTheme(application, gui::loadUiTheme());
    try {
        Harness harness;
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--profile-transforms")) {
            profileTransforms(harness, QString::fromLocal8Bit(argv[2]));
            return 0;
        }
        flags(harness);
        properties(harness);
        reparent(harness);
        singleChildGroups(harness);
        rejectedMoves(harness);
        actions(harness);
        liveryMoves(harness);
        moveAutoSelection(harness);
        gradientSnapping(harness);
        liveRenderEntries(harness);
        cachedTransformDrags(harness);
        liningCaps();
        toolSettingsControls(harness);
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
    QTextStream(stdout) << "GUI command propagation, history, reparenting and draw order checks passed\n";
    return 0;
}
