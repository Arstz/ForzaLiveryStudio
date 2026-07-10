#include "main_window.h"

#include "main_window_internal.h"

#include "car_preview_widget.h"
#include "car_registry.h"
#include "clipboard_buffer_widget.h"
#include "color_palette_widget.h"
#include "livery_section_bar.h"
#include "perf_utils.h"
#include "property_panel.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>

#include <functional>

namespace gui {

using namespace mw_detail;

void MainWindow::setProject(fh6::Project project)
{
    state_->setProject(std::move(project));
    projectJsonPath_.clear();  // imports/new projects have no associated .json yet
    autoExpandedTreeIndexes_.clear();
    autoExpandedGroupIds_.clear();
    if (canvas_ != nullptr) {
        canvas_->setProject(&state_->project_);
    }
    if (state_->project_.isLivery) {
        rebuildSectionBar();  // selects the first populated section (tree + canvas)
        prebakeLiverySectionCaches();
    } else {
        if (sectionBar_ != nullptr) {
            sectionBar_->setSections({});
        }
        treeModel_->setProject(&state_->project_);
    }
    updateColorPaletteWidget();
    updateClipboardWidget();
    updateStatus();
    maybeAutoLoadCarForProject();
    updateCarUnwrapOverlay();
}

void MainWindow::maybeAutoLoadCarForProject()
{
    if (carPreview_ == nullptr || state_ == nullptr || !state_->hasProject_) {
        return;
    }
    const fh6::Project &project = state_->project_;
    if (!project.isLivery) {
        return;  // group projects keep whatever model is loaded
    }

    BehaviorSettings settings = loadBehaviorSettings();
    if (carPreview_->hasModel()) {
        // Keep the current model unless the user wants it discarded on livery open.
        if (!settings.discardModelOnLiveryOpen) {
            return;
        }
        carPreview_->clearModel();
    }
    if (project.carId == 0) {
        return;  // nothing to auto-load (model already discarded above if requested)
    }
    // The friendly name is for messages; the model code is how files are named on disk.
    const QString modelName = sharedCarRegistry().name(project.carId);
    const QString modelCode = sharedCarRegistry().modelCode(project.carId);
    if (modelCode.isEmpty()) {
        return;  // unknown car id -> nothing to match against
    }

    QString folder = settings.carModelsFolder;
    if (folder.isEmpty() || !QDir(folder).exists()) {
        if (!promptedForCarModelsFolder_) {
            promptedForCarModelsFolder_ = true;
            const QMessageBox::StandardButton choice = QMessageBox::question(
                this, QStringLiteral("Car models folder"),
                QStringLiteral("This livery targets \"%1\".\n\nChoose the folder that holds your extracted "
                               "car models so the matching car can be loaded automatically? "
                               "(You can also set it later under Settings.)")
                    .arg(modelName.isEmpty() ? modelCode : modelName),
                QMessageBox::Yes | QMessageBox::No);
            if (choice == QMessageBox::Yes) {
                const QString picked = QFileDialog::getExistingDirectory(this, QStringLiteral("Car Models Folder"));
                if (!picked.isEmpty()) {
                    settings.carModelsFolder = picked;
                    saveBehaviorSettings(settings);
                    folder = picked;
                }
            }
        }
        if (folder.isEmpty() || !QDir(folder).exists()) {
            return;
        }
    }

    const QString path = findCarModelPath(folder, modelCode);
    if (path.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("No car model matching \"%1\" found in the car models folder").arg(modelCode), 4000);
        return;
    }
    QString error;
    if (!carPreview_->loadCar(path, &error)) {
        statusBar()->showMessage(error.isEmpty() ? QStringLiteral("Failed to auto-load car model") : error, 4000);
        return;
    }
    if (carPreviewDock_ != nullptr) {
        carPreviewDock_->show();
        carPreviewDock_->raise();
    }
    statusBar()->showMessage(QStringLiteral("Auto-loaded car model: %1").arg(QFileInfo(path).fileName()), 4000);
}

QString MainWindow::findCarModelPath(const QString &folder, const QString &modelName) const
{
    QDir dir(folder);
    if (modelName.isEmpty() || !dir.exists()) {
        return QString();
    }
    const QStringList exts = {QStringLiteral("carbin"), QStringLiteral("zip"), QStringLiteral("modelbin")};

    // 1) Direct <modelName>.<ext> next to the folder root.
    for (const QString &ext : exts) {
        const QString candidate = dir.filePath(QStringLiteral("%1.%2").arg(modelName, ext));
        if (QFileInfo(candidate).isFile()) {
            return candidate;
        }
    }

    // Returns the first .carbin (then .zip) inside a directory, or empty.
    const auto modelInDir = [](const QString &dirPath) -> QString {
        QDir sub(dirPath);
        for (const QString &pattern : {QStringLiteral("*.carbin"), QStringLiteral("*.zip")}) {
            const QStringList matches = sub.entryList(QStringList{pattern}, QDir::Files);
            if (!matches.isEmpty()) {
                return sub.filePath(matches.first());
            }
        }
        return QString();
    };

    // 2) Case-insensitive match at the root: a <modelName>.<ext> file or a same-named
    //    subfolder holding a car model.
    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &info : entries) {
        if (info.isFile()) {
            if (info.completeBaseName().compare(modelName, Qt::CaseInsensitive) == 0
                && exts.contains(info.suffix().toLower())) {
                return info.filePath();
            }
        } else if (info.isDir() && info.fileName().compare(modelName, Qt::CaseInsensitive) == 0) {
            const QString found = modelInDir(info.filePath());
            if (!found.isEmpty()) {
                return found;
            }
        }
    }

    // 3) Bounded recursive fallback: some libraries nest cars under manufacturer or
    //    variant subfolders. Look for a directory named after the model code holding a
    //    car model, or a <modelName>.carbin/.zip anywhere under the folder.
    QDirIterator it(folder, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString entryPath = it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isDir()) {
            if (info.fileName().compare(modelName, Qt::CaseInsensitive) == 0) {
                const QString found = modelInDir(entryPath);
                if (!found.isEmpty()) {
                    return found;
                }
            }
        } else if (info.isFile()
                   && info.completeBaseName().compare(modelName, Qt::CaseInsensitive) == 0
                   && (info.suffix().compare(QStringLiteral("carbin"), Qt::CaseInsensitive) == 0
                       || info.suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) == 0)) {
            return entryPath;
        }
    }
    return QString();
}

void MainWindow::rebuildSectionBar()
{
    if (sectionBar_ == nullptr) {
        return;
    }
    QVector<LiverySectionBar::SectionInfo> sections;
    for (fh6::scene::Group *group : liverySections(state_->project_)) {
        sections.push_back({group->id, group->name, static_cast<int>(state_->leafLayerIdsForEntry(group->id).size())});
    }
    sectionBar_->setSections(sections);
}

void MainWindow::setActiveSection(const QString &sectionGroupId)
{
    state_->setActiveSectionId(sectionGroupId);

    // Draw only this section on the canvas by toggling layer visibility (a view
    // operation; livery view is read-only so this does not disturb edits). Do
    // this before rebuilding the tree because thumbnail generation skips hidden
    // leaves and group badges read descendant visibility.
    applyLiverySectionVisibility(sectionGroupId);

    // Show only this section's contents in the tree.
    treeModel_->setProjectSection(&state_->project_, sectionGroupId);

    state_->selectedLayerIds_.clear();
    if (canvas_ != nullptr) {
        // Refit to the now-visible section (projectBounds() ignores hidden
        // layers) without replacing the project pointer, so per-section canvas
        // render caches survive tab switches.
        canvas_->refitView();
        canvas_->invalidateSelectionCache();
        canvas_->update();
    }
    refreshSelectionProperties();
    updateCarUnwrapOverlay();
}

void MainWindow::prebakeLiverySectionCaches()
{
    if (state_ == nullptr || !state_->hasProject_ || !state_->project_.isLivery || canvas_ == nullptr || treeModel_ == nullptr) {
        return;
    }
    const QString restoreSectionId = activeLiverySectionId();
    if (restoreSectionId.isEmpty()) {
        return;
    }

    QVector<QString> sectionIds;
    const QVector<fh6::scene::Group *> sections = liverySections(state_->project_);
    sectionIds.reserve(sections.size());
    for (const fh6::scene::Group *group : sections) {
        sectionIds.push_back(group->id);
    }

    const QSet<QString> restoreLayerSelection = state_->selectedLayerIds_;
    const QSet<QString> restoreGuideSelection = state_->selectedGuideLayerIds_;
    for (const QString &sectionId : sectionIds) {
        state_->setActiveSectionId(sectionId);
        applyLiverySectionVisibility(sectionId);
        treeModel_->setProjectSection(&state_->project_, sectionId);
        canvas_->refitView();
        canvas_->invalidateSelectionCache();
        canvas_->repaint();
    }

    state_->setActiveSectionId(restoreSectionId);
    applyLiverySectionVisibility(restoreSectionId);
    treeModel_->setProjectSection(&state_->project_, restoreSectionId);
    state_->selectedLayerIds_ = restoreLayerSelection;
    state_->selectedGuideLayerIds_ = restoreGuideSelection;
    canvas_->refitView();
    canvas_->invalidateSelectionCache();
    canvas_->update();
    refreshSelectionProperties();
}

QString MainWindow::activeLiverySectionId() const
{
    if (state_ == nullptr || !state_->hasProject_ || !state_->project_.isLivery) {
        return {};
    }
    auto isExistingSection = [this](const QString &id) {
        for (const fh6::scene::Group *group : liverySections(state_->project_)) {
            if (group->id == id) {
                return true;
            }
        }
        return false;
    };
    if (isExistingSection(state_->activeSectionId_)) {
        return state_->activeSectionId_;
    }
    for (const fh6::scene::Group *section : liverySections(state_->project_)) {
        if (isExistingSection(section->id)) {
            state_->setActiveSectionId(section->id);
            return section->id;
        }
    }
    return {};
}

int MainWindow::activeLiverySectionSlot() const
{
    if (state_ == nullptr || !state_->hasProject_ || !state_->project_.isLivery) {
        return -1;
    }
    const QString sectionId = activeLiverySectionId();
    if (sectionId.isEmpty()) {
        return -1;
    }
    for (const fh6::scene::Group *section : liverySections(state_->project_)) {
        if (section->id == sectionId) {
            return section->liverySectionSlot;
        }
    }
    return -1;
}

void MainWindow::applyLiverySectionVisibility(const QString &sectionGroupId)
{
    if (state_ == nullptr || !state_->hasProject_ || sectionGroupId.isEmpty()) {
        return;
    }
    const QVector<QString> activeLeaves = state_->leafLayerIdsForEntry(sectionGroupId);
    const QSet<QString> activeSet(activeLeaves.begin(), activeLeaves.end());
    forEachShape(state_->project_, [&](fh6::scene::Shape &layer) {
        layer.visible = activeSet.contains(layer.id);
    });
}

void MainWindow::updateStatus()
{
    if (!state_->hasProject_) {
        details_->setText(QStringLiteral("No project loaded"));
        return;
    }

    // Prefer the project's own header creator; fall back to the app-wide default creator
    // (which the Project > Creator Name dialog also edits) when the project has no header.
    QString creator = state_->project_.headerMetadata ? state_->project_.headerMetadata->creatorName : QString();
    if (creator.isEmpty()) {
        creator = creatorName_;
    }
    int shapeCount = 0;
    int guideCount = 0;
    int groupCount = 0;
    forEachShape(state_->project_, [&](fh6::scene::Shape &) { ++shapeCount; });
    forEachGuide(state_->project_, [&](fh6::scene::GuideLayer &) { ++guideCount; });
    if (state_->project_.root) {
        std::function<void(const fh6::scene::Layer &)> countGroups = [&](const fh6::scene::Layer &node) {
            if (node.kind() == fh6::scene::LayerKind::Group) {
                ++groupCount;
                for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                    countGroups(*child);
                }
            }
        };
        for (const auto &child : state_->project_.root->children) {
            countGroups(*child);
        }
    }
    QString text = QStringLiteral("%1\nCreator: %2\nSource: %3\nLayers: %4\nGroups: %5")
                          .arg(state_->project_.name)
                          .arg(creator.isEmpty() ? QStringLiteral("(unset)") : creator)
                          .arg(state_->project_.sourceFolder.isEmpty() ? QStringLiteral("(new project)") : state_->project_.sourceFolder)
                          .arg(shapeCount)
                          .arg(groupCount)
                      + QStringLiteral("\nGuide layers: %1").arg(guideCount);
    if (state_->project_.isLivery) {
        const QString car = state_->project_.carId != 0
            ? sharedCarRegistry().displayName(state_->project_.carId)
            : QStringLiteral("(unset)");
        text += QStringLiteral("\nCar: %1").arg(car);
    }
    details_->setText(text);
}

void MainWindow::updateClipboardWidget()
{
    if (clipboardWidget_ == nullptr) {
        return;
    }
    clipboardWidget_->setClipboard(state_->clipboard());
}

void MainWindow::updateColorPaletteWidget()
{
    if (colorPalette_ == nullptr || state_ == nullptr || !state_->hasProject()) {
        if (colorPalette_ != nullptr) {
            colorPalette_->setSwatches(nullptr);
        }
        return;
    }
    colorPalette_->setSwatches(&state_->project_.colorSwatches);
}

void MainWindow::updateLastSelectedShapeDefaults()
{
    const QVector<fh6::scene::Shape *> selected = state_->selectedLayers();
    if (selected.isEmpty() || selected.front() == nullptr) {
        return;
    }
    const fh6::scene::Shape *layer = selected.front();
    lastSelectedShapeColor_ = layer->color;
    lastSelectedShapeScaleX_ = layer->scaleX;
    lastSelectedShapeScaleY_ = layer->scaleY;
    haveLastSelectedShapeDefaults_ = true;
}

void MainWindow::updateSelectionFromTree()
{
    if (syncingSelection_) {
        return;
    }
    QSet<QString> ids;
    QSet<QString> guideIds;
    QVector<QString> entryIds;
    for (const QModelIndex &index : tree_->selectionModel()->selectedRows()) {
        const QString entryId = index.data(LayerTreeModel::EntryIdRole).toString();
        if (!entryId.isEmpty()) {
            entryIds.push_back(entryId);
        }
        for (const QString &id : idsForIndex(index)) {
            ids.insert(id);
        }
        for (const QString &id : index.data(LayerTreeModel::GuideIdsRole).toStringList()) {
            guideIds.insert(id);
        }
    }
    // The selection change below re-enters syncTreeSelectionFromIds() synchronously; suppress
    // its reveal so a click in the layers list doesn't scroll the list back to that row.
    suppressTreeReveal_ = true;
    state_->setSelectionFromEntries(ids, guideIds, entryIds);
    suppressTreeReveal_ = false;
    if (canvas_ != nullptr) {
        canvas_->setFocus();
    }
}

void MainWindow::syncTreeSelectionFromIds()
{
    if (tree_->selectionModel() == nullptr) {
        return;
    }
    ScopedPerf perf("syncTreeSelectionFromIds");
    syncingSelection_ = true;
    tree_->selectionModel()->clearSelection();

    QModelIndex firstSelected;
    QModelIndex firstExactLeaf;
    QModelIndex firstFullGroup;
    // A row is "covered" when all of its leaves are selected: a group when its whole
    // leaf set is a subset of the selection, a leaf when it is itself selected, a guide
    // when its guide id is selected. Selection is stored only as flat leaf ids, so this
    // is how we recover which group rows to highlight. Computed bottom-up and memoized by
    // entry id (a group is covered iff every child is covered) so we never re-walk a group's
    // whole leaf list once per child - that was O(rows x groupLeaves) and dominated the cost
    // of selecting large groups.
    QHash<QString, bool> coveredById;
    std::function<bool(const QModelIndex &)> coveredFor = [&](const QModelIndex &index) -> bool {
        if (!index.isValid()) {
            return false;
        }
        const QString id = index.data(LayerTreeModel::EntryIdRole).toString();
        const auto cached = coveredById.constFind(id);
        if (cached != coveredById.constEnd()) {
            return cached.value();
        }
        bool result = false;
        if (index.data(LayerTreeModel::IsGuideRole).toBool()) {
            const QStringList guideIds = index.data(LayerTreeModel::GuideIdsRole).toStringList();
            result = guideIds.size() == 1 && state_->selectedGuideLayerIds_.contains(guideIds.front());
        } else if (index.data(LayerTreeModel::IsGroupRole).toBool()) {
            const int childRows = treeModel_->rowCount(index);
            result = childRows > 0;
            for (int r = 0; r < childRows && result; ++r) {
                result = coveredFor(treeModel_->index(r, 0, index));
            }
        } else {
            const QStringList leafIds = index.data(LayerTreeModel::LeafIdsRole).toStringList();
            result = leafIds.size() == 1 && state_->selectedLayerIds_.contains(leafIds.front());
        }
        coveredById.insert(id, result);
        return result;
    };
    std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
        const int rows = treeModel_->rowCount(parent);
        const bool parentCovered = coveredFor(parent);  // false at the root; O(1) once memoized
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = treeModel_->index(row, 0, parent);
            visit(index);
            if (!index.isValid()) {
                continue;
            }
            // Select the topmost covered row: if an ancestor group is also fully covered
            // it already represents this row, so selecting several groups highlights the
            // group rows rather than expanding to (and selecting) their individual leaves.
            const bool selected = coveredFor(index) && !parentCovered;
            if (selected) {
                tree_->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
                if (!firstSelected.isValid()) {
                    firstSelected = index;
                }
                const bool isGroup = index.data(LayerTreeModel::IsGroupRole).toBool();
                if (!firstFullGroup.isValid() && isGroup
                    && index.data(LayerTreeModel::LeafIdsRole).toStringList().size() > 1) {
                    firstFullGroup = index;
                } else if (!firstExactLeaf.isValid() && !isGroup) {
                    firstExactLeaf = index;
                }
            }
        }
    };
    visit(QModelIndex());
    syncingSelection_ = false;

    if (!suppressTreeReveal_) {
        const QModelIndex revealIndex = firstFullGroup.isValid() ? firstFullGroup : (firstExactLeaf.isValid() ? firstExactLeaf : firstSelected);
        revealTreeIndex(revealIndex);
    }
}

void MainWindow::revealTreeIndex(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    QVector<QModelIndex> parents;
    QStringList parentIds;
    for (QModelIndex parent = index.parent(); parent.isValid(); parent = parent.parent()) {
        parents.push_front(parent);
        parentIds.push_front(parent.data(LayerTreeModel::EntryIdRole).toString());
    }

    if (parentIds != autoExpandedGroupIds_) {
        for (auto it = autoExpandedTreeIndexes_.crbegin(); it != autoExpandedTreeIndexes_.crend(); ++it) {
            if (it->isValid()) {
                tree_->collapse(*it);
            }
        }
        autoExpandedTreeIndexes_.clear();
        autoExpandedGroupIds_.clear();
    }
    for (const QModelIndex &parent : parents) {
        if (!tree_->isExpanded(parent)) {
            autoExpandedTreeIndexes_.push_back(QPersistentModelIndex(parent));
        }
        tree_->expand(parent);
    }
    autoExpandedGroupIds_ = parentIds;
    tree_->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

QVector<QString> MainWindow::selectedEntryIds() const
{
    QVector<QString> ids;
    QSet<QString> seen;
    if (tree_ != nullptr && tree_->selectionModel() != nullptr) {
        for (const QModelIndex &index : tree_->selectionModel()->selectedRows()) {
            const QString id = index.data(LayerTreeModel::EntryIdRole).toString();
            if (!id.isEmpty() && !seen.contains(id)) {
                ids.push_back(id);
                seen.insert(id);
            }
        }
    }
    if (!ids.isEmpty()) {
        return ids;
    }
    for (const QString &id : state_->selectedLayerIds_) {
        ids.push_back(id);
    }
    for (const QString &id : state_->selectedGuideLayerIds_) {
        ids.push_back(id);
    }
    return ids;
}

bool MainWindow::copySelectionToClipboard()
{
    if (!state_->hasProject()) {
        return false;
    }
    const QVector<QString> entries = state_->normalizeEntrySelection(selectedEntryIds());
    if (entries.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No selection to copy"), 2500);
        return false;
    }
    if (!state_->copyEntriesToClipboard(entries)) {
        statusBar()->showMessage(QStringLiteral("Selection contains locked layers"), 3000);
        return false;
    }
    return true;
}

bool MainWindow::ensureProjectForInsertion()
{
    if (state_->hasProject_) {
        return true;
    }

    const QString folder = QFileDialog::getExistingDirectory(this,
                                                            QStringLiteral("Create Project Folder"),
                                                            importDialogStartDirectory(this, QStringLiteral("newProject")));
    if (folder.isEmpty()) {
        return false;
    }
    rememberImportDirectory(folder, QStringLiteral("newProject"));

    fh6::Project project;
    project.name = QFileInfo(folder).fileName();
    if (project.name.isEmpty()) {
        project.name = QStringLiteral("Untitled");
    }
    project.sourceFolder = folder;
    project.headerMetadata = fh6::defaultDraftHeader(project.name, creatorName_);
    setProject(std::move(project));
    return true;
}

QStringList MainWindow::idsForIndex(const QModelIndex &index) const
{
    return index.data(LayerTreeModel::LeafIdsRole).toStringList();
}

QSet<QString> MainWindow::existingLayerIds(const QSet<QString> &ids) const
{
    QSet<QString> existing;
    if (!state_->hasProject_) {
        return existing;
    }
    existing = state_->existingLayerIds(ids);
    return existing;
}

void MainWindow::setTargetCarDialog()
{
    if (!state_->hasProject_ || !state_->project_.isLivery) {
        QMessageBox::information(this, QStringLiteral("Set Target Car"),
                                 QStringLiteral("Open a livery project to change its target car."));
        return;
    }
    int carId = state_->project_.carId;
    if (!chooseCarModel(this, carId, &carId)) {
        return;
    }
    if (carId == state_->project_.carId) {
        return;
    }
    state_->project_.carId = carId;
    state_->setModified(true);
    updateStatus();
    statusBar()->showMessage(
        QStringLiteral("Target car set to %1").arg(sharedCarRegistry().displayName(carId)), 5000);
}

void MainWindow::setProjectNameDialog()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Project Name"),
                                 QStringLiteral("Open a project to change its name."));
        return;
    }
    bool ok = false;
    const QString current = state_->project_.name;
    const QString name = QInputDialog::getText(this, QStringLiteral("Project Name"),
                                               QStringLiteral("Project name:"), QLineEdit::Normal, current, &ok);
    if (!ok) {
        return;
    }
    const QString trimmed = name.trimmed();
    if (trimmed == current) {
        return;
    }
    state_->project_.name = trimmed;
    if (state_->project_.headerMetadata) {
        state_->project_.headerMetadata->name = trimmed;
    }
    state_->setModified(true);
    updateStatus();
    refreshHeaderMetadataWidget();
    statusBar()->showMessage(QStringLiteral("Project name updated"), 5000);
}

void MainWindow::setCreatorNameDialog()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Creator Name"),
                                 QStringLiteral("Open a project to change its creator."));
        return;
    }
    bool ok = false;
    QString current = state_->project_.headerMetadata ? state_->project_.headerMetadata->creatorName : QString();
    if (current.isEmpty()) {
        current = creatorName_;
    }
    const QString name = QInputDialog::getText(this, QStringLiteral("Creator Name"),
                                               QStringLiteral("Creator name:"), QLineEdit::Normal, current, &ok);
    if (!ok) {
        return;
    }
    const QString trimmed = name.trimmed();
    if (trimmed == current) {
        return;
    }
    // Persist as the default creator for future new projects, and stamp the open project.
    creatorName_ = trimmed;
    QSettings().setValue(QStringLiteral("header/creatorName"), creatorName_);
    if (state_->project_.headerMetadata) {
        state_->project_.headerMetadata->creatorName = trimmed;
    }
    state_->setModified(true);
    updateStatus();
    refreshHeaderMetadataWidget();
    statusBar()->showMessage(QStringLiteral("Creator name updated"), 5000);
}

} // namespace gui
