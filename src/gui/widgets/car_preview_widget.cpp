#include "car_preview_widget.h"

#include "car_scene.h"
#include "editor_state.h"
#include "matrix_math.h"
#include "scene_view.h"
#include "zip_extract.h"

#include <QLabel>

#include <algorithm>
#include <cmath>
#include <optional>

namespace gui {
namespace {

// The vinyl paint canvas is rasterized at the livery canvas resolution: canvasX
// in [-1024, 1024], canvasY in [-512, 512] (see fh6::kLiveryCanvas*). A 2048x1024
// texture matches that 2:1 space 1:1; the runtime scale is user-configurable so
// projection sampling can trade memory for detail once clipped by UV masks.
constexpr int kLiveryBaseTexWidth = 2048;
constexpr int kLiveryBaseTexHeight = 1024;
constexpr int kProjectedSectionToMaskSlot[fh6::kLiverySideCount] = {
    0, 1, 2, 4, 3, 5, 6, 7, 8, 10, 9,
};

bool sectionOverlayFlipX(int displayedSectionSlot)
{
    switch (displayedSectionSlot) {
    case 1:  // Back
    case 4:  // Right
    case 10: // RightWindow
        return true;
    default:
        return false;
    }
}

bool sectionOverlayFlipY(int displayedSectionSlot)
{
    switch (displayedSectionSlot) {
    case 3: // Left
    case 9: // LeftWindow
        return true;
    default:
        return false;
    }
}

struct ProjectedLiverySection {
    fh6::Project project;
    QRect clipRect;
};

QRect liverySideClipRect(const fh6::LiverySide &side, const QSize &texSize)
{
    const double left = std::min(side.left, side.right);
    const double right = std::max(side.left, side.right);
    const double top = std::min(side.top, side.bottom);
    const double bottom = std::max(side.top, side.bottom);
    const double sx = static_cast<double>(texSize.width()) / (2.0 * fh6::kLiveryCanvasHalfWidth);
    const double sy = static_cast<double>(texSize.height()) / (2.0 * fh6::kLiveryCanvasHalfHeight);
    const int x0 = std::clamp(static_cast<int>(std::floor((left + fh6::kLiveryCanvasHalfWidth) * sx)), 0, texSize.width());
    const int x1 = std::clamp(static_cast<int>(std::ceil((right + fh6::kLiveryCanvasHalfWidth) * sx)), 0, texSize.width());
    const int y0 = std::clamp(static_cast<int>(std::floor((fh6::kLiveryCanvasHalfHeight + top) * sy)), 0, texSize.height());
    const int y1 = std::clamp(static_cast<int>(std::ceil((fh6::kLiveryCanvasHalfHeight + bottom) * sy)), 0, texSize.height());
    return QRect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

fh6::Matrix3 fromQTransform(const QTransform &t)
{
    fh6::Matrix3 m;
    m.m[0][0] = t.m11();
    m.m[1][0] = t.m12();
    m.m[0][1] = t.m21();
    m.m[1][1] = t.m22();
    m.m[0][2] = t.dx();
    m.m[1][2] = t.dy();
    return m;
}

void collectProjectedShapes(const fh6::scene::Layer &node,
                            const QTransform &parentWorld,
                            double xOrigin,
                            double yOrigin,
                            fh6::scene::Group &outRoot)
{
    const QTransform world = sceneLocalTransform(node) * parentWorld;
    if (node.kind() == fh6::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
            collectProjectedShapes(*child, world, xOrigin, yOrigin, outRoot);
        }
        return;
    }
    if (node.kind() != fh6::scene::LayerKind::Shape) {
        return;
    }
    auto copy = node.clone();
    auto *shape = static_cast<fh6::scene::Shape *>(copy.get());
    shape->transform = fh6::decomposeTransform2D(fromQTransform(world));
    shape->transform.x += xOrigin;
    shape->transform.y += yOrigin;
    shape->visible = true;
    outRoot.append(std::move(copy));
}

QVector<ProjectedLiverySection> buildProjectedLiverySections(const fh6::Project &project,
                                                             const fh6::LiveryMaskSet &masks,
                                                             const QSize &texSize)
{
    QVector<ProjectedLiverySection> projectedSections;
    if (!project.root) {
        return projectedSections;
    }

    for (const auto &rootChild : project.root->children) {
        if (rootChild->kind() != fh6::scene::LayerKind::Group) {
            continue;
        }
        const auto *section = static_cast<const fh6::scene::Group *>(rootChild.get());
        if (!section->isLiverySection) {
            continue;
        }
        const int slot = section->liverySectionSlot;
        if (slot < 0 || slot >= fh6::kLiverySideCount) {
            continue;
        }
        const int maskSlot = kProjectedSectionToMaskSlot[slot];
        if (maskSlot < 0 || maskSlot >= fh6::kLiverySideCount) {
            continue;
        }
        const fh6::LiverySide &side = masks.sides[maskSlot];
        if (!side.valid) {
            continue;
        }

        ProjectedLiverySection projected;
        projected.project.name = QStringLiteral("%1/%2").arg(project.name, section->name);
        projected.clipRect = liverySideClipRect(side, texSize);
        // Imported C_livery sections are authored around their own panel origin.
        // For the car texture, place each panel origin on the matching Masks.xml
        // paint-canvas origin while keeping decoded section/group geometry intact.
        collectProjectedShapes(*section, QTransform(), side.xOrigin, side.yOrigin, *projected.project.root);
        if (projected.project.root->children.empty()) {
            continue;
        }
        projectedSections.push_back(std::move(projected));
    }

    return projectedSections;
}

std::optional<ProjectedLiverySection> buildProjectedLiverySection(const fh6::Project &project,
                                                                  const fh6::scene::Group &section,
                                                                  const fh6::LiveryMaskSet &masks,
                                                                  const QSize &texSize)
{
    if (!section.isLiverySection) {
        return std::nullopt;
    }
    const int slot = section.liverySectionSlot;
    if (slot < 0 || slot >= fh6::kLiverySideCount) {
        return std::nullopt;
    }
    const int maskSlot = kProjectedSectionToMaskSlot[slot];
    if (maskSlot < 0 || maskSlot >= fh6::kLiverySideCount) {
        return std::nullopt;
    }
    const fh6::LiverySide &side = masks.sides[maskSlot];
    if (!side.valid) {
        return std::nullopt;
    }

    ProjectedLiverySection projected;
    projected.project.name = QStringLiteral("%1/%2").arg(project.name, section.name);
    projected.clipRect = liverySideClipRect(side, texSize);
    collectProjectedShapes(section, QTransform(), side.xOrigin, side.yOrigin, *projected.project.root);
    if (projected.project.root->children.empty()) {
        return std::nullopt;
    }
    return projected;
}

QString findCarbin(const QString &root)
{
    QDirIterator it(root, QStringList{QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        return it.next();
    }
    return {};
}

} // namespace

CarPreviewWidget::CarPreviewWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    setFormat(format);
    setFocusPolicy(Qt::StrongFocus);

    // Non-interactive disclaimer pinned to the top-left corner: the runtime planar
    // projection is an approximation of the game's own paint pipeline.
    referenceNote_ = new QLabel(QStringLiteral("Only for reference, ingame render may differ"), this);
    referenceNote_->setAttribute(Qt::WA_TransparentForMouseEvents);
    referenceNote_->setStyleSheet(QStringLiteral(
        "QLabel {"
        " color: rgba(235, 236, 240, 220);"
        " background: rgba(0, 0, 0, 90);"
        " border-radius: 3px;"
        " padding: 2px 6px;"
        " font-size: 10px;"
        "}"));
    referenceNote_->move(8, 6);
    referenceNote_->adjustSize();
    referenceNote_->raise();
}

CarPreviewWidget::~CarPreviewWidget()
{
    makeCurrent();
    carRenderer_.release();
    shapeRenderer_.release();
    doneCurrent();
}

bool CarPreviewWidget::loadCar(const QString &path, QString *error)
{
    QString loadPath = path;
    std::unique_ptr<QTemporaryDir> extracted;
    if (path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        extracted = std::make_unique<QTemporaryDir>();
        if (!extracted->isValid()) {
            if (error != nullptr) {
                *error = QStringLiteral("cannot create temporary directory for %1").arg(path);
            }
            return false;
        }
        if (!fh6::extractZipArchive(path, extracted->path(), error)) {
            return false;
        }
        loadPath = findCarbin(extracted->path());
        if (loadPath.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: no .carbin found in %1").arg(QFileInfo(path).fileName());
            }
            return false;
        }
    }

    fh6::CarModel model = loadPath.endsWith(QStringLiteral(".carbin"), Qt::CaseInsensitive)
        ? fh6::loadCarBin(loadPath, error)
        : fh6::loadModelBin(loadPath, error);
    if (model.meshes.empty()) {
        return false;
    }
    model_ = std::move(model);
    extractedCarDir_ = std::move(extracted);
    modelUploadPending_ = true;

    // A car ships its per-side livery masks in a sibling LiveryMasks/ folder.
    // Decode them on the CPU now; they are uploaded to GL on the next paint.
    liveryMasks_ = {};
    const QDir carDir = QFileInfo(loadPath).absoluteDir();
    const QString masksDir = carDir.filePath(QStringLiteral("LiveryMasks"));
    if (QFileInfo::exists(masksDir)) {
        liveryMasks_ = fh6::loadLiveryMasks(masksDir);
        liveryMasksDir_ = masksDir;
    } else {
        liveryMasksDir_.clear();
    }
    liveryMasksPending_ = true;
    liveryDirty_ = true;
    dirtySectionIds_.clear();
    projectedSectionCache_.clear();

    update();
    return true;
}

bool CarPreviewWidget::hasModel() const
{
    return !model_.meshes.empty();
}

void CarPreviewWidget::clearModel()
{
    if (!hasModel() && !carRenderer_.hasModel()) {
        return;
    }
    model_ = fh6::CarModel{};
    extractedCarDir_.reset();
    liveryMasks_ = {};
    liveryMasksDir_.clear();
    modelUploadPending_ = false;
    liveryMasksPending_ = false;
    liveryDirty_ = true;
    dirtySectionIds_.clear();
    projectedSectionCache_.clear();
    if (carRenderer_.isInitialized()) {
        makeCurrent();
        carRenderer_.clearModel();
        doneCurrent();
    }
    update();
}

QImage CarPreviewWidget::unwrapOverlay(int liverySectionSlot) const
{
    if (!liveryMasks_.valid()) {
        return {};
    }
    // Per-side tint (matches the 3D debug side colours).
    static const QColor kSideColors[fh6::kLiverySideCount] = {
        QColor(230, 60, 60),   // Front
        QColor(60, 200, 60),   // Back
        QColor(70, 120, 240),  // Top
        QColor(230, 220, 60),  // Left
        QColor(220, 80, 220),  // Right
        QColor(60, 210, 210),  // Spoiler
        QColor(255, 130, 60),  // FrontWindshield
        QColor(120, 255, 140), // BackWindshield
        QColor(120, 190, 255), // TopWindow
        QColor(255, 235, 120), // LeftWindow
        QColor(255, 140, 255), // RightWindow
    };

    // Find the mask dimensions (every swatchbin is the same size).
    int w = 0, h = 0;
    for (const fh6::LiverySide &side : liveryMasks_.sides) {
        if (side.mask.valid()) {
            w = side.mask.width;
            h = side.mask.height;
            break;
        }
    }
    if (w == 0 || h == 0) {
        return {};
    }

    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    int firstSide = 0;
    int lastSide = fh6::kLiverySideCount;
    if (liverySectionSlot >= 0) {
        if (liverySectionSlot >= fh6::kLiverySideCount) {
            return {};
        }
        firstSide = kProjectedSectionToMaskSlot[liverySectionSlot];
        if (firstSide < 0 || firstSide >= fh6::kLiverySideCount) {
            return {};
        }
        lastSide = firstSide + 1;
    }
    const bool flipSectionX = liverySectionSlot >= 0 && sectionOverlayFlipX(liverySectionSlot);
    const bool flipSectionY = liverySectionSlot >= 0 && sectionOverlayFlipY(liverySectionSlot);

    bool drew = false;
    for (int s = firstSide; s < lastSide; ++s) {
        const fh6::LiverySide &side = liveryMasks_.sides[s];
        const fh6::SwatchMask &mask = side.mask;
        if (!mask.valid() || mask.width != w || mask.height != h) {
            continue;
        }

        const double left = std::min(side.left, side.right);
        const double right = std::max(side.left, side.right);
        const double top = std::min(side.top, side.bottom);
        const double bottom = std::max(side.top, side.bottom);
        const double sx = static_cast<double>(w) / (2.0 * fh6::kLiveryCanvasHalfWidth);
        const double sy = static_cast<double>(h) / (2.0 * fh6::kLiveryCanvasHalfHeight);
        const int x0 = std::clamp(static_cast<int>(std::floor((left + fh6::kLiveryCanvasHalfWidth) * sx)), 0, w);
        const int x1 = std::clamp(static_cast<int>(std::ceil((right + fh6::kLiveryCanvasHalfWidth) * sx)), 0, w);
        // `y` here is a swatch texel row, which is Y-flipped vs canvas: texel_y =
        // halfHeight - canvasY (see livery_masks.h). So the covered texel rows for a
        // canvasY span [top, bottom] are [(half - bottom), (half - top)] * sy.
        const int y0 = std::clamp(static_cast<int>(std::floor((fh6::kLiveryCanvasHalfHeight - bottom) * sy)), 0, h);
        const int y1 = std::clamp(static_cast<int>(std::ceil((fh6::kLiveryCanvasHalfHeight - top) * sy)), 0, h);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }

        const QColor c = kSideColors[s];
        const double originX = liverySectionSlot >= 0 ? side.xOrigin : 0.0;
        const double originY = liverySectionSlot >= 0 ? side.yOrigin : 0.0;
        for (int y = y0; y < y1; ++y) {
            const double canvasY = fh6::kLiveryCanvasHalfHeight - (static_cast<double>(y) + 0.5) / sy;
            int outY = static_cast<int>(std::floor((canvasY - originY + fh6::kLiveryCanvasHalfHeight) * sy));
            if (outY < 0 || outY >= h) {
                continue;
            }
            if (flipSectionY) {
                outY = h - 1 - outY;
            }
            QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(outY));
            const uint8_t *cov = &mask.coverage[static_cast<size_t>(y) * w];
            for (int x = x0; x < x1; ++x) {
                if (cov[x] < 32) {
                    continue;
                }
                const double canvasX = (static_cast<double>(x) + 0.5) / sx - fh6::kLiveryCanvasHalfWidth;
                int outX = static_cast<int>(std::floor((canvasX - originX + fh6::kLiveryCanvasHalfWidth) * sx));
                if (outX < 0 || outX >= w) {
                    continue;
                }
                if (flipSectionX) {
                    outX = w - 1 - outX;
                }
                row[outX] = qRgba(c.red(), c.green(), c.blue(), 255);
                drew = true;
            }
        }
    }
    return drew ? image : QImage();
}

void CarPreviewWidget::setProject(fh6::Project *project)
{
    project_ = project;
    liveryDirty_ = true;
    dirtySectionIds_.clear();
    projectedSectionCache_.clear();
    update();
}

void CarPreviewWidget::setEditorState(EditorState *state)
{
    if (state_ == state) {
        return;
    }
    if (state_ != nullptr) {
        disconnect(state_, nullptr, this, nullptr);
    }
    state_ = state;
    if (state_ != nullptr) {
        connect(state_, &EditorState::projectGeometryChanged, this, &CarPreviewWidget::onProjectGeometryChanged);
        connect(state_, &EditorState::transformLiveChanged, this, &CarPreviewWidget::markLiverySectionsDirty);
        connect(state_, &EditorState::canvasRepaintRequested, this, &CarPreviewWidget::markLiveryDirtyImmediate);
        connect(state_, &EditorState::projectStructureChanged, this, &CarPreviewWidget::markLiveryDirty);
    }
}

QColor CarPreviewWidget::basePaint() const
{
    return basePaint_;
}

void CarPreviewWidget::setBasePaint(const QColor &color)
{
    if (color.isValid() && color != basePaint_) {
        basePaint_ = color;
        update();
    }
}

int CarPreviewWidget::liveryTextureScale() const
{
    return liveryTextureScale_;
}

void CarPreviewWidget::setLiveryTextureScale(int scale)
{
    scale = std::clamp(scale, 1, 8);
    if (liveryTextureScale_ == scale) {
        return;
    }
    liveryTextureScale_ = scale;
    projectedSectionCache_.clear();
    liveryTexture_ = 0;
    liveryDirty_ = true;
    liveLiveryFullDirty_ = true;
    update();
}

void CarPreviewWidget::markLiveryDirty()
{
    liveLiveryFullDirty_ = true;
    dirtySectionIds_.clear();
    projectedSectionCache_.clear();
    liveryDirty_ = true;
    update();
}

void CarPreviewWidget::markLiveryDirtyImmediate()
{
    // Full rebuild on the next paint (Qt coalesces repeated update() calls into one paintGL,
    // so the livery texture is rebuilt at most once per painted frame). No car -> nothing to
    // show, so skip the work.
    if (!hasModel()) {
        return;
    }
    markLiveryDirty();
}

void CarPreviewWidget::onProjectGeometryChanged(bool, const QVector<QString> &changedNodeIds)
{
    // Known edit targets -> scope the re-raster to their section(s) (markLiverySectionsDirty
    // itself falls back to a full rebuild if none resolve to a livery section). No target
    // ids -> the edit's scope is unknown, so rebuild everything.
    if (changedNodeIds.isEmpty()) {
        markLiveryDirty();
        return;
    }
    markLiverySectionsDirty(changedNodeIds);
}

void CarPreviewWidget::markLiverySectionsDirty(const QVector<QString> &nodeIds)
{
    if (!hasModel() || state_ == nullptr || project_ == nullptr || !project_->isLivery || !liveryMasks_.valid()) {
        markLiveryDirtyImmediate();
        return;
    }
    const QSet<QString> sections = state_->sectionIdsForNodes(nodeIds);
    if (sections.isEmpty()) {
        markLiveryDirtyImmediate();
        return;
    }
    dirtySectionIds_.unite(sections);
    liveLiveryFullDirty_ = false;
    liveryDirty_ = true;
    update();
}

void CarPreviewWidget::initializeGL()
{
    geometryLoaded_ = geometry_.loadDefault();
    shapeRenderer_.initialize();
    if (geometryLoaded_ && shapeRenderer_.isInitialized()) {
        shapeRenderer_.uploadGeometry(geometry_);
    }
    carRenderer_.initialize();
    // A fresh GL context means the persistent livery framebuffer is empty; force the
    // next paint to do a full rebuild before any incremental (section-scoped) redraw.
    liveryTexture_ = 0;
    liveLiveryFullDirty_ = true;
    liveryDirty_ = true;
}

void CarPreviewWidget::resizeGL(int, int)
{
}

void CarPreviewWidget::paintGL()
{
    QOpenGLContext *ctx = context();
    if (ctx == nullptr) {
        return;
    }
    QOpenGLFunctions *functions = ctx->functions();

    if (modelUploadPending_ && carRenderer_.isInitialized()) {
        carRenderer_.uploadModel(model_);
        modelUploadPending_ = false;
        fitCameraToModel();
    }
    if (liveryMasksPending_ && carRenderer_.isInitialized()) {
        carRenderer_.setLivery(model_, liveryMasks_);
        liveryMasksPending_ = false;
    }

    // Rasterize the vinyl scene into the shape renderer's own FBO. This leaves
    // framebuffer 0 bound and the viewport set to the livery texture size.
    GLuint liveryTexture = 0;
    if (project_ != nullptr && geometryLoaded_ && shapeRenderer_.isInitialized()) {
        if (liveryDirty_ || liveryTexture_ == 0) {
            const QSize texSize = liveryTextureSize();
            const bool projectImportedLivery = project_->isLivery && liveryMasks_.valid();
            // Full rebuild wipes and redraws every section; otherwise only the sections in
            // dirtySectionIds_ are re-projected and re-rasterized over the persistent texture.
            const bool fullRebuild = liveLiveryFullDirty_ || liveryTexture_ == 0
                || projectedSectionCache_.isEmpty() || dirtySectionIds_.isEmpty();
            QVector<ProjectedLiverySection> projectedSections;
            QVector<int> dirtyIndices;  // indices into projectedSections re-rasterized this frame
            if (projectImportedLivery && project_->root) {
                QSet<QString> liveSectionIds;
                for (const auto &rootChild : project_->root->children) {
                    if (rootChild->kind() != fh6::scene::LayerKind::Group) {
                        continue;
                    }
                    const auto *section = static_cast<const fh6::scene::Group *>(rootChild.get());
                    if (!section->isLiverySection) {
                        continue;
                    }
                    liveSectionIds.insert(section->id);
                    const bool sectionDirty = fullRebuild || dirtySectionIds_.contains(section->id)
                        || !projectedSectionCache_.contains(section->id);
                    if (sectionDirty) {
                        if (const std::optional<ProjectedLiverySection> projected = buildProjectedLiverySection(*project_, *section, liveryMasks_, texSize)) {
                            projectedSectionCache_.insert(section->id, {projected->project, projected->clipRect});
                        } else {
                            projectedSectionCache_.remove(section->id);
                        }
                    }
                    const auto cached = projectedSectionCache_.constFind(section->id);
                    if (cached != projectedSectionCache_.constEnd()) {
                        if (!fullRebuild && sectionDirty) {
                            dirtyIndices.push_back(projectedSections.size());
                        }
                        projectedSections.push_back({cached->project, cached->clipRect});
                    }
                }
                for (auto it = projectedSectionCache_.begin(); it != projectedSectionCache_.end();) {
                    if (!liveSectionIds.contains(it.key())) {
                        it = projectedSectionCache_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (!projectedSections.isEmpty()) {
                const auto collect = [](const QVector<ProjectedLiverySection> &sections,
                                        QVector<fh6::Project> &projects, QVector<QRect> &clips) {
                    projects.reserve(sections.size());
                    clips.reserve(sections.size());
                    for (const ProjectedLiverySection &section : sections) {
                        projects.push_back(section.project);
                        clips.push_back(section.clipRect);
                    }
                };
                GLuint tex = 0;
                if (!fullRebuild && liveryTexture_ != 0 && !dirtyIndices.isEmpty()) {
                    // Redraw only the connected cluster of sections whose atlas rects overlap the
                    // edited one(s). A section's clear could erase a neighbour that shares pixels,
                    // so grow the set until it is closed under rect intersection; the rest of the
                    // texture is preserved. (Livery side rects are bounding boxes, so overlap is
                    // common even when the sampled content is distinct - hence a cluster, not just
                    // the dirty sections.)
                    const int n = projectedSections.size();
                    QVector<bool> inCluster(n, false);
                    QVector<int> stack;
                    for (const int i : dirtyIndices) {
                        if (!projectedSections[i].clipRect.isEmpty() && !inCluster[i]) {
                            inCluster[i] = true;
                            stack.push_back(i);
                        }
                    }
                    while (!stack.isEmpty()) {
                        const int i = stack.takeLast();
                        for (int j = 0; j < n; ++j) {
                            if (!inCluster[j] && !projectedSections[j].clipRect.isEmpty()
                                && projectedSections[i].clipRect.intersects(projectedSections[j].clipRect)) {
                                inCluster[j] = true;
                                stack.push_back(j);
                            }
                        }
                    }
                    int nonEmptyCount = 0;
                    QVector<ProjectedLiverySection> cluster;  // original order preserved
                    for (int i = 0; i < n; ++i) {
                        if (projectedSections[i].clipRect.isEmpty()) {
                            continue;
                        }
                        ++nonEmptyCount;
                        if (inCluster[i]) {
                            cluster.push_back(projectedSections[i]);
                        }
                    }
                    // Only worth it if the cluster spares some sections; a whole-atlas cluster
                    // falls through to a single full-texture clear + redraw below.
                    if (!cluster.isEmpty() && cluster.size() < nonEmptyCount) {
                        QVector<fh6::Project> clusterProjects;
                        QVector<QRect> clusterClips;
                        collect(cluster, clusterProjects, clusterClips);
                        tex = shapeRenderer_.renderScenesToTexture(
                            clusterProjects, clusterClips, geometry_, liveryWorldToScreen(),
                            texSize, /*preserveExisting=*/true);
                    }
                }
                if (tex == 0) {
                    // Full rebuild (also the fallback when the cluster spans the whole atlas).
                    QVector<fh6::Project> sectionProjects;
                    QVector<QRect> clipRects;
                    collect(projectedSections, sectionProjects, clipRects);
                    tex = shapeRenderer_.renderScenesToTexture(
                        sectionProjects, clipRects, geometry_, liveryWorldToScreen(), texSize);
                }
                liveryTexture_ = tex;
            } else {
                liveryTexture_ = shapeRenderer_.renderSceneToTexture(
                    *project_, geometry_, liveryWorldToScreen(), texSize);
            }
            liveryDirty_ = false;
            liveLiveryFullDirty_ = false;
            dirtySectionIds_.clear();
        }
        liveryTexture = liveryTexture_;
    }

    // Restore the widget's framebuffer + viewport before drawing the car.
    const qreal dpr = devicePixelRatioF();
    const int pw = std::max(1, static_cast<int>(std::lround(width() * dpr)));
    const int ph = std::max(1, static_cast<int>(std::lround(height() * dpr)));
    functions->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    functions->glViewport(0, 0, pw, ph);
    functions->glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
    functions->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!carRenderer_.hasModel()) {
        return;
    }
    carRenderer_.render(cameraView(), cameraProjection(), liveryTexture, basePaint_);
}

QMatrix4x4 CarPreviewWidget::cameraView() const
{
    const float cp = std::cos(pitch_);
    const float sp = std::sin(pitch_);
    const float cy = std::cos(yaw_);
    const float sy = std::sin(yaw_);
    const QVector3D eye = target_ + distance_ * QVector3D(cp * sy, sp, cp * cy);
    QMatrix4x4 view;
    view.lookAt(eye, target_, QVector3D(0.0f, 1.0f, 0.0f));
    return view;
}

QMatrix4x4 CarPreviewWidget::cameraProjection() const
{
    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const float nearPlane = std::max(0.01f, modelRadius_ * 0.02f);
    const float farPlane = modelRadius_ * 8.0f + distance_ * 2.0f;
    QMatrix4x4 projection;
    projection.perspective(45.0f, aspect, nearPlane, farPlane);
    return projection;
}

QSize CarPreviewWidget::liveryTextureSize() const
{
    return QSize(kLiveryBaseTexWidth * liveryTextureScale_, kLiveryBaseTexHeight * liveryTextureScale_);
}

QTransform CarPreviewWidget::liveryWorldToScreen() const
{
    // Map livery canvas coords (canvasX in [-1024,1024], canvasY in [-512,512]) to
    // texture pixels so a texel's UV equals the shader's canvasToUv():
    //   u = (cx + 1024)/2048,  v = (512 - cy)/1024.
    // NativeShapeRenderer's screen y is top-left, then OpenGL stores the FBO with
    // bottom-left texture coordinates. py = 512 + cy therefore lands on the row
    // sampled by v, without a vertical flip.
    const QSize texSize = liveryTextureSize();
    const double sx = static_cast<double>(texSize.width()) / (2.0 * fh6::kLiveryCanvasHalfWidth);
    const double sy = static_cast<double>(texSize.height()) / (2.0 * fh6::kLiveryCanvasHalfHeight);
    QTransform transform;
    transform.translate(texSize.width() * 0.5, texSize.height() * 0.5);
    transform.scale(sx, sy);
    return transform;
}

void CarPreviewWidget::fitCameraToModel()
{
    const fh6::ModelVec3 &mn = model_.boundsMin;
    const fh6::ModelVec3 &mx = model_.boundsMax;
    target_ = QVector3D((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
    const QVector3D extent(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
    modelRadius_ = std::max(0.001f, 0.5f * extent.length());
    distance_ = modelRadius_ * 2.6f;
    yaw_ = 0.6f;
    pitch_ = 0.25f;
}

void CarPreviewWidget::mousePressEvent(QMouseEvent *event)
{
    lastMousePos_ = event->pos();
}

void CarPreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint delta = event->pos() - lastMousePos_;
    lastMousePos_ = event->pos();
    if (event->buttons() & Qt::LeftButton) {
        yaw_ -= delta.x() * 0.01f;
        pitch_ = std::clamp(pitch_ + delta.y() * 0.01f, -1.5f, 1.5f);
        update();
    } else if (event->buttons() & Qt::MiddleButton) {
        const QMatrix4x4 view = cameraView();
        const QVector3D right(view(0, 0), view(0, 1), view(0, 2));
        const QVector3D up(view(1, 0), view(1, 1), view(1, 2));
        const float k = distance_ * 0.0025f;
        target_ += right * (-delta.x() * k) + up * (delta.y() * k);
        update();
    }
}

void CarPreviewWidget::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    distance_ = std::clamp(distance_ * static_cast<float>(std::pow(0.88, steps)),
                           modelRadius_ * 0.1f, modelRadius_ * 40.0f);
    update();
}

void CarPreviewWidget::keyPressEvent(QKeyEvent *event)
{
    // 'P' cycles the livery debug view.
    switch (event->key()) {
    case Qt::Key_P:
        // Cycle debug: 0 normal -> 1 projected UV -> 2 side id -> 0.
        carRenderer_.setDebugMode((carRenderer_.debugMode() + 1) % 3);
        break;
    default:
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    update();
}

} // namespace gui
