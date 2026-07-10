#include "main_window.h"

#include "main_window_internal.h"

#include "car_registry.h"
#include "fh6_core.h"
#include "header_metadata_widget.h"
#include "image_io.h"
#include "layer.h"
#include "project_codec.h"
#include "shape_geometry_store.h"

#include <QtGui>

#include <exception>
#include <iterator>
#include <memory>
#include <utility>

namespace gui {

using namespace mw_detail;

namespace {

constexpr const char *kEmptyLiverySectionNames[] = {
    "Front",
    "Back",
    "Top",
    "Left",
    "Right",
    "Spoiler",
    "FrontWindshield",
    "BackWindshield",
    "TopWindow",
    "LeftWindow",
    "RightWindow",
};

QString generatedGroupId(int index)
{
    return QStringLiteral("group-%1").arg(index, 4, 10, QLatin1Char('0'));
}

fh6::Project createNewProject(bool livery, const QString &creatorName, int carId)
{
    fh6::Project project;
    project.name = QStringLiteral("Untitled");
    project.isLivery = livery;
    project.carId = livery ? carId : 0;
    project.headerMetadata = fh6::defaultDraftHeader(project.name, creatorName);
    if (livery && project.root) {
        for (int slot = 0; slot < static_cast<int>(std::size(kEmptyLiverySectionNames)); ++slot) {
            auto section = std::make_unique<fh6::scene::Group>();
            section->id = generatedGroupId(slot + 1);
            section->name = QString::fromLatin1(kEmptyLiverySectionNames[slot]);
            section->isLiverySection = true;
            section->liverySectionSlot = slot;
            project.root->append(std::move(section));
        }
    }
    return project;
}

bool containsRasterLogo(const fh6::scene::Layer &node)
{
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        return static_cast<const fh6::scene::Shape &>(node).raster;
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return false;
    }
    const auto &group = static_cast<const fh6::scene::Group &>(node);
    for (const auto &child : group.children) {
        if (containsRasterLogo(*child)) {
            return true;
        }
    }
    return false;
}

bool projectContainsRasterLogo(const fh6::Project &project)
{
    if (!project.root) {
        return false;
    }
    for (const auto &child : project.root->children) {
        if (containsRasterLogo(*child)) {
            return true;
        }
    }
    return false;
}

} // namespace

MainWindow::ExternalDropKind MainWindow::classifyExternalDropPath(const QString &path) const
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return ExternalDropKind::Unsupported;
    }
    if (info.isDir()) {
        const QDir dir(info.absoluteFilePath());
        if (QFileInfo(dir.filePath(QStringLiteral("C_group"))).isFile()
            || QFileInfo(dir.filePath(QStringLiteral("C_livery"))).isFile()) {
            return ExternalDropKind::CGroup;
        }
        return ExternalDropKind::Unsupported;
    }

    // Editor project documents: gzip container (.3so) and legacy bare JSON (.json).
    if (info.suffix().compare(QStringLiteral("3so"), Qt::CaseInsensitive) == 0
        || info.suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0) {
        return ExternalDropKind::ProjectJson;
    }
    if (info.fileName().compare(QStringLiteral("C_group"), Qt::CaseInsensitive) == 0
        || info.fileName().compare(QStringLiteral("C_livery"), Qt::CaseInsensitive) == 0) {
        return ExternalDropKind::CGroup;
    }

    if (supportedImageSuffixes().contains(info.suffix().toLower())) {
        return ExternalDropKind::Image;
    }
    return ExternalDropKind::Unsupported;
}

bool MainWindow::handleExternalDropUrls(const QList<QUrl> &urls)
{
    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    if (paths.size() != 1) {
        statusBar()->showMessage(QStringLiteral("Drop one project, C_group, or image file at a time"), 4000);
        return false;
    }

    const QString path = paths.front();
    const ExternalDropKind kind = classifyExternalDropPath(path);
    if (kind == ExternalDropKind::Unsupported) {
        statusBar()->showMessage(QStringLiteral("Unsupported dropped file: %1").arg(QFileInfo(path).fileName()), 4000);
        return false;
    }

    QString error;
    switch (kind) {
    case ExternalDropKind::ProjectJson:
        if (!confirmDiscardUnsavedChanges()) {
            return false;
        }
        rememberImportDirectory(path, QStringLiteral("projectJson"));
        if (!loadProjectJson(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("Open failed"), error);
            return false;
        }
        return true;
    case ExternalDropKind::CGroup:
        if (!confirmDiscardUnsavedChanges()) {
            return false;
        }
        if (!importAny(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("Import failed"), error);
            return false;
        }
        return true;
    case ExternalDropKind::Image:
        rememberImportDirectory(path, QStringLiteral("guideLayer"));
        if (!importGuideLayer(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("Guide layer import failed"), error);
            return false;
        }
        return true;
    case ExternalDropKind::Unsupported:
        break;
    }
    return false;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile() && classifyExternalDropPath(url.toLocalFile()) != ExternalDropKind::Unsupported) {
                event->setDropAction(Qt::CopyAction);
                event->accept();
                return;
            }
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QMainWindow::dragMoveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        if (handleExternalDropUrls(event->mimeData()->urls())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }
    QMainWindow::dropEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (confirmDiscardUnsavedChanges()) {
        event->accept();
    } else {
        event->ignore();
    }
}

bool MainWindow::confirmDiscardUnsavedChanges()
{
    if (state_ == nullptr || !state_->isModified()) {
        return true;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unsaved Changes"));
    box.setText(QStringLiteral("This project has unsaved changes."));
    box.setInformativeText(QStringLiteral("How would you like to save before closing?"));
    QPushButton *saveJsonBtn = box.addButton(QStringLiteral("Save project file"), QMessageBox::AcceptRole);
    QPushButton *dontSaveBtn = box.addButton(QStringLiteral("Don't Save"), QMessageBox::DestructiveRole);
    QPushButton *cancelBtn = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(saveJsonBtn);
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked == dontSaveBtn) {
        return true;
    }
    if (clicked == saveJsonBtn) {
        saveProjectJsonDialog();
        // A cancelled or failed save leaves the project modified; abort the close.
        return !state_->isModified();
    }
    // Cancel or the dialog being dismissed: stay open.
    Q_UNUSED(cancelBtn);
    return false;
}

void MainWindow::updateWindowTitle()
{
    const QString base = QStringLiteral("Forza Livery Studio");
    const bool dirty = state_ != nullptr && state_->isModified();
    setWindowTitle(dirty ? base + QStringLiteral(" *") : base);
}

bool MainWindow::loadProject(const QString &path, QString *error)
{
    try {
        setProject(fh6::importCGroupNested(path));
        statusBar()->showMessage(QStringLiteral("Imported %1").arg(path), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::loadLivery(const QString &path, QString *error)
{
    try {
        setProject(fh6::importCLivery(path));
        statusBar()->showMessage(QStringLiteral("Imported livery %1").arg(path), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::exportFolderImpl(const QString &folder, QString *error)
{
    if (!state_->hasProject_) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    try {
        fh6::Project exportProject = state_->project_;
        if (exportProject.isLivery) {
            // Liveries export as a C_livery folder (container rebuilt from the imported
            // source, with the current target car id), not as a C_group.
            const QString targetFolder =
                projectExportFolder(folder, exportProject.name, true);
            fh6::exportCLivery(exportProject, targetFolder);
            statusBar()->showMessage(QStringLiteral("Exported livery %1").arg(targetFolder), 5000);
            return true;
        }
        if (projectContainsRasterLogo(exportProject)) {
            throw std::runtime_error("logo layers can only be exported from livery projects");
        }
        fh6::HeaderMetadata meta;
        if (exportProject.headerMetadata) {
            meta = *exportProject.headerMetadata;
        } else if (!exportProject.sourceHeader.isEmpty()) {
            try {
                meta = fh6::parseHeader(exportProject.sourceHeader);
            } catch (const std::exception &) {
                meta = fh6::defaultDraftHeader(exportProject.name, creatorName_);
            }
            // A published imported header only decodes name + description; its draft-only
            // fields (creator, date, guid, type, ...) are never parsed. Flipping the flag
            // alone would export a blank stub, so rebuild a clean draft instead - the export
            // is always unpublished for projects imported from a C_group file.
            if (meta.published) {
                fh6::HeaderMetadata draft = fh6::defaultDraftHeader(exportProject.name, creatorName_);
                draft.name = meta.name;
                meta = draft;
            }
        } else {
            meta = fh6::defaultDraftHeader(exportProject.name, creatorName_);
        }
        meta.published = false;
        meta.description.clear();
        exportProject.sourceHeader.clear();
        exportProject.headerMetadata = meta;

        const QString targetFolder = projectExportFolder(folder, exportProject.name, exportProject.isLivery);
        // Grouped (nested) export is the single canonical export path. It needs each
        // shape's sprite size to place group origins.
        ShapeGeometryStore geometry;
        geometry.loadDefault();
        const fh6::SpriteSizeFn spriteSize = [&geometry](quint16 id) {
            return geometry.shapeSize(static_cast<int>(id));
        };
        fh6::exportNestedProjectFolder(exportProject, targetFolder, exportProject.name, spriteSize);
        const QImage thumb = renderProjectPreviewImage(exportProject, QSize(256, 256));
        if (!QImageWriter::supportedImageFormats().contains("webp")) {
            throw std::runtime_error("Qt WEBP image writer is not available; ensure qwebp.dll is deployed in the imageformats plugin folder");
        } else if (!thumb.isNull() && !thumb.save(QDir(targetFolder).filePath(QStringLiteral("thumb.webp")), "WEBP")) {
            throw std::runtime_error("could not write thumb.webp");
        }
        statusBar()->showMessage(QStringLiteral("Exported %1").arg(targetFolder), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::newProject(QString *error)
{
    return newProject(false, error);
}

bool MainWindow::newProject(bool livery, QString *error, int carId)
{
    Q_UNUSED(error);
    setProject(createNewProject(livery, creatorName_, carId));
    statusBar()->showMessage(livery ? QStringLiteral("New livery project created")
                                    : QStringLiteral("New project created"),
                             5000);
    return true;
}

bool MainWindow::saveProjectJson(const QString &path, QString *error)
{
    if (!state_->hasProject_) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    try {
        // Projects always save to the `.3so` container. A legacy `.json` target is
        // migrated: write the `.3so` sibling and delete the old `.json` on success.
        const bool wasLegacyJson =
            QFileInfo(path).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0;
        const QString targetPath = wasLegacyJson
            ? path.chopped(4) + QStringLiteral("3so")  // ".json" -> ".3so"
            : path;

        const QByteArray bytes = fh6::encodeProjectDocument(state_->project_);
        QFile file(targetPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            throw std::runtime_error(("could not open project file for writing: " + targetPath).toStdString());
        }
        if (file.write(bytes) != bytes.size()) {
            throw std::runtime_error("short write while saving project");
        }
        file.close();
        if (wasLegacyJson) {
            QFile::remove(path);  // targetPath is the .3so sibling, always distinct
        }
        projectJsonPath_ = targetPath;
        rememberRecentProjectJson(targetPath);
        state_->setModified(false);
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(targetPath), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::loadProjectJson(const QString &path, QString *error)
{
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            throw std::runtime_error(("could not open project file: " + path).toStdString());
        }
        setProject(fh6::decodeProjectDocument(file.readAll()));
        projectJsonPath_ = path;  // set after setProject (which clears it)
        rememberRecentProjectJson(path);
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(path), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::importAny(const QString &path, QString *error)
{
    const QFileInfo info(path);
    bool isLivery = false;
    if (info.isDir()) {
        // A folder is a livery source when it holds a C_livery file, otherwise treat
        // it as a C_group folder (importCGroupNested resolves the C_group inside).
        isLivery = QFileInfo(QDir(path).filePath(QStringLiteral("C_livery"))).isFile();
    } else {
        isLivery = info.fileName().compare(QStringLiteral("C_livery"), Qt::CaseInsensitive) == 0;
    }

    if (isLivery) {
        rememberImportDirectory(path, QStringLiteral("source"));
        rememberImportDirectory(path, QStringLiteral("liveryFolder"));
        return loadLivery(path, error);
    }
    rememberImportDirectory(path, QStringLiteral("source"));
    rememberImportDirectory(path, info.isDir() ? QStringLiteral("cgroupFolder") : QStringLiteral("cgroupFile"));
    return loadProject(path, error);
}

void MainWindow::importFileDialog()
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Import C_group or C_livery"),
                                                      importDialogStartDirectoryWithFallbacks(
                                                          this,
                                                          QStringLiteral("source"),
                                                          QStringList{
                                                              QStringLiteral("cgroupFile"),
                                                              QStringLiteral("liveryFolder"),
                                                              QStringLiteral("cgroupFolder"),
                                                          }),
                                                      QStringLiteral("Forza source (C_group C_livery);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    if (!importAny(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Import failed"), error);
    }
}

void MainWindow::importGuideLayerDialog()
{
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Add Guide Layer"),
                                                      importDialogStartDirectory(this, QStringLiteral("guideLayer")),
                                                      imageDialogFilter());
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("guideLayer"));

    QString error;
    if (!importGuideLayer(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Guide layer import failed"), error);
    }
}

void MainWindow::exportDialog()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Open a project before exporting."));
        return;
    }

    // Livery export is disabled for now: the container/retarget encoder is confirmed
    // in-game but full artwork synthesis is still pending, so exporting would produce
    // an incomplete C_livery. The encoder (exportCLivery) is kept intact for when the
    // remaining work lands; we just refuse to invoke it here. (See docs/LIVERY_ENCODER.md.)
    if (state_->project_.isLivery) {
        QMessageBox::warning(this, QStringLiteral("Livery export unavailable"),
                             QStringLiteral("Exporting liveries is not available yet.\n\n"
                                            "The livery encoder is still under development and does not yet "
                                            "synthesize the full artwork payload, so a livery cannot be exported "
                                            "to a game-ready C_livery at this time."));
        return;
    }

    // Export is the grouped (nested) path only: it preserves group structure and is
    // the single canonical export. (The legacy flat export option was removed.)
    const QString folder = QFileDialog::getExistingDirectory(this,
                                                            QStringLiteral("Export Folder"),
                                                            importDialogStartDirectoryWithFallbacks(
                                                                this,
                                                                QStringLiteral("exportFolder"),
                                                                QStringList{
                                                                    QStringLiteral("exportNested"),
                                                                }));
    if (folder.isEmpty()) {
        return;
    }
    rememberImportDirectory(folder, QStringLiteral("exportFolder"));
    rememberImportDirectory(folder, QStringLiteral("exportNested"));

    QString error;
    if (!exportFolderImpl(folder, &error)) {
        QMessageBox::critical(this, QStringLiteral("Export failed"), error);
    }
}

void MainWindow::newProjectDialog()
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("New Project"));
    box.setText(QStringLiteral("Create a new project type."));
    QPushButton *groupButton = box.addButton(QStringLiteral("Layer Group"), QMessageBox::AcceptRole);
    QPushButton *liveryButton = box.addButton(QStringLiteral("Livery"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(groupButton);
    box.exec();
    if (box.clickedButton() != groupButton && box.clickedButton() != liveryButton) {
        return;
    }

    const bool livery = box.clickedButton() == liveryButton;
    int carId = 0;
    if (livery) {
        // A livery must name its target car (written to the C_livery vlrc header
        // on export); the picker is the only place a fresh project gets it.
        if (!chooseCarModel(this, 0, &carId)) {
            return;  // cancelled: abort creation rather than default to a car
        }
    }

    QString error;
    if (!newProject(livery, &error, carId)) {
        QMessageBox::critical(this, QStringLiteral("New project failed"), error);
    }
}

void MainWindow::saveProjectJsonDialog()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Save Project"), QStringLiteral("Create or open a project before saving."));
        return;
    }

    // If this project already has an associated file, overwrite it silently instead
    // of prompting. A legacy .json is migrated to .3so by saveProjectJson.
    if (!projectJsonPath_.isEmpty()) {
        QString error;
        if (!saveProjectJson(projectJsonPath_, &error)) {
            QMessageBox::critical(this, QStringLiteral("Save failed"), error);
        }
        return;
    }

    const QString suggested = state_->project_.name.isEmpty() ? QStringLiteral("project") : state_->project_.name;
    const QString suggestedPath = QDir(importDialogStartDirectory(this, QStringLiteral("projectJson")))
                                      .filePath(suggested + QStringLiteral(".3so"));
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("Save Project"),
                                                      suggestedPath,
                                                      QStringLiteral("Forza Project (*.3so);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("projectJson"));

    QString error;
    if (!saveProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Save failed"), error);
    }
}

void MainWindow::autosaveProject()
{
    if (state_ == nullptr || !state_->hasProject() || !state_->isModified() || projectJsonPath_.isEmpty()) {
        return;
    }

    QString error;
    if (!saveProjectJson(projectJsonPath_, &error)) {
        statusBar()->showMessage(QStringLiteral("Autosave failed: %1").arg(error), 5000);
    }
}

void MainWindow::loadProjectJsonDialog()
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Open Project"),
                                                      importDialogStartDirectory(this, QStringLiteral("projectJson")),
                                                      QStringLiteral("Forza Project (*.3so *.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("projectJson"));

    QString error;
    if (!loadProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Open failed"), error);
    }
}

void MainWindow::openRecentProjectJson(const QString &path)
{
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    QString error;
    if (!loadProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Open failed"), error);
        refreshRecentProjectJsonMenu();
    }
}

void MainWindow::rememberRecentProjectJson(const QString &path)
{
    const QFileInfo info(path);
    if (!isProjectDocumentFile(info)) {
        return;
    }
    QStringList recent = QSettings().value(QStringLiteral("recent/projectJsons")).toStringList();
    recent.removeAll(info.absoluteFilePath());
    recent.push_front(info.absoluteFilePath());
    while (recent.size() > 10) {
        recent.removeLast();
    }
    QSettings().setValue(QStringLiteral("recent/projectJsons"), recent);
    refreshRecentProjectJsonMenu();
}

void MainWindow::refreshRecentProjectJsonMenu()
{
    if (recentProjectMenu_ == nullptr) {
        return;
    }
    recentProjectMenu_->clear();
    QStringList kept;
    const QStringList recent = QSettings().value(QStringLiteral("recent/projectJsons")).toStringList();
    for (const QString &path : recent) {
        const QFileInfo info(path);
        if (!isProjectDocumentFile(info) || kept.contains(info.absoluteFilePath())) {
            continue;
        }
        kept.push_back(info.absoluteFilePath());
        QAction *action = recentProjectMenu_->addAction(info.fileName());
        action->setToolTip(info.absoluteFilePath());
        connect(action, &QAction::triggered, this, [this, path = info.absoluteFilePath()]() {
            openRecentProjectJson(path);
        });
    }
    QSettings().setValue(QStringLiteral("recent/projectJsons"), kept);
    if (kept.isEmpty()) {
        QAction *empty = recentProjectMenu_->addAction(QStringLiteral("(No recent projects)"));
        empty->setEnabled(false);
    }
}

void MainWindow::refreshHeaderMetadataWidget()
{
    if (headerMetadata_ == nullptr) {
        return;
    }

    if (!state_->hasProject_) {
        headerMetadata_->setMetadata({}, false, false);
        return;
    }

    // Seed editable metadata: prefer existing metadata, else parse imported header bytes,
    // else start from sensible draft defaults.
    fh6::HeaderMetadata meta;
    if (state_->project_.headerMetadata) {
        meta = *state_->project_.headerMetadata;
    } else if (!state_->project_.sourceHeader.isEmpty()) {
        try {
            meta = fh6::parseHeader(state_->project_.sourceHeader);
        } catch (const std::exception &) {
            meta = fh6::defaultDraftHeader(state_->project_.name, creatorName_);
        }
    } else {
        meta = fh6::defaultDraftHeader(state_->project_.name, creatorName_);
    }
    if (meta.name.isEmpty()) {
        meta.name = state_->project_.name;
    }

    const bool importedDraft = !state_->project_.sourceHeader.isEmpty();
    headerMetadata_->setMetadata(meta, importedDraft, true);
}

void MainWindow::showHeaderMetadataDock()
{
    if (headerMetadataDock_ == nullptr) {
        return;
    }
    refreshHeaderMetadataWidget();
    headerMetadataDock_->show();
    headerMetadataDock_->raise();
    if (headerMetadata_ != nullptr) {
        headerMetadata_->setFocus();
    }
}

void MainWindow::applyHeaderMetadata()
{
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Header Metadata"), QStringLiteral("Create or open a project first."));
        return;
    }

    const fh6::HeaderMetadata meta = headerMetadata_->metadata();
    const bool importedDraft = !state_->project_.sourceHeader.isEmpty();
    const bool rebuild = headerMetadata_->rebuildRequested();

    state_->project_.name = meta.name;
    creatorName_ = meta.creatorName;
    QSettings().setValue(QStringLiteral("header/creatorName"), creatorName_);

    if (importedDraft && !rebuild) {
        // Keep the byte-exact source header; export will rename it to the new name.
        state_->project_.headerMetadata = meta; // store edits for reference / future rebuilds
    } else {
        state_->project_.sourceHeader.clear(); // export will synthesize from metadata
        state_->project_.headerMetadata = meta;
    }
    updateStatus();
    refreshHeaderMetadataWidget(); // reflect the (possibly dropped) source-header state
    statusBar()->showMessage(QStringLiteral("Header metadata updated"), 5000);
}

void MainWindow::saveLayout()
{
    QSettings settings;
    // Persist geometry alongside the dock state. restoreState() stores dock
    // sizes relative to the window size at save time, so the geometry must be
    // restored first or the splits (e.g. a bottom-docked Shapes panel) drift.
    settings.setValue(QStringLiteral("layout/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("layout/state"), saveState());
    statusBar()->showMessage(QStringLiteral("Layout saved"), 5000);
}

bool MainWindow::restoreLayout()
{
    QSettings settings;
    const QByteArray state = settings.value(QStringLiteral("layout/state")).toByteArray();
    if (state.isEmpty()) {
        return false;
    }
    // Restore geometry before state so restoreState() reconstructs dock sizes
    // against the same window size they were saved with.
    const QByteArray geometry = settings.value(QStringLiteral("layout/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    return restoreState(state);
}

void MainWindow::resetLayout()
{
    if (!defaultLayoutState_.isEmpty()) {
        restoreState(defaultLayoutState_);
        syncDockCollapseButtons();
    }
    QSettings settings;
    settings.remove(QStringLiteral("layout/state"));
    settings.remove(QStringLiteral("layout/geometry"));
    statusBar()->showMessage(QStringLiteral("Layout reset to default"), 5000);
}

} // namespace gui
