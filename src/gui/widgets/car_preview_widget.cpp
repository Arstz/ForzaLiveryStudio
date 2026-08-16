#include "car_preview_widget.h"

#include "car_scene.h"
#include "editor_state.h"
#include "gui_assets.h"
#include "material_hashes.h"
#include "matrix_math.h"
#include "model_material.h"
#include "scene_view.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
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
constexpr int kLiverySectionMaskSlots[fls::kLiverySideCount] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
};

class NoWheelComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void wheelEvent(QWheelEvent *event) override {
        event->ignore();
    }
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

// Stock tyre spec per car, from assets/cars/wheel_sizes.json. The wheel/tire models are
// normalised, so this is what sizes them per car (see docs/GAMEDATA.md). Lazily loaded once.
fls::WheelSizing wheelSizingForModelCode(const QString &modelCode) {
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
                sizes.insert(it.key().toLower(), parse(it.value().toObject(), fallback));
            }
            break;
        }
        return std::make_pair(sizes, fallback);
    }();
    return table.first.value(modelCode.toLower(), table.second);
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

QIcon carColorSwatchIcon(const QColor &color) {
    QPixmap icon(18, 18);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(255, 255, 255, 150), 1.0));
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(1.5, 1.5, 15.0, 15.0), 3.0, 3.0);
    return QIcon(icon);
}

struct PaintRegion {
    QString name;
    QVector<quint64> materialHashes;
    QVector<quint64> fallbackHashes;
};

void appendUniqueHashes(QVector<quint64> &target, const QVector<quint64> &source) {
    for (quint64 hash : source) {
        if (!target.contains(hash)) {
            target.push_back(hash);
        }
    }
}

template <size_t Size>
QVector<quint64> paintHashVector(const std::array<quint64, Size> &source) {
    QVector<quint64> hashes;
    hashes.reserve(static_cast<qsizetype>(Size));
    for (quint64 hash : source) {
        hashes.push_back(hash);
    }
    return hashes;
}

QString paintMaterialToken(QString value) {
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    value = value.mid(value.lastIndexOf(QLatin1Char('/')) + 1).toLower();
    const int pipe = value.indexOf(QLatin1Char('|'));
    if (pipe >= 0) {
        value.truncate(pipe);
    }
    if (value.endsWith(QStringLiteral(".materialbin"))) {
        value.chop(12);
    }

    return value;
}

bool isWheelPaintMesh(const fls::CarMesh &mesh) {
    QString path = mesh.sourceModelPath;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!path.contains(QStringLiteral("/wheels/"), Qt::CaseInsensitive)) {
        return false;
    }
    const QString material = paintMaterialToken(mesh.materialName);
    return material == QStringLiteral("rim")
        || material == QStringLiteral("rim2")
        || material == QStringLiteral("rim3")
        || material == QStringLiteral("inner_rim")
        || material == QStringLiteral("lip")
        || material == QStringLiteral("hub")
        || material == QStringLiteral("lug")
        || material == QStringLiteral("detail")
        || material == QStringLiteral("detail2");
}

QVector<quint64> modelSharedWheelPaintHashes(
    const fls::CarModel &model, int lodIndex) {
    using namespace fls::material_hashes::binding;
    QVector<quint64> hashes;
    QSet<quint64> seen;
    const auto appendMeshes = [&](const std::vector<fls::CarMesh> &meshes) {
        for (const fls::CarMesh &mesh : meshes) {
            if (!isWheelPaintMesh(mesh) || mesh.paintMaterialHash == 0) {
                continue;
            }
            if (fls::material_hashes::contains(kFrontWheelPaint, mesh.paintMaterialHash)
                || fls::material_hashes::contains(kRearWheelPaint, mesh.paintMaterialHash)) {
                continue;
            }
            if (!seen.contains(mesh.paintMaterialHash)) {
                seen.insert(mesh.paintMaterialHash);
                hashes.push_back(mesh.paintMaterialHash);
            }
        }
    };
    appendMeshes(model.meshesForLod(lodIndex));
    appendMeshes(model.variantMeshesForLod(lodIndex));
    for (quint64 materialHash : kWheelPaintGroups) {
        if (!seen.contains(materialHash)) {
            seen.insert(materialHash);
            hashes.push_back(materialHash);
        }
    }

    return hashes;
}

QVector<PaintRegion> paintRegions(const fls::CarModel &model, int lodIndex) {
    using namespace fls::material_hashes::binding;
    const QVector<quint64> sharedRims = modelSharedWheelPaintHashes(model, lodIndex);
    const QVector<quint64> frontRims = paintHashVector(kFrontWheelPaint);
    const QVector<quint64> rearRims = paintHashVector(kRearWheelPaint);
    QVector<quint64> allRims = sharedRims;
    appendUniqueHashes(allRims, frontRims);
    appendUniqueHashes(allRims, rearRims);

    return {
        {QStringLiteral("Body"), {kBodyPaint}},
        {QStringLiteral("Hood"), {kHoodPaint}},
        {QStringLiteral("Mirrors"), {kMirrorPaint}},
        {QStringLiteral("Wing"), {kSpoilerPaint}},
        {QStringLiteral("Brakes"), {kBrakeCaliper}},
        {QStringLiteral("Rims"), allRims},
        {QStringLiteral("Front Rims"), frontRims, sharedRims},
        {QStringLiteral("Rear Rims"), rearRims, sharedRims},
        {QStringLiteral("Windows"), {kWindowGlass}},
    };
}

QString partTypeDisplayName(int partType) {
    switch (partType) {
    case fls::car_part_types::CarBody: return QStringLiteral("Body Kit");
    case fls::car_part_types::RearWing: return QStringLiteral("Rear Wing / Spoiler");
    case fls::car_part_types::FrontBumper: return QStringLiteral("Front Bumper");
    case fls::car_part_types::RearBumper: return QStringLiteral("Rear Bumper");
    case fls::car_part_types::Hood: return QStringLiteral("Hood");
    case fls::car_part_types::SideSkirts: return QStringLiteral("Side Skirts");
    default: return QStringLiteral("Part");
    }
}

QString optionModelKey(const fls::CarPartOption &option) {
    QStringList paths;
    paths.reserve(option.modelPaths.size());
    for (QString path : option.modelPaths) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        paths.push_back(path.toLower());
    }
    paths.removeDuplicates();
    paths.sort();
    return paths.join(QLatin1Char('|'));
}

const fls::CarPartOption *linkedCarBodyPartOption(
    const fls::CarModel &model, int partType, int carBodyOptionId, int linkedCarBodyId) {
    const fls::CarPartOption *selected = nullptr;

    for (const fls::CarPartOption &option : model.partOptions) {
        const bool matches = option.partType == partType
            && (linkedCarBodyId >= 0
                ? option.carBodyId == linkedCarBodyId
                : option.id == carBodyOptionId);
        if (!matches) {
            continue;
        }
        if (selected == nullptr
            || (option.stock && !selected->stock)
            || (option.stock == selected->stock
                && (option.level < selected->level
                    || (option.level == selected->level && option.id < selected->id)))) {
            selected = &option;
        }
    }

    return selected;
}

QString catalogPartName(const QString &mediaName, const fls::CarPartOption &option) {
    static const auto catalog = [] {
        QHash<QString, QHash<QString, QString>> names;
        QFile file(assetPath(QStringLiteral("cars/part_names.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            return names;
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        for (auto model = root.constBegin(); model != root.constEnd(); ++model) {
            if (!model.value().isObject()) {
                continue;
            }
            QHash<QString, QString> modelNames;
            const QJsonObject entries = model.value().toObject();
            for (auto entry = entries.constBegin(); entry != entries.constEnd(); ++entry) {
                if (entry.value().isString()) {
                    modelNames.insert(entry.key(), entry.value().toString());
                }
            }
            names.insert(model.key().toLower(), modelNames);
        }
        return names;
    }();

    const auto model = catalog.constFind(mediaName.toLower());
    if (model == catalog.constEnd()) {
        return {};
    }
    return model->value(QStringLiteral("%1:%2").arg(option.partType).arg(option.id));
}

QString partOptionDisplayName(const QString &mediaName, const fls::CarPartOption &option,
                              int alternativeIndex, bool defaultOption) {
    if (defaultOption) {
        if (option.partType == fls::car_part_types::RearWing
            && option.modelPaths.isEmpty()) {
            return QStringLiteral("None (Stock)");
        }
    }
    const QString catalogName = catalogPartName(mediaName, option);
    if (!catalogName.isEmpty()) {
        return catalogName;
    }
    if (defaultOption) {
        return QStringLiteral("Stock");
    }
    if (option.partType == fls::car_part_types::CarBody) {
        return alternativeIndex == 0
            ? QStringLiteral("Body Kit")
            : QStringLiteral("Body Kit %1").arg(alternativeIndex + 1);
    }

    QString identity = optionModelKey(option);
    if (identity.contains(QStringLiteral("_race"))) {
        return QStringLiteral("Race");
    }
    if (identity.contains(QStringLiteral("_wide"))) {
        return QStringLiteral("Wide");
    }
    if (identity.contains(QStringLiteral("_b.modelbin"))
        || identity.contains(QStringLiteral("_b_"))) {
        return QStringLiteral("Option B");
    }
    return QStringLiteral("Option %1").arg(alternativeIndex + 1);
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
    const QHash<QString, QByteArray> missingMaterialData =
        fls::readZipEntries(archivePath, missingMaterialEntries);
    for (const QString &entry : missingMaterialEntries) {
        std::shared_ptr<fls::ModelMaterial> decoded;
        const QByteArray bytes = missingMaterialData.value(entry.toLower());
        if (!bytes.isEmpty()) {
            try {
                decoded = fls::decodeMaterialBundle(bytes);
            } catch (const std::exception &) {
            }
        }
        defaults.insert(materialArchiveKey + QLatin1Char('|') + entry.toLower(), decoded);
    }
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

    const QHash<QString, QByteArray> materialData =
        fls::readZipEntries(archivePath, missingEntries);
    for (const QString &entry : missingEntries) {
        std::shared_ptr<fls::ModelMaterial> material;
        const QByteArray bytes = materialData.value(entry.toLower());
        if (!bytes.isEmpty()) {
            try {
                material = fls::decodeMaterialBundle(bytes);
            } catch (const std::exception &) {
            }
        }
        defaults.insert(archiveKey + QLatin1Char('|') + entry.toLower(), material);
    }
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
    std::unique_ptr<QTemporaryDir> extractedCarDir;
    QString loadedCarPath;
    QString liveryMasksDir;
};

std::shared_ptr<PreparedCar> prepareCar(
    const QString &path, bool loadCarTextures, QString *error) {
    static QMutex loadMutex;
    QMutexLocker lock(&loadMutex);
    QString loadPath = path;
    std::unique_ptr<QTemporaryDir> extracted;
    if (path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        extracted = std::make_unique<QTemporaryDir>();
        if (!extracted->isValid()) {
            if (error != nullptr) {
                *error = QStringLiteral("cannot create temporary directory for %1").arg(path);
            }
            return {};
        }
        if (!fls::extractZipArchive(path, extracted->path(), error)) {
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

    const fls::WheelSizing wheelSizing =
        wheelSizingForModelCode(QFileInfo(loadPath).completeBaseName());
    fls::CarModel model = loadPath.endsWith(QStringLiteral(".carbin"), Qt::CaseInsensitive)
        ? fls::loadCarBin(loadPath, error, wheelSizing)
        : fls::loadModelBin(loadPath, error);
    if (model.meshes.empty()) {
        return {};
    }
    appendSharedTireB(model, path, wheelSizing);
    assignSharedSlotMaterials(model);
    resolveExteriorMaterials(model, path, QFileInfo(loadPath).absolutePath(), loadCarTextures);

    const QDir carDir = QFileInfo(loadPath).absoluteDir();
    auto prepared = std::make_shared<PreparedCar>();
    prepared->model = std::move(model);
    prepared->manufacturerColors = loadManufacturerColors(carDir.absolutePath(), path);
    prepared->extractedCarDir = std::move(extracted);
    prepared->loadedCarPath = path;

    const QString masksDir = carDir.filePath(QStringLiteral("LiveryMasks"));
    if (QFileInfo::exists(masksDir)) {
        prepared->liveryMasks = fls::loadLiveryMasks(masksDir);
        prepared->liveryMasksDir = masksDir;
        QSet<int> selectablePartTypes;
        for (int partType : kSelectablePartTypes) {
            selectablePartTypes.insert(partType);
        }
        prepared->carUnwrapOverlaySets.reserve(
            static_cast<size_t>(prepared->model.lodCount()));
        for (int lodIndex = 0;
             lodIndex < prepared->model.lodCount();
             ++lodIndex) {
            prepared->carUnwrapOverlaySets.push_back(
                buildCarUnwrapOverlaySet(
                    prepared->model, prepared->liveryMasks,
                    selectablePartTypes, lodIndex));
        }
    }

    return prepared;
}

} // namespace

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

    sidePanel_ = new QFrame(this);
    sidePanel_->setObjectName(QStringLiteral("previewSidePanel"));
    sidePanel_->setAttribute(Qt::WA_StyledBackground, true);
    sidePanel_->setStyleSheet(QStringLiteral(
        "QFrame#previewSidePanel {"
        " background: rgba(24, 25, 29, 246);"
        " border-left: 1px solid rgba(255, 255, 255, 42);"
        "}"
        "QWidget#previewCategoryBar {"
        " background: rgba(14, 15, 18, 150);"
        " border-bottom: 1px solid rgba(255, 255, 255, 30);"
        "}"
        "QToolButton#previewCategoryButton {"
        " color: rgba(225, 226, 230, 185);"
        " background: transparent;"
        " border: 0;"
        " border-bottom: 2px solid transparent;"
        " padding: 10px 3px 8px 3px;"
        " font-size: 10px;"
        " font-weight: 600;"
        "}"
        "QToolButton#previewCategoryButton:hover {"
        " color: rgb(245, 246, 248);"
        " background: rgba(255, 255, 255, 12);"
        "}"
        "QToolButton#previewCategoryButton:checked {"
        " color: rgb(245, 246, 248);"
        " border-bottom-color: rgb(88, 157, 235);"
        "}"
        "QLabel { color: rgb(235, 236, 240); }"
        "QLabel[secondary=\"true\"] { color: rgba(220, 222, 228, 155); }"
        "QCheckBox { color: rgb(225, 226, 230); padding: 3px 0; }"
        "QRadioButton { color: rgb(225, 226, 230); padding: 3px 0; }"
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

    auto *categoryBar = new QWidget(sidePanel_);
    categoryBar->setObjectName(QStringLiteral("previewCategoryBar"));
    auto *categoryLayout = new QHBoxLayout(categoryBar);
    categoryLayout->setContentsMargins(38, 0, 5, 0);
    categoryLayout->setSpacing(0);
    categoryButtons_ = new QButtonGroup(sidePanel_);
    categoryButtons_->setExclusive(true);
    const auto addCategory = [this, categoryLayout](const QString &text, int page) {
        auto *button = new QToolButton(sidePanel_);
        button->setObjectName(QStringLiteral("previewCategoryButton"));
        button->setText(text);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        categoryButtons_->addButton(button, page);
        categoryLayout->addWidget(button, 1);
        return button;
    };
    QToolButton *paintCategory = addCategory(QStringLiteral("PAINT"), 0);
    addCategory(QStringLiteral("PARTS"), 1);
    addCategory(QStringLiteral("LOD"), 2);
    paintCategory->setChecked(true);
    panelLayout->addWidget(categoryBar);

    categoryPages_ = new QStackedWidget(sidePanel_);
    const auto addScrollPage = [this](QWidget **page, QVBoxLayout **contentLayout) {
        *page = new QWidget(categoryPages_);
        auto *pageLayout = new QVBoxLayout(*page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        auto *scroll = new QScrollArea(*page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget(scroll);
        content->setAttribute(Qt::WA_StyledBackground, true);
        *contentLayout = new QVBoxLayout(content);
        (*contentLayout)->setContentsMargins(13, 13, 11, 13);
        (*contentLayout)->setSpacing(6);
        scroll->setWidget(content);
        pageLayout->addWidget(scroll);
        categoryPages_->addWidget(*page);
    };
    addScrollPage(&colorsPage_, &colorsLayout_);
    addScrollPage(&partsPage_, &partsOptionsLayout_);
    addScrollPage(&modelPage_, &modelOptionsLayout_);
    panelLayout->addWidget(categoryPages_, 1);

    connect(categoryButtons_, &QButtonGroup::idClicked,
            categoryPages_, &QStackedWidget::setCurrentIndex);

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

    rebuildPartsPanel();
    updateCarColorControl();
    updateLodControl();
    setSidePanelVisible(false);
}

CarPreviewWidget::~CarPreviewWidget() {
    makeCurrent();
    carRenderer_.release();
    shapeRenderer_.release();
    doneCurrent();
}

void CarPreviewWidget::loadCarAsync(const QString &path, CarLoadCallback callback) {
    const bool loadCarTextures = loadCarTextures_;
    const quint64 generation = ++carLoadGeneration_;
    QPointer<CarPreviewWidget> guard(this);
    QThreadPool::globalInstance()->start(
        [guard, path, loadCarTextures, generation, callback = std::move(callback)]() mutable {
            QString error;
            std::shared_ptr<PreparedCar> prepared =
                prepareCar(path, loadCarTextures, &error);
            if (!guard) {
                return;
            }
            QMetaObject::invokeMethod(
                guard,
                [guard, prepared = std::move(prepared), error = std::move(error),
                 generation, callback = std::move(callback)]() mutable {
                    if (!guard || guard->carLoadGeneration_ != generation) {
                        return;
                    }
                    if (!prepared) {
                        if (callback) {
                            callback(false, error);
                        }
                        return;
                    }

                    guard->model_ = std::move(prepared->model);
                    guard->manufacturerColors_ = std::move(prepared->manufacturerColors);
                    guard->extractedCarDir_ = std::move(prepared->extractedCarDir);
                    guard->loadedCarPath_ = std::move(prepared->loadedCarPath);
                    guard->liveryMasks_ = std::move(prepared->liveryMasks);
                    guard->carUnwrapOverlaySets_ = std::move(
                        prepared->carUnwrapOverlaySets);
                    guard->liveryMasksDir_ = std::move(prepared->liveryMasksDir);
                    guard->selectedLodIndex_ = 0;
                    guard->rebuildPartsPanel();
                    guard->switchCarUnwrapOverlay();
                    guard->updateCarColorControl();
                    guard->modelUploadPending_ = true;
                    guard->modelFitPending_ = true;
                    guard->liveryMasksPending_ = true;
                    guard->invalidateCachedLivery();
                    guard->updateLodControl();
                    guard->update();
                    if (callback) {
                        callback(true, {});
                    }
                },
                Qt::QueuedConnection);
        });
}

void CarPreviewWidget::cancelCarLoad() {
    ++carLoadGeneration_;
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
    manufacturerColors_ = {};
    selectedLodIndex_ = 0;
    selectedPartOptions_.clear();
    carRenderer_.setPartSelections(selectedPartOptions_);
    if (!hasModel() && !carRenderer_.hasModel()) {
        rebuildPartsPanel();
        updateCarColorControl();
        updateLodControl();
        return;
    }
    model_ = fls::CarModel{};
    rebuildPartsPanel();
    updateCarColorControl();
    updateLodControl();
    extractedCarDir_.reset();
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
    updateLodControl();
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
    project_ = project;
    modelUploadPending_ = hasModel();
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
            modelUploadPending_ = hasModel();
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

void CarPreviewWidget::chooseRegionColor(
    const QVector<quint64> &materialHashes, bool secondary,
    const QVector<quint64> &fallbackHashes) {
    if (state_ == nullptr || project_ == nullptr || !project_->isLivery
        || state_->project() != project_ || materialHashes.isEmpty()) {
        return;
    }
    const bool bodyRegion = materialHashes.contains(
        fls::material_hashes::binding::kBodyPaint);
    QColor initial = bodyRegion ? basePaint_ : defaultPreviewCarColor();
    QVector<quint64> displayHashes = materialHashes;
    appendUniqueHashes(displayHashes, fallbackHashes);
    for (quint64 materialHash : displayHashes) {
        const fls::LiveryPaintMaterial *material =
            project_->liveryPaint.find(materialHash);
        const fls::LiveryPaintColor *current = material != nullptr
            ? secondary ? &material->secondary : &material->primary
            : nullptr;
        if (current != nullptr && current->enabled) {
            initial = colorFromBgra(current->bgra);
            break;
        }
        if (secondary && material != nullptr && material->primary.enabled) {
            initial = colorFromBgra(material->primary.bgra);
        }
    }
    const QColor selected = QColorDialog::getColor(
        initial, this,
        secondary ? QStringLiteral("Secondary Paint Color")
                  : QStringLiteral("Primary Paint Color"));
    if (!selected.isValid()) {
        return;
    }

    const QVector<quint64> editHashes = effectivePaintHashes(materialHashes);
    state_->beginProjectEdit();
    if (bodyRegion && !secondary && !overrideBodyChildPaint_) {
        using namespace fls::material_hashes::binding;
        // Unpainted body-type meshes inherit the body's fallback color. Materialize their
        // current color before changing Body so an isolated body edit does not recolor them.
        for (quint64 childHash : {kHoodPaint, kMirrorPaint, kSpoilerPaint}) {
            const fls::LiveryPaintMaterial *child =
                project_->liveryPaint.find(childHash);
            if (child != nullptr && child->primary.enabled) {
                continue;
            }
            project_->liveryPaint.setColorBgra(
                childHash, false, opaqueBgra(basePaint_));
        }
    }
    for (quint64 materialHash : editHashes) {
        project_->liveryPaint.setColorBgra(
            materialHash, secondary, opaqueBgra(selected));
    }
    state_->commitProjectEdit();
    state_->noteProjectPaintChanged();
    modelUploadPending_ = true;
    update();
}

void CarPreviewWidget::chooseRegionFinish(
    const QVector<quint64> &materialHashes, quint32 finish) {
    if (state_ == nullptr || project_ == nullptr || !project_->isLivery
        || state_->project() != project_ || materialHashes.isEmpty()
        || (finish != 1 && finish != 2 && finish != 3 && finish != 4)) {
        return;
    }

    const QVector<quint64> editHashes = effectivePaintHashes(materialHashes);
    bool changed = false;
    for (quint64 materialHash : editHashes) {
        const fls::LiveryPaintMaterial *material =
            project_->liveryPaint.find(materialHash);
        if (material == nullptr || material->finish != finish) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return;
    }

    state_->beginProjectEdit();
    for (quint64 materialHash : editHashes) {
        project_->liveryPaint.ensure(materialHash).finish = finish;
    }
    state_->commitProjectEdit();
    state_->noteProjectPaintChanged();
    modelUploadPending_ = true;
    update();
}

QVector<quint64> CarPreviewWidget::effectivePaintHashes(
    const QVector<quint64> &materialHashes) const {
    QVector<quint64> result = materialHashes;
    using namespace fls::material_hashes::binding;
    if (!overrideBodyChildPaint_ || !result.contains(kBodyPaint)) {
        return result;
    }
    for (quint64 childHash : {kHoodPaint, kMirrorPaint, kSpoilerPaint}) {
        if (!result.contains(childHash)) {
            result.push_back(childHash);
        }
    }
    return result;
}

void CarPreviewWidget::clearPaintOverrides() {
    if (state_ == nullptr || project_ == nullptr || !project_->isLivery
        || state_->project() != project_ || project_->liveryPaint.materials.isEmpty()) {
        return;
    }
    state_->beginProjectEdit();
    project_->liveryPaint.materials.clear();
    state_->commitProjectEdit();
    state_->noteProjectPaintChanged();
    modelUploadPending_ = true;
    update();
}

void CarPreviewWidget::rebuildColorsPanel() {
    if (colorsLayout_ == nullptr || colorsPage_ == nullptr) {
        return;
    }
    while (QLayoutItem *item = colorsLayout_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            delete widget;
        }
        delete item;
    }

    const bool canEditPaint = state_ != nullptr && project_ != nullptr
        && project_->isLivery && state_->project() == project_;

    auto *header = new QWidget(colorsPage_);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("Paint"), header);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
    title->setFont(titleFont);
    headerLayout->addWidget(title);
    headerLayout->addStretch(1);
    auto *clearButton = new QPushButton(QStringLiteral("Clear all"), header);
    clearButton->setObjectName(QStringLiteral("previewClearPaint"));
    clearButton->setFocusPolicy(Qt::NoFocus);
    clearButton->setToolTip(QStringLiteral("Clear all paint overrides (undoable)"));
    clearButton->setEnabled(
        canEditPaint && !project_->liveryPaint.materials.isEmpty());
    headerLayout->addWidget(clearButton);
    colorsLayout_->addWidget(header);
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        QTimer::singleShot(0, this, [this]() { clearPaintOverrides(); });
    });

    auto *description = new QLabel(
        QStringLiteral("Choose each region's colors and material finish."),
        colorsPage_);
    description->setProperty("secondary", true);
    description->setWordWrap(true);
    colorsLayout_->addWidget(description);
    colorsLayout_->addSpacing(5);

    const bool editable = hasModel() && canEditPaint;
    if (!editable) {
        auto *empty = new QLabel(
            hasModel()
                ? QStringLiteral("Open a livery project to edit car paint colors.")
                : QStringLiteral("Load a car to see its paint regions."),
            colorsPage_);
        empty->setProperty("secondary", true);
        empty->setWordWrap(true);
        colorsLayout_->addWidget(empty);
        colorsLayout_->addStretch(1);
        return;
    }

    int regionIndex = 0;
    for (const PaintRegion &paintRegion : paintRegions(model_, selectedLodIndex_)) {
        const bool bodyRegion = paintRegion.materialHashes.contains(
            fls::material_hashes::binding::kBodyPaint);
        QColor primary = bodyRegion ? basePaint_ : defaultPreviewCarColor();
        QColor secondary = primary;
        int currentFinish = 0;
        bool finishFound = false;
        bool colorFound = false;
        QVector<quint64> displayHashes = paintRegion.materialHashes;
        appendUniqueHashes(displayHashes, paintRegion.fallbackHashes);
        for (quint64 materialHash : displayHashes) {
            const fls::LiveryPaintMaterial *material = project_ != nullptr
                ? project_->liveryPaint.find(materialHash)
                : nullptr;
            if (!finishFound && material != nullptr && material->finish != 0) {
                currentFinish = static_cast<int>(material->finish);
                finishFound = true;
            }
            if (!colorFound && material != nullptr && material->primary.enabled) {
                primary = colorFromBgra(material->primary.bgra);
                secondary = primary;
                if (material->secondary.enabled) {
                    secondary = colorFromBgra(material->secondary.bgra);
                }
                colorFound = true;
            }
        }

        if (regionIndex++ > 0) {
            auto *separator = new QFrame(colorsPage_);
            separator->setFrameShape(QFrame::HLine);
            separator->setStyleSheet(
                QStringLiteral("color: rgba(255, 255, 255, 28);"));
            colorsLayout_->addWidget(separator);
        }

        auto *regionLabel = new QLabel(paintRegion.name, colorsPage_);
        QFont regionFont = regionLabel->font();
        regionFont.setBold(true);
        regionLabel->setFont(regionFont);
        colorsLayout_->addWidget(regionLabel);

        if (bodyRegion) {
            auto *overrideChildren = new QCheckBox(
                QStringLiteral("Override hood, mirrors and wing"), colorsPage_);
            overrideChildren->setChecked(overrideBodyChildPaint_);
            overrideChildren->setToolTip(QStringLiteral(
                "Apply body color and material changes to its child parts too"));
            colorsLayout_->addWidget(overrideChildren);
            connect(overrideChildren, &QCheckBox::toggled, this,
                    [this](bool checked) {
                        overrideBodyChildPaint_ = checked;
                    });
        }

        auto *primaryButton = new QPushButton(
            carColorSwatchIcon(primary), QStringLiteral("Primary color"), colorsPage_);
        auto *secondaryButton = new QPushButton(
            carColorSwatchIcon(secondary), QStringLiteral("Secondary color"), colorsPage_);
        colorsLayout_->addWidget(primaryButton);
        colorsLayout_->addWidget(secondaryButton);

        auto *materialCombo = new NoWheelComboBox(colorsPage_);
        materialCombo->addItem(QStringLiteral("Gloss"), 1);
        materialCombo->addItem(QStringLiteral("Semigloss"), 2);
        materialCombo->addItem(QStringLiteral("Metallic"), 4);
        materialCombo->addItem(QStringLiteral("Matte"), 3);
        const int materialIndex = materialCombo->findData(currentFinish);
        materialCombo->setCurrentIndex(materialIndex);
        materialCombo->setPlaceholderText(QStringLiteral("Material"));
        materialCombo->setToolTip(QStringLiteral("Paint material finish"));
        colorsLayout_->addWidget(materialCombo);

        connect(primaryButton, &QPushButton::clicked, this,
                [this, hashes = paintRegion.materialHashes,
                 fallbackHashes = paintRegion.fallbackHashes]() {
                    QTimer::singleShot(0, this, [this, hashes, fallbackHashes]() {
                        chooseRegionColor(hashes, false, fallbackHashes);
                    });
                });
        connect(secondaryButton, &QPushButton::clicked, this,
                [this, hashes = paintRegion.materialHashes,
                 fallbackHashes = paintRegion.fallbackHashes]() {
                    QTimer::singleShot(0, this, [this, hashes, fallbackHashes]() {
                        chooseRegionColor(hashes, true, fallbackHashes);
                    });
                });
        connect(materialCombo, qOverload<int>(&QComboBox::activated), this,
                [this, materialCombo, hashes = paintRegion.materialHashes](int index) {
                    if (index < 0) {
                        return;
                    }
                    const quint32 finish = materialCombo->itemData(index).toUInt();
                    QTimer::singleShot(0, this, [this, hashes, finish]() {
                        chooseRegionFinish(hashes, finish);
                    });
                });
    }
    colorsLayout_->addStretch(1);
}

void CarPreviewWidget::updateCarColorControl() {
    std::optional<std::array<quint8, 4>> projectColor;
    if (project_ != nullptr && project_->isLivery) {
        projectColor = project_->liveryPaint.defaultCarColorBgra();
    }
    basePaint_ = projectColor.has_value()
        ? colorFromBgra(*projectColor)
        : defaultPreviewCarColor();
    basePaint_.setAlpha(255);
    rebuildColorsPanel();
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
    context()->functions()->glEnable(GL_MULTISAMPLE);
    geometryLoaded_ = geometry_.loadDefault();
    shapeRenderer_.initialize();
    if (geometryLoaded_ && shapeRenderer_.isInitialized()) {
        shapeRenderer_.uploadGeometry(geometry_);
    }
    carRenderer_.initialize();
    liveryTexture_ = 0;
    liveLiveryFullDirty_ = true;
    liveryDirty_ = true;
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

    if (modelUploadPending_ && carRenderer_.isInitialized()) {
        carRenderer_.uploadModel(model_, selectedLodIndex_);
        modelUploadPending_ = false;
        if (modelFitPending_) {
            fitCameraToModel();
            modelFitPending_ = false;
        }
    }
    if (liveryMasksPending_ && carRenderer_.isInitialized()) {
        carRenderer_.setLivery(model_, liveryMasks_);
        liveryMasksPending_ = false;
    }

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
    loadCarAsync(path, [](bool loaded, const QString &error) {
        if (!loaded) {
            qWarning().noquote() << error;
        }
    });
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

void CarPreviewWidget::updateLodControl() {
    if (modelPage_ == nullptr || modelOptionsLayout_ == nullptr) {
        return;
    }
    const QVariant displayedLodCount = modelPage_->property("lodCount");
    if (!displayedLodCount.isValid()
        || displayedLodCount.toInt() != model_.lodCount()) {
        rebuildModelPanel();
        return;
    }
    for (QRadioButton *radio : modelPage_->findChildren<QRadioButton *>()) {
        if (!radio->property("previewLodIndex").isValid()) {
            continue;
        }
        const QSignalBlocker blocker(radio);
        radio->setChecked(
            radio->property("previewLodIndex").toInt() == selectedLodIndex_);
    }
}

void CarPreviewWidget::layoutOverlayControls() {
    if (sidePanel_ == nullptr || sidePanelToggle_ == nullptr) {
        return;
    }
    const int preferredWidth = std::min(292, std::max(220, width() * 2 / 5));
    const int panelWidth = std::min(preferredWidth, std::max(0, width() - 38));
    sidePanel_->setGeometry(width() - panelWidth, 0, panelWidth, height());
    sidePanel_->setVisible(sidePanelVisible_);

    const int toggleX = sidePanelVisible_
        ? std::max(3, width() - panelWidth + 5)
        : std::max(3, width() - sidePanelToggle_->width() - 5);
    sidePanelToggle_->move(toggleX, 5);
    sidePanel_->raise();
    sidePanelToggle_->raise();
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
            visible ? QStringLiteral("Hide preview controls")
                    : QStringLiteral("Show preview controls"));
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

void CarPreviewWidget::rebuildModelPanel() {
    if (modelOptionsLayout_ == nullptr || modelPage_ == nullptr) {
        return;
    }
    qDeleteAll(modelPage_->findChildren<QButtonGroup *>(
        QString(), Qt::FindDirectChildrenOnly));
    while (QLayoutItem *item = modelOptionsLayout_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            delete widget;
        }
        delete item;
    }
    modelPage_->setProperty("lodCount", model_.lodCount());

    auto *title = new QLabel(QStringLiteral("Level of detail"), modelPage_);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
    title->setFont(titleFont);
    modelOptionsLayout_->addWidget(title);

    auto *description = new QLabel(
        QStringLiteral("Select which level of detail is rendered in the preview."),
        modelPage_);
    description->setProperty("secondary", true);
    description->setWordWrap(true);
    modelOptionsLayout_->addWidget(description);
    modelOptionsLayout_->addSpacing(5);

    if (!hasModel()) {
        auto *empty = new QLabel(
            QStringLiteral("Load a car to choose its model detail."), modelPage_);
        empty->setProperty("secondary", true);
        empty->setWordWrap(true);
        modelOptionsLayout_->addWidget(empty);
        modelOptionsLayout_->addStretch(1);
        return;
    }

    auto *buttonGroup = new QButtonGroup(modelPage_);
    buttonGroup->setExclusive(true);
    for (int lodIndex = 0; lodIndex < model_.lodCount(); ++lodIndex) {
        const QString label = QStringLiteral("LOD %1").arg(lodIndex);
        auto *radio = new QRadioButton(label, modelPage_);
        radio->setProperty("previewLodIndex", lodIndex);
        radio->setChecked(lodIndex == selectedLodIndex_);
        buttonGroup->addButton(radio, lodIndex);
        modelOptionsLayout_->addWidget(radio);
        connect(radio, &QRadioButton::clicked, this,
                [this, lodIndex]() { selectLod(lodIndex); });
    }
    if (model_.lodCount() == 1) {
        auto *note = new QLabel(
            QStringLiteral("This car only provides one detail level."), modelPage_);
        note->setProperty("secondary", true);
        note->setWordWrap(true);
        modelOptionsLayout_->addWidget(note);
    }
    modelOptionsLayout_->addStretch(1);
}

void CarPreviewWidget::rebuildPartsPanel() {
    if (partsOptionsLayout_ == nullptr || partsPage_ == nullptr) {
        return;
    }
    qDeleteAll(partsPage_->findChildren<QButtonGroup *>(
        QString(), Qt::FindDirectChildrenOnly));
    while (QLayoutItem *item = partsOptionsLayout_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            delete widget;
        }
        delete item;
    }

    auto *title = new QLabel(QStringLiteral("Body parts"), partsPage_);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
    title->setFont(titleFont);
    partsOptionsLayout_->addWidget(title);

    auto *description = new QLabel(
        QStringLiteral("Choose the body kit and installed exterior parts."),
        partsPage_);
    description->setProperty("secondary", true);
    description->setWordWrap(true);
    partsOptionsLayout_->addWidget(description);
    partsOptionsLayout_->addSpacing(5);

    selectedPartOptions_.clear();
    const fls::CarPartOption *defaultCarBody = nullptr;
    for (const fls::CarPartOption &option : model_.partOptions) {
        if (option.partType != fls::car_part_types::CarBody || !option.stock) {
            continue;
        }
        if (defaultCarBody == nullptr || option.level < defaultCarBody->level
            || (option.level == defaultCarBody->level && option.id < defaultCarBody->id)) {
            defaultCarBody = &option;
        }
    }
    const int defaultCarBodyId = defaultCarBody != nullptr ? defaultCarBody->id : -1;
    QSet<int> bodyKitOptionIds;
    for (const fls::CarPartOption &option : model_.partOptions) {
        if (option.partType == fls::car_part_types::CarBody
            && option.id != defaultCarBodyId) {
            bodyKitOptionIds.insert(option.id);
        }
    }
    int visibleGroups = 0;
    for (int partType : kSelectablePartTypes) {
        QVector<const fls::CarPartOption *> allOptions;
        for (const fls::CarPartOption &option : model_.partOptions) {
            if (option.partType == partType) {
                allOptions.push_back(&option);
            }
        }
        if (allOptions.isEmpty()) {
            continue;
        }
        std::sort(allOptions.begin(), allOptions.end(), [](const auto *a, const auto *b) {
            if (a->stock != b->stock) return a->stock;
            if (a->level != b->level) return a->level < b->level;
            return a->id < b->id;
        });

        const fls::CarPartOption *selected = allOptions.front();
        if (partType == fls::car_part_types::CarBody && defaultCarBody != nullptr) {
            selected = defaultCarBody;
        } else {
            const auto preferred = std::find_if(
                allOptions.begin(), allOptions.end(), [defaultCarBodyId](const auto *option) {
                    return option->id == defaultCarBodyId;
                });
            if (preferred != allOptions.end()) {
                selected = *preferred;
            } else {
                const auto stock = std::find_if(
                    allOptions.begin(), allOptions.end(), [](const auto *option) {
                        return option->stock;
                    });
                if (stock != allOptions.end()) {
                    selected = *stock;
                }
            }
        }
        selectedPartOptions_.insert(partType, selected->id);

        QVector<const fls::CarPartOption *> options;
        for (const fls::CarPartOption *option : allOptions) {
            const bool bodyKitOwned = partType != fls::car_part_types::CarBody
                && (bodyKitOptionIds.contains(option->id)
                    || (defaultCarBodyId >= 0 && option->carBodyId >= 0
                        && option->carBodyId != defaultCarBodyId));
            if (!bodyKitOwned) {
                options.push_back(option);
            }
        }

        QSet<QString> distinctModels;
        for (const fls::CarPartOption *option : options) {
            distinctModels.insert(optionModelKey(*option));
        }
        if (options.size() < 2 || distinctModels.size() < 2) {
            continue;
        }

        if (visibleGroups > 0) {
            auto *separator = new QFrame(partsPage_);
            separator->setFrameShape(QFrame::HLine);
            separator->setStyleSheet(
                QStringLiteral("color: rgba(255, 255, 255, 28);"));
            partsOptionsLayout_->addWidget(separator);
        }
        auto *groupLabel = new QLabel(partTypeDisplayName(partType), partsPage_);
        QFont font = groupLabel->font();
        font.setBold(true);
        groupLabel->setFont(font);
        partsOptionsLayout_->addWidget(groupLabel);

        auto *buttonGroup = new QButtonGroup(partsPage_);
        buttonGroup->setExclusive(true);
        int alternativeIndex = 0;
        for (const fls::CarPartOption *option : options) {
            const int labelIndex = option->stock ? 0 : alternativeIndex++;
            auto *radio = new QRadioButton(
                partOptionDisplayName(
                    model_.mediaName, *option, labelIndex, option->id == selected->id),
                partsPage_);
            radio->setProperty("carPartType", partType);
            radio->setProperty("carPartOptionId", option->id);
            radio->setToolTip(option->modelPaths.join(QLatin1Char('\n')));
            radio->setChecked(option->id == selected->id);
            buttonGroup->addButton(radio);
            partsOptionsLayout_->addWidget(radio);
            connect(radio, &QRadioButton::toggled, this,
                    [this, partType, optionId = option->id](bool checked) {
                if (checked) {
                    selectPartOption(partType, optionId);
                }
            });
        }
        ++visibleGroups;
    }

    carRenderer_.setPartSelections(selectedPartOptions_);
    if (visibleGroups == 0) {
        auto *empty = new QLabel(
            hasModel()
                ? QStringLiteral("This car has no selectable exterior part variants.")
                : QStringLiteral("Load a car to see its available parts."),
            partsPage_);
        empty->setProperty("secondary", true);
        empty->setWordWrap(true);
        partsOptionsLayout_->addWidget(empty);
    }
    partsOptionsLayout_->addStretch(1);
}

void CarPreviewWidget::selectPartOption(int partType, int optionId) {
    const auto applyCarBody = [this](int carBodyOptionId) {
        const auto body = std::find_if(
            model_.partOptions.begin(), model_.partOptions.end(),
            [carBodyOptionId](const fls::CarPartOption &option) {
                return option.partType == fls::car_part_types::CarBody
                    && option.id == carBodyOptionId;
            });
        const int linkedCarBodyId = body != model_.partOptions.end()
            ? body->carBodyId : -1;

        selectedPartOptions_.insert(fls::car_part_types::CarBody, carBodyOptionId);
        for (int relatedType : {fls::car_part_types::FrontBumper,
                                fls::car_part_types::RearBumper,
                                fls::car_part_types::SideSkirts}) {
            const fls::CarPartOption *related = linkedCarBodyPartOption(
                model_, relatedType, carBodyOptionId, linkedCarBodyId);
            if (related != nullptr) {
                selectedPartOptions_.insert(relatedType, related->id);
            }
        }
    };

    if (partType == fls::car_part_types::CarBody) {
        applyCarBody(optionId);
    } else {
        selectedPartOptions_.insert(partType, optionId);
    }
    carRenderer_.setPartSelections(selectedPartOptions_);
    syncPartOptionControls();
    switchCarUnwrapOverlay();
    Q_EMIT unwrapOverlayChanged();
    update();
}

void CarPreviewWidget::syncPartOptionControls() {
    if (partsPage_ == nullptr) {
        return;
    }
    for (QRadioButton *radio : partsPage_->findChildren<QRadioButton *>()) {
        const int partType = radio->property("carPartType").toInt();
        const int optionId = radio->property("carPartOptionId").toInt();
        const QSignalBlocker blocker(radio);
        radio->setChecked(selectedPartOptions_.value(partType, -1) == optionId);
    }
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
