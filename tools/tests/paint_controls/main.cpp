#include "car_paint_catalog.h"
#include "car_preview_widget.h"
#include "editor_state.h"
#include "material_hashes.h"
#include "paint_color_popup.h"

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>

#include <cstdio>
#include <stdexcept>
#include <limits>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Widget>
Widget *control(QObject &parent, const QString &name) {
    auto *widget = parent.findChild<Widget *>(name);
    require(widget != nullptr, qPrintable(QStringLiteral("Missing control: ") + name));
    return widget;
}

void chooseMaterial(gui::CarPreviewWidget &preview, quint32 code) {
    auto *combo = control<QComboBox>(preview, QStringLiteral("paintMaterial.Body"));
    const int index = combo->findData(code);
    require(index >= 0, "material not offered");
    combo->setCurrentIndex(index);
    QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
}

gui::PaintColorPopup *openColor(gui::CarPreviewWidget &preview, bool secondary = false) {
    const QString name = secondary ? QStringLiteral("paintSecondary.Body") : QStringLiteral("paintPrimary.Body");
    control<QPushButton>(preview, name)->click();
    return control<gui::PaintColorPopup>(preview, QStringLiteral("paintColorPopup"));
}

void closeColor(gui::PaintColorPopup *popup, bool accepted) {
    control<QPushButton>(*popup, accepted ? QStringLiteral("paintColorDone") : QStringLiteral("paintColorCancel"))->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QColor bodyColor(const gui::EditorState &state, bool secondary = false) {
    const auto *material = state.project_.liveryPaint.find(fls::material_hashes::binding::kBodyPaint);
    require(material != nullptr, "body paint missing");
    const auto &color = secondary ? material->secondary : material->primary;
    require(color.enabled, "paint channel disabled");
    return QColor(color.bgra[2], color.bgra[1], color.bgra[0]);
}

fls::CarMesh triangle(quint64 hash) {
    fls::CarMesh mesh;
    mesh.paintMaterialHash = hash;
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.indices = {0, 1, 2};
    return mesh;
}

void loadTestCar(gui::CarPreviewWidget &preview, const QString &path = qEnvironmentVariable("FLS_PAINT_TEST_MODEL")) {
    QEventLoop loop;
    bool loaded = false;
    bool finished = false;
    QString error;
    preview.loadCarAsync(path, [&](bool success, const QString &message) {
        loaded = success;
        finished = true;
        error = message;
        loop.quit();
    });
    if (!finished) loop.exec();
    require(loaded, qPrintable(error));
    QStringList names;
    for (auto *combo : preview.findChildren<QComboBox *>()) {
        if (combo->objectName().startsWith("paintMaterial.")) names.push_back(combo->objectName().mid(14));
    }
    std::printf("Active paint slots: %s\n", qPrintable(names.join(", ")));
}

void testSurfaceSlots() {
    using namespace fls::material_hashes::binding;
    fls::LiveryPaintState paint;
    for (quint64 hash : kLiveryMaterials) paint.ensure(hash);
    paint.ensure(424242);
    fls::CarModel model;
    model.meshes.push_back(triangle(kBodyPaint));
    auto degenerate = triangle(kMirrorPaint);
    degenerate.positions[2] = {2, 0, 0};
    model.meshes.push_back(degenerate);
    auto unused = degenerate;
    unused.paintMaterialHash = kWindowGlass;
    unused.positions.push_back({0, 1, 0});
    model.meshes.push_back(unused);
    auto collapsed = triangle(kSpoilerPaint);
    collapsed.boneTransform.m[0] = collapsed.boneTransform.m[5] = 0;
    model.meshes.push_back(collapsed);
    auto invalid = triangle(kTrim1);
    invalid.indices[2] = 999;
    model.meshes.push_back(invalid);
    auto empty = triangle(kBrakeCaliper);
    empty.indices.clear();
    model.meshes.push_back(empty);
    auto interior = triangle(kOverride1);
    interior.drawGroups = fls::car_draw_groups::kCockpit;
    model.meshes.push_back(interior);
    auto sticker = triangle(kOverride2);
    sticker.materialName = "livery_sticker";
    model.meshes.push_back(sticker);
    auto rotor = triangle(kBrakeCaliper);
    rotor.name = "brake_disc";
    model.meshes.push_back(rotor);
    auto nonfinite = triangle(kOverride8);
    nonfinite.positions[0].x = std::numeric_limits<float>::quiet_NaN();
    model.meshes.push_back(nonfinite);
    auto tiny = triangle(kOverride9);
    tiny.positions[1].x = tiny.positions[2].y = 1e-20f;
    model.meshes.push_back(tiny);
    auto hood = triangle(kHoodPaint);
    hood.carPartType = fls::car_part_types::Hood;
    hood.partOptionIds = {11};
    model.meshes.push_back(hood);
    auto variant = triangle(kTrim2);
    variant.stockPart = false;
    variant.carPartType = fls::car_part_types::Hood;
    variant.partOptionIds = {12};
    model.variantMeshes.push_back(variant);
    const auto hashes = [](const auto &paintSlots) {
        QSet<quint64> result;
        for (const auto &slot : paintSlots) result.insert(slot.hash);
        return result;
    };
    require(hashes(gui::carPaintSlots(model, paint)) == QSet<quint64>{kBodyPaint, kHoodPaint, kOverride9},
            "unused, hidden, or zero-area paint slot exposed");
    require(hashes(gui::carPaintSlots(model, paint, 0, {{fls::car_part_types::Hood, 12}}))
                == QSet<quint64>{kBodyPaint, kTrim2, kOverride9}, "part selection ignored for slot area");
    model.additionalLodMeshes.push_back({triangle(kOverride3)});
    require(hashes(gui::carPaintSlots(model, paint, 1)) == QSet<quint64>{kOverride3}, "inactive LOD supplied slots");
    model = {};
    auto front = triangle(kFrontWheelPaint[2]);
    front.materialName = "inner_rim";
    front.sourceModelPath = "cars/_library/scene/wheels/test.modelbin";
    model.meshes.push_back(front);
    require(hashes(gui::carPaintSlots(model, paint)) == QSet<quint64>{kFrontWheelPaint[2]}, "wheel fallback aliases exposed as slots");
    auto secondaryRim = triangle(kBodyPaint);
    secondaryRim.materialName = "rim2";
    secondaryRim.sourceModelPath = front.sourceModelPath;
    model.meshes.push_back(secondaryRim);
    require(hashes(gui::carPaintSlots(model, paint)) == QSet<quint64>{kFrontWheelPaint[2], kRims2},
            "secondary rim used a body or primary-rim binding");
    paint.game = "FH5";
    require(hashes(gui::carPaintSlots(model, paint)) == QSet<quint64>{kRims}, "positional wheel slot lost its surface");
}

void testNoModel() {
    gui::EditorState state;
    fls::Project project;
    project.isLivery = true;
    project.liveryPaint.ensure(fls::material_hashes::binding::kBodyPaint);
    state.setProject(std::move(project));
    gui::CarPreviewWidget preview;
    preview.setEditorState(&state);
    preview.setProject(state.project());
    require(preview.findChildren<QComboBox *>().isEmpty(), "slots exposed before model geometry was available");
}

void testCatalog() {
    const auto fh5 = gui::carPaintFinishes(QStringLiteral("FH5"));
    const auto fh6 = gui::carPaintFinishes(QStringLiteral("FH6"));
    const auto count = [](const auto &finishes, int code) {
        for (const auto &finish : finishes) if (finish.code == code) return finish.colorCount;
        return -1;
    };
    require(count(fh5, 4) == 2 && count(fh6, 4) == 2, "metallic secondary metadata lost");
    require(count(fh5, 70) == 0 && count(fh6, 70) == 2, "title-specific finish metadata lost");
    const auto special = [](const auto &finishes, int code) {
        for (const auto &finish : finishes) if (finish.code == code) return finish.special;
        throw std::runtime_error("finish classification missing");
    };
    require(!special(fh5, 4) && !special(fh6, 51) && !special(fh6, 70), "generic paint placed in Special");
    require(special(fh5, 70) && special(fh6, 7) && special(fh6, 12) && special(fh6, 20),
            "special material placed among generic paints");
    fls::LiveryPaintState paint;
    for (quint64 hash : fls::material_hashes::binding::kLiveryMaterials) paint.ensure(hash);
    require(gui::carPaintSlots({}, paint).isEmpty(), "saved paint records supplied slot area");
    fls::CarModel model;
    for (quint64 hash : fls::material_hashes::binding::kLiveryMaterials) model.meshes.push_back(triangle(hash));
    const auto paintSlots = gui::carPaintSlots(model, paint);
    require(paintSlots.size() == fls::material_hashes::binding::kLiveryMaterials.size(), "paint slots were merged");
    const auto colors = gui::carManufacturerColors(QStringLiteral("FH5"), 460,
        fls::material_hashes::binding::kBodyPaint, false);
    require(colors.size() == 6 && colors[0].color.isValid(), "native manufacturer palette unavailable");
}

void testPaintControls() {
    using namespace fls::material_hashes::binding;
    gui::EditorState state;
    fls::Project project;
    project.isLivery = true;
    project.carId = 460;
    project.liveryPaint.game = QStringLiteral("FH5");
    project.colorSwatches.push_back({30, 20, 10, 255});
    project.liveryPaint.setColorBgra(kBodyPaint, false, {0, 0, 255, 255});
    project.liveryPaint.ensure(kBodyPaint).finish = 1;
    project.liveryPaint.setColorBgra(kHoodPaint, false, {255, 0, 0, 255});
    project.liveryPaint.ensure(kHoodPaint).finish = 1;
    state.setProject(std::move(project));
    gui::CarPreviewWidget preview;
    preview.setEditorState(&state);
    preview.setProject(state.project());
    loadTestCar(preview);
    require(control<QPushButton>(preview, QStringLiteral("paintPrimary.Rims"))->property("paintColor").value<QColor>() != preview.basePaint(),
            "unpainted rims inherited the body's button color");
    for (auto *button : preview.findChildren<QToolButton *>())
        require(button->text() != "PARTS" && button->text() != "LOD", "obsolete preview page still wired");
    auto *secondary = control<QPushButton>(preview, QStringLiteral("paintSecondary.Body"));
    require(secondary->isHidden(), "secondary shown for gloss");
    chooseMaterial(preview, 4);
    require(!secondary->isHidden(), "secondary missing for metallic");
    auto *combo = control<QComboBox>(preview, QStringLiteral("paintMaterial.Body"));
    combo->showPopup();
    auto *menu = control<QMenu>(*combo, QStringLiteral("paintMaterialMenu"));
    auto *special = control<QMenu>(*menu, QStringLiteral("paintSpecialMenu"));
    require(menu->actions().last()->text() == "Special" && menu->actions().last()->menu() == special,
            "Special is not the final cascading menu entry");
    QAction *carbon = nullptr;
    bool metallicOffered = false;
    for (auto *action : menu->actions()) {
        if (action->menu()) continue;
        const int index = combo->findData(action->data());
        require(index >= 0 && !combo->itemData(index, Qt::UserRole + 2).toBool(), "special finish exposed in root menu");
        if (action->data().toUInt() == 4) {
            metallicOffered = true;
            require(action->isChecked(), "selected generic finish not checked");
        }
    }
    require(metallicOffered, "generic material absent from dropdown");
    for (auto *action : special->actions()) {
        const int index = combo->findData(action->data());
        require(index >= 0 && combo->itemData(index, Qt::UserRole + 2).toBool(), "generic finish exposed in Special");
        if (action->data().toUInt() == 7) carbon = action;
    }
    require(carbon != nullptr, "tintable textured finish absent from Special");
    carbon->trigger();
    require(state.project()->liveryPaint.find(kBodyPaint)->finish == 7, "submenu choice did not update paint");
    require(control<QPushButton>(preview, QStringLiteral("paintPrimary.Body"))->isHidden() && secondary->isHidden(),
            "special material exposed color buttons");
    menu->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    chooseMaterial(preview, 6);
    require(control<QPushButton>(preview, QStringLiteral("paintPrimary.Body"))->isHidden(), "self-colored material exposed a color button");
    chooseMaterial(preview, 51);
    require(!control<QPushButton>(preview, QStringLiteral("paintPrimary.Body"))->isHidden() && !secondary->isHidden(),
            "color buttons missing after returning to two-tone");
    const auto startingPaint = state.project()->liveryPaint;
    state.undoStack_.clear();
    int paintUpdates = 0;
    int geometryUpdates = 0;
    QObject::connect(&state, &gui::EditorState::projectPaintChanged, [&]() { ++paintUpdates; });
    QObject::connect(&state, &gui::EditorState::projectGeometryChanged, [&]() { ++geometryUpdates; });
    auto *popup = openColor(preview);
    control<QSpinBox>(*popup, QStringLiteral("paintColorR"))->setValue(64);
    control<QSpinBox>(*popup, QStringLiteral("paintColorG"))->setValue(128);
    control<QSpinBox>(*popup, QStringLiteral("paintColorB"))->setValue(192);
    require(bodyColor(state) == QColor(64, 128, 192), "RGB edits did not reach live paint");
    require(preview.basePaint() == bodyColor(state), "preview base paint stayed stale");
    require(control<QPushButton>(preview, QStringLiteral("paintPrimary.Body"))->property("paintColor").value<QColor>() == bodyColor(state),
            "row color stayed stale");
    auto *square = control<QWidget>(*popup, QStringLiteral("paintColorSquare"));
    const QPointF corner(square->width() - 1, square->height() - 1);
    QMouseEvent squarePress(QEvent::MouseButtonPress, corner, square->mapToGlobal(corner.toPoint()), Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(square, &squarePress);
    require(bodyColor(state) == QColor(Qt::black), "color square did not reach live paint");
    auto *hex = control<QLineEdit>(*popup, QStringLiteral("paintColorHex"));
    hex->setText(QStringLiteral("#12AB34"));
    QMetaObject::invokeMethod(hex, "textEdited", Qt::DirectConnection, Q_ARG(QString, hex->text()));
    require(bodyColor(state) == QColor("#12AB34"), "hex edits did not reach live paint");
    control<QSpinBox>(*popup, QStringLiteral("paintColorH"))->setValue(120);
    control<QSpinBox>(*popup, QStringLiteral("paintColorS"))->setValue(100);
    control<QSpinBox>(*popup, QStringLiteral("paintColorV"))->setValue(100);
    require(bodyColor(state) == QColor(Qt::green), "HSV edits did not reach live paint");
    control<QPushButton>(*popup, QStringLiteral("paintSwatchColor.0"))->click();
    require(bodyColor(state) == QColor(10, 20, 30), "swatch did not reach live paint");
    require(state.undoStack_.isEmpty(), "live picker changes created undo entries");
    require(paintUpdates > 4 && geometryUpdates == 0, "paint changes used geometry invalidation");
    closeColor(popup, true);
    require(state.undoStack_.size() == 1, "popup did not commit one undo entry");
    const auto acceptedPaint = state.project()->liveryPaint;
    require(acceptedPaint.find(kHoodSecondaryPaint) == nullptr, "body edit created an unavailable paint slot");
    require(acceptedPaint.find(kHoodPaint)->primary.bgra == startingPaint.find(kHoodPaint)->primary.bgra,
            "body edit changed explicit hood paint");
    popup = openColor(preview, true);
    control<QPushButton>(*popup, QStringLiteral("paintManufacturerColor.0"))->click();
    require(bodyColor(state) == QColor(10, 20, 30), "secondary picker changed primary");
    require(bodyColor(state, true) != bodyColor(state), "manufacturer preset did not change secondary");
    closeColor(popup, false);
    require(state.project()->liveryPaint == acceptedPaint && state.undoStack_.size() == 1, "cancel did not restore starting paint");
    state.undo();
    require(state.project()->liveryPaint == startingPaint, "undo did not restore popup paint");
    state.redo();
    require(state.project()->liveryPaint == acceptedPaint, "redo did not restore popup paint");
    popup = openColor(preview);
    popup->setColor(QColor(Qt::red));
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(control<QLineEdit>(*popup, QStringLiteral("paintColorHex")), &escape);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(state.project()->liveryPaint == acceptedPaint && state.undoStack_.size() == 1, "Escape did not cancel focused popup");
    popup = openColor(preview);
    popup->setColor(QColor(Qt::green));
    popup->hide();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(bodyColor(state) == QColor(Qt::green) && state.undoStack_.size() == 2, "outside dismissal did not commit popup");
}

void testManufacturerEditing() {
    using namespace fls::material_hashes::binding;
    gui::EditorState state;
    fls::Project project;
    project.isLivery = true;
    project.carId = 368;
    project.liveryPaint.game = QStringLiteral("FH5");
    project.liveryPaint.setColorBgra(kBodyPaint, false, {0, 0, 255, 255});
    project.liveryPaint.ensure(kBodyPaint).manufacturerSelector = 0;
    state.setProject(std::move(project));
    gui::CarPreviewWidget preview;
    preview.setEditorState(&state);
    preview.setProject(state.project());
    loadTestCar(preview);
    const QColor factory = control<QPushButton>(preview, QStringLiteral("paintPrimary.Body"))->property("paintColor").value<QColor>();
    require(factory.rgb() != QColor(Qt::red).rgb(), "stored color displaced manufacturer override in controls");
    chooseMaterial(preview, 3);
    require(bodyColor(state).rgb() == factory.rgb(), "finish edit discarded visible manufacturer color");
    state.undo();
    require(state.project()->liveryPaint.find(kBodyPaint)->manufacturerSelector == 0, "undo lost manufacturer selector");
    state.project()->liveryPaint.ensure(kBodyPaint).finish = 51;
    state.noteProjectPaintChanged();
    auto *popup = openColor(preview, true);
    popup->setColor(QColor(Qt::blue));
    require(bodyColor(state).rgb() == factory.rgb(), "secondary edit discarded visible manufacturer primary");
    closeColor(popup, true);
}

void testModelSwitch(const QString &first, const QString &second) {
    using namespace fls::material_hashes::binding;
    gui::EditorState state;
    fls::Project project;
    project.isLivery = true;
    project.liveryPaint.game = QStringLiteral("FH5");
    project.liveryPaint.setColorBgra(kBodyPaint, false, {60, 30, 90, 255});
    project.liveryPaint.ensure(kHoodPaint).finish = 31;
    state.setProject(std::move(project));
    gui::CarPreviewWidget preview;
    preview.setEditorState(&state);
    preview.setProject(state.project());
    loadTestCar(preview, first);
    chooseMaterial(preview, 8);
    const auto paint = state.project()->liveryPaint;
    const auto history = state.undoStack_.size();
    for (const QString &path : {second, first}) {
        loadTestCar(preview, path);
        const auto *combo = control<QComboBox>(preview, QStringLiteral("paintMaterial.Body"));
        require(combo->currentData().toUInt() == 8 && state.project()->liveryPaint == paint
            && state.undoStack_.size() == history, "model switch edited the livery paint or lost its selection");
    }
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "minimal");
    QApplication app(argc, argv);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--model") qputenv("FLS_PAINT_TEST_MODEL", QByteArray(argv[2]));
    try {
        testCatalog();
        testSurfaceSlots();
        testNoModel();
        if (!qEnvironmentVariableIsEmpty("FLS_PAINT_TEST_MODEL")) {
            testPaintControls();
            testManufacturerEditing();
        }
        if (argc == 4 && app.arguments()[1] == QStringLiteral("--switch-models")) {
            testModelSwitch(app.arguments()[2], app.arguments()[3]);
        }
        std::puts("Paint catalog, live controls, popup undo and cancellation checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
