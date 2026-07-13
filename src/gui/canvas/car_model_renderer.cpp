#include "car_model_renderer.h"

#include <QHash>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>

#include <algorithm>
#include <cmath>
#include <limits>

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

void main()
{
    vec4 world = model * vec4(in_position, 1.0);
    gl_Position = mvp * vec4(in_position, 1.0);
    v_normal = mat3(model) * in_normal;
    v_world = world.xyz;
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
in vec3 v_normal;
in vec3 v_world;

uniform sampler2D livery_tex;
uniform sampler2DArray side_masks;
uniform vec3 base_paint;
uniform int has_livery;
uniform int side_count;
uniform vec4 side_axis[11];
uniform vec2 side_emin[11];
uniform vec2 side_emax[11];
uniform vec4 side_region[11];
uniform vec3 side_facing[11];
uniform float side_swap[11];
uniform float livery_scale;
uniform int debug_mode;
uniform int allowed_sides;
uniform float facing_min;
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
    vec3 albedo = base_paint;
    if (has_livery == 1 && side_count > 0) {
        vec3 n = normalize(v_normal);
        float bestScore = 0.0;
        float bestCoverage = 0.0;
        vec2 bestUv = vec2(0.0);
        int bestSide = -1;
        for (int s = 0; s < side_count; ++s) {
            if ((allowed_sides & (1 << s)) == 0) {
                continue;
            }
            float facing = dot(side_facing[s], n);
            if (s < 6 && facing < facing_min) {
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
            if (side_swap[s] > 0.5) {
                nrm = vec2(nrm.y, 1.0 - nrm.x);
            }
            nrm = 0.5 + (nrm - 0.5) * livery_scale;
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
            vec2 paintNrm = (s == 1 || s == 3 || s == 9) ? vec2(1.0 - nrm.x, nrm.y)
                                                          : vec2(nrm.x, 1.0 - nrm.y);
            vec2 paintCanvas = vec2(mix(reg.x, reg.y, paintNrm.x), mix(reg.z, reg.w, paintNrm.y));
            vec2 paintUv = canvasToUv(paintCanvas);
            float score = s >= 6 ? 1.0 : facing;
            if ((allowed_sides & 4) != 0 && (s == 3 || s == 4)) {
                score += 2.5;
            }
            if (score > bestScore) {
                bestScore = score;
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
            albedo = base_paint * (1.0 - paint.a) + paint.rgb;
        }
    }
    vec3 n = normalize(v_normal);
    vec3 l = normalize(vec3(0.4, 0.8, 0.6));
    float ambient = 0.35;
    float diffuse = max(dot(n, l), 0.0);
    vec3 lit = albedo * (ambient + (1.0 - ambient) * diffuse);
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

bool isWindowGlassMaterial(const QString &material)
{
    const QString name = material.toLower();
    if (name.isEmpty()
        || name.contains(QStringLiteral("screw"))
        || name.contains(QStringLiteral("frame"))
        || name.contains(QStringLiteral("label"))
        || name.contains(QStringLiteral("bulb"))
        || name.contains(QStringLiteral("light"))) {
        return false;
    }
    return name.contains(QStringLiteral("window"))
        || name.contains(QStringLiteral("windshield"))
        || name.contains(QStringLiteral("windsheild"))
        || name.contains(QStringLiteral("blackglass"));
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
    sideMasksLocation_ = program_.uniformLocation("side_masks");
    sideCountLocation_ = program_.uniformLocation("side_count");
    sideAxisLocation_ = program_.uniformLocation("side_axis");
    sideEMinLocation_ = program_.uniformLocation("side_emin");
    sideEMaxLocation_ = program_.uniformLocation("side_emax");
    sideRegionLocation_ = program_.uniformLocation("side_region");
    sideFacingLocation_ = program_.uniformLocation("side_facing");
    sideSwapLocation_ = program_.uniformLocation("side_swap");
    liveryScaleLocation_ = program_.uniformLocation("livery_scale");
    debugModeLocation_ = program_.uniformLocation("debug_mode");
    allowedSidesLocation_ = program_.uniformLocation("allowed_sides");
    facingMinLocation_ = program_.uniformLocation("facing_min");
    materialAlphaLocation_ = program_.uniformLocation("material_alpha");
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
    sideMasksLocation_ = -1;
    sideCountLocation_ = -1;
    sideAxisLocation_ = -1;
    sideEMinLocation_ = -1;
    sideEMaxLocation_ = -1;
    sideRegionLocation_ = -1;
    sideFacingLocation_ = -1;
    sideSwapLocation_ = -1;
    liveryScaleLocation_ = -1;
    allowedSidesLocation_ = -1;
    facingMinLocation_ = -1;
    materialAlphaLocation_ = -1;
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
    sideFacing_.clear();
    sideSwap_.clear();
}

void CarModelRenderer::setLiveryScale(float unitsPerMetre)
{
    if (unitsPerMetre > 0.0f) {
        liveryScale_ = unitsPerMetre;
    }
}

void CarModelRenderer::setFacingMin(float cosThreshold)
{
    facingMin_ = std::clamp(cosThreshold, 0.0f, 0.99f);
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

std::vector<char> highestLodFlags(const fh6::CarModel &model)
{
    QHash<QString, int> best;
    for (const fh6::CarMesh &mesh : model.meshes) {
        const int rank = lodRank(mesh.name);
        const QString base = lodBase(mesh.name);
        auto it = best.find(base);
        if (it == best.end() || rank > it.value()) {
            best.insert(base, rank);
        }
    }
    std::vector<char> keep(model.meshes.size(), 0);
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        keep[i] = lodRank(model.meshes[i].name) == best.value(lodBase(model.meshes[i].name)) ? 1 : 0;
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
    kAllBodySides = 0x3F,
};

int singleSideIndex(int mask)
{
    if (mask == 0 || (mask & (mask - 1)) != 0) {
        return -1;
    }
    int side = 0;
    while ((mask & 1) == 0) {
        mask >>= 1;
        ++side;
    }
    return side;
}

int allowedSidesForPart(const QString &rawName)
{
    const QString n = rawName.toLower();
    const auto has = [&](const char *s) { return n.contains(QLatin1String(s)); };

    if (has("taillight")) return kSideBack | kSideLeft | kSideRight;
    if (has("mirror") || has("liner") || has("jamb") || has("hinge")
        || has("handle") || has("bay") || has("label") || has("light")
        || has("glass") || has("wiper") || has("engine")) {
        return 0;
    }
    if (has("wing")) return kSideSpoiler;
    if (has("hood")) return kSideTop;
    if (has("trunk")) return kSideBack | kSideTop;
    if (has("bumperf")) return kSideFront | kSideLeft | kSideRight;
    if (has("bumperr")) return kSideBack | kSideLeft | kSideRight;
    if (has("skirt") || has("sill")) return kSideLeft | kSideRight;
    if (has("door")) {
        if (n.contains(QLatin1String("doorl"))) return kSideLeft;
        if (n.contains(QLatin1String("doorr"))) return kSideRight;
        return kSideLeft | kSideRight;
    }
    if (has("fender")) return kSideLeft | kSideRight | kSideTop;
    if (has("body")) return kSideLeft | kSideRight | kSideTop | kSideBack;
    return kAllBodySides;
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
    const std::vector<char> keepLod = highestLodFlags(model);
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const fh6::CarMesh &mesh = model.meshes[mi];
        if (!keepLod[mi] || mesh.positions.empty()) {
            continue;
        }
        const int allowed = isBodyPaintMaterial(mesh.materialName)
            ? allowedSidesForPart(mesh.name)
            : (isWindowGlassMaterial(mesh.materialName) ? allowedWindowSidesForPart(mesh.name) : 0);
        if (allowed == 0) {
            continue;
        }
        const bool isSpoiler = isSpoilerMesh(mesh.name);
        const int directSide = singleSideIndex(allowed & ~kAllBodySides);
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            const fh6::ModelVec3 wp = mesh.boneTransform.transformPoint(mesh.positions[i]);
            int side = -1;
            if (directSide >= 0) {
                side = directSide;
            } else if (isSpoiler) {
                side = (allowed & kSideSpoiler) ? 5 : -1;
            } else if (i < mesh.normals.size()) {
                fh6::ModelVec3 wn = mesh.boneTransform.transformVector(mesh.normals[i]);
                const QVector3D nv = QVector3D(wn.x, wn.y, wn.z).normalized();
                float best = 0.3f;
                for (int s = 0; s < 5; ++s) {
                    if ((allowed & (1 << s)) == 0) {
                        continue;
                    }
                    const float d = QVector3D::dotProduct(kFacing[s], nv);
                    if (d > best) {
                        best = d;
                        side = s;
                    }
                }
            }
            if (side < 0) {
                continue;
            }
            const fh6::LiverySide &L = masks.sides[side];
            const float ax = sgn(L, 0) * axisOf(wp, L.xAxis);
            const float ay = sgn(L, 1) * axisOf(wp, L.yAxis);
            axlo[side] = std::min(axlo[side], ax);
            axhi[side] = std::max(axhi[side], ax);
            aylo[side] = std::min(aylo[side], ay);
            ayhi[side] = std::max(ayhi[side], ay);
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
    sideFacing_.reserve(kLiverySideCount);
    sideSwap_.reserve(kLiverySideCount);
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
        sideEMin_.append(have ? QVector2D(axlo[s], aylo[s]) : QVector2D(-1.0f, -1.0f));
        sideEMax_.append(have ? QVector2D(axhi[s], ayhi[s]) : QVector2D(1.0f, 1.0f));

        // Region axes map horizontal left-to-right and vertical top-to-bottom.
        sideRegion_.append(QVector4D(side.left, side.right, side.top, side.bottom));

        sideFacing_.append(kFacing[s]);
        sideSwap_.append(std::abs(std::abs(side.rotationDeg) - 90.0f) < 1.0f ? 1.0f : 0.0f);
    }
    fns->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    sideCount_ = kLiverySideCount;
}

void CarModelRenderer::uploadModel(const fh6::CarModel &model)
{
    if (!initialized_) {
        return;
    }
    clearModel();

    const std::vector<char> keepLod = highestLodFlags(model);
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const fh6::CarMesh &mesh = model.meshes[mi];
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
                interleaved.push_back((*uv)[i].u);
                interleaved.push_back((*uv)[i].v);
            } else {
                interleaved.push_back(0.0f);
                interleaved.push_back(0.0f);
            }
        }

        auto buffers = std::make_unique<MeshBuffers>();
        buffers->indexCount = static_cast<int>(mesh.indices.size());
        const int bodySides = isBodyPaintMaterial(mesh.materialName) ? allowedSidesForPart(mesh.name) : 0;
        const bool windowGlass = isWindowGlassMaterial(mesh.materialName);
        const int windowSides = windowGlass ? allowedWindowSidesForPart(mesh.name) : 0;
        buffers->allowedSides = bodySides | windowSides;
        buffers->applyLivery = buffers->allowedSides != 0;
        buffers->translucent = windowGlass;
        buffers->alpha = windowGlass ? 0.42f : 1.0f;

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
    const QColor &basePaint)
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
    program_.setUniformValue(basePaintLocation_,
                             static_cast<float>(basePaint.redF()),
                             static_cast<float>(basePaint.greenF()),
                             static_cast<float>(basePaint.blueF()));

    const bool hasLivery = liveryTexture != 0 && sideMaskArray_ != 0 && sideCount_ > 0;
    if (hasLivery) {
        functions->glActiveTexture(GL_TEXTURE0);
        functions->glBindTexture(GL_TEXTURE_2D, liveryTexture);
        program_.setUniformValue(liveryTexLocation_, 0);

        functions->glActiveTexture(GL_TEXTURE1);
        functions->glBindTexture(GL_TEXTURE_2D_ARRAY, sideMaskArray_);
        program_.setUniformValue(sideMasksLocation_, 1);

        program_.setUniformValue(sideCountLocation_, sideCount_);
        program_.setUniformValue(liveryScaleLocation_, liveryScale_);
        program_.setUniformValue(facingMinLocation_, facingMin_);
        program_.setUniformValue(debugModeLocation_, debugMode_);
        program_.setUniformValueArray(sideAxisLocation_, sideAxis_.constData(), sideAxis_.size());
        program_.setUniformValueArray(sideEMinLocation_, sideEMin_.constData(), sideEMin_.size());
        program_.setUniformValueArray(sideEMaxLocation_, sideEMax_.constData(), sideEMax_.size());
        program_.setUniformValueArray(sideRegionLocation_, sideRegion_.constData(), sideRegion_.size());
        program_.setUniformValueArray(sideFacingLocation_, sideFacing_.constData(), sideFacing_.size());
        program_.setUniformValueArray(sideSwapLocation_, sideSwap_.constData(), sideSwap_.size(), 1);
        functions->glActiveTexture(GL_TEXTURE0);
    }

    const QMatrix4x4 viewProjection = projection * view;
    const auto drawMesh = [&](MeshBuffers &mesh) {
        program_.setUniformValue(hasLiveryLocation_, (hasLivery && mesh.applyLivery) ? 1 : 0);
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
        bool ok = false;
        const QVector3D eye = view.inverted(&ok).map(QVector3D(0.0f, 0.0f, 0.0f));
        if (ok) {
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
