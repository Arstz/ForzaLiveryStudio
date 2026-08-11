#include "car_model_renderer.h"

#include "material_hashes.h"
#include "model_material.h"
#include "region_fill.h"

#include <QDebug>
#include <QHash>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions>

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace gui {
namespace {

constexpr double kUnwrapPathScale = 1024.0;

constexpr char kVertexShader[] = R"(#version 330 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec2 in_material_uv;

uniform mat4 mvp;
uniform mat4 model;

out vec3 v_normal;
out vec3 v_world;
out vec2 v_uv;
out vec2 v_material_uv;

void main()
{
    vec4 world = model * vec4(in_position, 1.0);
    gl_Position = mvp * vec4(in_position, 1.0);
    v_normal = mat3(model) * in_normal;
    v_world = world.xyz;
    v_uv = in_uv;
    v_material_uv = in_material_uv;
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
in vec3 v_normal;
in vec3 v_world;
in vec2 v_uv;
in vec2 v_material_uv;

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
uniform int wireframe_overlay;
uniform int allowed_sides;
uniform float material_alpha;
uniform sampler2D native_diffuse;
uniform sampler2D native_alpha;
uniform sampler2D native_normal;
uniform sampler2D native_surface;
uniform sampler2D native_emissive;
uniform int has_native_diffuse;
uniform int has_native_alpha;
uniform int has_native_normal;
uniform int has_native_surface;
uniform int has_native_emissive;
uniform sampler2D finish_pattern;
uniform sampler2D finish_normal;
uniform sampler2D finish_surface;
uniform int has_finish_pattern;
uniform int has_finish_normal;
uniform int has_finish_surface;
uniform int finish_self_colored;
uniform float finish_tiling;
uniform float finish_flake;
uniform float clear_coat_roughness;
uniform float clear_coat_intensity;

out vec4 out_color;

const vec3 DEBUG_FRONT_COLOR = vec3(1.0, 0.2, 0.2);
const vec3 DEBUG_BACK_COLOR = vec3(0.2, 1.0, 0.2);
const vec3 DEBUG_TOP_COLOR = vec3(0.3, 0.5, 1.0);
const vec3 DEBUG_LEFT_COLOR = vec3(1.0, 1.0, 0.2);
const vec3 DEBUG_RIGHT_COLOR = vec3(1.0, 0.3, 1.0);
const vec3 DEBUG_SPOILER_COLOR = vec3(0.2, 1.0, 1.0);
const vec3 DEBUG_FRONT_GLASS_COLOR = vec3(1.0, 0.5, 0.2);
const vec3 DEBUG_BACK_GLASS_COLOR = vec3(0.5, 1.0, 0.5);
const vec3 DEBUG_TOP_GLASS_COLOR = vec3(0.5, 0.8, 1.0);
const vec3 DEBUG_LEFT_GLASS_COLOR = vec3(1.0, 0.9, 0.4);
const vec3 DEBUG_RIGHT_GLASS_COLOR = vec3(1.0, 0.5, 1.0);
const vec3 ENVIRONMENT_LOW_COLOR = vec3(0.025, 0.028, 0.035);
const vec3 ENVIRONMENT_HIGH_COLOR = vec3(0.58, 0.64, 0.72);

const float PI = 3.14159265359;

float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 sideColor(int s)
{
    if (s == 0) return DEBUG_FRONT_COLOR;
    if (s == 1) return DEBUG_BACK_COLOR;
    if (s == 2) return DEBUG_TOP_COLOR;
    if (s == 3) return DEBUG_LEFT_COLOR;
    if (s == 4) return DEBUG_RIGHT_COLOR;
    if (s == 5) return DEBUG_SPOILER_COLOR;
    if (s == 6) return DEBUG_FRONT_GLASS_COLOR;
    if (s == 7) return DEBUG_BACK_GLASS_COLOR;
    if (s == 8) return DEBUG_TOP_GLASS_COLOR;
    if (s == 9) return DEBUG_LEFT_GLASS_COLOR;
    return DEBUG_RIGHT_GLASS_COLOR;
}

float axisComponent(vec3 v, float axis)
{
    return axis < 0.5 ? v.x : (axis < 1.5 ? v.y : v.z);
}

vec2 canvasToUv(vec2 c)
{
    return vec2((c.x + 1024.0) / 2048.0, (512.0 - c.y) / 1024.0);
}

mat3 materialTangentFrame(vec3 normal, vec3 position, vec2 uv)
{
    vec3 dp1 = dFdx(position);
    vec3 dp2 = dFdy(position);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, normal);
    vec3 dp1perp = cross(normal, dp1);
    vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = inversesqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 0.000001));
    return mat3(tangent * scale, bitangent * scale, normal);
}

vec3 finishBaseColor(vec2 uv, vec3 paint)
{
    if (has_finish_pattern == 0) {
        return paint;
    }
    vec3 pattern = texture(finish_pattern, fract(uv * finish_tiling)).rgb;
    return finish_self_colored == 1 ? pattern : paint * pattern;
}

// Procedural metal-flake sparkle: quantise world space into tiny cells, keep only the
// cells dense enough to hold a flake, and light each one when its (randomly offset)
// facing lines up with the highlight so the sparkle twinkles as the view moves.
float finishGlitter(vec3 worldPos, float facing)
{
    if (finish_flake <= 0.0) {
        return 0.0;
    }
    vec3 cell = floor(worldPos * 900.0);
    float rnd = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
    float rnd2 = fract(rnd * 251.7);
    // Sparse: only a fraction of cells hold a flake, and each lights only very close to
    // the highlight so the result reads as occasional sparkle, not salt-and-pepper noise.
    float active = step(1.0 - 0.3 * finish_flake, rnd);
    float lit = smoothstep(0.92, 1.0, facing + (rnd2 - 0.5) * 0.05);
    return active * lit;
}

void main()
{
    // Surfaces drawn double-sided are seen from behind as often as in front — a wheel barrel is
    // only ever viewed from inside — so light the side that is actually facing the eye.
    vec3 n = normalize(v_normal);
    if (!gl_FrontFacing) {
        n = -n;
    }
    vec2 materialUv = v_material_uv;
    if (has_native_normal == 1) {
        vec3 mappedNormal = texture(native_normal, materialUv).xyz * 2.0 - 1.0;
        mappedNormal.z = sqrt(max(1.0 - dot(mappedNormal.xy, mappedNormal.xy), 0.0));
        n = normalize(materialTangentFrame(n, v_world, materialUv) * mappedNormal);
    }
    vec3 viewDir = normalize(eye_position - v_world);
    float edge = pow(1.0 - max(dot(n, viewDir), 0.0), 2.0);
    vec3 surfacePaint = mix(base_paint, secondary_paint, secondary_mix * edge);
    float outputAlpha = material_alpha;
    if (has_native_diffuse == 1) {
        vec4 nativeColor = texture(native_diffuse, materialUv);
        surfacePaint *= nativeColor.rgb;
        outputAlpha *= nativeColor.a;
    }
    if (has_native_alpha == 1) {
        outputAlpha *= texture(native_alpha, materialUv).r;
    }
    if (outputAlpha < 0.02) {
        discard;
    }
    vec3 albedo = surfacePaint;
    vec2 finishUv = vec2(0.0);
    bool finishUvValid = false;
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
                    if (dot(side_facing[s], normalize(v_normal)) <= 0.0) {
                        continue;
                    }
                    float candidate = texture(side_masks, vec3(atlasUv, float(s))).r;
                    if (candidate > coverage) {
                        coverage = candidate;
                        coveredSide = s;
                    }
                }
            }
            if (wireframe_overlay == 1) {
                if (coverage <= 0.5 || dot(n, viewDir) <= 0.0) {
                    discard;
                }
                out_color = vec4(1.0);
                return;
            }
            vec2 paintUv = vec2(atlasUv.x, 1.0 - atlasUv.y);
            if (coveredSide >= 0) {
                vec4 sourceRegion = side_region[coveredSide];
                vec2 sourceStart = canvasToUv(vec2(sourceRegion.x, sourceRegion.z));
                vec2 sourceEnd = canvasToUv(vec2(sourceRegion.y, sourceRegion.w));
                vec2 sourceRange = sourceEnd - sourceStart;
                if (abs(sourceRange.x) > 0.000001 && abs(sourceRange.y) > 0.000001) {
                    vec2 sectionUv = (atlasUv - sourceStart) / sourceRange;
                    if (coveredSide == 5 || coveredSide == 6 || coveredSide == 7) {
                        sectionUv = sectionUv.yx;
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
            if (coveredSide >= 0) {
                finishUv = paintUv;
                finishUvValid = true;
                albedo = finishBaseColor(paintUv, surfacePaint);
            }
            if (coverage > 0.5) {
                vec4 paint = texture(livery_tex, paintUv);
                albedo = albedo * (1.0 - paint.a) + paint.rgb;
            }
        } else {
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
            if (s == 5 || s == 6 || s == 7) {
                sectionUv = sectionUv.yx;
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
        if (wireframe_overlay == 1) {
            if (bestCoverage <= 0.5 || dot(n, viewDir) <= 0.0) {
                discard;
            }
            out_color = vec4(1.0);
            return;
        }
        if (debug_mode == 1 && bestSide >= 0) {
            out_color = vec4(bestUv, 0.0, 1.0);
            return;
        }
        if (debug_mode == 2 && bestSide >= 0) {
            out_color = vec4(sideColor(bestSide), 1.0);
            return;
        }
        if (bestSide >= 0) {
            finishUv = bestUv;
            finishUvValid = true;
            albedo = finishBaseColor(bestUv, surfacePaint);
        }
        if (bestCoverage > 0.5) {
            vec4 paint = texture(livery_tex, bestUv);
            albedo = albedo * (1.0 - paint.a) + paint.rgb;
        }
        }
    }
    float surfaceGloss = material_gloss;
    float surfaceMetallic = material_metallic;
    float surfaceAo = 1.0;
    if (has_native_surface == 1) {
        vec3 packedSurface = texture(native_surface, materialUv).rgb;
        surfaceGloss = 1.0 - packedSurface.r;
        surfaceMetallic = packedSurface.g;
        surfaceAo = packedSurface.b;
    }
    if (finishUvValid) {
        vec2 finishTiledUv = fract(finishUv * finish_tiling);
        if (has_finish_surface == 1) {
            vec3 packedFinish = texture(finish_surface, finishTiledUv).rgb;
            surfaceGloss = 1.0 - packedFinish.r;
            surfaceMetallic = packedFinish.g;
            surfaceAo = packedFinish.b;
        }
        if (has_finish_normal == 1) {
            vec3 mapped = texture(finish_normal, finishTiledUv).xyz * 2.0 - 1.0;
            mapped.z = sqrt(max(1.0 - dot(mapped.xy, mapped.xy), 0.0));
            n = normalize(materialTangentFrame(n, v_world, finishTiledUv) * mapped);
        }
    }
    vec3 nativeEmission = has_native_emissive == 1
        ? texture(native_emissive, materialUv).rgb
        : vec3(0.0);
    vec3 l = normalize(vec3(0.4, 0.8, 0.6));
    vec3 v = viewDir;
    float NdotV = max(dot(n, v), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, surfaceMetallic);
    float roughness = 1.0 - surfaceGloss;
    roughness = clamp(roughness, 0.04, 1.0);
    float NdotL = max(dot(n, l), 0.0);
    vec3 H = normalize(l + v);
    float D = distributionGGX(n, H, roughness);
    float G = geometrySmith(n, v, l, roughness);
    vec3 F = fresnelSchlick(max(dot(H, v), 0.0), F0);
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - surfaceMetallic);
    vec3 diffuse = kD * albedo / PI;
    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
    vec3 lo = (diffuse + specular) * vec3(3.0) * NdotL;
    vec3 ambient = vec3(0.25) * albedo * surfaceAo;
    vec3 color = ambient + lo;
    vec3 reflected = reflect(-v, n);
    float envUp = reflected.y * 0.5 + 0.5;
    vec3 envColor = mix(ENVIRONMENT_LOW_COLOR, ENVIRONMENT_HIGH_COLOR, clamp(envUp, 0.0, 1.0));
    vec3 Fenv = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 envSpecular = envColor * Fenv * mix(0.15, 0.95, surfaceGloss) * surfaceAo;
    vec3 envDiffuse = albedo * envColor * 0.15 * (1.0 - surfaceMetallic);
    color += envDiffuse + envSpecular;
    float glitter = finishGlitter(v_world, max(dot(n, H), 0.0));
    color += vec3(glitter * finish_flake * 1.4) * Fenv;
    if (clear_coat_intensity > 0.01) {
        float ccRoughness = clamp(clear_coat_roughness, 0.04, 1.0);
        float ccD = distributionGGX(n, H, ccRoughness);
        float ccG = geometrySmith(n, v, l, ccRoughness);
        vec3 ccF = fresnelSchlick(max(dot(H, v), 0.0), vec3(0.04));
        vec3 ccSpecular = (ccD * ccG * ccF) / max(4.0 * NdotV * NdotL, 0.001);
        color += ccSpecular * vec3(3.0) * NdotL * clear_coat_intensity;
        vec3 ccEnvF = fresnelSchlickRoughness(NdotV, vec3(0.04), ccRoughness);
        color += envColor * ccEnvF * mix(0.15, 0.95, 1.0 - ccRoughness) * clear_coat_intensity;
        float ccFresnelEdge = pow(1.0 - NdotV, 4.0);
        color += vec3(ccFresnelEdge * 0.06 * clear_coat_intensity);
    }
    color += material_emissive + nativeEmission;
    out_color = vec4(color, outputAlpha);
}
)";

constexpr int kMaskTextureScale = 2;

bool isBodyPaintMaterial(const QString &material) {
    const QString name = material.toLower();
    if (name.isEmpty() || name.contains(QStringLiteral("caliper")) || name.contains(QStringLiteral("texture"))) {
        return false;
    }
    return name.startsWith(QStringLiteral("carpaint")) || name.startsWith(QStringLiteral("car_paint"));
}

bool isWindowGlassMaterial(const fls::CarMesh &mesh) {
    const QString name = mesh.materialName.toLower();
    if (name.contains(QStringLiteral("screw"))
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
    const bool windowGlassResource =
        resource.contains(QStringLiteral("/glass/glass_window"))
        || resource.contains(QStringLiteral("/glass/glass_windshield"))
        || resource.contains(QStringLiteral("/glass/glass_limotint"));
    return mesh.paintMaterialHash == fls::material_hashes::binding::kWindowGlass
        || windowGlassResource
        || name.contains(QStringLiteral("window"))
        || name.contains(QStringLiteral("windshield"))
        || name.contains(QStringLiteral("windsheild"))
        || (name.contains(QStringLiteral("blackglass")) && glassResource);
}

bool isInteriorWindowShell(const QString &rawName) {
    QString name = rawName.toLower();
    const int pipe = name.indexOf(QLatin1Char('|'));
    if (pipe >= 0) {
        name = name.left(pipe);
    }
    return name.startsWith(QStringLiteral("glass")) && name.contains(QStringLiteral("int"));
}

// The interior ships as detailed parts (dash, seats, roll cage…) plus a single coarse whole-cabin
// stand-in — material InteriorLOD* — that the game only swaps in at distance. That stand-in is
// authored at the base LOD level, so the LOD filter keeps it, and up close it renders as a pale
// low-poly shell overlapping the real interior (the "torpedo" clipping). Drop it so only the
// detailed interior shows.
bool isCoarseInteriorLod(const fls::CarMesh &mesh) {
    return mesh.materialName.startsWith(QStringLiteral("InteriorLOD"), Qt::CaseInsensitive);
}

QString materialIdentity(const fls::CarMesh &mesh) {
    QString identity = mesh.materialName.toLower();
    if (mesh.material) {
        identity += QLatin1Char('|') + mesh.material->name.toLower();
        identity += QLatin1Char('|') + mesh.material->resourcePath.toLower();
    }
    identity.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return identity;
}

QString materialResourceIdentity(const fls::CarMesh &mesh) {
    if (!mesh.material) {
        return {};
    }
    QString resource = mesh.material->resourcePath.toLower();
    resource.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return resource;
}

bool isLampSurface(const fls::CarMesh &mesh) {
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

bool isLampEmitterMaterial(const fls::CarMesh &mesh) {
    const QString material = materialIdentity(mesh);
    return material.contains(QStringLiteral("lights"))
        || material.contains(QStringLiteral("lightbulb"))
        || material.contains(QStringLiteral("emitter"));
}

struct MaterialFallback {
    QVector3D color;
    float gloss = 0.45f;
    float metallic = 0.0f;
    float minimumColor = 0.0f;
};

const QVector3D kMirrorColor(0.42f, 0.48f, 0.56f);
const QVector3D kChromeColor(0.68f, 0.71f, 0.76f);
const QVector3D kGoldColor(0.64f, 0.43f, 0.10f);
const QVector3D kAluminumColor(0.42f, 0.44f, 0.46f);
const QVector3D kTitaniumColor(0.30f, 0.31f, 0.34f);
const QVector3D kGunmetalColor(0.10f, 0.11f, 0.13f);
const QVector3D kSteelColor(0.28f, 0.30f, 0.33f);
const QVector3D kPaintedMetalColor(0.055f, 0.058f, 0.064f);
const QVector3D kCarbonColor(0.085f, 0.092f, 0.10f);
const QVector3D kRubberColor(0.018f, 0.019f, 0.021f);
const QVector3D kBlackExteriorColor(0.008f, 0.009f, 0.011f);
const QVector3D kPlasticColor(0.025f, 0.027f, 0.030f);
const QVector3D kBadgeColor(0.46f, 0.49f, 0.53f);
const QVector3D kPlateColor(0.78f, 0.79f, 0.75f);
const QVector3D kBlackWheelColor(0.015f, 0.015f, 0.018f);
const QVector3D kRimColor(0.32f, 0.34f, 0.37f);
const QVector3D kInnerRimColor(0.12f, 0.13f, 0.15f);
const QVector3D kLugColor(0.38f, 0.40f, 0.43f);
const QVector3D kDefaultMaterialColor(0.55f, 0.55f, 0.55f);
const QVector3D kTextureMaterialColor(1.0f, 1.0f, 1.0f);
const QVector3D kLampChromeColor(0.72f, 0.74f, 0.78f);
const QVector3D kLampGlassColor(0.78f, 0.82f, 0.88f);
const QVector3D kTailLampEmissionColor(1.0f, 0.025f, 0.012f);
const QVector3D kIndicatorEmissionColor(1.0f, 0.30f, 0.025f);
const QVector3D kHeadLampEmissionColor(1.0f, 0.86f, 0.68f);

float linearToDisplay(float value) {
    const float linear = std::clamp(value, 0.0f, 1.0f);
    return linear <= 0.0031308f
        ? linear * 12.92f
        : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

std::optional<MaterialFallback> exteriorMaterialFallback(const fls::CarMesh &mesh) {
    const QString resource = materialResourceIdentity(mesh);
    const QString material = resource.isEmpty() ? materialIdentity(mesh) : resource;
    if (material.contains(QStringLiteral("mirror_left"))
        || material.contains(QStringLiteral("mirror_right"))) {
        return MaterialFallback{kMirrorColor, 0.98f, 0.82f};
    }
    if (material.contains(QStringLiteral("chrome"))) {
        return MaterialFallback{kChromeColor, 0.96f, 1.0f};
    }
    if (material.contains(QStringLiteral("gold"))) {
        return MaterialFallback{kGoldColor, 0.86f, 0.92f};
    }
    if (material.contains(QStringLiteral("aluminum"))) {
        return MaterialFallback{kAluminumColor, 0.62f, 0.82f};
    }
    if (material.contains(QStringLiteral("titanium"))) {
        return MaterialFallback{kTitaniumColor, 0.68f, 0.88f};
    }
    if (material.contains(QStringLiteral("gunmetal"))
        || material.contains(QStringLiteral("anodizedmetal"))) {
        return MaterialFallback{kGunmetalColor, 0.72f, 0.86f};
    }
    if (material.contains(QStringLiteral("steel"))
        || material.contains(QStringLiteral("metallic"))) {
        return MaterialFallback{kSteelColor, 0.70f, 0.84f};
    }
    if (material.contains(QStringLiteral("paintedmetal"))) {
        return MaterialFallback{kPaintedMetalColor, 0.58f, 0.45f};
    }
    if (material.contains(QStringLiteral("carbon"))) {
        return MaterialFallback{kCarbonColor, 0.78f, 0.0f, 0.08f};
    }
    if (material.contains(QStringLiteral("rubber"))) {
        return MaterialFallback{kRubberColor, 0.18f, 0.0f};
    }
    if (material.contains(QStringLiteral("blackframe"))
        || material.contains(QStringLiteral("blackhole"))
        || material.contains(QStringLiteral("grille"))) {
        return MaterialFallback{kBlackExteriorColor, 0.22f, 0.05f};
    }
    if (material.contains(QStringLiteral("plastic"))) {
        return MaterialFallback{kPlasticColor, 0.30f, 0.0f};
    }
    if (material.contains(QStringLiteral("badge"))) {
        return MaterialFallback{kBadgeColor, 0.86f, 0.78f};
    }
    if (material.contains(QStringLiteral("plate"))) {
        return MaterialFallback{kPlateColor, 0.35f, 0.0f};
    }
    return std::nullopt;
}

} // namespace

QString CarModelRenderer::shaderSelfTest() {
    QOpenGLShaderProgram program;
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        || !program.link()) {
        return program.log();
    }

    return QString();
}

CarModelRenderer::CarModelRenderer() = default;

CarModelRenderer::~CarModelRenderer() {
    release();
}

void CarModelRenderer::initialize() {
    if (initialized_) {
        return;
    }
    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        || !program_.link()) {
        qWarning().noquote() << "CarModelRenderer shader failed to build:" << program_.log();
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
    wireframeOverlayLocation_ = program_.uniformLocation("wireframe_overlay");
    allowedSidesLocation_ = program_.uniformLocation("allowed_sides");
    materialAlphaLocation_ = program_.uniformLocation("material_alpha");
    secondaryPaintLocation_ = program_.uniformLocation("secondary_paint");
    secondaryMixLocation_ = program_.uniformLocation("secondary_mix");
    glossLocation_ = program_.uniformLocation("material_gloss");
    metallicLocation_ = program_.uniformLocation("material_metallic");
    emissiveLocation_ = program_.uniformLocation("material_emissive");
    eyePositionLocation_ = program_.uniformLocation("eye_position");
    nativeDiffuseLocation_ = program_.uniformLocation("native_diffuse");
    nativeAlphaLocation_ = program_.uniformLocation("native_alpha");
    nativeNormalLocation_ = program_.uniformLocation("native_normal");
    nativeSurfaceLocation_ = program_.uniformLocation("native_surface");
    nativeEmissiveLocation_ = program_.uniformLocation("native_emissive");
    hasNativeDiffuseLocation_ = program_.uniformLocation("has_native_diffuse");
    hasNativeAlphaLocation_ = program_.uniformLocation("has_native_alpha");
    hasNativeNormalLocation_ = program_.uniformLocation("has_native_normal");
    hasNativeSurfaceLocation_ = program_.uniformLocation("has_native_surface");
    hasNativeEmissiveLocation_ = program_.uniformLocation("has_native_emissive");
    finishPatternLocation_ = program_.uniformLocation("finish_pattern");
    finishNormalLocation_ = program_.uniformLocation("finish_normal");
    finishSurfaceLocation_ = program_.uniformLocation("finish_surface");
    hasFinishPatternLocation_ = program_.uniformLocation("has_finish_pattern");
    hasFinishNormalLocation_ = program_.uniformLocation("has_finish_normal");
    hasFinishSurfaceLocation_ = program_.uniformLocation("has_finish_surface");
    finishSelfColoredLocation_ = program_.uniformLocation("finish_self_colored");
    finishTilingLocation_ = program_.uniformLocation("finish_tiling");
    finishFlakeLocation_ = program_.uniformLocation("finish_flake");
    clearCoatRoughnessLocation_ = program_.uniformLocation("clear_coat_roughness");
    clearCoatIntensityLocation_ = program_.uniformLocation("clear_coat_intensity");
    initialized_ = true;
}

void CarModelRenderer::release() {
    clearModel();
    if (!materialTextureCache_.isEmpty()) {
        if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
            QVector<GLuint> ids;
            ids.reserve(materialTextureCache_.size());
            for (const MaterialTextureCacheEntry &entry : std::as_const(materialTextureCache_)) {
                ids.push_back(entry.id);
            }
            context->functions()->glDeleteTextures(ids.size(), ids.constData());
        }
        materialTextureCache_.clear();
        materialTextureCacheBytes_ = 0;
    }
    clearPaintFinishTextures();
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
    wireframeOverlayLocation_ = -1;
    allowedSidesLocation_ = -1;
    materialAlphaLocation_ = -1;
    secondaryPaintLocation_ = -1;
    secondaryMixLocation_ = -1;
    glossLocation_ = -1;
    metallicLocation_ = -1;
    emissiveLocation_ = -1;
    eyePositionLocation_ = -1;
    nativeDiffuseLocation_ = -1;
    nativeAlphaLocation_ = -1;
    nativeNormalLocation_ = -1;
    nativeSurfaceLocation_ = -1;
    nativeEmissiveLocation_ = -1;
    hasNativeDiffuseLocation_ = -1;
    hasNativeAlphaLocation_ = -1;
    hasNativeNormalLocation_ = -1;
    hasNativeSurfaceLocation_ = -1;
    hasNativeEmissiveLocation_ = -1;
    clearCoatRoughnessLocation_ = -1;
    clearCoatIntensityLocation_ = -1;
}

bool CarModelRenderer::isInitialized() const {
    return initialized_;
}

bool CarModelRenderer::hasModel() const {
    return !meshes_.empty();
}

void CarModelRenderer::clearModel() {
    const auto destroyBuffers = [](auto &mesh) {
        if (mesh->vbo.isCreated()) {
            mesh->vbo.destroy();
        }
        if (mesh->ibo.isCreated()) {
            mesh->ibo.destroy();
        }
        if (mesh->vao.isCreated()) {
            mesh->vao.destroy();
        }
    };
    for (auto &mesh : meshes_) {
        destroyBuffers(mesh);
    }
    for (auto &mesh : wireframeMeshes_) {
        destroyBuffers(mesh);
    }
    meshes_.clear();
    wireframeMeshes_.clear();
}

void CarModelRenderer::clearLivery() {
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
const QVector3D kFacing[fls::kLiverySideCount] = {
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

constexpr int kMaskSlotGeometrySides[fls::kLiverySideCount] = {
    0, 1, 2, 4, 3, 5, 6, 7, 8, 10, 9,
};

fls::ModelVec3 mirroredCarSpace(fls::ModelVec3 value) {
    value.x = -value.x;
    return value;
}

QMatrix4x4 carMeshModelMatrix(const fls::CarMesh &mesh) {
    const auto &matrix = mesh.boneTransform.m;
    const QMatrix4x4 boneModel(
        matrix[0], matrix[4], matrix[8], matrix[12],
        matrix[1], matrix[5], matrix[9], matrix[13],
        matrix[2], matrix[6], matrix[10], matrix[14],
        matrix[3], matrix[7], matrix[11], matrix[15]);
    QMatrix4x4 horizontalMirror;
    horizontalMirror.scale(-1.0f, 1.0f, 1.0f);

    return horizontalMirror * boneModel;
}

float axisOf(const fls::ModelVec3 &v, int axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

float sideAxisScale(const fls::LiverySide &side, bool horizontal) {
    return horizontal
        ? side.xSign * side.xScale
        : side.ySign * side.yScale;
}

bool isSpoilerMesh(const QString &name) {
    if (name.contains(QStringLiteral("mirror"), Qt::CaseInsensitive)) {
        return false;
    }
    return name.contains(QStringLiteral("spoiler"), Qt::CaseInsensitive)
        || name.contains(QStringLiteral("wing"), Qt::CaseInsensitive);
}

bool isTrunkPanelMesh(const QString &name) {
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

quint64 fallbackPaintHash(const fls::CarMesh &mesh) {
    const QString identity = mesh.name.toLower() + QLatin1Char('|') + mesh.materialName.toLower();
    if (isWindowGlassMaterial(mesh)) {
        return fls::material_hashes::binding::kWindowGlass;
    }
    if (identity.contains(QStringLiteral("caliper")) || identity.contains(QStringLiteral("brake"))) {
        return fls::material_hashes::binding::kBrakeCaliper;
    }
    if (isBodyPaintMaterial(mesh.materialName)) {
        if (isSpoilerMesh(mesh.name)) {
            return fls::material_hashes::binding::kSpoilerPaint;
        }
        if (mesh.carPartType == kHoodPart || identity.contains(QStringLiteral("hood"))) {
            return fls::material_hashes::binding::kHoodPaint;
        }
        if (identity.contains(QStringLiteral("mirror"))) {
            return fls::material_hashes::binding::kMirrorPaint;
        }
        return fls::material_hashes::binding::kBodyPaint;
    }
    return 0;
}

struct WheelMaterialFallback {
    QVector3D color;
    float gloss = 0.45f;
    float metallic = 0.0f;
};

std::optional<WheelMaterialFallback> wheelMaterialFallback(const fls::CarMesh &mesh) {
    if (!mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    const QString material = mesh.materialName.toLower();
    if (material == QStringLiteral("black")) {
        return WheelMaterialFallback{kBlackWheelColor, 0.25f, 0.0f};
    }
    if (material == QStringLiteral("rim") || material == QStringLiteral("rim2")) {
        return WheelMaterialFallback{kRimColor, 0.78f, 0.85f};
    }
    if (material == QStringLiteral("inner_rim") || material == QStringLiteral("hub")) {
        return WheelMaterialFallback{kInnerRimColor, 0.48f, 0.65f};
    }
    if (material == QStringLiteral("lug")) {
        return WheelMaterialFallback{kLugColor, 0.72f, 0.8f};
    }
    return std::nullopt;
}

int allowedWindowSidesForPart(const QString &rawName) {
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

int projectionSidesForMesh(const fls::CarMesh &mesh) {
    int sides = 0;
    if (isBodyPaintMaterial(mesh.materialName)) {
        if (isSpoilerMesh(mesh.name)) {
            sides = kSideSpoiler;
        } else if (isTrunkPanelMesh(mesh.name)) {
            sides = kSideBack | kSideTop;
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

int renderedLiverySidesForMesh(const fls::CarMesh &mesh) {
    int sides = 0;
    if (isBodyPaintMaterial(mesh.materialName)) {
        if (isSpoilerMesh(mesh.name)) {
            sides = kSideSpoiler;
        } else if (isTrunkPanelMesh(mesh.name)) {
            sides = kSideBack | kSideTop;
        } else {
            sides = kAllBodySides;
        }
    }
    if (isWindowGlassMaterial(mesh)
        && allowedWindowSidesForPart(mesh.name) != 0) {
        sides |= kAllGlassSides;
    }

    return sides;
}

std::optional<fls::ModelVec3> locatorPosition(const fls::CarModel &model, const char *name) {
    for (const fls::CarLocator &locator : model.locators) {
        if (locator.name.compare(QLatin1String(name), Qt::CaseInsensitive) == 0) {
            return locator.position;
        }
    }
    return std::nullopt;
}

std::optional<float> wheelAxleMidpointZ(const fls::CarModel &model) {
    std::vector<float> locatorSamples;
    for (const char *name : {"carLocator_wheelLF", "carLocator_wheelLR",
                             "carLocator_wheelRF", "carLocator_wheelRR"}) {
        if (const std::optional<fls::ModelVec3> position = locatorPosition(model, name)) {
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
    for (const fls::CarMesh &mesh : model.meshes) {
        if (!mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)
            || mesh.positions.empty()) {
            continue;
        }
        double sum = 0.0;
        for (const fls::ModelVec3 &position : mesh.positions) {
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
    const fls::CarModel &model, const fls::LiverySide &side) {
    if (side.xAxis != 2) {
        return std::nullopt;
    }
    const std::optional<fls::ModelVec3> front = locatorPosition(model, "carLocator_bumperF");
    const std::optional<fls::ModelVec3> rear = locatorPosition(model, "carLocator_bumperR");
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
    const fls::LiverySide &side,
    const std::vector<fls::CarMesh> &meshes,
    float axlo,
    float axhi,
    float aylo,
    float ayhi,
    const std::optional<float> &longitudinalPivotZ,
    bool lockLongitudinalX) {
    ProjectionAlignment result;
    const int geometrySide = kMaskSlotGeometrySides[sideIndex];

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

    const auto signedAxis = [&](const fls::ModelVec3 &point, bool xAxis) {
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
        const fls::CarMesh &mesh = meshes[mi];
        if ((projectionSidesForMesh(mesh) & (1 << geometrySide)) == 0) {
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
                const fls::ModelVec3 point =
                    mirroredCarSpace(mesh.boneTransform.transformPoint(mesh.positions[index]));
                const float x = (signedAxis(point, true) - axlo) / axisWidth;
                const float y = (signedAxis(point, false) - aylo) / axisHeight;
                triangle[corner] = QPointF(x * (rasterWidth - 1), y * (rasterHeight - 1));
                if (index < mesh.normals.size()) {
                    const fls::ModelVec3 normal =
                        mirroredCarSpace(mesh.boneTransform.transformVector(mesh.normals[index]));
                    facing += normal.x * kFacing[geometrySide].x()
                        + normal.y * kFacing[geometrySide].y()
                        + normal.z * kFacing[geometrySide].z();
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

struct LiveryProjectionData {
    std::array<QVector2D, fls::kLiverySideCount> minimum;
    std::array<QVector2D, fls::kLiverySideCount> maximum;
};

LiveryProjectionData buildLiveryProjectionData(
    const fls::CarModel &model, const fls::LiveryMaskSet &masks) {
    std::array<float, fls::kLiverySideCount> minimumX;
    std::array<float, fls::kLiverySideCount> maximumX;
    std::array<float, fls::kLiverySideCount> minimumY;
    std::array<float, fls::kLiverySideCount> maximumY;
    minimumX.fill(std::numeric_limits<float>::max());
    minimumY.fill(std::numeric_limits<float>::max());
    maximumX.fill(std::numeric_limits<float>::lowest());
    maximumY.fill(std::numeric_limits<float>::lowest());

    const std::vector<fls::CarMesh> &projectionMeshes =
        model.liveryProjectionMeshes.empty()
        ? model.meshes
        : model.liveryProjectionMeshes;
    for (const fls::CarMesh &mesh : projectionMeshes) {
        if (mesh.positions.empty()) {
            continue;
        }
        const int candidateSides = projectionSidesForMesh(mesh);
        if (candidateSides == 0) {
            continue;
        }
        for (const fls::ModelVec3 &position : mesh.positions) {
            const fls::ModelVec3 world = mirroredCarSpace(
                mesh.boneTransform.transformPoint(position));
            for (int sideIndex = 0;
                 sideIndex < fls::kLiverySideCount;
                 ++sideIndex) {
                if ((candidateSides
                     & (1 << kMaskSlotGeometrySides[sideIndex])) == 0) {
                    continue;
                }
                const fls::LiverySide &side = masks.sides[sideIndex];
                const float x = sideAxisScale(side, true)
                    * axisOf(world, side.xAxis);
                const float y = sideAxisScale(side, false)
                    * axisOf(world, side.yAxis);
                minimumX[sideIndex] = std::min(minimumX[sideIndex], x);
                maximumX[sideIndex] = std::max(maximumX[sideIndex], x);
                minimumY[sideIndex] = std::min(minimumY[sideIndex], y);
                maximumY[sideIndex] = std::max(maximumY[sideIndex], y);
            }
        }
    }

    std::array<bool, fls::kLiverySideCount> locatorAnchored{};
    for (int sideIndex = 2; sideIndex <= 4; ++sideIndex) {
        if (const std::optional<std::pair<float, float>> range =
                longitudinalLocatorRange(model, masks.sides[sideIndex])) {
            minimumX[sideIndex] = range->first;
            maximumX[sideIndex] = range->second;
            locatorAnchored[sideIndex] = true;
        }
    }

    LiveryProjectionData projection;
    const std::optional<float> longitudinalPivotZ = wheelAxleMidpointZ(model);
    for (int sideIndex = 0;
         sideIndex < fls::kLiverySideCount;
         ++sideIndex) {
        const bool haveProjection = maximumX[sideIndex] > minimumX[sideIndex]
            && maximumY[sideIndex] > minimumY[sideIndex];
        if (!haveProjection) {
            projection.minimum[sideIndex] = QVector2D(-1.0f, -1.0f);
            projection.maximum[sideIndex] = QVector2D(1.0f, 1.0f);
            continue;
        }

        const ProjectionAlignment alignment = alignProjectionToMask(
            sideIndex, masks.sides[sideIndex], projectionMeshes,
            minimumX[sideIndex], maximumX[sideIndex],
            minimumY[sideIndex], maximumY[sideIndex],
            longitudinalPivotZ, locatorAnchored[sideIndex]);
        const float width = (maximumX[sideIndex] - minimumX[sideIndex])
            / alignment.scaleX;
        const float height = (maximumY[sideIndex] - minimumY[sideIndex])
            / alignment.scaleY;
        const float startX = alignment.pivotX + alignment.offsetX
            - alignment.scaleX * alignment.pivotX;
        const float startY = alignment.pivotY + alignment.offsetY
            - alignment.scaleY * alignment.pivotY;
        const float alignedMinimumX = minimumX[sideIndex] - startX * width;
        const float alignedMinimumY = minimumY[sideIndex] - startY * height;
        projection.minimum[sideIndex] = QVector2D(
            alignedMinimumX, alignedMinimumY);
        projection.maximum[sideIndex] = QVector2D(
            alignedMinimumX + width, alignedMinimumY + height);
    }

    return projection;
}

bool transposedLiverySide(int sideIndex) {
    return sideIndex == 5 || sideIndex == 6 || sideIndex == 7;
}

QPointF liverySectionPoint(const fls::LiverySide &side,
                           int sideIndex,
                           const QPointF &maskCanvasPoint) {
    const bool transpose = transposedLiverySide(sideIndex);
    double x = transpose
        ? maskCanvasPoint.y() - side.yOrigin
        : maskCanvasPoint.x() - side.xOrigin;
    double y = transpose
        ? maskCanvasPoint.x() - side.xOrigin
        : maskCanvasPoint.y() - side.yOrigin;
    if (sideIndex == 4 || sideIndex == 6
        || sideIndex == 7 || sideIndex == 10) {
        x = -x;
    }
    if (sideIndex == 4 || sideIndex == 7 || sideIndex == 10) {
        y = -y;
    }

    return QPointF(x, y);
}

QPainterPath liveryCoveragePath(const fls::LiverySide &side,
                                 int sideIndex) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    if (!side.mask.valid()) {
        return path;
    }

    const fls::SwatchMask &mask = side.mask;
    const double pixelWidth = 2.0 * fls::kLiveryCanvasHalfWidth
        / mask.width;
    const double pixelHeight = 2.0 * fls::kLiveryCanvasHalfHeight
        / mask.height;
    for (int y = 0; y < mask.height; ++y) {
        int x = 0;
        while (x < mask.width) {
            while (x < mask.width && mask.at(x, y) < 128) {
                ++x;
            }
            const int runStart = x;
            while (x < mask.width && mask.at(x, y) >= 128) {
                ++x;
            }
            if (runStart == x) {
                continue;
            }

            const double left = runStart * pixelWidth
                - fls::kLiveryCanvasHalfWidth;
            const double right = x * pixelWidth
                - fls::kLiveryCanvasHalfWidth;
            const double top = fls::kLiveryCanvasHalfHeight
                - y * pixelHeight;
            const double bottom = top - pixelHeight;
            QPolygonF run;
            run.reserve(4);
            run.push_back(liverySectionPoint(
                side, sideIndex, QPointF(left, top)));
            run.push_back(liverySectionPoint(
                side, sideIndex, QPointF(right, top)));
            run.push_back(liverySectionPoint(
                side, sideIndex, QPointF(right, bottom)));
            run.push_back(liverySectionPoint(
                side, sideIndex, QPointF(left, bottom)));
            path.addPolygon(run);
            path.closeSubpath();
        }
    }

    return path;
}

QPointF projectedMaskPoint(const fls::CarMesh &mesh,
                           quint32 vertexIndex,
                           const fls::LiverySide &side,
                           int sideIndex,
                           const LiveryProjectionData &projection,
                           bool directUv) {
    if (directUv) {
        const fls::ModelVec2 &uv = mesh.uvChannels[3][vertexIndex];
        const fls::TexCoordTransform &transform = mesh.texCoordTransforms[3];
        const double u = (uv.u * transform.scaleU + transform.offsetU) * 0.5;
        const double v = uv.v * transform.scaleV + transform.offsetV;

        return QPointF(
            u * 2.0 * fls::kLiveryCanvasHalfWidth
                - fls::kLiveryCanvasHalfWidth,
            fls::kLiveryCanvasHalfHeight
                - v * 2.0 * fls::kLiveryCanvasHalfHeight);
    }

    const fls::ModelVec3 world = mirroredCarSpace(
        mesh.boneTransform.transformPoint(mesh.positions[vertexIndex]));
    const QVector2D minimum = projection.minimum[sideIndex];
    const QVector2D maximum = projection.maximum[sideIndex];
    const QVector2D range = maximum - minimum;
    const double axisX = sideAxisScale(side, true)
        * axisOf(world, side.xAxis);
    const double axisY = sideAxisScale(side, false)
        * axisOf(world, side.yAxis);
    const double normalizedX = range.x() > 0.0f
        ? (axisX - minimum.x()) / range.x()
        : 0.0;
    const double normalizedY = range.y() > 0.0f
        ? (axisY - minimum.y()) / range.y()
        : 0.0;

    return QPointF(
        side.left + (side.right - side.left) * normalizedX,
        side.top + (side.bottom - side.top) * normalizedY);
}

QPainterPath projectedLiveryGeometryPath(
    const std::vector<fls::CarMesh> &meshes,
    const fls::LiveryMaskSet &masks,
    const LiveryProjectionData &projection,
    int sideIndex) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const fls::LiverySide &side = masks.sides[sideIndex];
    const int geometrySide = kMaskSlotGeometrySides[sideIndex];
    for (const fls::CarMesh &mesh : meshes) {
        if ((renderedLiverySidesForMesh(mesh) & (1 << sideIndex)) == 0
            || mesh.positions.empty() || mesh.indices.empty()) {
            continue;
        }
        const bool directUv = mesh.liveryUvChannel == 3
            && mesh.uvChannels.size() > 3
            && mesh.uvChannels[3].size() == mesh.positions.size();
        for (size_t indexOffset = 0;
             indexOffset + 2 < mesh.indices.size();
             indexOffset += 3) {
            QPolygonF triangle;
            triangle.reserve(3);
            bool valid = true;
            bool haveNormals = true;
            double facing = 0.0;
            for (int corner = 0; corner < 3; ++corner) {
                const quint32 vertexIndex = mesh.indices[indexOffset + corner];
                if (vertexIndex >= mesh.positions.size()) {
                    valid = false;
                    break;
                }
                const QPointF maskPoint = projectedMaskPoint(
                    mesh, vertexIndex, side, sideIndex,
                    projection, directUv);
                triangle.push_back(liverySectionPoint(
                    side, sideIndex, maskPoint));
                if (vertexIndex < mesh.normals.size()) {
                    const fls::ModelVec3 normal = mirroredCarSpace(
                        mesh.boneTransform.transformVector(
                            mesh.normals[vertexIndex]));
                    facing += normal.x * kFacing[geometrySide].x()
                        + normal.y * kFacing[geometrySide].y()
                        + normal.z * kFacing[geometrySide].z();
                } else {
                    haveNormals = false;
                }
            }
            if (!valid || (haveNormals && facing <= 0.0)) {
                continue;
            }
            path.addPolygon(triangle);
            path.closeSubpath();
        }
    }

    return path;
}

quint64 unwrapEdgeKey(quint32 first, quint32 second) {
    const quint32 minimum = std::min(first, second);
    const quint32 maximum = std::max(first, second);

    return (static_cast<quint64>(minimum) << 32) | maximum;
}

QPainterPath projectedLiveryWireframePath(
    const std::vector<fls::CarMesh> &meshes,
    const fls::LiveryMaskSet &masks,
    const LiveryProjectionData &projection,
    int sideIndex) {
    QPainterPath path;
    const fls::LiverySide &side = masks.sides[sideIndex];
    const int geometrySide = kMaskSlotGeometrySides[sideIndex];
    for (const fls::CarMesh &mesh : meshes) {
        if ((renderedLiverySidesForMesh(mesh) & (1 << sideIndex)) == 0
            || mesh.positions.empty() || mesh.indices.empty()) {
            continue;
        }
        const bool directUv = mesh.liveryUvChannel == 3
            && mesh.uvChannels.size() > 3
            && mesh.uvChannels[3].size() == mesh.positions.size();
        QSet<quint64> edges;
        for (size_t indexOffset = 0;
             indexOffset + 2 < mesh.indices.size();
             indexOffset += 3) {
            std::array<QPointF, 3> triangle;
            std::array<quint32, 3> vertexIndices;
            bool valid = true;
            bool haveNormals = true;
            double facing = 0.0;
            for (int corner = 0; corner < 3; ++corner) {
                const quint32 vertexIndex = mesh.indices[indexOffset + corner];
                vertexIndices[corner] = vertexIndex;
                if (vertexIndex >= mesh.positions.size()) {
                    valid = false;
                    break;
                }
                const QPointF maskPoint = projectedMaskPoint(
                    mesh, vertexIndex, side, sideIndex,
                    projection, directUv);
                triangle[corner] = liverySectionPoint(
                    side, sideIndex, maskPoint);
                if (vertexIndex < mesh.normals.size()) {
                    const fls::ModelVec3 normal = mirroredCarSpace(
                        mesh.boneTransform.transformVector(
                            mesh.normals[vertexIndex]));
                    facing += normal.x * kFacing[geometrySide].x()
                        + normal.y * kFacing[geometrySide].y()
                        + normal.z * kFacing[geometrySide].z();
                } else {
                    haveNormals = false;
                }
            }
            if (!valid || (haveNormals && facing <= 0.0)) {
                continue;
            }
            for (int corner = 0; corner < 3; ++corner) {
                const int nextCorner = (corner + 1) % 3;
                const quint64 edge = unwrapEdgeKey(
                    vertexIndices[corner], vertexIndices[nextCorner]);
                if (edges.contains(edge)) {
                    continue;
                }
                edges.insert(edge);
                path.moveTo(triangle[corner]);
                path.lineTo(triangle[nextCorner]);
            }
        }
    }

    return path;
}

Clipper2Lib::Paths64 unwrapClipperPaths(const QPainterPath &path) {
    const QList<QPolygonF> polygons = path.toSubpathPolygons();
    Clipper2Lib::Paths64 paths;
    paths.reserve(static_cast<size_t>(polygons.size()));
    for (const QPolygonF &polygon : polygons) {
        Clipper2Lib::Path64 sampled;
        sampled.reserve(static_cast<size_t>(polygon.size()));
        for (const QPointF &point : polygon) {
            sampled.emplace_back(
                std::llround(point.x() * kUnwrapPathScale),
                std::llround(point.y() * kUnwrapPathScale));
        }
        if (sampled.size() >= 3) {
            paths.push_back(std::move(sampled));
        }
    }

    return paths;
}

QPainterPath rdpUnwrapPath(const QPainterPath &geometry,
                           const QPainterPath &coverage) {
    const RegionPenLoopConversionOptions options;
    Clipper2Lib::Clipper64 clipper;
    Clipper2Lib::Paths64 clipped;
    clipper.AddSubject(unwrapClipperPaths(geometry));
    clipper.AddClip(unwrapClipperPaths(coverage));
    if (!clipper.Execute(Clipper2Lib::ClipType::Intersection,
                         Clipper2Lib::FillRule::NonZero, clipped)) {
        return {};
    }

    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (const Clipper2Lib::Path64 &sampled : clipped) {
        if (sampled.size() < 3) {
            continue;
        }
        QPolygonF polygon;
        polygon.reserve(static_cast<qsizetype>(sampled.size()));
        for (const Clipper2Lib::Point64 &point : sampled) {
            polygon.push_back(QPointF(
                static_cast<double>(point.x) / kUnwrapPathScale,
                static_cast<double>(point.y) / kUnwrapPathScale));
        }
        const QVector<PenPoint> points = simplifyClosedPolygonRdpHybridQuadratic(
            polygon, options.simplifyEpsilon, options.minimumCurveBow);
        const PenContour contour = buildPenContour(points);
        if (contour.valid()) {
            path.addPath(contour.path);
        } else {
            path.addPolygon(polygon);
            path.closeSubpath();
        }
    }

    return path;
}

std::vector<uint8_t> upsampleCoverageMask(const fls::SwatchMask &mask, int dstW, int dstH) {
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

CarUnwrapOverlay buildCarUnwrapOverlay(const fls::CarModel &model,
                                       const fls::LiveryMaskSet &masks,
                                       int lodIndex) {
    CarUnwrapOverlay overlay;
    if (model.meshes.empty() || !masks.valid()) {
        return overlay;
    }

    const LiveryProjectionData projection =
        buildLiveryProjectionData(model, masks);
    const std::vector<fls::CarMesh> &selectedMeshes =
        model.meshesForLod(lodIndex);
    for (int sideIndex = 0;
         sideIndex < fls::kLiverySideCount;
         ++sideIndex) {
        const QPainterPath coverage = liveryCoveragePath(
            masks.sides[sideIndex], sideIndex);
        QPainterPath geometry = projectedLiveryGeometryPath(
            selectedMeshes, masks, projection, sideIndex);
        if (geometry.isEmpty() && lodIndex > 0) {
            geometry = projectedLiveryGeometryPath(
                model.meshes, masks, projection, sideIndex);
        }
        overlay.sides[sideIndex].path = rdpUnwrapPath(
            geometry, coverage);
        overlay.sides[sideIndex].wireframe = projectedLiveryWireframePath(
            selectedMeshes, masks, projection, sideIndex);
        if (overlay.sides[sideIndex].wireframe.isEmpty()
            && lodIndex > 0) {
            overlay.sides[sideIndex].wireframe = projectedLiveryWireframePath(
                model.meshes, masks, projection, sideIndex);
        }
    }

    return overlay;
}

void CarModelRenderer::setLivery(const fls::CarModel &model, const fls::LiveryMaskSet &masks) {
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
        const fls::SwatchMask &m = masks.sides[s].mask;
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
    const LiveryProjectionData projection =
        buildLiveryProjectionData(model, masks);

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
        const fls::LiverySide &side = masks.sides[s];
        const fls::SwatchMask &m = side.mask;
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
                                   sideAxisScale(side, true),
                                   sideAxisScale(side, false)));
        sideEMin_.append(projection.minimum[s]);
        sideEMax_.append(projection.maximum[s]);

        sideRegion_.append(QVector4D(side.left, side.right, side.top, side.bottom));
        defaultSidePaintRegion_.append(QVector4D(
            (side.left + 1024.0f) / 2048.0f,
            (side.right + 1024.0f) / 2048.0f,
            (512.0f - side.top) / 1024.0f,
            (512.0f - side.bottom) / 1024.0f));

        sideFacing_.append(kFacing[kMaskSlotGeometrySides[s]]);
    }
    sidePaintRegion_ = defaultSidePaintRegion_;
    fns->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    sideCount_ = kLiverySideCount;
}

void CarModelRenderer::setPaintTextureRegions(const QVector<QVector4D> &regions) {
    sidePaintRegion_ = regions.size() == sideCount_ ? regions : defaultSidePaintRegion_;
}

void CarModelRenderer::uploadModel(const fls::CarModel &model, int lodIndex) {
    if (!initialized_) {
        return;
    }
    clearModel();
    paintDiagnosticsLogged_ = false;
    const std::vector<fls::CarMesh> &renderMeshes = model.meshesForLod(lodIndex);

    const auto textureKey = [](const std::shared_ptr<const fls::ModelMaterialTexture> &texture,
                               bool srgb) {
        return QString::number(reinterpret_cast<quintptr>(texture.get()), 16)
            + (srgb ? QLatin1String("|s") : QLatin1String("|l"));
    };
    QSet<QString> requiredTextureKeys;
    qsizetype missingTextureBytes = 0;
    const auto requireTexture = [&](const std::shared_ptr<const fls::ModelMaterialTexture> &texture,
                                    bool srgb) {
        if (!texture || !texture->image.valid()) {
            return;
        }
        const QString key = textureKey(texture, srgb);
        if (requiredTextureKeys.contains(key)) {
            return;
        }
        requiredTextureKeys.insert(key);
        if (!materialTextureCache_.contains(key)) {
            missingTextureBytes += static_cast<qsizetype>(texture->image.rgba.size());
        }
    };
    for (const fls::CarMesh &mesh : renderMeshes) {
        if (!mesh.material) {
            continue;
        }
        requireTexture(mesh.material->diffuseTexture, false);
        requireTexture(mesh.material->alphaTexture, false);
        requireTexture(mesh.material->normalTexture, false);
        requireTexture(mesh.material->surfaceTexture, false);
        requireTexture(mesh.material->emissiveTexture, true);
    }
    constexpr qsizetype textureCacheBudget = 256ll * 1024 * 1024;
    if (materialTextureCacheBytes_ + missingTextureBytes > textureCacheBudget) {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        QOpenGLFunctions *functions = context ? context->functions() : nullptr;
        for (auto it = materialTextureCache_.begin();
             it != materialTextureCache_.end()
             && materialTextureCacheBytes_ + missingTextureBytes > textureCacheBudget;) {
            if (requiredTextureKeys.contains(it.key())) {
                ++it;
                continue;
            }
            if (functions) {
                functions->glDeleteTextures(1, &it->id);
            }
            materialTextureCacheBytes_ -= it->bytes;
            it = materialTextureCache_.erase(it);
        }
    }

    const auto uploadTexture = [&](const std::shared_ptr<const fls::ModelMaterialTexture> &texture,
                                   bool srgb) {
        if (!texture || !texture->image.valid()) {
            return GLuint{0};
        }
        const QString key = textureKey(texture, srgb);
        if (const auto it = materialTextureCache_.constFind(key);
            it != materialTextureCache_.cend()) {
            return it->id;
        }
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (context == nullptr) {
            return GLuint{0};
        }
        QOpenGLFunctions *functions = context->functions();
        QOpenGLExtraFunctions *extra = context->extraFunctions();
        GLuint id = 0;
        functions->glGenTextures(1, &id);
        functions->glBindTexture(GL_TEXTURE_2D, id);
        functions->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        functions->glTexImage2D(
            GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
            texture->image.width, texture->image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
            texture->image.rgba.data());
        extra->glGenerateMipmap(GL_TEXTURE_2D);
        functions->glBindTexture(GL_TEXTURE_2D, 0);
        MaterialTextureCacheEntry entry;
        entry.source = texture;
        entry.id = id;
        entry.bytes = static_cast<qsizetype>(texture->image.rgba.size());
        materialTextureCacheBytes_ += entry.bytes;
        materialTextureCache_.insert(key, std::move(entry));
        return id;
    };

    for (size_t mi = 0; mi < renderMeshes.size(); ++mi) {
        const fls::CarMesh &mesh = renderMeshes[mi];
        if (mesh.positions.empty() || mesh.indices.empty()) {
            continue;
        }
        if (isInteriorWindowShell(mesh.name)) {
            continue;
        }
        if (isCoarseInteriorLod(mesh)) {
            continue;
        }

        const std::vector<fls::ModelVec2> *uv = nullptr;
        if (mesh.liveryUvChannel >= 0
            && mesh.liveryUvChannel < static_cast<int>(mesh.uvChannels.size())
            && mesh.uvChannels[mesh.liveryUvChannel].size() == mesh.positions.size()) {
            uv = &mesh.uvChannels[mesh.liveryUvChannel];
        }
        const bool hasDirectLiveryUv = uv != nullptr && mesh.liveryUvChannel == 3;
        const fls::TexCoordTransform &uvTransform = mesh.texCoordTransforms[3];
        int materialUvIndex = 0;
        float materialUTiling = mesh.material ? mesh.material->uTiling : 1.0f;
        float materialVTiling = mesh.material ? mesh.material->vTiling : 1.0f;
        bool rawMaterialUv = false;
        // The car_carbonfiber shader (into which resolveExteriorMaterials injects the twin-twill
        // weave) samples its weave on RAW UV channel 0 tiled by the material's U/V tiling. Decompiled
        // from the game PS+VS (see docs/SHADER_ANALYSIS.md "Carbon Weave UV & Tiling"): the pixel
        // shader samples the weave at raw TEXCOORD0; the vertex shader builds TEXCOORD0 = channel0 *
        // (U_Tiling, V_Tiling) with U_Tiling=V_Tiling=32 for carbonfiber.materialbin (the ID15 variant
        // bakes 75). We must NOT apply channel 0's decoded transform — for carbon that is a ~0.03
        // livery-atlas packing the game replaces with the tiling — so sample channel 0 raw × tiling.
        QString carbonResource = mesh.material ? mesh.material->resourcePath : QString();
        carbonResource.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (carbonResource.contains(QStringLiteral("carbonfiber/carbonfiber"), Qt::CaseInsensitive)) {
            materialUvIndex = 0;
            rawMaterialUv = true;
            // Force the shader's U_Tiling if the shared-library merge didn't resolve it (leaving 1).
            if (materialUTiling <= 1.5f) {
                materialUTiling = 32.0f;
            }
            if (materialVTiling <= 1.5f) {
                materialVTiling = 32.0f;
            }
        }
        const std::vector<fls::ModelVec2> *materialUv =
            materialUvIndex < static_cast<int>(mesh.uvChannels.size())
            && mesh.uvChannels[materialUvIndex].size() == mesh.positions.size()
            ? &mesh.uvChannels[materialUvIndex]
            : nullptr;
        const fls::TexCoordTransform identityUvTransform;
        const fls::TexCoordTransform &materialUvTransform =
            rawMaterialUv ? identityUvTransform : mesh.texCoordTransforms[materialUvIndex];

        std::vector<float> interleaved;
        interleaved.reserve(mesh.positions.size() * 10);
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            const fls::ModelVec3 &p = mesh.positions[i];
            const fls::ModelVec3 &n = i < mesh.normals.size() ? mesh.normals[i] : fls::ModelVec3{0.0f, 1.0f, 0.0f};
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
            if (materialUv != nullptr) {
                // The car_carbonfiber weave rides raw TEXCOORD0 (V NOT flipped); every other native
                // material samples with the usual V-flip. Flipping the weave mirrors the twill
                // diagonal (/ vs \), which reads as the wrong orientation.
                const float mv = rawMaterialUv ? (*materialUv)[i].v : (1.0f - (*materialUv)[i].v);
                interleaved.push_back(((*materialUv)[i].u * materialUvTransform.scaleU
                                       + materialUvTransform.offsetU) * materialUTiling);
                interleaved.push_back((mv * materialUvTransform.scaleV
                                       + materialUvTransform.offsetV) * materialVTiling);
            } else {
                interleaved.push_back(0.0f);
                interleaved.push_back(0.0f);
            }
        }

        auto buffers = std::make_unique<MeshBuffers>();
        buffers->materialColor = kDefaultMaterialColor;
        buffers->name = mesh.name;
        buffers->materialName = mesh.materialName;
        buffers->indexCount = static_cast<int>(mesh.indices.size());
        buffers->hasDirectLiveryUv = hasDirectLiveryUv;
        buffers->bodyPaint = isBodyPaintMaterial(mesh.materialName);
        const bool windowGlass = isWindowGlassMaterial(mesh);
        buffers->allowedSides = renderedLiverySidesForMesh(mesh);
        buffers->applyLivery = buffers->allowedSides != 0;
        buffers->paintMaterialHash = mesh.paintMaterialHash != 0
            ? mesh.paintMaterialHash
            : fallbackPaintHash(mesh);
        if (mesh.material) {
            buffers->hasMaterialColor = mesh.material->hasBaseColor;
            buffers->materialColor = QVector3D(
                linearToDisplay(mesh.material->baseColor[0]),
                linearToDisplay(mesh.material->baseColor[1]),
                linearToDisplay(mesh.material->baseColor[2]));
            buffers->emissiveColor = QVector3D(
                mesh.material->emissiveColor[0], mesh.material->emissiveColor[1], mesh.material->emissiveColor[2]);
            buffers->emissiveIntensity = mesh.material->emissiveIntensity;
            buffers->gloss = mesh.material->gloss;
            if (mesh.material->hasMetallic) {
                buffers->metallic = mesh.material->metallic;
            }
            buffers->alpha = mesh.material->opacity;
            if (materialUv != nullptr) {
                buffers->diffuseTexture = uploadTexture(mesh.material->diffuseTexture, false);
                buffers->alphaTexture = uploadTexture(mesh.material->alphaTexture, false);
                buffers->normalTexture = uploadTexture(mesh.material->normalTexture, false);
                buffers->surfaceTexture = uploadTexture(mesh.material->surfaceTexture, false);
                buffers->emissiveTexture = uploadTexture(mesh.material->emissiveTexture, true);
            }
            if (buffers->diffuseTexture != 0 && !buffers->hasMaterialColor) {
                buffers->hasMaterialColor = true;
                buffers->materialColor = kTextureMaterialColor;
            }
        }
        if (isBodyPaintMaterial(mesh.materialName)) {
            buffers->hasMaterialColor = false;
        }
        // When the shared _library material was resolved, its gloss/metallic are the game's
        // real tuning; the name-heuristic fallbacks then only fill in a base colour.
        const bool libraryMaterial = mesh.material && mesh.material->resolvedFromLibrary;
        const std::optional<WheelMaterialFallback> wheel = wheelMaterialFallback(mesh);
        const std::optional<MaterialFallback> material = exteriorMaterialFallback(mesh);
        if (mesh.name.startsWith(QStringLiteral("tire"), Qt::CaseInsensitive)) {
            if (!libraryMaterial || !mesh.material->hasBaseColor) {
                buffers->hasMaterialColor = true;
                buffers->materialColor = kRubberColor;
            }
            if (!libraryMaterial) {
                buffers->gloss = 0.22f;
                buffers->metallic = 0.0f;
            }
        } else if (wheel) {
            if (!buffers->hasMaterialColor) {
                buffers->hasMaterialColor = true;
                buffers->materialColor = wheel->color;
            }
            if (!libraryMaterial) {
                buffers->gloss = wheel->gloss;
                buffers->metallic = wheel->metallic;
            }
        } else if (material) {
            const float brightestColor = std::max({buffers->materialColor.x(),
                                                   buffers->materialColor.y(),
                                                   buffers->materialColor.z()});
            if (!buffers->hasMaterialColor || brightestColor < material->minimumColor) {
                buffers->hasMaterialColor = true;
                buffers->materialColor = material->color;
            }
            if (!libraryMaterial) {
                buffers->gloss = material->gloss;
                buffers->metallic = material->metallic;
            }
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
                buffers->materialColor = kLampChromeColor;
                buffers->gloss = 0.94f;
                buffers->metallic = 1.0f;
            }
            if (glass) {
                buffers->hasMaterialColor = true;
                if (buffers->materialColor == kDefaultMaterialColor) {
                    buffers->materialColor = kLampGlassColor;
                }
                buffers->alpha = material.contains(QStringLiteral("colored")) ? 0.34f : 0.20f;
                buffers->gloss = 0.96f;
                buffers->metallic = 0.0f;
            }
            if (emitter) {
                if (buffers->emissiveColor.lengthSquared() < 0.000001f) {
                    if (name.contains(QStringLiteral("tail"))) {
                        buffers->emissiveColor = kTailLampEmissionColor;
                    } else if (name.contains(QStringLiteral("marker"))
                               || name.contains(QStringLiteral("indicator"))) {
                        buffers->emissiveColor = kIndicatorEmissionColor;
                    } else {
                        buffers->emissiveColor = kHeadLampEmissionColor;
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
        // Wheels/tires don't play well with back-face culling: the generated tires bake an
        // X-mirror into their right-side vertices (flipping winding) and the wheel discs
        // pair with motion-blur geometry, so cull them double-sided like before.
        {
            QString wheelPath = mesh.sourceModelPath.toLower();
            wheelPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
            buffers->doubleSided = wheelPath.contains(QStringLiteral("/wheels/"))
                || wheelPath.contains(QStringLiteral("/tires/"))
                || mesh.name.startsWith(QStringLiteral("tire"), Qt::CaseInsensitive)
                || mesh.name.startsWith(QStringLiteral("wheel"), Qt::CaseInsensitive);
        }

        buffers->model = carMeshModelMatrix(mesh);

        QVector3D center;
        for (const fls::ModelVec3 &p : mesh.positions) {
            const fls::ModelVec3 wp =
                mirroredCarSpace(mesh.boneTransform.transformPoint(p));
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

        constexpr int stride = 10 * sizeof(float);
        program_.enableAttributeArray(0);
        program_.setAttributeBuffer(0, GL_FLOAT, 0, 3, stride);
        program_.enableAttributeArray(1);
        program_.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, stride);
        program_.enableAttributeArray(2);
        program_.setAttributeBuffer(2, GL_FLOAT, 6 * sizeof(float), 2, stride);
        program_.enableAttributeArray(3);
        program_.setAttributeBuffer(3, GL_FLOAT, 8 * sizeof(float), 2, stride);

        buffers->vao.release();
        buffers->vbo.release();
        buffers->ibo.release();

        meshes_.push_back(std::move(buffers));
    }

    const auto uploadWireframeMeshes = [&](const std::vector<fls::CarMesh> &sourceMeshes) {
        const size_t initialCount = wireframeMeshes_.size();
        for (const fls::CarMesh &mesh : sourceMeshes) {
            const int allowedSides = renderedLiverySidesForMesh(mesh);
            if (allowedSides == 0 || mesh.positions.empty() || mesh.indices.empty()) {
                continue;
            }

            QSet<quint64> uniqueEdges;
            std::vector<quint32> edgeIndices;
            uniqueEdges.reserve(static_cast<qsizetype>(mesh.indices.size()));
            edgeIndices.reserve(mesh.indices.size() * 2);
            for (size_t indexOffset = 0;
                 indexOffset + 2 < mesh.indices.size();
                 indexOffset += 3) {
                const std::array<quint32, 3> triangle = {
                    mesh.indices[indexOffset],
                    mesh.indices[indexOffset + 1],
                    mesh.indices[indexOffset + 2],
                };
                if (triangle[0] >= mesh.positions.size()
                    || triangle[1] >= mesh.positions.size()
                    || triangle[2] >= mesh.positions.size()) {
                    continue;
                }
                for (int corner = 0; corner < 3; ++corner) {
                    const quint32 first = triangle[corner];
                    const quint32 second = triangle[(corner + 1) % 3];
                    const quint64 edge = unwrapEdgeKey(first, second);
                    if (first == second || uniqueEdges.contains(edge)) {
                        continue;
                    }
                    uniqueEdges.insert(edge);
                    edgeIndices.push_back(first);
                    edgeIndices.push_back(second);
                }
            }
            if (edgeIndices.empty()) {
                continue;
            }

            const bool directUv = mesh.liveryUvChannel == 3
                && mesh.uvChannels.size() > 3
                && mesh.uvChannels[3].size() == mesh.positions.size();
            const fls::TexCoordTransform &uvTransform = mesh.texCoordTransforms[3];
            std::vector<float> interleaved;
            interleaved.reserve(mesh.positions.size() * 8);
            for (size_t vertexIndex = 0;
                 vertexIndex < mesh.positions.size();
                 ++vertexIndex) {
                const fls::ModelVec3 &position = mesh.positions[vertexIndex];
                const fls::ModelVec3 normal = vertexIndex < mesh.normals.size()
                    ? mesh.normals[vertexIndex]
                    : fls::ModelVec3{0.0f, 1.0f, 0.0f};
                const fls::ModelVec2 uv = directUv
                    ? mesh.uvChannels[3][vertexIndex]
                    : fls::ModelVec2{};
                interleaved.push_back(position.x);
                interleaved.push_back(position.y);
                interleaved.push_back(position.z);
                interleaved.push_back(normal.x);
                interleaved.push_back(normal.y);
                interleaved.push_back(normal.z);
                interleaved.push_back(uv.u * uvTransform.scaleU + uvTransform.offsetU);
                interleaved.push_back(uv.v * uvTransform.scaleV + uvTransform.offsetV);
            }

            auto buffers = std::make_unique<WireframeBuffers>();
            buffers->indexCount = static_cast<int>(edgeIndices.size());
            buffers->hasDirectLiveryUv = directUv;
            buffers->allowedSides = allowedSides;
            buffers->model = carMeshModelMatrix(mesh);
            buffers->vao.create();
            buffers->vao.bind();
            buffers->vbo.create();
            buffers->vbo.bind();
            buffers->vbo.allocate(
                interleaved.data(),
                static_cast<int>(interleaved.size() * sizeof(float)));
            buffers->ibo.create();
            buffers->ibo.bind();
            buffers->ibo.allocate(
                edgeIndices.data(),
                static_cast<int>(edgeIndices.size() * sizeof(quint32)));

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
            wireframeMeshes_.push_back(std::move(buffers));
        }

        return wireframeMeshes_.size() - initialCount;
    };

    const size_t uploadedWireframes = uploadWireframeMeshes(renderMeshes);
    if (uploadedWireframes == 0 && lodIndex > 0) {
        uploadWireframeMeshes(model.meshes);
    }
}

namespace {

constexpr float kTwoToneSecondaryMix = 0.78f;
constexpr float kFlakeSecondaryMix = 0.6f;
constexpr float kFlakeSecondaryMixMax = 0.45f;
constexpr float kFlakeBaseTint = 0.7f;
constexpr float kFlakeBaseTintMax = 0.9f;
constexpr float kManufacturerFlakeBaseMetallic = 0.45f;
constexpr float kManufacturerFlakeMetallicGain = 0.5f;

// Automotive paint carries a clear lacquer coat over the colour; matte finishes below the
// gloss floor read as bare, and bare metals keep only a faint residual coat.
constexpr float kClearCoatGlossFloor = 0.35f;
constexpr float kClearCoatGlossRange = 0.6f;
constexpr float kClearCoatMetalCoverage = 0.25f;
constexpr float kClearCoatMinRoughness = 0.04f;
constexpr float kClearCoatMaxRoughness = 0.4f;

struct ClearCoat {
    float coverage = 0.0f;
    float roughness = kClearCoatMinRoughness;
};

ClearCoat bodyClearCoat(bool bodyPaint, const fls::PaintFinishRender *finish, float gloss) {
    if (!bodyPaint) {
        return {};
    }
    const bool bareMetal = finish != nullptr && finish->valid
        && finish->category == fls::PaintFinishCategory::Metal;
    ClearCoat coat;
    coat.coverage = std::clamp((gloss - kClearCoatGlossFloor) / kClearCoatGlossRange, 0.0f, 1.0f);
    if (bareMetal) {
        coat.coverage *= kClearCoatMetalCoverage;
    }
    coat.roughness = std::clamp(1.0f - gloss, kClearCoatMinRoughness, kClearCoatMaxRoughness);

    return coat;
}

// Approximates a finish's shading when the painttype material library is unavailable,
// preserving the earlier hardcoded behaviour for the most common finish codes.
void applyLegacyPaintFinish(const fls::LiveryPaintMaterial &paint, float &secondaryMix,
                            float &gloss, float &metallic) {
    switch (paint.finish) {
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
        break;
    case 50:
        gloss = 0.12f;
        secondaryMix = paint.secondary.enabled ? kTwoToneSecondaryMix : 0.0f;
        break;
    case 51:
        gloss = 0.95f;
        secondaryMix = paint.secondary.enabled ? kTwoToneSecondaryMix : 0.0f;
        break;
    case 52:
        gloss = 0.55f;
        secondaryMix = paint.secondary.enabled ? kTwoToneSecondaryMix : 0.0f;
        break;
    default:
        break;
    }
}

void applyPaintFinish(const fls::LiveryPaintMaterial &paint, const fls::PaintFinishRender *finish,
                      QVector3D &primary, QVector3D &secondary, float &secondaryMix, float &gloss,
                      float &metallic) {
    if (finish == nullptr || !finish->valid) {
        applyLegacyPaintFinish(paint, secondaryMix, gloss, metallic);
        return;
    }
    gloss = finish->gloss;
    metallic = finish->metallic;
    if (finish->usesSecondary && paint.secondary.enabled) {
        secondaryMix = kTwoToneSecondaryMix;
    } else if (finish->flakeAmount > 0.0f && paint.secondary.enabled) {
        // Dense flake finishes (metallic glitter) read as the flake colour, with the darker
        // base coat only peeking through between sparkles.
        const float tint =
            std::clamp(0.45f + finish->flakeAmount * kFlakeBaseTint, 0.0f, kFlakeBaseTintMax);
        primary = primary * (1.0f - tint) + secondary * tint;
        secondaryMix = std::clamp(finish->flakeAmount * kFlakeSecondaryMix, 0.0f, kFlakeSecondaryMixMax);
    }
    if (finish->hasMaterialColor
        && (finish->selfColored || finish->category == fls::PaintFinishCategory::Metal)) {
        primary = QVector3D(finish->materialColor[0], finish->materialColor[1], finish->materialColor[2]);
        secondary = primary;
    }
}

} // namespace

GLuint CarModelRenderer::uploadSwatchTexture(const fls::SwatchImage &image) {
    if (!image.valid()) {
        return 0;
    }
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return 0;
    }
    QOpenGLExtraFunctions *functions = context->extraFunctions();
    GLuint texture = 0;
    functions->glGenTextures(1, &texture);
    functions->glBindTexture(GL_TEXTURE_2D, texture);
    functions->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA,
                            GL_UNSIGNED_BYTE, image.rgba.data());
    functions->glGenerateMipmap(GL_TEXTURE_2D);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions->glBindTexture(GL_TEXTURE_2D, 0);

    return texture;
}

const CarModelRenderer::FinishTextureEntry *CarModelRenderer::ensurePaintFinishTextures(
    int code, const fls::PaintFinishRender &render) {
    const auto existing = finishTextureCache_.constFind(code);
    if (existing != finishTextureCache_.constEnd()) {
        return &existing.value();
    }
    FinishTextureEntry entry;
    entry.pattern = uploadSwatchTexture(render.patternImage);
    entry.normal = uploadSwatchTexture(render.detailNormalImage);
    entry.surface = uploadSwatchTexture(render.roughMetalAoImage);

    return &*finishTextureCache_.insert(code, entry);
}

void CarModelRenderer::clearPaintFinishTextures() {
    if (!finishTextureCache_.isEmpty()) {
        if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
            QVector<GLuint> ids;
            ids.reserve(finishTextureCache_.size() * 3);
            for (const FinishTextureEntry &entry : std::as_const(finishTextureCache_)) {
                for (GLuint id : {entry.pattern, entry.normal, entry.surface}) {
                    if (id != 0) {
                        ids.push_back(id);
                    }
                }
            }
            if (!ids.isEmpty()) {
                context->functions()->glDeleteTextures(ids.size(), ids.constData());
            }
        }
        finishTextureCache_.clear();
    }
    paintFinishTracked_ = false;
}

void CarModelRenderer::render(
    const QMatrix4x4 &view,
    const QMatrix4x4 &projection,
    GLuint liveryTexture,
    const QColor &basePaint,
    const fls::LiveryPaintState *paintState,
    const fls::ManufacturerColorPalette *manufacturerColors,
    const fls::PaintFinishLibrary *paintFinishes) {
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
    // Every mesh is drawn through an X-mirror (model matrix scale -1 on X), which reverses
    // triangle winding, so front faces are clockwise on screen.
    functions->glFrontFace(GL_CW);

    if (paintFinishes != nullptr
        && (!paintFinishTracked_ || paintFinishes->generation() != paintFinishGeneration_)) {
        clearPaintFinishTextures();
        paintFinishGeneration_ = paintFinishes->generation();
        paintFinishTracked_ = true;
    }

    program_.bind();
    program_.setUniformValue(wireframeOverlayLocation_, 0);
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

    // Manufacturer (factory) colours are the car's default paint. They only belong on an
    // uncustomised car: the moment the livery assigns any explicit primary colour, the whole car
    // is custom-painted, and materials left blank must inherit the chosen base paint rather than a
    // factory tint. A blank paint record is all-zero, so its manufacturerSelector reads 0 — which
    // would otherwise resolve to palette slot 0 (this car's golden yellow) and bleed onto every
    // livery panel. A blank record carries no colour either. Every blank-record mesh is a livery-
    // canvas panel (name "…Livery…") the game paints over, so with the livery off there is no single
    // right base for it — but material and part don't discriminate (the maroon rocker line and the
    // grey hood both carry blank carPaint_secondary on grey-dominant parts). What DOES discriminate
    // is position: a canvas panel should read as the real paint physically next to it. So collect
    // the painted panels' centres/colours and give each blank panel its nearest painted neighbour.
    bool liveryCustomPainted = false;
    QVector3D liveryPrimaryColor = fallbackColor;
    struct PaintedPanel {
        QVector3D center;
        QVector3D color;
        float weight;
    };
    std::vector<PaintedPanel> paintedPanels;
    if (paintState != nullptr) {
        QHash<quint32, int> primaryCounts;
        int bestPrimary = 0;
        for (const std::unique_ptr<MeshBuffers> &meshPtr : meshes_) {
            const MeshBuffers &m = *meshPtr;
            const fls::LiveryPaintMaterial *paint =
                m.bodyPaint ? paintState->find(m.paintMaterialHash) : nullptr;
            if (paint == nullptr || !paint->primary.enabled) {
                continue;
            }
            liveryCustomPainted = true;
            const quint32 key = (quint32(paint->primary.bgra[0]) << 16)
                | (quint32(paint->primary.bgra[1]) << 8) | quint32(paint->primary.bgra[2]);
            const QVector3D color(paint->primary.bgra[2] / 255.0f,
                                  paint->primary.bgra[1] / 255.0f,
                                  paint->primary.bgra[0] / 255.0f);
            const int weight = std::max(1, m.indexCount);
            paintedPanels.push_back({m.center, color, static_cast<float>(weight)});
            primaryCounts[key] += weight;
            if (primaryCounts[key] > bestPrimary) {
                bestPrimary = primaryCounts[key];
                liveryPrimaryColor = color;
            }
        }
    }

    const QMatrix4x4 viewProjection = projection * view;
    const auto drawMesh = [&](MeshBuffers &mesh) {
        QVector3D primary = mesh.hasMaterialColor ? mesh.materialColor : fallbackColor;
        // A custom-painted body panel with no explicit colour of its own (the livery-canvas layer)
        // takes the colour of the painted panel around it, so it reads as the paint nearby rather
        // than a car-wide guess (grey on the maroon rocker, or maroon on the grey hood). Rank by
        // distance²/panelArea so a big adjacent panel (the grey roof/body skin) wins over a small
        // accent that merely happens to be close (a maroon pillar sliver) — otherwise thin accents
        // bleed across whole canvas regions (e.g. the roof).
        if (mesh.bodyPaint && liveryCustomPainted) {
            primary = liveryPrimaryColor;
            float bestScore = std::numeric_limits<float>::max();
            for (const PaintedPanel &panel : paintedPanels) {
                const float score = (panel.center - mesh.center).lengthSquared() / panel.weight;
                if (score < bestScore) {
                    bestScore = score;
                    primary = panel.color;
                }
            }
        }
        QVector3D secondary = primary;
        float secondaryMix = 0.0f;
        float gloss = mesh.gloss;
        float metallic = mesh.metallic;
        float manufacturerFlake = 0.0f;
        const fls::LiveryPaintMaterial *paint = paintState != nullptr
            ? paintState->find(mesh.paintMaterialHash)
            : nullptr;
        const fls::ManufacturerColor *manufacturerColor =
            paint != nullptr && manufacturerColors != nullptr && !liveryCustomPainted
            ? manufacturerColors->find(paint->manufacturerSelector)
            : nullptr;
        const fls::PaintFinishRender *finish = (paint != nullptr && paintFinishes != nullptr)
            ? paintFinishes->find(static_cast<int>(paint->finish))
            : nullptr;
        const auto decodedColor = [](const fls::LiveryPaintColor &color) {
            return QVector3D(color.bgra[2] / 255.0f, color.bgra[1] / 255.0f, color.bgra[0] / 255.0f);
        };
        const auto decodedManufacturerColor = [](const std::array<float, 3> &color) {
            return QVector3D(
                linearToDisplay(color[0]),
                linearToDisplay(color[1]),
                linearToDisplay(color[2]));
        };
        if (manufacturerColor != nullptr) {
            primary = decodedManufacturerColor(manufacturerColor->primary);
            secondary = manufacturerColor->secondaryEnabled
                ? decodedManufacturerColor(manufacturerColor->secondary)
                : primary;
            if (manufacturerColor->material) {
                gloss = manufacturerColor->material->gloss;
                if (manufacturerColor->material->hasMetallic) {
                    metallic = manufacturerColor->material->metallic;
                }
                manufacturerFlake = manufacturerColor->material->flakeAmount;
                if (manufacturerFlake > 0.0f) {
                    metallic = std::max(
                        metallic,
                        kManufacturerFlakeBaseMetallic
                            + kManufacturerFlakeMetallicGain * manufacturerFlake);
                }
            }
        }
        if (paint != nullptr) {
            if (paint->primary.enabled) {
                primary = decodedColor(paint->primary);
            }
            if (paint->secondary.enabled) {
                secondary = decodedColor(paint->secondary);
            }
            applyPaintFinish(*paint, finish, primary, secondary, secondaryMix, gloss, metallic);
        }
        program_.setUniformValue(basePaintLocation_, primary);
        program_.setUniformValue(secondaryPaintLocation_, secondary);
        program_.setUniformValue(secondaryMixLocation_, secondaryMix);
        program_.setUniformValue(glossLocation_, gloss);
        program_.setUniformValue(metallicLocation_, metallic);
        const ClearCoat clearCoat = bodyClearCoat(mesh.bodyPaint, finish, gloss);
        program_.setUniformValue(clearCoatRoughnessLocation_, clearCoat.roughness);
        program_.setUniformValue(clearCoatIntensityLocation_, clearCoat.coverage);
        // Opt-in trace of the paint/finish each mesh resolves to, for diagnosing mis-shaded parts.
        static const bool paintDiagnostics = qEnvironmentVariableIsSet("FLS_PAINT_DIAG");
        if (paintDiagnostics && !paintDiagnosticsLogged_
            && (paint != nullptr || mesh.diffuseTexture != 0)) {
            qDebug().nospace().noquote()
                << "[paint] " << mesh.name << " mat=" << mesh.materialName
                << " painted=" << (paint != nullptr)
                << " nativeDiffuse=" << (mesh.diffuseTexture != 0)
                << " finish=" << (paint != nullptr ? int(paint->finish) : -1)
                << " primary=(" << primary.x() << "," << primary.y() << "," << primary.z() << ")";
        }
        program_.setUniformValue(
            emissiveLocation_, mesh.emissiveColor * std::max(0.0f, mesh.emissiveIntensity));
        program_.setUniformValue(hasLiveryLocation_, (hasLivery && mesh.applyLivery) ? 1 : 0);
        program_.setUniformValue(useDirectUvLocation_, mesh.hasDirectLiveryUv ? 1 : 0);
        program_.setUniformValue(allowedSidesLocation_, mesh.allowedSides);
        program_.setUniformValue(materialAlphaLocation_, mesh.alpha);
        const auto bindMaterialTexture = [&](int unit, GLuint texture, int samplerLocation,
                                             int enabledLocation) {
            functions->glActiveTexture(GL_TEXTURE0 + unit);
            functions->glBindTexture(GL_TEXTURE_2D, texture);
            program_.setUniformValue(samplerLocation, unit);
            program_.setUniformValue(enabledLocation, texture != 0 ? 1 : 0);
        };
        bindMaterialTexture(2, mesh.diffuseTexture, nativeDiffuseLocation_, hasNativeDiffuseLocation_);
        bindMaterialTexture(3, mesh.alphaTexture, nativeAlphaLocation_, hasNativeAlphaLocation_);
        bindMaterialTexture(4, mesh.normalTexture, nativeNormalLocation_, hasNativeNormalLocation_);
        bindMaterialTexture(5, mesh.surfaceTexture, nativeSurfaceLocation_, hasNativeSurfaceLocation_);
        bindMaterialTexture(6, mesh.emissiveTexture, nativeEmissiveLocation_, hasNativeEmissiveLocation_);
        const FinishTextureEntry *finishTextures =
            (finish != nullptr && finish->valid) ? ensurePaintFinishTextures(static_cast<int>(paint->finish), *finish)
                                                 : nullptr;
        bindMaterialTexture(7, finishTextures != nullptr ? finishTextures->pattern : 0,
                            finishPatternLocation_, hasFinishPatternLocation_);
        bindMaterialTexture(8, finishTextures != nullptr ? finishTextures->normal : 0,
                            finishNormalLocation_, hasFinishNormalLocation_);
        bindMaterialTexture(9, finishTextures != nullptr ? finishTextures->surface : 0,
                            finishSurfaceLocation_, hasFinishSurfaceLocation_);
        program_.setUniformValue(finishSelfColoredLocation_,
                                 (finish != nullptr && finish->selfColored) ? 1 : 0);
        program_.setUniformValue(finishTilingLocation_, 1.0f);
        program_.setUniformValue(
            finishFlakeLocation_,
            finish != nullptr ? finish->flakeAmount : manufacturerFlake);
        functions->glActiveTexture(GL_TEXTURE0);
        program_.setUniformValue(mvpLocation_, viewProjection * mesh.model);
        program_.setUniformValue(modelLocation_, mesh.model);
        mesh.vao.bind();
        functions->glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
        mesh.vao.release();
    };

    functions->glDisable(GL_BLEND);
    functions->glDepthMask(GL_TRUE);
    // Cull back faces on solid geometry so recessed cavities (intakes, grilles) stop
    // z-fighting against their own interior faces.
    functions->glEnable(GL_CULL_FACE);
    functions->glCullFace(GL_BACK);
    for (const auto &mesh : meshes_) {
        if (mesh->translucent) {
            continue;
        }
        if (mesh->doubleSided) {
            functions->glDisable(GL_CULL_FACE);
            drawMesh(*mesh);
            functions->glEnable(GL_CULL_FACE);
        } else {
            drawMesh(*mesh);
        }
    }
    // Translucent surfaces (glass, lenses) stay double-sided.
    functions->glDisable(GL_CULL_FACE);

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

    if (hasLivery && wireframeVisible_ && !wireframeMeshes_.empty()) {
        functions->glDisable(GL_DEPTH_TEST);
        functions->glDisable(GL_CULL_FACE);
        functions->glDepthMask(GL_FALSE);
        functions->glLineWidth(1.0f);
        program_.setUniformValue(wireframeOverlayLocation_, 1);
        program_.setUniformValue(hasLiveryLocation_, 1);
        program_.setUniformValue(debugModeLocation_, 0);
        program_.setUniformValue(materialAlphaLocation_, 1.0f);
        program_.setUniformValue(hasNativeDiffuseLocation_, 0);
        program_.setUniformValue(hasNativeAlphaLocation_, 0);
        program_.setUniformValue(hasNativeNormalLocation_, 0);
        program_.setUniformValue(hasNativeSurfaceLocation_, 0);
        program_.setUniformValue(hasNativeEmissiveLocation_, 0);
        program_.setUniformValue(hasFinishPatternLocation_, 0);
        program_.setUniformValue(hasFinishNormalLocation_, 0);
        program_.setUniformValue(hasFinishSurfaceLocation_, 0);
        for (const auto &mesh : wireframeMeshes_) {
            const int allowedSides = wireframeSide_ >= 0
                ? mesh->allowedSides & (1 << wireframeSide_)
                : mesh->allowedSides;
            if (allowedSides == 0) {
                continue;
            }
            program_.setUniformValue(
                useDirectUvLocation_, mesh->hasDirectLiveryUv ? 1 : 0);
            program_.setUniformValue(allowedSidesLocation_, allowedSides);
            program_.setUniformValue(mvpLocation_, viewProjection * mesh->model);
            program_.setUniformValue(modelLocation_, mesh->model);
            mesh->vao.bind();
            functions->glDrawElements(
                GL_LINES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
            mesh->vao.release();
        }
        program_.setUniformValue(wireframeOverlayLocation_, 0);
        functions->glDepthMask(GL_TRUE);
        functions->glEnable(GL_DEPTH_TEST);
    }

    paintDiagnosticsLogged_ = true;

    if (hasLivery) {
        functions->glActiveTexture(GL_TEXTURE1);
        functions->glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        functions->glActiveTexture(GL_TEXTURE0);
        functions->glBindTexture(GL_TEXTURE_2D, 0);
    }
    for (int unit = 2; unit <= 6; ++unit) {
        functions->glActiveTexture(GL_TEXTURE0 + unit);
        functions->glBindTexture(GL_TEXTURE_2D, 0);
    }
    functions->glActiveTexture(GL_TEXTURE0);
    program_.release();
}

} // namespace gui
