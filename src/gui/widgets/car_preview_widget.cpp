#include "car_preview_widget.h"

#include "car_scene.h"
#include "editor_state.h"
#include "gui_assets.h"
#include "material_hashes.h"
#include "matrix_math.h"
#include "model_material.h"
#include "paint_database.h"
#include "paint_color_popup.h"
#include "scene_view.h"
#include "zip_extract.h"

#include <QActionGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace gui {
namespace {

constexpr int kLiveryBaseTexWidth = 2048;
constexpr int kLiveryBaseTexHeight = 1024;
constexpr int kFinishColorCountRole = Qt::UserRole + 1;
constexpr int kFinishSpecialRole = Qt::UserRole + 2;
constexpr int kLiverySectionMaskSlots[fls::kLiverySideCount] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
};

class PaintMaterialComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

    void showPopup() override {
        if (popup_) return;
        auto *menu = new QMenu(this);
        auto *special = new QMenu(QStringLiteral("Special"), menu);
        auto *group = new QActionGroup(menu);
        menu->setObjectName(QStringLiteral("paintMaterialMenu"));
        special->setObjectName(QStringLiteral("paintSpecialMenu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        popup_ = menu;
        for (int index = 0; index < count(); ++index) {
            QMenu *target = itemData(index, kFinishSpecialRole).toBool() ? special : menu;
            QAction *action = target->addAction(itemText(index));
            action->setData(itemData(index));
            action->setCheckable(true);
            action->setChecked(index == currentIndex());
            group->addAction(action);
            if (target == menu && action->isChecked()) menu->setActiveAction(action);
            connect(action, &QAction::triggered, this, [this, index]() {
                setCurrentIndex(index);
                Q_EMIT activated(index);
            });
        }
        if (!special->isEmpty()) menu->addMenu(special);
        connect(menu, &QMenu::aboutToHide, this, [this]() {
            popup_.clear();
            QComboBox::hidePopup();
        });
        menu->setMinimumWidth(width());
        menu->popup(mapToGlobal(QPoint(0, height())));
    }

    void hidePopup() override {
        if (popup_) popup_->close();
        QComboBox::hidePopup();
    }

protected:
    void wheelEvent(QWheelEvent *event) override {
        event->ignore();
    }

private:
    QPointer<QMenu> popup_;
};

bool transposedSection(int maskSlot) {
    return maskSlot == 5 || maskSlot == 6 || maskSlot == 7;
}

QTransform sectionProjectionTransform(int slot) {
    QTransform transform;
    if (slot == 5 || slot == 6) {
        transform.scale(-1.0, 1.0);
    } else if (slot == 4 || slot == 10) {
        transform.scale(-1.0, -1.0);
    } else if (slot == 7) {
        transform.scale(1.0, -1.0);
    }

    return transform;
}

struct ProjectedLiverySection {
    fls::Project project;
    QRect clipRect;
};

struct PackedLiveryLayout {
    std::array<QRect, fls::kLiverySideCount> rects;
    QVector<QVector4D> uvRegions;
    QSize textureSize;
    bool valid = false;
};

PackedLiveryLayout packedLiveryLayout(const fls::LiveryMaskSet &masks, const QSize &baseTextureSize) {
    PackedLiveryLayout layout;
    layout.uvRegions.resize(fls::kLiverySideCount);
    struct Item {
        int slot = 0;
        QSize size;
    };
    QVector<Item> items;
    const double scale = static_cast<double>(baseTextureSize.width()) / kLiveryBaseTexWidth;
    for (int slot = 0; slot < fls::kLiverySideCount; ++slot) {
        const fls::LiverySide &side = masks.sides[slot];
        if (!side.valid) {
            continue;
        }
        QSize size(
            std::max(0, static_cast<int>(std::ceil(std::abs(side.right - side.left) * scale))),
            std::max(0, static_cast<int>(std::ceil(std::abs(side.bottom - side.top) * scale))));
        if (transposedSection(slot)) {
            size.transpose();
        }
        if (!size.isEmpty()) {
            items.push_back({slot, size});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        return a.size.height() != b.size.height()
            ? a.size.height() > b.size.height()
            : a.size.width() > b.size.width();
    });

    const int padding = std::max(1, baseTextureSize.width() / kLiveryBaseTexWidth);
    int x = padding;
    int y = padding;
    int rowHeight = 0;
    for (const Item &item : items) {
        if (x + item.size.width() + padding > baseTextureSize.width()) {
            x = padding;
            y += rowHeight + padding;
            rowHeight = 0;
        }
        if (item.size.width() + 2 * padding > baseTextureSize.width()) {
            return layout;
        }
        layout.rects[item.slot] = QRect(QPoint(x, y), item.size);
        x += item.size.width() + padding;
        rowHeight = std::max(rowHeight, item.size.height());
    }
    layout.textureSize = QSize(
        baseTextureSize.width(), std::max(baseTextureSize.height(), y + rowHeight + padding));

    for (int slot = 0; slot < fls::kLiverySideCount; ++slot) {
        const QRect rect = layout.rects[slot];
        layout.uvRegions[slot] = QVector4D(
            static_cast<float>(rect.left()) / layout.textureSize.width(),
            static_cast<float>(rect.left() + rect.width()) / layout.textureSize.width(),
            1.0f - static_cast<float>(rect.top()) / layout.textureSize.height(),
            1.0f - static_cast<float>(rect.top() + rect.height()) / layout.textureSize.height());
    }
    layout.valid = true;
    return layout;
}

fls::Matrix3 fromQTransform(const QTransform &t) {
    fls::Matrix3 m;
    m.m[0][0] = t.m11();
    m.m[1][0] = t.m12();
    m.m[0][1] = t.m21();
    m.m[1][1] = t.m22();
    m.m[0][2] = t.dx();
    m.m[1][2] = t.dy();
    return m;
}

void collectProjectedShapes(const fls::scene::Layer &node,
                            const QTransform &parentWorld,
                            double xOrigin,
                            double yOrigin,
                            fls::scene::Group &outRoot) {
    const QTransform world = sceneLocalTransform(node) * parentWorld;
    if (node.kind() == fls::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fls::scene::Group &>(node).children) {
            collectProjectedShapes(*child, world, xOrigin, yOrigin, outRoot);
        }
        return;
    }
    if (node.kind() != fls::scene::LayerKind::Shape) {
        return;
    }
    auto copy = node.clone();
    auto *shape = static_cast<fls::scene::Shape *>(copy.get());
    shape->transform = fls::decomposeTransform2D(fromQTransform(world));
    shape->transform.x += xOrigin;
    shape->transform.y += yOrigin;
    shape->visible = true;
    outRoot.append(std::move(copy));
}

std::optional<ProjectedLiverySection> buildProjectedLiverySection(const fls::Project &project,
                                                                  const fls::scene::Group &section,
                                                                  const fls::LiveryMaskSet &masks,
                                                                  const QSize &texSize,
                                                                  const PackedLiveryLayout &layout) {
    if (!section.isLiverySection || !layout.valid) {
        return std::nullopt;
    }
    const int slot = section.liverySectionSlot;
    if (slot < 0 || slot >= fls::kLiverySideCount) {
        return std::nullopt;
    }
    const int maskSlot = kLiverySectionMaskSlots[slot];
    const fls::LiverySide &side = masks.sides[maskSlot];
    if (!side.valid) {
        return std::nullopt;
    }

    ProjectedLiverySection projected;
    projected.project.name = QStringLiteral("%1/%2").arg(project.name, section.name);
    projected.clipRect = layout.rects[maskSlot];
    const double scale = static_cast<double>(texSize.width()) / kLiveryBaseTexWidth;
    const bool transpose = transposedSection(maskSlot);
    const double left = transpose
        ? std::min(side.top, side.bottom)
        : std::min(side.left, side.right);
    const double top = transpose
        ? std::min(side.left, side.right)
        : std::min(side.top, side.bottom);
    const double originX = transpose ? side.yOrigin : side.xOrigin;
    const double originY = transpose ? side.xOrigin : side.yOrigin;
    const double originPixelX = projected.clipRect.x() + (originX - left) * scale;
    const double originPixelY = projected.clipRect.y() + (originY - top) * scale;
    const double packedOriginX = (originPixelX - layout.textureSize.width() * 0.5) / scale;
    const double packedOriginY = (originPixelY - layout.textureSize.height() * 0.5) / scale;
    const QTransform projectionTransform = sectionProjectionTransform(slot);
    collectProjectedShapes(
        section, projectionTransform, packedOriginX, packedOriginY, *projected.project.root);
    if (projected.project.root->children.empty()) {
        return std::nullopt;
    }
    return projected;
}

QString findCarbin(const QString &root) {
    QDirIterator it(root, QStringList{QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        return it.next();
    }
    return {};
}

QString findSharedCarAsset(const QString &sourcePath, const QString &relativePath) {
    QDir dir = QFileInfo(sourcePath).absoluteDir();
    for (int depth = 0; depth < 6; ++depth) {
        const QString candidate = dir.filePath(relativePath);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

fls::WheelSizing wheelSizingForModelCode(const QString &modelCode, const QString &game) {
    static const auto table = [] {
        QHash<QString, fls::WheelSizing> sizes;
        fls::WheelSizing fallback;
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString cwd = QDir::currentPath();
        const QStringList candidates = {
            QDir(appDir).filePath(QStringLiteral("assets/cars/wheel_sizes.json")),
            QDir(cwd).filePath(QStringLiteral("assets/cars/wheel_sizes.json")),
            QDir(cwd).filePath(QStringLiteral("cpp-port/assets/cars/wheel_sizes.json")),
        };
        for (const QString &path : candidates) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            const auto parseAxle = [](const QJsonValue &value, fls::AxleSizing def) {
                const QJsonObject o = value.toObject();
                fls::AxleSizing axle = def;
                axle.tireWidthMillimetres = static_cast<float>(
                    o.value(QStringLiteral("width")).toDouble(def.tireWidthMillimetres));
                axle.tireAspectPercent = static_cast<float>(
                    o.value(QStringLiteral("aspect")).toDouble(def.tireAspectPercent));
                axle.rimDiameterInches = static_cast<float>(
                    o.value(QStringLiteral("rim")).toDouble(def.rimDiameterInches));
                axle.trackOuterMetres = static_cast<float>(
                    o.value(QStringLiteral("track")).toDouble(def.trackOuterMetres));
                axle.rideHeightMetres = static_cast<float>(
                    o.value(QStringLiteral("ride")).toDouble(def.rideHeightMetres));
                return axle;
            };
            const auto parse = [&parseAxle](const QJsonObject &o, fls::WheelSizing def) {
                fls::WheelSizing w;
                w.front = parseAxle(o.value(QStringLiteral("front")), def.front);
                w.rear = parseAxle(o.value(QStringLiteral("rear")), def.rear);
                return w;
            };
            if (root.value(QStringLiteral("_default")).isObject()) {
                fallback = parse(root.value(QStringLiteral("_default")).toObject(), fallback);
            }
            for (auto it = root.begin(); it != root.end(); ++it) {
                if (it.key().startsWith(QLatin1Char('_')) || !it.value().isObject()) {
                    continue;
                }
                const QJsonObject entry = it.value().toObject();
                const fls::WheelSizing sizing = parse(entry, fallback);
                const QJsonObject games = entry.value(QStringLiteral("games")).toObject();
                sizes.insert(it.key().toLower(), sizing);
                for (auto version = games.begin(); version != games.end(); ++version) {
                    sizes.insert(it.key().toLower() + QLatin1Char('|') + version.key().toUpper(),
                                 parse(version.value().toObject(), sizing));
                }
            }
            break;
        }
        return std::make_pair(sizes, fallback);
    }();
    const fls::WheelSizing canonical = table.first.value(modelCode.toLower(), table.second);

    return table.first.value(modelCode.toLower() + QLatin1Char('|') + game.toUpper(), canonical);
}

std::optional<fls::CarModel> loadArchivedModel(
    const QString &archivePath, const QString &modelName) {
    if (archivePath.isEmpty()) {
        return std::nullopt;
    }
    QTemporaryDir extracted;
    QString error;
    if (!extracted.isValid()
        || !fls::extractZipArchive(archivePath, extracted.path(), &error)) {
        return std::nullopt;
    }
    QDirIterator it(
        extracted.path(), QStringList{modelName}, QDir::Files, QDirIterator::Subdirectories);
    if (!it.hasNext()) {
        return std::nullopt;
    }
    fls::CarModel model = fls::loadModelBin(it.next(), &error);
    return model.meshes.empty() ? std::nullopt
                                : std::optional<fls::CarModel>(std::move(model));
}

void appendSharedTireB(
    fls::CarModel &model, const QString &sourcePath, const fls::WheelSizing &wheels) {
    const QString leftArchive = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/scene/tires/tire_b.zip"));
    const QString rightArchive = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/scene/tires/tireR_b.zip"));
    std::optional<fls::CarModel> left = loadArchivedModel(
        leftArchive, QStringLiteral("tireL_b.modelbin"));
    std::optional<fls::CarModel> right = loadArchivedModel(
        rightArchive, QStringLiteral("tireR_b.modelbin"));
    if (!right) {
        right = loadArchivedModel(leftArchive, QStringLiteral("tireR_b.modelbin"));
    }
    // Tag the shared tire meshes with a /tires/ path so their rubber material resolves from
    // the _library like the exterior parts (appendApproximateTires copies these meshes).
    const auto tagTires = [](std::optional<fls::CarModel> &tire, const QString &path) {
        if (tire) {
            for (fls::CarMesh &mesh : tire->meshes) {
                mesh.sourceModelPath = path;
            }
        }
    };
    tagTires(left, QStringLiteral("_library/scene/tires/tireL_b.modelbin"));
    tagTires(right, QStringLiteral("_library/scene/tires/tireR_b.modelbin"));
    if (left && right) {
        const size_t firstAddedMesh = model.meshes.size();
        fls::appendApproximateTires(model, *left, *right, wheels);
        for (std::vector<fls::CarMesh> &lodMeshes : model.additionalLodMeshes) {
            lodMeshes.insert(
                lodMeshes.end(),
                model.meshes.begin() + static_cast<std::ptrdiff_t>(firstAddedMesh),
                model.meshes.end());
        }
    }
}

std::vector<std::vector<fls::CarMesh> *> renderMeshSets(fls::CarModel &model) {
    std::vector<std::vector<fls::CarMesh> *> meshSets;
    meshSets.reserve(
        2 + model.additionalLodMeshes.size() + model.additionalVariantLodMeshes.size());
    meshSets.push_back(&model.meshes);
    for (std::vector<fls::CarMesh> &lodMeshes : model.additionalLodMeshes) {
        meshSets.push_back(&lodMeshes);
    }
    meshSets.push_back(&model.variantMeshes);
    for (std::vector<fls::CarMesh> &lodMeshes : model.additionalVariantLodMeshes) {
        meshSets.push_back(&lodMeshes);
    }
    return meshSets;
}

constexpr std::array<int, 6> kSelectablePartTypes = {
    fls::car_part_types::CarBody,
    fls::car_part_types::RearWing,
    fls::car_part_types::FrontBumper,
    fls::car_part_types::RearBumper,
    fls::car_part_types::Hood,
    fls::car_part_types::SideSkirts,
};

QColor defaultPreviewCarColor() {
    return QColor(180, 182, 190);
}

QColor colorFromBgra(const std::array<quint8, 4> &bgra) {
    return QColor(bgra[2], bgra[1], bgra[0], bgra[3]);
}

std::array<quint8, 4> opaqueBgra(const QColor &color) {
    return {
        static_cast<quint8>(color.blue()),
        static_cast<quint8>(color.green()),
        static_cast<quint8>(color.red()),
        255,
    };
}

QString materialArchiveEntry(QString resourcePath) {
    resourcePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int materials = resourcePath.indexOf(
        QStringLiteral("/materials/"), 0, Qt::CaseInsensitive);
    return materials < 0 ? QString() : resourcePath.mid(materials + 11);
}

// Wheel and tire modelbins reference their materials only by slot name (rim, hub, tread…),
// not by a materialbin path, so the shared-library binding is a fixed convention. Map the
// slot name to its _library materialbin so the normal material resolver can pick it up.
QString sharedSlotMaterialEntry(const QString &materialName) {
    const QString n = materialName.toLower();
    if (n == QStringLiteral("rim") || n == QStringLiteral("rim2")) {
        return QStringLiteral("_fmnext/wheel/wheelpaint.materialbin");
    }
    if (n == QStringLiteral("black")) {
        return QStringLiteral("_fmnext/specialcase/blackhole.materialbin");
    }
    if (n == QStringLiteral("lip") || n == QStringLiteral("hub") || n == QStringLiteral("lug")
        || n == QStringLiteral("inner_rim") || n == QStringLiteral("detail")
        || n == QStringLiteral("detail2") || n == QStringLiteral("valve_cap")) {
        return QStringLiteral("wheelmaterials/aluminum_machined_satin.materialbin");
    }
    return QString();
}

// Give wheel/tire meshes a synthetic materialbin resourcePath (from their slot name) unless
// they already carry a resolvable one, so resolveExteriorMaterials loads the real tuning.
void assignSharedSlotMaterials(fls::CarModel &model) {
    for (std::vector<fls::CarMesh> *meshes : renderMeshSets(model)) {
        for (fls::CarMesh &mesh : *meshes) {
            if (!mesh.material) {
                continue;
            }
            QString path = mesh.sourceModelPath.toLower();
            path.replace(QLatin1Char('\\'), QLatin1Char('/'));
            const bool wheel = path.contains(QStringLiteral("/wheels/"));
            // Tires stay on the solid rubber fallback for now: the shared tire materials decode
            // with sub-1 opacity, which renders the approximated tires see-through.
            if (!wheel || !materialArchiveEntry(mesh.material->resourcePath).isEmpty()) {
                continue;
            }
            const QString entry = sharedSlotMaterialEntry(mesh.materialName);
            if (entry.isEmpty()) {
                continue;
            }
            mesh.material->resourcePath =
                QStringLiteral("game:/media/cars/_library/materials/") + entry;
        }
    }
}

QString assetFileIdentity(const QString &path) {
    const QFileInfo info(path);
    return QDir::cleanPath(info.absoluteFilePath()).toLower()
        + QLatin1Char('|') + QString::number(info.size())
        + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
}

QHash<QString, std::shared_ptr<fls::ModelMaterial>> &materialDefaultsCache() {
    static QHash<QString, std::shared_ptr<fls::ModelMaterial>> cache;
    return cache;
}

class NativeTextureCache {
public:
    std::shared_ptr<const fls::ModelMaterialTexture> find(const QString &key, bool &known) {
        const auto it = entries_.find(key);
        known = it != entries_.end();
        if (!known) {
            return {};
        }
        it->lastUse = ++clock_;
        return it->texture;
    }

    void insert(const QString &key,
                const std::shared_ptr<const fls::ModelMaterialTexture> &texture) {
        Entry entry;
        entry.texture = texture;
        entry.bytes = texture ? static_cast<qsizetype>(texture->image.rgba.size()) : 0;
        entry.lastUse = ++clock_;
        entries_.insert(key, std::move(entry));
        bytes_ += entries_.value(key).bytes;
    }

    void trim() {
        constexpr qsizetype budget = 256ll * 1024 * 1024;
        while (bytes_ > budget) {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (!it->texture || it->texture.use_count() != 1) {
                    continue;
                }
                if (oldest == entries_.end() || it->lastUse < oldest->lastUse) {
                    oldest = it;
                }
            }
            if (oldest == entries_.end()) {
                break;
            }
            bytes_ -= oldest->bytes;
            entries_.erase(oldest);
        }
    }

private:
    struct Entry {
        std::shared_ptr<const fls::ModelMaterialTexture> texture;
        qsizetype bytes = 0;
        quint64 lastUse = 0;
    };

    QHash<QString, Entry> entries_;
    qsizetype bytes_ = 0;
    quint64 clock_ = 0;
};

NativeTextureCache &nativeTextureCache() {
    static NativeTextureCache cache;
    return cache;
}

bool isLibraryMaterialPath(QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return path.contains(QStringLiteral("/exterior/"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("/wheels/"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("/tires/"), Qt::CaseInsensitive);
}

enum class NativeTextureSlot {
    Diffuse,
    Alpha,
    Normal,
    Surface,
    Emissive,
    Unknown,
};

NativeTextureSlot nativeTextureSlot(const fls::ModelMaterialParameter &parameter) {
    QString path = parameter.texturePath.toLower();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (parameter.nameHash == fls::material_hashes::parameter::kNormalTexture
        || path.contains(QStringLiteral("normal"))
        || path.contains(QStringLiteral("nrml"))) {
        return NativeTextureSlot::Normal;
    }
    if (parameter.nameHash == fls::material_hashes::parameter::kSurfaceTexture
        || path.contains(QStringLiteral("rmao"))
        || path.contains(QStringLiteral("roughmetal"))
        || path.contains(QStringLiteral("metalrough"))) {
        return NativeTextureSlot::Surface;
    }
    if (fls::material_hashes::contains(
            fls::material_hashes::parameter::kEmissiveTexture, parameter.nameHash)
        || path.contains(QStringLiteral("emissive"))
        || path.contains(QStringLiteral("emission"))) {
        return NativeTextureSlot::Emissive;
    }
    if (parameter.nameHash == fls::material_hashes::parameter::kColorTexture
        || path.contains(QStringLiteral("basecolor"))
        || path.contains(QStringLiteral("diffuse"))
        || path.contains(QStringLiteral("albedo"))) {
        return NativeTextureSlot::Diffuse;
    }
    if (fls::material_hashes::contains(
            fls::material_hashes::parameter::kAlphaTexture, parameter.nameHash)
        || path.contains(QStringLiteral("opacity"))
        || path.contains(QStringLiteral("opac"))
        || path.contains(QStringLiteral("alpha"))) {
        return NativeTextureSlot::Alpha;
    }
    return NativeTextureSlot::Unknown;
}

QString normalizedTexturePath(QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return path.toLower();
}

QString sharedTextureEntry(const QString &path) {
    const QString normalized = normalizedTexturePath(path);
    const QString marker = QStringLiteral("/_library/textures/");
    const int offset = normalized.indexOf(marker);
    return offset < 0 ? QString() : normalized.mid(offset + marker.size());
}

QString localTexturePath(const QString &path, const QString &carRoot) {
    QString normalized = path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int cars = normalized.indexOf(QStringLiteral("/cars/"), 0, Qt::CaseInsensitive);
    if (cars >= 0) {
        const QString carRelative = normalized.mid(cars + 6);
        const int separator = carRelative.indexOf(QLatin1Char('/'));
        if (separator >= 0) {
            return QDir(carRoot).filePath(carRelative.mid(separator + 1));
        }
    }
    if (normalized.startsWith(QStringLiteral("textures/"), Qt::CaseInsensitive)) {
        return QDir(carRoot).filePath(normalized);
    }
    return {};
}

void assignNativeTexture(fls::ModelMaterial &material,
                         NativeTextureSlot slot,
                         const std::shared_ptr<const fls::ModelMaterialTexture> &texture) {
    switch (slot) {
    case NativeTextureSlot::Diffuse:
        material.diffuseTexture = texture;
        break;
    case NativeTextureSlot::Alpha:
        material.alphaTexture = texture;
        break;
    case NativeTextureSlot::Normal:
        material.normalTexture = texture;
        break;
    case NativeTextureSlot::Surface:
        material.surfaceTexture = texture;
        break;
    case NativeTextureSlot::Emissive:
        material.emissiveTexture = texture;
        break;
    case NativeTextureSlot::Unknown:
        break;
    }
}

std::shared_ptr<const fls::ModelMaterialTexture> missingColorTexture() {
    static const std::shared_ptr<const fls::ModelMaterialTexture> texture = []() {
        QImage image(assetPath(QStringLiteral("raster/MissingTexture.png")));
        image = image.convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) {
            return std::shared_ptr<const fls::ModelMaterialTexture>{};
        }
        auto result = std::make_shared<fls::ModelMaterialTexture>();
        result->path = QStringLiteral("raster/MissingTexture.png");
        result->image.width = image.width();
        result->image.height = image.height();
        result->image.rgba.resize(static_cast<size_t>(image.width()) * image.height() * 4);
        for (int row = 0; row < image.height(); ++row) {
            std::memcpy(
                result->image.rgba.data() + static_cast<size_t>(row) * image.width() * 4,
                image.constScanLine(row),
                static_cast<size_t>(image.width()) * 4);
        }
        return std::shared_ptr<const fls::ModelMaterialTexture>(std::move(result));
    }();

    return texture;
}

void assignNativeTextureOrFallback(
    fls::ModelMaterial &material,
    NativeTextureSlot slot,
    const std::shared_ptr<const fls::ModelMaterialTexture> &texture) {
    assignNativeTexture(
        material, slot,
        texture || slot != NativeTextureSlot::Diffuse ? texture : missingColorTexture());
}

void cacheMaterialDefaults(const QString &archivePath, const QString &archiveKey,
                            const QStringList &entries) {
    auto &defaults = materialDefaultsCache();
    const QHash<QString, QByteArray> missingMaterialData =
        fls::readZipEntries(archivePath, entries);

    for (const QString &entry : entries) {
        std::shared_ptr<fls::ModelMaterial> decoded;
        const QByteArray bytes = missingMaterialData.value(entry.toLower());
        if (!bytes.isEmpty()) {
            try {
                decoded = fls::decodeMaterialBundle(bytes);
            } catch (const std::exception &) {
            }
        }
        defaults.insert(archiveKey + QLatin1Char('|') + entry.toLower(), decoded);
    }
}

void resolveExteriorMaterials(
    fls::CarModel &model, const QString &sourcePath, const QString &carRoot,
    bool includeTextures) {
    const QString archivePath = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/Materials.zip"));
    if (archivePath.isEmpty()) {
        return;
    }
    const QString materialArchiveKey = assetFileIdentity(archivePath);
    QHash<QString, std::shared_ptr<fls::ModelMaterial>> &defaults = materialDefaultsCache();
    QStringList missingMaterialEntries;
    QSet<QString> requestedMaterials;
    for (std::vector<fls::CarMesh> *meshes : renderMeshSets(model)) {
        for (fls::CarMesh &mesh : *meshes) {
            if (!mesh.material || !isLibraryMaterialPath(mesh.sourceModelPath)) {
                continue;
            }
            const QString entry = materialArchiveEntry(mesh.material->resourcePath);
            if (entry.isEmpty()) {
                continue;
            }
            const QString key = materialArchiveKey + QLatin1Char('|') + entry.toLower();
            if (!defaults.contains(key) && !requestedMaterials.contains(entry.toLower())) {
                requestedMaterials.insert(entry.toLower());
                missingMaterialEntries.push_back(entry);
            }
        }
    }
    cacheMaterialDefaults(archivePath, materialArchiveKey, missingMaterialEntries);
    for (std::vector<fls::CarMesh> *meshes : renderMeshSets(model)) {
        for (fls::CarMesh &mesh : *meshes) {
            if (!mesh.material || !isLibraryMaterialPath(mesh.sourceModelPath)) {
                continue;
            }
            const QString entry = materialArchiveEntry(mesh.material->resourcePath);
            const std::shared_ptr<fls::ModelMaterial> materialDefaults =
                defaults.value(materialArchiveKey + QLatin1Char('|') + entry.toLower());
            if (materialDefaults) {
                mesh.material = fls::mergeModelMaterialDefaults(*materialDefaults, *mesh.material);
                mesh.material->resolvedFromLibrary = true;
            }
        }
    }

    if (!includeTextures) {
        return;
    }

    struct PendingTexture {
        std::shared_ptr<fls::ModelMaterial> material;
        NativeTextureSlot slot = NativeTextureSlot::Unknown;
        QString path;
        QString sharedEntry;
        QString localPath;
        QString cacheKey;
    };
    QVector<PendingTexture> pending;
    const auto appendPending = [&](const std::shared_ptr<fls::ModelMaterial> &material,
                                   NativeTextureSlot slot, const QString &path) {
        PendingTexture item;
        item.material = material;
        item.slot = slot;
        item.path = path;
        item.sharedEntry = sharedTextureEntry(item.path);
        if (item.sharedEntry.isEmpty()) {
            item.localPath = localTexturePath(item.path, carRoot);
        }
        pending.push_back(std::move(item));
    };
    QSet<const fls::ModelMaterial *> visited;
    for (std::vector<fls::CarMesh> *meshes : renderMeshSets(model)) {
        for (fls::CarMesh &mesh : *meshes) {
            if (!mesh.material || visited.contains(mesh.material.get()) ||
                !isLibraryMaterialPath(mesh.sourceModelPath) ||
                mesh.materialName.startsWith(QStringLiteral("carPaint"), Qt::CaseInsensitive)) {
                continue;
            }
            visited.insert(mesh.material.get());
            bool hasNormal = false;
            for (const fls::ModelMaterialParameter &parameter : mesh.material->parameters) {
                if (parameter.type != fls::ModelMaterialParameterType::Texture2D || parameter.texturePath.isEmpty()) {
                    continue;
                }
                const NativeTextureSlot slot = nativeTextureSlot(parameter);
                if (slot == NativeTextureSlot::Unknown) {
                    continue;
                }
                // Flat placeholder swatches (e.g. the wheel paint's green base) are solid colours the
                // game replaces with the chosen paint; loaded as real maps they tint painted parts
                // (green rims) and skew their shading, so drop them and let the paint colour stand.
                if (normalizedTexturePath(parameter.texturePath)
                        .contains(QStringLiteral("globaltexture/swatches/flat_texture"))) {
                    continue;
                }
                if (slot == NativeTextureSlot::Normal) {
                    hasNormal = true;
                }
                appendPending(mesh.material, slot, parameter.texturePath);
            }
            // The car_carbonfiber shader bakes its weave into the shader, so the plain carbonfiber
            // materialbin names no maps at all (only 32x tiling): a mesh using it arrives with no
            // normal and renders as flat gloss. Supply the shader's canonical twin-twill weave
            // normal/rmao — the very swatches the carbonfiber_livery variant lists explicitly — so
            // structural carbon reads as carbon. The material's own 32x tiling turns it into a weave.
            if (!hasNormal && normalizedTexturePath(mesh.material->resourcePath)
                                  .contains(QStringLiteral("/carbonfiber/carbonfiber"))) {
                appendPending(mesh.material, NativeTextureSlot::Normal,
                              QStringLiteral("Game:\\Media\\cars\\_library\\textures\\_fmnext\\carbonfiber"
                                             "\\twin_twill_weave\\swatches\\twin_twill_weave_normal_5eewzs7"
                                             ".swatchbin"));
                appendPending(mesh.material, NativeTextureSlot::Surface,
                              QStringLiteral("Game:\\Media\\cars\\_library\\textures\\_fmnext\\carbonfiber"
                                             "\\twin_twill_weave\\swatches\\twin_twill_weave_rmao_x9t8d8u"
                                             ".swatchbin"));
            }
        }
    }

    const QString textureArchive = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/Textures.zip"));
    const QString textureArchiveKey = textureArchive.isEmpty()
        ? QString()
        : assetFileIdentity(textureArchive);
    const bool sourceIsArchive = sourcePath.endsWith(
        QStringLiteral(".zip"), Qt::CaseInsensitive);
    const QString sourceArchiveKey = sourceIsArchive
        ? assetFileIdentity(sourcePath)
        : QString();
    NativeTextureCache &textureCache = nativeTextureCache();
    QStringList missingSharedEntries;
    QSet<QString> requestedSharedEntries;
    for (PendingTexture &item : pending) {
        item.cacheKey = !item.sharedEntry.isEmpty()
            ? textureArchiveKey + QLatin1Char('|') + item.sharedEntry.toLower()
            : (sourceIsArchive
                ? sourceArchiveKey + QLatin1Char('|') + normalizedTexturePath(item.path)
                : !item.localPath.isEmpty()
                ? assetFileIdentity(item.localPath)
                : QDir::cleanPath(carRoot).toLower() + QLatin1Char('|')
                    + normalizedTexturePath(item.path));
        bool known = false;
        const auto texture = textureCache.find(item.cacheKey, known);
        if (known) {
            assignNativeTextureOrFallback(*item.material, item.slot, texture);
            item.cacheKey.clear();
        } else if (!item.sharedEntry.isEmpty()
                   && !requestedSharedEntries.contains(item.sharedEntry.toLower())) {
            requestedSharedEntries.insert(item.sharedEntry.toLower());
            missingSharedEntries.push_back(item.sharedEntry);
        }
    }
    const QHash<QString, QByteArray> sharedData = textureArchive.isEmpty()
        ? QHash<QString, QByteArray>{}
        : fls::readZipEntries(textureArchive, missingSharedEntries);
    for (const PendingTexture &item : pending) {
        if (item.cacheKey.isEmpty()) {
            continue;
        }
        bool known = false;
        std::shared_ptr<const fls::ModelMaterialTexture> texture =
            textureCache.find(item.cacheKey, known);
        if (!known) {
            QByteArray bytes;
            if (!item.sharedEntry.isEmpty()) {
                bytes = sharedData.value(item.sharedEntry.toLower());
            } else if (!item.localPath.isEmpty()) {
                QFile file(item.localPath);
                if (file.open(QIODevice::ReadOnly)) {
                    bytes = file.readAll();
                }
            }
            if (!bytes.isEmpty()) {
                fls::SwatchImage image = fls::decodeSwatchImage(bytes);
                if (image.valid()) {
                    auto decoded = std::make_shared<fls::ModelMaterialTexture>();
                    decoded->path = item.path;
                    decoded->image = std::move(image);
                    texture = std::move(decoded);
                }
            }
            textureCache.insert(item.cacheKey, texture);
        }
        assignNativeTextureOrFallback(*item.material, item.slot, texture);
    }
    textureCache.trim();
}

fls::ManufacturerColorPalette loadManufacturerColors(
    const QString &carRoot, const QString &sourcePath) {
    QFile file(QDir(carRoot).filePath(QStringLiteral("ManufacturerColors.bin")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    fls::ManufacturerColorPalette palette;
    try {
        palette = fls::decodeManufacturerColors(file.readAll());
    } catch (const std::exception &) {
        return {};
    }

    const QString archivePath = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/Materials.zip"));
    if (archivePath.isEmpty()) {
        return palette;
    }

    const QString archiveKey = assetFileIdentity(archivePath);
    QHash<QString, std::shared_ptr<fls::ModelMaterial>> &defaults = materialDefaultsCache();
    QStringList missingEntries;
    QSet<QString> requestedEntries;
    for (const fls::ManufacturerColor &color : std::as_const(palette.colors)) {
        const QString entry = materialArchiveEntry(color.materialPath);
        const QString lowerEntry = entry.toLower();
        const QString key = archiveKey + QLatin1Char('|') + lowerEntry;
        if (!entry.isEmpty() && !defaults.contains(key)
            && !requestedEntries.contains(lowerEntry)) {
            requestedEntries.insert(lowerEntry);
            missingEntries.push_back(entry);
        }
    }

    cacheMaterialDefaults(archivePath, archiveKey, missingEntries);
    for (fls::ManufacturerColor &color : palette.colors) {
        const QString entry = materialArchiveEntry(color.materialPath);
        color.material = defaults.value(
            archiveKey + QLatin1Char('|') + entry.toLower());
    }

    return palette;
}

struct PreparedCar {
    fls::CarModel model;
    fls::LiveryMaskSet liveryMasks;
    fls::ManufacturerColorPalette manufacturerColors;
    std::vector<CarUnwrapOverlaySet> carUnwrapOverlaySets;
    std::shared_ptr<QTemporaryDir> extractedCarDir;
    QString loadedCarPath;
    QString liveryMasksDir;
};

std::shared_ptr<PreparedCar> prepareCar(
    const QString &path, bool loadCarTextures, QString *error, QJsonObject *timings = nullptr,
    const std::function<bool()> &cancelled = {}, const QString &game = {}) {
    static QMutex loadMutex;
    QElapsedTimer timer;
    qint64 previousTime = 0;
    timer.start();
    const auto checkpoint = [&](const QString &stage) {
        const qint64 currentTime = timer.nsecsElapsed();
        if (timings != nullptr) {
            timings->insert(stage, (currentTime - previousTime) / 1000000.0);
        }
        previousTime = currentTime;
    };
    QMutexLocker lock(&loadMutex);
    checkpoint(QStringLiteral("wait_ms"));
    if (cancelled && cancelled()) {
        return {};
    }
    QString loadPath = path;
    std::shared_ptr<QTemporaryDir> extracted;
    bool extractionReused = false;
    if (path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        extracted = fls::sharedZipExtraction(path, error, &extractionReused);
        if (!extracted) {
            return {};
        }
        loadPath = findCarbin(extracted->path());
        if (loadPath.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("zip: no .carbin found in %1").arg(QFileInfo(path).fileName());
            }
            return {};
        }
    }
    checkpoint(QStringLiteral("extract_ms"));
    if (timings != nullptr) {
        timings->insert(QStringLiteral("extraction_reused"), extractionReused);
    }

    const fls::WheelSizing wheelSizing =
        wheelSizingForModelCode(QFileInfo(loadPath).completeBaseName(), game);
    fls::CarModel model = loadPath.endsWith(QStringLiteral(".carbin"), Qt::CaseInsensitive)
        ? fls::loadCarBin(loadPath, error, wheelSizing)
        : fls::loadModelBin(loadPath, error);
    if (model.meshes.empty()) {
        return {};
    }
    checkpoint(QStringLiteral("geometry_ms"));
    appendSharedTireB(model, path, wheelSizing);
    checkpoint(QStringLiteral("tires_ms"));
    assignSharedSlotMaterials(model);
    resolveExteriorMaterials(model, path, QFileInfo(loadPath).absolutePath(), loadCarTextures);
    checkpoint(QStringLiteral("materials_ms"));

    const QDir carDir = QFileInfo(loadPath).absoluteDir();
    auto prepared = std::make_shared<PreparedCar>();
    prepared->model = std::move(model);
    prepared->manufacturerColors = loadManufacturerColors(carDir.absolutePath(), path);
    checkpoint(QStringLiteral("manufacturer_ms"));
    prepared->extractedCarDir = std::move(extracted);
    prepared->loadedCarPath = path;
    lock.unlock();
    if (cancelled && cancelled()) {
        return {};
    }

    const QString masksDir = carDir.filePath(QStringLiteral("LiveryMasks"));
    if (QFileInfo::exists(masksDir)) {
        prepared->liveryMasks = fls::loadLiveryMasks(masksDir);
        checkpoint(QStringLiteral("masks_ms"));
        prepared->liveryMasksDir = masksDir;
        QSet<int> selectablePartTypes;
        for (int partType : kSelectablePartTypes) {
            selectablePartTypes.insert(partType);
        }
        QJsonObject unwrapTimings;
        prepared->carUnwrapOverlaySets = buildCarUnwrapOverlaySets(
            prepared->model, prepared->liveryMasks, selectablePartTypes,
            timings != nullptr ? &unwrapTimings : nullptr, cancelled);
        if (timings != nullptr) {
            timings->insert(QStringLiteral("unwrap_stages"), unwrapTimings);
        }
        checkpoint(QStringLiteral("unwrap_ms"));
    }
    if (cancelled && cancelled()) {
        return {};
    }
    if (timings != nullptr) {
        timings->insert(QStringLiteral("total_ms"), timer.nsecsElapsed() / 1000000.0);
    }

    return prepared;
}

} // namespace

QJsonObject profileCarLoading(const QString &path, bool loadTextures, int repetitions, bool verifyUnwrap) {
    QJsonArray runs;
    std::shared_ptr<QTemporaryDir> extraction;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        QJsonObject timings;
        QString error;
        std::shared_ptr<PreparedCar> prepared = prepareCar(path, loadTextures, &error, &timings);
        if (!prepared) {
            return {{QStringLiteral("error"), error}, {QStringLiteral("runs"), runs}};
        }
        extraction = prepared->extractedCarDir;
        timings.insert(QStringLiteral("meshes"), static_cast<qint64>(prepared->model.meshes.size()));
        timings.insert(QStringLiteral("variant_meshes"), static_cast<qint64>(prepared->model.variantMeshes.size()));
        timings.insert(QStringLiteral("lods"), prepared->model.lodCount());
        timings.insert(QStringLiteral("manufacturer_colors"), prepared->manufacturerColors.colors.size());
        if (verifyUnwrap && prepared->liveryMasks.valid()) {
            QSet<int> partTypes;
            bool matches = true;
            for (int partType : kSelectablePartTypes) {
                partTypes.insert(partType);
            }
            for (int lod = 0; lod < prepared->model.lodCount(); ++lod) {
                matches = matches && buildCarUnwrapOverlaySet(
                    prepared->model, prepared->liveryMasks, partTypes, lod)
                    == prepared->carUnwrapOverlaySets[lod];
            }
            timings.insert(QStringLiteral("unwrap_matches_serial"), matches);
            if (!matches) {
                runs.append(timings);
                return {{QStringLiteral("error"), QStringLiteral("parallel unwrap differs from serial calculation")},
                        {QStringLiteral("runs"), runs}};
            }
        }
        runs.append(timings);
    }

    return {{QStringLiteral("path"), path}, {QStringLiteral("textures"), loadTextures},
            {QStringLiteral("runs"), runs}};
}

CarPreviewWidget::CarPreviewWidget(QWidget *parent)
    : QOpenGLWidget(parent) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setAlphaBufferSize(8);
    format.setSamples(4);
    setFormat(format);
    setFocusPolicy(Qt::StrongFocus);

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

    modelStatus_ = new QLabel(this);
    modelStatus_->setAttribute(Qt::WA_TransparentForMouseEvents);
    modelStatus_->setAlignment(Qt::AlignCenter);
    modelStatus_->setStyleSheet(QStringLiteral(
        "QLabel {"
        " color: rgba(235, 236, 240, 220);"
        " background: rgba(0, 0, 0, 105);"
        " border-radius: 4px;"
        " padding: 6px 10px;"
        "}"));

    sidePanel_ = new QFrame(this);
    sidePanel_->setObjectName(QStringLiteral("previewSidePanel"));
    sidePanel_->setAttribute(Qt::WA_StyledBackground, true);
    sidePanel_->setStyleSheet(QStringLiteral(
        "QFrame#previewSidePanel {"
        " background: rgba(24, 25, 29, 246);"
        " border-left: 1px solid rgba(255, 255, 255, 42);"
        "}"
        "QLabel { color: rgb(235, 236, 240); }"
        "QLabel[secondary=\"true\"] { color: rgba(220, 222, 228, 155); }"
        "QPushButton { text-align: left; padding: 5px 7px; }"
        "QPushButton#previewClearPaint {"
        " color: rgba(230, 232, 236, 190);"
        " background: transparent;"
        " border: 1px solid rgba(255, 255, 255, 42);"
        " border-radius: 3px;"
        " padding: 3px 9px;"
        " text-align: center;"
        "}"
        "QPushButton#previewClearPaint:hover {"
        " color: rgb(245, 246, 248);"
        " background: rgba(255, 255, 255, 14);"
        "}"
        "QComboBox { padding: 5px 7px; }"
        "QScrollArea { background: transparent; border: 0; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"));

    auto *panelLayout = new QVBoxLayout(sidePanel_);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    auto *heading = new QLabel(QStringLiteral("PAINT"), sidePanel_);
    heading->setContentsMargins(42, 10, 10, 10);
    panelLayout->addWidget(heading);
    auto *scroll = new QScrollArea(sidePanel_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    colorsPage_ = new QWidget(scroll);
    colorsLayout_ = new QVBoxLayout(colorsPage_);
    colorsLayout_->setContentsMargins(12, 10, 12, 12);
    colorsLayout_->setSpacing(10);
    scroll->setWidget(colorsPage_);
    panelLayout->addWidget(scroll, 1);

    sidePanelToggle_ = new QToolButton(this);
    sidePanelToggle_->setObjectName(QStringLiteral("previewSidePanelToggle"));
    sidePanelToggle_->setFocusPolicy(Qt::NoFocus);
    sidePanelToggle_->setFixedSize(29, 32);
    sidePanelToggle_->setStyleSheet(QStringLiteral(
        "QToolButton#previewSidePanelToggle {"
        " color: rgb(235, 236, 240);"
        " background: rgba(24, 25, 29, 246);"
        " border: 1px solid rgba(255, 255, 255, 42);"
        " border-radius: 4px;"
        " font-size: 16px;"
        "}"
        "QToolButton#previewSidePanelToggle:hover {"
        " background: rgba(54, 57, 64, 250);"
        "}"));
    connect(sidePanelToggle_, &QToolButton::clicked, this, [this]() {
        setSidePanelVisible(!sidePanelVisible_);
    });

    resetPartSelections();
    updateCarColorControl();

    setSidePanelVisible(false);
    updateModelStatus();
    connect(this, &QOpenGLWidget::frameSwapped, this, [this]() {
        if (!carLoadTimings_.contains(QStringLiteral("render_submit_ms"))
            || carLoadTimings_.contains(QStringLiteral("first_frame_ms"))) {
            return;
        }
        carLoadTimings_.insert(QStringLiteral("first_frame_ms"), carLoadTimer_.nsecsElapsed() / 1000000.0);
        if (qEnvironmentVariableIsSet("FLS_PERF_LOG") || qEnvironmentVariableIsSet("FORZA_PERF_LOG")) {
            qInfo().noquote() << "[car-load]" << QJsonDocument(carLoadTimings_).toJson(QJsonDocument::Compact);
        }
        const QString reportPath = qEnvironmentVariable("FLS_MODEL_LOAD_REPORT");
        if (!reportPath.isEmpty()) {
            QFile report(reportPath);
            if (report.open(QIODevice::WriteOnly)) {
                report.write(QJsonDocument(carLoadTimings_).toJson());
            }
        }
    });
}

CarPreviewWidget::~CarPreviewWidget() {
    cancelCarLoad();
    makeCurrent();
    carRenderer_.release();
    shapeRenderer_.release();
    doneCurrent();
}

void CarPreviewWidget::loadCarAsync(const QString &path, CarLoadCallback callback, const QString &game) {
    const bool loadCarTextures = loadCarTextures_;
    const QString key = assetFileIdentity(path) + QLatin1Char('|') + QString::number(loadCarTextures)
        + QLatin1Char('|') + game;
    if (modelLoadPending_ && pendingCarKey_ == key) {
        if (callback) {
            carLoadCallbacks_.push_back(std::move(callback));
        }
        return;
    }
    if (!modelLoadPending_ && hasModel() && loadedCarKey_ == key) {
        if (callback) {
            callback(true, {});
        }
        return;
    }
    cancelCarLoad();
    carLoadTimer_.start();
    carLoadTimings_ = {};
    pendingCarKey_ = key;
    carLoadCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = carLoadCancelled_;
    if (callback) {
        carLoadCallbacks_.push_back(std::move(callback));
    }
    const quint64 generation = ++carLoadGeneration_;

    modelLoadPending_ = true;
    updateModelStatus();
    QPointer<CarPreviewWidget> guard(this);
    QThreadPool::globalInstance()->start(
        [guard, path, key, game, loadCarTextures, generation, cancellation, started = carLoadTimer_]() mutable {
            QString error;
            QJsonObject timings;
            const auto cancelled = [cancellation]() { return cancellation->load(std::memory_order_relaxed); };
            timings.insert(QStringLiteral("worker_start_ms"), started.nsecsElapsed() / 1000000.0);
            std::shared_ptr<PreparedCar> prepared =
                prepareCar(path, loadCarTextures, &error, &timings, cancelled, game);
            timings.insert(QStringLiteral("cpu_ready_ms"), started.nsecsElapsed() / 1000000.0);
            if (!guard || cancelled()) {
                return;
            }
            QMetaObject::invokeMethod(
                guard,
                [guard, key, game, prepared = std::move(prepared), error = std::move(error),
                 generation, timings = std::move(timings)]() mutable {
                    if (!guard || guard->carLoadGeneration_ != generation) {
                        return;
                    }
                    QElapsedTimer adoption;
                    adoption.start();
                    timings.insert(QStringLiteral("gui_dispatch_ms"), guard->carLoadTimer_.nsecsElapsed() / 1000000.0
                        - timings.value(QStringLiteral("cpu_ready_ms")).toDouble());
                    guard->carLoadTimings_ = std::move(timings);
                    guard->carLoadTimings_.insert(QStringLiteral("path"), prepared ? prepared->loadedCarPath : QString());
                    std::vector<CarLoadCallback> callbacks = std::move(guard->carLoadCallbacks_);
                    guard->pendingCarKey_.clear();
                    guard->modelLoadPending_ = false;
                    if (!prepared) {
                        guard->updateModelStatus();
                        for (const CarLoadCallback &callback : callbacks) {
                            callback(false, error);
                        }
                        return;
                    }

                    guard->model_ = std::move(prepared->model);
                    guard->manufacturerColors_ = std::move(prepared->manufacturerColors);
                    guard->extractedCarDir_ = std::move(prepared->extractedCarDir);
                    guard->loadedCarPath_ = std::move(prepared->loadedCarPath);
                    guard->loadedCarGame_ = game;
                    guard->loadedCarKey_ = key;
                    guard->liveryMasks_ = std::move(prepared->liveryMasks);
                    guard->carUnwrapOverlaySets_ = std::move(
                        prepared->carUnwrapOverlaySets);
                    guard->liveryMasksDir_ = std::move(prepared->liveryMasksDir);
                    guard->selectedLodIndex_ = 0;
                    guard->resetPartSelections();
                    guard->switchCarUnwrapOverlay();
                    guard->paintControls_.clear();
                    guard->updateCarColorControl();
                    guard->modelUploadPending_ = true;
                    guard->modelFitPending_ = true;
                    guard->liveryMasksPending_ = true;
                    guard->invalidateCachedLivery();

                    guard->updateModelStatus();
                    guard->update();
                    guard->carLoadTimings_.insert(QStringLiteral("gui_adopt_ms"), adoption.nsecsElapsed() / 1000000.0);
                    for (const CarLoadCallback &callback : callbacks) {
                        callback(true, {});
                    }
                },
                Qt::QueuedConnection);
        });
}

void CarPreviewWidget::cancelCarLoad() {
    if (carLoadCancelled_) {
        carLoadCancelled_->store(true, std::memory_order_relaxed);
    }
    pendingCarKey_.clear();
    carLoadCallbacks_.clear();
    carLoadTimings_ = {};
    ++carLoadGeneration_;
    modelLoadPending_ = false;
    updateModelStatus();
}

bool CarPreviewWidget::hasModel() const {
    return !model_.meshes.empty();
}

QImage CarPreviewWidget::renderThumbnail(const QSize &size) {
    if (!hasModel() || !size.isValid() || size.isEmpty()) {
        return {};
    }
    const int debugMode = carRenderer_.debugMode();
    const bool wireframeVisible = carRenderer_.wireframeVisible();
    const int selectedLodIndex = selectedLodIndex_;
    if (selectedLodIndex != 0) {
        selectedLodIndex_ = 0;
        modelUploadPending_ = true;
    }
    carRenderer_.setDebugMode(0);
    carRenderer_.setWireframeVisible(false);
    transparentBackground_ = true;
    repaint();
    const QImage framebuffer = grabFramebuffer();
    transparentBackground_ = false;
    carRenderer_.setDebugMode(debugMode);
    carRenderer_.setWireframeVisible(wireframeVisible);
    if (selectedLodIndex != 0) {
        selectedLodIndex_ = selectedLodIndex;
        modelUploadPending_ = true;
    }
    update();
    if (framebuffer.isNull()) {
        return {};
    }
    const QImage scaled = framebuffer.scaled(
        size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = std::max(0, (scaled.width() - size.width()) / 2);
    const int y = std::max(0, (scaled.height() - size.height()) / 2);
    return scaled.copy(x, y, size.width(), size.height());
}

void CarPreviewWidget::clearModel() {
    cancelCarLoad();
    loadedCarPath_.clear();
    loadedCarGame_.clear();
    loadedCarKey_.clear();
    manufacturerColors_ = {};
    selectedLodIndex_ = 0;
    selectedPartOptions_.clear();
    paintControls_.clear();
    carRenderer_.setPartSelections(selectedPartOptions_);
    if (!hasModel() && !carRenderer_.hasModel()) {
        resetPartSelections();
        updateCarColorControl();

        updateModelStatus();
        return;
    }
    model_ = fls::CarModel{};
    resetPartSelections();
    updateCarColorControl();

    liveryMasks_ = {};
    carUnwrapOverlay_ = {};
    carUnwrapOverlaySets_.clear();
    liveryMasksDir_.clear();
    modelUploadPending_ = false;
    modelFitPending_ = false;
    liveryMasksPending_ = false;
    invalidateCachedLivery();
    if (carRenderer_.isInitialized()) {
        makeCurrent();
        carRenderer_.clearModel();
        doneCurrent();
    }
    updateModelStatus();
    update();
}

CarUnwrapOverlay CarPreviewWidget::unwrapOverlay(int liverySectionSlot) const {
    if (liverySectionSlot < 0) {
        return carUnwrapOverlay_;
    }
    if (liverySectionSlot >= fls::kLiverySideCount) {
        return {};
    }

    CarUnwrapOverlay overlay;
    const int maskSlot = kLiverySectionMaskSlots[liverySectionSlot];
    overlay.sides[maskSlot] = carUnwrapOverlay_.sides[maskSlot];

    return overlay;
}

std::optional<QTransform> CarPreviewWidget::liverySectionToAtlasTransform(int liverySectionSlot) const {
    if (modelLoadPending_ || !hasModel() || !liveryMasks_.valid()
        || liverySectionSlot < 0 || liverySectionSlot >= fls::kLiverySideCount) {
        return std::nullopt;
    }
    const int maskSlot = kLiverySectionMaskSlots[liverySectionSlot];
    const fls::LiverySide &side = liveryMasks_.sides[maskSlot];
    const QTransform maskToAtlas(1.0, 0.0, 0.0, -1.0,
                                 fls::kLiveryCanvasHalfWidth, fls::kLiveryCanvasHalfHeight);

    if (!side.valid || !side.mask.valid()) {
        return std::nullopt;
    }

    return liveryMaskToSectionTransform(side, maskSlot).inverted() * maskToAtlas;
}

void CarPreviewWidget::setSectionWireframeVisible(bool visible) {
    if (carRenderer_.wireframeVisible() == visible) {
        return;
    }
    carRenderer_.setWireframeVisible(visible);
    update();
}

void CarPreviewWidget::cycleLod() {
    if (model_.lodCount() < 2) {
        return;
    }
    selectLod((selectedLodIndex_ + 1) % model_.lodCount());
}

void CarPreviewWidget::selectLod(int lodIndex) {
    if (lodIndex < 0 || lodIndex >= model_.lodCount()
        || selectedLodIndex_ == lodIndex) {
        return;
    }
    selectedLodIndex_ = lodIndex;
    switchCarUnwrapOverlay();
    modelUploadPending_ = true;

    rebuildColorsPanel();
    Q_EMIT unwrapOverlayChanged();
    update();
}

void CarPreviewWidget::setCarUnwrapSection(int liverySectionSlot) {
    const int maskSlot = liverySectionSlot < 0
        ? -1
        : liverySectionSlot < fls::kLiverySideCount
            ? kLiverySectionMaskSlots[liverySectionSlot]
            : fls::kLiverySideCount;
    carRenderer_.setWireframeSide(maskSlot);
    update();
}

void CarPreviewWidget::setProject(fls::Project *project) {
    if (paintPopup_) paintPopup_->close();
    project_ = project;
    paintControls_.clear();
    invalidateCachedLivery();
    updateCarColorControl();
    update();
}

void CarPreviewWidget::setEditorState(EditorState *state) {
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
        connect(state_, &EditorState::projectPaintChanged, this, [this]() {
            updateCarColorControl();
            update();
        });
        connect(state_, &EditorState::projectStructureChanged, this, [this]() {
            updateCarColorControl();
            markLiveryDirty();
        });
    }
    updateCarColorControl();
}

QColor CarPreviewWidget::basePaint() const {
    return basePaint_;
}

void CarPreviewWidget::setBasePaint(const QColor &color) {
    if (!color.isValid() || color == basePaint_) {
        return;
    }
    basePaint_ = color;
    update();
}

fls::LiveryPaintMaterial CarPreviewWidget::slotPaint(const CarPaintSlot &slot) const {
    fls::LiveryPaintMaterial result;
    result.materialHash = slot.hash;
    if (project_ == nullptr) return result;
    if (const auto *material = project_->liveryPaint.find(slot.hash)) result = *material;
    if (!result.primary.enabled && result.manufacturerSelector == 0xffffffffu) {
        for (quint64 fallbackHash : slot.fallbackHashes) {
            if (const auto *fallback = project_->liveryPaint.find(fallbackHash)) {
                if (fallback->primary.enabled || fallback->manufacturerSelector != 0xffffffffu) {
                    result.primary = fallback->primary;
                    result.secondary = fallback->secondary;
                    result.manufacturerSelector = fallback->manufacturerSelector;
                    break;
                }
            }
        }
    }

    return result;
}

QColor CarPreviewWidget::slotColor(const CarPaintSlot &slot, bool secondary) const {
    const fls::LiveryPaintMaterial material = slotPaint(slot);
    const bool positional = project_ != nullptr && !project_->liveryPaint.game.isEmpty()
        && project_->liveryPaint.game != QStringLiteral("FH6");
    if (project_ != nullptr && material.manufacturerSelector != 0xffffffffu
        && (positional || !material.primary.enabled)) {
        for (const PaintColorPreset &preset : carManufacturerColors(
                 project_->liveryPaint.game, project_->carId, slot.hash, secondary)) {
            if (preset.index == static_cast<int>(material.manufacturerSelector)) return preset.color;
        }
    }
    const auto &color = secondary && material.secondary.enabled ? material.secondary : material.primary;
    if (color.enabled) return colorFromBgra(color.bgra);

    if (slot.window) return QColor(Qt::black);

    return slot.hash == fls::material_hashes::binding::kBodyPaint
        || slot.fallbackHashes.contains(fls::material_hashes::binding::kBodyPaint) ? basePaint_ : defaultPreviewCarColor();
}

void CarPreviewWidget::chooseSlotColor(const CarPaintSlot &slot, bool secondary, QWidget *anchor) {
    if (!state_ || project_ == nullptr || !project_->isLivery || state_->project() != project_) return;
    if (paintPopup_) paintPopup_->close();
    QVector<PaintColorPreset> swatches;
    for (const auto &color : project_->colorSwatches) {
        swatches.push_back({colorFromBgra(color).name(QColor::HexRgb), colorFromBgra(color)});
    }
    auto *popup = new PaintColorPopup(slotColor(slot, secondary),
        carManufacturerColors(project_->liveryPaint.game, project_->carId, slot.hash, secondary), swatches, this);
    paintPopup_ = popup;
    popup->setWindowTitle(slot.name + (secondary ? QStringLiteral(" / Secondary") : QStringLiteral(" / Primary")));
    const fls::LiveryPaintState before = project_->liveryPaint;
    const auto *root = project_->root.get();
    const QPointer<EditorState> editor = state_;
    const auto valid = [this, root, editor]() {
        return editor && state_ == editor && project_ != nullptr && state_->project() == project_
            && project_->root.get() == root;
    };
    state_->beginProjectEdit();
    connect(popup, &PaintColorPopup::colorChanged, this, [this, popup, slot, secondary, valid](const QColor &color) {
        if (!valid()) {
            popup->close();
            return;
        }
        const auto previous = slotPaint(slot);
        const bool positional = !project_->liveryPaint.game.isEmpty() && project_->liveryPaint.game != QStringLiteral("FH6");
        const bool manufacturer = previous.manufacturerSelector != 0xffffffffu && (positional || !previous.primary.enabled);
        const QColor primary = slotColor(slot, false);
        const QColor otherColor = manufacturer ? slotColor(slot, true) : QColor();
        if (slot.hash == fls::material_hashes::binding::kBodyPaint && !secondary) {
            for (const PaintControls &controls : paintControls_) {
                if (!controls.slot.fallbackHashes.contains(fls::material_hashes::binding::kBodyPaint)) continue;
                const quint64 hash = controls.slot.hash;
                const auto *child = project_->liveryPaint.find(hash);
                if (child == nullptr || (!child->primary.enabled && child->manufacturerSelector == 0xffffffffu)) {
                    const CarPaintSlot childSlot = {QString(), {fls::material_hashes::binding::kBodyPaint}, hash};
                    project_->liveryPaint.setColorBgra(hash, false, opaqueBgra(slotColor(childSlot, false)));
                }
            }
        }
        auto &material = project_->liveryPaint.ensure(slot.hash);
        if (secondary && (!material.primary.enabled || manufacturer)) material.primary = {true, opaqueBgra(primary)};
        if (!secondary && manufacturer && material.secondary.enabled) material.secondary.bgra = opaqueBgra(otherColor);
        project_->liveryPaint.setColorBgra(slot.hash, secondary, opaqueBgra(color));
        if (!slot.window && material.finish == 0) material.finish = 1;
        state_->noteProjectPaintChanged();
    });
    connect(popup, &PaintColorPopup::finished, this, [this, popup, before, valid](bool accepted) {
        if (valid()) {
            if (!accepted) project_->liveryPaint = before;
            state_->commitProjectEdit();
            state_->noteProjectPaintChanged();
        }
        if (paintPopup_ == popup) paintPopup_.clear();
        popup->deleteLater();
    });
    popup->adjustSize();
    const QRect available = anchor->screen()->availableGeometry();
    QPoint position = anchor->mapToGlobal(QPoint(0, anchor->height()));
    position.setX(std::clamp(position.x(), available.left(), std::max(available.left(), available.right() - popup->width() + 1)));
    position.setY(std::clamp(position.y(), available.top(), std::max(available.top(), available.bottom() - popup->height() + 1)));
    popup->move(position);
    popup->show();
}

void CarPreviewWidget::chooseSlotFinish(quint64 hash, quint32 finish) {
    if (!state_ || project_ == nullptr || !project_->isLivery || state_->project() != project_) return;
    const auto found = std::find_if(paintControls_.cbegin(), paintControls_.cend(), [hash](const PaintControls &controls) {
        return controls.slot.hash == hash;
    });
    if (found == paintControls_.cend()) return;
    const QColor primary = slotColor(found->slot, false);
    const QColor secondary = slotColor(found->slot, true);
    const auto previous = slotPaint(found->slot);
    const bool positional = !project_->liveryPaint.game.isEmpty() && project_->liveryPaint.game != QStringLiteral("FH6");
    const bool manufacturer = previous.manufacturerSelector != 0xffffffffu && (positional || !previous.primary.enabled);
    state_->beginProjectEdit();
    auto &material = project_->liveryPaint.ensure(hash);
    material.finish = finish;
    if (finish != 0) {
        material.manufacturerSelector = 0xffffffffu;
        if (!material.primary.enabled || manufacturer) material.primary = {true, opaqueBgra(primary)};
        const int index = found->material->findData(finish);
        if (found->material->itemData(index, kFinishColorCountRole).toInt() > 1 && (!material.secondary.enabled || manufacturer))
            material.secondary = {true, opaqueBgra(secondary)};
    }
    state_->commitProjectEdit();
    state_->noteProjectPaintChanged();
}

void CarPreviewWidget::clearPaintOverrides() {
    if (!state_ || project_ == nullptr || !project_->isLivery
        || state_->project() != project_ || project_->liveryPaint.materials.isEmpty()) return;
    if (paintPopup_) paintPopup_->close();
    state_->beginProjectEdit();
    project_->liveryPaint.materials.clear();
    state_->commitProjectEdit();
    state_->noteProjectPaintChanged();
}

void CarPreviewWidget::rebuildColorsPanel() {
    if (colorsLayout_ == nullptr || colorsPage_ == nullptr) return;
    paintControls_.clear();
    clearPaintButton_ = nullptr;
    while (QLayoutItem *item = colorsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto *header = new QWidget(colorsPage_);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->addWidget(new QLabel(QStringLiteral("Paint slots"), header));
    headerLayout->addStretch();
    clearPaintButton_ = new QPushButton(QStringLiteral("Reset"), header);
    clearPaintButton_->setObjectName(QStringLiteral("previewClearPaint"));
    headerLayout->addWidget(clearPaintButton_);
    connect(clearPaintButton_, &QPushButton::clicked, this, &CarPreviewWidget::clearPaintOverrides);
    colorsLayout_->addWidget(header);
    if (project_ == nullptr || !project_->isLivery || !state_ || state_->project() != project_) {
        auto *empty = new QLabel(QStringLiteral("Open a livery project to edit paint slots."), colorsPage_);
        empty->setWordWrap(true);
        colorsLayout_->addWidget(empty);
        clearPaintButton_->setEnabled(false);
        colorsLayout_->addStretch();
        return;
    }
    paintFinishChoices_ = carPaintFinishes(project_->liveryPaint.game);
    const auto paintSlots = carPaintSlots(model_, project_->liveryPaint, selectedLodIndex_, selectedPartOptions_);
    if (paintSlots.isEmpty()) {
        auto *empty = new QLabel(hasModel() ? QStringLiteral("This model has no paintable surfaces.")
                                           : QStringLiteral("Load a car to see its paint slots."), colorsPage_);
        empty->setWordWrap(true);
        colorsLayout_->addWidget(empty);
    }
    for (const CarPaintSlot &slot : paintSlots) {
        PaintControls controls;
        controls.slot = slot;
        auto *row = new QWidget(colorsPage_);
        row->setObjectName(QStringLiteral("paintSlot.") + slot.name);
        auto *layout = new QVBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(new QLabel(slot.name, row));
        controls.material = new PaintMaterialComboBox(row);
        controls.material->setObjectName(QStringLiteral("paintMaterial.") + slot.name);
        controls.material->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        controls.material->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        controls.material->setMinimumContentsLength(10);
        controls.material->addItem(slot.window ? QStringLiteral("Tint") : QStringLiteral("Manufacturer / default"), 0u);
        controls.material->setItemData(0, 1, kFinishColorCountRole);
        if (!slot.window) {
            for (const CarPaintFinish &finish : paintFinishChoices_) {
                if (slot.wheel && !finish.wheelsAllowed) continue;
                controls.material->addItem(finish.name, finish.code);
                controls.material->setItemData(controls.material->count() - 1, finish.colorCount, kFinishColorCountRole);
                controls.material->setItemData(controls.material->count() - 1, finish.special, kFinishSpecialRole);
            }
        }
        layout->addWidget(controls.material);
        const auto addColorButton = [this, &slot, row, layout](bool secondary) {
            auto *button = new QPushButton(secondary ? QStringLiteral("Secondary") : QStringLiteral("Primary"), row);
            button->setObjectName((secondary ? QStringLiteral("paintSecondary.") : QStringLiteral("paintPrimary.")) + slot.name);
            button->setAccessibleName(slot.name + (secondary ? QStringLiteral(" secondary color") : QStringLiteral(" primary color")));
            button->setFixedHeight(29);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            layout->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, slot, secondary, button]() { chooseSlotColor(slot, secondary, button); });
            return button;
        };
        controls.primary = addColorButton(false);
        controls.secondary = addColorButton(true);
        connect(controls.material, qOverload<int>(&QComboBox::activated), this, [this, combo = controls.material, hash = slot.hash](int index) {
            chooseSlotFinish(hash, combo->itemData(index).toUInt());
        });
        colorsLayout_->addWidget(row);
        paintControls_.push_back(std::move(controls));
    }
    colorsLayout_->addStretch();
    syncPaintControls();
}

void CarPreviewWidget::syncPaintControls() {
    if (project_ == nullptr || !project_->isLivery || !state_ || state_->project() != project_) return;
    if (clearPaintButton_ != nullptr) clearPaintButton_->setEnabled(!project_->liveryPaint.materials.isEmpty());
    for (const PaintControls &controls : paintControls_) {
        const auto material = slotPaint(controls.slot);
        const quint32 finish = controls.slot.window ? 0 : material.finish != 0 ? material.finish
            : material.manufacturerSelector != 0xffffffffu ? 0 : 1;
        int index = controls.material->findData(finish);
        if (index < 0) {
            controls.material->addItem(QStringLiteral("Material %1").arg(finish), finish);
            index = controls.material->count() - 1;
            controls.material->setItemData(index, material.secondary.enabled ? 2 : 1, kFinishColorCountRole);
            controls.material->setItemData(index, true, kFinishSpecialRole);
        }
        const QSignalBlocker blocker(controls.material);
        controls.material->setCurrentIndex(index);
        const int colorCount = controls.material->itemData(index, kFinishColorCountRole).toInt();
        const bool editableColors = !controls.material->itemData(index, kFinishSpecialRole).toBool();
        const QColor primary = slotColor(controls.slot, false);
        const QColor secondary = colorCount > 1 ? slotColor(controls.slot, true) : primary;
        stylePaintColorButton(controls.primary, primary);
        stylePaintColorButton(controls.secondary, secondary);
        controls.primary->setVisible(editableColors && colorCount > 0);
        controls.primary->setToolTip(QStringLiteral("Primary color / %1").arg(primary.name(QColor::HexRgb).toUpper()));
        controls.secondary->setToolTip(QStringLiteral("Secondary color / %1").arg(secondary.name(QColor::HexRgb).toUpper()));
        controls.secondary->setVisible(editableColors && colorCount > 1);
    }
}

void CarPreviewWidget::updateCarColorControl() {
    const auto color = project_ != nullptr && project_->isLivery ? project_->liveryPaint.defaultCarColorBgra() : std::nullopt;
    basePaint_ = color ? colorFromBgra(*color) : defaultPreviewCarColor();
    basePaint_.setAlpha(255);
    if (paintControls_.isEmpty()) rebuildColorsPanel();
    else syncPaintControls();
}

int CarPreviewWidget::liveryTextureScale() const {
    return liveryTextureScale_;
}

void CarPreviewWidget::setLiveryTextureScale(int scale) {
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

void CarPreviewWidget::markLiveryDirty() {
    liveLiveryFullDirty_ = true;
    invalidateCachedLivery();
    update();
}

void CarPreviewWidget::markLiveryDirtyImmediate() {
    if (!hasModel()) {
        return;
    }
    markLiveryDirty();
}

void CarPreviewWidget::onProjectGeometryChanged(bool, const QVector<QString> &changedNodeIds) {
    if (changedNodeIds.isEmpty()) {
        markLiveryDirty();
        return;
    }
    markLiverySectionsDirty(changedNodeIds);
}

void CarPreviewWidget::markLiverySectionsDirty(const QVector<QString> &nodeIds) {
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

void CarPreviewWidget::initializeGL() {
    graphicsInitializationAttempted_ = true;
    context()->functions()->glEnable(GL_MULTISAMPLE);
    geometryLoaded_ = geometry_.loadDefault();
    shapeRenderer_.initialize();
    if (geometryLoaded_ && shapeRenderer_.isInitialized()) {
        shapeRenderer_.uploadGeometry(geometry_);
    }
    carRenderer_.initialize();
    graphicsApiSuitable_ = carRenderer_.isInitialized();
    liveryTexture_ = 0;
    liveLiveryFullDirty_ = true;
    liveryDirty_ = true;
    updateModelStatus();
}

void CarPreviewWidget::resizeGL(int width, int) {
    (void)width;
    layoutOverlayControls();
}

void CarPreviewWidget::paintGL() {
    QOpenGLContext *ctx = context();
    if (ctx == nullptr) {
        return;
    }
    QOpenGLFunctions *functions = ctx->functions();
    const bool traceLoad = modelUploadPending_ && carLoadTimings_.contains(QStringLiteral("cpu_ready_ms"))
        && !carLoadTimings_.contains(QStringLiteral("first_frame_ms"));
    QElapsedTimer frameTimer;
    qint64 previousTime = 0;
    if (traceLoad) {
        frameTimer.start();
    }
    const auto checkpoint = [&](const QString &stage) {
        if (traceLoad) {
            const qint64 time = frameTimer.nsecsElapsed();
            carLoadTimings_.insert(stage, (time - previousTime) / 1000000.0);
            previousTime = time;
        }
    };

    if (modelUploadPending_ && carRenderer_.isInitialized()) {
        carRenderer_.uploadModel(model_, selectedLodIndex_);
        modelUploadPending_ = false;
        if (modelFitPending_) {
            fitCameraToModel();
            modelFitPending_ = false;
        }
        updateModelStatus();
    }
    checkpoint(QStringLiteral("gpu_model_upload_ms"));
    if (liveryMasksPending_ && carRenderer_.isInitialized()) {
        carRenderer_.setLivery(model_, liveryMasks_);
        liveryMasksPending_ = false;
    }
    checkpoint(QStringLiteral("gpu_masks_ms"));

    GLuint liveryTexture = 0;
    if (project_ != nullptr && geometryLoaded_ && shapeRenderer_.isInitialized()) {
        if (liveryDirty_ || liveryTexture_ == 0) {
            const QSize texSize = liveryTextureSize();
            const bool projectImportedLivery = project_->isLivery && liveryMasks_.valid();
            const PackedLiveryLayout paintLayout = projectImportedLivery
                ? packedLiveryLayout(liveryMasks_, texSize)
                : PackedLiveryLayout{};
            const QSize paintTextureSize = paintLayout.valid ? paintLayout.textureSize : texSize;
            carRenderer_.setPaintTextureRegions(
                paintLayout.valid ? paintLayout.uvRegions : QVector<QVector4D>{});
            const bool fullRebuild = liveLiveryFullDirty_ || liveryTexture_ == 0
                || projectedSectionCache_.isEmpty() || dirtySectionIds_.isEmpty();
            QVector<ProjectedLiverySection> projectedSections;
            QVector<int> dirtyIndices;
            if (projectImportedLivery && project_->root) {
                QSet<QString> liveSectionIds;
                for (const auto &rootChild : project_->root->children) {
                    if (rootChild->kind() != fls::scene::LayerKind::Group) {
                        continue;
                    }
                    const auto *section = static_cast<const fls::scene::Group *>(rootChild.get());
                    if (!section->isLiverySection) {
                        continue;
                    }
                    liveSectionIds.insert(section->id);
                    const bool sectionDirty = fullRebuild || dirtySectionIds_.contains(section->id)
                        || !projectedSectionCache_.contains(section->id);
                    if (sectionDirty) {
                        if (const std::optional<ProjectedLiverySection> projected = buildProjectedLiverySection(
                                *project_, *section, liveryMasks_, texSize, paintLayout)) {
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
                                        QVector<fls::Project> &projects, QVector<QRect> &clips) {
                    projects.reserve(sections.size());
                    clips.reserve(sections.size());
                    for (const ProjectedLiverySection &section : sections) {
                        projects.push_back(section.project);
                        clips.push_back(section.clipRect);
                    }
                };
                GLuint tex = 0;
                if (!fullRebuild && liveryTexture_ != 0 && !dirtyIndices.isEmpty()) {
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
                    QVector<ProjectedLiverySection> cluster;
                    for (int i = 0; i < n; ++i) {
                        if (projectedSections[i].clipRect.isEmpty()) {
                            continue;
                        }
                        ++nonEmptyCount;
                        if (inCluster[i]) {
                            cluster.push_back(projectedSections[i]);
                        }
                    }
                    if (!cluster.isEmpty() && cluster.size() < nonEmptyCount) {
                        QVector<fls::Project> clusterProjects;
                        QVector<QRect> clusterClips;
                        collect(cluster, clusterProjects, clusterClips);
                        tex = shapeRenderer_.renderScenesToTexture(
                            clusterProjects, clusterClips, geometry_, liveryWorldToScreen(paintTextureSize),
                            paintTextureSize, /*preserveExisting=*/true);
                    }
                }
                if (tex == 0) {
                    QVector<fls::Project> sectionProjects;
                    QVector<QRect> clipRects;
                    collect(projectedSections, sectionProjects, clipRects);
                    tex = shapeRenderer_.renderScenesToTexture(
                        sectionProjects, clipRects, geometry_, liveryWorldToScreen(paintTextureSize), paintTextureSize);
                }
                liveryTexture_ = tex;
            } else {
                liveryTexture_ = shapeRenderer_.renderSceneToTexture(
                    *project_, geometry_, liveryWorldToScreen(paintTextureSize), paintTextureSize);
            }
            liveryDirty_ = false;
            liveLiveryFullDirty_ = false;
            dirtySectionIds_.clear();
        }
        liveryTexture = liveryTexture_;
    }
    checkpoint(QStringLiteral("livery_texture_ms"));
    const qreal dpr = devicePixelRatioF();
    const int pw = std::max(
        1, static_cast<int>(std::lround(previewViewportWidth() * dpr)));
    const int ph = std::max(1, static_cast<int>(std::lround(height() * dpr)));
    functions->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    functions->glViewport(0, 0, pw, ph);
    if (transparentBackground_) {
        functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        functions->glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
    }
    functions->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!carRenderer_.hasModel()) {
        return;
    }
    carRenderer_.render(
        cameraView(), cameraProjection(), liveryTexture, basePaint_,
        project_ != nullptr ? &project_->liveryPaint : nullptr,
        manufacturerColors_.colors.isEmpty() ? nullptr : &manufacturerColors_,
        paintFinishes_.loaded() ? &paintFinishes_ : nullptr);
    checkpoint(QStringLiteral("render_submit_ms"));
}

void CarPreviewWidget::paintEvent(QPaintEvent *event) {
    QOpenGLWidget::paintEvent(event);
    if (!graphicsInitializationAttempted_) {
        graphicsInitializationAttempted_ = true;
        graphicsApiSuitable_ = false;
        updateModelStatus();
    }
}

void CarPreviewWidget::setGameFolder(const QString &folder) {
    if (folder == gameFolder_) {
        return;
    }
    gameFolder_ = folder;
    const quint64 generation = ++paintFinishLoadGeneration_;
    if (folder.isEmpty()) {
        paintFinishes_.clear();
        update();
        return;
    }
    QPointer<CarPreviewWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, folder, generation]() {
        auto library = std::make_shared<fls::PaintFinishLibrary>();
        library->load(folder);
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard,
            [guard, library = std::move(library), generation]() mutable {
                if (!guard || guard->paintFinishLoadGeneration_ != generation) {
                    return;
                }
                guard->paintFinishes_.replace(std::move(*library));
                guard->update();
            },
            Qt::QueuedConnection);
    });
}

void CarPreviewWidget::setLoadCarTextures(bool enabled) {
    if (loadCarTextures_ == enabled) {
        return;
    }
    loadCarTextures_ = enabled;
    if (loadedCarPath_.isEmpty()) {
        return;
    }
    const QString path = loadedCarPath_;
    const QString game = loadedCarGame_;
    loadCarAsync(path, [](bool loaded, const QString &error) {
        if (!loaded) {
            qWarning().noquote() << error;
        }
    }, game);
}

QMatrix4x4 CarPreviewWidget::cameraView() const {
    const float cp = std::cos(pitch_);
    const float sp = std::sin(pitch_);
    const float cy = std::cos(yaw_);
    const float sy = std::sin(yaw_);
    const QVector3D eye = target_ + distance_ * QVector3D(cp * sy, sp, cp * cy);
    QMatrix4x4 view;
    view.lookAt(eye, target_, QVector3D(0.0f, 1.0f, 0.0f));
    return view;
}

QMatrix4x4 CarPreviewWidget::cameraProjection() const {
    const float aspect = height() > 0
        ? static_cast<float>(previewViewportWidth()) / static_cast<float>(height())
        : 1.0f;
    const float nearPlane = std::max(0.01f, modelRadius_ * 0.02f);
    const float farPlane = modelRadius_ * 8.0f + distance_ * 2.0f;
    QMatrix4x4 projection;
    projection.perspective(45.0f, aspect, nearPlane, farPlane);
    return projection;
}

QSize CarPreviewWidget::liveryTextureSize() const {
    return QSize(kLiveryBaseTexWidth * liveryTextureScale_, kLiveryBaseTexHeight * liveryTextureScale_);
}

QTransform CarPreviewWidget::liveryWorldToScreen(const QSize &textureSize) const {
    QTransform transform;
    transform.translate(textureSize.width() * 0.5, textureSize.height() * 0.5);
    transform.scale(liveryTextureScale_, liveryTextureScale_);
    return transform;
}

void CarPreviewWidget::fitCameraToModel() {
    const fls::ModelVec3 &mn = model_.boundsMin;
    const fls::ModelVec3 &mx = model_.boundsMax;
    target_ = QVector3D(-(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
    const QVector3D extent(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
    modelRadius_ = std::max(0.001f, 0.5f * extent.length());
    distance_ = modelRadius_ * 2.6f;
    yaw_ = 0.6f;
    pitch_ = 0.25f;
}

void CarPreviewWidget::invalidateCachedLivery() {
    dirtySectionIds_.clear();
    projectedSectionCache_.clear();
    liveryDirty_ = true;
}

void CarPreviewWidget::layoutOverlayControls() {
    if (sidePanel_ == nullptr || sidePanelToggle_ == nullptr) {
        return;
    }
    const int preferredWidth = std::min(292, std::max(220, width() * 2 / 5));
    const int panelWidth = std::min(preferredWidth, std::max(0, width() - 38));
    sidePanel_->setGeometry(width() - panelWidth, 0, panelWidth, height());
    sidePanel_->setVisible(sidePanelVisible_);

    if (modelStatus_ != nullptr) {
        modelStatus_->adjustSize();
        const int viewportWidth = previewViewportWidth();
        modelStatus_->move(
            std::max(0, (viewportWidth - modelStatus_->width()) / 2),
            std::max(0, (height() - modelStatus_->height()) / 2));
        modelStatus_->raise();
    }

    const int toggleX = sidePanelVisible_
        ? std::max(3, width() - panelWidth + 5)
        : std::max(3, width() - sidePanelToggle_->width() - 5);
    sidePanelToggle_->move(toggleX, 5);
    sidePanel_->raise();
    sidePanelToggle_->raise();
}

void CarPreviewWidget::updateModelStatus() {
    if (modelStatus_ == nullptr) {
        return;
    }

    QString status;
    if (graphicsInitializationAttempted_ && !graphicsApiSuitable_) {
        status = QStringLiteral("No suitable graphics API");
    } else if (!carRenderer_.hasModel()) {
        status = modelLoadPending_ || modelUploadPending_
            ? QStringLiteral("Loading model")
            : QStringLiteral("No model found");
    }

    modelStatus_->setText(status);
    modelStatus_->setVisible(!status.isEmpty());
    layoutOverlayControls();
}

void CarPreviewWidget::setSidePanelVisible(bool visible) {
    sidePanelVisible_ = visible;
    if (sidePanel_ != nullptr) {
        sidePanel_->setVisible(visible);
    }
    if (sidePanelToggle_ != nullptr) {
        sidePanelToggle_->setText(visible ? QStringLiteral(">")
                                          : QStringLiteral("<"));
        sidePanelToggle_->setToolTip(
            visible ? QStringLiteral("Hide paint menu")
                    : QStringLiteral("Show paint menu"));
    }
    layoutOverlayControls();
    update();
}

int CarPreviewWidget::previewViewportWidth() const {
    if (transparentBackground_ || !sidePanelVisible_ || sidePanel_ == nullptr) {
        return width();
    }
    return std::max(1, width() - sidePanel_->width());
}

void CarPreviewWidget::resetPartSelections() {
    selectedPartOptions_.clear();
    for (int partType : kSelectablePartTypes) {
        const fls::CarPartOption *selected = nullptr;
        for (const fls::CarPartOption &option : model_.partOptions) {
            if (option.partType != partType || !option.stock) continue;
            if (selected == nullptr || option.level < selected->level
                || (option.level == selected->level && option.id < selected->id)) {
                selected = &option;
            }
        }
        if (selected != nullptr) selectedPartOptions_.insert(partType, selected->id);
    }
    carRenderer_.setPartSelections(selectedPartOptions_);
}

void CarPreviewWidget::switchCarUnwrapOverlay() {
    if (selectedLodIndex_ < 0
        || selectedLodIndex_ >= static_cast<int>(carUnwrapOverlaySets_.size())) {
        carUnwrapOverlay_ = {};
        return;
    }
    carUnwrapOverlay_ = carUnwrapOverlaySets_[
        static_cast<size_t>(selectedLodIndex_)].selected(selectedPartOptions_);
}

void CarPreviewWidget::mousePressEvent(QMouseEvent *event) {
    lastMousePos_ = event->pos();
}

void CarPreviewWidget::mouseMoveEvent(QMouseEvent *event) {
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

void CarPreviewWidget::wheelEvent(QWheelEvent *event) {
    const double steps = event->angleDelta().y() / 120.0;
    distance_ = std::clamp(distance_ * static_cast<float>(std::pow(0.88, steps)),
                           modelRadius_ * 0.1f, modelRadius_ * 40.0f);
    update();
}

void CarPreviewWidget::cycleDebugMode() {
    carRenderer_.setDebugMode((carRenderer_.debugMode() + 1) % 3);
    update();
}

} // namespace gui
