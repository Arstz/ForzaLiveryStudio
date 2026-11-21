#include "main_window.h"

#include "main_window_internal.h"

#include "car_preview_widget.h"
#include "car_registry.h"
#include "export_asset_dialog.h"
#include "fls_core.h"
#include "gui_constants.h"
#include "header_metadata_widget.h"
#include "image_io.h"
#include "image_preprocess_dialog.h"
#include "import_asset_dialog.h"
#include "layer.h"
#include "livery_codec.h"
#include "project_codec.h"
#include "shape_geometry_store.h"
#include "shapes_browser_widget.h"
#include "xbox360_vinyl.h"

#include <QtGui>

#include <QInputDialog>
#include <QLockFile>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <exception>
#include <iterator>
#include <memory>
#include <stdexcept>
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
constexpr char kScratchProjectSuffix[] = ".3so";
constexpr char kScratchMetadataSuffix[] = ".json";
constexpr char kScratchLockSuffix[] = ".lock";
constexpr quint32 kFh5HeaderVersion = 4;
const QSize kWgsSmallThumbnailSize(128, 72);

QString absoluteProjectPath(const QString &path) {
    return QFileInfo(path).absoluteFilePath();
}

QString scratchRootPath() {
    QString dataRoot = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (dataRoot.isEmpty()) {
        dataRoot = QStandardPaths::writableLocation(
            QStandardPaths::TempLocation);
    }

    return QDir(dataRoot).filePath(QStringLiteral("scratch"));
}

QString scratchSessionPath(const QString &sessionId,
                           const char *suffix) {
    return QDir(scratchRootPath()).filePath(
        sessionId + QString::fromLatin1(suffix));
}

void removeScratchSessionFiles(const QString &sessionId) {
    if (sessionId.isEmpty()) {
        return;
    }
    QFile::remove(scratchSessionPath(sessionId, kScratchProjectSuffix));
    QFile::remove(scratchSessionPath(sessionId, kScratchMetadataSuffix));
}

void writeBytesAtomically(const QByteArray &bytes,
                          const QString &path) {
    const QFileInfo target(path);
    if (!QDir().mkpath(target.absolutePath())) {
        throw std::runtime_error(
            ("could not create project directory: "
             + target.absolutePath()).toStdString());
    }
    QSaveFile file(target.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(
            ("could not open project file for writing: "
             + target.absoluteFilePath()).toStdString());
    }
    if (file.write(bytes) != bytes.size()) {
        throw std::runtime_error("short write while saving project");
    }
    if (!file.commit()) {
        throw std::runtime_error(
            ("could not commit project file: "
             + target.absoluteFilePath()).toStdString());
    }
}

void writeProjectDocument(const fls::Project &project,
                          const QString &path) {
    writeBytesAtomically(fls::encodeProjectDocument(project), path);
}

fls::Project readProjectDocument(const QString &path) {
    QFile projectFile(path);
    if (!projectFile.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            ("could not open project file: " + path).toStdString());
    }
    const QByteArray savedBytes = projectFile.readAll();

    return fls::decodeProjectDocument(savedBytes);
}

QString generatedGroupId(int index) {
    return QStringLiteral("group-%1").arg(index, 4, 10, QLatin1Char('0'));
}

QByteArray readImportFile(const QString &path, bool optional = false) {
    if (path.isEmpty() && optional) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open import file: " + path).toStdString());
    }

    return file.readAll();
}

std::optional<fls::Xbox360Credentials> chooseXbox360KeyFile(QWidget *parent, fls::ExportGameVersion game) {
    constexpr qint64 kMaximumKeyFileSize = 48;
    const QString path = QFileDialog::getOpenFileName(parent,
        QStringLiteral("Select %1 Xbox 360 Key File").arg(fls::exportGameCode(game)),
        QString(), QStringLiteral("Profile-derived keys (*.keys)"));
    if (path.isEmpty()) {
        return std::nullopt;
    }
    if (QFileInfo(path).suffix().compare(QStringLiteral("keys"), Qt::CaseInsensitive) != 0) {
        throw std::runtime_error("select an Xbox 360 .keys file");
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("could not read the Xbox 360 key file");
    }
    fls::Xbox360Credentials credentials;
    credentials.keyData = file.read(kMaximumKeyFileSize + 1);
    if (file.error() != QFileDevice::NoError) {
        throw std::runtime_error("could not read the Xbox 360 key file");
    }

    return credentials;
}

std::optional<fls::ExportGameVersion> xbox360ImportGame(QWidget *parent, fls::Xbox360Envelope envelope) {
    if (envelope == fls::Xbox360Envelope::Sha1) {
        return fls::ExportGameVersion::Motorsport3;
    }
    if (envelope != fls::Xbox360Envelope::Sha256) {
        throw std::runtime_error("unrecognized Xbox 360 vinyl file");
    }
    bool accepted = false;
    const QStringList games{QStringLiteral("Forza Motorsport 4"), QStringLiteral("Forza Horizon 1")};
    const QString selected = QInputDialog::getItem(parent, QStringLiteral("Xbox 360 Import"),
        QStringLiteral("Source game"), games, 0, false, &accepted);
    if (!accepted) {
        return std::nullopt;
    }

    return selected == games.front() ? fls::ExportGameVersion::Motorsport4 : fls::ExportGameVersion::Horizon1;
}

fls::Project createNewProject(bool livery, const QString &creatorName, int carId) {
    fls::Project project;
    project.name = QStringLiteral("Untitled");
    project.isLivery = livery;
    project.carId = livery ? carId : 0;
    project.headerMetadata = fls::defaultDraftHeader(project.name, creatorName,
                                                      static_cast<quint32>(project.carId));
    if (livery && project.root) {
        for (int slot = 0; slot < static_cast<int>(std::size(kEmptyLiverySectionNames)); ++slot) {
            auto section = std::make_unique<fls::scene::Group>();
            section->id = generatedGroupId(slot + 1);
            section->name = QString::fromLatin1(kEmptyLiverySectionNames[slot]);
            section->isLiverySection = true;
            section->liverySectionSlot = slot;
            project.root->append(std::move(section));
        }
    }
    return project;
}

bool containsRasterLogo(const fls::scene::Layer &node) {
    if (node.kind() == fls::scene::LayerKind::Shape) {
        return static_cast<const fls::scene::Shape &>(node).raster;
    }
    if (node.kind() != fls::scene::LayerKind::Group) {
        return false;
    }
    const auto &group = static_cast<const fls::scene::Group &>(node);
    for (const auto &child : group.children) {
        if (containsRasterLogo(*child)) {
            return true;
        }
    }
    return false;
}

bool projectContainsRasterLogo(const fls::Project &project) {
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

fls::HeaderMetadata effectiveHeaderMetadata(const fls::Project &project,
                                            const QString &creatorName) {
    if (project.headerMetadata) {
        return *project.headerMetadata;
    }
    if (!project.sourceHeader.isEmpty()) {
        try {
            return fls::parseHeader(project.sourceHeader);
        } catch (const std::exception &) {
        }
    }

    return fls::defaultDraftHeader(project.name, creatorName,
                                   static_cast<quint32>(project.carId));
}

} // namespace

void MainWindow::initializeScratchSession() {
    const QString rootPath = scratchRootPath();
    if (!QDir().mkpath(rootPath)) {
        qWarning().noquote() << QStringLiteral(
            "Could not create scratch directory: %1").arg(rootPath);
        return;
    }

    scratchSessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    scratchLock_ = std::make_unique<QLockFile>(
        scratchSessionPath(scratchSessionId_, kScratchLockSuffix));
    if (!scratchLock_->tryLock()) {
        qWarning().noquote() << QStringLiteral(
            "Could not lock scratch session: %1").arg(scratchSessionId_);
        scratchLock_.reset();
        scratchSessionId_.clear();
    }
}

bool MainWindow::writeScratchCopy(QString *error) {
    if (state_ == nullptr || !state_->hasProject()
        || scratchSessionId_.isEmpty() || scratchLock_ == nullptr) {
        return false;
    }

    try {
        QJsonObject metadata;
        metadata.insert(QStringLiteral("project_path"), projectJsonPath_);
        metadata.insert(
            QStringLiteral("updated_utc"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        writeProjectDocument(
            state_->project_,
            scratchSessionPath(scratchSessionId_, kScratchProjectSuffix));
        writeBytesAtomically(
            QJsonDocument(metadata).toJson(QJsonDocument::Compact),
            scratchSessionPath(scratchSessionId_, kScratchMetadataSuffix));
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

void MainWindow::saveScratchProject() {
    QString error;
    if (!writeScratchCopy(&error) && !error.isEmpty()) {
        canvasStatusBar_->showMessage(
            QStringLiteral("Recovery save failed: %1").arg(error), 5000);
    }
}

void MainWindow::clearScratchCopy() {
    removeScratchSessionFiles(scratchSessionId_);
}

bool MainWindow::recoverUnexpectedScratchProject() {
    QDir scratchDirectory(scratchRootPath());
    const QFileInfoList lockFiles = scratchDirectory.entryInfoList(
        QStringList{QStringLiteral("*.lock")}, QDir::Files);
    for (const QFileInfo &lockInfo : lockFiles) {
        const QString sessionId = lockInfo.completeBaseName();
        if (sessionId == scratchSessionId_
            || QFileInfo::exists(scratchSessionPath(
                sessionId, kScratchMetadataSuffix))
            || QFileInfo::exists(scratchSessionPath(
                sessionId, kScratchProjectSuffix))) {
            continue;
        }
        QLockFile orphanLock(lockInfo.absoluteFilePath());
        if (orphanLock.tryLock()) {
            orphanLock.unlock();
        }
    }

    const QFileInfoList metadataFiles = scratchDirectory.entryInfoList(
        QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Time);
    for (const QFileInfo &metadataInfo : metadataFiles) {
        const QString sessionId = metadataInfo.completeBaseName();
        if (sessionId == scratchSessionId_) {
            continue;
        }

        QLockFile candidateLock(
            scratchSessionPath(sessionId, kScratchLockSuffix));
        if (!candidateLock.tryLock()) {
            continue;
        }

        const QString projectScratchPath = scratchSessionPath(
            sessionId, kScratchProjectSuffix);
        QFile projectScratchFile(projectScratchPath);
        QFile metadataFile(metadataInfo.absoluteFilePath());
        if (!projectScratchFile.open(QIODevice::ReadOnly)) {
            removeScratchSessionFiles(sessionId);
            continue;
        }
        const QByteArray scratchBytes = projectScratchFile.readAll();
        projectScratchFile.close();
        QJsonObject metadata;
        if (metadataFile.open(QIODevice::ReadOnly)) {
            const QJsonDocument document = QJsonDocument::fromJson(
                metadataFile.readAll());
            if (document.isObject()) {
                metadata = document.object();
            }
            metadataFile.close();
        }
        const QString sourcePath = metadata.value(
            QStringLiteral("project_path")).toString();
        const QDateTime updatedUtc = QDateTime::fromString(
            metadata.value(QStringLiteral("updated_utc")).toString(),
            Qt::ISODateWithMs);
        if (!sourcePath.isEmpty()) {
            QFile sourceFile(sourcePath);
            if (sourceFile.open(QIODevice::ReadOnly)
                && sourceFile.readAll() == scratchBytes) {
                removeScratchSessionFiles(sessionId);
                continue;
            }
        }

        fls::Project recoveredProject;
        try {
            recoveredProject = fls::decodeProjectDocument(scratchBytes);
        } catch (const std::exception &) {
            removeScratchSessionFiles(sessionId);
            continue;
        }

        const QString projectLabel = sourcePath.isEmpty()
            ? recoveredProject.name
            : QFileInfo(sourcePath).fileName();
        const QString recoveryTime = updatedUtc.isValid()
            ? QLocale().toString(updatedUtc.toLocalTime(), QLocale::ShortFormat)
            : QLocale().toString(metadataInfo.lastModified(), QLocale::ShortFormat);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("Recover Project"));
        box.setText(QStringLiteral(
            "An editor session ended without closing properly."));
        box.setInformativeText(
            QStringLiteral("Recover %1 from %2?")
                .arg(projectLabel.isEmpty() ? QStringLiteral("Untitled") : projectLabel,
                     recoveryTime));
        QPushButton *recoverButton = box.addButton(
            QStringLiteral("Recover"), QMessageBox::AcceptRole);
        QPushButton *discardButton = box.addButton(
            QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        box.addButton(QStringLiteral("Not Now"), QMessageBox::RejectRole);
        box.setDefaultButton(recoverButton);
        box.exec();
        if (box.clickedButton() == recoverButton) {
            setProject(std::move(recoveredProject));
            projectJsonPath_ = sourcePath;
            state_->setModified(true);
            removeScratchSessionFiles(sessionId);
            writeScratchCopy();
            if (!sourcePath.isEmpty()) {
                rememberRecentProjectJson(sourcePath);
            }
            canvasStatusBar_->showMessage(
                QStringLiteral("Recovered %1")
                    .arg(projectLabel.isEmpty()
                             ? QStringLiteral("Untitled") : projectLabel),
                5000);
            return true;
        }
        if (box.clickedButton() == discardButton) {
            removeScratchSessionFiles(sessionId);
            continue;
        }
        return false;
    }

    return false;
}

MainWindow::ExternalDropKind MainWindow::classifyExternalDropPath(const QString &path) const {
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

    if (isProjectDocumentFile(info)) {
        return ExternalDropKind::ProjectJson;
    }
    if (info.fileName().compare(QStringLiteral("C_group"), Qt::CaseInsensitive) == 0
        || fls::isLiveryAssetFileName(info.fileName())
        || fls::xbox360FileKind(info.fileName()) != fls::Xbox360FileKind::Unknown) {
        return ExternalDropKind::CGroup;
    }

    if (supportedImageSuffixes().contains(info.suffix().toLower())) {
        return ExternalDropKind::Image;
    }
    return ExternalDropKind::Unsupported;
}

bool MainWindow::handleExternalDropUrls(const QList<QUrl> &urls) {
    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    if (paths.size() != 1) {
        canvasStatusBar_->showMessage(QStringLiteral("Drop one project, C_group, or image file at a time"), 4000);
        return false;
    }

    const QString path = paths.front();
    const ExternalDropKind kind = classifyExternalDropPath(path);
    if (kind == ExternalDropKind::Unsupported) {
        canvasStatusBar_->showMessage(QStringLiteral("Unsupported dropped file: %1").arg(QFileInfo(path).fileName()), 4000);
        return false;
    }
    if (kind != ExternalDropKind::Image && !confirmDiscardUnsavedChanges()) {
        return false;
    }

    QString error;
    switch (kind) {
    case ExternalDropKind::ProjectJson:
        rememberImportDirectory(path, QStringLiteral("projectJson"));
        if (!loadProjectJson(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("Open failed"), error);
            return false;
        }
        return true;
    case ExternalDropKind::CGroup:
        if (!importAny(path, &error)) {
            if (!error.isEmpty()) {
                QMessageBox::critical(this, QStringLiteral("Import failed"), error);
            }
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

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
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

void MainWindow::dragMoveEvent(QDragMoveEvent *event) {
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QMainWindow::dragMoveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event) {
    if (event != nullptr && event->mimeData() != nullptr && event->mimeData()->hasUrls()) {
        if (handleExternalDropUrls(event->mimeData()->urls())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
    }
    QMainWindow::dropEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (restartRequested_ || confirmDiscardUnsavedChanges()) {
        saveLayout();
        clearScratchCopy();
        event->accept();
    } else {
        event->ignore();
    }
}

bool MainWindow::confirmDiscardUnsavedChanges() {
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
        clearScratchCopy();
        return true;
    }
    if (clicked == saveJsonBtn) {
        saveProjectJsonDialog();
        return !state_->isModified();
    }
    Q_UNUSED(cancelBtn);
    return false;
}

void MainWindow::updateWindowTitle() {
    const QString base = QStringLiteral("Forza Livery Studio");
    const bool dirty = state_ != nullptr && state_->isModified();
    setWindowTitle(dirty ? base + QStringLiteral(" *") : base);
}

bool MainWindow::loadProject(const QString &path, QString *error) {
    return loadImportedProject([&path]() { return fls::importCGroupNested(path); },
                               QStringLiteral("Imported %1").arg(path), error);
}

bool MainWindow::loadLivery(const QString &path, QString *error) {
    return loadImportedProject([&path]() { return fls::importCLivery(path); },
                               QStringLiteral("Imported livery %1").arg(path), error);
}

bool MainWindow::loadImportedProject(const std::function<fls::Project()> &load,
                                     const QString &statusMessage,
                                     QString *error) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        fls::Project project = load();
        const CarRegistry &registry = sharedCarRegistry();
        const auto gameFolders = loadBehaviorSettings().gameFolders;
        CarRegistry::ModelSource alternativeSource;
        const bool missingModel = project.isLivery && project.carId > 0
            && registry.findModel(project.carId, gameFolders, project.liveryPaint.game).path.isEmpty();

        if (missingModel) {
            const int alternativeCarId = registry.findClosestModelCarId(project.carId, gameFolders,
                                                                        project.liveryPaint.game);
            const QString alternativeName = registry.name(alternativeCarId);
            const QString targetGame = project.liveryPaint.game.isEmpty()
                ? QStringLiteral("unknown game") : project.liveryPaint.game;
            const auto choice = chooseMissingCarModel(this, project.carId, targetGame, alternativeName);
            if (choice == MissingCarModelChoice::Cancel) {
                return false;
            }
            if (choice == MissingCarModelChoice::LoadClosest) {
                alternativeSource = chooseCarModelSource(this, alternativeName,
                    registry.findModels(alternativeCarId, gameFolders, project.liveryPaint.game));
            }
        }
        setProject(std::move(project), missingModel);
        if (!alternativeSource.path.isEmpty()) {
            loadCarModel(alternativeSource.path, true, alternativeSource.game);
        }
        canvasStatusBar_->showMessage(statusMessage, 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::exportFolderImpl(const QString &folder, fls::ExportGameVersion game, QString *error,
                                  const fls::WgsExportTarget *wgsTarget) {
    if (!state_->hasProject_) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }
    try {
        fls::Project exportProject = state_->project_;
        std::unique_ptr<QTemporaryDir> staging;
        if (wgsTarget != nullptr) {
            fls::HeaderMetadata metadata = effectiveHeaderMetadata(exportProject, creatorName_);
            metadata.formatVersion = kFh5HeaderVersion;
            metadata.guid = fls::parseHeader(wgsTarget->headerBytes).guid;
            metadata.creatorTag = wgsTarget->creatorTag;
            metadata.published = false;
            metadata.description.clear();
            exportProject.sourceHeader.clear();
            exportProject.headerMetadata = metadata;
            staging = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("fls-wgs-export-XXXXXX")));
            if (!staging->isValid()) {
                throw std::runtime_error("could not create the WGS export staging folder");
            }
        }
        const bool webp = game == fls::ExportGameVersion::Horizon6;
        const char *imageFormat = webp ? "WEBP" : "PNG";
        const QString thumbnailName = fls::exportThumbnailName(game, exportProject.isLivery);
        const QString targetFolder = staging ? staging->path() : game == fls::ExportGameVersion::Motorsport2023
            ? (!QUuid(QDir(folder).dirName()).isNull() ? folder
                : QDir(folder).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)))
            : projectExportFolder(folder, exportProject.name, exportProject.isLivery);
        const auto finishExport = [this, wgsTarget, &targetFolder]() {
            if (wgsTarget == nullptr) {
                canvasStatusBar_->showMessage(QStringLiteral("Exported %1").arg(targetFolder), 5000);
                return;
            }
            QMap<QString, QByteArray> blobs;
            const QDir staged(targetFolder);
            for (const QString &name : staged.entryList(QDir::Files)) {
                const QString key = name.toLower();
                if (!wgsTarget->blobPaths.contains(key)) {
                    continue;
                }
                QFile file(staged.filePath(name));
                if (!file.open(QIODevice::ReadOnly)) {
                    throw std::runtime_error("could not read a staged WGS export blob");
                }
                blobs.insert(key, file.readAll());
            }
            const QString backup = fls::replaceWgsContainer(*wgsTarget, blobs);
            canvasStatusBar_->showMessage(QStringLiteral("Replaced %1 — backup: %2")
                .arg(wgsTarget->containerName, QDir::toNativeSeparators(backup)), 15000);
        };
        if (exportProject.isLivery) {
            QImage thumb;
            const bool incompatibleArtwork = fls::exportIncompatibleLayerCount(exportProject, game) > 0;
            if (incompatibleArtwork) {
                fls::discardIncompatibleExportLayers(exportProject, game);
            }
            if (carPreview_ != nullptr && carPreview_->hasModel()) {
                if (webp && !QImageWriter::supportedImageFormats().contains("webp")) {
                    throw std::runtime_error(
                        "Qt WEBP image writer is not available; ensure qwebp.dll is deployed in the imageformats plugin folder");
                }
                thumb = carPreview_->renderThumbnail(QSize(670, 376), incompatibleArtwork ? &exportProject : nullptr);
                if (thumb.isNull()) {
                    throw std::runtime_error("could not render the livery thumbnail from the car preview");
                }
            }
            fls::exportGameLivery(exportProject, targetFolder, game);
            if (!thumb.isNull()) {
                if (!thumb.save(QDir(targetFolder).filePath(thumbnailName), imageFormat)) {
                    throw std::runtime_error("could not write the livery thumbnail");
                }
                if (wgsTarget != nullptr && wgsTarget->blobPaths.contains(QStringLiteral("smallthumb.png"))) {
                    const QImage previous(wgsTarget->blobPaths.value(QStringLiteral("smallthumb.png")));
                    const QSize size = previous.isNull() ? kWgsSmallThumbnailSize : previous.size();
                    if (!thumb.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(
                            QDir(targetFolder).filePath(QStringLiteral("smallThumb.png")), "PNG")) {
                        throw std::runtime_error("could not write the small WGS livery thumbnail");
                    }
                }
            }
            finishExport();
            return true;
        }
        if (projectContainsRasterLogo(exportProject)) {
            throw std::runtime_error("logo layers can only be exported from livery projects");
        }
        const bool parsedSourceHeader = !exportProject.headerMetadata
            && !exportProject.sourceHeader.isEmpty();
        fls::HeaderMetadata meta = effectiveHeaderMetadata(exportProject, creatorName_);
        if (parsedSourceHeader && meta.published) {
            fls::HeaderMetadata draft = fls::defaultDraftHeader(exportProject.name, creatorName_);
            draft.name = meta.name;
            meta = draft;
        }
        meta.published = false;
        meta.description.clear();
        exportProject.sourceHeader.clear();
        exportProject.headerMetadata = meta;

        ShapeGeometryStore geometry;
        geometry.loadDefault();
        const fls::SpriteSizeFn spriteSize = [&geometry](quint16 id) {
            return geometry.shapeSize(static_cast<int>(id));
        };
        fls::exportGameGroup(exportProject, targetFolder, game, spriteSize);
        fls::discardIncompatibleExportLayers(exportProject, game);
        const QImage thumb = renderProjectPreviewImage(exportProject, QSize(256, 256));
        if (webp && !QImageWriter::supportedImageFormats().contains("webp")) {
            throw std::runtime_error("Qt WEBP image writer is not available; ensure qwebp.dll is deployed in the imageformats plugin folder");
        }
        if (!thumb.isNull() && !thumb.save(QDir(targetFolder).filePath(thumbnailName), imageFormat)) {
            throw std::runtime_error("could not write the group thumbnail");
        }
        finishExport();
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::newProject(QString *error) {
    return newProject(false, error);
}

bool MainWindow::newProject(bool livery, QString *error, int carId) {
    Q_UNUSED(error);
    setProject(createNewProject(livery, creatorName_, carId));
    canvasStatusBar_->showMessage(livery ? QStringLiteral("New livery project created")
                                    : QStringLiteral("New project created"),
                             5000);
    return true;
}

bool MainWindow::saveProjectJson(const QString &path, QString *error) {
    if (!state_->hasProject_) {
        if (error != nullptr) {
            *error = QStringLiteral("no project is loaded");
        }
        return false;
    }

    try {
        const QString targetPath = absoluteProjectPath(path);
        QString scratchError;

        writeProjectDocument(state_->project_, targetPath);
        projectJsonPath_ = targetPath;
        rememberRecentProjectJson(targetPath);
        state_->setModified(false);
        writeScratchCopy(&scratchError);
        canvasStatusBar_->showMessage(
            scratchError.isEmpty()
                ? QStringLiteral("Saved %1").arg(targetPath)
                : QStringLiteral("Saved %1; recovery save failed: %2")
                      .arg(targetPath, scratchError),
            5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::loadProjectJson(const QString &path, QString *error) {
    try {
        const QString targetPath = absoluteProjectPath(path);
        fls::Project loadedProject = readProjectDocument(targetPath);

        setProject(std::move(loadedProject));
        projectJsonPath_ = targetPath;
        rememberRecentProjectJson(targetPath);
        canvasStatusBar_->showMessage(QStringLiteral("Opened %1").arg(targetPath), 5000);
        return true;
    } catch (const std::exception &ex) {
        if (error != nullptr) {
            *error = QString::fromUtf8(ex.what());
        }
        return false;
    }
}

bool MainWindow::importAny(const QString &path, QString *error) {
    const QFileInfo info(path);
    const auto xbox360Kind = fls::xbox360FileKind(info.fileName());
    if (info.isFile() && xbox360Kind != fls::Xbox360FileKind::Unknown) {
        if (error != nullptr) {
            error->clear();
        }
        try {
            QByteArray bytes = readImportFile(info.absoluteFilePath());
            QString gameCode;
            const bool livery = xbox360Kind == fls::Xbox360FileKind::Livery;
            const bool plain = livery ? fls::detail::isXbox360LiveryPayload(bytes)
                || fls::detail::isMotorsport2LiveryContainer(bytes) : bytes.startsWith(fls::detail::kXbox360GroupMagic);
            if (!plain) {
                const auto game = xbox360ImportGame(this, fls::xbox360Envelope(bytes));
                if (!game) {
                    return false;
                }
                const auto credentials = chooseXbox360KeyFile(this, *game);
                if (!credentials) {
                    return false;
                }
                bytes = fls::decryptXbox360Asset(bytes, *game, xbox360Kind, *credentials);
                gameCode = fls::exportGameCode(*game);
            }
            rememberImportDirectory(info.absoluteFilePath(), QStringLiteral("source"));

            return loadImportedProject([&]() {
                fls::Project project = livery
                    ? fls::importCLiveryData(bytes, {}, info.completeBaseName())
                    : fls::importCGroupNestedData(bytes, {}, info.completeBaseName());
                project.sourceFolder = info.absolutePath();
                if (livery && !gameCode.isEmpty()) {
                    project.liveryPaint.game = gameCode;
                }

                return project;
            }, QStringLiteral("Imported %1").arg(path), error);
        } catch (const std::exception &ex) {
            if (error != nullptr) {
                *error = QString::fromUtf8(ex.what());
            }
            return false;
        }
    }
    bool isLivery = false;
    if (info.isDir()) {
        isLivery = QFileInfo(QDir(path).filePath(QStringLiteral("C_livery"))).isFile();
    } else {
        isLivery = fls::isLiveryAssetFileName(info.fileName());
    }
    if (!isLivery) {
        QFile data(info.isDir() ? QDir(path).filePath(QStringLiteral("data")) : path);
        if (data.open(QIODevice::ReadOnly)) {
            isLivery = fls::isLiveryAsset(data.readAll());
        }
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

void MainWindow::importFileDialog() {
    for (;;) {
        const ImportAssetSelection selection = showImportAssetDialog(
            this,
            importBrowserStartDirectory(
                QStringLiteral("sourceBrowser"),
                QStringList{
                    QStringLiteral("source"),
                    QStringLiteral("motorsportFolder"),
                    QStringLiteral("liveryFolder"),
                    QStringLiteral("cgroupFolder"),
                }));
        if (!selection.directory.isEmpty()) {
            rememberImportDirectory(selection.directory, QStringLiteral("sourceBrowser"));
        }
        if (selection.path.isEmpty() || !confirmDiscardUnsavedChanges()) {
            return;
        }

        QString error;
        bool imported = false;
        if (selection.wgsAsset) {
            const fls::WgsAsset asset = *selection.wgsAsset;
            rememberImportDirectory(selection.directory, QStringLiteral("source"));
            rememberImportDirectory(
                selection.directory,
                asset.kind == fls::WgsAssetKind::Livery
                    ? QStringLiteral("liveryFolder")
                    : QStringLiteral("cgroupFolder"));
            imported = loadImportedProject(
                [asset]() {
                    const QByteArray payload = readImportFile(asset.payloadPath);
                    const QByteArray header = readImportFile(asset.headerPath, true);
                    return asset.kind == fls::WgsAssetKind::Livery
                        ? fls::importCLiveryData(payload, header, asset.containerName)
                        : fls::importCGroupNestedData(payload, header, asset.containerName);
                },
                QStringLiteral("Imported WGS asset %1").arg(asset.containerName),
                &error);
        } else {
            if (selection.motorsport) {
                rememberImportDirectory(selection.path, QStringLiteral("motorsportFolder"));
            }
            imported = importAny(selection.path, &error);
        }
        if (imported) {
            return;
        }
        if (!error.isEmpty()) {
            QMessageBox::critical(this, QStringLiteral("Import failed"), error);
            return;
        }
    }
}

void MainWindow::importGuideLayerDialog() {
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

void MainWindow::preprocessSelectedGuide() {
    if (state_ == nullptr || !state_->hasProject()) {
        canvasStatusBar_->showMessage(QStringLiteral("Open or create a project first"), 3500);
        return;
    }
    const QVector<fls::scene::GuideLayer *> guides = state_->selectedGuideLayers();
    if (guides.size() != 1) {
        canvasStatusBar_->showMessage(guides.isEmpty()
                                     ? QStringLiteral("Select a guide layer first")
                                     : QStringLiteral("Select exactly one guide layer"),
                                 3500);
        return;
    }
    fls::scene::GuideLayer *guide = guides.front();
    if (guide == nullptr || guide->locked) {
        canvasStatusBar_->showMessage(QStringLiteral("Unlock the selected guide layer first"), 3500);
        return;
    }
    if (!guide->image) {
        canvasStatusBar_->showMessage(QStringLiteral("The selected guide layer has no image"), 3500);
        return;
    }

    QImage source;
    if (!guide->image->pixels.isEmpty() && guide->image->width > 0 && guide->image->height > 0) {
        source = QImage(reinterpret_cast<const uchar *>(guide->image->pixels.constData()),
                        guide->image->width,
                        guide->image->height,
                        guide->image->width * 4,
                        QImage::Format_ARGB32_Premultiplied).copy();
    } else if (!guide->image->encoded.isEmpty()) {
        source = decodeGuideImage(guide->image->encoded, guide->image->format);
        source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    if (!source.isNull() && !guide->imageTopDown) {
        source = source.mirrored(false, true);
    }
    if (source.isNull()) {
        canvasStatusBar_->showMessage(QStringLiteral("Could not decode the selected guide image"), 4000);
        return;
    }

    const QString guideId = guide->id;
    const QString guideName = guide->name;
    QVector<QColor> projectSwatches;
    projectSwatches.reserve(state_->project_.colorSwatches.size());
    for (const std::array<quint8, 4> &swatch : std::as_const(state_->project_.colorSwatches)) {
        projectSwatches.push_back(QColor(swatch[ColorByteRed], swatch[ColorByteGreen],
                                        swatch[ColorByteBlue], swatch[ColorByteAlpha]));
    }
    ImagePreprocessDialog dialog(source, projectSwatches, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QImage processed = dialog.resultImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (processed.isNull() || processed.size() != source.size()) {
        QMessageBox::critical(this, QStringLiteral("Preprocess Image"),
                              QStringLiteral("Full-resolution image preprocessing failed."));
        return;
    }

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QString embedFormat;
    const QByteArray encoded = encodeGuideImage(processed, &embedFormat);
    QGuiApplication::restoreOverrideCursor();
    if (encoded.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("Preprocess Image"),
                              QStringLiteral("Could not encode the processed guide image."));
        return;
    }

    fls::scene::Layer *node = state_->sceneNode(guideId);
    if (node == nullptr || node->kind() != fls::scene::LayerKind::Guide) {
        QMessageBox::critical(this, QStringLiteral("Preprocess Image"),
                              QStringLiteral("The selected guide layer is no longer available."));
        return;
    }
    guide = static_cast<fls::scene::GuideLayer *>(node);
    if (guide->locked) {
        canvasStatusBar_->showMessage(QStringLiteral("The selected guide layer is locked"), 3500);
        return;
    }

    state_->beginProjectEdit();
    if (!guide->image) {
        guide->image = std::make_unique<fls::scene::RasterContainer>();
    }
    guide->image->width = processed.width();
    guide->image->height = processed.height();
    guide->image->format = embedFormat;
    guide->image->encoded = encoded;
    guide->image->pixels = QByteArray(reinterpret_cast<const char *>(processed.constBits()),
                                      processed.sizeInBytes());
    guide->imageTopDown = true;
    guide->preprocessColorCount = std::clamp(dialog.retainedColorCount(), 0, 256);
    const int retainedColors = guide->preprocessColorCount;
    int addedSwatches = 0;
    for (const QColor &color : dialog.retainedPalette()) {
        const bool exists = std::any_of(
            state_->project_.colorSwatches.cbegin(), state_->project_.colorSwatches.cend(),
            [&](const std::array<quint8, 4> &swatch) {
                return swatch[ColorByteRed] == color.red()
                    && swatch[ColorByteGreen] == color.green()
                    && swatch[ColorByteBlue] == color.blue();
            });
        if (!exists) {
            state_->project_.colorSwatches.push_back({static_cast<quint8>(color.blue()),
                                                      static_cast<quint8>(color.green()),
                                                      static_cast<quint8>(color.red()),
                                                      255});
            ++addedSwatches;
        }
    }
    state_->commitProjectEdit();
    state_->noteProjectGeometryChanged(true, {guideId});
    canvasStatusBar_->showMessage(QStringLiteral("Preprocessed guide layer %1 (%2 retained colors, %3 new swatches)")
                                 .arg(guideName).arg(retainedColors).arg(addedSwatches),
                             3500);
}

void MainWindow::createRegions() {
    if (canvas_ == nullptr) {
        return;
    }
    cancelRegionFill();
    QString message;
    if (canvas_->createRegionsForSelectedGuide(regionMergeAreaThreshold_, &message)) {
        canvasStatusBar_->showMessage(message, 5000);
    } else {
        canvasStatusBar_->showMessage(message.isEmpty()
                                     ? QStringLiteral("Could not create regions")
                                     : message,
                                 5000);
    }
}

void MainWindow::fillRegions() {
    if (canvas_ == nullptr || !state_->hasProject()) {
        canvasStatusBar_->showMessage(QStringLiteral("Open or create a project first"), 4000);
        return;
    }
    cancelGeneratedFill();
    cancelRegionFill();
    RegionFillBatchRequest request;
    QString message;
    if (!canvas_->prepareRegionFillBatch(&request, &message)) {
        canvasStatusBar_->showMessage(message.isEmpty()
                                     ? QStringLiteral("Could not fill regions")
                                     : message,
                                 5000);
        return;
    }

    const quint64 generation = ++regionFillGeneration_;
    const auto token = std::make_shared<std::atomic_bool>(false);
    regionFillCancel_ = token;
    regionFillInsertionEntries_ = selectedEntryIds();
    regionFillProgress_->setRange(0, 0);
    regionFillProgress_->setFormat(QStringLiteral("Planning regions…"));
    regionFillProgress_->show();
    canvasStatusBar_->showMessage(QStringLiteral("Filling regions… Press %1 to cancel")
                                 .arg(interactionShortcutText(KeyInteraction::CancelActiveFill)));

    QPointer<MainWindow> guard(this);
    auto *task = QRunnable::create(
        [guard, generation, request = std::move(request), token]() mutable {
            RegionFillBatchResult result = computeRegionFills(
                request,
                [guard, generation](const QString &phase, int completed, int total) {
                    queueMainWindowCallback(guard,
                        [generation, phase, completed, total](MainWindow &window) {
                            window.updateRegionFillProgress(generation, phase, completed, total);
                        });
                },
                [token]() { return token->load(std::memory_order_relaxed); });
            queueMainWindowCallback(guard,
                [generation, result = std::move(result)](MainWindow &window) mutable {
                    window.finishRegionFill(generation, std::move(result));
                });
        });
    task->setAutoDelete(true);
    QThreadPool::globalInstance()->start(task);
}

void MainWindow::cancelRegionFill() {
    if (regionFillCancel_ == nullptr) {
        return;
    }
    regionFillCancel_->store(true, std::memory_order_relaxed);
    regionFillCancel_.reset();
    ++regionFillGeneration_;
    regionFillInsertionEntries_.clear();
    if (regionFillProgress_ != nullptr) {
        regionFillProgress_->hide();
    }
    canvasStatusBar_->showMessage(QStringLiteral("Region Fill cancelled"), 1500);
}

void MainWindow::updateRegionFillProgress(quint64 generation,
                                          const QString &phase,
                                          int completed,
                                          int total) {
    if (generation != regionFillGeneration_ || regionFillCancel_ == nullptr
        || regionFillProgress_ == nullptr) {
        return;
    }
    if (total <= 0) {
        regionFillProgress_->setRange(0, 0);
        regionFillProgress_->setFormat(phase);
        return;
    }
    const int maximum = std::max(1, total);
    const QString format = QStringLiteral("%1 %v/%m").arg(phase);
    const bool samePhase = regionFillProgress_->format() == format;
    const int value = std::clamp(completed, 0, maximum);

    regionFillProgress_->setRange(0, maximum);
    regionFillProgress_->setFormat(format);
    regionFillProgress_->setValue(
        samePhase ? std::max(regionFillProgress_->value(), value) : value);
}

void MainWindow::finishRegionFill(quint64 generation, RegionFillBatchResult result) {
    if (generation != regionFillGeneration_ || regionFillCancel_ == nullptr) {
        return;
    }
    regionFillCancel_.reset();
    regionFillProgress_->hide();
    if (result.cancelled) {
        regionFillInsertionEntries_.clear();
        canvasStatusBar_->showMessage(QStringLiteral("Region Fill cancelled"), 1500);
        return;
    }
    const QImage differenceHeatmap = result.differenceHeatmap;
    const QString sourceGuideId = result.overlayGuideId;
    QString message;
    if (canvas_ == nullptr || !state_->hasProject()
        || !canvas_->applyRegionFillBatch(std::move(result), &message)) {
        regionFillInsertionEntries_.clear();
        canvasStatusBar_->showMessage(message.isEmpty()
                                     ? QStringLiteral("Could not fill regions") : message,
                                 5000);
        return;
    }
    const QVector<GeneratedRegionVariant> variants =
        canvas_->regionFillWorldVariants();
    if (variants.isEmpty()) {
        regionFillInsertionEntries_.clear();
        canvasStatusBar_->showMessage(QStringLiteral("Region fill produced no shapes"), 4000);
        return;
    }
    const QVector<QString> insertionEntries = regionFillInsertionEntries_;
    regionFillInsertionEntries_.clear();
    insertGeneratedRegionVariants(
        QStringLiteral("Region Fill"), QStringLiteral("Region Fill"),
        variants, insertionEntries, differenceHeatmap, sourceGuideId);
    canvas_->hideRegionOverlay();
}

void MainWindow::exportDialog() {
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Export"), QStringLiteral("Open a project before exporting."));
        return;
    }
    const std::optional<ExportAssetSelection> selection = showExportAssetDialog(
        this,
        importBrowserStartDirectory(QStringLiteral("exportFolder"), {QStringLiteral("exportNested")}),
        state_->project_.name,
        state_->project_.isLivery,
        state_->project_.carId,
        fls::exportMaskedLayerCount(state_->project_),
        [this](fls::ExportGameVersion game) { return fls::exportIncompatibleLayerCount(state_->project_, game); });
    if (!selection) {
        return;
    }
    const QString game = fls::exportGameCode(selection->gameVersion);
    const CarRegistry &registry = sharedCarRegistry();
    const ExportContainerFormat format = selection->containerFormat;

    if (!supportsExportContainer(selection->gameVersion, format)) {
        QMessageBox::critical(this, QStringLiteral("Export failed"),
            QStringLiteral("The selected container format is unavailable for %1.").arg(game));
        return;
    }
    if (state_->project_.isLivery && selection->gameVersion != fls::ExportGameVersion::Motorsport2
        && !registry.isSupported(state_->project_.carId, game)) {
        const QString car = registry.displayName(state_->project_.carId);
        QMessageBox::critical(this, QStringLiteral("Export failed"), car.isEmpty()
            ? QStringLiteral("Choose a target car before exporting this livery to %1.").arg(game)
            : QStringLiteral("%1 is not listed as supported in %2.").arg(car, game));
        return;
    }
    if (format == ExportContainerFormat::Xbox360PlainFile || format == ExportContainerFormat::Xbox360SignedFile) {
        const bool livery = state_->project_.isLivery;
        const auto kind = livery ? fls::Xbox360FileKind::Livery : fls::Xbox360FileKind::Group;
        const QString extension = fls::xbox360ExportExtension(selection->gameVersion, kind);
        const QString proposed = safeGroupName(state_->project_.name) + extension;
        const QString filter = (livery ? QStringLiteral("Xbox 360 livery (*%1)")
                                       : QStringLiteral("Xbox 360 vinyl group (*%1)")).arg(extension);
        QFileDialog destination(this, QStringLiteral("Export Xbox 360 Asset"), selection->directory, filter);
        destination.setAcceptMode(QFileDialog::AcceptSave);
        destination.setDefaultSuffix(extension.mid(1));
        destination.selectFile(proposed);
        if (destination.exec() != QDialog::Accepted) {
            return;
        }
        const QString path = destination.selectedFiles().front();
        try {
            std::optional<fls::Xbox360Credentials> credentials;
            if (format == ExportContainerFormat::Xbox360SignedFile) {
                credentials = chooseXbox360KeyFile(this, selection->gameVersion);
                if (!credentials) {
                    return;
                }
            }
            fls::exportXbox360Asset(state_->project_, path, selection->gameVersion,
                                    credentials ? &*credentials : nullptr);
            rememberImportDirectory(path, QStringLiteral("exportFolder"));
            canvasStatusBar_->showMessage(QStringLiteral("Exported %1").arg(path), 5000);
        } catch (const std::exception &ex) {
            QMessageBox::critical(this, QStringLiteral("Export failed"), QString::fromUtf8(ex.what()));
        }
        return;
    }
    if (format == ExportContainerFormat::WgsContainer) {
        if (!selection->wgsTarget) {
            return;
        }
        QMessageBox confirmation(this);
        confirmation.setIcon(QMessageBox::Warning);
        confirmation.setWindowTitle(QStringLiteral("Replace WGS Container"));
        confirmation.setTextFormat(Qt::PlainText);
        confirmation.setText(QStringLiteral("Replace %1 with this project?\n\n%2\n\n"
            "FH5 must be closed and cloud synchronization finished. "
            "The current container and containers.index will be backed up before replacement.")
            .arg(selection->wgsTarget->containerName,
                 QDir::toNativeSeparators(selection->wgsTarget->descriptorPath)));
        confirmation.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
    }
    const QString folder = selection->directory;
    if (format == ExportContainerFormat::PgsFolder) {
        rememberImportDirectory(folder, QStringLiteral("exportFolder"));
        rememberImportDirectory(folder, QStringLiteral("exportNested"));
    }

    QString error;
    if (!exportFolderImpl(folder, selection->gameVersion, &error,
                          format == ExportContainerFormat::WgsContainer ? &*selection->wgsTarget : nullptr)) {
        QMessageBox::critical(this, QStringLiteral("Export failed"), error);
    }
}

void MainWindow::newProjectDialog() {
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
    if (livery && !chooseCarModel(this, 0, &carId)) {
        return;
    }

    QString error;
    if (!newProject(livery, &error, carId)) {
        QMessageBox::critical(this, QStringLiteral("New project failed"), error);
    }
}

void MainWindow::saveProjectJsonDialog() {
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Save Project"), QStringLiteral("Create or open a project before saving."));
        return;
    }

    if (!projectJsonPath_.isEmpty()) {
        QString error;
        if (!saveProjectJson(projectJsonPath_, &error)) {
            QMessageBox::critical(this, QStringLiteral("Save failed"), error);
        }
        return;
    }

    saveProjectJsonAsDialog();
}

void MainWindow::saveProjectJsonAsDialog() {
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Save Project As"),
                                 QStringLiteral("Create or open a project before saving."));
        return;
    }

    const QString suggested = state_->project_.name.isEmpty() ? QStringLiteral("project") : state_->project_.name;
    const QString suggestedPath = projectJsonPath_.isEmpty()
        ? QDir(importDialogStartDirectory(this, QStringLiteral("projectJson")))
              .filePath(suggested + QStringLiteral(".3so"))
        : projectJsonPath_;
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("Save Project As"),
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

void MainWindow::autosaveProject() {
    if (state_ == nullptr || !state_->hasProject() || !state_->isModified() || projectJsonPath_.isEmpty()) {
        return;
    }

    QString error;
    if (!saveProjectJson(projectJsonPath_, &error)) {
        canvasStatusBar_->showMessage(
            QStringLiteral("Autosave failed: %1").arg(error), 5000);
    } else {
        canvasStatusBar_->showMessage(
            QStringLiteral("Autosaved %1").arg(projectJsonPath_), 3000);
    }
}

void MainWindow::loadProjectJsonDialog() {
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Open Project"),
                                                      importDialogStartDirectory(this, QStringLiteral("projectJson")),
                                                      QStringLiteral("Forza Project (*.3so);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    rememberImportDirectory(path, QStringLiteral("projectJson"));

    QString error;
    if (!loadProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Open failed"), error);
    }
}

void MainWindow::openRecentProjectJson(const QString &path) {
    if (!confirmDiscardUnsavedChanges()) {
        return;
    }
    QString error;
    if (!loadProjectJson(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("Open failed"), error);
        refreshRecentProjectJsonMenu();
    }
}

void MainWindow::rememberRecentProjectJson(const QString &path) {
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

void MainWindow::refreshRecentProjectJsonMenu() {
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

void MainWindow::refreshHeaderMetadataWidget() {
    if (headerMetadata_ == nullptr) {
        return;
    }

    if (!state_->hasProject_) {
        headerMetadata_->setMetadata({}, {}, false, false);
        return;
    }

    fls::HeaderMetadata meta = effectiveHeaderMetadata(state_->project_, creatorName_);
    if (meta.name.isEmpty()) {
        meta.name = state_->project_.name;
    }

    headerMetadata_->setMetadata(meta, targetCarLabel(), true, state_->project_.isLivery);
}

QString MainWindow::targetCarLabel() const {
    if (!state_->hasProject_ || !state_->project_.isLivery) {
        return {};
    }
    QString label = sharedCarRegistry().displayName(state_->project_.carId);
    if (!label.isEmpty() && (carPreview_ == nullptr || !carPreview_->hasModel())) {
        label += QStringLiteral(" (No model loaded)");
    }

    return label;
}

void MainWindow::showHeaderMetadataDock() {
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

void MainWindow::applyHeaderMetadata() {
    if (!state_->hasProject_) {
        QMessageBox::information(this, QStringLiteral("Header Metadata"), QStringLiteral("Create or open a project first."));
        return;
    }

    fls::HeaderMetadata meta = headerMetadata_->metadata();
    meta.carId = static_cast<quint32>(state_->project_.carId);
    const fls::HeaderMetadata current =
        effectiveHeaderMetadata(state_->project_, creatorName_);
    if (meta.name == current.name
        && meta.creatorName == current.creatorName
        && meta.year == current.year
        && meta.month == current.month) {
        return;
    }

    state_->project_.name = meta.name;
    if (meta.creatorName != current.creatorName) {
        const QString sourceCreator = fls::detail::motorsport2CreatorName(state_->project_.liverySource);
        if (sourceCreator.isEmpty() || meta.creatorName != sourceCreator) {
            creatorName_ = meta.creatorName;
            QSettings().setValue(QStringLiteral("header/creatorName"), creatorName_);
        }
    }

    state_->project_.sourceHeader.clear();
    state_->project_.headerMetadata = meta;
    state_->setModified(true);
    updateStatus();
    refreshHeaderMetadataWidget();
    canvasStatusBar_->showMessage(QStringLiteral("Header metadata updated"), 5000);
}

void MainWindow::scheduleLayoutSave() {
    if (layoutSaveTimer_ == nullptr || !isVisible()) {
        return;
    }
    layoutSaveTimer_->start();
}

void MainWindow::saveLayout() {
    if (layoutSaveTimer_ != nullptr) {
        layoutSaveTimer_->stop();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("layout/geometry"), saveGeometry());
    if (std::none_of(dockAreaCollapseStates_.cbegin(), dockAreaCollapseStates_.cend(),
            [](const DockAreaCollapseState &state) { return state.collapsed; })) {
        settings.setValue(QStringLiteral("layout/state"), saveState());
    }
    settings.sync();
}

bool MainWindow::restoreLayout() {
    QSettings settings;
    const QByteArray state = settings.value(QStringLiteral("layout/state")).toByteArray();
    if (state.isEmpty()) {
        return false;
    }
    const QByteArray geometry = settings.value(QStringLiteral("layout/geometry")).toByteArray();
    const bool restored = restoreState(state);
    positionToolSettings();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
#ifdef Q_OS_WIN
        if (isMaximized() && isFullScreen()) {
            setWindowState(windowState() & ~Qt::WindowFullScreen);
        }
#endif
    }

    return restored;
}

void MainWindow::resetLayout() {
    const QSize availableSize = screen()->availableGeometry().size();

    for (DockAreaCollapseState &state : dockAreaCollapseStates_) {
        state.expandedExtent = 0;
    }
    for (const DockAreaCollapseState &state : dockAreaCollapseStates_) {
        if (state.collapsed) {
            toggleDockAreaCollapsed(state.area);
        }
    }
    dockAreaCollapseStates_.clear();

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    for (QDockWidget *dock : dockWidgets_) {
        dock->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    }
#endif
    if (!defaultLayoutState_.isEmpty()) {
        restoreState(defaultLayoutState_);
        positionToolSettings();
        layout()->activate();
        if (width() > availableSize.width() || height() > availableSize.height()) {
            showMaximized();
        }
        applyDefaultLayoutSizes();
        syncDockCollapseButtons();
    }
    QSettings settings;
    settings.remove(QStringLiteral("layout/state"));
    settings.remove(QStringLiteral("layout/geometry"));
    settings.remove(QStringLiteral("ui/splitters/shapesSections"));
    scheduleLayoutRepaint();
    scheduleLayoutSave();
    canvasStatusBar_->showMessage(QStringLiteral("Layout reset to default"), 5000);
}

void MainWindow::applyDefaultLayoutSizes() {
    constexpr double kLeftWidthFraction = 0.125;
    constexpr double kRightWidthFraction = 0.20;
    constexpr double kBottomHeightFraction = 0.28;
    constexpr double kPropertiesHeightFraction = 0.73;
    constexpr double kShapesWidthFraction = 0.80;
    auto *propertiesDock = findChild<QDockWidget *>(QStringLiteral("PropertiesDock"));
    auto *swatchesDock = findChild<QDockWidget *>(QStringLiteral("SwatchesDock"));
    auto *layersDock = findChild<QDockWidget *>(QStringLiteral("LayersDock"));
    auto *shapesDock = findChild<QDockWidget *>(QStringLiteral("ShapesDock"));
    const int dockAreaHeight = height() - centralWidget()->y();
    const int bottomHeight = qRound(dockAreaHeight * kBottomHeightFraction);
    const int sideHeight = dockAreaHeight - bottomHeight;
    const int propertiesHeight = qRound(sideHeight * kPropertiesHeightFraction);
    const int shapesWidth = qRound(width() * kShapesWidthFraction);

    resizeDocks({propertiesDock, layersDock},
                {qRound(width() * kLeftWidthFraction), qRound(width() * kRightWidthFraction)},
                Qt::Horizontal);
    resizeDocks({shapesDock}, {bottomHeight}, Qt::Vertical);
    resizeDocks({propertiesDock, swatchesDock},
                {propertiesHeight, sideHeight - propertiesHeight}, Qt::Vertical);
    resizeDocks({shapesDock, headerMetadataDock_},
                {shapesWidth, width() - shapesWidth}, Qt::Horizontal);
    layout()->activate();
    shapesBrowser_->resetLayout();
    positionToolSettings();
}

} // namespace gui
