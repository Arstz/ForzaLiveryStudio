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

// Body paint applies the livery by runtime planar projection. For each livery
// side the fragment's world position is projected onto the paint canvas via the
// side's Masks.xml axes/region; the side's coverage swatchbin clips that
// projection. The owning side is picked in three steps: the per-mesh allowed_sides
// mask limits which sides the part may take, a facing-angle gate discards sides the
// surface is turned away from, and among the survivors the side the fragment faces
// most directly wins (Left/Right biased over Top on parts that take both). The
// side_* uniform arrays are sized to the full C_livery side count: body paint
// slots followed by the glass/window slots.
constexpr char kFragmentShader[] = R"(#version 330 core
in vec3 v_normal;
in vec3 v_world;

uniform sampler2D livery_tex;          // rasterized vinyl paint canvas
uniform sampler2DArray side_masks;     // per-side coverage swatchbins
uniform vec3 base_paint;
uniform int has_livery;                // 1 when this mesh receives the livery
uniform int side_count;                // sides projected (0 = flat paint)
uniform vec4 side_axis[11];            // (xAxisIdx, yAxisIdx, xSignScale, ySignScale)
uniform vec2 side_emin[11];            // min projected (ax, ay) over the panel
uniform vec2 side_emax[11];            // max projected (ax, ay) over the panel
uniform vec4 side_region[11];          // (left, right, top, bottom) canvas units
uniform vec3 side_facing[11];          // outward panel normal
uniform float side_swap[11];           // 1 when the region is rotated 90 degrees
uniform float livery_scale;            // global fine-tune multiplier (1.0 = exact)
uniform int debug_mode;                // 0 normal, 1 projected-UV, 2 side id
uniform int allowed_sides;             // per-mesh bitmask of sides this part may use
uniform float facing_min;              // min dot(side facing, normal) to keep a side
uniform float material_alpha;           // 1 for opaque, <1 for translucent glass

out vec4 out_color;

// Distinct debug colour per side index (body slots, then window slots).
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

// Paint canvas (canvasX in [-1024,1024], canvasY in [-512,512]) to texture UV.
vec2 canvasToUv(vec2 c)
{
    return vec2((c.x + 1024.0) / 2048.0, (512.0 - c.y) / 1024.0);
}

void main()
{
    vec3 albedo = base_paint;
    if (has_livery == 1 && side_count > 0) {
        vec3 n = normalize(v_normal);
        // Every side competes by facing: a fragment goes to whichever side it faces
        // most directly (highest dot product). The swatch coverage still bounds each
        // side to its authored region, so a side reaches only as far as its mask.
        // Parts that also take Top (the body/fenders) give Left/Right a slight bias
        // so the side wins the side<->Top border instead of Top creeping down onto
        // it; front/back caps have no Top so they are unaffected.
        float bestScore = 0.0;
        float bestCoverage = 0.0;
        vec2 bestUv = vec2(0.0);
        int bestSide = -1;
        for (int s = 0; s < side_count; ++s) {
            // Each body part is limited to the sides the game paints it from
            // (e.g. the trunk never takes Left/Right), so the livery crops at panel
            // seams instead of bleeding across them via side projection.
            if ((allowed_sides & (1 << s)) == 0) {
                continue;
            }
            // facing = cos(angle between the fragment normal and this side's target
            // facing). A loose threshold lets a side reach into curved nooks; the
            // per-side selection below stops that reach from bleeding onto the caps.
            float facing = dot(side_facing[s], n);
            if (s < 6 && facing < facing_min) {
                continue;
            }
            // Project world pos onto the mask's two signed axes.
            vec4 ax = side_axis[s];
            vec2 av = vec2(axisComponent(v_world, ax.x) * ax.z,
                           axisComponent(v_world, ax.y) * ax.w);
            vec2 range = side_emax[s] - side_emin[s];
            if (range.x <= 0.0 || range.y <= 0.0) {
                continue;
            }
            vec2 nrm = (av - side_emin[s]) / range;
            if (side_swap[s] > 0.5) {
                nrm = vec2(nrm.y, 1.0 - nrm.x); // 90-degree region rotation
            }
            nrm = 0.5 + (nrm - 0.5) * livery_scale;
            vec4 reg = side_region[s]; // left, right, top, bottom
            vec2 maskCanvas = vec2(mix(reg.x, reg.y, nrm.x), mix(reg.z, reg.w, nrm.y));
            vec2 maskUv = canvasToUv(maskCanvas);
            if (maskUv.x < 0.0 || maskUv.x > 1.0 || maskUv.y < 0.0 || maskUv.y > 1.0) {
                continue;
            }
            float coverage = texture(side_masks, vec3(maskUv, float(s))).r;
            if (coverage <= 0.5) {
                continue; // only surfaces the swatch authored for this side
            }
            // Imported livery sections are decoded in editor canvas orientation. Most
            // model sides need a Y correction, but Back/Left/WindowLeft are authored
            // mirrored on X instead.
            vec2 paintNrm = (s == 1 || s == 3 || s == 9) ? vec2(1.0 - nrm.x, nrm.y)
                                                          : vec2(nrm.x, 1.0 - nrm.y);
            vec2 paintCanvas = vec2(mix(reg.x, reg.y, paintNrm.x), mix(reg.z, reg.w, paintNrm.y));
            vec2 paintUv = canvasToUv(paintCanvas);
            float score = s >= 6 ? 1.0 : facing;
            // Bias Left/Right up only on parts that also take Top (allowed bit 2),
            // i.e. the body/fenders side<->Top border.
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
            // Show the projected paint-canvas UV as red/green: a smooth gradient
            // across a panel means the projection scale is sane.
            out_color = vec4(bestUv, 0.0, 1.0);
            return;
        }
        if (debug_mode == 2 && bestSide >= 0) {
            out_color = vec4(sideColor(bestSide), 1.0);
            return;
        }
        if (bestCoverage > 0.5) {
            // Premultiplied paint over transparent black: base*(1-a) + rgb.
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

// The body livery decal only belongs on paint materials. The main body panel uses
// "carPaint" (and variants like "carPaint_secondary"); brake calipers use
// "caliper_paint"/"carpaint_texture" and must be excluded.
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

// Outward panel normal per livery side. Forza car space (confirmed against
// Locators.xml + the decoded geometry): +X right, +Y up, FRONT of the car at +Z,
// rear at -Z. Top and Spoiler both face up; the spoiler is picked out by mesh
// name while measuring extents. Window slots use the matching exterior direction.
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

// --- Highest-LOD selection -------------------------------------------------
// Forza parts ship several baked LODs: mesh names end in "_LODS0" for the source
// LOD and "_LOD1".."_LODn" for reductions. The assembled car keeps them all, so a
// static preview that draws every mesh gets ~6x body overdraw (paint z-fighting)
// and a livery extent measurement polluted by the reduced copies. Rank a name's
// LOD suffix (higher = more detailed) and, per base part, keep only the top rank.
constexpr int kStandaloneLodRank = 500;

int lodRank(const QString &name)
{
    const int idx = name.lastIndexOf(QStringLiteral("_LOD"));
    if (idx < 0) {
        return kStandaloneLodRank; // no LOD suffix: a standalone mesh, always kept
    }
    const QString suffix = name.mid(idx + 4);
    if (suffix.startsWith(QLatin1Char('S')) || suffix.startsWith(QLatin1Char('s'))) {
        bool ok = false;
        const int n = suffix.mid(1).toInt(&ok);
        return 1000 - (ok ? n : 0); // "_LODS"/"_LODS0" is the source (best) LOD
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

// Per mesh (by model.meshes index), true when it is the most detailed LOD present
// for its base part. Parts that exist only as a numbered LOD keep that LOD, so no
// part is ever dropped entirely.
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

// A carPaint mesh belongs to the rear wing/spoiler side (slot 5). Match the real
// "wing_a"/"spoiler" body panel but NOT the side mirrors ("wingMirror*"), which are
// also carPaint and would otherwise stretch the spoiler's projected extent across
// the whole car (front mirrors + rear wing) and wreck its livery mapping.
bool isSpoilerMesh(const QString &name)
{
    if (name.contains(QStringLiteral("mirror"), Qt::CaseInsensitive)) {
        return false;
    }
    return name.contains(QStringLiteral("spoiler"), Qt::CaseInsensitive)
        || name.contains(QStringLiteral("wing"), Qt::CaseInsensitive);
}

// Per-side bits, indexed to match kFacing / the shader's side index.
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

// Which livery sides a body part may take, keyed on its model/mesh name (the
// finest part identity in the model files; the carbin's CCarParts type only
// distinguishes hood/bumpers/skirts/wing and lumps everything else as CarBody).
// Limiting the candidate sides per part is what makes the livery crop at panel
// seams (trunk/hood) rather than bleed via side projection. Unknown names on
// other cars fall back to all sides, i.e. the previous geometric behaviour.
int allowedSidesForPart(const QString &rawName)
{
    const QString n = rawName.toLower();
    const auto has = [&](const char *s) { return n.contains(QLatin1String(s)); };

    // Rear light panel: the painted housing takes Back and wraps to the sides. The
    // actual lenses are a separate non-paint material and are excluded by
    // isBodyPaintMaterial anyway, so this only paints the panel, not the lights.
    // Checked before the "light" exclusion below.
    if (has("taillight")) return kSideBack | kSideLeft | kSideRight;
    // Interior/trim/optics: no exterior livery. Checked first so "wingMirror",
    // "hoodLiner", "trunkBay", "engineBay_label", "headlight" etc. never reach the
    // panel rules below.
    if (has("mirror") || has("liner") || has("jamb") || has("hinge")
        || has("handle") || has("bay") || has("label") || has("light")
        || has("glass") || has("wiper") || has("engine")) {
        return 0;
    }
    // Exterior panels.
    if (has("wing")) return kSideSpoiler;                 // rear wing / spoiler
    if (has("hood")) return kSideTop;                     // hood: top only
    if (has("trunk")) return kSideBack | kSideTop;        // trunk lid
    if (has("bumperf")) return kSideFront | kSideLeft | kSideRight; // wraps to sides
    if (has("bumperr")) return kSideBack | kSideLeft | kSideRight;  // wraps to sides
    if (has("skirt") || has("sill")) return kSideLeft | kSideRight;
    if (has("door")) {
        if (n.contains(QLatin1String("doorl"))) return kSideLeft;  // e.g. doorLF
        if (n.contains(QLatin1String("doorr"))) return kSideRight; // e.g. doorRF
        return kSideLeft | kSideRight;
    }
    if (has("fender")) return kSideLeft | kSideRight | kSideTop;
    if (has("body")) return kSideLeft | kSideRight | kSideTop | kSideBack;
    return kAllBodySides; // unknown part: geometric fallback
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

    // Use the first loaded side's mask dimensions as the source size; every
    // livery swatchbin is 2048x1024.
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
        return; // no coverage masks
    }
    const int texW = sourceW * kMaskTextureScale;
    const int texH = sourceH * kMaskTextureScale;

    // Measure each panel's real extent along its Masks.xml signed axes. Body paint
    // uses the existing facing/name rules; window glass is assigned by mesh name.
    // This makes each region fit its panel/window, not the whole car.
    const auto sgn = [](const fh6::LiverySide &L, int which) {
        return which == 0 ? L.xSign * L.xScale : L.ySign * L.yScale;
    };
    float axlo[kLiverySideCount], axhi[kLiverySideCount], aylo[kLiverySideCount], ayhi[kLiverySideCount];
    for (int s = 0; s < kLiverySideCount; ++s) {
        axlo[s] = aylo[s] = std::numeric_limits<float>::max();
        axhi[s] = ayhi[s] = std::numeric_limits<float>::lowest();
    }
    // Measure the extent from the highest LOD only, so reduced LODs don't skew it.
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
                float best = 0.3f; // require a clear facing
                for (int s = 0; s < 5; ++s) { // exclude Spoiler from normal match
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

        // Fall back to a unit range if a side had no assigned geometry.
        const bool have = axhi[s] > axlo[s] && ayhi[s] > aylo[s];
        sideEMin_.append(have ? QVector2D(axlo[s], aylo[s]) : QVector2D(-1.0f, -1.0f));
        sideEMax_.append(have ? QVector2D(axhi[s], ayhi[s]) : QVector2D(1.0f, 1.0f));

        // Canvas region (left, right, top, bottom): ax maps left->right, ay maps
        // top->bottom (validated offline against the swatchbin coverage).
        sideRegion_.append(QVector4D(side.left, side.right, side.top, side.bottom));

        sideFacing_.append(kFacing[s]);
        // A +/-90 authored rotation becomes an axis swap; 0/180 are handled by the
        // axis signs alone.
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

    // Draw only the most detailed LOD of each part; the assembled car carries every
    // baked LOD, and rendering them all overdraws the body and z-fights the paint.
    const std::vector<char> keepLod = highestLodFlags(model);
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const fh6::CarMesh &mesh = model.meshes[mi];
        if (!keepLod[mi] || mesh.positions.empty() || mesh.indices.empty()) {
            continue;
        }
        // The car ships exterior and interior window shells at near-identical
        // positions. Keep the exterior shell for the preview; drawing both causes
        // angle-dependent depth fighting across the glass.
        if (isInteriorWindowShell(mesh.name)) {
            continue;
        }

        const std::vector<fh6::ModelVec2> *uv = nullptr;
        if (mesh.liveryUvChannel >= 0
            && mesh.liveryUvChannel < static_cast<int>(mesh.uvChannels.size())
            && mesh.uvChannels[mesh.liveryUvChannel].size() == mesh.positions.size()) {
            uv = &mesh.uvChannels[mesh.liveryUvChannel];
        }

        // Interleave position(3) / normal(3) / UV padding(2). The livery colour
        // is sampled by runtime projection; the UV attribute is retained only to
        // keep the vertex layout stable.
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

        // Bone world matrix is stored in .NET row-vector layout; transpose it into
        // QMatrix4x4's column-vector (M*v) convention.
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
    // Forza body meshes are authored two-sided; skip culling to avoid holes.
    functions->glDisable(GL_CULL_FACE);

    program_.bind();
    program_.setUniformValue(basePaintLocation_,
                             static_cast<float>(basePaint.redF()),
                             static_cast<float>(basePaint.greenF()),
                             static_cast<float>(basePaint.blueF()));

    // The livery needs both the paint canvas and the per-side coverage masks.
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
