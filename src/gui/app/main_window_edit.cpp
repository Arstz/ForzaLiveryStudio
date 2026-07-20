#include "main_window.h"

#include "main_window_internal.h"

#include "clipboard_buffer_widget.h"
#include "color_palette_widget.h"
#include "image_io.h"
#include "layer_tree_view.h"
#include "perf_utils.h"
#include "property_panel.h"
#include "shape_registry.h"
#include "shapes_browser_widget.h"
#include "matrix_math.h"

#include <QtGui>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>

namespace gui {

using namespace mw_detail;

namespace {

fh6::Matrix3 generatedShapeMatrix(const QTransform &transform)
{
    fh6::Matrix3 matrix;
    matrix.m[0][0] = transform.m11();
    matrix.m[0][1] = transform.m21();
    matrix.m[0][2] = transform.dx();
    matrix.m[1][0] = transform.m12();
    matrix.m[1][1] = transform.m22();
    matrix.m[1][2] = transform.dy();
    return matrix;
}

} // namespace

void MainWindow::startPenFill(const QVector<PenPoint> &points,
                              const std::optional<QColor> &fillColor)
{
    if (!ensureProjectForInsertion() || canvas_ == nullptr) {
        if (canvas_ != nullptr) {
            canvas_->setPenFillRunning(false);
        }
        return;
    }
    cancelGeneratedFill();
    updateLastSelectedShapeDefaults();
    const BehaviorSettings behavior = loadBehaviorSettings();
    if (fillColor.has_value() && fillColor->isValid()) {
        generatedFillColor_ = {
            static_cast<quint8>(fillColor->blue()),
            static_cast<quint8>(fillColor->green()),
            static_cast<quint8>(fillColor->red()),
            static_cast<quint8>(fillColor->alpha()),
        };
    } else {
        generatedFillColor_ = behavior.insertShapeWithLastSelectedColor && haveLastSelectedShapeDefaults_
            ? lastSelectedShapeColor_
            : std::array<quint8, 4>{255, 255, 255, 255};
    }
    generatedFillInsertionEntries_ = selectedEntryIds();
    generatedFillLabel_ = QStringLiteral("Pen fill");
    generatedFillTool_ = QStringLiteral("pen");

    PenFillRequest request;
    request.points = points;
    request.primitives = canvas_->penPrimitiveCatalog();
    if (request.primitives.isEmpty()) {
        canvas_->setPenFillRunning(false);
        generatedFillInsertionEntries_.clear();
        generatedFillLabel_.clear();
        generatedFillTool_.clear();
        statusBar()->showMessage(QStringLiteral("Pen fill failed: Primitive geometry is unavailable"), 4000);
        return;
    }

    const quint64 generation = ++generatedFillGeneration_;
    const auto token = std::make_shared<std::atomic_bool>(false);
    generatedFillCancel_ = token;
    canvas_->setPenFillRunning(true, QStringLiteral("Filling Pen path…"));
    statusBar()->showMessage(QStringLiteral("Filling Pen path… Press Esc to cancel"));

    QPointer<MainWindow> guard(this);
    auto *task = QRunnable::create([guard, generation, request = std::move(request), token]() mutable {
        PenFillResult result = fillPenPath(request, [token]() {
            return token->load(std::memory_order_relaxed);
        });
        if (guard.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(guard.data(),
                                  [guard, generation, result = std::move(result)]() mutable {
                                      if (!guard.isNull()) {
                                          guard->finishGeneratedFill(generation, std::move(result));
                                      }
                                  },
                                  Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void MainWindow::startLiningFill(const QVector<PenPoint> &points,
                                 double width,
                                 const std::optional<QColor> &fillColor)
{
    if (!ensureProjectForInsertion() || canvas_ == nullptr) {
        if (canvas_ != nullptr) {
            canvas_->setLiningFillRunning(false);
        }
        return;
    }
    cancelGeneratedFill();
    updateLastSelectedShapeDefaults();
    const BehaviorSettings behavior = loadBehaviorSettings();
    if (fillColor.has_value() && fillColor->isValid()) {
        generatedFillColor_ = {
            static_cast<quint8>(fillColor->blue()),
            static_cast<quint8>(fillColor->green()),
            static_cast<quint8>(fillColor->red()),
            static_cast<quint8>(fillColor->alpha()),
        };
    } else {
        generatedFillColor_ = behavior.insertShapeWithLastSelectedColor && haveLastSelectedShapeDefaults_
            ? lastSelectedShapeColor_
            : std::array<quint8, 4>{255, 255, 255, 255};
    }
    generatedFillInsertionEntries_ = selectedEntryIds();
    generatedFillLabel_ = QStringLiteral("Lining fill");
    generatedFillTool_ = QStringLiteral("lining");

    LiningFillRequest request;
    request.points = points;
    request.width = width;
    request.primitives = canvas_->liningPrimitiveCatalog();
    if (request.primitives.isEmpty()) {
        canvas_->setLiningFillRunning(false);
        generatedFillInsertionEntries_.clear();
        generatedFillLabel_.clear();
        generatedFillTool_.clear();
        statusBar()->showMessage(QStringLiteral("Lining fill failed: Primitive geometry is unavailable"), 4000);
        return;
    }

    const quint64 generation = ++generatedFillGeneration_;
    const auto token = std::make_shared<std::atomic_bool>(false);
    generatedFillCancel_ = token;
    canvas_->setLiningFillRunning(true, QStringLiteral("Filling lining path…"));
    statusBar()->showMessage(QStringLiteral("Filling lining path… Press Esc to cancel"));

    QPointer<MainWindow> guard(this);
    auto *task = QRunnable::create([guard, generation, request = std::move(request), token]() mutable {
        PenFillResult result = fillLiningPath(request, [token]() {
            return token->load(std::memory_order_relaxed);
        });
        if (guard.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(guard.data(),
                                  [guard, generation, result = std::move(result)]() mutable {
                                      if (!guard.isNull()) {
                                          guard->finishGeneratedFill(generation, std::move(result));
                                      }
                                  },
                                  Qt::QueuedConnection);
    });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void MainWindow::cancelGeneratedFill()
{
    if (generatedFillCancel_ == nullptr) {
        return;
    }
    generatedFillCancel_->store(true, std::memory_order_relaxed);
    generatedFillCancel_.reset();
    ++generatedFillGeneration_;
    generatedFillInsertionEntries_.clear();
    if (canvas_ != nullptr) {
        canvas_->setPenFillRunning(false);
        canvas_->setLiningFillRunning(false);
    }
    statusBar()->showMessage(QStringLiteral("%1 cancelled").arg(generatedFillLabel_), 1500);
    generatedFillLabel_.clear();
    generatedFillTool_.clear();
}

void MainWindow::finishGeneratedFill(quint64 generation, PenFillResult result)
{
    if (generation != generatedFillGeneration_ || generatedFillCancel_ == nullptr) {
        return;
    }
    generatedFillCancel_.reset();
    if (canvas_ != nullptr) {
        if (generatedFillTool_ == QStringLiteral("lining")) {
            canvas_->setLiningFillRunning(false);
        } else {
            canvas_->setPenFillRunning(false);
        }
    }
    if (result.cancelled) {
        statusBar()->showMessage(QStringLiteral("%1 cancelled").arg(generatedFillLabel_), 1500);
        generatedFillInsertionEntries_.clear();
        generatedFillLabel_.clear();
        generatedFillTool_.clear();
        return;
    }
    if (!result.error.isEmpty() || result.placements.isEmpty() || !state_->hasProject()) {
        statusBar()->showMessage(QStringLiteral("%1 failed: %2")
                                     .arg(generatedFillLabel_)
                                     .arg(result.error.isEmpty() ? QStringLiteral("no shapes generated") : result.error),
                                 5000);
        generatedFillInsertionEntries_.clear();
        generatedFillLabel_.clear();
        generatedFillTool_.clear();
        return;
    }

    QVector<QPair<int, QTransform>> placements;
    placements.reserve(result.placements.size());
    for (const PenPlacement &placement : result.placements) {
        placements.push_back({placement.shapeId, placement.transform});
    }
    const bool lining = generatedFillTool_ == QStringLiteral("lining");
    const QString groupName = lining ? QStringLiteral("Lining") : QStringLiteral("Pen Fill");
    insertGeneratedFill(groupName, groupName, placements);
    if (canvas_ != nullptr) {
        if (lining) {
            canvas_->cancelLiningInteraction();
        } else {
            canvas_->cancelPenInteraction();
        }
    }
    generatedFillLabel_.clear();
    generatedFillTool_.clear();
}

void MainWindow::insertGeneratedFill(const QString &groupName,
                                     const QString &displayName,
                                     const QVector<QPair<int, QTransform>> &placements)
{
    if (placements.isEmpty() || !state_->hasProject()) {
        generatedFillInsertionEntries_.clear();
        return;
    }

    auto group = std::make_unique<fh6::scene::Group>();
    group->id = state_->uniqueGroupId();
    const QString groupId = group->id;
    group->name = groupName;
    QSet<QString> generatedIds;
    generatedIds.reserve(placements.size());
    for (const auto &placement : placements) {
        auto shape = std::make_unique<fh6::scene::Shape>();
        shape->id = QStringLiteral("layer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        shape->name = fh6::detail::shapeName(static_cast<quint16>(placement.first));
        shape->setVectorShape(static_cast<quint16>(placement.first));
        shape->transform = fh6::decomposeTransform2D(generatedShapeMatrix(placement.second));
        shape->color = generatedFillColor_;
        generatedIds.insert(shape->id);
        group->append(std::move(shape));
    }

    state_->beginProjectEdit();
    state_->insertLayerAboveSelection(std::move(group), generatedFillInsertionEntries_);
    if (fh6::scene::Group *inserted = state_->groupForId(groupId); inserted != nullptr) {
        const QString parentId = state_->parentGroupForEntry(groupId);
        if (const fh6::scene::Group *parent = state_->groupForId(parentId); parent != nullptr) {
            const fh6::Matrix3 parentInverse = fh6::invertAffine(parent->worldMatrix());
            for (const auto &child : inserted->children) {
                child->transform = fh6::decomposeTransform2D(
                    fh6::detail::multiply(parentInverse, child->transform.matrix()));
            }
        }
    }
    state_->selectedLayerIds_ = generatedIds;
    state_->selectedGuideLayerIds_.clear();
    state_->selectedEntryIds_.clear();
    state_->commitProjectEdit();
    generatedFillInsertionEntries_.clear();
    generatedFillLabel_.clear();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Created %1 with %2 shapes")
                                 .arg(displayName)
                                 .arg(placements.size()),
                             3500);
}

void MainWindow::insertGeneratedFillColored(const QString &groupName,
                                            const QString &displayName,
                                            const QVector<GeneratedRegionShape> &shapes,
                                            const QVector<QString> &insertionEntries)
{
    if (shapes.isEmpty() || !state_->hasProject()) {
        return;
    }

    auto group = std::make_unique<fh6::scene::Group>();
    group->id = state_->uniqueGroupId();
    const QString groupId = group->id;
    group->name = groupName;
    QSet<QString> generatedIds;
    generatedIds.reserve(shapes.size());
    for (const GeneratedRegionShape &placement : shapes) {
        auto shape = std::make_unique<fh6::scene::Shape>();
        shape->id = QStringLiteral("layer_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        shape->name = fh6::detail::shapeName(static_cast<quint16>(placement.shapeId));
        shape->setVectorShape(static_cast<quint16>(placement.shapeId));
        shape->transform = fh6::decomposeTransform2D(generatedShapeMatrix(placement.transform));
        shape->color = {placement.color[0], placement.color[1], placement.color[2], placement.color[3]};
        generatedIds.insert(shape->id);
        group->append(std::move(shape));
    }

    state_->beginProjectEdit();
    state_->insertLayerAboveSelection(std::move(group), insertionEntries);
    if (fh6::scene::Group *inserted = state_->groupForId(groupId); inserted != nullptr) {
        const QString parentId = state_->parentGroupForEntry(groupId);
        if (const fh6::scene::Group *parent = state_->groupForId(parentId); parent != nullptr) {
            const fh6::Matrix3 parentInverse = fh6::invertAffine(parent->worldMatrix());
            for (const auto &child : inserted->children) {
                child->transform = fh6::decomposeTransform2D(
                    fh6::detail::multiply(parentInverse, child->transform.matrix()));
            }
        }
    }
    state_->selectedLayerIds_ = generatedIds;
    state_->selectedGuideLayerIds_.clear();
    state_->selectedEntryIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Created %1 with %2 shapes")
                                 .arg(displayName)
                                 .arg(shapes.size()),
                             3500);
}

fh6::Project *MainWindow::project()
{
    return state_->project();
}

QVector<fh6::scene::Group *> MainWindow::selectedGroups()
{
    return state_->selectedGroups(state_->fullySelectedTopGroupIds());
}

void MainWindow::refreshSelectionProperties()
{
    if (properties_ == nullptr) {
        return;
    }
    ScopedPerf perf("refreshSelectionProperties");
    properties_->setSelection(state_->selectedLayers(), state_->selectedGuideLayers(), selectedGroups());
    refreshPropertyBoxFieldsFromCanvas();
}

void MainWindow::refreshPropertyBoxFieldsFromCanvas()
{
    if (properties_ == nullptr || canvas_ == nullptr) {
        return;
    }
    QPointF center;
    double width = 0.0;
    double height = 0.0;
    QTransform boxFrame;
    const bool valid = canvas_->currentTransformBox(&center, &width, &height, &boxFrame);
    properties_->refreshTransformFieldsFromBox(center, width, height, boxFrame, valid);
}

void MainWindow::deleteSelectedLayers()
{
    if (!state_->hasProject() || (state_->selectedLayerIds().isEmpty() && state_->selectedGuideLayerIds().isEmpty())) {
        return;
    }
    const QSet<QString> lockedIds = state_->lockedLayerIds();
    for (const QString &layerId : state_->selectedLayerIds()) {
        if (lockedIds.contains(layerId)) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }
    for (fh6::scene::GuideLayer *guide : state_->selectedGuideLayers()) {
        if (guide->locked) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }

    state_->beginProjectEdit();
    state_->removeEntries(selectedEntryIds());
    state_->pruneEmptyGroups();
    state_->selectedLayerIds_.clear();
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
}

void MainWindow::copySelection()
{
    if (copySelectionToClipboard()) {
        statusBar()->showMessage(QStringLiteral("Copied selection"), 1500);
    }
}

void MainWindow::cutSelection()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to cut"), 2500);
        return;
    }
    if (!state_->copyEntriesToClipboard(entries)) {
        statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
        return;
    }

    state_->beginProjectEdit();
    state_->removeEntries(entries);
    state_->selectedLayerIds_.clear();
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Cut selection"), 1500);
}

void MainWindow::pasteClipboard()
{
    if (!state_->hasProject() || state_->clipboard() == nullptr) {
        statusBar()->showMessage(QStringLiteral("Clipboard is empty"), 2500);
        return;
    }

    state_->beginProjectEdit();
    QSet<QString> newLayerSelection;
    QSet<QString> newGuideSelection;
    state_->insertClipboardAboveSelection(*state_->clipboard(), selectedEntryIds(), &newLayerSelection, &newGuideSelection);
    state_->selectedLayerIds_ = newLayerSelection;
    state_->selectedGuideLayerIds_ = newGuideSelection;
    state_->selectedEntryIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Pasted selection"), 1500);
}

void MainWindow::stampSelection()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to stamp"), 2500);
        return;
    }
    state_->beginProjectEdit();
    const bool stamped = state_->duplicateEntriesInPlace(entries);
    state_->commitProjectEdit();
    if (!stamped) {
        statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
        return;
    }
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Stamped selection"), 1500);
}

void MainWindow::sampleGuideColorToSelection()
{
    if (properties_ == nullptr || !properties_->sampleGuideColorToSelection()) {
        statusBar()->showMessage(QStringLiteral("No guide color under cursor or no colorable selection"), 2500);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Picked guide color"), 1500);
}

void MainWindow::insertShape(int shapeId)
{
    if (!ensureProjectForInsertion()) {
        return;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    auto layer = std::make_unique<fh6::scene::Shape>();
    layer->id = state_->uniqueLayerId();
    layer->name = fh6::detail::shapeName(static_cast<quint16>(shapeId));
    layer->setVectorShape(static_cast<quint16>(shapeId));
    layer->x = center.x();
    layer->y = center.y();
    updateLastSelectedShapeDefaults();
    const BehaviorSettings behavior = loadBehaviorSettings();
    if (behavior.insertShapeWithLastSelectedScale && haveLastSelectedShapeDefaults_) {
        layer->scaleX = lastSelectedShapeScaleX_;
        layer->scaleY = lastSelectedShapeScaleY_;
    }

    state_->beginProjectEdit();
    const QString insertedId = layer->id;
    const QString insertedName = layer->name;
    state_->insertLayerAboveSelection(std::move(layer), selectedEntryIds());
    state_->selectedLayerIds_ = {insertedId};
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Inserted %1").arg(insertedName), 1500);
}

void MainWindow::replaceSelectedShape(int shapeId)
{
    const QVector<fh6::scene::Shape *> selected = state_->selectedLayers();
    if (selected.size() != 1 || !state_->selectedGuideLayerIds().isEmpty()
        || selected.front() == nullptr) {
        statusBar()->showMessage(QStringLiteral("Select one shape to replace"), 2000);
        return;
    }

    fh6::scene::Shape *layer = selected.front();
    if (!layer->isRaster() && layer->shapeId == shapeId) {
        statusBar()->showMessage(QStringLiteral("Selected shape already uses this geometry"), 1500);
        return;
    }

    state_->beginProjectEdit();
    layer->setVectorShape(static_cast<quint16>(shapeId));
    layer->sourceLogoId = 0;
    state_->commitProjectEdit();
    state_->noteProjectGeometryChanged(true, {layer->id});
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Replaced selected shape"), 1500);
}

void MainWindow::insertLogo(quint32 rasterId, int width, int height)
{
    if (!ensureProjectForInsertion()) {
        return;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    auto layer = std::make_unique<fh6::scene::Shape>();
    layer->id = state_->uniqueLayerId();
    layer->name = QStringLiteral("Logo %1").arg(rasterId);
    layer->setRasterShape(rasterId, width, height);
    layer->sourceLogoId = static_cast<quint16>(0x8000u | (rasterId & 0x7fffu));
    layer->x = center.x();
    layer->y = center.y();
    updateLastSelectedShapeDefaults();
    const BehaviorSettings behavior = loadBehaviorSettings();
    if (behavior.insertShapeWithLastSelectedColor && haveLastSelectedShapeDefaults_) {
        layer->color = lastSelectedShapeColor_;
    }
    if (behavior.insertShapeWithLastSelectedScale && haveLastSelectedShapeDefaults_) {
        layer->scaleX = lastSelectedShapeScaleX_;
        layer->scaleY = lastSelectedShapeScaleY_;
    }

    state_->beginProjectEdit();
    const QString insertedId = layer->id;
    const QString insertedName = layer->name;
    state_->insertLayerAboveSelection(std::move(layer), selectedEntryIds());
    state_->selectedLayerIds_ = {insertedId};
    state_->selectedGuideLayerIds_.clear();
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Inserted %1").arg(insertedName), 1500);
}

void MainWindow::placeTextDialog()
{
    if (!ensureProjectForInsertion()) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Place Text"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *fontCombo = new QComboBox(&dialog);
    fontCombo->addItems(gui::fontglyphs::fontNames());
    const QString lastFont = QSettings().value(QStringLiteral("placeText/font")).toString();
    const int lastFontIndex = fontCombo->findText(lastFont);
    if (lastFontIndex >= 0) {
        fontCombo->setCurrentIndex(lastFontIndex);
    }
    auto *textEdit = new QLineEdit(&dialog);
    textEdit->setPlaceholderText(QStringLiteral("Type text to place..."));
    auto *monoCheck = new QCheckBox(QStringLiteral("Monospace"), &dialog);
    monoCheck->setToolTip(QStringLiteral(
        "Advance every glyph by a fixed cell (the font's average glyph width, "
        "computed separately for upper- and lower-case) instead of each glyph's "
        "own width."));
    monoCheck->setChecked(QSettings().value(QStringLiteral("placeText/monospace"), false).toBool());
    form->addRow(QStringLiteral("Font"), fontCombo);
    form->addRow(QStringLiteral("Text"), textEdit);
    form->addRow(QString(), monoCheck);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    textEdit->setFocus();

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString fontName = fontCombo->currentText();
    const QString text = textEdit->text();
    const bool monospace = monoCheck->isChecked();
    if (text.trimmed().isEmpty()) {
        return;
    }
    QSettings().setValue(QStringLiteral("placeText/font"), fontName);
    QSettings().setValue(QStringLiteral("placeText/monospace"), monospace);

    const PlacedTextLine line = layoutTextGlyphs(fontName, text, monospace, [this](int shapeId) {
        return canvas_ != nullptr ? canvas_->shapeInkBounds(shapeId)
                                  : QRectF(-64.0, -64.0, 128.0, 128.0);
    });

    if (line.glyphs.isEmpty()) {
        QMessageBox::information(this,
                                QStringLiteral("Place Text"),
                                QStringLiteral("None of the typed characters have a glyph in %1.").arg(fontName));
        return;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    const double startX = center.x() - line.width * 0.5;

    state_->beginProjectEdit();
    QVector<QString> newIds;
    newIds.reserve(line.glyphs.size());
    for (const PlacedTextGlyph &glyph : line.glyphs) {
        auto layer = std::make_unique<fh6::scene::Shape>();
        layer->id = state_->uniqueLayerId();
        layer->name = fh6::detail::shapeName(static_cast<quint16>(glyph.shapeId));
        layer->setVectorShape(static_cast<quint16>(glyph.shapeId));
        layer->x = startX + glyph.originX;
        layer->y = center.y();
        const QString id = layer->id;
        state_->insertLayerAboveSelection(std::move(layer), {});
        newIds.push_back(id);
    }

    QString groupName = text.trimmed();
    if (newIds.size() > 1) {
        state_->groupEntries(newIds);
        const QString parent = state_->parentGroupForEntry(newIds.front());
        if (fh6::scene::Group *group = state_->groupForId(parent); group != nullptr) {
            group->name = groupName;
        }
    } else {
        state_->selectedLayerIds_ = {newIds.front()};
        state_->selectedGuideLayerIds_.clear();
    }
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }

    if (!line.skipped.isEmpty()) {
        QMessageBox::information(this,
                                QStringLiteral("Place Text"),
                                QStringLiteral("Placed \"%1\" in %2.\n\nSkipped characters with no glyph in this font: %3")
                                    .arg(groupName, fontName, line.skipped));
    } else {
        statusBar()->showMessage(QStringLiteral("Placed \"%1\" (%2)").arg(groupName, fontName), 2000);
    }
}

void MainWindow::saveCurrentSelectionAsCustomGroup()
{
    if (!state_->hasProject()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("Open a project before saving a custom group."));
        return;
    }
    QVector<QString> entries;
    for (const QString &id : selectedEntryIds()) {
        if (!state_->entryIsGuide(id)) {
            entries.push_back(id);
        }
    }
    entries = state_->normalizeEntrySelection(entries);
    if (entries.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("Select one or more shape layers or groups first."));
        return;
    }

    ProjectClipboard clipboard;
    if (!state_->buildEntryClipboard(entries, clipboard) || clipboard.nodes.empty()) {
        QMessageBox::information(this, QStringLiteral("Custom Group"), QStringLiteral("The selected layers cannot be saved as a custom group."));
        return;
    }

    QString defaultName = entryNameForId(state_->project_, entries.front()).trimmed();
    if (defaultName.isEmpty()) {
        defaultName = QStringLiteral("Custom Group");
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Add Custom Group"),
                                               QStringLiteral("Name"),
                                               QLineEdit::Normal,
                                               defaultName,
                                               &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    shapesBrowser_->addCustomGroup(name, clipboard);
    statusBar()->showMessage(QStringLiteral("Saved custom group %1").arg(name), 2000);
}

void MainWindow::insertCustomGroup(const QString &name, const ProjectClipboard &clipboard)
{
    if (!ensureProjectForInsertion()) {
        return;
    }
    if (clipboard.nodes.empty()) {
        return;
    }
    ProjectClipboard placement;
    if (clipboard.nodes.size() == 1 && clipboard.nodes.front()
        && clipboard.nodes.front()->kind() == fh6::scene::LayerKind::Group) {
        std::unique_ptr<fh6::scene::Layer> root = clipboard.nodes.front()->clone();
        root->name = name;
        placement.rootIds = {root->id};
        placement.nodes.push_back(std::move(root));
    } else {
        auto root = std::make_unique<fh6::scene::Group>();
        root->id = QStringLiteral("custom_group_root");
        root->name = name;
        for (const auto &node : clipboard.nodes) {
            if (node) {
                root->append(node->clone());
            }
        }
        placement.rootIds = {root->id};
        placement.nodes.push_back(std::move(root));
    }

    QSet<QString> layerSelection;
    QSet<QString> guideSelection;
    QVector<QString> insertedRoots;
    state_->beginProjectEdit();
    state_->insertClipboardAboveSelection(
        placement, selectedEntryIds(), &layerSelection, &guideSelection, false, &insertedRoots);
    state_->selectedLayerIds_ = layerSelection;
    state_->selectedGuideLayerIds_ = guideSelection;
    state_->selectedEntryIds_.clear();
    QRectF insertedBounds;
    bool hasInsertedBounds = false;
    for (const QString &id : layerSelection) {
        auto *layer = dynamic_cast<fh6::scene::Shape *>(state_->sceneNode(id));
        if (layer == nullptr) {
            continue;
        }
        const QSizeF size = canvas_ == nullptr ? QSizeF(128.0, 128.0) : canvas_->shapeSize(layer->shapeId);
        const QRectF bounds = shapeWorldBounds(*layer, size);
        insertedBounds = hasInsertedBounds ? insertedBounds.united(bounds) : bounds;
        hasInsertedBounds = true;
    }
    if (hasInsertedBounds) {
        const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
        const QPointF delta = center - insertedBounds.center();
        if (!insertedRoots.isEmpty()) {
            state_->transformGroupFrames(insertedRoots, QTransform::fromTranslate(delta.x(), delta.y()));
        }
    }
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Inserted custom group"), 1500);
}

bool MainWindow::importGuideLayer(const QString &path, QString *error)
{
    if (!ensureProjectForInsertion()) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    QString decodeError;
    QByteArray decodedFormat;
    const QImage image = readGuideImage(path, &decodedFormat, &decodeError);
    if (image.isNull()) {
        if (error != nullptr) {
            *error = decodeError.isEmpty()
                ? QStringLiteral("could not decode image: %1").arg(path)
                : decodeError;
        }
        return false;
    }

    QString embedFormat;
    QProgressDialog progress(QStringLiteral("Converting guide image for project storage..."),
                             QString(),
                             0,
                             0,
                             this);
    progress.setWindowTitle(QStringLiteral("Converting Image"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    progress.show();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const QByteArray embedBytes = encodeGuideImage(image, &embedFormat);
    progress.close();
    if (embedBytes.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("could not encode guide image: %1").arg(path);
        }
        return false;
    }

    const QPointF center = canvas_ == nullptr ? QPointF() : canvas_->viewCenterWorld();
    auto guide = std::make_unique<fh6::scene::GuideLayer>();
    guide->id = state_->uniqueGuideLayerId();
    guide->name = QFileInfo(path).completeBaseName();
    if (guide->name.isEmpty()) {
        guide->name = QStringLiteral("Guide");
    }
    guide->sourcePath = QFileInfo(path).absoluteFilePath();
    guide->image = std::make_unique<fh6::scene::RasterContainer>();
    guide->image->encoded = embedBytes;
    guide->image->pixels = QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    guide->image->format = embedFormat;
    guide->image->width = image.width();
    guide->image->height = image.height();
    guide->x = center.x();
    guide->y = center.y();
    guide->opacity = 0.5;

    state_->beginProjectEdit();
    const QString guideId = guide->id;
    const QString guideName = guide->name;
    const QString sectionId = state_->project_.isLivery ? activeLiverySectionId() : QString();
    if (!sectionId.isEmpty()) {
        if (fh6::scene::Group *section = state_->groupForId(sectionId)) {
            section->append(std::move(guide));
        } else {
            state_->project_.root->append(std::move(guide));
        }
    } else {
        state_->project_.root->append(std::move(guide));
    }
    state_->selectedLayerIds_.clear();
    state_->selectedGuideLayerIds_ = {guideId};
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    if (canvas_ != nullptr) {
        canvas_->setTool(QStringLiteral("transform"));
        canvas_->setFocus();
    }
    statusBar()->showMessage(QStringLiteral("Added guide layer %1").arg(guideName), 2500);
    return true;
}

void MainWindow::groupOrUngroupSelection()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to group"), 2500);
        return;
    }

    for (const QString &entryId : entries) {
        if (state_->entryHasLockedLayer(entryId)) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }
    if (entries.size() == 1 && state_->entryIsGroup(entries.front())) {
        state_->beginProjectEdit();
        state_->ungroupEntries(entries, false);
        state_->commitProjectEdit();
        state_->noteProjectStructureChanged();
        statusBar()->showMessage(QStringLiteral("Ungrouped selection"), 1500);
        return;
    }

    for (const QString &entryId : entries) {
        if (state_->entryIsGuide(entryId)) {
            statusBar()->showMessage(QStringLiteral("Guide layers cannot be grouped"), 3000);
            return;
        }
    }
    if (entries.size() < 2) {
        statusBar()->showMessage(QStringLiteral("Select at least two layers to group"), 3000);
        return;
    }
    const QString parentId = state_->parentGroupForEntry(entries.front());
    const fh6::scene::Group *parentGroup = state_->groupForId(parentId);
    if (parentGroup == nullptr) {
        statusBar()->showMessage(QStringLiteral("Selection cannot be grouped"), 3000);
        return;
    }
    QVector<int> orders;
    orders.reserve(entries.size());
    for (const QString &entryId : entries) {
        if (state_->parentGroupForEntry(entryId) != parentId) {
            statusBar()->showMessage(QStringLiteral("Only direct sibling layers can be grouped"), 3000);
            return;
        }
        int order = -1;
        for (int i = 0; i < static_cast<int>(parentGroup->children.size()); ++i) {
            if (parentGroup->children[i]->id == entryId) {
                order = i;
                break;
            }
        }
        if (order < 0) {
            statusBar()->showMessage(QStringLiteral("Selection cannot be grouped"), 3000);
            return;
        }
        orders.push_back(order);
    }
    std::sort(orders.begin(), orders.end());
    for (int i = 1; i < orders.size(); ++i) {
        if (orders[i] != orders[i - 1] + 1) {
            statusBar()->showMessage(QStringLiteral("Select adjacent sibling layers to group"), 3000);
            return;
        }
    }

    state_->beginProjectEdit();
    state_->groupEntries(entries);
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Grouped selection"), 1500);
}

void MainWindow::ungroupSelectionFlat()
{
    if (!state_->hasProject()) {
        return;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to ungroup"), 2500);
        return;
    }

    for (const QString &entryId : entries) {
        if (state_->entryHasLockedLayer(entryId)) {
            statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
            return;
        }
    }

    state_->beginProjectEdit();
    state_->ungroupEntries(entries, true);
    state_->commitProjectEdit();
    state_->noteProjectStructureChanged();
    statusBar()->showMessage(QStringLiteral("Flat ungrouped selection"), 1500);
}

void MainWindow::collapseAllGroups()
{
    if (tree_ == nullptr) {
        return;
    }
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    tree_->collapseAll();
}

void MainWindow::undo()
{
    state_->undo();
    canvas_->resetRelativeSelectionFrame();
}

void MainWindow::redo()
{
    state_->redo();
    canvas_->resetRelativeSelectionFrame();
}

void MainWindow::centerViewOnSelection()
{
    if (canvas_ == nullptr || !canvas_->centerViewOnSelection()) {
        statusBar()->showMessage(QStringLiteral("No selection to center"), 1500);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Centered view on selection"), 1500);
}

void MainWindow::noteProjectGeometryChanged(bool refreshPreviews)
{
    ScopedPerf perf(refreshPreviews ? "noteProjectGeometryChanged(previews)" : "noteProjectGeometryChanged");
    if (canvas_ != nullptr) {
        canvas_->invalidateSelectionCache();
        canvas_->invalidateSceneCache();
        canvas_->update();
    }
    if (refreshPreviews) {
        ScopedPerf perfPrev("  refreshPreviews");
        treeModel_->refreshStateRoles(&state_->project_);
        treeModel_->refreshPreviews(&state_->project_);
    }
    updateColorPaletteWidget();
    refreshSelectionProperties();
    updateStatus();
}

void MainWindow::noteProjectStructureChanged()
{
    ScopedPerf perf("noteProjectStructureChanged");
    state_->selectedLayerIds_ = existingLayerIds(state_->selectedLayerIds_);
    state_->selectedGuideLayerIds_ = state_->existingGuideLayerIds(state_->selectedGuideLayerIds_);
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    if (state_->project_.isLivery) {
        const QString sectionId = activeLiverySectionId();
        if (!sectionId.isEmpty()) {
            applyLiverySectionVisibility(sectionId);
            treeModel_->clearSectionCache();
            treeModel_->clear();
            {
                ScopedPerf perfTree("  treeModel_->setProjectSection");
                treeModel_->setProjectSection(&state_->project_, sectionId);
            }
        } else {
            treeModel_->setProject(&state_->project_);
        }
    } else {
        {
            ScopedPerf perfTree("  treeModel_->setProject");
            treeModel_->setProject(&state_->project_);
        }
    }
    if (state_->project_.isLivery) {
        rebuildSectionBar();
    }
    syncTreeSelectionFromIds();
    noteProjectGeometryChanged();
    refreshSelectionProperties();
}

void MainWindow::setToolName(const QString &name)
{
    for (QAction *action : actions()) {
        const QVariant toolName = action->property("canvasToolName");
        if (toolName.isValid()) {
            action->setChecked(toolName.toString() == name);
        }
    }
    const bool lining = name == QStringLiteral("lining");
    if (liningWidthLabel_ != nullptr) {
        liningWidthLabel_->setVisible(lining);
    }
    if (liningWidthSpin_ != nullptr) {
        liningWidthSpin_->setVisible(lining);
    }
    statusBar()->showMessage(QStringLiteral("Tool: %1").arg(name), 1500);
}

} // namespace gui
