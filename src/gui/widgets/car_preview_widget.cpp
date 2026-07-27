#include "car_preview_widget.h"

#include "color_space.h"
#include "car_scene.h"
#include "editor_state.h"
#include "gui_assets.h"
#include "material_hashes.h"
#include "matrix_math.h"
#include "model_material.h"
#include "scene_view.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

namespace gui {
namespace {

constexpr int kLiveryBaseTexWidth = 2048;
constexpr int kLiveryBaseTexHeight = 1024;
constexpr int kLiverySectionMaskSlots[fh6::kLiverySideCount] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
};

constexpr char kPostProcessVertexShader[] = R"(#version 330 core
out vec2 v_uv;

void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2(3.0, -1.0),
        vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexID];
    v_uv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

constexpr char kBloomExtractFragmentShader[] = R"(#version 330 core
in vec2 v_uv;

uniform sampler2D scene_texture;
uniform float exposure;
uniform float bloom_cutoff;

out vec4 out_color;

void main()
{
    vec3 exposed = max(texture(scene_texture, v_uv).rgb, vec3(0.0)) * exp2(exposure);
    float luminance = dot(exposed, vec3(0.2126, 0.7152, 0.0722));
    float contribution = max(luminance - bloom_cutoff, 0.0) / max(luminance, 0.0001);
    out_color = vec4(exposed * contribution, 0.0);
}
)";

constexpr char kBloomBlurFragmentShader[] = R"(#version 330 core
in vec2 v_uv;

uniform sampler2D source_texture;
uniform vec2 texel_step;

out vec4 out_color;

void main()
{
    vec3 color = texture(source_texture, v_uv).rgb * 0.227027;
    color += texture(source_texture, v_uv + texel_step * 1.384615).rgb * 0.316216;
    color += texture(source_texture, v_uv - texel_step * 1.384615).rgb * 0.316216;
    color += texture(source_texture, v_uv + texel_step * 3.230769).rgb * 0.070270;
    color += texture(source_texture, v_uv - texel_step * 3.230769).rgb * 0.070270;
    out_color = vec4(color, 0.0);
}
)";

constexpr char kBloomCompositeFragmentShader[] = R"(#version 330 core
in vec2 v_uv;

uniform sampler2D scene_texture;
uniform sampler2D bloom_texture;
uniform float exposure;
uniform float bloom_scale;
uniform int bloom_enabled;

out vec4 out_color;

void main()
{
    vec4 scene = texture(scene_texture, v_uv);
    vec3 exposed = max(scene.rgb, vec3(0.0)) * exp2(exposure);
    vec3 bloom = bloom_enabled != 0 ? texture(bloom_texture, v_uv).rgb : vec3(0.0);
    out_color = vec4(exposed + bloom * bloom_scale, scene.a);
}
)";

constexpr char kColorGradeFragmentShader[] = R"(#version 330 core
in vec2 v_uv;

uniform sampler2D scene_texture;
uniform sampler3D color_lut;
uniform float lut_dimension;
uniform float lut_scale;
uniform int lut_enabled;

out vec4 out_color;

vec3 pqEncode(vec3 value)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 powered = pow(clamp(value, vec3(0.0), vec3(1.0)), vec3(m1));
    return pow((c1 + c2 * powered) / (1.0 + c3 * powered), vec3(m2));
}

void main()
{
    vec4 scene = texture(scene_texture, v_uv);
    if (lut_enabled == 0) {
        out_color = scene;
        return;
    }
    vec3 encoded = pqEncode(max(scene.rgb, vec3(0.0)) / lut_scale);
    vec3 coordinate = (encoded * (lut_dimension - 1.0) + 0.5) / lut_dimension;
    out_color = vec4(texture(color_lut, coordinate).rgb, scene.a);
}
)";

constexpr char kPostProcessFragmentShader[] = R"(#version 330 core
in vec2 v_uv;

uniform sampler2D scene_texture;
uniform float filmic_white;

out vec4 out_color;

vec3 hableCurve(vec3 x)
{
    const float A = 0.151;
    const float B = 0.05;
    const float C = 0.566;
    const float D = 0.21;
    const float E = 0.003;
    const float F = 0.141;
    return ((x * (A * x + C * B) + D * E)
            / (x * (A * x + B) + D * F)) - E / F;
}

vec3 linearToSrgb(vec3 color)
{
    vec3 low = color * 12.92;
    vec3 high = 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

void main()
{
    vec4 scene = texture(scene_texture, v_uv);
    vec3 mapped = hableCurve(max(scene.rgb, vec3(0.0))) / hableCurve(vec3(filmic_white));
    out_color = vec4(linearToSrgb(max(mapped, vec3(0.0))), scene.a);
}
)";

enum class PostProcessPass {
    BloomExtract,
    BloomBlurHorizontal,
    BloomBlurVertical,
    BloomComposite,
    ColorGrade,
    DisplayOutput,
};

constexpr std::array<PostProcessPass, 6> kPostProcessOrder = {
    PostProcessPass::BloomExtract,
    PostProcessPass::BloomBlurHorizontal,
    PostProcessPass::BloomBlurVertical,
    PostProcessPass::BloomComposite,
    PostProcessPass::ColorGrade,
    PostProcessPass::DisplayOutput,
};

bool transposedSection(int maskSlot) {
    return maskSlot == 5 || maskSlot == 6 || maskSlot == 7;
}

struct ProjectedLiverySection {
    fh6::Project project;
    QRect clipRect;
};

struct PackedLiveryLayout {
    std::array<QRect, fh6::kLiverySideCount> rects;
    QVector<QVector4D> uvRegions;
    QSize textureSize;
    bool valid = false;
};

PackedLiveryLayout packedLiveryLayout(const fh6::LiveryMaskSet &masks, const QSize &baseTextureSize) {
    PackedLiveryLayout layout;
    layout.uvRegions.resize(fh6::kLiverySideCount);
    struct Item {
        int slot = 0;
        QSize size;
    };
    QVector<Item> items;
    const double scale = static_cast<double>(baseTextureSize.width()) / kLiveryBaseTexWidth;
    for (int slot = 0; slot < fh6::kLiverySideCount; ++slot) {
        const fh6::LiverySide &side = masks.sides[slot];
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

    for (int slot = 0; slot < fh6::kLiverySideCount; ++slot) {
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

fh6::Matrix3 fromQTransform(const QTransform &t) {
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
                            fh6::scene::Group &outRoot) {
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

std::optional<ProjectedLiverySection> buildProjectedLiverySection(const fh6::Project &project,
                                                                  const fh6::scene::Group &section,
                                                                  const fh6::LiveryMaskSet &masks,
                                                                  const QSize &texSize,
                                                                  const PackedLiveryLayout &layout) {
    if (!section.isLiverySection || !layout.valid) {
        return std::nullopt;
    }
    const int slot = section.liverySectionSlot;
    if (slot < 0 || slot >= fh6::kLiverySideCount) {
        return std::nullopt;
    }
    const int maskSlot = kLiverySectionMaskSlots[slot];
    const fh6::LiverySide &side = masks.sides[maskSlot];
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
    QTransform projectionTransform;
    if (slot == 4 || slot == 10) {
        projectionTransform.scale(-1.0, -1.0);
    } else if (slot == 6) {
        projectionTransform.scale(-1.0, 1.0);
    } else if (slot == 7) {
        projectionTransform.scale(1.0, -1.0);
    }
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
fh6::WheelSizing wheelSizingForModelCode(const QString &modelCode) {
    static const auto table = [] {
        QHash<QString, fh6::WheelSizing> sizes;
        fh6::WheelSizing fallback;
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
            const auto parseAxle = [](const QJsonValue &value, fh6::AxleSizing def) {
                const QJsonObject o = value.toObject();
                fh6::AxleSizing axle = def;
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
            const auto parse = [&parseAxle](const QJsonObject &o, fh6::WheelSizing def) {
                fh6::WheelSizing w;
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

std::optional<fh6::CarModel> loadArchivedModel(
    const QString &archivePath, const QString &modelName) {
    if (archivePath.isEmpty()) {
        return std::nullopt;
    }
    QTemporaryDir extracted;
    QString error;
    if (!extracted.isValid()
        || !fh6::extractZipArchive(archivePath, extracted.path(), &error)) {
        return std::nullopt;
    }
    QDirIterator it(
        extracted.path(), QStringList{modelName}, QDir::Files, QDirIterator::Subdirectories);
    if (!it.hasNext()) {
        return std::nullopt;
    }
    fh6::CarModel model = fh6::loadModelBin(it.next(), &error);
    return model.meshes.empty() ? std::nullopt
                                : std::optional<fh6::CarModel>(std::move(model));
}

void appendSharedTireB(
    fh6::CarModel &model, const QString &sourcePath, const fh6::WheelSizing &wheels) {
    const QString leftArchive = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/scene/tires/tire_b.zip"));
    const QString rightArchive = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/scene/tires/tireR_b.zip"));
    std::optional<fh6::CarModel> left = loadArchivedModel(
        leftArchive, QStringLiteral("tireL_b.modelbin"));
    std::optional<fh6::CarModel> right = loadArchivedModel(
        rightArchive, QStringLiteral("tireR_b.modelbin"));
    if (!right) {
        right = loadArchivedModel(leftArchive, QStringLiteral("tireR_b.modelbin"));
    }
    // Tag the shared tire meshes with a /tires/ path so their rubber material resolves from
    // the _library like the exterior parts (appendApproximateTires copies these meshes).
    const auto tagTires = [](std::optional<fh6::CarModel> &tire, const QString &path) {
        if (tire) {
            for (fh6::CarMesh &mesh : tire->meshes) {
                mesh.sourceModelPath = path;
            }
        }
    };
    tagTires(left, QStringLiteral("_library/scene/tires/tireL_b.modelbin"));
    tagTires(right, QStringLiteral("_library/scene/tires/tireR_b.modelbin"));
    if (left && right) {
        fh6::appendApproximateTires(model, *left, *right, wheels);
    }
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
void assignSharedSlotMaterials(fh6::CarModel &model) {
    for (fh6::CarMesh &mesh : model.meshes) {
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

QString assetFileIdentity(const QString &path) {
    const QFileInfo info(path);
    return QDir::cleanPath(info.absoluteFilePath()).toLower()
        + QLatin1Char('|') + QString::number(info.size())
        + QLatin1Char('|') + QString::number(info.lastModified().toMSecsSinceEpoch());
}

QHash<QString, std::shared_ptr<fh6::ModelMaterial>> &materialDefaultsCache() {
    static QHash<QString, std::shared_ptr<fh6::ModelMaterial>> cache;
    return cache;
}

class NativeTextureCache {
public:
    std::shared_ptr<const fh6::ModelMaterialTexture> find(const QString &key, bool &known) {
        const auto it = entries_.find(key);
        known = it != entries_.end();
        if (!known) {
            return {};
        }
        it->lastUse = ++clock_;
        return it->texture;
    }

    void insert(const QString &key,
                const std::shared_ptr<const fh6::ModelMaterialTexture> &texture) {
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
        std::shared_ptr<const fh6::ModelMaterialTexture> texture;
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
    PaintNormalMap00,
    PaintNormalMap0,
    OrangePeelNormal,
    Unknown,
};

NativeTextureSlot nativeTextureSlot(const fh6::ModelMaterialParameter &parameter) {
    QString path = parameter.texturePath.toLower();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (parameter.nameHash == fh6::material_hashes::parameter::kNormalMap00Texture) {
        return NativeTextureSlot::PaintNormalMap00;
    }
    if (parameter.nameHash == fh6::material_hashes::parameter::kNormalMap0Texture) {
        return NativeTextureSlot::PaintNormalMap0;
    }
    if (parameter.nameHash == fh6::material_hashes::parameter::kOrangePeelNormalTexture) {
        return NativeTextureSlot::OrangePeelNormal;
    }
    if (parameter.nameHash == fh6::material_hashes::parameter::kNormalTexture
        || path.contains(QStringLiteral("normal"))
        || path.contains(QStringLiteral("nrml"))) {
        return NativeTextureSlot::Normal;
    }
    if (parameter.nameHash == fh6::material_hashes::parameter::kSurfaceTexture
        || path.contains(QStringLiteral("rmao"))
        || path.contains(QStringLiteral("roughmetal"))
        || path.contains(QStringLiteral("metalrough"))) {
        return NativeTextureSlot::Surface;
    }
    if (fh6::material_hashes::contains(
            fh6::material_hashes::parameter::kEmissiveTexture, parameter.nameHash)
        || path.contains(QStringLiteral("emissive"))
        || path.contains(QStringLiteral("emission"))) {
        return NativeTextureSlot::Emissive;
    }
    if (parameter.nameHash == fh6::material_hashes::parameter::kColorTexture
        || path.contains(QStringLiteral("basecolor"))
        || path.contains(QStringLiteral("diffuse"))
        || path.contains(QStringLiteral("albedo"))) {
        return NativeTextureSlot::Diffuse;
    }
    if (fh6::material_hashes::contains(
            fh6::material_hashes::parameter::kAlphaTexture, parameter.nameHash)
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

void assignNativeTexture(fh6::ModelMaterial &material,
                         NativeTextureSlot slot,
                         const std::shared_ptr<const fh6::ModelMaterialTexture> &texture) {
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
    case NativeTextureSlot::PaintNormalMap00:
        material.paintNormalMap00Texture = texture;
        break;
    case NativeTextureSlot::PaintNormalMap0:
        material.paintNormalMap0Texture = texture;
        break;
    case NativeTextureSlot::OrangePeelNormal:
        material.orangePeelNormalTexture = texture;
        break;
    case NativeTextureSlot::Unknown:
        break;
    }
}

std::shared_ptr<const fh6::ModelMaterialTexture> missingColorTexture() {
    static const std::shared_ptr<const fh6::ModelMaterialTexture> texture = []() {
        QImage image(assetPath(QStringLiteral("raster/MissingTexture.png")));
        image = image.convertToFormat(QImage::Format_RGBA8888);
        if (image.isNull()) {
            return std::shared_ptr<const fh6::ModelMaterialTexture>{};
        }
        auto result = std::make_shared<fh6::ModelMaterialTexture>();
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
        return std::shared_ptr<const fh6::ModelMaterialTexture>(std::move(result));
    }();

    return texture;
}

void assignNativeTextureOrFallback(
    fh6::ModelMaterial &material,
    NativeTextureSlot slot,
    const std::shared_ptr<const fh6::ModelMaterialTexture> &texture) {
    assignNativeTexture(
        material, slot,
        texture || slot != NativeTextureSlot::Diffuse ? texture : missingColorTexture());
}

void resolveExteriorMaterials(
    fh6::CarModel &model, const QString &sourcePath, const QString &carRoot,
    bool includeTextures) {
    const QString archivePath = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/Materials.zip"));
    if (archivePath.isEmpty()) {
        return;
    }
    const QString materialArchiveKey = assetFileIdentity(archivePath);
    QHash<QString, std::shared_ptr<fh6::ModelMaterial>> &defaults = materialDefaultsCache();
    QStringList missingMaterialEntries;
    QSet<QString> requestedMaterials;
    for (fh6::CarMesh &mesh : model.meshes) {
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
    const QHash<QString, QByteArray> missingMaterialData =
        fh6::readZipEntries(archivePath, missingMaterialEntries);
    for (const QString &entry : missingMaterialEntries) {
        std::shared_ptr<fh6::ModelMaterial> decoded;
        const QByteArray bytes = missingMaterialData.value(entry.toLower());
        if (!bytes.isEmpty()) {
            try {
                decoded = fh6::decodeMaterialBundle(bytes);
            } catch (const std::exception &) {
            }
        }
        defaults.insert(materialArchiveKey + QLatin1Char('|') + entry.toLower(), decoded);
    }
    for (fh6::CarMesh &mesh : model.meshes) {
        if (!mesh.material || !isLibraryMaterialPath(mesh.sourceModelPath)) {
            continue;
        }
        const QString entry = materialArchiveEntry(mesh.material->resourcePath);
        const std::shared_ptr<fh6::ModelMaterial> materialDefaults =
            defaults.value(materialArchiveKey + QLatin1Char('|') + entry.toLower());
        if (materialDefaults) {
            mesh.material = fh6::mergeModelMaterialDefaults(*materialDefaults, *mesh.material);
            mesh.material->resolvedFromLibrary = true;
        }
    }

    if (!includeTextures) {
        return;
    }

    struct PendingTexture {
        std::shared_ptr<fh6::ModelMaterial> material;
        NativeTextureSlot slot = NativeTextureSlot::Unknown;
        QString path;
        QString sharedEntry;
        QString localPath;
        QString cacheKey;
    };
    QVector<PendingTexture> pending;
    const auto appendPending = [&](const std::shared_ptr<fh6::ModelMaterial> &material,
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
    QSet<const fh6::ModelMaterial *> visited;
    for (fh6::CarMesh &mesh : model.meshes) {
        if (!mesh.material || visited.contains(mesh.material.get())
            || !isLibraryMaterialPath(mesh.sourceModelPath)
            || mesh.materialName.startsWith(QStringLiteral("carPaint"), Qt::CaseInsensitive)) {
            continue;
        }
        visited.insert(mesh.material.get());
        bool hasNormal = false;
        for (const fh6::ModelMaterialParameter &parameter : mesh.material->parameters) {
            if (parameter.type != fh6::ModelMaterialParameterType::Texture2D
                || parameter.texturePath.isEmpty()) {
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
        if (!hasNormal
            && normalizedTexturePath(mesh.material->resourcePath)
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
        : fh6::readZipEntries(textureArchive, missingSharedEntries);
    for (const PendingTexture &item : pending) {
        if (item.cacheKey.isEmpty()) {
            continue;
        }
        bool known = false;
        std::shared_ptr<const fh6::ModelMaterialTexture> texture =
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
                fh6::SwatchImage image = fh6::decodeSwatchImage(bytes);
                if (image.valid()) {
                    auto decoded = std::make_shared<fh6::ModelMaterialTexture>();
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

fh6::ManufacturerColorPalette loadManufacturerColors(
    const QString &carRoot, const QString &sourcePath) {
    QFile file(QDir(carRoot).filePath(QStringLiteral("ManufacturerColors.bin")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    fh6::ManufacturerColorPalette palette;
    try {
        palette = fh6::decodeManufacturerColors(file.readAll());
    } catch (const std::exception &) {
        return {};
    }

    const QString archivePath = findSharedCarAsset(
        sourcePath, QStringLiteral("_library/Materials.zip"));
    if (archivePath.isEmpty()) {
        return palette;
    }

    const QString archiveKey = assetFileIdentity(archivePath);
    QHash<QString, std::shared_ptr<fh6::ModelMaterial>> &defaults = materialDefaultsCache();
    QStringList missingEntries;
    QSet<QString> requestedEntries;
    for (const fh6::ManufacturerColor &color : std::as_const(palette.colors)) {
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
        fh6::readZipEntries(archivePath, missingEntries);
    for (const QString &entry : missingEntries) {
        std::shared_ptr<fh6::ModelMaterial> material;
        const QByteArray bytes = materialData.value(entry.toLower());
        if (!bytes.isEmpty()) {
            try {
                material = fh6::decodeMaterialBundle(bytes);
            } catch (const std::exception &) {
            }
        }
        defaults.insert(archiveKey + QLatin1Char('|') + entry.toLower(), material);
    }
    for (fh6::ManufacturerColor &color : palette.colors) {
        const QString entry = materialArchiveEntry(color.materialPath);
        color.material = defaults.value(
            archiveKey + QLatin1Char('|') + entry.toLower());
    }

    return palette;
}

struct PreparedCar {
    fh6::CarModel model;
    fh6::LiveryMaskSet liveryMasks;
    fh6::ManufacturerColorPalette manufacturerColors;
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
        if (!fh6::extractZipArchive(path, extracted->path(), error)) {
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

    const fh6::WheelSizing wheelSizing =
        wheelSizingForModelCode(QFileInfo(loadPath).completeBaseName());
    fh6::CarModel model = loadPath.endsWith(QStringLiteral(".carbin"), Qt::CaseInsensitive)
        ? fh6::loadCarBin(loadPath, error, wheelSizing)
        : fh6::loadModelBin(loadPath, error);
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
        prepared->liveryMasks = fh6::loadLiveryMasks(masksDir);
        prepared->liveryMasksDir = masksDir;
    }

    return prepared;
}

} // namespace

QString CarPreviewWidget::postProcessShaderSelfTest() {
    const std::array<std::pair<const char *, const char *>, 5> shaders = {{
        {"bloom extract", kBloomExtractFragmentShader},
        {"bloom blur", kBloomBlurFragmentShader},
        {"bloom composite", kBloomCompositeFragmentShader},
        {"colour grade", kColorGradeFragmentShader},
        {"display output", kPostProcessFragmentShader},
    }};
    for (const auto &[name, fragmentShader] : shaders) {
        QOpenGLShaderProgram program;
        if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, kPostProcessVertexShader)
            || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)
            || !program.link()) {
            return QStringLiteral("%1: %2").arg(QString::fromLatin1(name), program.log());
        }
    }

    return QString();
}

CarPreviewWidget::CarPreviewWidget(QWidget *parent)
    : QOpenGLWidget(parent),
      yaw_(renderSettings_.camera.yawRadians),
      pitch_(renderSettings_.camera.pitchRadians),
      distance_(renderSettings_.camera.initialDistance) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setAlphaBufferSize(8);
    format.setSamples(4);
    setFormat(format);
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::ActionsContextMenu);

    auto *lightDirectionGroup = new QActionGroup(this);
    lightDirectionGroup->setExclusive(true);
    auto *oppositeXmlDirection = new QAction(
        QStringLiteral("Light direction A — opposite XML sign"), lightDirectionGroup);
    oppositeXmlDirection->setCheckable(true);
    oppositeXmlDirection->setChecked(
        renderSettings_.lighting.directionCandidate == LightDirectionCandidate::OppositeXmlDirection);
    connect(oppositeXmlDirection, &QAction::triggered, this, [this]() {
        setLightDirectionCandidate(LightDirectionCandidate::OppositeXmlDirection);
    });
    addAction(oppositeXmlDirection);
    auto *xmlDirection = new QAction(
        QStringLiteral("Light direction B — XML sign"), lightDirectionGroup);
    xmlDirection->setCheckable(true);
    xmlDirection->setChecked(
        renderSettings_.lighting.directionCandidate == LightDirectionCandidate::XmlDirection);
    connect(xmlDirection, &QAction::triggered, this, [this]() {
        setLightDirectionCandidate(LightDirectionCandidate::XmlDirection);
    });
    addAction(xmlDirection);
    auto *bothDirections = new QAction(
        QStringLiteral("Light direction A+B — balanced"), lightDirectionGroup);
    bothDirections->setCheckable(true);
    bothDirections->setChecked(
        renderSettings_.lighting.directionCandidate == LightDirectionCandidate::BothDirections);
    connect(bothDirections, &QAction::triggered, this, [this]() {
        setLightDirectionCandidate(LightDirectionCandidate::BothDirections);
    });
    addAction(bothDirections);

    auto *environmentGroup = new QActionGroup(this);
    environmentGroup->setExclusive(true);
    auto *gameEnvironment = new QAction(
        QStringLiteral("Environment — game probes"), environmentGroup);
    gameEnvironment->setCheckable(true);
    gameEnvironment->setChecked(gameEnvironmentEnabled_);
    connect(gameEnvironment, &QAction::triggered, this, [this]() {
        setGameEnvironmentEnabled(true);
    });
    addAction(gameEnvironment);
    auto *analyticEnvironment = new QAction(
        QStringLiteral("Environment — analytic fallback"), environmentGroup);
    analyticEnvironment->setCheckable(true);
    analyticEnvironment->setChecked(!gameEnvironmentEnabled_);
    connect(analyticEnvironment, &QAction::triggered, this, [this]() {
        setGameEnvironmentEnabled(false);
    });
    addAction(analyticEnvironment);

    auto *ground = new QAction(QStringLiteral("Ground and contact shadow"), this);
    ground->setCheckable(true);
    ground->setChecked(renderSettings_.ground.enabled);
    connect(ground, &QAction::toggled, this, [this](bool enabled) {
        renderSettings_.ground.enabled = enabled;
        update();
    });
    addAction(ground);

    auto *bloom = new QAction(QStringLiteral("HDR bloom"), this);
    bloom->setCheckable(true);
    bloom->setChecked(renderSettings_.postProcessing.bloomEnabled);
    connect(bloom, &QAction::toggled, this, [this](bool enabled) {
        renderSettings_.postProcessing.bloomEnabled = enabled;
        update();
    });
    addAction(bloom);

    auto *colorGrade = new QAction(QStringLiteral("Paint Car colour grade"), this);
    colorGrade->setCheckable(true);
    colorGrade->setChecked(renderSettings_.postProcessing.colorGradeEnabled);
    connect(colorGrade, &QAction::toggled, this, [this](bool enabled) {
        renderSettings_.postProcessing.colorGradeEnabled = enabled;
        update();
    });
    addAction(colorGrade);

    referenceNote_ = new QLabel(this);
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
    updateReferenceNote();
    referenceNote_->adjustSize();
    referenceNote_->raise();
}

CarPreviewWidget::~CarPreviewWidget() {
    makeCurrent();
    releasePostProcessing();
    groundRenderer_.release();
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
                    guard->liveryMasksDir_ = std::move(prepared->liveryMasksDir);
                    guard->modelUploadPending_ = true;
                    guard->liveryMasksPending_ = true;
                    guard->invalidateCachedLivery();
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
    carRenderer_.setDebugMode(0);
    transparentBackground_ = true;
    repaint();
    const QImage framebuffer = grabFramebuffer();
    transparentBackground_ = false;
    carRenderer_.setDebugMode(debugMode);
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
    if (!hasModel() && !carRenderer_.hasModel()) {
        return;
    }
    model_ = fh6::CarModel{};
    extractedCarDir_.reset();
    liveryMasks_ = {};
    liveryMasksDir_.clear();
    modelUploadPending_ = false;
    liveryMasksPending_ = false;
    invalidateCachedLivery();
    if (carRenderer_.isInitialized()) {
        makeCurrent();
        carRenderer_.clearModel();
        doneCurrent();
    }
    update();
}

QImage CarPreviewWidget::unwrapOverlay(int liverySectionSlot) const {
    if (!liveryMasks_.valid()) {
        return {};
    }
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
        firstSide = kLiverySectionMaskSlots[liverySectionSlot];
        lastSide = firstSide + 1;
    }
    const bool flipSectionX = liverySectionSlot == 4
        || liverySectionSlot == 6
        || liverySectionSlot == 7
        || liverySectionSlot == 10;
    const bool flipSectionY = liverySectionSlot == 4
        || liverySectionSlot == 7
        || liverySectionSlot == 10;
    const bool transpose = liverySectionSlot == 5
        || liverySectionSlot == 6
        || liverySectionSlot == 7;
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
            const uint8_t *cov = &mask.coverage[static_cast<size_t>(y) * w];
            for (int x = x0; x < x1; ++x) {
                if (cov[x] < 32) {
                    continue;
                }
                const double canvasX = (static_cast<double>(x) + 0.5) / sx - fh6::kLiveryCanvasHalfWidth;
                const double sectionX = transpose ? canvasY - originY : canvasX - originX;
                const double sectionY = transpose ? canvasX - originX : canvasY - originY;
                int outX = static_cast<int>(std::floor(
                    (sectionX + fh6::kLiveryCanvasHalfWidth) * sx));
                int outY = static_cast<int>(std::floor(
                    (sectionY + fh6::kLiveryCanvasHalfHeight) * sy));
                if (outX < 0 || outX >= w || outY < 0 || outY >= h) {
                    continue;
                }
                if (flipSectionX) {
                    outX = w - 1 - outX;
                }
                if (flipSectionY) {
                    outY = h - 1 - outY;
                }
                QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(outY));
                row[outX] = qRgba(c.red(), c.green(), c.blue(), 255);
                drew = true;
            }
        }
    }
    return drew ? image : QImage();
}

void CarPreviewWidget::setProject(fh6::Project *project) {
    project_ = project;
    invalidateCachedLivery();
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
        connect(state_, &EditorState::projectStructureChanged, this, &CarPreviewWidget::markLiveryDirty);
    }
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
    logGlCapabilities();
    initializePostProcessing();
    colorLutUploadPending_ = colorLut_.valid();
    geometryLoaded_ = geometry_.loadDefault();
    shapeRenderer_.initialize();
    if (geometryLoaded_ && shapeRenderer_.isInitialized()) {
        shapeRenderer_.uploadGeometry(geometry_);
    }
    carRenderer_.initialize();
    groundRenderer_.initialize();
    liveryTexture_ = 0;
    liveLiveryFullDirty_ = true;
    liveryDirty_ = true;
}

void CarPreviewWidget::resizeGL(int, int) {
}

void CarPreviewWidget::paintGL() {
    QOpenGLContext *ctx = context();
    if (ctx == nullptr) {
        return;
    }
    QOpenGLFunctions *functions = ctx->functions();

    if (colorLutUploadPending_) {
        const bool uploaded = uploadColorLut();
        if (!uploaded && !colorLutError_.isEmpty()) {
            qWarning().noquote() << "Garage colour-grade fallback:" << colorLutError_;
        }
        colorLutUploadPending_ = false;
    }

    if (environmentUploadPending_ && carRenderer_.isInitialized()) {
        QString error;
        const bool uploaded = gameEnvironmentEnabled_ && environmentResources_.valid()
            && carRenderer_.setEnvironmentMaps(
                environmentResources_.diffuseCubemap,
                environmentResources_.specularCubemap, &error);
        if (!uploaded) {
            carRenderer_.clearEnvironmentMaps();
            if (gameEnvironmentEnabled_ && error.isEmpty()) {
                error = environmentResources_.error;
            }
            if (gameEnvironmentEnabled_ && !error.isEmpty()) {
                qWarning().noquote() << "Garage environment fallback:" << error;
            }
        }
        environmentSourceLabel_ = uploaded
            ? QStringLiteral("Game probes")
            : QStringLiteral("Analytic env");
        environmentUploadPending_ = false;
        updateReferenceNote();
    }

    if (modelUploadPending_ && carRenderer_.isInitialized()) {
        carRenderer_.uploadModel(model_);
        modelUploadPending_ = false;
        fitCameraToModel();
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
                        QVector<fh6::Project> clusterProjects;
                        QVector<QRect> clusterClips;
                        collect(cluster, clusterProjects, clusterClips);
                        tex = shapeRenderer_.renderScenesToTexture(
                            clusterProjects, clusterClips, geometry_, liveryWorldToScreen(paintTextureSize),
                            paintTextureSize, /*preserveExisting=*/true);
                    }
                }
                if (tex == 0) {
                    QVector<fh6::Project> sectionProjects;
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

    const QSize framebufferSize = physicalFramebufferSize();

    if (!carRenderer_.hasModel()) {
        functions->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        functions->glViewport(0, 0, framebufferSize.width(), framebufferSize.height());
        clearRenderTarget(false);
        return;
    }
    if (renderSettings_.postProcessing.hdrEnabled && renderCarHdr(liveryTexture, framebufferSize)) {
        return;
    }
    functions->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    functions->glViewport(0, 0, framebufferSize.width(), framebufferSize.height());
    clearRenderTarget(false);
    renderGround(false);
    renderCar(liveryTexture, false);
}

void CarPreviewWidget::setGameFolder(const QString &folder) {
    if (folder == gameFolder_) {
        return;
    }
    gameFolder_ = folder;
    const quint64 generation = ++paintFinishLoadGeneration_;
    if (folder.isEmpty()) {
        paintFinishes_.clear();
        environmentResources_ = {};
        colorLut_ = {};
        colorLutError_.clear();
        environmentUploadPending_ = true;
        colorLutUploadPending_ = true;
        update();
        return;
    }
    QPointer<CarPreviewWidget> guard(this);
    QThreadPool::globalInstance()->start([guard, folder, generation]() {
        QString colorLutError;
        auto library = std::make_shared<fh6::PaintFinishLibrary>();
        library->load(folder);
        auto environment = std::make_shared<fh6::GarageEnvironmentResources>(
            fh6::loadGarageEnvironmentResources(folder));
        auto colorLut = std::make_shared<std::optional<fh6::GarageColorLut>>(
            fh6::loadGarageColorLut(folder, &colorLutError));
        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(
            guard,
            [guard, library = std::move(library), environment = std::move(environment),
             colorLut = std::move(colorLut), colorLutError = std::move(colorLutError),
             generation]() mutable {
                if (!guard || guard->paintFinishLoadGeneration_ != generation) {
                    return;
                }
                guard->paintFinishes_.replace(std::move(*library));
                guard->environmentResources_ = std::move(*environment);
                guard->colorLut_ = colorLut->has_value()
                    ? std::move(colorLut->value())
                    : fh6::GarageColorLut{};
                guard->colorLutError_ = std::move(colorLutError);
                guard->environmentUploadPending_ = true;
                guard->colorLutUploadPending_ = true;
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
    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const float nearPlane = std::max(0.01f, modelRadius_ * 0.02f);
    const float farPlane = modelRadius_ * 8.0f + distance_ * 2.0f;
    QMatrix4x4 projection;
    projection.perspective(renderSettings_.camera.fovDegrees, aspect, nearPlane, farPlane);
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

QSize CarPreviewWidget::physicalFramebufferSize() const {
    const qreal dpr = devicePixelRatioF();

    return QSize(
        std::max(1, static_cast<int>(std::lround(width() * dpr))),
        std::max(1, static_cast<int>(std::lround(height() * dpr))));
}

void CarPreviewWidget::fitCameraToModel() {
    const fh6::ModelVec3 &mn = model_.boundsMin;
    const fh6::ModelVec3 &mx = model_.boundsMax;
    target_ = QVector3D(-(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
    const QVector3D extent(mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
    modelRadius_ = std::max(0.001f, 0.5f * extent.length());
    resetComparisonCamera();
}

void CarPreviewWidget::resetComparisonCamera() {
    distance_ = modelRadius_ * renderSettings_.camera.distanceRadiusScale;
    yaw_ = renderSettings_.camera.yawRadians;
    pitch_ = renderSettings_.camera.pitchRadians;
}

void CarPreviewWidget::setLightDirectionCandidate(LightDirectionCandidate candidate) {
    if (renderSettings_.lighting.directionCandidate == candidate) {
        return;
    }
    renderSettings_.lighting.directionCandidate = candidate;
    updateReferenceNote();
    update();
}

void CarPreviewWidget::setGameEnvironmentEnabled(bool enabled) {
    if (gameEnvironmentEnabled_ == enabled) {
        return;
    }
    gameEnvironmentEnabled_ = enabled;
    environmentUploadPending_ = true;
    update();
}

void CarPreviewWidget::updateReferenceNote() {
    if (referenceNote_ == nullptr) {
        return;
    }
    QString candidate;
    switch (renderSettings_.lighting.directionCandidate) {
    case LightDirectionCandidate::OppositeXmlDirection:
        candidate = QStringLiteral("A");
        break;
    case LightDirectionCandidate::XmlDirection:
        candidate = QStringLiteral("B");
        break;
    case LightDirectionCandidate::BothDirections:
        candidate = QStringLiteral("A+B");
        break;
    }
    referenceNote_->setText(QStringLiteral(
        "Only for reference, ingame render may differ · Light %1 · %2")
                                .arg(candidate, environmentSourceLabel_));
    referenceNote_->adjustSize();
}

void CarPreviewWidget::logGlCapabilities() const {
    static const bool enabled = qEnvironmentVariableIsSet("FLS_GL_DIAG");
    QOpenGLContext *glContext = context();
    if (!enabled || glContext == nullptr) {
        return;
    }
    QOpenGLFunctions *functions = glContext->functions();
    const QSurfaceFormat actualFormat = glContext->format();
    const QSize framebufferSize(
        std::max(1, static_cast<int>(std::lround(width() * devicePixelRatioF()))),
        std::max(1, static_cast<int>(std::lround(height() * devicePixelRatioF()))));
    GLint maxTextureUnits = 0;
    GLint maxCombinedTextureUnits = 0;
    GLint maxCubeMapSize = 0;
    GLint max3dTextureSize = 0;
    GLint maxSamples = 0;

    functions->glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
    functions->glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxCombinedTextureUnits);
    functions->glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &maxCubeMapSize);
    functions->glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max3dTextureSize);
    functions->glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);

    qDebug().nospace()
        << "[car-preview-gl] version=" << actualFormat.majorVersion() << '.' << actualFormat.minorVersion()
        << " profile=" << static_cast<int>(actualFormat.profile())
        << " samples=" << actualFormat.samples()
        << " maxSamples=" << maxSamples
        << " depth=" << actualFormat.depthBufferSize()
        << " alpha=" << actualFormat.alphaBufferSize()
        << " framebuffer=" << framebufferSize.width() << 'x' << framebufferSize.height()
        << " textureUnits=" << maxTextureUnits
        << " combinedTextureUnits=" << maxCombinedTextureUnits
        << " maxCubeMap=" << maxCubeMapSize
        << " max3dTexture=" << max3dTextureSize;
}

void CarPreviewWidget::initializePostProcessing() {
    if (postProcessInitialized_) {
        return;
    }
    const auto buildProgram = [](QOpenGLShaderProgram &program, const char *name,
                                 const char *fragmentShader) {
        if (program.addShaderFromSourceCode(QOpenGLShader::Vertex, kPostProcessVertexShader)
            && program.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)
            && program.link()) {
            return true;
        }
        qWarning().noquote()
            << QStringLiteral("Car preview %1 shader failed to build:").arg(QString::fromLatin1(name))
            << program.log();

        return false;
    };
    const bool programsBuilt =
        buildProgram(bloomExtractProgram_, "bloom extract", kBloomExtractFragmentShader)
        && buildProgram(bloomBlurProgram_, "bloom blur", kBloomBlurFragmentShader)
        && buildProgram(bloomCompositeProgram_, "bloom composite", kBloomCompositeFragmentShader)
        && buildProgram(colorGradeProgram_, "colour grade", kColorGradeFragmentShader)
        && buildProgram(postProcessProgram_, "display output", kPostProcessFragmentShader);
    if (!programsBuilt) {
        bloomExtractProgram_.removeAllShaders();
        bloomBlurProgram_.removeAllShaders();
        bloomCompositeProgram_.removeAllShaders();
        colorGradeProgram_.removeAllShaders();
        postProcessProgram_.removeAllShaders();
        return;
    }
    if (!postProcessVao_.create()) {
        qWarning() << "Car preview post-process VAO failed to initialize";
        bloomExtractProgram_.removeAllShaders();
        bloomBlurProgram_.removeAllShaders();
        bloomCompositeProgram_.removeAllShaders();
        colorGradeProgram_.removeAllShaders();
        postProcessProgram_.removeAllShaders();
        return;
    }
    bloomSceneTextureLocation_ = bloomExtractProgram_.uniformLocation("scene_texture");
    bloomExposureLocation_ = bloomExtractProgram_.uniformLocation("exposure");
    bloomCutoffLocation_ = bloomExtractProgram_.uniformLocation("bloom_cutoff");
    bloomBlurTextureLocation_ = bloomBlurProgram_.uniformLocation("source_texture");
    bloomBlurTexelStepLocation_ = bloomBlurProgram_.uniformLocation("texel_step");
    bloomCompositeSceneLocation_ = bloomCompositeProgram_.uniformLocation("scene_texture");
    bloomCompositeTextureLocation_ = bloomCompositeProgram_.uniformLocation("bloom_texture");
    bloomCompositeExposureLocation_ = bloomCompositeProgram_.uniformLocation("exposure");
    bloomCompositeScaleLocation_ = bloomCompositeProgram_.uniformLocation("bloom_scale");
    bloomCompositeEnabledLocation_ = bloomCompositeProgram_.uniformLocation("bloom_enabled");
    colorGradeSceneLocation_ = colorGradeProgram_.uniformLocation("scene_texture");
    colorGradeLutLocation_ = colorGradeProgram_.uniformLocation("color_lut");
    colorGradeDimensionLocation_ = colorGradeProgram_.uniformLocation("lut_dimension");
    colorGradeScaleLocation_ = colorGradeProgram_.uniformLocation("lut_scale");
    colorGradeEnabledLocation_ = colorGradeProgram_.uniformLocation("lut_enabled");
    postSceneTextureLocation_ = postProcessProgram_.uniformLocation("scene_texture");
    postFilmicWhiteLocation_ = postProcessProgram_.uniformLocation("filmic_white");
    postProcessInitialized_ = true;
}

void CarPreviewWidget::releasePostProcessing() {
    releaseHdrFramebuffers();
    if (colorLutTexture_ != 0 && context() != nullptr) {
        context()->functions()->glDeleteTextures(1, &colorLutTexture_);
    }
    colorLutTexture_ = 0;
    if (postProcessVao_.isCreated()) {
        postProcessVao_.destroy();
    }
    bloomExtractProgram_.removeAllShaders();
    bloomBlurProgram_.removeAllShaders();
    bloomCompositeProgram_.removeAllShaders();
    colorGradeProgram_.removeAllShaders();
    postProcessProgram_.removeAllShaders();
    bloomSceneTextureLocation_ = -1;
    bloomExposureLocation_ = -1;
    bloomCutoffLocation_ = -1;
    bloomBlurTextureLocation_ = -1;
    bloomBlurTexelStepLocation_ = -1;
    bloomCompositeSceneLocation_ = -1;
    bloomCompositeTextureLocation_ = -1;
    bloomCompositeExposureLocation_ = -1;
    bloomCompositeScaleLocation_ = -1;
    bloomCompositeEnabledLocation_ = -1;
    colorGradeSceneLocation_ = -1;
    colorGradeLutLocation_ = -1;
    colorGradeDimensionLocation_ = -1;
    colorGradeScaleLocation_ = -1;
    colorGradeEnabledLocation_ = -1;
    postSceneTextureLocation_ = -1;
    postFilmicWhiteLocation_ = -1;
    postProcessInitialized_ = false;
}

void CarPreviewWidget::releaseHdrFramebuffers() {
    QOpenGLContext *glContext = context();
    if (glContext != nullptr) {
        QOpenGLExtraFunctions *functions = glContext->extraFunctions();
        functions->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        if (hdrSceneColor_ != 0) {
            functions->glDeleteRenderbuffers(1, &hdrSceneColor_);
        }
        if (hdrSceneDepth_ != 0) {
            functions->glDeleteRenderbuffers(1, &hdrSceneDepth_);
        }
        if (hdrSceneFramebuffer_ != 0) {
            functions->glDeleteFramebuffers(1, &hdrSceneFramebuffer_);
        }
        if (hdrResolveTexture_ != 0) {
            functions->glDeleteTextures(1, &hdrResolveTexture_);
        }
        if (hdrResolveFramebuffer_ != 0) {
            functions->glDeleteFramebuffers(1, &hdrResolveFramebuffer_);
        }
        if (hdrCompositeTexture_ != 0) {
            functions->glDeleteTextures(1, &hdrCompositeTexture_);
        }
        if (hdrCompositeFramebuffer_ != 0) {
            functions->glDeleteFramebuffers(1, &hdrCompositeFramebuffer_);
        }
        if (hdrGradeTexture_ != 0) {
            functions->glDeleteTextures(1, &hdrGradeTexture_);
        }
        if (hdrGradeFramebuffer_ != 0) {
            functions->glDeleteFramebuffers(1, &hdrGradeFramebuffer_);
        }
        functions->glDeleteTextures(
            static_cast<GLsizei>(bloomTextures_.size()), bloomTextures_.data());
        functions->glDeleteFramebuffers(
            static_cast<GLsizei>(bloomFramebuffers_.size()), bloomFramebuffers_.data());
    }
    hdrFramebufferSize_ = {};
    hdrSceneFramebuffer_ = 0;
    hdrSceneColor_ = 0;
    hdrSceneDepth_ = 0;
    hdrResolveFramebuffer_ = 0;
    hdrResolveTexture_ = 0;
    hdrCompositeFramebuffer_ = 0;
    hdrCompositeTexture_ = 0;
    hdrGradeFramebuffer_ = 0;
    hdrGradeTexture_ = 0;
    bloomFramebuffers_ = {};
    bloomTextures_ = {};
    hdrSampleCount_ = 0;
}

bool CarPreviewWidget::uploadColorLut() {
    QOpenGLContext *glContext = context();
    if (glContext == nullptr) {
        return false;
    }
    QOpenGLExtraFunctions *functions = glContext->extraFunctions();
    GLint activeTexture = 0;
    GLint maximumSize = 0;
    GLint textureBinding = 0;

    functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    functions->glActiveTexture(GL_TEXTURE1);
    functions->glGetIntegerv(GL_TEXTURE_BINDING_3D, &textureBinding);
    const GLuint previousColorLut = colorLutTexture_;
    if (colorLutTexture_ != 0) {
        functions->glDeleteTextures(1, &colorLutTexture_);
        colorLutTexture_ = 0;
        if (textureBinding == static_cast<GLint>(previousColorLut)) {
            textureBinding = 0;
        }
    }
    const auto restoreBinding = [&]() {
        functions->glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(textureBinding));
        functions->glActiveTexture(static_cast<GLenum>(activeTexture));
    };
    if (!colorLut_.valid()) {
        restoreBinding();
        return false;
    }
    functions->glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &maximumSize);
    if (colorLut_.dimension > maximumSize) {
        colorLutError_ = QStringLiteral("LUT dimension %1 exceeds GL limit %2")
                             .arg(colorLut_.dimension)
                             .arg(maximumSize);
        restoreBinding();
        return false;
    }

    while (functions->glGetError() != GL_NO_ERROR) {
    }
    functions->glGenTextures(1, &colorLutTexture_);
    functions->glBindTexture(GL_TEXTURE_3D, colorLutTexture_);
    functions->glTexImage3D(
        GL_TEXTURE_3D, 0, GL_RGBA16F,
        colorLut_.dimension, colorLut_.dimension, colorLut_.dimension,
        0, GL_RGBA, GL_FLOAT, colorLut_.rgba.data());
    functions->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    const GLenum error = functions->glGetError();
    restoreBinding();
    if (error != GL_NO_ERROR) {
        functions->glDeleteTextures(1, &colorLutTexture_);
        colorLutTexture_ = 0;
        colorLutError_ = QStringLiteral("OpenGL LUT upload failed with error 0x%1").arg(error, 0, 16);
        return false;
    }
    colorLutError_.clear();

    return true;
}

bool CarPreviewWidget::ensureHdrFramebuffers(const QSize &size) {
    QOpenGLContext *glContext = context();
    if (glContext == nullptr || size.isEmpty()) {
        return false;
    }
    QOpenGLExtraFunctions *functions = glContext->extraFunctions();
    GLint drawFramebuffer = 0;
    GLint readFramebuffer = 0;
    GLint renderbuffer = 0;
    GLint textureBinding = 0;

    functions->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    functions->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    functions->glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
    const auto restoreBindings = [&]() {
        functions->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(textureBinding));
        functions->glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
        functions->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
        functions->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
    };
    GLint maximumSamples = 0;
    functions->glGetIntegerv(GL_MAX_SAMPLES, &maximumSamples);
    const int sampleCount = std::clamp(
        std::max(1, glContext->format().samples()), 1, std::max(1, maximumSamples));
    if (hdrSceneFramebuffer_ != 0 && hdrResolveFramebuffer_ != 0
        && hdrCompositeFramebuffer_ != 0 && hdrGradeFramebuffer_ != 0
        && bloomFramebuffers_[0] != 0 && bloomFramebuffers_[1] != 0
        && hdrFramebufferSize_ == size && hdrSampleCount_ == sampleCount) {
        return true;
    }

    releaseHdrFramebuffers();

    functions->glGenFramebuffers(1, &hdrSceneFramebuffer_);
    functions->glBindFramebuffer(GL_FRAMEBUFFER, hdrSceneFramebuffer_);
    functions->glGenRenderbuffers(1, &hdrSceneColor_);
    functions->glBindRenderbuffer(GL_RENDERBUFFER, hdrSceneColor_);
    functions->glRenderbufferStorageMultisample(
        GL_RENDERBUFFER, sampleCount, GL_RGBA16F, size.width(), size.height());
    functions->glFramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, hdrSceneColor_);
    functions->glGenRenderbuffers(1, &hdrSceneDepth_);
    functions->glBindRenderbuffer(GL_RENDERBUFFER, hdrSceneDepth_);
    functions->glRenderbufferStorageMultisample(
        GL_RENDERBUFFER, sampleCount, GL_DEPTH_COMPONENT24, size.width(), size.height());
    functions->glFramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, hdrSceneDepth_);
    const GLenum sceneStatus = functions->glCheckFramebufferStatus(GL_FRAMEBUFFER);

    functions->glGenFramebuffers(1, &hdrResolveFramebuffer_);
    functions->glBindFramebuffer(GL_FRAMEBUFFER, hdrResolveFramebuffer_);
    functions->glGenTextures(1, &hdrResolveTexture_);
    functions->glBindTexture(GL_TEXTURE_2D, hdrResolveTexture_);
    functions->glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA16F, size.width(), size.height(), 0,
        GL_RGBA, GL_FLOAT, nullptr);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrResolveTexture_, 0);
    const GLenum resolveStatus = functions->glCheckFramebufferStatus(GL_FRAMEBUFFER);

    const auto createColorTarget = [functions](const QSize &targetSize, GLuint &framebuffer,
                                                GLuint &texture) {
        functions->glGenFramebuffers(1, &framebuffer);
        functions->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        functions->glGenTextures(1, &texture);
        functions->glBindTexture(GL_TEXTURE_2D, texture);
        functions->glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA16F, targetSize.width(), targetSize.height(), 0,
            GL_RGBA, GL_FLOAT, nullptr);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        functions->glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

        return functions->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    };
    const float bloomResolutionScale = std::clamp(
        renderSettings_.postProcessing.bloomResolutionScale, 0.125f, 1.0f);
    const QSize bloomSize(
        std::max(1, static_cast<int>(std::lround(size.width() * bloomResolutionScale))),
        std::max(1, static_cast<int>(std::lround(size.height() * bloomResolutionScale))));
    const GLenum compositeStatus = createColorTarget(
        size, hdrCompositeFramebuffer_, hdrCompositeTexture_);
    const GLenum gradeStatus = createColorTarget(size, hdrGradeFramebuffer_, hdrGradeTexture_);
    const GLenum bloomStatus0 = createColorTarget(
        bloomSize, bloomFramebuffers_[0], bloomTextures_[0]);
    const GLenum bloomStatus1 = createColorTarget(
        bloomSize, bloomFramebuffers_[1], bloomTextures_[1]);

    if (sceneStatus != GL_FRAMEBUFFER_COMPLETE || resolveStatus != GL_FRAMEBUFFER_COMPLETE
        || compositeStatus != GL_FRAMEBUFFER_COMPLETE || gradeStatus != GL_FRAMEBUFFER_COMPLETE
        || bloomStatus0 != GL_FRAMEBUFFER_COMPLETE || bloomStatus1 != GL_FRAMEBUFFER_COMPLETE) {
        qWarning().noquote()
            << QStringLiteral(
                   "Car preview HDR framebuffer incomplete: scene=0x%1 resolve=0x%2 "
                   "composite=0x%3 grade=0x%4 bloomA=0x%5 bloomB=0x%6")
                   .arg(sceneStatus, 0, 16)
                   .arg(resolveStatus, 0, 16)
                   .arg(compositeStatus, 0, 16)
                   .arg(gradeStatus, 0, 16)
                   .arg(bloomStatus0, 0, 16)
                   .arg(bloomStatus1, 0, 16);
        releaseHdrFramebuffers();
        restoreBindings();
        return false;
    }
    hdrFramebufferSize_ = size;
    hdrSampleCount_ = sampleCount;
    restoreBindings();

    return true;
}

void CarPreviewWidget::clearRenderTarget(bool linearColor) const {
    QOpenGLFunctions *functions = context()->functions();
    functions->glDepthMask(GL_TRUE);
    if (transparentBackground_) {
        functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        const QVector3D authoredBackground = renderSettings_.environment.backgroundColor;
        const QVector3D background = linearColor
            ? srgbToLinear(authoredBackground)
            : authoredBackground;
        functions->glClearColor(background.x(), background.y(), background.z(), 1.0f);
    }
    functions->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void CarPreviewWidget::renderGround(bool linearOutput) {
    if (transparentBackground_ || !hasModel()) {
        return;
    }
    groundRenderer_.render(
        cameraView(), cameraProjection(), model_.boundsMin, model_.boundsMax,
        renderSettings_.ground, renderSettings_.environment.backgroundColor,
        linearOutput);
}

void CarPreviewWidget::renderCar(GLuint liveryTexture, bool linearOutput) {
    carRenderer_.render(
        cameraView(), cameraProjection(), liveryTexture, basePaint_,
        project_ != nullptr ? &project_->liveryPaint : nullptr,
        manufacturerColors_.colors.isEmpty() ? nullptr : &manufacturerColors_,
        paintFinishes_.loaded() ? &paintFinishes_ : nullptr,
        renderSettings_.lighting,
        renderSettings_.environment,
        linearOutput);
}

bool CarPreviewWidget::renderBloomExtract(const QSize &size) {
    QOpenGLExtraFunctions *functions = context()->extraFunctions();
    const float resolutionScale = std::clamp(
        renderSettings_.postProcessing.bloomResolutionScale, 0.125f, 1.0f);
    const QSize bloomSize(
        std::max(1, static_cast<int>(std::lround(size.width() * resolutionScale))),
        std::max(1, static_cast<int>(std::lround(size.height() * resolutionScale))));

    functions->glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[0]);
    functions->glViewport(0, 0, bloomSize.width(), bloomSize.height());
    if (!renderSettings_.postProcessing.bloomEnabled) {
        functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        functions->glClear(GL_COLOR_BUFFER_BIT);
        return true;
    }
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, hdrResolveTexture_);
    if (!bloomExtractProgram_.bind()) {
        return false;
    }
    bloomExtractProgram_.setUniformValue(bloomSceneTextureLocation_, 0);
    bloomExtractProgram_.setUniformValue(
        bloomExposureLocation_, renderSettings_.postProcessing.exposure);
    bloomExtractProgram_.setUniformValue(
        bloomCutoffLocation_,
        renderSettings_.postProcessing.bloomCutoff
            * garage_render_defaults::kBloomCapturedCutoffFactor);
    postProcessVao_.bind();
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);
    postProcessVao_.release();
    bloomExtractProgram_.release();

    return true;
}

bool CarPreviewWidget::renderBloomBlur(
    const QSize &size, int sourceIndex, int targetIndex, bool horizontal) {
    if (!renderSettings_.postProcessing.bloomEnabled) {
        return true;
    }
    QOpenGLExtraFunctions *functions = context()->extraFunctions();
    const float resolutionScale = std::clamp(
        renderSettings_.postProcessing.bloomResolutionScale, 0.125f, 1.0f);
    const QSize bloomSize(
        std::max(1, static_cast<int>(std::lround(size.width() * resolutionScale))),
        std::max(1, static_cast<int>(std::lround(size.height() * resolutionScale))));
    const QVector2D texelStep = horizontal
        ? QVector2D(1.0f / bloomSize.width(), 0.0f)
        : QVector2D(0.0f, 1.0f / bloomSize.height());

    functions->glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[targetIndex]);
    functions->glViewport(0, 0, bloomSize.width(), bloomSize.height());
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, bloomTextures_[sourceIndex]);
    if (!bloomBlurProgram_.bind()) {
        return false;
    }
    bloomBlurProgram_.setUniformValue(bloomBlurTextureLocation_, 0);
    bloomBlurProgram_.setUniformValue(bloomBlurTexelStepLocation_, texelStep);
    postProcessVao_.bind();
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);
    postProcessVao_.release();
    bloomBlurProgram_.release();

    return true;
}

bool CarPreviewWidget::renderBloomComposite(const QSize &size) {
    QOpenGLExtraFunctions *functions = context()->extraFunctions();

    functions->glBindFramebuffer(GL_FRAMEBUFFER, hdrCompositeFramebuffer_);
    functions->glViewport(0, 0, size.width(), size.height());
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, hdrResolveTexture_);
    functions->glActiveTexture(GL_TEXTURE1);
    functions->glBindTexture(GL_TEXTURE_2D, bloomTextures_[0]);
    if (!bloomCompositeProgram_.bind()) {
        return false;
    }
    bloomCompositeProgram_.setUniformValue(bloomCompositeSceneLocation_, 0);
    bloomCompositeProgram_.setUniformValue(bloomCompositeTextureLocation_, 1);
    bloomCompositeProgram_.setUniformValue(
        bloomCompositeExposureLocation_, renderSettings_.postProcessing.exposure);
    bloomCompositeProgram_.setUniformValue(
        bloomCompositeScaleLocation_,
        renderSettings_.postProcessing.bloomScale
            * garage_render_defaults::kBloomCapturedScaleFactor);
    bloomCompositeProgram_.setUniformValue(
        bloomCompositeEnabledLocation_, renderSettings_.postProcessing.bloomEnabled ? 1 : 0);
    postProcessVao_.bind();
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);
    postProcessVao_.release();
    bloomCompositeProgram_.release();

    return true;
}

bool CarPreviewWidget::renderColorGrade(const QSize &size) {
    QOpenGLExtraFunctions *functions = context()->extraFunctions();
    const bool colorGradeEnabled = renderSettings_.postProcessing.colorGradeEnabled
        && colorLutTexture_ != 0 && colorLut_.valid();

    functions->glBindFramebuffer(GL_FRAMEBUFFER, hdrGradeFramebuffer_);
    functions->glViewport(0, 0, size.width(), size.height());
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, hdrCompositeTexture_);
    functions->glActiveTexture(GL_TEXTURE1);
    functions->glBindTexture(GL_TEXTURE_3D, colorLutTexture_);
    if (!colorGradeProgram_.bind()) {
        return false;
    }
    colorGradeProgram_.setUniformValue(colorGradeSceneLocation_, 0);
    colorGradeProgram_.setUniformValue(colorGradeLutLocation_, 1);
    colorGradeProgram_.setUniformValue(
        colorGradeDimensionLocation_, static_cast<float>(std::max(1, colorLut_.dimension)));
    colorGradeProgram_.setUniformValue(
        colorGradeScaleLocation_, colorLut_.valid() ? colorLut_.scale : 1.0f);
    colorGradeProgram_.setUniformValue(colorGradeEnabledLocation_, colorGradeEnabled ? 1 : 0);
    postProcessVao_.bind();
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);
    postProcessVao_.release();
    colorGradeProgram_.release();

    return true;
}

bool CarPreviewWidget::renderDisplayOutput(const QSize &size) {
    QOpenGLExtraFunctions *functions = context()->extraFunctions();

    functions->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    functions->glViewport(0, 0, size.width(), size.height());
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, hdrGradeTexture_);
    if (!postProcessProgram_.bind()) {
        return false;
    }
    postProcessProgram_.setUniformValue(postSceneTextureLocation_, 0);
    postProcessProgram_.setUniformValue(
        postFilmicWhiteLocation_, renderSettings_.postProcessing.filmicWhite);
    postProcessVao_.bind();
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);
    postProcessVao_.release();
    postProcessProgram_.release();

    return true;
}

bool CarPreviewWidget::renderCarHdr(GLuint liveryTexture, const QSize &size) {
    if (!postProcessInitialized_ || !ensureHdrFramebuffers(size)) {
        return false;
    }
    QOpenGLExtraFunctions *functions = context()->extraFunctions();
    const GLboolean depthEnabled = functions->glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendEnabled = functions->glIsEnabled(GL_BLEND);
    const GLboolean cullEnabled = functions->glIsEnabled(GL_CULL_FACE);
    const GLboolean framebufferSrgbEnabled = functions->glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLboolean depthWriteEnabled = GL_TRUE;
    GLint activeTexture = 0;
    std::array<GLint, 2> texture2dBindings = {};
    std::array<GLint, 2> texture3dBindings = {};
    GLint currentProgram = 0;
    GLint vertexArray = 0;
    GLint drawFramebuffer = 0;
    GLint readFramebuffer = 0;
    GLint viewport[4] = {};

    functions->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteEnabled);
    functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    for (int unit = 0; unit < static_cast<int>(texture2dBindings.size()); ++unit) {
        functions->glActiveTexture(GL_TEXTURE0 + unit);
        functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2dBindings[unit]);
        functions->glGetIntegerv(GL_TEXTURE_BINDING_3D, &texture3dBindings[unit]);
    }
    functions->glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    functions->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
    functions->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
    functions->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    functions->glGetIntegerv(GL_VIEWPORT, viewport);
    const auto restoreCapability = [functions](GLenum capability, GLboolean enabled) {
        if (enabled == GL_TRUE) {
            functions->glEnable(capability);
        } else {
            functions->glDisable(capability);
        }
    };
    const auto restoreState = [&]() {
        for (int unit = 0; unit < static_cast<int>(texture2dBindings.size()); ++unit) {
            functions->glActiveTexture(GL_TEXTURE0 + unit);
            functions->glBindTexture(
                GL_TEXTURE_2D, static_cast<GLuint>(texture2dBindings[unit]));
            functions->glBindTexture(
                GL_TEXTURE_3D, static_cast<GLuint>(texture3dBindings[unit]));
        }
        functions->glActiveTexture(static_cast<GLenum>(activeTexture));
        functions->glUseProgram(static_cast<GLuint>(currentProgram));
        functions->glBindVertexArray(static_cast<GLuint>(vertexArray));
        functions->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
        functions->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
        functions->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        functions->glDepthMask(depthWriteEnabled);
        restoreCapability(GL_DEPTH_TEST, depthEnabled);
        restoreCapability(GL_BLEND, blendEnabled);
        restoreCapability(GL_CULL_FACE, cullEnabled);
        restoreCapability(GL_FRAMEBUFFER_SRGB, framebufferSrgbEnabled);
    };

    functions->glBindFramebuffer(GL_FRAMEBUFFER, hdrSceneFramebuffer_);
    functions->glViewport(0, 0, size.width(), size.height());
    clearRenderTarget(true);
    renderGround(true);
    renderCar(liveryTexture, true);

    functions->glBindFramebuffer(GL_READ_FRAMEBUFFER, hdrSceneFramebuffer_);
    functions->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hdrResolveFramebuffer_);
    functions->glBlitFramebuffer(
        0, 0, size.width(), size.height(),
        0, 0, size.width(), size.height(),
        GL_COLOR_BUFFER_BIT, GL_NEAREST);

    functions->glDisable(GL_DEPTH_TEST);
    functions->glDisable(GL_BLEND);
    functions->glDisable(GL_CULL_FACE);
    functions->glDisable(GL_FRAMEBUFFER_SRGB);
    bool stackRendered = true;
    for (PostProcessPass pass : kPostProcessOrder) {
        switch (pass) {
        case PostProcessPass::BloomExtract:
            stackRendered = renderBloomExtract(size);
            break;
        case PostProcessPass::BloomBlurHorizontal:
            stackRendered = renderBloomBlur(size, 0, 1, true);
            break;
        case PostProcessPass::BloomBlurVertical:
            stackRendered = renderBloomBlur(size, 1, 0, false);
            break;
        case PostProcessPass::BloomComposite:
            stackRendered = renderBloomComposite(size);
            break;
        case PostProcessPass::ColorGrade:
            stackRendered = renderColorGrade(size);
            break;
        case PostProcessPass::DisplayOutput:
            stackRendered = renderDisplayOutput(size);
            break;
        }
        if (!stackRendered) {
            break;
        }
    }

    restoreState();
    if (!stackRendered) {
        qWarning() << "Car preview post-process pass failed to bind";
        renderSettings_.postProcessing.hdrEnabled = false;
        return false;
    }

    return true;
}

void CarPreviewWidget::invalidateCachedLivery() {
    dirtySectionIds_.clear();
    projectedSectionCache_.clear();
    liveryDirty_ = true;
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
