#include "paint_finish_catalog.h"

#include "game_paths.h"
#include "material_hashes.h"
#include "model_material.h"
#include "paint_database.h"
#include "zip_extract.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace fls {
namespace {

constexpr float kMetalFallbackMetallic = 0.9f;
constexpr float kFlakeBaseMetallic = 0.45f;
constexpr float kFlakeMetallicGain = 0.5f;
constexpr int kSecondUvChannel = 1;
constexpr quint32 kLegacyPaintColorEnabled = 4138700651;
constexpr quint32 kLegacyPaintColorTiling = 822378793;
constexpr quint32 kLegacyPaintColorRotation = 3501618446;
constexpr quint32 kLegacyDetailTiling = 2916271230;
constexpr quint32 kEarlyPaintColorTiling = 1369122465;
constexpr quint32 kEarlyPaintDetailTiling = 2396575845;
constexpr quint32 kEarlyPaintTintEnabled = 4125381471;
constexpr quint32 kEarlyPearlTintTiling = 1976922207;
constexpr quint32 kLegacyNormalTiling = 2485952580;
constexpr quint32 kLegacyNormalStrength = 4005214566;
constexpr quint32 kLegacyWeaveTint = 3878017450;
constexpr quint32 kLegacyWeaveTintEnabled = 1312426078;
constexpr quint32 kLegacyWeaveTintTiling = 1476456406;
constexpr quint32 kLegacyWeaveBaseColor = 1940513503;
constexpr quint32 kLegacyWeaveReflectance = 2475239088;
constexpr quint32 kLegacyWeaveLocalAo = 1812199748;
constexpr quint32 kLegacyPolishedReflectance = 530277512;
constexpr quint32 kLegacyMetalReflectance = 2752865311;
constexpr quint32 kLegacyMetalAlternateReflectance = 3611967824;
constexpr quint32 kLegacyMetalGloss = 2580310449;
constexpr quint32 kLegacyMetalAlternateGloss = 3601359974;
constexpr quint32 kLegacyMetalMask = 1665309683;
constexpr quint32 kLegacyMetalMaskTiling = 2164484293;
constexpr quint32 kEarlyMetalMask = 2571783952;
constexpr quint32 kEarlyMetalMaskTiling = 2263541979;
constexpr quint32 kLegacyMetalGlossMaskEnabled = 1460211077;
constexpr quint32 kLegacyMetalReflectanceMaskEnabled = 3124979876;
constexpr quint32 kLegacyMetalReflectanceFromNormal = 2377392304;
constexpr quint32 kLegacyMetalNormalStrength = 11643509;
constexpr quint32 kLegacyMetalCoatGloss = 2122899069;
constexpr quint32 kMirrorFinishNormal = 1085321393;
constexpr quint32 kStandardNormalStrength = 3446342299;
constexpr quint32 kStandardDetailNormalStrength = 3762475552;
constexpr quint32 kStandardRoughnessShift = 1311252771;
constexpr float kLegacyPaintColorRotationPivot = 0.04f;
constexpr float kLegacyWeaveTintIntensity = 0.5f;

using Cat = PaintFinishCategory;

enum class FinishUvProfile {
    Generic,
    Standard,
    Weave,
    LegacyPaint,
    LegacyWeave,
    LegacyMetal,
    LegacyPolishedMetal,
    LegacyPearl,
};

const QString kMaterialSuffix = QStringLiteral(".materialbin");
const QString kLegacyPaintMaterialsPrefix = QStringLiteral("painttypes/");
const QString kTextureLibraryMarker = QStringLiteral("/_library/textures/");

struct FinishMaterial {
    QString name;
    Cat category = Cat::Solid;
    int colorCount = 1;
};

struct FinishTexturePaths {
    QString pattern;
    QString normal;
    QString surface;
    QString weaveMask;
    bool mirrorNormal = false;
};

struct ResolvedPaintFinish {
    std::shared_ptr<ModelMaterial> material;
    FinishTexturePaths textures;
    FinishUvProfile profile = FinishUvProfile::Generic;
};

QHash<int, FinishMaterial> finishMaterials(const QString &game) {
    QHash<int, FinishMaterial> result;
    const QString referenceGame = QStringLiteral("FH6");
    const QJsonArray entries = gamePaintDatabase(game).value(QStringLiteral("finishes")).toArray();
    for (const auto &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString name = paintFinishMaterialName(entry);
        const int code = entry.value(QStringLiteral("ID")).toInt();
        const int colorCount = entry.value(QStringLiteral("NumInputColors")).toInt(1);
        const PaintFinishInfo *info = findPaintFinish(matchingPaintFinish(code, game, referenceGame));
        if (!name.isEmpty()) {
            result.insert(code, {name, info != nullptr ? info->category : colorCount == 0 ? Cat::Pattern : Cat::Solid,
                colorCount});
        }
    }
    if (result.isEmpty()) {
        for (const PaintFinishInfo &info : paintFinishTable()) {
            result.insert(info.code, {info.materialBase, info.category,
                info.category == Cat::Pattern ? 0 : info.category == Cat::TwoTone ? 2 : 1});
        }
    }

    return result;
}

const ModelMaterialParameter *materialParameter(const ModelMaterial &material, quint32 hash) {
    const auto found = std::find_if(material.parameters.crbegin(), material.parameters.crend(), [hash](const auto &parameter) {
        return parameter.nameHash == hash;
    });

    return found != material.parameters.crend() ? &*found : nullptr;
}

QString finishTexture(const ModelMaterial &material, quint32 hash) {
    const auto *parameter = materialParameter(material, hash);

    return parameter != nullptr && parameter->type == ModelMaterialParameterType::Texture2D ? parameter->texturePath : QString();
}

bool finishSwitch(const ModelMaterial &material, quint32 hash) {
    const auto *parameter = materialParameter(material, hash);

    return parameter != nullptr && parameter->type == ModelMaterialParameterType::Bool && parameter->boolean;
}

void applyWeaveTints(PaintFinishRender &render, const ModelMaterial &material) {
    const auto *first = materialParameter(material, material_hashes::parameter::kWeaveColorTintA);
    const auto *second = materialParameter(material, material_hashes::parameter::kWeaveColorTintB);
    const auto *userSwitch = materialParameter(material, material_hashes::parameter::kUserColorSwitch);
    const auto *userColor = materialParameter(material, material_hashes::parameter::kUserCarColor);
    const bool useUserColor = userSwitch != nullptr && userSwitch->boolean && userColor != nullptr;

    if (first == nullptr || second == nullptr) return;
    for (int channel = 0; channel < 3; ++channel) {
        render.weaveColorTintA[channel] = useUserColor ? userColor->vector[channel] * 0.1f : first->vector[channel];
        render.weaveColorTintB[channel] = second->vector[channel];
    }
}

QString normalizedAssetPath(QString path) {
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));

    return path.toLower();
}

QString finishShaderPath(const ModelMaterial &material) {
    for (auto link = material.linkedPaths.crbegin(); link != material.linkedPaths.crend(); ++link) {
        if (link->endsWith(QStringLiteral(".shaderbin"), Qt::CaseInsensitive)) return normalizedAssetPath(*link);
    }

    return {};
}

FinishUvProfile finishUvProfile(const QString &shaderPath) {
    const QString program = shaderPath.section(QLatin1Char('/'), -1);

    if (program == QStringLiteral("car_standard_user.shaderbin")) return FinishUvProfile::Standard;
    if (program == QStringLiteral("car_carbonfiber_user.shaderbin")) return FinishUvProfile::Weave;
    if (program == QStringLiteral("carpaint_user.shaderbin")) return FinishUvProfile::LegacyPaint;
    if (program == QStringLiteral("clc_metalch2normalch1mask.shaderbin")) return FinishUvProfile::LegacyWeave;
    if (program == QStringLiteral("carpaint_user_metals.shaderbin")) return FinishUvProfile::LegacyMetal;
    if (program == QStringLiteral("carpaint_user_hgm.shaderbin")) return FinishUvProfile::LegacyPolishedMetal;
    if (program == QStringLiteral("carpaint_pearl.shaderbin")) return FinishUvProfile::LegacyPearl;

    return FinishUvProfile::Generic;
}

std::shared_ptr<ModelMaterial> decodeFinishMaterial(const QByteArray &bytes);

QHash<QString, std::shared_ptr<ModelMaterial>> loadFinishShaderDefaults(
    const QHash<int, std::shared_ptr<ModelMaterial>> &materials, const QString &gameFolder) {
    struct ShaderSource {
        QString archive;
        QString entry;
    };
    QHash<QString, ShaderSource> sources;
    QHash<QString, QStringList> requests;
    QHash<QString, std::shared_ptr<ModelMaterial>> defaults;

    const QDir cars(gameCarsDir(gameFolder));
    const QDir media(gameMediaDir(gameFolder));
    const QString prefix = QStringLiteral("/_library/shaders/");
    for (const auto &material : materials) {
        const QString path = finishShaderPath(*material);
        if (sources.contains(path) || finishUvProfile(path) == FinishUvProfile::Generic) continue;
        const qsizetype start = path.indexOf(prefix);
        if (start < 0) continue;
        QString entry = path.mid(start + prefix.size());
        QString archive = gameAssetFilePath(media, QStringLiteral("_library/Shaders.zip"));
        if (path.contains(QStringLiteral("/cars") + prefix)) {
            const qsizetype separator = entry.indexOf(QLatin1Char('/'));
            if (separator < 0) continue;
            archive = gameAssetFilePath(cars, QStringLiteral("_library/shaders/") + entry.left(separator) + QStringLiteral(".zip"));
            entry = entry.mid(separator + 1);
        }
        sources.insert(path, {archive, entry});
        requests[archive].push_back(entry);
    }
    for (auto request = requests.cbegin(); request != requests.cend(); ++request) {
        if (!QFileInfo::exists(request.key())) continue;
        const auto blobs = readZipEntries(request.key(), request.value());
        for (const QString &entry : request.value()) {
            if (auto decoded = decodeFinishMaterial(blobs.value(entry))) defaults.insert(request.key() + entry, std::move(decoded));
        }
    }
    QHash<QString, std::shared_ptr<ModelMaterial>> shaders;
    for (auto source = sources.cbegin(); source != sources.cend(); ++source) {
        if (auto decoded = defaults.value(source->archive + source->entry)) shaders.insert(source.key(), std::move(decoded));
    }

    return shaders;
}

QString textureArchiveEntry(const QString &path) {
    const qsizetype marker = path.indexOf(kTextureLibraryMarker);

    return marker >= 0 ? path.mid(marker + kTextureLibraryMarker.size()) : path;
}

bool isCarTexture(const QString &path) {
    return !path.contains(kTextureLibraryMarker) || path.contains(QStringLiteral("/cars") + kTextureLibraryMarker);
}

bool isOpaqueWhite(const SwatchImage &image) {
    return image.valid()
        && std::all_of(image.rgba.cbegin(), image.rgba.cend(),
                       [](uint8_t channel) { return channel == 255; });
}

PaintFinishRender renderFromMaterial(const FinishMaterial &info, const ModelMaterial &material) {
    PaintFinishRender render;
    render.valid = true;
    render.category = info.category;
    render.usesSecondary = info.category == PaintFinishCategory::TwoTone && info.colorCount > 1;
    render.selfColored = info.colorCount == 0;
    render.gloss = material.gloss;
    render.metallic = material.hasMetallic
        ? material.metallic
        : (info.category == PaintFinishCategory::Metal ? kMetalFallbackMetallic : 0.0f);
    render.flakeAmount = material.flakeAmount;
    // Flake (metallic/glitter/candy) paints read as metallic sparkle even without an F0.
    if (info.category == PaintFinishCategory::Solid && material.flakeAmount > 0.0f) {
        render.metallic = std::max(render.metallic, kFlakeBaseMetallic + kFlakeMetallicGain * material.flakeAmount);
    }
    render.materialColor = material.baseColor;
    render.hasMaterialColor = material.hasBaseColor;

    return render;
}

QStringList paintArchives(const QString &primaryArchive) {
    if (primaryArchive.isEmpty()) return {};
    const QFileInfo primary(primaryArchive);
    const QString priorityPrefix = QFileInfo(gameAssetFileName(primary)).completeBaseName().toLower()
        + QStringLiteral("_pri_");
    QStringList archives;

    if (primary.isFile()) archives.push_back(primaryArchive);
    for (const QFileInfo &file : primary.dir().entryInfoList(QDir::Files)) {
        const QString name = gameAssetFileName(file).toLower();
        if (name.startsWith(priorityPrefix) && name.endsWith(QStringLiteral(".zip"))) {
            archives.push_back(file.absoluteFilePath());
        }
    }

    return archives;
}

QHash<QString, QByteArray> readPaintArchiveEntries(const QStringList &archives, QStringList entries) {
    QHash<QString, QByteArray> result;
    entries.removeDuplicates();
    for (const QString &archive : archives) {
        if (entries.isEmpty()) break;
        auto blobs = readZipEntries(archive, entries);
        if (blobs.isEmpty()) continue;
        entries.removeIf([&](const QString &entry) { return blobs.contains(entry); });
        for (auto it = blobs.begin(); it != blobs.end(); ++it) {
            result.insert(it.key(), std::move(it.value()));
        }
    }

    return result;
}

std::shared_ptr<ModelMaterial> decodeFinishMaterial(const QByteArray &bytes) {
    try {
        return decodeMaterialBundle(bytes);
    } catch (const std::runtime_error &) {
        return {};
    }
}

QHash<int, std::shared_ptr<ModelMaterial>> decodeFinishMaterials(
    const QHash<int, FinishMaterial> &finishes, const QString &gameFolder, const QString &game) {
    const QStringList archives = paintArchives(gamePaintMaterialsArchive(gameFolder));
    const QString modernPrefix = gamePaintMaterialsPrefix();
    const QStringList prefixes = game == QStringLiteral("FH6") || game == QStringLiteral("FM2023")
        ? QStringList{modernPrefix, kLegacyPaintMaterialsPrefix}
        : QStringList{kLegacyPaintMaterialsPrefix, modernPrefix};
    QHash<int, std::shared_ptr<ModelMaterial>> materials;
    if (!archives.isEmpty()) {
        for (const QString &prefix : prefixes) {
            QStringList entries;
            for (auto it = finishes.cbegin(); it != finishes.cend(); ++it) {
                if (!materials.contains(it.key())) entries.push_back(prefix + it->name + kMaterialSuffix);
            }
            const auto blobs = readPaintArchiveEntries(archives, std::move(entries));
            for (auto it = finishes.cbegin(); it != finishes.cend(); ++it) {
                if (!materials.contains(it.key())) {
                    if (auto material = decodeFinishMaterial(blobs.value(prefix + it->name + kMaterialSuffix))) {
                        materials.insert(it.key(), std::move(material));
                    }
                }
            }
        }
    } else {
        const QDir dir(gameFolder);
        for (auto it = finishes.cbegin(); it != finishes.cend(); ++it) {
            QFile file(dir.filePath(it->name + kMaterialSuffix));
            if (!file.open(QIODevice::ReadOnly)) continue;
            if (auto material = decodeFinishMaterial(file.readAll())) {
                materials.insert(it.key(), std::move(material));
            }
        }
    }

    return materials;
}

FinishTexturePaths finishTexturePaths(
    const ModelMaterial &material, const ModelMaterial &resolved, FinishUvProfile profile) {
    constexpr quint32 kLegacyPaintColor = 3394122313;
    constexpr quint32 kEarlyPaintColor = 692954876;
    constexpr quint32 kLegacyFlakeTint = 3262721781;
    constexpr quint32 kEarlyPearlTint = 1936471985;
    constexpr quint32 kLegacyDetailNormal = 1071456347;
    using namespace material_hashes::parameter;
    FinishTexturePaths paths{material.patternTexture, material.detailNormalTexture, material.roughMetalAoTexture,
                             finishTexture(material, kWeaveMask)};
    const bool earlyBindings = resolved.shaderBindingLayout == ShaderBindingLayout::EarlyLegacy;

    if (profile == FinishUvProfile::LegacyPaint) {
        const bool mappedColor = finishSwitch(resolved, kLegacyPaintColorEnabled)
            || (earlyBindings && finishSwitch(resolved, kEarlyPaintTintEnabled));
        paths.pattern = mappedColor
            ? finishTexture(resolved, earlyBindings ? kEarlyPaintColor : kLegacyPaintColor) : QString();
    } else if (paths.pattern.isEmpty()) {
        const quint32 tint = earlyBindings && profile == FinishUvProfile::LegacyPearl
            ? kEarlyPearlTint : kLegacyFlakeTint;
        paths.pattern = finishTexture(material, tint);
        if (paths.pattern.isEmpty()) paths.pattern = finishTexture(material, kLegacyPaintColor);
    }
    if (paths.normal.isEmpty()) {
        paths.normal = finishTexture(material, kMirrorFinishNormal);
        paths.mirrorNormal = !paths.normal.isEmpty();
    }
    if (paths.normal.isEmpty()) paths.normal = finishTexture(material, kLegacyDetailNormal);
    if (paths.normal.isEmpty()) paths.normal = finishTexture(material, kNormalTexture);
    if (profile == FinishUvProfile::LegacyWeave && finishSwitch(resolved, kLegacyWeaveTintEnabled)) {
        paths.pattern = finishTexture(resolved, kLegacyWeaveTint);
    }
    if (profile == FinishUvProfile::LegacyMetal) {
        paths.pattern.clear();
        if (finishSwitch(resolved, kLegacyMetalGlossMaskEnabled)
            || finishSwitch(resolved, kLegacyMetalReflectanceMaskEnabled)) {
            paths.surface = finishTexture(resolved, earlyBindings ? kEarlyMetalMask : kLegacyMetalMask);
        }
    }

    return paths;
}

float finishScalar(const ModelMaterial &material, quint32 hash, float fallback = 0.0f) {
    const auto *parameter = materialParameter(material, hash);

    return parameter != nullptr && parameter->type == ModelMaterialParameterType::Float && std::isfinite(parameter->scalar)
        ? parameter->scalar : fallback;
}

std::array<float, 2> finishTiling(const ModelMaterial &material, quint32 hash, std::array<float, 2> fallback) {
    const auto *parameter = materialParameter(material, hash);

    return parameter != nullptr && parameter->type == ModelMaterialParameterType::Vector2
        && std::isfinite(parameter->vector[0]) && std::isfinite(parameter->vector[1])
        ? std::array{parameter->vector[0], parameter->vector[1]} : fallback;
}

void applyFinishUvMappings(PaintFinishRender &render, const ModelMaterial &material,
                           FinishUvProfile profile, bool mirrorNormal) {
    PaintFinishUvMapping mapping{{material.uTiling, material.vTiling}};
    const bool earlyBindings = material.shaderBindingLayout == ShaderBindingLayout::EarlyLegacy;
    const quint32 detailTiling = earlyBindings ? kEarlyPaintDetailTiling : kLegacyDetailTiling;

    switch (profile) {
    case FinishUvProfile::Standard:
        mapping.channel = material.materialUvChannel >= 0 ? material.materialUvChannel : kSecondUvChannel;
        mapping.rotationDegrees = material.uvOrientationDegrees;
        render.patternUv = render.normalUv = render.surfaceUv = mapping;
        if (mirrorNormal) {
            render.normalUv = {{1.0f, 1.0f}, 0};
        }
        break;
    case FinishUvProfile::Weave:
        mapping.channel = kSecondUvChannel;
        mapping.rotationDegrees = material.uvOrientationDegrees;
        render.patternUv = render.normalUv = render.surfaceUv = mapping;
        break;
    case FinishUvProfile::LegacyPaint:
        mapping.channel = kSecondUvChannel;
        render.patternUv = {finishTiling(material, earlyBindings ? kEarlyPaintColorTiling : kLegacyPaintColorTiling,
            mapping.tiling), mapping.channel,
            finishScalar(material, kLegacyPaintColorRotation), kLegacyPaintColorRotationPivot};
        render.normalUv = {finishTiling(material, detailTiling, mapping.tiling), mapping.channel};
        render.surfaceUv = render.patternUv;
        break;
    case FinishUvProfile::LegacyWeave:
        mapping.channel = kSecondUvChannel;
        render.patternUv = {finishTiling(material, finishSwitch(material, kLegacyWeaveTintEnabled)
            ? kLegacyWeaveTintTiling : kLegacyPaintColorTiling, mapping.tiling), mapping.channel};
        render.normalUv = {finishTiling(material, kLegacyNormalTiling, mapping.tiling), mapping.channel};
        render.surfaceUv = render.patternUv;
        break;
    case FinishUvProfile::LegacyMetal:
        render.normalUv = {finishTiling(material, detailTiling, mapping.tiling), kSecondUvChannel};
        render.surfaceUv = {finishTiling(material, earlyBindings ? kEarlyMetalMaskTiling : kLegacyMetalMaskTiling,
            mapping.tiling), kSecondUvChannel};
        break;
    case FinishUvProfile::LegacyPolishedMetal:
        break;
    case FinishUvProfile::LegacyPearl:
        render.normalUv = {finishTiling(material, detailTiling, mapping.tiling), kSecondUvChannel};
        if (earlyBindings) {
            render.patternUv = {finishTiling(material, kEarlyPearlTintTiling, mapping.tiling), kSecondUvChannel};
        }
        break;
    case FinishUvProfile::Generic: {
        const auto normalTiling = render.hasDetailNormal()
            ? finishTiling(material, kLegacyNormalTiling, mapping.tiling) : mapping.tiling;
        mapping.tiling = finishTiling(material, render.hasPattern() ? kLegacyPaintColorTiling : kLegacyDetailTiling, normalTiling);
        render.patternUv = render.normalUv = render.surfaceUv = mapping;
        break;
    }
    }
}

bool readFinishColor(const ModelMaterial &material, quint32 hash, std::array<float, 3> &color) {
    const auto *parameter = materialParameter(material, hash);
    if (parameter == nullptr || (parameter->type != ModelMaterialParameterType::Color
        && parameter->type != ModelMaterialParameterType::Vector)) return false;
    std::copy_n(parameter->vector.begin(), color.size(), color.begin());

    return true;
}

void applyFinishSurface(PaintFinishRender &render, const ModelMaterial &material,
                        FinishUvProfile profile, bool mirrorNormal) {
    switch (profile) {
    case FinishUvProfile::LegacyWeave: {
        std::array<float, 3> reflectance;
        if (readFinishColor(material, kLegacyWeaveReflectance, reflectance)) render.specularReflectance = reflectance;
        render.selfColored = true;
        render.hasMaterialColor = readFinishColor(material, kLegacyWeaveBaseColor, render.materialColor);
        render.reflectanceFromNormalAlpha = finishSwitch(material, kLegacyWeaveLocalAo);
        render.normalIntensity = finishScalar(material, kLegacyNormalStrength, render.normalIntensity);
        render.patternCoverage = render.flakeAmount;
        render.patternIntensity = kLegacyWeaveTintIntensity;
        render.flakeAmount = 0.0f;
        break;
    }
    case FinishUvProfile::LegacyPolishedMetal:
        render.hasMaterialColor = readFinishColor(material, kLegacyPolishedReflectance, render.materialColor);
        render.metallic = 1.0f;
        break;
    case FinishUvProfile::LegacyMetal:
        render.hasMaterialColor = readFinishColor(material, kLegacyMetalReflectance, render.materialColor);
        render.gloss = finishScalar(material, kLegacyMetalGloss, render.gloss);
        render.clearCoatCoverage = 1.0f;
        render.clearCoatRoughness = 1.0f - finishScalar(material, kLegacyMetalCoatGloss, render.gloss);
        render.normalIntensity = finishScalar(material, kLegacyMetalNormalStrength, render.normalIntensity);
        render.reflectanceFromNormalAlpha = finishSwitch(material, kLegacyMetalReflectanceFromNormal);
        render.metallic = 1.0f;
        render.flakeAmount = 0.0f;
        if (render.hasRoughMetalAo()) {
            PaintFinishMetalMask mask;
            mask.glossRange = {render.gloss, finishScalar(material, kLegacyMetalAlternateGloss, render.gloss)};
            mask.variesGloss = finishSwitch(material, kLegacyMetalGlossMaskEnabled);
            mask.variesReflectance = finishSwitch(material, kLegacyMetalReflectanceMaskEnabled)
                && readFinishColor(material, kLegacyMetalAlternateReflectance, mask.alternateColor);
            render.metalMask = mask;
        }
        break;
    case FinishUvProfile::Standard:
        if (render.category == Cat::Metal) {
            render.roughnessShift = std::clamp(finishScalar(material, kStandardRoughnessShift), -1.0f, 1.0f);
            render.normalIntensity = finishScalar(material, mirrorNormal
                ? kStandardNormalStrength : kStandardDetailNormalStrength, render.normalIntensity);
            if (isOpaqueWhite(render.roughMetalAoImage)) {
                render.gloss = std::max(0.0f, -render.roughnessShift);
                render.metallic = 1.0f;
            }
        }
        break;
    case FinishUvProfile::LegacyPearl:
        if (render.category == Cat::Metal) {
            render.gloss = finishScalar(material, kLegacyMetalGloss, render.gloss);
        }
        break;
    case FinishUvProfile::Generic:
    case FinishUvProfile::Weave:
        break;
    }
}

QVector<PaintFinishInfo> buildTable() {
    return {
        {1, QStringLiteral("Gloss"), QStringLiteral("normalpaint"), Cat::Solid},
        {2, QStringLiteral("Semigloss"), QStringLiteral("normalpaintsemigloss"), Cat::Solid},
        {3, QStringLiteral("Matte"), QStringLiteral("normalpaintmatte"), Cat::Solid},
        {4, QStringLiteral("Metallic"), QStringLiteral("metallicpaint"), Cat::Solid},
        {6, QStringLiteral("Carbon Fiber Matte"), QStringLiteral("carbonfibermatte"), Cat::Pattern},
        {7, QStringLiteral("Carbon Fiber Polished"), QStringLiteral("carbonfiberpolished"), Cat::Pattern},
        {8, QStringLiteral("Carbon Kevlar Matte"), QStringLiteral("carbonkevlarsquareweavematte"), Cat::Pattern},
        {9, QStringLiteral("Carbon Kevlar Polished"), QStringLiteral("carbonkevlarsquareweavepolished"), Cat::Pattern},
        {12, QStringLiteral("Chrome"), QStringLiteral("chromiumpolished"), Cat::Metal},
        {13, QStringLiteral("Gold"), QStringLiteral("goldpolished"), Cat::Metal},
        {14, QStringLiteral("Aluminum Brushed"), QStringLiteral("aluminumbrushed"), Cat::Metal},
        {16, QStringLiteral("Aluminum Polished"), QStringLiteral("aluminumpolished"), Cat::Metal},
        {17, QStringLiteral("Aluminum Semigloss"), QStringLiteral("aluminumsemigloss"), Cat::Metal},
        {20, QStringLiteral("Blob Camo Desert Matte"), QStringLiteral("camoclassicdesertmatte"), Cat::Pattern},
        {21, QStringLiteral("Blob Camo Desert Polished"), QStringLiteral("camoclassicdesertpolished"), Cat::Pattern},
        {22, QStringLiteral("Blob Camo Snow Matte"), QStringLiteral("camoclassicsnowmatte"), Cat::Pattern},
        {23, QStringLiteral("Blob Camo Snow Polished"), QStringLiteral("camoclassicsnowpolished"), Cat::Pattern},
        {24, QStringLiteral("Blob Camo Woodland Matte"), QStringLiteral("camoclassicwoodlandmatte"), Cat::Pattern},
        {25, QStringLiteral("Blob Camo Woodland Polished"), QStringLiteral("camoclassicwoodlandpolished"), Cat::Pattern},
        {26, QStringLiteral("Brass Brushed"), QStringLiteral("brassbrushed"), Cat::Metal},
        {27, QStringLiteral("Brass Polished"), QStringLiteral("brasspolished"), Cat::Metal},
        {28, QStringLiteral("Brass Semigloss"), QStringLiteral("brasssemigloss"), Cat::Metal},
        {31, QStringLiteral("Copper Brushed"), QStringLiteral("copperbrushed"), Cat::Metal},
        {32, QStringLiteral("Copper Polished"), QStringLiteral("copperpolished"), Cat::Metal},
        {33, QStringLiteral("Copper Semigloss"), QStringLiteral("coppersemigloss"), Cat::Metal},
        {36, QStringLiteral("Digital Camo Desert Matte"), QStringLiteral("camodigitaldesertmatte"), Cat::Pattern},
        {37, QStringLiteral("Digital Camo Desert Polished"), QStringLiteral("camodigitaldesertpolished"), Cat::Pattern},
        {38, QStringLiteral("Digital Camo Snow Matte"), QStringLiteral("camodigitalsnowmatte"), Cat::Pattern},
        {39, QStringLiteral("Digital Camo Snow Polished"), QStringLiteral("camodigitalsnowpolished"), Cat::Pattern},
        {40, QStringLiteral("Digital Camo Woodland Matte"), QStringLiteral("camodigitalwoodlandmatte"), Cat::Pattern},
        {41, QStringLiteral("Digital Camo Woodland Polished"), QStringLiteral("camodigitalwoodlandpolished"), Cat::Pattern},
        {42, QStringLiteral("Spyshot Swirls"), QStringLiteral("camospyshotswirl"), Cat::Pattern},
        {43, QStringLiteral("Spyshot Triangles"), QStringLiteral("camospyshottriangles"), Cat::Pattern},
        {44, QStringLiteral("Steel Brushed"), QStringLiteral("steelbrushed"), Cat::Metal},
        {45, QStringLiteral("Diamond Plate"), QStringLiteral("steeldiamondplate"), Cat::Metal},
        {46, QStringLiteral("Steel Polished"), QStringLiteral("steelpolished"), Cat::Metal},
        {47, QStringLiteral("Steel Semigloss"), QStringLiteral("steelsemigloss"), Cat::Metal},
        {50, QStringLiteral("Two-Tone Matte"), QStringLiteral("twotonematte"), Cat::TwoTone},
        {51, QStringLiteral("Two-Tone Polished"), QStringLiteral("twotonepolished"), Cat::TwoTone},
        {52, QStringLiteral("Two-Tone Semigloss"), QStringLiteral("twotonesemigloss"), Cat::TwoTone},
        {53, QStringLiteral("Wood Dark"), QStringLiteral("wooddarkmatte"), Cat::Pattern},
        {56, QStringLiteral("Wood Light"), QStringLiteral("woodlightmatte"), Cat::Pattern},
        {59, QStringLiteral("Wood Medium"), QStringLiteral("woodmediummatte"), Cat::Pattern},
        {60, QStringLiteral("Realistic Camo Woodland Matte"), QStringLiteral("camorealforestmatte"), Cat::Pattern},
        {61, QStringLiteral("Realistic Camo Woodland Polished"), QStringLiteral("camorealforestpolished"), Cat::Pattern},
        {62, QStringLiteral("Realistic Camo Snow Polished"), QStringLiteral("camorealsnowpolished"), Cat::Pattern},
        {63, QStringLiteral("Realistic Camo Snow Matte"), QStringLiteral("camorealsnowmatte"), Cat::Pattern},
        {64, QStringLiteral("Zinc"), QStringLiteral("zinc"), Cat::Metal},
        {65, QStringLiteral("Prismacolor White"), QStringLiteral("prismacolor_white"), Cat::Pattern},
        {66, QStringLiteral("Steel Damascus"), QStringLiteral("damascus"), Cat::Metal},
        {67, QStringLiteral("Prismacolor Black"), QStringLiteral("prismacolor_black"), Cat::Pattern},
        {68, QStringLiteral("Steel Galvanized"), QStringLiteral("galvanized"), Cat::Metal},
        {69, QStringLiteral("Candy Paint"), QStringLiteral("candypaint"), Cat::Solid},
        {70, QStringLiteral("Metallic Low Flake"), QStringLiteral("metallicpaintlowflake"), Cat::Solid},
        {71, QStringLiteral("Metallic High Flake"), QStringLiteral("metallicpainthighflake"), Cat::Solid},
        {72, QStringLiteral("Metallic Glitter"), QStringLiteral("metallicglitterpaint"), Cat::Solid},
    };
}

} // namespace

const QVector<PaintFinishInfo> &paintFinishTable() {
    static const QVector<PaintFinishInfo> table = buildTable();

    return table;
}

const PaintFinishInfo *findPaintFinish(int code) {
    for (const PaintFinishInfo &info : paintFinishTable()) {
        if (info.code == code) {
            return &info;
        }
    }

    return nullptr;
}

void PaintFinishLibrary::clear() {
    folder_.clear();
    game_.clear();
    byCode_.clear();
    loaded_ = false;
    ++generation_;
}

void PaintFinishLibrary::replace(PaintFinishLibrary library) {
    folder_ = std::move(library.folder_);
    game_ = std::move(library.game_);
    loaded_ = library.loaded_;
    byCode_ = std::move(library.byCode_);
    ++generation_;
}

void PaintFinishLibrary::load(const QString &gameFolder, const QString &game) {
    clear();
    folder_ = gameFolder;
    game_ = game.isEmpty() ? QStringLiteral("FH6") : game;

    if (gameFolder.isEmpty()) return;
    const auto finishes = finishMaterials(game_);
    const auto materials = decodeFinishMaterials(finishes, gameFolder, game_);
    if (materials.isEmpty()) return;
    const auto shaders = loadFinishShaderDefaults(materials, gameFolder);

    QStringList wantedCarTextures;
    QStringList wantedMediaTextures;
    QHash<int, ResolvedPaintFinish> resolvedFinishes;
    for (auto it = materials.cbegin(); it != materials.cend(); ++it) {
        const ModelMaterial &material = *it.value();
        const QString shaderPath = finishShaderPath(material);
        const auto profile = finishUvProfile(shaderPath);
        const auto defaults = shaders.value(shaderPath);
        auto resolved = defaults ? mergeModelMaterialDefaults(*defaults, material) : it.value();
        auto paths = finishTexturePaths(material, *resolved, profile);
        for (QString *path : {&paths.pattern, &paths.normal, &paths.surface, &paths.weaveMask}) {
            if (path->isEmpty()) continue;
            *path = normalizedAssetPath(*path);
            auto &wanted = isCarTexture(*path) ? wantedCarTextures : wantedMediaTextures;
            wanted.push_back(textureArchiveEntry(*path));
        }
        resolvedFinishes.insert(it.key(), {std::move(resolved), std::move(paths), profile});
    }
    const QString mediaTextures = gameAssetFilePath(QDir(gameMediaDir(gameFolder)), QStringLiteral("_library/Textures.zip"));
    const auto carBlobs = readPaintArchiveEntries(paintArchives(gamePaintTexturesArchive(gameFolder)), std::move(wantedCarTextures));
    const auto mediaBlobs = readPaintArchiveEntries(paintArchives(mediaTextures), std::move(wantedMediaTextures));
    const auto swatchImages = [&](const QString &reference, SwatchImage &image, std::vector<SwatchImage> &mips) {
        if (reference.isEmpty()) return;
        const auto &blobs = isCarTexture(reference) ? carBlobs : mediaBlobs;
        auto images = decodeSwatchMipImages(blobs.value(textureArchiveEntry(reference)));

        if (images.empty()) return;
        image = std::move(images.front());
        images.erase(images.begin());
        mips = std::move(images);
    };

    for (auto it = materials.cbegin(); it != materials.cend(); ++it) {
        const ModelMaterial &material = *it.value();
        const auto &resolved = resolvedFinishes[it.key()];
        const auto &paths = resolved.textures;
        const FinishMaterial &info = finishes[it.key()];
        PaintFinishRender render = renderFromMaterial(info, paths.weaveMask.isEmpty() ? material : *resolved.material);
        swatchImages(paths.pattern, render.patternImage, render.patternMips);
        swatchImages(paths.normal, render.detailNormalImage, render.detailNormalMips);
        swatchImages(paths.surface, render.roughMetalAoImage, render.roughMetalAoMips);
        if (!paths.weaveMask.isEmpty()) {
            swatchImages(paths.weaveMask, render.weaveMaskImage, render.weaveMaskMips);
            applyWeaveTints(render, *resolved.material);
            render.selfColored = true;
            const auto scalar = [&](quint32 hash, float fallback) {
                return std::clamp(finishScalar(*resolved.material, hash, fallback), 0.0f, 1.0f);
            };
            using namespace material_hashes::parameter;
            render.metallic = scalar(kWeaveMetalness, render.metallic);
            render.gloss = 1.0f - scalar(kWeaveRoughness, 1.0f - render.gloss);
            render.normalIntensity = scalar(kWeaveNormalIntensity, render.normalIntensity);
            render.anisotropy = scalar(kAnisotropy, render.anisotropy);
            render.clearCoatCoverage = scalar(kClearCoatCoverage, 1.0f);
            render.clearCoatRoughness = scalar(kClearcoatRoughness, 1.0f - resolved.material->gloss);
        }
        if (isOpaqueWhite(render.patternImage)) {
            render.patternImage = {};
            render.patternMips.clear();
        }
        applyFinishUvMappings(render, *resolved.material, resolved.profile, paths.mirrorNormal);
        applyFinishSurface(render, *resolved.material, resolved.profile, paths.mirrorNormal);
        if (!render.metalMask && isOpaqueWhite(render.roughMetalAoImage)) {
            render.roughMetalAoImage = {};
            render.roughMetalAoMips.clear();
        }
        byCode_.insert(it.key(), std::move(render));
    }
    loaded_ = !byCode_.isEmpty();
}

int PaintFinishLibrary::previewCode(quint32 code, const QString &paintGame) const {
    const QString source = paintGame.isEmpty() ? QStringLiteral("FH6") : paintGame;

    return source == game_ ? static_cast<int>(code) : matchingPaintFinish(code, source, game_);
}

const PaintFinishRender *PaintFinishLibrary::find(int code) const {
    const auto it = byCode_.constFind(code);

    return it != byCode_.constEnd() ? &it.value() : nullptr;
}

} // namespace fls
