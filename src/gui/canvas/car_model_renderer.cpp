#include "car_model_renderer.h"

#include "model_material.h"

#include <QHash>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace gui {
namespace {

constexpr char kVertexShader[] = R"(#version 330 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

uniform mat4 mvp;
uniform mat4 model;

out vec3 v_normal;
out vec3 v_world;
out vec2 v_uv;

void main()
{
    vec4 world = model * vec4(in_position, 1.0);
    gl_Position = mvp * vec4(in_position, 1.0);
    v_normal = mat3(model) * in_normal;
    v_world = world.xyz;
    v_uv = in_uv;
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
in vec3 v_normal;
in vec3 v_world;
in vec2 v_uv;

uniform sampler2D livery_tex;
uniform sampler2DArray side_masks;
uniform vec3 base_paint;
uniform vec3 secondary_paint;
uniform float secondary_mix;
uniform float material_gloss;
uniform float material_metallic;
uniform vec3 material_emissive;
uniform vec3 eye_position;
uniform int has_livery;
uniform int use_direct_uv;
uniform int side_count;
uniform vec4 side_axis[11];
uniform vec2 side_emin[11];
uniform vec2 side_emax[11];
uniform vec4 side_region[11];
uniform vec4 side_paint_region[11];
uniform vec3 side_facing[11];
uniform int debug_mode;
uniform int allowed_sides;
uniform float material_alpha;

out vec4 out_color;

vec3 sideColor(int s)
{
    if (s == 0) return vec3(1.0, 0.2, 0.2); // Front  red
    if (s == 1) return vec3(0.2, 1.0, 0.2); // Back   green
    if (s == 2) return vec3(0.3, 0.5, 1.0); // Top    blue
    if (s == 3) return vec3(1.0, 1.0, 0.2); // Left   yellow
    if (s == 4) return vec3(1.0, 0.3, 1.0); // Right  magenta
    if (s == 5) return vec3(0.2, 1.0, 1.0); // Spoiler cyan
    if (s == 6) return vec3(1.0, 0.5, 0.2); // Front glass
    if (s == 7) return vec3(0.5, 1.0, 0.5); // Back glass
    if (s == 8) return vec3(0.5, 0.8, 1.0); // Top glass
    if (s == 9) return vec3(1.0, 0.9, 0.4); // Left glass
    return vec3(1.0, 0.5, 1.0);             // Right glass
}

float axisComponent(vec3 v, float axis)
{
    return axis < 0.5 ? v.x : (axis < 1.5 ? v.y : v.z);
}

vec2 canvasToUv(vec2 c)
{
    return vec2((c.x + 1024.0) / 2048.0, (512.0 - c.y) / 1024.0);
}

void main()
{
    vec3 n = normalize(v_normal);
    vec3 viewDir = normalize(eye_position - v_world);
    float edge = pow(1.0 - max(dot(n, viewDir), 0.0), 2.0);
    vec3 surfacePaint = mix(base_paint, secondary_paint, secondary_mix * edge);
    vec3 albedo = surfacePaint;
    if (has_livery == 1 && side_count > 0) {
        if (use_direct_uv == 1) {
            vec2 atlasUv = vec2(v_uv.x * 0.5, v_uv.y);
            float coverage = 0.0;
            int coveredSide = -1;
            if (atlasUv.x >= 0.0 && atlasUv.x <= 1.0 && atlasUv.y >= 0.0 && atlasUv.y <= 1.0) {
                for (int s = 0; s < side_count; ++s) {
                    if ((allowed_sides & (1 << s)) == 0) {
                        continue;
                    }
                    float candidate = texture(side_masks, vec3(atlasUv, float(s))).r;
                    if (candidate > coverage) {
                        coverage = candidate;
                        coveredSide = s;
                    }
                }
            }
            vec2 paintUv = vec2(atlasUv.x, 1.0 - atlasUv.y);
            if (coveredSide >= 0) {
                vec4 sourceRegion = side_region[coveredSide];
                vec2 sourceStart = canvasToUv(vec2(sourceRegion.x, sourceRegion.z));
                vec2 sourceEnd = canvasToUv(vec2(sourceRegion.y, sourceRegion.w));
                vec2 sourceRange = sourceEnd - sourceStart;
                if (abs(sourceRange.x) > 0.000001 && abs(sourceRange.y) > 0.000001) {
                    vec2 sectionUv = (atlasUv - sourceStart) / sourceRange;
                    if (coveredSide == 5 || coveredSide == 7) {
                        sectionUv = sectionUv.yx;
                    }
                    if (coveredSide == 2 || coveredSide == 4 || coveredSide == 10) {
                        sectionUv.y = 1.0 - sectionUv.y;
                    } else {
                        sectionUv.x = 1.0 - sectionUv.x;
                    }
                    if (coveredSide == 7) {
                        sectionUv.y = 1.0 - sectionUv.y;
                    }
                    vec4 paintRegion = side_paint_region[coveredSide];
                    paintUv = vec2(mix(paintRegion.x, paintRegion.y, sectionUv.x),
                                   mix(paintRegion.z, paintRegion.w, sectionUv.y));
                }
            }
            if (debug_mode == 1 && coveredSide >= 0) {
                out_color = vec4(paintUv, 0.0, 1.0);
                return;
            }
            if (debug_mode == 2 && coveredSide >= 0) {
                out_color = vec4(sideColor(coveredSide), 1.0);
                return;
            }
            if (coverage > 0.5) {
                vec4 paint = texture(livery_tex, paintUv);
                albedo = surfacePaint * (1.0 - paint.a) + paint.rgb;
            }
        } else {
        vec3 n = normalize(v_normal);
        float bestScore = -1.0;
        float bestCoverage = 0.0;
        vec2 bestUv = vec2(0.0);
        int bestSide = -1;
        for (int s = 0; s < side_count; ++s) {
            if ((allowed_sides & (1 << s)) == 0) {
                continue;
            }
            float facing = dot(side_facing[s], n);
            if (facing <= 0.0) {
                continue;
            }
            vec4 ax = side_axis[s];
            vec2 av = vec2(axisComponent(v_world, ax.x) * ax.z,
                           axisComponent(v_world, ax.y) * ax.w);
            vec2 range = side_emax[s] - side_emin[s];
            if (range.x <= 0.0 || range.y <= 0.0) {
                continue;
            }
            vec2 nrm = (av - side_emin[s]) / range;
            vec4 reg = side_region[s];
            vec2 maskCanvas = vec2(mix(reg.x, reg.y, nrm.x), mix(reg.z, reg.w, nrm.y));
            vec2 maskUv = canvasToUv(maskCanvas);
            if (maskUv.x < 0.0 || maskUv.x > 1.0 || maskUv.y < 0.0 || maskUv.y > 1.0) {
                continue;
            }
            float coverage = texture(side_masks, vec3(maskUv, float(s))).r;
            if (coverage <= 0.5) {
                continue;
            }
            vec4 paintRegion = side_paint_region[s];
            vec2 sectionUv = nrm;
            if (s == 5 || s == 7) {
                sectionUv = sectionUv.yx;
            }
            sectionUv = (s == 2 || s == 4 || s == 10)
                ? vec2(sectionUv.x, 1.0 - sectionUv.y)
                : vec2(1.0 - sectionUv.x, sectionUv.y);
            if (s == 7) {
                sectionUv.y = 1.0 - sectionUv.y;
            }
            vec2 paintUv = vec2(mix(paintRegion.x, paintRegion.y, sectionUv.x),
                                mix(paintRegion.z, paintRegion.w, sectionUv.y));
            if (facing >= bestScore) {
                bestScore = facing;
                bestCoverage = coverage;
                bestUv = paintUv;
                bestSide = s;
            }
        }
        if (debug_mode == 1 && bestSide >= 0) {
            out_color = vec4(bestUv, 0.0, 1.0);
            return;
        }
        if (debug_mode == 2 && bestSide >= 0) {
            out_color = vec4(sideColor(bestSide), 1.0);
            return;
        }
        if (bestCoverage > 0.5) {
            vec4 paint = texture(livery_tex, bestUv);
            albedo = surfacePaint * (1.0 - paint.a) + paint.rgb;
        }
        }
    }
    vec3 l = normalize(vec3(0.4, 0.8, 0.6));
    vec3 h = normalize(l + viewDir);
    float ambient = 0.35;
    float diffuse = max(dot(n, l), 0.0);
    float shininess = mix(8.0, 128.0, material_gloss);
    float specular = pow(max(dot(n, h), 0.0), shininess);
    float specularStrength = mix(0.08, 0.62, material_metallic);
    vec3 specularColor = mix(vec3(1.0), albedo, material_metallic);
    vec3 lit = albedo * (ambient + (1.0 - ambient) * diffuse)
        + specularColor * specular * specularStrength
        + material_emissive;
    out_color = vec4(lit, material_alpha);
}
)";

constexpr int kMaskTextureScale = 2;

bool isBodyPaintMaterial(const QString &material)
{
    const QString name = material.toLower();
    if (name.isEmpty() || name.contains(QStringLiteral("caliper")) || name.contains(QStringLiteral("texture"))) {
        return false;
    }
    return name.startsWith(QStringLiteral("carpaint")) || name.startsWith(QStringLiteral("car_paint"));
}

bool isWindowGlassMaterial(const fh6::CarMesh &mesh)
{
    const QString name = mesh.materialName.toLower();
    if (name.isEmpty()
        || name.contains(QStringLiteral("screw"))
        || name.contains(QStringLiteral("frame"))
        || name.contains(QStringLiteral("label"))
        || name.contains(QStringLiteral("bulb"))
        || name.contains(QStringLiteral("light"))) {
        return false;
    }
    QString resource;
    if (mesh.material) {
        resource = mesh.material->resourcePath.toLower();
        resource.replace(QLatin1Char('\\'), QLatin1Char('/'));
    }
    const bool glassResource = resource.isEmpty()
        || resource.contains(QStringLiteral("/glass/"));
    return name.contains(QStringLiteral("window"))
        || name.contains(QStringLiteral("windshield"))
        || name.contains(QStringLiteral("windsheild"))
        || (name.contains(QStringLiteral("blackglass")) && glassResource);
}

bool isInteriorWindowShell(const QString &rawName)
{
    QString name = rawName.toLower();
    const int pipe = name.indexOf(QLatin1Char('|'));
    if (pipe >= 0) {
        name = name.left(pipe);
    }
    return name.startsWith(QStringLiteral("glass")) && name.contains(QStringLiteral("int"));
}

QString materialIdentity(const fh6::CarMesh &mesh)
{
    QString identity = mesh.materialName.toLower();
    if (mesh.material) {
        identity += QLatin1Char('|') + mesh.material->name.toLower();
        identity += QLatin1Char('|') + mesh.material->resourcePath.toLower();
    }
    identity.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return identity;
}

QString materialResourceIdentity(const fh6::CarMesh &mesh)
{
    if (!mesh.material) {
        return {};
    }
    QString resource = mesh.material->resourcePath.toLower();
    resource.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return resource;
}

bool isLampSurface(const fh6::CarMesh &mesh)
{
    const QString name = mesh.name.toLower();
    const QString material = materialIdentity(mesh);
    const bool lampGlass = name.startsWith(QStringLiteral("glass"))
        && (name.contains(QStringLiteral("hl_"))
            || name.contains(QStringLiteral("tl_"))
            || name.contains(QStringLiteral("chmsl")));
    return name.contains(QStringLiteral("headlight"))
        || name.contains(QStringLiteral("headlamp"))
        || name.contains(QStringLiteral("taillight"))
        || name.contains(QStringLiteral("taillamp"))
        || name.contains(QStringLiteral("sidemarker"))
        || lampGlass
        || material.contains(QStringLiteral("/lamp/"));
}

bool isLampEmitterMaterial(const fh6::CarMesh &mesh)
{
    const QString material = materialIdentity(mesh);
    return material.contains(QStringLiteral("lights"))
        || material.contains(QStringLiteral("lightbulb"))
        || material.contains(QStringLiteral("emitter"));
}

struct MaterialFallback {
    QVector3D color;
    float gloss = 0.45f;
    float metallic = 0.0f;
};

std::optional<MaterialFallback> exteriorMaterialFallback(const fh6::CarMesh &mesh)
{
    const QString resource = materialResourceIdentity(mesh);
    const QString material = resource.isEmpty() ? materialIdentity(mesh) : resource;
    if (material.contains(QStringLiteral("mirror_left"))
        || material.contains(QStringLiteral("mirror_right"))) {
        return MaterialFallback{QVector3D(0.42f, 0.48f, 0.56f), 0.98f, 0.82f};
    }
    if (material.contains(QStringLiteral("chrome"))) {
        return MaterialFallback{QVector3D(0.68f, 0.71f, 0.76f), 0.96f, 1.0f};
    }
    if (material.contains(QStringLiteral("gold"))) {
        return MaterialFallback{QVector3D(0.64f, 0.43f, 0.10f), 0.86f, 0.92f};
    }
    if (material.contains(QStringLiteral("aluminum"))) {
        return MaterialFallback{QVector3D(0.42f, 0.44f, 0.46f), 0.62f, 0.82f};
    }
    if (material.contains(QStringLiteral("titanium"))) {
        return MaterialFallback{QVector3D(0.30f, 0.31f, 0.34f), 0.68f, 0.88f};
    }
    if (material.contains(QStringLiteral("gunmetal"))
        || material.contains(QStringLiteral("anodizedmetal"))) {
        return MaterialFallback{QVector3D(0.10f, 0.11f, 0.13f), 0.72f, 0.86f};
    }
    if (material.contains(QStringLiteral("steel"))
        || material.contains(QStringLiteral("metallic"))) {
        return MaterialFallback{QVector3D(0.28f, 0.30f, 0.33f), 0.70f, 0.84f};
    }
    if (material.contains(QStringLiteral("paintedmetal"))) {
        return MaterialFallback{QVector3D(0.055f, 0.058f, 0.064f), 0.58f, 0.45f};
    }
    if (material.contains(QStringLiteral("rubber"))) {
        return MaterialFallback{QVector3D(0.018f, 0.019f, 0.021f), 0.18f, 0.0f};
    }
    if (material.contains(QStringLiteral("blackframe"))
        || material.contains(QStringLiteral("blackhole"))
        || material.contains(QStringLiteral("grille"))) {
        return MaterialFallback{QVector3D(0.008f, 0.009f, 0.011f), 0.22f, 0.05f};
    }
    if (material.contains(QStringLiteral("plastic"))) {
        return MaterialFallback{QVector3D(0.025f, 0.027f, 0.030f), 0.30f, 0.0f};
    }
    if (material.contains(QStringLiteral("badge"))) {
        return MaterialFallback{QVector3D(0.46f, 0.49f, 0.53f), 0.86f, 0.78f};
    }
    if (material.contains(QStringLiteral("plate"))) {
        return MaterialFallback{QVector3D(0.78f, 0.79f, 0.75f), 0.35f, 0.0f};
    }
    return std::nullopt;
}

} // namespace

CarModelRenderer::CarModelRenderer() = default;

CarModelRenderer::~CarModelRenderer()
{
    release();
}

void CarModelRenderer::initialize()
{
    if (initialized_) {
        return;
    }
    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        || !program_.link()) {
        return;
    }
    mvpLocation_ = program_.uniformLocation("mvp");
    modelLocation_ = program_.uniformLocation("model");
    liveryTexLocation_ = program_.uniformLocation("livery_tex");
    basePaintLocation_ = program_.uniformLocation("base_paint");
    hasLiveryLocation_ = program_.uniformLocation("has_livery");
    useDirectUvLocation_ = program_.uniformLocation("use_direct_uv");
    sideMasksLocation_ = program_.uniformLocation("side_masks");
    sideCountLocation_ = program_.uniformLocation("side_count");
    sideAxisLocation_ = program_.uniformLocation("side_axis");
    sideEMinLocation_ = program_.uniformLocation("side_emin");
    sideEMaxLocation_ = program_.uniformLocation("side_emax");
    sideRegionLocation_ = program_.uniformLocation("side_region");
    sidePaintRegionLocation_ = program_.uniformLocation("side_paint_region");
    sideFacingLocation_ = program_.uniformLocation("side_facing");
    debugModeLocation_ = program_.uniformLocation("debug_mode");
    allowedSidesLocation_ = program_.uniformLocation("allowed_sides");
    materialAlphaLocation_ = program_.uniformLocation("material_alpha");
    secondaryPaintLocation_ = program_.uniformLocation("secondary_paint");
    secondaryMixLocation_ = program_.uniformLocation("secondary_mix");
    glossLocation_ = program_.uniformLocation("material_gloss");
    metallicLocation_ = program_.uniformLocation("material_metallic");
    emissiveLocation_ = program_.uniformLocation("material_emissive");
    eyePositionLocation_ = program_.uniformLocation("eye_position");
    initialized_ = true;
}

void CarModelRenderer::release()
{
    clearModel();
    clearLivery();
    program_.removeAllShaders();
    initialized_ = false;
    mvpLocation_ = -1;
    modelLocation_ = -1;
    liveryTexLocation_ = -1;
    basePaintLocation_ = -1;
    hasLiveryLocation_ = -1;
    useDirectUvLocation_ = -1;
    sideMasksLocation_ = -1;
    sideCountLocation_ = -1;
    sideAxisLocation_ = -1;
    sideEMinLocation_ = -1;
    sideEMaxLocation_ = -1;
    sideRegionLocation_ = -1;
    sidePaintRegionLocation_ = -1;
    sideFacingLocation_ = -1;
    debugModeLocation_ = -1;
    allowedSidesLocation_ = -1;
    materialAlphaLocation_ = -1;
    secondaryPaintLocation_ = -1;
    secondaryMixLocation_ = -1;
    glossLocation_ = -1;
    metallicLocation_ = -1;
    emissiveLocation_ = -1;
    eyePositionLocation_ = -1;
}

bool CarModelRenderer::isInitialized() const
{
    return initialized_;
}

bool CarModelRenderer::hasModel() const
{
    return !meshes_.empty();
}

void CarModelRenderer::clearModel()
{
    for (auto &mesh : meshes_) {
        if (mesh->vbo.isCreated()) {
            mesh->vbo.destroy();
        }
        if (mesh->ibo.isCreated()) {
            mesh->ibo.destroy();
        }
        if (mesh->vao.isCreated()) {
            mesh->vao.destroy();
        }
    }
    meshes_.clear();
}

void CarModelRenderer::clearLivery()
{
    if (sideMaskArray_ != 0) {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (context != nullptr) {
            context->functions()->glDeleteTextures(1, &sideMaskArray_);
        }
        sideMaskArray_ = 0;
    }
    sideCount_ = 0;
    sideAxis_.clear();
    sideEMin_.clear();
    sideEMax_.clear();
    sideRegion_.clear();
    defaultSidePaintRegion_.clear();
    sidePaintRegion_.clear();
    sideFacing_.clear();
}

namespace {

// Model space uses +X right, +Y up, and +Z front.
const QVector3D kFacing[fh6::kLiverySideCount] = {
    QVector3D(0.0f, 0.0f, 1.0f),  // Front (+Z)
    QVector3D(0.0f, 0.0f, -1.0f), // Back  (-Z)
    QVector3D(0.0f, 1.0f, 0.0f),  // Top
    QVector3D(-1.0f, 0.0f, 0.0f), // Left
    QVector3D(1.0f, 0.0f, 0.0f),  // Right
    QVector3D(0.0f, 1.0f, 0.0f),  // Spoiler
    QVector3D(0.0f, 0.0f, 1.0f),  // Front windshield
    QVector3D(0.0f, 0.0f, -1.0f), // Back windshield
    QVector3D(0.0f, 1.0f, 0.0f),  // Top window
    QVector3D(-1.0f, 0.0f, 0.0f), // Left window
    QVector3D(1.0f, 0.0f, 0.0f),  // Right window
};

float axisOf(const fh6::ModelVec3 &v, int axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

constexpr int kStandaloneLodRank = 500;

int lodRank(const QString &name)
{
    const int idx = name.lastIndexOf(QStringLiteral("_LOD"));
    if (idx < 0) {
        return kStandaloneLodRank;
    }
    const QString suffix = name.mid(idx + 4);
    if (suffix.startsWith(QLatin1Char('S')) || suffix.startsWith(QLatin1Char('s'))) {
        bool ok = false;
        const int n = suffix.mid(1).toInt(&ok);
        return 1000 - (ok ? n : 0);
    }
    bool ok = false;
    const int n = suffix.toInt(&ok);
    return ok ? 100 - n : kStandaloneLodRank;
}

QString lodBase(const QString &name)
{
    const int idx = name.lastIndexOf(QStringLiteral("_LOD"));
    return idx < 0 ? name : name.left(idx);
}

QString lodGroup(const fh6::CarMesh &mesh)
{
    const QString base = lodBase(mesh.name);
    return mesh.modelInstanceId >= 0
        ? QString::number(mesh.modelInstanceId) + QLatin1Char('|') + base
        : base;
}

std::vector<char> highestLodFlags(const std::vector<fh6::CarMesh> &meshes)
{
    QHash<QString, int> best;
    for (const fh6::CarMesh &mesh : meshes) {
        const int rank = lodRank(mesh.name);
        const QString group = lodGroup(mesh);
        auto it = best.find(group);
        if (it == best.end() || rank > it.value()) {
            best.insert(group, rank);
        }
    }
    std::vector<char> keep(meshes.size(), 0);
    for (size_t i = 0; i < meshes.size(); ++i) {
        keep[i] = lodRank(meshes[i].name) == best.value(lodGroup(meshes[i])) ? 1 : 0;
    }
    return keep;
}

bool isSpoilerMesh(const QString &name)
{
    if (name.contains(QStringLiteral("mirror"), Qt::CaseInsensitive)) {
        return false;
    }
    return name.contains(QStringLiteral("spoiler"), Qt::CaseInsensitive)
        || name.contains(QStringLiteral("wing"), Qt::CaseInsensitive);
}

bool isTrunkPanelMesh(const QString &name)
{
    return name.startsWith(QStringLiteral("trunk"), Qt::CaseInsensitive)
        && !isSpoilerMesh(name);
}

enum SideBit {
    kSideFront = 1 << 0,
    kSideBack = 1 << 1,
    kSideTop = 1 << 2,
    kSideLeft = 1 << 3,
    kSideRight = 1 << 4,
    kSideSpoiler = 1 << 5,
    kSideGlassFront = 1 << 6,
    kSideGlassBack = 1 << 7,
    kSideGlassTop = 1 << 8,
    kSideGlassLeft = 1 << 9,
    kSideGlassRight = 1 << 10,
    kAllBodySides = 0x1F,
    kAllGlassSides = 0x7C0,
};

enum CarPartType {
    kFrontBumperPart = 34,
    kRearBumperPart = 35,
    kHoodPart = 36,
    kSideSkirtsPart = 37,
};

quint64 fallbackPaintHash(const fh6::CarMesh &mesh)
{
    const QString identity = mesh.name.toLower() + QLatin1Char('|') + mesh.materialName.toLower();
    if (isWindowGlassMaterial(mesh)) {
        return 0x9582FD1BA2FFF9A4ull;
    }
    if (identity.contains(QStringLiteral("caliper")) || identity.contains(QStringLiteral("brake"))) {
        return 0xA5495E0A43DF55B9ull;
    }
    if (isBodyPaintMaterial(mesh.materialName)) {
        if (isSpoilerMesh(mesh.name)) {
            return 0xCD48110253EE319Aull;
        }
        if (mesh.carPartType == kHoodPart || identity.contains(QStringLiteral("hood"))) {
            return 0x6AC1E9D87FE5D953ull;
        }
        if (identity.contains(QStringLiteral("mirror"))) {
            return 0x1E5FF0F50C741122ull;
        }
        return 0xF7DBE8A7C839A675ull;
    }
    return 0;
}

struct WheelMaterialFallback {
    QVector3D color;
    float gloss = 0.45f;
    float metallic = 0.0f;
};

std::optional<WheelMaterialFallback> wheelMaterialFallback(const fh6::CarMesh &mesh)
{
    if (!mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    const QString material = mesh.materialName.toLower();
    if (material == QStringLiteral("black")) {
        return WheelMaterialFallback{QVector3D(0.015f, 0.015f, 0.018f), 0.25f, 0.0f};
    }
    if (material == QStringLiteral("rim") || material == QStringLiteral("rim2")) {
        return WheelMaterialFallback{QVector3D(0.32f, 0.34f, 0.37f), 0.78f, 0.85f};
    }
    if (material == QStringLiteral("inner_rim") || material == QStringLiteral("hub")) {
        return WheelMaterialFallback{QVector3D(0.12f, 0.13f, 0.15f), 0.48f, 0.65f};
    }
    if (material == QStringLiteral("lug")) {
        return WheelMaterialFallback{QVector3D(0.38f, 0.40f, 0.43f), 0.72f, 0.8f};
    }
    return std::nullopt;
}

int allowedWindowSidesForPart(const QString &rawName)
{
    QString n = rawName.toLower();
    const int pipe = n.indexOf(QLatin1Char('|'));
    if (pipe >= 0) {
        n = n.left(pipe);
    }
    if (n.contains(QStringLiteral("int"))) {
        return 0;
    }
    if (n.startsWith(QStringLiteral("glassf_"))) return kSideGlassFront;
    if (n.startsWith(QStringLiteral("glassr_"))) return kSideGlassBack;
    if (n.startsWith(QStringLiteral("glasstop_")) || n.startsWith(QStringLiteral("glassroof_"))) return kSideGlassTop;
    if (n.startsWith(QStringLiteral("glasslf_")) || n.startsWith(QStringLiteral("glasslr_"))) return kSideGlassLeft;
    if (n.startsWith(QStringLiteral("glassrf_")) || n.startsWith(QStringLiteral("glassrr_"))) return kSideGlassRight;
    return 0;
}

int projectionSidesForMesh(const fh6::CarMesh &mesh)
{
    int sides = 0;
    if (isBodyPaintMaterial(mesh.materialName)) {
        if (isSpoilerMesh(mesh.name)) {
            sides = kSideSpoiler;
        } else if (isTrunkPanelMesh(mesh.name)) {
            sides = kSideTop;
        } else {
            switch (mesh.carPartType) {
            case kFrontBumperPart:
                sides = kSideFront | kSideLeft | kSideRight;
                break;
            case kRearBumperPart:
                sides = kSideBack | kSideLeft | kSideRight;
                break;
            case kHoodPart:
                sides = kSideTop;
                break;
            case kSideSkirtsPart:
                sides = kSideLeft | kSideRight;
                break;
            default:
                sides = kAllBodySides;
                break;
            }
        }
    }
    if (isWindowGlassMaterial(mesh)) {
        sides |= allowedWindowSidesForPart(mesh.name);
    }
    return sides;
}

std::optional<fh6::ModelVec3> locatorPosition(const fh6::CarModel &model, const char *name)
{
    for (const fh6::CarLocator &locator : model.locators) {
        if (locator.name.compare(QLatin1String(name), Qt::CaseInsensitive) == 0) {
            return locator.position;
        }
    }
    return std::nullopt;
}

std::optional<float> wheelAxleMidpointZ(const fh6::CarModel &model)
{
    std::vector<float> locatorSamples;
    for (const char *name : {"carLocator_wheelLF", "carLocator_wheelLR",
                             "carLocator_wheelRF", "carLocator_wheelRR"}) {
        if (const std::optional<fh6::ModelVec3> position = locatorPosition(model, name)) {
            locatorSamples.push_back(position->z);
        }
    }
    if (locatorSamples.size() >= 2) {
        const float rear = *std::min_element(locatorSamples.begin(), locatorSamples.end());
        const float front = *std::max_element(locatorSamples.begin(), locatorSamples.end());
        if (front - rear >= 0.5f) {
            return 0.5f * (front + rear);
        }
    }

    std::vector<float> samples;
    for (const fh6::CarMesh &mesh : model.meshes) {
        if (!mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)
            || mesh.positions.empty()) {
            continue;
        }
        double sum = 0.0;
        for (const fh6::ModelVec3 &position : mesh.positions) {
            sum += mesh.boneTransform.transformPoint(position).z;
        }
        samples.push_back(static_cast<float>(sum / mesh.positions.size()));
    }
    if (samples.size() < 2) {
        return std::nullopt;
    }

    float rear = *std::min_element(samples.begin(), samples.end());
    float front = *std::max_element(samples.begin(), samples.end());
    for (int iteration = 0; iteration < 8; ++iteration) {
        double rearSum = 0.0;
        double frontSum = 0.0;
        int rearCount = 0;
        int frontCount = 0;
        for (float z : samples) {
            if (std::abs(z - rear) < std::abs(z - front)) {
                rearSum += z;
                ++rearCount;
            } else {
                frontSum += z;
                ++frontCount;
            }
        }
        if (rearCount > 0) {
            rear = static_cast<float>(rearSum / rearCount);
        }
        if (frontCount > 0) {
            front = static_cast<float>(frontSum / frontCount);
        }
    }
    if (front - rear < 0.5f) {
        return std::nullopt;
    }
    return 0.5f * (front + rear);
}

std::optional<std::pair<float, float>> longitudinalLocatorRange(
    const fh6::CarModel &model, const fh6::LiverySide &side)
{
    if (side.xAxis != 2) {
        return std::nullopt;
    }
    const std::optional<fh6::ModelVec3> front = locatorPosition(model, "carLocator_bumperF");
    const std::optional<fh6::ModelVec3> rear = locatorPosition(model, "carLocator_bumperR");
    if (!front.has_value() || !rear.has_value()) {
        return std::nullopt;
    }
    const float sign = side.xSign * side.xScale;
    const float a = sign * front->z;
    const float b = sign * rear->z;
    if (std::abs(a - b) < 0.5f) {
        return std::nullopt;
    }
    return std::pair<float, float>{std::min(a, b), std::max(a, b)};
}

struct ProjectionAlignment {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float pivotX = 0.5f;
    float pivotY = 0.5f;
};

ProjectionAlignment alignProjectionToMask(
    int sideIndex,
    const fh6::LiverySide &side,
    const std::vector<fh6::CarMesh> &meshes,
    const std::vector<char> &keepLod,
    float axlo,
    float axhi,
    float aylo,
    float ayhi,
    const std::optional<float> &longitudinalPivotZ,
    bool lockLongitudinalX)
{
    ProjectionAlignment result;
    if (!side.mask.valid() || axhi <= axlo || ayhi <= aylo) {
        return result;
    }

    const float regionWidth = std::abs(side.right - side.left);
    const float regionHeight = std::abs(side.bottom - side.top);
    if (regionWidth < 1.0f || regionHeight < 1.0f) {
        return result;
    }
    constexpr int longEdge = 256;
    const int rasterWidth = regionWidth >= regionHeight
        ? longEdge
        : std::max(32, static_cast<int>(std::lround(longEdge * regionWidth / regionHeight)));
    const int rasterHeight = regionHeight >= regionWidth
        ? longEdge
        : std::max(32, static_cast<int>(std::lround(longEdge * regionHeight / regionWidth)));

    QImage geometry(rasterWidth, rasterHeight, QImage::Format_Grayscale8);
    geometry.fill(0);
    QPainter painter(&geometry);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const auto signedAxis = [&](const fh6::ModelVec3 &point, bool xAxis) {
        const int axis = xAxis ? side.xAxis : side.yAxis;
        const float sign = xAxis ? side.xSign * side.xScale : side.ySign * side.yScale;
        return sign * axisOf(point, axis);
    };
    const float axisWidth = axhi - axlo;
    const float axisHeight = ayhi - aylo;
    const auto projectionPivot = [&](bool horizontal) {
        const int axis = horizontal ? side.xAxis : side.yAxis;
        if (sideIndex < 2 || sideIndex > 4 || axis != 2
            || !longitudinalPivotZ.has_value()) {
            return 0.5f;
        }
        const float sign = horizontal
            ? side.xSign * side.xScale
            : side.ySign * side.yScale;
        const float lo = horizontal ? axlo : aylo;
        const float span = horizontal ? axisWidth : axisHeight;
        return (sign * *longitudinalPivotZ - lo) / span;
    };
    result.pivotX = projectionPivot(true);
    result.pivotY = projectionPivot(false);
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const fh6::CarMesh &mesh = meshes[mi];
        if (!keepLod[mi] || (projectionSidesForMesh(mesh) & (1 << sideIndex)) == 0) {
            continue;
        }
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            QPointF triangle[3];
            bool valid = true;
            bool haveNormals = true;
            float facing = 0.0f;
            for (int corner = 0; corner < 3; ++corner) {
                const quint32 index = mesh.indices[i + corner];
                if (index >= mesh.positions.size()) {
                    valid = false;
                    break;
                }
                const fh6::ModelVec3 point = mesh.boneTransform.transformPoint(mesh.positions[index]);
                const float x = (signedAxis(point, true) - axlo) / axisWidth;
                const float y = (signedAxis(point, false) - aylo) / axisHeight;
                triangle[corner] = QPointF(x * (rasterWidth - 1), y * (rasterHeight - 1));
                if (index < mesh.normals.size()) {
                    const fh6::ModelVec3 normal =
                        mesh.boneTransform.transformVector(mesh.normals[index]);
                    facing += normal.x * kFacing[sideIndex].x()
                        + normal.y * kFacing[sideIndex].y()
                        + normal.z * kFacing[sideIndex].z();
                } else {
                    haveNormals = false;
                }
            }
            if (valid && (!haveNormals || facing > 0.0f)) {
                painter.drawPolygon(triangle, 3);
            }
        }
    }
    painter.end();

    const size_t rasterSize = static_cast<size_t>(rasterWidth) * rasterHeight;
    std::vector<uint8_t> source(rasterSize, 0);
    for (int y = 0; y < rasterHeight; ++y) {
        const uchar *row = geometry.constScanLine(y);
        for (int x = 0; x < rasterWidth; ++x) {
            source[static_cast<size_t>(y) * rasterWidth + x] = row[x] >= 128 ? 1 : 0;
        }
    }

    std::vector<uint8_t> target(rasterSize, 0);
    int targetCount = 0;
    for (int y = 0; y < rasterHeight; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / rasterHeight;
        const float canvasY = side.top + (side.bottom - side.top) * v;
        const float maskV = (512.0f - canvasY) / 1024.0f;
        const int maskY = std::clamp(
            static_cast<int>(std::lround(maskV * (side.mask.height - 1))), 0, side.mask.height - 1);
        for (int x = 0; x < rasterWidth; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / rasterWidth;
            const float canvasX = side.left + (side.right - side.left) * u;
            const float maskU = (canvasX + 1024.0f) / 2048.0f;
            const int maskX = std::clamp(
                static_cast<int>(std::lround(maskU * (side.mask.width - 1))), 0, side.mask.width - 1);
            const bool covered = side.mask.at(maskX, maskY) >= 128;
            target[static_cast<size_t>(y) * rasterWidth + x] = covered ? 1 : 0;
            targetCount += covered ? 1 : 0;
        }
    }
    if (targetCount == 0) {
        return result;
    }

    const auto boundaryPoints = [&](const std::vector<uint8_t> &image) {
        std::vector<QPoint> points;
        for (int y = 0; y < rasterHeight; ++y) {
            for (int x = 0; x < rasterWidth; ++x) {
                if (image[static_cast<size_t>(y) * rasterWidth + x] == 0) {
                    continue;
                }
                const bool boundary = x == 0 || y == 0 || x + 1 == rasterWidth
                    || y + 1 == rasterHeight
                    || image[static_cast<size_t>(y) * rasterWidth + x - 1] == 0
                    || image[static_cast<size_t>(y) * rasterWidth + x + 1] == 0
                    || image[static_cast<size_t>(y - 1) * rasterWidth + x] == 0
                    || image[static_cast<size_t>(y + 1) * rasterWidth + x] == 0;
                if (boundary) {
                    points.emplace_back(x, y);
                }
            }
        }
        return points;
    };

    const std::vector<QPoint> sourceBoundary = boundaryPoints(source);
    const std::vector<QPoint> targetBoundary = boundaryPoints(target);
    if (sourceBoundary.empty() || targetBoundary.empty()) {
        return result;
    }

    const float distanceLimit = std::max(6.0f, std::min(rasterWidth, rasterHeight) * 0.08f);
    const auto distanceField = [&](const std::vector<QPoint> &points) {
        std::vector<float> distance(rasterSize, distanceLimit);
        for (const QPoint &point : points) {
            distance[static_cast<size_t>(point.y()) * rasterWidth + point.x()] = 0.0f;
        }
        constexpr float diagonal = 1.41421356f;
        const auto relax = [&](int x, int y, int nx, int ny, float step) {
            if (nx < 0 || ny < 0 || nx >= rasterWidth || ny >= rasterHeight) {
                return;
            }
            float &value = distance[static_cast<size_t>(y) * rasterWidth + x];
            value = std::min(value,
                             distance[static_cast<size_t>(ny) * rasterWidth + nx] + step);
        };
        for (int y = 0; y < rasterHeight; ++y) {
            for (int x = 0; x < rasterWidth; ++x) {
                relax(x, y, x - 1, y, 1.0f);
                relax(x, y, x, y - 1, 1.0f);
                relax(x, y, x - 1, y - 1, diagonal);
                relax(x, y, x + 1, y - 1, diagonal);
            }
        }
        for (int y = rasterHeight - 1; y >= 0; --y) {
            for (int x = rasterWidth - 1; x >= 0; --x) {
                relax(x, y, x + 1, y, 1.0f);
                relax(x, y, x, y + 1, 1.0f);
                relax(x, y, x + 1, y + 1, diagonal);
                relax(x, y, x - 1, y + 1, diagonal);
            }
        }
        return distance;
    };
    const std::vector<float> sourceDistance = distanceField(sourceBoundary);
    const std::vector<float> targetDistance = distanceField(targetBoundary);

    const auto matchCost = [&](const ProjectionAlignment &alignment) {
        double sourceCost = 0.0;
        for (const QPoint &point : sourceBoundary) {
            const float x = static_cast<float>(point.x()) / (rasterWidth - 1);
            const float y = static_cast<float>(point.y()) / (rasterHeight - 1);
            const float tx = alignment.pivotX + alignment.offsetX
                + alignment.scaleX * (x - alignment.pivotX);
            const float ty = alignment.pivotY + alignment.offsetY
                + alignment.scaleY * (y - alignment.pivotY);
            if (tx < 0.0f || tx > 1.0f || ty < 0.0f || ty > 1.0f) {
                sourceCost += distanceLimit;
                continue;
            }
            const int px = std::clamp(static_cast<int>(std::lround(tx * (rasterWidth - 1))),
                                      0, rasterWidth - 1);
            const int py = std::clamp(static_cast<int>(std::lround(ty * (rasterHeight - 1))),
                                      0, rasterHeight - 1);
            sourceCost += targetDistance[static_cast<size_t>(py) * rasterWidth + px];
        }

        double targetCost = 0.0;
        for (const QPoint &point : targetBoundary) {
            const float x = static_cast<float>(point.x()) / (rasterWidth - 1);
            const float y = static_cast<float>(point.y()) / (rasterHeight - 1);
            const float sx = alignment.pivotX
                + (x - alignment.pivotX - alignment.offsetX) / alignment.scaleX;
            const float sy = alignment.pivotY
                + (y - alignment.pivotY - alignment.offsetY) / alignment.scaleY;
            if (sx < 0.0f || sx > 1.0f || sy < 0.0f || sy > 1.0f) {
                targetCost += distanceLimit;
                continue;
            }
            const int px = std::clamp(static_cast<int>(std::lround(sx * (rasterWidth - 1))),
                                      0, rasterWidth - 1);
            const int py = std::clamp(static_cast<int>(std::lround(sy * (rasterHeight - 1))),
                                      0, rasterHeight - 1);
            targetCost += sourceDistance[static_cast<size_t>(py) * rasterWidth + px];
        }
        return static_cast<float>(0.65 * sourceCost / sourceBoundary.size()
                                  + 0.35 * targetCost / targetBoundary.size());
    };

    const ProjectionAlignment initialAlignment = result;
    const float initialCost = matchCost(result);
    float bestCost = initialCost;
    float scaleStep = 0.025f;
    float offsetStep = 0.015f;
    for (int iteration = 0; iteration < 7; ++iteration) {
        for (int parameter = 0; parameter < 4; ++parameter) {
            if (lockLongitudinalX && (parameter == 0 || parameter == 2)) {
                continue;
            }
            ProjectionAlignment best = result;
            for (int direction = -2; direction <= 2; ++direction) {
                ProjectionAlignment candidate = result;
                float *value = parameter == 0 ? &candidate.scaleX
                    : parameter == 1 ? &candidate.scaleY
                    : parameter == 2 ? &candidate.offsetX
                                     : &candidate.offsetY;
                *value += direction * (parameter < 2 ? scaleStep : offsetStep);
                if (candidate.scaleX < 0.85f || candidate.scaleX > 1.15f
                    || candidate.scaleY < 0.85f || candidate.scaleY > 1.15f
                    || std::abs(candidate.offsetX) > 0.08f
                    || std::abs(candidate.offsetY) > 0.08f) {
                    continue;
                }
                const float candidateCost = matchCost(candidate);
                if (candidateCost < bestCost) {
                    bestCost = candidateCost;
                    best = candidate;
                }
            }
            result = best;
        }
        scaleStep *= 0.5f;
        offsetStep *= 0.5f;
    }
    if (bestCost > initialCost - 0.05f) {
        return initialAlignment;
    }
    return result;
}

std::vector<uint8_t> upsampleCoverageMask(const fh6::SwatchMask &mask, int dstW, int dstH)
{
    if (!mask.valid() || dstW <= 0 || dstH <= 0) {
        return {};
    }
    if (mask.width == dstW && mask.height == dstH) {
        return mask.coverage;
    }

    std::vector<uint8_t> out(static_cast<size_t>(dstW) * dstH, 0);
    const float scaleX = static_cast<float>(mask.width) / static_cast<float>(dstW);
    const float scaleY = static_cast<float>(mask.height) / static_cast<float>(dstH);
    for (int y = 0; y < dstH; ++y) {
        const float sy = std::clamp((static_cast<float>(y) + 0.5f) * scaleY - 0.5f,
                                    0.0f,
                                    static_cast<float>(mask.height - 1));
        const int y0 = static_cast<int>(std::floor(sy));
        const int y1 = std::min(y0 + 1, mask.height - 1);
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < dstW; ++x) {
            const float sx = std::clamp((static_cast<float>(x) + 0.5f) * scaleX - 0.5f,
                                        0.0f,
                                        static_cast<float>(mask.width - 1));
            const int x0 = static_cast<int>(std::floor(sx));
            const int x1 = std::min(x0 + 1, mask.width - 1);
            const float fx = sx - static_cast<float>(x0);
            const auto at = [&](int ix, int iy) {
                return static_cast<float>(mask.coverage[static_cast<size_t>(iy) * mask.width + ix]);
            };
            const float top = at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx;
            const float bottom = at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx;
            out[static_cast<size_t>(y) * dstW + x] =
                static_cast<uint8_t>(std::clamp(std::lround(top * (1.0f - fy) + bottom * fy), 0L, 255L));
        }
    }
    return out;
}

} // namespace

void CarModelRenderer::setLivery(const fh6::CarModel &model, const fh6::LiveryMaskSet &masks)
{
    clearLivery();
    if (!initialized_ || !masks.valid()) {
        return;
    }
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return;
    }

    int sourceW = 0, sourceH = 0;
    for (int s = 0; s < kLiverySideCount; ++s) {
        const fh6::SwatchMask &m = masks.sides[s].mask;
        if (m.valid()) {
            sourceW = m.width;
            sourceH = m.height;
            break;
        }
    }
    if (sourceW == 0 || sourceH == 0) {
        return;
    }
    const int texW = sourceW * kMaskTextureScale;
    const int texH = sourceH * kMaskTextureScale;

    const auto sgn = [](const fh6::LiverySide &L, int which) {
        return which == 0 ? L.xSign * L.xScale : L.ySign * L.yScale;
    };
    float axlo[kLiverySideCount], axhi[kLiverySideCount], aylo[kLiverySideCount], ayhi[kLiverySideCount];
    for (int s = 0; s < kLiverySideCount; ++s) {
        axlo[s] = aylo[s] = std::numeric_limits<float>::max();
        axhi[s] = ayhi[s] = std::numeric_limits<float>::lowest();
    }
    const std::vector<fh6::CarMesh> &projectionMeshes = model.liveryProjectionMeshes.empty()
        ? model.meshes
        : model.liveryProjectionMeshes;
    const std::vector<char> keepLod = highestLodFlags(projectionMeshes);
    const std::optional<float> longitudinalPivotZ = wheelAxleMidpointZ(model);
    for (size_t mi = 0; mi < projectionMeshes.size(); ++mi) {
        const fh6::CarMesh &mesh = projectionMeshes[mi];
        if (!keepLod[mi] || mesh.positions.empty()) {
            continue;
        }
        const int candidateSides = projectionSidesForMesh(mesh);
        if (candidateSides == 0) {
            continue;
        }
        for (const fh6::ModelVec3 &position : mesh.positions) {
            const fh6::ModelVec3 wp = mesh.boneTransform.transformPoint(position);
            for (int side = 0; side < kLiverySideCount; ++side) {
                if ((candidateSides & (1 << side)) == 0) {
                    continue;
                }
                const fh6::LiverySide &liverySide = masks.sides[side];
                const float ax = sgn(liverySide, 0) * axisOf(wp, liverySide.xAxis);
                const float ay = sgn(liverySide, 1) * axisOf(wp, liverySide.yAxis);
                axlo[side] = std::min(axlo[side], ax);
                axhi[side] = std::max(axhi[side], ax);
                aylo[side] = std::min(aylo[side], ay);
                ayhi[side] = std::max(ayhi[side], ay);
            }
        }
    }

    std::array<bool, kLiverySideCount> locatorAnchored{};
    for (int side = 2; side <= 4; ++side) {
        if (const std::optional<std::pair<float, float>> range =
                longitudinalLocatorRange(model, masks.sides[side])) {
            axlo[side] = range->first;
            axhi[side] = range->second;
            locatorAnchored[side] = true;
        }
    }

    QOpenGLExtraFunctions *ext = context->extraFunctions();
    QOpenGLFunctions *fns = context->functions();

    fns->glGenTextures(1, &sideMaskArray_);
    fns->glBindTexture(GL_TEXTURE_2D_ARRAY, sideMaskArray_);
    ext->glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, texW, texH, kLiverySideCount,
                      0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    fns->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    fns->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    fns->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    fns->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    fns->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const std::vector<uint8_t> empty(static_cast<size_t>(texW) * texH, 0);
    sideAxis_.reserve(kLiverySideCount);
    sideEMin_.reserve(kLiverySideCount);
    sideEMax_.reserve(kLiverySideCount);
    sideRegion_.reserve(kLiverySideCount);
    defaultSidePaintRegion_.reserve(kLiverySideCount);
    sideFacing_.reserve(kLiverySideCount);
    for (int s = 0; s < kLiverySideCount; ++s) {
        const fh6::LiverySide &side = masks.sides[s];
        const fh6::SwatchMask &m = side.mask;
        const bool haveMask = m.valid() && m.width == sourceW && m.height == sourceH;
        std::vector<uint8_t> upsampled;
        const uint8_t *maskData = empty.data();
        if (haveMask) {
            upsampled = upsampleCoverageMask(m, texW, texH);
            if (!upsampled.empty()) {
                maskData = upsampled.data();
            }
        }
        ext->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, s, texW, texH, 1,
                             GL_RED, GL_UNSIGNED_BYTE,
                             maskData);

        sideAxis_.append(QVector4D(static_cast<float>(side.xAxis),
                                   static_cast<float>(side.yAxis),
                                   sgn(side, 0), sgn(side, 1)));

        const bool have = axhi[s] > axlo[s] && ayhi[s] > aylo[s];
        if (have) {
            const ProjectionAlignment alignment = alignProjectionToMask(
                s, side, projectionMeshes, keepLod, axlo[s], axhi[s], aylo[s], ayhi[s],
                longitudinalPivotZ, locatorAnchored[s]);
            const float width = (axhi[s] - axlo[s]) / alignment.scaleX;
            const float height = (ayhi[s] - aylo[s]) / alignment.scaleY;
            const float startX = alignment.pivotX + alignment.offsetX
                - alignment.scaleX * alignment.pivotX;
            const float startY = alignment.pivotY + alignment.offsetY
                - alignment.scaleY * alignment.pivotY;
            const float alignedMinX = axlo[s] - startX * width;
            const float alignedMinY = aylo[s] - startY * height;
            sideEMin_.append(QVector2D(alignedMinX, alignedMinY));
            sideEMax_.append(QVector2D(alignedMinX + width, alignedMinY + height));
        } else {
            sideEMin_.append(QVector2D(-1.0f, -1.0f));
            sideEMax_.append(QVector2D(1.0f, 1.0f));
        }

        sideRegion_.append(QVector4D(side.left, side.right, side.top, side.bottom));
        defaultSidePaintRegion_.append(QVector4D(
            (side.left + 1024.0f) / 2048.0f,
            (side.right + 1024.0f) / 2048.0f,
            (512.0f - side.top) / 1024.0f,
            (512.0f - side.bottom) / 1024.0f));

        sideFacing_.append(kFacing[s]);
    }
    sidePaintRegion_ = defaultSidePaintRegion_;
    fns->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    sideCount_ = kLiverySideCount;
}

void CarModelRenderer::setPaintTextureRegions(const QVector<QVector4D> &regions)
{
    sidePaintRegion_ = regions.size() == sideCount_ ? regions : defaultSidePaintRegion_;
}

void CarModelRenderer::uploadModel(const fh6::CarModel &model)
{
    if (!initialized_) {
        return;
    }
    clearModel();

    const std::vector<char> keepLod = highestLodFlags(model.meshes);
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const fh6::CarMesh &mesh = model.meshes[mi];
        const QString materialName = mesh.materialName.toLower();
        if (mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)
            && materialName != QStringLiteral("rim") && materialName != QStringLiteral("rim2")) {
            continue;
        }
        if (!keepLod[mi] || mesh.positions.empty() || mesh.indices.empty()) {
            continue;
        }
        if (isInteriorWindowShell(mesh.name)) {
            continue;
        }

        const std::vector<fh6::ModelVec2> *uv = nullptr;
        if (mesh.liveryUvChannel >= 0
            && mesh.liveryUvChannel < static_cast<int>(mesh.uvChannels.size())
            && mesh.uvChannels[mesh.liveryUvChannel].size() == mesh.positions.size()) {
            uv = &mesh.uvChannels[mesh.liveryUvChannel];
        }
        const bool hasDirectLiveryUv = uv != nullptr && mesh.liveryUvChannel == 3;
        const fh6::TexCoordTransform &uvTransform = mesh.texCoordTransforms[3];

        std::vector<float> interleaved;
        interleaved.reserve(mesh.positions.size() * 8);
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            const fh6::ModelVec3 &p = mesh.positions[i];
            const fh6::ModelVec3 &n = i < mesh.normals.size() ? mesh.normals[i] : fh6::ModelVec3{0.0f, 1.0f, 0.0f};
            interleaved.push_back(p.x);
            interleaved.push_back(p.y);
            interleaved.push_back(p.z);
            interleaved.push_back(n.x);
            interleaved.push_back(n.y);
            interleaved.push_back(n.z);
            if (uv != nullptr) {
                interleaved.push_back((*uv)[i].u * uvTransform.scaleU + uvTransform.offsetU);
                interleaved.push_back((*uv)[i].v * uvTransform.scaleV + uvTransform.offsetV);
            } else {
                interleaved.push_back(0.0f);
                interleaved.push_back(0.0f);
            }
        }

        auto buffers = std::make_unique<MeshBuffers>();
        buffers->indexCount = static_cast<int>(mesh.indices.size());
        buffers->hasDirectLiveryUv = hasDirectLiveryUv;
        int bodySides = 0;
        if (isBodyPaintMaterial(mesh.materialName)) {
            if (isSpoilerMesh(mesh.name)) {
                bodySides = kSideSpoiler;
            } else if (isTrunkPanelMesh(mesh.name)) {
                bodySides = kSideTop;
            } else {
                bodySides = kAllBodySides;
            }
        }
        const bool windowGlass = isWindowGlassMaterial(mesh);
        const int windowSides = windowGlass && allowedWindowSidesForPart(mesh.name) != 0
            ? kAllGlassSides
            : 0;
        buffers->allowedSides = bodySides | windowSides;
        buffers->applyLivery = buffers->allowedSides != 0;
        buffers->paintMaterialHash = mesh.paintMaterialHash != 0
            ? mesh.paintMaterialHash
            : fallbackPaintHash(mesh);
        if (mesh.material) {
            buffers->hasMaterialColor = mesh.material->hasBaseColor;
            buffers->materialColor = QVector3D(
                mesh.material->baseColor[0], mesh.material->baseColor[1], mesh.material->baseColor[2]);
            buffers->emissiveColor = QVector3D(
                mesh.material->emissiveColor[0], mesh.material->emissiveColor[1], mesh.material->emissiveColor[2]);
            buffers->emissiveIntensity = mesh.material->emissiveIntensity;
            buffers->gloss = mesh.material->gloss;
            buffers->alpha = mesh.material->opacity;
        }
        if (isBodyPaintMaterial(mesh.materialName)) {
            buffers->hasMaterialColor = false;
        }
        const std::optional<WheelMaterialFallback> wheel = wheelMaterialFallback(mesh);
        const std::optional<MaterialFallback> material = exteriorMaterialFallback(mesh);
        if (mesh.name.startsWith(QStringLiteral("tire"), Qt::CaseInsensitive)) {
            buffers->hasMaterialColor = true;
            buffers->materialColor = QVector3D(0.018f, 0.019f, 0.021f);
            buffers->gloss = 0.22f;
            buffers->metallic = 0.0f;
        } else if (wheel) {
            if (!buffers->hasMaterialColor) {
                buffers->hasMaterialColor = true;
                buffers->materialColor = wheel->color;
            }
            buffers->gloss = wheel->gloss;
            buffers->metallic = wheel->metallic;
        } else if (material) {
            if (!buffers->hasMaterialColor) {
                buffers->hasMaterialColor = true;
                buffers->materialColor = material->color;
            }
            buffers->gloss = material->gloss;
            buffers->metallic = material->metallic;
        }
        if (isLampSurface(mesh)) {
            const QString material = materialIdentity(mesh);
            const QString resource = materialResourceIdentity(mesh);
            const QString materialClass = resource.isEmpty() ? material : resource;
            const QString name = mesh.name.toLower();
            const bool emitter = isLampEmitterMaterial(mesh);
            const bool glass = name.contains(QStringLiteral("glass"))
                || name.contains(QStringLiteral("lens"))
                || material.startsWith(QStringLiteral("gls"))
                || material.contains(QStringLiteral("|gls"))
                || material.contains(QStringLiteral("glass"));
            if (materialClass.contains(QStringLiteral("chrome"))) {
                buffers->hasMaterialColor = true;
                buffers->materialColor = QVector3D(0.72f, 0.74f, 0.78f);
                buffers->gloss = 0.94f;
                buffers->metallic = 1.0f;
            }
            if (glass) {
                buffers->hasMaterialColor = true;
                if (buffers->materialColor == QVector3D(0.55f, 0.55f, 0.55f)) {
                    buffers->materialColor = QVector3D(0.78f, 0.82f, 0.88f);
                }
                buffers->alpha = material.contains(QStringLiteral("colored")) ? 0.34f : 0.20f;
                buffers->gloss = 0.96f;
                buffers->metallic = 0.0f;
            }
            if (emitter) {
                if (buffers->emissiveColor.lengthSquared() < 0.000001f) {
                    if (name.contains(QStringLiteral("tail"))) {
                        buffers->emissiveColor = QVector3D(1.0f, 0.025f, 0.012f);
                    } else if (name.contains(QStringLiteral("marker"))
                               || name.contains(QStringLiteral("indicator"))) {
                        buffers->emissiveColor = QVector3D(1.0f, 0.30f, 0.025f);
                    } else {
                        buffers->emissiveColor = QVector3D(1.0f, 0.86f, 0.68f);
                    }
                }
                if (buffers->emissiveIntensity <= 0.0f) {
                    buffers->emissiveIntensity = 1.0f;
                }
                buffers->hasMaterialColor = true;
                buffers->materialColor = buffers->emissiveColor * 0.10f;
                buffers->gloss = 0.84f;
                if (material.contains(QStringLiteral("lightbulb"))) {
                    buffers->alpha = 0.52f;
                }
            } else {
                buffers->emissiveIntensity = 0.0f;
            }
        }
        if (windowGlass && buffers->alpha >= 0.995f) {
            buffers->alpha = 0.42f;
        }
        buffers->translucent = buffers->alpha < 0.995f;

        // Bone matrices cross from row-vector to column-vector convention.
        const auto &m = mesh.boneTransform.m;
        buffers->model = QMatrix4x4(m[0], m[4], m[8], m[12],
                                    m[1], m[5], m[9], m[13],
                                    m[2], m[6], m[10], m[14],
                                    m[3], m[7], m[11], m[15]);

        QVector3D center;
        for (const fh6::ModelVec3 &p : mesh.positions) {
            const fh6::ModelVec3 wp = mesh.boneTransform.transformPoint(p);
            center += QVector3D(wp.x, wp.y, wp.z);
        }
        center /= static_cast<float>(mesh.positions.size());
        buffers->center = center;

        buffers->vao.create();
        buffers->vao.bind();

        buffers->vbo.create();
        buffers->vbo.bind();
        buffers->vbo.allocate(interleaved.data(), static_cast<int>(interleaved.size() * sizeof(float)));

        buffers->ibo.create();
        buffers->ibo.bind();
        buffers->ibo.allocate(mesh.indices.data(), static_cast<int>(mesh.indices.size() * sizeof(quint32)));

        constexpr int stride = 8 * sizeof(float);
        program_.enableAttributeArray(0);
        program_.setAttributeBuffer(0, GL_FLOAT, 0, 3, stride);
        program_.enableAttributeArray(1);
        program_.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, stride);
        program_.enableAttributeArray(2);
        program_.setAttributeBuffer(2, GL_FLOAT, 6 * sizeof(float), 2, stride);

        buffers->vao.release();
        buffers->vbo.release();
        buffers->ibo.release();

        meshes_.push_back(std::move(buffers));
    }
}

void CarModelRenderer::render(
    const QMatrix4x4 &view,
    const QMatrix4x4 &projection,
    GLuint liveryTexture,
    const QColor &basePaint,
    const fh6::LiveryPaintState *paintState)
{
    if (!initialized_ || meshes_.empty()) {
        return;
    }
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return;
    }
    QOpenGLFunctions *functions = context->functions();

    functions->glEnable(GL_DEPTH_TEST);
    functions->glDepthFunc(GL_LEQUAL);
    functions->glDisable(GL_CULL_FACE);

    program_.bind();
    const QVector3D fallbackColor(
        static_cast<float>(basePaint.redF()),
        static_cast<float>(basePaint.greenF()),
        static_cast<float>(basePaint.blueF()));

    bool eyeValid = false;
    const QVector3D eye = view.inverted(&eyeValid).map(QVector3D(0.0f, 0.0f, 0.0f));
    program_.setUniformValue(eyePositionLocation_, eyeValid ? eye : QVector3D());

    const bool hasLivery = liveryTexture != 0 && sideMaskArray_ != 0 && sideCount_ > 0;
    if (hasLivery) {
        functions->glActiveTexture(GL_TEXTURE0);
        functions->glBindTexture(GL_TEXTURE_2D, liveryTexture);
        program_.setUniformValue(liveryTexLocation_, 0);

        functions->glActiveTexture(GL_TEXTURE1);
        functions->glBindTexture(GL_TEXTURE_2D_ARRAY, sideMaskArray_);
        program_.setUniformValue(sideMasksLocation_, 1);

        program_.setUniformValue(sideCountLocation_, sideCount_);
        program_.setUniformValue(debugModeLocation_, debugMode_);
        program_.setUniformValueArray(sideAxisLocation_, sideAxis_.constData(), sideAxis_.size());
        program_.setUniformValueArray(sideEMinLocation_, sideEMin_.constData(), sideEMin_.size());
        program_.setUniformValueArray(sideEMaxLocation_, sideEMax_.constData(), sideEMax_.size());
        program_.setUniformValueArray(sideRegionLocation_, sideRegion_.constData(), sideRegion_.size());
        program_.setUniformValueArray(
            sidePaintRegionLocation_, sidePaintRegion_.constData(), sidePaintRegion_.size());
        program_.setUniformValueArray(sideFacingLocation_, sideFacing_.constData(), sideFacing_.size());
        functions->glActiveTexture(GL_TEXTURE0);
    }

    const QMatrix4x4 viewProjection = projection * view;
    const auto drawMesh = [&](MeshBuffers &mesh) {
        QVector3D primary = mesh.hasMaterialColor ? mesh.materialColor : fallbackColor;
        QVector3D secondary = primary;
        float secondaryMix = 0.0f;
        float gloss = mesh.gloss;
        float metallic = mesh.metallic;
        const fh6::LiveryPaintMaterial *paint = paintState != nullptr
            ? paintState->find(mesh.paintMaterialHash)
            : nullptr;
        const auto decodedColor = [](const fh6::LiveryPaintColor &color) {
            return QVector3D(color.bgra[2] / 255.0f, color.bgra[1] / 255.0f, color.bgra[0] / 255.0f);
        };
        if (paint != nullptr) {
            if (paint->primary.enabled) {
                primary = decodedColor(paint->primary);
            }
            if (paint->secondary.enabled) {
                secondary = decodedColor(paint->secondary);
            }
            switch (paint->finish) {
            case 1:
                gloss = 0.88f;
                break;
            case 2:
                gloss = 0.55f;
                break;
            case 3:
                gloss = 0.12f;
                break;
            case 4:
                gloss = 0.78f;
                metallic = 0.85f;
                secondaryMix = paint->secondary.enabled ? 0.20f : 0.0f;
                break;
            case 50:
                gloss = 0.12f;
                secondaryMix = paint->secondary.enabled ? 0.78f : 0.0f;
                break;
            case 51:
                gloss = 0.95f;
                secondaryMix = paint->secondary.enabled ? 0.78f : 0.0f;
                break;
            case 52:
                gloss = 0.55f;
                secondaryMix = paint->secondary.enabled ? 0.78f : 0.0f;
                break;
            case 69:
                gloss = 0.92f;
                metallic = 0.35f;
                break;
            case 70:
                gloss = 0.80f;
                metallic = 0.75f;
                secondaryMix = paint->secondary.enabled ? 0.12f : 0.0f;
                break;
            case 71:
                gloss = 0.84f;
                metallic = 0.85f;
                secondaryMix = paint->secondary.enabled ? 0.24f : 0.0f;
                break;
            case 72:
                gloss = 0.90f;
                metallic = 1.0f;
                secondaryMix = paint->secondary.enabled ? 0.34f : 0.0f;
                break;
            default:
                break;
            }
        }
        program_.setUniformValue(basePaintLocation_, primary);
        program_.setUniformValue(secondaryPaintLocation_, secondary);
        program_.setUniformValue(secondaryMixLocation_, secondaryMix);
        program_.setUniformValue(glossLocation_, gloss);
        program_.setUniformValue(metallicLocation_, metallic);
        program_.setUniformValue(
            emissiveLocation_, mesh.emissiveColor * std::max(0.0f, mesh.emissiveIntensity));
        program_.setUniformValue(hasLiveryLocation_, (hasLivery && mesh.applyLivery) ? 1 : 0);
        program_.setUniformValue(useDirectUvLocation_, mesh.hasDirectLiveryUv ? 1 : 0);
        program_.setUniformValue(allowedSidesLocation_, mesh.allowedSides);
        program_.setUniformValue(materialAlphaLocation_, mesh.alpha);
        program_.setUniformValue(mvpLocation_, viewProjection * mesh.model);
        program_.setUniformValue(modelLocation_, mesh.model);
        mesh.vao.bind();
        functions->glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
        mesh.vao.release();
    };

    functions->glDisable(GL_BLEND);
    functions->glDepthMask(GL_TRUE);
    for (const auto &mesh : meshes_) {
        if (!mesh->translucent) {
            drawMesh(*mesh);
        }
    }

    std::vector<MeshBuffers *> translucentMeshes;
    translucentMeshes.reserve(meshes_.size());
    for (const auto &mesh : meshes_) {
        if (mesh->translucent) {
            translucentMeshes.push_back(mesh.get());
        }
    }
    if (!translucentMeshes.empty()) {
        if (eyeValid) {
            std::sort(translucentMeshes.begin(), translucentMeshes.end(), [&](const MeshBuffers *a, const MeshBuffers *b) {
                return (a->center - eye).lengthSquared() > (b->center - eye).lengthSquared();
            });
        }
        functions->glEnable(GL_BLEND);
        functions->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        functions->glDepthMask(GL_FALSE);
        for (MeshBuffers *mesh : translucentMeshes) {
            drawMesh(*mesh);
        }
        functions->glDepthMask(GL_TRUE);
        functions->glDisable(GL_BLEND);
    }

    if (hasLivery) {
        functions->glActiveTexture(GL_TEXTURE1);
        functions->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        functions->glActiveTexture(GL_TEXTURE0);
        functions->glBindTexture(GL_TEXTURE_2D, 0);
    }
    program_.release();
}

} // namespace gui
