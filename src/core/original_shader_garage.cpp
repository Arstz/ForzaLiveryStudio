#include "original_shader_garage.h"

#include "game_paths.h"
#include "binary_io.h"
#include "core_types.h"
#include "manufacturer_colors.h"
#include "model_bundle.h"
#include "model_material.h"
#include "paint_finish_catalog.h"
#include "pgzp_extract.h"
#include "tokyo_house_layout_generated.h"
#include "zip_extract.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace fh6 {

namespace {

constexpr float kCleanGarageSurfaceFrequency = 0.5f;

bool decodeTextureWithAuthoredMips(
    const QByteArray &bytes, SwatchImage *base,
    std::vector<SwatchImage> *mips, QString *error) {
    const std::optional<SwatchTexture> source = parseSwatchTexture(bytes, error);
    if (!source.has_value()) {
        return false;
    }
    *base = decodeSwatchImage(*source, 0, 0, error);
    if (!base->valid()) {
        return false;
    }
    mips->clear();
    for (int level = 1; level < source->mipCount; ++level) {
        SwatchImage mip = decodeSwatchImage(*source, 0, level, error);
        if (!mip.valid()) {
            break;
        }
        mips->push_back(std::move(mip));
    }
    return true;
}

float worldFrequencyTiling(
    const CarMesh &mesh, float frequency, float fallback,
    float minimum, float maximum) {
    const float uvDensity = meshUvWorldDensity(mesh, 0);
    if (uvDensity <= 0.0f) {
        return fallback;
    }

    return std::clamp(frequency / uvDensity, minimum, maximum);
}

constexpr float kClearCoatGlossFloor = 0.35f;
constexpr float kClearCoatGlossRange = 0.6f;
constexpr float kClearCoatMetalCoverage = 0.25f;
constexpr float kClearCoatMinRoughness = 0.04f;
constexpr float kClearCoatMaxRoughness = 0.4f;

struct GarageShellResource {
    const char *directory;
    const char *archive;
    OriginalShaderSurfaceFamily family;
};

// Default-House8.xml is authored directly in this enclosure's coordinate space:
// its instance extents match these six pieces to within the wall thickness.
constexpr std::array<GarageShellResource, 6> kGarageShellResources = {{
    {"bld_gbl_grge_custom_02_floor_a", "bld_gbl_grge_custom_02_floor_a.i.zip",
     OriginalShaderSurfaceFamily::Floor},
    {"bld_gbl_grge_custom_02_roof_a", "bld_gbl_grge_custom_02_roof_a.i.zip",
     OriginalShaderSurfaceFamily::Default},
    {"bld_gbl_grge_custom_02_walls_a", "bld_gbl_grge_custom_02_wall_a.i.zip",
     OriginalShaderSurfaceFamily::Default},
    {"bld_gbl_grge_custom_02_walls_b", "bld_gbl_grge_custom_02_wall_b.i.zip",
     OriginalShaderSurfaceFamily::Default},
    {"bld_gbl_grge_custom_02_walls_c", "bld_gbl_grge_custom_02_wall_c.i.zip",
     OriginalShaderSurfaceFamily::Default},
    {"bld_gbl_grge_custom_02_walls_d", "bld_gbl_grge_custom_02_wall_d.i.zip",
     OriginalShaderSurfaceFamily::Default},
}};

constexpr std::uint64_t kMainCarLocator = 9143969636317885086ull;
constexpr std::uint64_t kFloodLightProp = 8157816743302734859ull;

struct ProgramResource {
    const char *vertex;
    const char *pixel;
};

constexpr ProgramResource kDefaultProgram = {
    "homespace_default_001/homespace_default_001CubemapLightScenario.pcdxil.vso",
    "homespace_default_001/homespace_default_001CubemapLightScenario.pcdxil.pso",
};

constexpr ProgramResource kFloorProgram = {
    "homespace_floor_001/homespace_floor_001CubemapLightScenario.pcdxil.vso",
    "homespace_floor_001/homespace_floor_001CubemapLightScenario.pcdxil.pso",
};

struct TextureResource {
    const char *semantic;
    const char *entry;
};

// Slot order is recovered from the CubemapLightScenario DXIL texture-offset table.
// Repeated source entries are intentional shader-declared defaults, not guessed
// authored floor textures.
constexpr std::array<TextureResource, 7> kMaterialTextures = {{
    {"AO_LightingTexture", "test/swatches/flatwhite_diff_a8c0d933-6aa4-4ab3-8256-190f53595c25.swatchbin"},
    {"DiffuseTexture", "swatches/defaultshader_diff_9bc78e78-12e9-45b7-a907-3e62b84af58c.swatchbin"},
    {"Direct_Lighting0Texture", "test/swatches/flatwhite_diff_a8c0d933-6aa4-4ab3-8256-190f53595c25.swatchbin"},
    {"GlossTexture", "test/swatches/flatblack_diffusenoalpha_dd41dd07-3822-46c1-b37a-aefcc9e5ea59.swatchbin"},
    {"Indirect_LightingTexture", "test/swatches/flatblack_diff_8f56e0a3-0f9c-4707-86e7-382b93f4e3cc.swatchbin"},
    {"MicroNormalMapTexture", "test/swatches/normal_nrml_98be145f-e42e-48cb-bdfc-04afe6371274.swatchbin"},
    {"NormalMapTexture", "test/swatches/normal_nrml_98be145f-e42e-48cb-bdfc-04afe6371274.swatchbin"},
}};

struct ManifestTexture {
    QString canonicalPath;
    QString archiveLeaf;
};

QString pathLeaf(const QString &path) {
    const qsizetype slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    return path.mid(slash + 1);
}

quint32 forzaPathHash(const QString &path) {
    const QByteArray bytes = path.toUtf8();
    return static_cast<quint32>(crc32(
        0, reinterpret_cast<const Bytef *>(bytes.constData()),
        static_cast<uInt>(bytes.size())));
}

QHash<quint32, ManifestTexture> resolveManifestTextures(
    const QString &manifestPath, const QSet<quint32> &targets, QString *error) {
    QHash<quint32, ManifestTexture> resolved;
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1: %2")
                         .arg(manifestPath, manifest.errorString());
        }
        return resolved;
    }
    const QString preZipped = QStringLiteral("<PREZIPPED>");
    const QString zipCache = QStringLiteral("zipcache\\pc\\");
    const std::array<QString, 5> qualitySuffixes = {
        QStringLiteral("_quality1.pb"), QStringLiteral("_quality2.pb"),
        QStringLiteral("_quality3.pb"), QStringLiteral("_quality4.pb"),
        QStringLiteral("_quality5.pb")};
    const std::array<QString, 9> extensions = {
        QStringLiteral(".swatchbin"), QStringLiteral(".texturebin"),
        QStringLiteral(".modelbin"), QStringLiteral(".materialbin"),
        QStringLiteral(".shaderbin"), QStringLiteral(".anims"),
        QStringLiteral(".minizip"), QStringLiteral(".pb"), QString()};
    while (!manifest.atEnd() && resolved.size() < targets.size()) {
        QString line = QString::fromUtf8(manifest.readLine()).trimmed();
        if (!line.startsWith(preZipped, Qt::CaseInsensitive)) {
            continue;
        }
        QString physicalPath = line.mid(preZipped.size());
        const qsizetype separator = physicalPath.lastIndexOf('|');
        if (separator >= 0) {
            physicalPath.truncate(separator);
        }
        physicalPath = physicalPath.trimmed();
        QString lower = physicalPath.toLower();
        lower.replace('/', '\\');
        const qsizetype cache = lower.indexOf(zipCache);
        if (cache < 0) {
            continue;
        }
        QString logicalBase = lower.mid(cache + zipCache.size());
        bool strippedQuality = false;
        for (const QString &suffix : qualitySuffixes) {
            if (logicalBase.endsWith(suffix)) {
                logicalBase.chop(suffix.size());
                strippedQuality = true;
                break;
            }
        }
        if (!strippedQuality && logicalBase.endsWith(QStringLiteral(".pb"))) {
            logicalBase.chop(3);
        }
        const QString prefix = QStringLiteral("game:\\media\\") + logicalBase;
        for (const QString &extension : extensions) {
            const QString candidate = prefix + extension;
            const quint32 hash = forzaPathHash(candidate);
            if (targets.contains(hash) && !resolved.contains(hash)) {
                resolved.insert(hash, {candidate, pathLeaf(physicalPath)});
            }
        }
    }
    return resolved;
}

QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>>
loadAuthoredDiffuseTextures(
    const QString &archive, const QString &manifest,
    const std::vector<CarModel> &geometry,
    QString *error) {
    QSet<quint32> targetHashes;
    for (const CarModel &model : geometry) {
        for (const CarMesh &mesh : model.meshes) {
            if (mesh.material == nullptr) {
                continue;
            }
            for (const ModelMaterialParameter &parameter : mesh.material->parameters) {
                if (parameter.type == ModelMaterialParameterType::Texture2D
                    && parameter.texturePathHash != 0) {
                    targetHashes.insert(parameter.texturePathHash);
                }
            }
        }
    }
    const QHash<quint32, ManifestTexture> resolved =
        resolveManifestTextures(manifest, targetHashes, error);
    if (resolved.isEmpty()) {
        return {};
    }

    QHash<quint32, ManifestTexture> diffuseEntries;
    QStringList leaves;
    for (auto iterator = resolved.cbegin(); iterator != resolved.cend(); ++iterator) {
        if (!iterator->canonicalPath.contains(QStringLiteral("_bclr_"))) {
            continue;
        }
        diffuseEntries.insert(iterator.key(), iterator.value());
        if (!leaves.contains(iterator->archiveLeaf, Qt::CaseInsensitive)) {
            leaves.push_back(iterator->archiveLeaf);
        }
    }
    std::vector<PgzpExtractedEntry> extracted =
        extractPgzpEntries(archive, manifest, leaves, error);
    if (extracted.size() != static_cast<std::size_t>(leaves.size())) {
        return {};
    }
    QHash<QString, QByteArray> payloads;
    for (PgzpExtractedEntry &entry : extracted) {
        payloads.insert(entry.requestedName.toLower(), std::move(entry.bytes));
    }

    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> textures;
    for (auto iterator = diffuseEntries.cbegin(); iterator != diffuseEntries.cend(); ++iterator) {
        QString decodeError;
        SwatchImage image;
        std::vector<SwatchImage> mips;
        if (!decodeTextureWithAuthoredMips(
                payloads.value(iterator->archiveLeaf.toLower()),
                &image, &mips, &decodeError)) {
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2")
                             .arg(iterator->canonicalPath, decodeError);
            }
            return {};
        }
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = QStringLiteral("DiffuseTexture");
        texture->sourceEntry = iterator->canonicalPath;
        texture->image = std::move(image);
        texture->authoredMips = std::move(mips);
        textures.insert(iterator.key(), std::move(texture));
    }
    return textures;
}

QHash<quint32, ManifestTexture> resolveGeneralSceneTextures(
    const QString &manifestPath, const QSet<quint32> &targets, QString *error) {
    QHash<quint32, ManifestTexture> resolved;
    QFile manifest(manifestPath);
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1: %2")
                         .arg(manifestPath, manifest.errorString());
        }
        return resolved;
    }
    QXmlStreamReader xml(&manifest);
    while (!xml.atEnd() && resolved.size() < targets.size()) {
        xml.readNext();
        if (!xml.isStartElement()
            || xml.name().compare(QLatin1String("texture"), Qt::CaseInsensitive)
                != 0) {
            continue;
        }
        QString physical = xml.attributes().value(QLatin1String("value")).toString();
        if (physical.isEmpty()) {
            continue;
        }
        physical = physical.toLower();
        physical.replace(QLatin1Char('/'), QLatin1Char('\\'));
        QString base = physical;
        if (base.endsWith(QStringLiteral(".pti"))) {
            base.chop(4);
        }
        const QString prefix = QStringLiteral("game:\\media\\") + base;
        const std::array<QString, 4> candidates = {
            prefix + QStringLiteral(".swatchbin"),
            prefix + QStringLiteral(".texturebin"),
            prefix + QStringLiteral(".pti"), prefix};
        for (const QString &candidate : candidates) {
            const quint32 hash = forzaPathHash(candidate);
            if (!targets.contains(hash) || resolved.contains(hash)) {
                continue;
            }
            QString archiveEntry = base;
            archiveEntry.replace(QLatin1Char('\\'), QLatin1Char('/'));
            archiveEntry += QStringLiteral("_quality1.pb");
            resolved.insert(hash, {candidate, archiveEntry});
        }
    }
    if (xml.hasError() && error != nullptr) {
        *error = QStringLiteral("%1: %2").arg(manifestPath, xml.errorString());
    }
    return resolved;
}

bool isBaseColourTexture(const QString &path) {
    return path.contains(QStringLiteral("_bclr_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_diff_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_diffuse_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_albedo_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_color_"), Qt::CaseInsensitive);
}

bool isEmissiveTexture(const QString &path) {
    return path.contains(QStringLiteral("_emis_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_emissive_"), Qt::CaseInsensitive);
}

bool isNormalTexture(const QString &path) {
    return path.contains(QStringLiteral("_nrml_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_normal_"), Qt::CaseInsensitive);
}

bool isSurfaceTexture(const QString &path) {
    return path.contains(QStringLiteral("_rcsm_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_rmao_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_roughmetal"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_extra_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_mask_"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("_mod_"), Qt::CaseInsensitive);
}

QString textureSemantic(const QString &path) {
    if (isEmissiveTexture(path)) {
        return QStringLiteral("EmissiveTexture");
    }
    if (isNormalTexture(path)) {
        return QStringLiteral("NormalTexture");
    }
    if (isSurfaceTexture(path)) {
        return QStringLiteral("SurfaceTexture");
    }
    return QStringLiteral("DiffuseTexture");
}

// Track model instances store AssetManifest.xml SourceHash values. They are not
// CRC32s of the source path, despite occupying the same material parameter field
// used by path hashes elsewhere. Resolve the authoritative manifest mapping first,
// then use ChunkContentsMiniZip0.txt only to locate the PGZP payload leaf.
QHash<quint32, ManifestTexture> resolveTrackSceneTextures(
    const QString &assetManifestPath, const QSet<quint32> &targets, QString *error) {
    QHash<quint32, ManifestTexture> resolved;
    QFile manifest(assetManifestPath);
    if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1: %2")
                         .arg(assetManifestPath, manifest.errorString());
        }
        return resolved;
    }
    QXmlStreamReader xml(&manifest);
    while (!xml.atEnd() && resolved.size() < targets.size()) {
        xml.readNext();
        if (!xml.isStartElement()
            || xml.name().compare(QLatin1String("Texture"), Qt::CaseInsensitive) != 0) {
            continue;
        }
        bool validHash = false;
        const qlonglong signedHash = xml.attributes()
                                         .value(QLatin1String("SourceHash"))
                                         .toString()
                                         .toLongLong(&validHash);
        const quint32 sourceHash = static_cast<quint32>(signedHash);
        if (!validHash || !targets.contains(sourceHash) || resolved.contains(sourceHash)) {
            continue;
        }
        QString source = xml.attributes().value(QLatin1String("Source")).toString();
        if (source.isEmpty()) {
            continue;
        }
        source.replace(QLatin1Char('/'), QLatin1Char('\\'));
        QString archiveLeaf = pathLeaf(source);
        if (archiveLeaf.endsWith(QStringLiteral(".swatch"), Qt::CaseInsensitive)) {
            archiveLeaf.chop(7);
        }
        archiveLeaf += QStringLiteral("_quality1.pb");
        resolved.insert(sourceHash, {
            QStringLiteral("game:\\media\\") + source + QStringLiteral(".swatchbin"),
            archiveLeaf});
    }
    if (xml.hasError() && error != nullptr) {
        *error = QStringLiteral("%1: %2").arg(assetManifestPath, xml.errorString());
    }
    return resolved;
}

QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>>
loadTrackSceneTextures(
    const QString &archive, const QString &chunkManifest,
    const QString &assetManifest, const std::vector<CarModel> &geometry,
    QString *error) {
    QSet<quint32> targetHashes;
    for (const CarModel &model : geometry) {
        for (const CarMesh &mesh : model.meshes) {
            if (mesh.material == nullptr) {
                continue;
            }
            for (const ModelMaterialParameter &parameter : mesh.material->parameters) {
                if (parameter.type == ModelMaterialParameterType::Texture2D
                    && parameter.texturePathHash != 0) {
                    targetHashes.insert(parameter.texturePathHash);
                }
            }
        }
    }
    const QHash<quint32, ManifestTexture> resolved =
        resolveTrackSceneTextures(assetManifest, targetHashes, error);
    QStringList requested;
    QHash<QString, quint32> hashByEntry;
    for (auto iterator = resolved.cbegin(); iterator != resolved.cend(); ++iterator) {
        if (!isBaseColourTexture(iterator->canonicalPath)
            && !isEmissiveTexture(iterator->canonicalPath)
            && !isNormalTexture(iterator->canonicalPath)
            && !isSurfaceTexture(iterator->canonicalPath)) {
            continue;
        }
        if (!hashByEntry.contains(iterator->archiveLeaf.toLower())) {
            requested.push_back(iterator->archiveLeaf);
        }
        hashByEntry.insert(iterator->archiveLeaf.toLower(), iterator.key());
    }
    if (requested.isEmpty()) {
        if (error != nullptr && error->isEmpty()) {
            *error = QStringLiteral("none of %1 Tokyo texture SourceHash values resolved")
                         .arg(targetHashes.size());
        }
        return {};
    }
    std::vector<PgzpExtractedEntry> extracted =
        extractPgzpEntries(archive, chunkManifest, requested, error);
    if (extracted.size() != static_cast<std::size_t>(requested.size())) {
        return {};
    }
    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> textures;
    for (PgzpExtractedEntry &entry : extracted) {
        const quint32 hash = hashByEntry.value(entry.requestedName.toLower());
        const QString sourceEntry = resolved.value(hash).canonicalPath;
        QString decodeError;
        SwatchImage image;
        std::vector<SwatchImage> mips;
        if (!decodeTextureWithAuthoredMips(
                entry.bytes, &image, &mips, &decodeError)) {
            // Keep the scene usable and expose unresolved colour maps with the
            // diagnostic checker. Auxiliary compressed maps may be skipped.
            continue;
        }
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = textureSemantic(sourceEntry);
        texture->sourceEntry = sourceEntry;
        texture->image = std::move(image);
        texture->authoredMips = std::move(mips);
        textures.insert(hash, std::move(texture));
    }
    return textures;
}

QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>>
loadGeneralSceneTextures(
    const QString &archive, const QString &manifest,
    const std::vector<CarModel> &geometry, QString *error) {
    QSet<quint32> targetHashes;
    for (const CarModel &model : geometry) {
        for (const CarMesh &mesh : model.meshes) {
            if (mesh.material == nullptr) {
                continue;
            }
            for (const ModelMaterialParameter &parameter : mesh.material->parameters) {
                if (parameter.type == ModelMaterialParameterType::Texture2D
                    && parameter.texturePathHash != 0) {
                    targetHashes.insert(parameter.texturePathHash);
                }
            }
        }
    }
    // House 8's clean/restored floor is not referenced by the distressed shell
    // material. Pull its complete authored stack explicitly from the same general
    // scene archive so replacement includes normal and packed surface data too.
    constexpr std::array<const char *, 3> kCleanFloorTexturePaths = {{
        "game:\\media\\tracks\\brio\\textures\\assets\\el\\_ws\\tex_el_tile_concrete_polished_a\\swatches\\tex_el_tile_concrete_polished_a_bclr_a9a9207d-31b5-4920-b1ef-623bc8431398.swatchbin",
        "game:\\media\\tracks\\brio\\textures\\assets\\el\\_ws\\tex_el_tile_concrete_polished_a\\swatches\\tex_el_tile_concrete_polished_a_extra_6e18ac65-e7f2-4a86-a04e-df89fdf8956f.swatchbin",
        "game:\\media\\tracks\\brio\\textures\\assets\\el\\_ws\\tex_el_tile_concrete_polished_a\\swatches\\tex_el_tile_concrete_polished_a_nrml_e93b1adb-fe05-4c71-bfbf-e34523ecacf2.swatchbin",
    }};
    for (const char *path : kCleanFloorTexturePaths) {
        targetHashes.insert(forzaPathHash(QString::fromLatin1(path)));
    }
    const QHash<quint32, ManifestTexture> resolved =
        resolveGeneralSceneTextures(manifest, targetHashes, error);
    QStringList requested;
    QHash<QString, quint32> hashByEntry;
    for (auto iterator = resolved.cbegin(); iterator != resolved.cend(); ++iterator) {
        if (!isBaseColourTexture(iterator->canonicalPath)
            && !isEmissiveTexture(iterator->canonicalPath)
            && !isNormalTexture(iterator->canonicalPath)
            && !isSurfaceTexture(iterator->canonicalPath)) {
            continue;
        }
        const QString leaf = iterator->archiveLeaf.toLower();
        if (!hashByEntry.contains(leaf)) {
            requested.push_back(iterator->archiveLeaf);
        }
        hashByEntry.insert(leaf, iterator.key());
    }
    if (requested.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "no authored base-colour dependencies matched %1 material texture hashes")
                         .arg(targetHashes.size());
        }
        return {};
    }
    const QHash<QString, QByteArray> payloads =
        readZipEntries(archive, requested, error);
    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> textures;
    for (auto iterator = payloads.cbegin(); iterator != payloads.cend(); ++iterator) {
        const quint32 hash = hashByEntry.value(iterator.key());
        const QString sourceEntry = resolved.value(hash).canonicalPath;
        QString decodeError;
        SwatchImage image;
        std::vector<SwatchImage> mips;
        if (!decodeTextureWithAuthoredMips(
                iterator.value(), &image, &mips, &decodeError)) {
            // A few auxiliary maps use encodings that the preview decoder cannot
            // upload. Keep the other authored slots on that material active.
            continue;
        }
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = textureSemantic(sourceEntry);
        texture->sourceEntry = sourceEntry;
        texture->image = std::move(image);
        texture->authoredMips = std::move(mips);
        textures.insert(hash, std::move(texture));
    }
    return textures;
}

std::shared_ptr<const OriginalShaderMaterialTexture> meshDiffuseTexture(
    const CarMesh &mesh,
    const QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> &textures) {
    if (mesh.material == nullptr) {
        return {};
    }
    const bool emissiveMaterial = mesh.materialName.contains(
        QStringLiteral("emissive"), Qt::CaseInsensitive);
    for (int pass = 0; pass < 2; ++pass) {
        const QString wantedSemantic = (emissiveMaterial == (pass == 0))
            ? QStringLiteral("EmissiveTexture")
            : QStringLiteral("DiffuseTexture");
        std::shared_ptr<const OriginalShaderMaterialTexture> best;
        int bestScore = std::numeric_limits<int>::min();
        for (const ModelMaterialParameter &parameter : mesh.material->parameters) {
            if (parameter.type != ModelMaterialParameterType::Texture2D) {
                continue;
            }
            const auto found = textures.constFind(parameter.texturePathHash);
            if (found != textures.cend()
                && found.value()->semantic == wantedSemantic) {
                const QString &path = found.value()->sourceEntry;
                const int score = path.contains(
                                      QStringLiteral("_xsummerx_"),
                                      Qt::CaseInsensitive)
                    ? 2
                    : (path.contains(QStringLiteral("_xwinterx_"),
                                     Qt::CaseInsensitive)
                           ? 0
                           : 1);
                if (score > bestScore) {
                    best = found.value();
                    bestScore = score;
                }
            }
        }
        if (best != nullptr) {
            return best;
        }
    }
    return {};
}

std::shared_ptr<const OriginalShaderMaterialTexture> meshTexture(
    const CarMesh &mesh,
    const QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> &textures,
    const QString &semantic) {
    if (mesh.material == nullptr) {
        return {};
    }
    std::shared_ptr<const OriginalShaderMaterialTexture> best;
    int bestScore = std::numeric_limits<int>::min();
    for (auto parameter = mesh.material->parameters.crbegin();
         parameter != mesh.material->parameters.crend(); ++parameter) {
        if (parameter->type != ModelMaterialParameterType::Texture2D) {
            continue;
        }
        const auto found = textures.constFind(parameter->texturePathHash);
        if (found != textures.cend() && found.value()->semantic == semantic) {
            const QString &path = found.value()->sourceEntry;
            const int score = path.contains(
                                  QStringLiteral("_xsummerx_"),
                                  Qt::CaseInsensitive)
                ? 2
                : (path.contains(QStringLiteral("_xwinterx_"),
                                 Qt::CaseInsensitive)
                       ? 0
                       : 1);
            if (score > bestScore) {
                best = found.value();
                bestScore = score;
            }
        }
    }
    return best;
}

std::shared_ptr<const OriginalShaderMaterialTexture> authoredTextureByStem(
    const QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> &textures,
    const QString &stem, const QString &semantic) {
    for (auto texture = textures.cbegin(); texture != textures.cend(); ++texture) {
        if (texture.value()->semantic == semantic
            && texture.value()->sourceEntry.contains(stem, Qt::CaseInsensitive)) {
            return texture.value();
        }
    }

    return {};
}

ModelMat4 rebasePlinth(
    const CarModel &building, const CarModel &plinth) {
    ModelMat4 placement;
    placement.m[12] = (building.boundsMin.x + building.boundsMax.x
                           - plinth.boundsMin.x - plinth.boundsMax.x)
        * 0.5f;
    placement.m[13] = -plinth.boundsMin.y;
    placement.m[14] = (building.boundsMin.z + building.boundsMax.z
                           - plinth.boundsMin.z - plinth.boundsMax.z)
        * 0.5f;
    return placement;
}

ModelMat4 tokyoCarPlacement(const CarModel &building) {
    ModelMat4 placement;
    // The editable House-8 layout is in HomespaceLocator space, while the bespoke
    // building model is in chunk-local space. Anchor the focus car to the decoded
    // LOD0 shutter (the garage opening) and leave one car length of apron clearance.
    for (const CarMesh &mesh : building.meshes) {
        if (!mesh.materialName.contains(
                QStringLiteral("SHUTTER"), Qt::CaseInsensitive)
            || mesh.positions.empty()) {
            continue;
        }
        ModelVec3 minimum = mesh.boneTransform.transformPoint(mesh.positions.front());
        ModelVec3 maximum = minimum;
        for (const ModelVec3 &position : mesh.positions) {
            const ModelVec3 point = mesh.boneTransform.transformPoint(position);
            minimum.x = std::min(minimum.x, point.x);
            minimum.y = std::min(minimum.y, point.y);
            minimum.z = std::min(minimum.z, point.z);
            maximum.x = std::max(maximum.x, point.x);
            maximum.y = std::max(maximum.y, point.y);
            maximum.z = std::max(maximum.z, point.z);
        }
        placement.m[12] = (minimum.x + maximum.x) * 0.5f;
        placement.m[13] = building.boundsMin.y + 0.78f;
        placement.m[14] = maximum.z + 4.0f;
        return placement;
    }
    return placement;
}

void fail(OriginalShaderGarageScene *scene, const QString &context, const QString &detail) {
    scene->error = detail.isEmpty() ? context : QStringLiteral("%1: %2").arg(context, detail);
}

CarModel decodeModelBytes(
    const QByteArray &bytes, const QString &source, QString *error) {
    try {
        CarModel model = decodeModel(parseModelBundle(bytes), error);
        model.sourcePath = source;
        for (CarMesh &mesh : model.meshes) {
            mesh.sourceModelPath = source;
        }
        return model;
    } catch (const std::exception &exception) {
        if (error != nullptr) {
            *error = QString::fromUtf8(exception.what());
        }
        return {};
    }
}

bool loadProgram(
    const QString &archive, const ProgramResource &resource,
    OriginalShaderProgram *program, QString *error) {
    program->vertexShader = readZipEntry(
        archive, QString::fromLatin1(resource.vertex), error);
    if (program->vertexShader.isEmpty()) {
        return false;
    }
    program->pixelShader = readZipEntry(
        archive, QString::fromLatin1(resource.pixel), error);
    return !program->pixelShader.isEmpty();
}

SwatchImage solidColorImage(const std::array<float, 3> &color) {
    SwatchImage image;
    image.width = 1;
    image.height = 1;
    image.rgba = {
        static_cast<std::uint8_t>(std::lround(
            std::clamp(color[0], 0.0f, 1.0f) * 255.0f)),
        static_cast<std::uint8_t>(std::lround(
            std::clamp(color[1], 0.0f, 1.0f) * 255.0f)),
        static_cast<std::uint8_t>(std::lround(
            std::clamp(color[2], 0.0f, 1.0f) * 255.0f)),
        255};
    return image;
}

struct CarMaterialFallback {
    std::array<float, 3> color;
    float gloss = 0.45f;
    float metallic = 0.0f;
    float minimumColor = 0.0f;
};

QString carMaterialIdentity(const CarMesh &mesh) {
    QString identity = mesh.materialName.toLower();
    if (mesh.material != nullptr) {
        identity += QLatin1Char('|') + mesh.material->name.toLower();
        identity += QLatin1Char('|') + mesh.material->resourcePath.toLower();
    }
    identity.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return identity;
}

bool usesCarbonFiberShader(const CarMesh &mesh) {
    if (mesh.material == nullptr) {
        return false;
    }
    QString resource = mesh.material->resourcePath.toLower();
    resource.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return resource.contains(QStringLiteral("/carbonfiber/carbonfiber.materialbin"));
}

std::optional<CarMaterialFallback> carMaterialFallback(const CarMesh &mesh) {
    QString source = mesh.sourceModelPath.toLower();
    source.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString slot = mesh.materialName.toLower();
    const int separator = slot.indexOf(QLatin1Char('|'));
    if (separator >= 0) {
        slot.truncate(separator);
    }
    const bool tire = source.contains(QStringLiteral("/tires/"))
        || mesh.name.startsWith(QStringLiteral("tire"), Qt::CaseInsensitive);
    if (tire) {
        return CarMaterialFallback{{0.018f, 0.019f, 0.021f}, 0.22f, 0.0f};
    }
    const bool wheel = source.contains(QStringLiteral("/wheels/"))
        || mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive);
    if (wheel) {
        if (slot == QStringLiteral("black")) {
            return CarMaterialFallback{{0.015f, 0.015f, 0.018f}, 0.25f, 0.0f};
        }
        if (slot == QStringLiteral("rim") || slot == QStringLiteral("rim2")
            || slot == QStringLiteral("rim3")) {
            return CarMaterialFallback{{0.32f, 0.34f, 0.37f}, 0.78f, 0.85f};
        }
        if (slot == QStringLiteral("inner_rim") || slot == QStringLiteral("hub")) {
            return CarMaterialFallback{{0.12f, 0.13f, 0.15f}, 0.48f, 0.65f};
        }
        if (slot == QStringLiteral("lug") || slot == QStringLiteral("lip")
            || slot == QStringLiteral("valve_cap")) {
            return CarMaterialFallback{{0.38f, 0.40f, 0.43f}, 0.72f, 0.80f};
        }
        if (slot.contains(QStringLiteral("badge"))
            || slot.contains(QStringLiteral("emblem"))) {
            return CarMaterialFallback{{0.46f, 0.49f, 0.53f}, 0.86f, 0.78f};
        }
    }

    const QString identity = carMaterialIdentity(mesh);
    // Prefer the authored slot family over a generic inherited resource path.  Some embedded
    // interiors name leather/plastic/fabric here while their MaterialResource points at a
    // carbon-fibre or BlackHole base; those paths are not the visible surface classification.
    if (slot.contains(QStringLiteral("leather"))) {
        return CarMaterialFallback{{0.045f, 0.040f, 0.038f}, 0.38f, 0.0f};
    }
    if (slot.contains(QStringLiteral("fabric"))
        || slot.contains(QStringLiteral("alcantara"))
        || slot.contains(QStringLiteral("carpet"))
        || slot.contains(QStringLiteral("cloth"))
        || slot.contains(QStringLiteral("felt"))) {
        return CarMaterialFallback{{0.025f, 0.027f, 0.030f}, 0.16f, 0.0f};
    }
    if (slot.contains(QStringLiteral("stitch"))) {
        return CarMaterialFallback{{0.22f, 0.20f, 0.18f}, 0.28f, 0.0f};
    }
    if (slot.contains(QStringLiteral("wood"))) {
        return CarMaterialFallback{{0.20f, 0.085f, 0.030f}, 0.52f, 0.0f};
    }
    if (slot.contains(QStringLiteral("reflector"))) {
        return CarMaterialFallback{{0.52f, 0.12f, 0.055f}, 0.82f, 0.20f};
    }
    if (slot.contains(QStringLiteral("plastic"))) {
        return CarMaterialFallback{{0.025f, 0.027f, 0.030f}, 0.30f, 0.0f};
    }
    if (slot == QStringLiteral("frame")) {
        return CarMaterialFallback{{0.075f, 0.080f, 0.088f}, 0.48f, 0.55f};
    }
    if (slot.startsWith(QStringLiteral("metal_"))) {
        return CarMaterialFallback{{0.30f, 0.32f, 0.35f}, 0.72f, 0.88f};
    }
    if (identity.contains(QStringLiteral("mirror_left"))
        || identity.contains(QStringLiteral("mirror_right"))) {
        return CarMaterialFallback{{0.42f, 0.48f, 0.56f}, 0.98f, 0.82f};
    }
    if (identity.contains(QStringLiteral("chrome"))) {
        return CarMaterialFallback{{0.68f, 0.71f, 0.76f}, 0.96f, 1.0f};
    }
    if (identity.contains(QStringLiteral("gold"))) {
        return CarMaterialFallback{{0.64f, 0.43f, 0.10f}, 0.86f, 0.92f};
    }
    if (identity.contains(QStringLiteral("aluminum"))) {
        return CarMaterialFallback{{0.42f, 0.44f, 0.46f}, 0.62f, 0.82f};
    }
    if (identity.contains(QStringLiteral("titanium"))) {
        return CarMaterialFallback{{0.30f, 0.31f, 0.34f}, 0.68f, 0.88f};
    }
    if (identity.contains(QStringLiteral("gunmetal"))
        || identity.contains(QStringLiteral("anodizedmetal"))) {
        return CarMaterialFallback{{0.10f, 0.11f, 0.13f}, 0.72f, 0.86f};
    }
    if (identity.contains(QStringLiteral("steel"))
        || identity.contains(QStringLiteral("metallic"))) {
        return CarMaterialFallback{{0.28f, 0.30f, 0.33f}, 0.70f, 0.84f};
    }
    if (identity.contains(QStringLiteral("paintedmetal"))) {
        return CarMaterialFallback{{0.055f, 0.058f, 0.064f}, 0.58f, 0.45f};
    }
    if (identity.contains(QStringLiteral("carbon"))) {
        return CarMaterialFallback{{0.085f, 0.092f, 0.10f}, 0.78f, 0.0f, 0.08f};
    }
    if (identity.contains(QStringLiteral("rubber"))) {
        return CarMaterialFallback{{0.018f, 0.019f, 0.021f}, 0.18f, 0.0f};
    }
    if (identity.contains(QStringLiteral("blackframe"))
        || identity.contains(QStringLiteral("blackhole"))
        || identity.contains(QStringLiteral("grille"))) {
        return CarMaterialFallback{{0.008f, 0.009f, 0.011f}, 0.22f, 0.05f};
    }
    if (identity.contains(QStringLiteral("plastic"))) {
        return CarMaterialFallback{{0.025f, 0.027f, 0.030f}, 0.30f, 0.0f};
    }
    if (identity.contains(QStringLiteral("badge"))) {
        return CarMaterialFallback{{0.46f, 0.49f, 0.53f}, 0.86f, 0.78f};
    }
    if (identity.contains(QStringLiteral("plate"))) {
        return CarMaterialFallback{{0.78f, 0.79f, 0.75f}, 0.35f, 0.0f};
    }
    return std::nullopt;
}

CarMaterialFallback homespaceMaterialFallback(const CarMesh &mesh) {
    const QString name = mesh.materialName.toLower();
    if (name.contains(QStringLiteral("wood"))) {
        return {{0.30f, 0.14f, 0.055f}, 0.42f, 0.0f};
    }
    if (name.contains(QStringLiteral("porcelain"))) {
        return {{0.025f, 0.028f, 0.032f}, 0.72f, 0.02f};
    }
    if (name.contains(QStringLiteral("stone"))) {
        return {{0.19f, 0.20f, 0.22f}, 0.30f, 0.0f};
    }
    if (name.contains(QStringLiteral("black"))
        && name.contains(QStringLiteral("metal"))) {
        return {{0.025f, 0.028f, 0.033f}, 0.76f, 0.72f};
    }
    if (name.contains(QStringLiteral("grey"))
        && name.contains(QStringLiteral("metal"))) {
        return {{0.22f, 0.24f, 0.27f}, 0.70f, 0.75f};
    }
    if (name.contains(QStringLiteral("perforated"))) {
        return {{0.10f, 0.11f, 0.13f}, 0.62f, 0.68f};
    }
    if (name.contains(QStringLiteral("ceiling_light"))
        || name.contains(QStringLiteral("led_strip"))) {
        return {{0.72f, 0.68f, 0.56f}, 0.82f, 0.15f};
    }
    if (name.contains(QStringLiteral("garage_door_motor"))) {
        return {{0.055f, 0.060f, 0.068f}, 0.48f, 0.50f};
    }
    if (name.contains(QStringLiteral("feature"))) {
        return {{0.20f, 0.12f, 0.075f}, 0.34f, 0.0f};
    }
    if (name.contains(QStringLiteral("tent"))) {
        return {{0.12f, 0.13f, 0.15f}, 0.20f, 0.0f};
    }
    return {{0.16f, 0.17f, 0.19f}, 0.40f, 0.0f};
}

quint32 solidColorKey(const std::array<float, 3> &color) {
    const SwatchImage image = solidColorImage(color);
    return static_cast<quint32>(image.rgba[0])
        | (static_cast<quint32>(image.rgba[1]) << 8)
        | (static_cast<quint32>(image.rgba[2]) << 16)
        | 0xff000000u;
}

constexpr int kStandaloneLodRank = 500;

int lodRank(const QString &name) {
    const int marker = name.lastIndexOf(QStringLiteral("_LOD"));
    if (marker < 0) {
        return kStandaloneLodRank;
    }
    const QString suffix = name.mid(marker + 4);
    if (suffix.startsWith(QLatin1Char('S'), Qt::CaseInsensitive)) {
        bool valid = false;
        const int level = suffix.mid(1).toInt(&valid);
        return 1000 - (valid ? level : 0);
    }
    bool valid = false;
    const int level = suffix.toInt(&valid);
    return valid ? 100 - level : kStandaloneLodRank;
}

QString lodGroup(const CarMesh &mesh) {
    const int marker = mesh.name.lastIndexOf(QStringLiteral("_LOD"));
    const QString base = marker < 0 ? mesh.name : mesh.name.left(marker);
    return mesh.modelInstanceId >= 0
        ? QString::number(mesh.modelInstanceId) + QLatin1Char('|') + base
        : base;
}

std::vector<char> highestLodFlags(const std::vector<CarMesh> &meshes) {
    QHash<QString, int> bestRanks;
    for (const CarMesh &mesh : meshes) {
        const QString group = lodGroup(mesh);
        const int rank = lodRank(mesh.name);
        auto found = bestRanks.find(group);
        if (found == bestRanks.end() || rank > found.value()) {
            bestRanks.insert(group, rank);
        }
    }
    std::vector<char> keep(meshes.size(), 0);
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        keep[index] = lodRank(meshes[index].name)
                == bestRanks.value(lodGroup(meshes[index]))
            ? 1 : 0;
    }
    return keep;
}

bool isInteriorWindowShell(QString name) {
    name = name.toLower();
    const int separator = name.indexOf(QLatin1Char('|'));
    if (separator >= 0) {
        name.truncate(separator);
    }
    return name.startsWith(QStringLiteral("glass"))
        && name.contains(QStringLiteral("int"));
}

bool isWindowGlassMaterial(const CarMesh &mesh) {
    const QString name = mesh.materialName.toLower();
    if (name.isEmpty() || name.contains(QStringLiteral("screw"))
        || name.contains(QStringLiteral("frame"))
        || name.contains(QStringLiteral("label"))
        || name.contains(QStringLiteral("bulb"))
        || name.contains(QStringLiteral("light"))) {
        return false;
    }
    QString resource = mesh.material != nullptr
        ? mesh.material->resourcePath.toLower() : QString();
    resource.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const bool glassResource = resource.isEmpty()
        || resource.contains(QStringLiteral("/glass/"));
    return name.contains(QStringLiteral("window"))
        || name.contains(QStringLiteral("windshield"))
        || name.contains(QStringLiteral("windsheild"))
        || (name.contains(QStringLiteral("blackglass")) && glassResource);
}

bool isGlassSurface(const CarMesh &mesh) {
    QString resource = mesh.material != nullptr
        ? mesh.material->resourcePath.toLower() : QString();
    resource.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return isWindowGlassMaterial(mesh)
        || mesh.materialName.contains(QStringLiteral("glass"), Qt::CaseInsensitive)
        || resource.contains(QStringLiteral("/glass/"));
}

bool decodeGarageLights(
    const QByteArray &bytes, std::vector<OriginalShaderPointLight> *lights,
    QString *error) {
    constexpr qsizetype kHeaderSize = 20;
    constexpr qsizetype kRecordSize = 72;
    if (bytes.size() < kHeaderSize || bytes.first(4) != QByteArray("PVSL", 4)) {
        if (error != nullptr) {
            *error = QStringLiteral("lightdb: invalid PVSL header");
        }
        return false;
    }
    const quint32 count = detail::readLeU32(bytes, 16);
    if (count == 0
        || static_cast<quint64>(count) * kRecordSize + kHeaderSize
            > static_cast<quint64>(bytes.size())) {
        if (error != nullptr) {
            *error = QStringLiteral("lightdb: invalid light-model table");
        }
        return false;
    }
    lights->clear();
    lights->reserve(count);
    for (quint32 index = 0; index < count; ++index) {
        const qsizetype offset = kHeaderSize + index * kRecordSize;
        OriginalShaderPointLight light;
        for (int component = 0; component < 16; ++component) {
            const quint32 bits = detail::readLeU32(
                bytes, static_cast<int>(offset + component * 4));
            std::memcpy(&light.transform.m[component], &bits, sizeof(bits));
        }
        light.presetHash = detail::readLeU32(bytes, offset + 64);
        lights->push_back(light);
    }
    return true;
}

bool decodeGarageLightPresets(
    const QByteArray &bytes, std::vector<OriginalShaderPointLight> *lights,
    QString *error) {
    const qsizetype header = bytes.indexOf(QByteArray("LDFB", 4));
    if (header < 0 || header + 16 > bytes.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("lightdb: invalid LDFB preset header");
        }
        return false;
    }
    const quint32 version = detail::readLeU32(bytes, header + 8);
    const quint32 count = detail::readLeU32(bytes, header + 12);
    if (version < 2 || count == 0
        || header + 16 + static_cast<qsizetype>(count) * 4 > bytes.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("lightdb: unsupported preset table");
        }
        return false;
    }
    qsizetype cursor = header + 16 + static_cast<qsizetype>(count) * 4;
    int matched = 0;
    for (quint32 index = 0; index < count; ++index) {
        if (cursor + 8 > bytes.size()) {
            break;
        }
        const qsizetype blockStart = cursor;
        const quint32 blockSize = detail::readLeU32(bytes, cursor);
        const quint32 parameterCount = detail::readLeU32(bytes, cursor + 4);
        cursor += 8;
        const quint32 hash = detail::readLeU32(
            bytes, header + 16 + static_cast<qsizetype>(index) * 4);
        std::vector<OriginalShaderPointLight *> matchingLights;
        for (OriginalShaderPointLight &candidate : *lights) {
            if (candidate.presetHash == hash) {
                matchingLights.push_back(&candidate);
            }
        }
        for (quint32 parameter = 0;
             parameter < parameterCount && cursor + 2 <= bytes.size();
             ++parameter) {
            const quint8 id = static_cast<quint8>(bytes[cursor]);
            const quint8 length = static_cast<quint8>(bytes[cursor + 1]);
            cursor += 2;
            if (cursor + length > bytes.size()) {
                break;
            }
            for (OriginalShaderPointLight *light : matchingLights) {
                auto readFloat = [&]() {
                    const quint32 bits = detail::readLeU32(bytes, cursor);
                    float value = 0.0f;
                    std::memcpy(&value, &bits, sizeof(value));
                    return value;
                };
                if (id == 0x00 && length == 1) {
                    light->enabled = bytes[cursor] != 0;
                } else if (id == 0x01 && length == 4) {
                    light->type = detail::readLeU32(bytes, cursor);
                } else if (id == 0x02 && length == 12) {
                    float *components[] = {
                        &light->color.x, &light->color.y, &light->color.z};
                    for (int component = 0; component < 3; ++component) {
                        const quint32 bits = detail::readLeU32(
                            bytes, cursor + component * 4);
                        std::memcpy(components[component], &bits, sizeof(float));
                    }
                } else if (id == 0x03 && length == 4) {
                    light->range = readFloat();
                } else if (id == 0x04 && length == 4) {
                    light->intensity = readFloat();
                } else if (id == 0x05 && length == 4) {
                    light->penumbraAngleDegrees = readFloat();
                } else if (id == 0x06 && length == 4) {
                    light->coneAngleDegrees = readFloat();
                }
            }
            cursor += length;
        }
        matched += static_cast<int>(matchingLights.size());
        if (blockSize < 8 || blockStart + blockSize > bytes.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("lightdb: malformed preset block");
            }
            return false;
        }
        cursor = blockStart + blockSize;
    }
    if (matched == 0) {
        if (error != nullptr) {
            *error = QStringLiteral("lightdb: no light-model preset hashes matched");
        }
        return false;
    }
    return true;
}

} // namespace

bool OriginalShaderGarageScene::valid() const {
    return error.isEmpty() && !draws.empty()
        && std::all_of(draws.cbegin(), draws.cend(), [](const auto &draw) {
            return draw.valid();
        })
        && defaultProgram.valid() && floorProgram.valid()
        && std::all_of(
            materialTextures.cbegin(), materialTextures.cend(),
            [](const auto &texture) { return texture.valid(); })
        && environment.valid() && environment.panorama.valid() && colorLut.valid();
}

long long OriginalShaderGarageScene::totalVertices() const {
    long long total = 0;
    for (const OriginalShaderGarageDraw &draw : draws) {
        total += draw.geometry.totalVertices();
    }
    return total;
}

long long OriginalShaderGarageScene::totalTriangles() const {
    long long total = 0;
    for (const OriginalShaderGarageDraw &draw : draws) {
        total += draw.geometry.totalIndices() / 3;
    }
    return total;
}

OriginalShaderGarageScene loadOriginalShaderGarageScene(
    const QString &gameFolder, const SwatchImage &missingTexture) {
    OriginalShaderGarageScene scene;
    scene.name = QStringLiteral("Tokyo House");
    for (const tokyo_house_layout::PropInstance &instance
         : tokyo_house_layout::kInstances) {
        if (instance.id == kMainCarLocator) {
            scene.carPlacement.m = instance.transform;
            break;
        }
    }
    scene.geometryStatus = QStringLiteral(
        "Garage customiser enclosure and exact Default-House8 prop layout are loading");
    scene.materialStatus = QStringLiteral(
        "Garage customiser and House 8 general-scene material bindings are loading");
    scene.lighting = {
        {-0.197420f, -0.302618f, 0.932442f},
        {0.900001f, 0.900000f, 0.750001f},
        {0.142109f, 0.175171f, 0.197850f},
        QStringLiteral("homespace compatibility light constants")};
    scene.lightingStatus = QStringLiteral(
        "House 8 artificial-light records are loading");
    scene.glassStatus = QStringLiteral(
        "Garage and car glass use the DX12 translucent material pass");
    scene.carStatus = QStringLiteral(
        "House 8 main-car locator loaded from Default-House8.xml");

    const QString media = gameMediaDir(gameFolder);
    if (media.isEmpty()) {
        scene.error = QStringLiteral("game folder is not configured");
        return scene;
    }

    const QString stripped = QDir(media).filePath(QStringLiteral("Stripped/gs"));
    const QString shellDirectory = QDir(stripped).filePath(QStringLiteral(
        "Tracks/Brio/scene/models/buildings/global/garage_customiser"));
    const QString generalManifest =
        QDir(stripped).filePath(QStringLiteral("generalsceneassets.manifest"));
    const QString swatchArchive =
        QDir(stripped).filePath(QStringLiteral("swatchbins.zip"));
    QString error;
    std::vector<CarModel> geometry;
    std::vector<QString> sources;
    std::vector<OriginalShaderSurfaceFamily> families;
    QHash<quint64, std::size_t> geometryByProp;
    geometry.reserve(kGarageShellResources.size() + tokyo_house_layout::kAssets.size());
    sources.reserve(geometry.capacity());
    families.reserve(geometry.capacity());
    QString roofLightArchive;
    for (const GarageShellResource &resource : kGarageShellResources) {
        const QString archive = QDir(shellDirectory).filePath(
            QStringLiteral("%1/%2")
                .arg(QString::fromLatin1(resource.directory),
                     QString::fromLatin1(resource.archive)));
        const QString source = QStringLiteral("%1!/000.i.modelbin").arg(archive);
        CarModel model = decodeModelBytes(
            readZipEntry(archive, QStringLiteral("000.i.modelbin"), &error),
            source, &error);
        if (model.meshes.empty()) {
            fail(&scene, QStringLiteral("garage customiser enclosure model"), error);
            return scene;
        }
        if (QString::fromLatin1(resource.directory).contains(
                QStringLiteral("roof"), Qt::CaseInsensitive)) {
            roofLightArchive = archive;
        }
        sources.push_back(source);
        geometry.push_back(std::move(model));
        families.push_back(resource.family);
    }
    const std::size_t shellModelCount = geometry.size();
    QString floodLightArchive;
    for (const tokyo_house_layout::PropAsset &asset
         : tokyo_house_layout::kAssets) {
        const QString relative = QString::fromLatin1(asset.archive);
        if (relative.contains(QStringLiteral("/Locators/"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString archive = QDir(stripped).filePath(relative);
        const QString source = QStringLiteral("%1!/000.i.modelbin").arg(archive);
        error.clear();
        CarModel model = decodeModelBytes(
            readZipEntry(archive, QStringLiteral("000.i.modelbin"), &error),
            source, &error);
        if (model.meshes.empty()) {
            fail(&scene, QStringLiteral("House 8 prop model %1").arg(relative), error);
            return scene;
        }
        geometryByProp.insert(asset.id, geometry.size());
        sources.push_back(source);
        geometry.push_back(std::move(model));
        families.push_back(OriginalShaderSurfaceFamily::Default);
        if (asset.id == kFloodLightProp) {
            floodLightArchive = archive;
        }
    }
    error.clear();
    const auto authoredTextures = loadGeneralSceneTextures(
        swatchArchive, generalManifest, geometry, &error);
    const QString paintedMetalStem = QStringLiteral("tex_gbl_tile_mtl_paint_a");
    const QString polishedConcreteStem = QStringLiteral("tex_gbl_grnd_concrete_pol_a");
    const QString cleanFloorStem = QStringLiteral("tex_el_tile_concrete_polished_a");
    const auto cleanPaintedDiffuse = authoredTextureByStem(
        authoredTextures, paintedMetalStem, QStringLiteral("DiffuseTexture"));
    const auto cleanPaintedNormal = authoredTextureByStem(
        authoredTextures, paintedMetalStem, QStringLiteral("NormalTexture"));
    const auto cleanPaintedSurface = authoredTextureByStem(
        authoredTextures, paintedMetalStem, QStringLiteral("SurfaceTexture"));
    const auto cleanConcreteDiffuse = authoredTextureByStem(
        authoredTextures, polishedConcreteStem, QStringLiteral("DiffuseTexture"));
    const auto cleanConcreteNormal = authoredTextureByStem(
        authoredTextures, polishedConcreteStem, QStringLiteral("NormalTexture"));
    const auto cleanConcreteSurface = authoredTextureByStem(
        authoredTextures, polishedConcreteStem, QStringLiteral("SurfaceTexture"));
    const auto cleanFloorDiffuse = authoredTextureByStem(
        authoredTextures, cleanFloorStem, QStringLiteral("DiffuseTexture"));
    const auto cleanFloorNormal = authoredTextureByStem(
        authoredTextures, cleanFloorStem, QStringLiteral("NormalTexture"));
    const auto cleanFloorSurface = authoredTextureByStem(
        authoredTextures, cleanFloorStem, QStringLiteral("SurfaceTexture"));
    std::size_t estimatedDraws = 0;
    for (std::size_t index = 0; index < shellModelCount; ++index) {
        estimatedDraws += geometry[index].meshes.size();
    }
    for (const tokyo_house_layout::PropInstance &instance
         : tokyo_house_layout::kInstances) {
        const auto found = geometryByProp.constFind(instance.id);
        if (found != geometryByProp.cend()) {
            estimatedDraws += geometry[found.value()].meshes.size();
        }
    }
    scene.draws.reserve(estimatedDraws);
    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>>
        solidTextures;
    std::shared_ptr<const OriginalShaderMaterialTexture> missingDiagnostic;
    if (missingTexture.valid()) {
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = QStringLiteral("DiffuseTexture");
        texture->sourceEntry = QStringLiteral("raster/MissingTexture.png");
        texture->image = missingTexture;
        missingDiagnostic = std::move(texture);
    }
    int texturedDraws = 0;
    int normalDraws = 0;
    int surfaceDraws = 0;
    int emissiveDraws = 0;
    int materialFallbackDraws = 0;
    int unresolvedDraws = 0;
    int cleanShellDraws = 0;
    const auto appendModel = [&](
                                 const std::size_t modelIndex,
                                 const ModelMat4 &placement) {
        const CarModel &model = geometry[modelIndex];
        const ModelVec3 boundsMin = model.boundsMin;
        const ModelVec3 boundsMax = model.boundsMax;
        const QString modelSource = model.sourcePath;
        for (const CarMesh &mesh : model.meshes) {
            CarModel singleMesh;
            singleMesh.sourcePath = modelSource;
            singleMesh.boundsMin = boundsMin;
            singleMesh.boundsMax = boundsMax;
            const QString drawName = mesh.materialName;
            auto diffuseTexture = meshDiffuseTexture(mesh, authoredTextures);
            bool generatedSolidBase = false;
            const CarMaterialFallback fallback = homespaceMaterialFallback(mesh);
            if (diffuseTexture != nullptr) {
                ++texturedDraws;
            } else if (mesh.material != nullptr && mesh.material->hasBaseColor) {
                const quint32 key = solidColorKey(mesh.material->baseColor);
                diffuseTexture = solidTextures.value(key);
                if (diffuseTexture == nullptr) {
                    auto texture = std::make_shared<OriginalShaderMaterialTexture>();
                    texture->semantic = QStringLiteral("DiffuseTexture");
                    texture->sourceEntry = QStringLiteral("material://base-colour/%1")
                        .arg(key, 8, 16, QLatin1Char('0'));
                    texture->image = solidColorImage(mesh.material->baseColor);
                    diffuseTexture = texture;
                    solidTextures.insert(key, std::move(texture));
                }
                generatedSolidBase = true;
                ++materialFallbackDraws;
            } else if (isGlassSurface(mesh)) {
                const quint32 key = solidColorKey(fallback.color);
                diffuseTexture = solidTextures.value(key);
                if (diffuseTexture == nullptr) {
                    auto texture = std::make_shared<OriginalShaderMaterialTexture>();
                    texture->semantic = QStringLiteral("DiffuseTexture");
                    texture->sourceEntry = QStringLiteral("material://glass/%1")
                        .arg(key, 8, 16, QLatin1Char('0'));
                    texture->image = solidColorImage(fallback.color);
                    diffuseTexture = texture;
                    solidTextures.insert(key, std::move(texture));
                }
                generatedSolidBase = true;
                ++materialFallbackDraws;
            } else if (missingDiagnostic != nullptr) {
                diffuseTexture = missingDiagnostic;
                ++unresolvedDraws;
            } else {
                std::array<float, 3> color = fallback.color;
                const quint32 key = solidColorKey(color);
                diffuseTexture = solidTextures.value(key);
                if (diffuseTexture == nullptr) {
                    auto texture = std::make_shared<OriginalShaderMaterialTexture>();
                    texture->semantic = QStringLiteral("DiffuseTexture");
                    texture->sourceEntry = QStringLiteral("material://base-colour/%1")
                        .arg(key, 8, 16, QLatin1Char('0'));
                    texture->image = solidColorImage(color);
                    diffuseTexture = texture;
                    solidTextures.insert(key, std::move(texture));
                }
                generatedSolidBase = true;
                ++materialFallbackDraws;
            }
            auto normalTexture = meshTexture(
                mesh, authoredTextures, QStringLiteral("NormalTexture"));
            auto surfaceTexture = meshTexture(
                mesh, authoredTextures, QStringLiteral("SurfaceTexture"));
            auto emissiveTexture = meshTexture(
                mesh, authoredTextures, QStringLiteral("EmissiveTexture"));
            const QString lowerMaterialName = mesh.materialName.toLower();
            const bool shellSurface = modelIndex < shellModelCount;
            const bool shellDecal = lowerMaterialName.contains(
                    QStringLiteral("decal_comm"))
                || lowerMaterialName.contains(QStringLiteral("decal_urb"));
            const bool shellAlphaOverlay = lowerMaterialName.endsWith(
                QStringLiteral("_b_alpha"));
            const bool cleanFloorShell = modelIndex < shellModelCount
                && families[modelIndex] == OriginalShaderSurfaceFamily::Floor
                && !shellDecal && !shellAlphaOverlay
                && !lowerMaterialName.contains(QStringLiteral("emissive"));
            const bool cleanConcreteShell = modelIndex < shellModelCount
                && mesh.materialName == QStringLiteral("BLD_GBL_GRGE_CUSTOM_02_B");
            const bool cleanPaintedShell = shellSurface
                && !cleanFloorShell && !cleanConcreteShell
                && !shellDecal && !shellAlphaOverlay
                && !isGlassSurface(mesh)
                && !lowerMaterialName.contains(QStringLiteral("grnd_concrete"))
                && !lowerMaterialName.contains(QStringLiteral("emissive"));
            if (cleanFloorShell && cleanFloorDiffuse != nullptr) {
                diffuseTexture = cleanFloorDiffuse;
                normalTexture = cleanFloorNormal;
                surfaceTexture = cleanFloorSurface;
                ++cleanShellDraws;
            } else if (cleanPaintedShell && cleanPaintedDiffuse != nullptr) {
                diffuseTexture = cleanPaintedDiffuse;
                normalTexture = cleanPaintedNormal;
                surfaceTexture = cleanPaintedSurface;
                ++cleanShellDraws;
            } else if (cleanConcreteShell && cleanConcreteDiffuse != nullptr) {
                diffuseTexture = cleanConcreteDiffuse;
                normalTexture = cleanConcreteNormal;
                surfaceTexture = cleanConcreteSurface;
                ++cleanShellDraws;
            }
            normalDraws += normalTexture != nullptr ? 1 : 0;
            surfaceDraws += surfaceTexture != nullptr ? 1 : 0;
            emissiveDraws += emissiveTexture != nullptr ? 1 : 0;
            OriginalShaderGarageDraw draw;
            draw.name = drawName;
            draw.source = sources[modelIndex];
            draw.family = families[modelIndex];
            draw.placement = placement;
            draw.diffuseTexture = std::move(diffuseTexture);
            draw.normalTexture = std::move(normalTexture);
            draw.surfaceTexture = std::move(surfaceTexture);
            draw.emissiveTexture = std::move(emissiveTexture);
            draw.translucent = isGlassSurface(mesh);
            draw.hidden = shellDecal || shellAlphaOverlay;
            if (mesh.material != nullptr) {
                draw.baseColor = generatedSolidBase
                    ? std::array<float, 3>{1.0f, 1.0f, 1.0f}
                    : mesh.material->hasBaseColor
                    ? mesh.material->baseColor
                    : std::array<float, 3>{1.0f, 1.0f, 1.0f};
                draw.emissiveColor = mesh.material->emissiveColor;
                draw.opacity = mesh.material->opacity;
                draw.gloss = mesh.material->gloss;
                draw.metallic = mesh.material->hasMetallic
                    ? mesh.material->metallic : 0.0f;
                draw.uTiling = mesh.material->uTiling;
                draw.vTiling = mesh.material->vTiling;
                draw.detailUTiling = mesh.material->uTiling;
                draw.detailVTiling = mesh.material->vTiling;
                draw.normalIntensity = mesh.material->normalIntensity;
                draw.weaveNormalIntensity = mesh.material->weaveNormalIntensity;
                draw.clearCoatNormalUTiling = mesh.material->clearCoatNormalUTiling;
                draw.clearCoatNormalVTiling = mesh.material->clearCoatNormalVTiling;
                draw.weaveColorTintA = mesh.material->weaveColorTintA;
                draw.weaveColorTintB = mesh.material->weaveColorTintB;
                draw.sampler = mesh.material->sampler;
                draw.shaderFamily = modelShaderFamily(*mesh.material);
            }
            if (cleanFloorShell || cleanPaintedShell || cleanConcreteShell) {
                draw.rawMaterialUv = true;
                draw.uTiling = worldFrequencyTiling(
                    mesh, kCleanGarageSurfaceFrequency, 1.0f, 0.25f, 128.0f);
                draw.vTiling = draw.uTiling;
                draw.detailUTiling = draw.uTiling;
                draw.detailVTiling = draw.uTiling;
            }
            if (mesh.material == nullptr || !mesh.material->hasMetallic) {
                draw.gloss = fallback.gloss;
                draw.metallic = fallback.metallic;
            }
            if (draw.translucent) {
                draw.opacity = std::min(draw.opacity, 0.22f);
                draw.gloss = std::max(draw.gloss, 0.90f);
            }
            singleMesh.meshes.push_back(mesh);
            draw.geometry = std::move(singleMesh);
            scene.draws.push_back(std::move(draw));
        }
    };
    for (std::size_t index = 0; index < shellModelCount; ++index) {
        appendModel(index, ModelMat4{});
    }
    int visiblePropInstances = 0;
    for (const tokyo_house_layout::PropInstance &instance
         : tokyo_house_layout::kInstances) {
        const auto found = geometryByProp.constFind(instance.id);
        if (found == geometryByProp.cend()) {
            continue;
        }
        ModelMat4 placement;
        placement.m = instance.transform;
        appendModel(found.value(), placement);
        ++visiblePropInstances;
    }
    scene.materialStatus = QStringLiteral(
        "Authored general-scene maps bound for %1/%2 garage/House-8 draws "
        "(%3 normal, %4 RCSM/extra, %5 emissive); %6 procedural/base-colour "
        "draws, %7 visibly unresolved checker draws, and %8 clean shell surfaces")
        .arg(texturedDraws)
        .arg(scene.draws.size())
        .arg(normalDraws)
        .arg(surfaceDraws)
        .arg(emissiveDraws)
        .arg(materialFallbackDraws)
        .arg(unresolvedDraws)
        .arg(cleanShellDraws);
    scene.geometryStatus = QStringLiteral(
        "Six-piece garage_customiser enclosure whose X/Z bounds match the "
        "Default-House8.xml instance extents, plus %1 visible House 8 instances; "
        "5 gameplay locators are non-rendered")
        .arg(visiblePropInstances);

    if (floodLightArchive.isEmpty()) {
        fail(&scene, QStringLiteral("House 8 floodlight model"),
             QStringLiteral("catalog entry is missing"));
        return scene;
    }
    error.clear();
    std::vector<OriginalShaderPointLight> localLights;
    const QByteArray lightModels = readZipEntry(
        floodLightArchive,
        QStringLiteral("prp_el_garage_lights_flood_a.lightsmodels.lightdb"),
        &error);
    if (!decodeGarageLights(lightModels, &localLights, &error)) {
        fail(&scene, QStringLiteral("House 8 floodlight records"), error);
        return scene;
    }
    error.clear();
    const QByteArray lightPresets = readZipEntry(
        floodLightArchive,
        QStringLiteral("prp_el_garage_lights_flood_a.lightspresets.lightdb"),
        &error);
    if (!decodeGarageLightPresets(lightPresets, &localLights, &error)) {
        fail(&scene, QStringLiteral("House 8 floodlight presets"), error);
        return scene;
    }
    if (!roofLightArchive.isEmpty()) {
        error.clear();
        std::vector<OriginalShaderPointLight> roofLights;
        const QByteArray roofModels = readZipEntry(
            roofLightArchive,
            QStringLiteral("bld_gbl_grge_custom_02_roof_a.lightsmodels.lightdb"),
            &error);
        if (!decodeGarageLights(roofModels, &roofLights, &error)) {
            fail(&scene, QStringLiteral("garage roof light records"), error);
            return scene;
        }
        error.clear();
        const QByteArray roofPresets = readZipEntry(
            roofLightArchive,
            QStringLiteral("bld_gbl_grge_custom_02_roof_a.lightspresets.lightdb"),
            &error);
        if (!decodeGarageLightPresets(roofPresets, &roofLights, &error)) {
            fail(&scene, QStringLiteral("garage roof light presets"), error);
            return scene;
        }
        scene.authoredLights.insert(
            scene.authoredLights.end(), roofLights.cbegin(), roofLights.cend());
    }
    const int roofLightRecords = static_cast<int>(scene.authoredLights.size());
    int floodLightInstances = 0;
    for (const tokyo_house_layout::PropInstance &instance
         : tokyo_house_layout::kInstances) {
        if (instance.id != kFloodLightProp) {
            continue;
        }
        ModelMat4 placement;
        placement.m = instance.transform;
        for (const OriginalShaderPointLight &local : localLights) {
            OriginalShaderPointLight placed = local;
            placed.transform = matMul(local.transform, placement);
            scene.authoredLights.push_back(std::move(placed));
        }
        ++floodLightInstances;
    }
    const int enabledLights = static_cast<int>(std::count_if(
        scene.authoredLights.cbegin(), scene.authoredLights.cend(),
        [](const OriginalShaderPointLight &light) { return light.enabled; }));
    scene.lightingStatus = QStringLiteral(
        "%1 garage-roof light records plus %2 House 8 floodlight instances "
        "produce %3 light records; %4 active presets include authored colour, "
        "range, intensity, and cone values")
        .arg(roofLightRecords)
        .arg(floodLightInstances)
        .arg(scene.authoredLights.size())
        .arg(enabledLights);

    const QString shaderArchive = QDir(media).filePath(QStringLiteral("_library/Homespace.zip"));
    error.clear();
    if (!loadProgram(shaderArchive, kDefaultProgram, &scene.defaultProgram, &error)) {
        fail(&scene, QStringLiteral("default homespace DXIL"), error);
        return scene;
    }
    error.clear();
    if (!loadProgram(shaderArchive, kFloorProgram, &scene.floorProgram, &error)) {
        fail(&scene, QStringLiteral("floor homespace DXIL"), error);
        return scene;
    }

    const QString textureArchive = QDir(media).filePath(QStringLiteral("_library/Textures.zip"));
    for (std::size_t index = 0; index < kMaterialTextures.size(); ++index) {
        const TextureResource &resource = kMaterialTextures[index];
        error.clear();
        const QByteArray bytes = readZipEntry(
            textureArchive, QString::fromLatin1(resource.entry), &error);
        if (bytes.isEmpty()) {
            fail(&scene, QStringLiteral("%1 fallback").arg(
                QString::fromLatin1(resource.semantic)), error);
            return scene;
        }
        SwatchImage image;
        std::vector<SwatchImage> mips;
        if (!decodeTextureWithAuthoredMips(bytes, &image, &mips, &error)) {
            fail(&scene, QStringLiteral("%1 fallback").arg(
                QString::fromLatin1(resource.semantic)), error);
            return scene;
        }
        scene.materialTextures[index] = {
            QString::fromLatin1(resource.semantic),
            QString::fromLatin1(resource.entry), std::move(image), std::move(mips)};
    }

    scene.environment = loadGarageEnvironmentResources(gameFolder);
    if (!scene.environment.valid() || !scene.environment.panorama.valid()) {
        fail(&scene, QStringLiteral("StagedSpaces_Garage_09 / Forte_Garage_01"),
             !scene.environment.error.isEmpty()
                 ? scene.environment.error : scene.environment.panoramaError);
        return scene;
    }
    QString colorLutError;
    const std::optional<GarageColorLut> colorLut =
        loadGarageColorLut(gameFolder, &colorLutError);
    if (!colorLut.has_value()) {
        fail(&scene, QStringLiteral("Homespace colour grade"), colorLutError);
        return scene;
    }
    scene.colorLut = *colorLut;

    scene.error.clear();
    return scene;
}

bool appendOriginalShaderGarageCar(
    OriginalShaderGarageScene *scene, CarModel car,
    const std::array<float, 3> &paintColor, const SwatchImage &livery,
    const LiveryMaskSet *liveryMasks,
    const std::array<std::array<float, 4>, kLiverySideCount> *paintRegions,
    const LiveryPaintState *paintState,
    const ManufacturerColorPalette *manufacturerColors,
    const PaintFinishLibrary *paintFinishes,
    QString *error) {
    if (error != nullptr) {
        error->clear();
    }
    if (scene == nullptr || car.meshes.empty()) {
        if (error != nullptr) {
            *error = QStringLiteral("a decoded car model is required");
        }
        return false;
    }

    QHash<const ModelMaterialTexture *,
          std::shared_ptr<const OriginalShaderMaterialTexture>> decodedTextures;
    QHash<QString, std::shared_ptr<const OriginalShaderMaterialTexture>> finishTextures;
    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> solidTextures;
    const ModelVec3 boundsMin = car.boundsMin;
    const ModelVec3 boundsMax = car.boundsMax;
    const QString source = car.sourcePath;
    // House 8's car locator is on the finished floor, while car model origins
    // are not guaranteed to be at tire contact.  Seat the highest-detail wheel
    // and tire geometry on that plane instead of burying it below the floor.
    float contactY = std::numeric_limits<float>::max();
    const std::vector<char> contactLod = highestLodFlags(car.meshes);
    for (std::size_t meshIndex = 0; meshIndex < car.meshes.size(); ++meshIndex) {
        const CarMesh &mesh = car.meshes[meshIndex];
        if (!contactLod[meshIndex]) {
            continue;
        }
        QString identity = mesh.name + QLatin1Char('|') + mesh.sourceModelPath;
        identity.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (!identity.contains(QStringLiteral("wheel"), Qt::CaseInsensitive)
            && !identity.contains(QStringLiteral("tire"), Qt::CaseInsensitive)
            && !identity.contains(QStringLiteral("tyre"), Qt::CaseInsensitive)) {
            continue;
        }
        for (const ModelVec3 &position : mesh.positions) {
            contactY = std::min(
                contactY, mesh.boneTransform.transformPoint(position).y);
        }
    }
    if (!std::isfinite(contactY)) {
        contactY = boundsMin.y;
    }
    ModelMat4 groundedCarPlacement = scene->carPlacement;
    groundedCarPlacement.m[13] -= contactY;
    const std::size_t firstDraw = scene->draws.size();
    int opaqueDraws = 0;
    int liveryDraws = 0;
    int nativeBaseDraws = 0;
    int solidBaseDraws = 0;
    int unresolvedCheckerDraws = 0;
    int normalDraws = 0;
    int surfaceDraws = 0;
    int emissiveDraws = 0;
    int translucentDraws = 0;
    int excludedLowerLod = 0;
    int excludedInteriorShell = 0;
    int excludedFactoryLiveryStickers = 0;
    std::shared_ptr<const OriginalShaderMaterialTexture> liveryTexture;
    if (livery.valid()) {
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = QStringLiteral("DiffuseTexture");
        texture->sourceEntry = QStringLiteral("car://composited-livery");
        texture->image = livery;
        liveryTexture = std::move(texture);
    } else if (liveryMasks != nullptr && liveryMasks->valid()
               && paintRegions != nullptr) {
        // Keep the projection path alive for an empty imported livery so the
        // interactive DX12 viewport can replace this texture without rebuilding
        // the entire garage and car scene.
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = QStringLiteral("DiffuseTexture");
        texture->sourceEntry = QStringLiteral("car://composited-livery");
        texture->image.width = 1;
        texture->image.height = 1;
        texture->image.rgba = {0, 0, 0, 0};
        liveryTexture = std::move(texture);
    }
    if (liveryTexture != nullptr && liveryMasks != nullptr
        && liveryMasks->valid()) {
        constexpr std::array<std::array<float, 3>, kLiverySideCount> kFacing = {{
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
            {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
            {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
        }};
        scene->liveryMapping.sideCount = kLiverySideCount;
        scene->liveryMapping.facing = kFacing;
        for (int sideIndex = 0; sideIndex < kLiverySideCount; ++sideIndex) {
            const LiverySide &side = liveryMasks->sides[sideIndex];
            scene->liveryMapping.sourceRegions[sideIndex] = {
                side.left, side.right, side.top, side.bottom};
            scene->liveryMapping.paintRegions[sideIndex] = paintRegions != nullptr
                ? (*paintRegions)[sideIndex]
                : std::array<float, 4>{
                      (side.left + kLiveryCanvasHalfWidth)
                          / (2.0f * kLiveryCanvasHalfWidth),
                      (side.right + kLiveryCanvasHalfWidth)
                          / (2.0f * kLiveryCanvasHalfWidth),
                      (kLiveryCanvasHalfHeight - side.top)
                          / (2.0f * kLiveryCanvasHalfHeight),
                      (kLiveryCanvasHalfHeight - side.bottom)
                          / (2.0f * kLiveryCanvasHalfHeight)};
            const SwatchMask &mask = side.mask;
            if (!mask.valid()) {
                continue;
            }
            OriginalShaderMaterialTexture &mappedMask =
                scene->liveryMapping.masks[sideIndex];
            mappedMask.semantic = QStringLiteral("LiveryMask%1").arg(sideIndex);
            mappedMask.sourceEntry = QStringLiteral("car://livery-mask/%1")
                .arg(sideIndex);
            mappedMask.image.width = mask.width;
            mappedMask.image.height = mask.height;
            mappedMask.image.rgba.resize(mask.coverage.size() * 4);
            for (std::size_t pixel = 0; pixel < mask.coverage.size(); ++pixel) {
                const std::uint8_t coverage = mask.coverage[pixel];
                mappedMask.image.rgba[pixel * 4 + 0] = coverage;
                mappedMask.image.rgba[pixel * 4 + 1] = coverage;
                mappedMask.image.rgba[pixel * 4 + 2] = coverage;
                mappedMask.image.rgba[pixel * 4 + 3] = 255;
            }
        }
    }
    const auto copyNativeTexture = [&](const std::shared_ptr<const ModelMaterialTexture> &native,
                                       const QString &semantic)
        -> std::shared_ptr<const OriginalShaderMaterialTexture> {
        if (native == nullptr || !native->image.valid()) {
            return {};
        }
        const auto found = decodedTextures.constFind(native.get());
        if (found != decodedTextures.cend()) {
            return found.value();
        }
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = semantic;
        texture->sourceEntry = native->path.isEmpty()
            ? QStringLiteral("car://decoded-material-map") : native->path;
        texture->image = native->image;
        texture->authoredMips = native->authoredMips;
        decodedTextures.insert(native.get(), texture);
        return texture;
    };
    const auto copyFinishTexture = [&](const SwatchImage &image, const QString &semantic,
                                       const QString &sourceEntry)
        -> std::shared_ptr<const OriginalShaderMaterialTexture> {
        if (!image.valid()) {
            return {};
        }
        const QString key = semantic + QLatin1Char('|') + sourceEntry;
        const auto found = finishTextures.constFind(key);
        if (found != finishTextures.cend()) {
            return found.value();
        }
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = semantic;
        texture->sourceEntry = sourceEntry;
        texture->image = image;
        finishTextures.insert(key, texture);
        return texture;
    };
    bool liveryCustomPainted = false;
    if (paintState != nullptr) {
        liveryCustomPainted = std::any_of(
            paintState->materials.cbegin(), paintState->materials.cend(),
            [](const LiveryPaintMaterial &paint) { return paint.primary.enabled; });
    }
    const std::vector<char> keepLod = highestLodFlags(car.meshes);
    for (std::size_t meshIndex = 0; meshIndex < car.meshes.size(); ++meshIndex) {
        CarMesh &mesh = car.meshes[meshIndex];
        if (!keepLod[meshIndex]) {
            ++excludedLowerLod;
            continue;
        }
        if (isInteriorWindowShell(mesh.name)
            || mesh.materialName.startsWith(
                QStringLiteral("InteriorLOD"), Qt::CaseInsensitive)) {
            ++excludedInteriorShell;
            continue;
        }
        const QString materialPath = mesh.material != nullptr
            ? mesh.material->resourcePath
            : QString();
        QString normalizedMaterialPath = materialPath;
        normalizedMaterialPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        // Some race cars use raised polygons shaped like their factory sponsor
        // graphics. Repainting those polygons with a custom atlas preserves the
        // old logo silhouettes even though the body panel below is correct.
        // The editor livery is authoritative, including an intentionally empty
        // one, so this factory sticker layer is never part of the preview.
        // Genuine badge/symbol materials stay.
        if (normalizedMaterialPath.contains(
                QStringLiteral("/carpaint_default/livery_sticker.materialbin"),
                Qt::CaseInsensitive)) {
            ++excludedFactoryLiveryStickers;
            continue;
        }
        const bool glassSurface = isGlassSurface(mesh);

        const QString lowerMaterialName = mesh.materialName.toLower();
        const bool bodyPaint = lowerMaterialName.startsWith(QStringLiteral("carpaint"))
            || lowerMaterialName.startsWith(QStringLiteral("car_paint"));
        const bool paintSurface = bodyPaint;
        const bool usesLivery = liveryTexture != nullptr
            && bodyPaint && mesh.liveryUvChannel == 3;
        const std::optional<CarMaterialFallback> fallback =
            carMaterialFallback(mesh);
        const LiveryPaintMaterial *paint = paintState != nullptr
            ? paintState->find(mesh.paintMaterialHash) : nullptr;
        const ManufacturerColor *manufacturerColor =
            paint != nullptr && manufacturerColors != nullptr && !liveryCustomPainted
            ? manufacturerColors->find(paint->manufacturerSelector) : nullptr;
        const PaintFinishRender *paintFinish =
            paint != nullptr && paintFinishes != nullptr
            ? paintFinishes->find(static_cast<int>(paint->finish)) : nullptr;
        std::array<float, 3> resolvedPaintColor = paintColor;
        if (manufacturerColor != nullptr) {
            resolvedPaintColor = manufacturerColor->primary;
        }
        if (paint != nullptr && paint->primary.enabled) {
            for (int channel = 0; channel < 3; ++channel) {
                const float srgb = paint->primary.bgra[2 - channel] / 255.0f;
                resolvedPaintColor[channel] = std::pow(srgb, 2.2f);
            }
        }
        bool generatedSolidBase = false;
        const auto finishPattern = bodyPaint && paintFinish != nullptr
                && paintFinish->selfColored
            ? copyFinishTexture(
                  paintFinish->patternImage, QStringLiteral("DiffuseTexture"),
                  QStringLiteral("car://paint-finish/%1/pattern").arg(paint->finish))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        std::shared_ptr<const OriginalShaderMaterialTexture> diffuse =
            usesLivery ? liveryTexture : nullptr;
        const std::shared_ptr<const ModelMaterialTexture> nativeDiffuse =
            mesh.material != nullptr ? mesh.material->diffuseTexture : nullptr;
        if (diffuse != nullptr) {
            ++liveryDraws;
        } else if (finishPattern != nullptr) {
            diffuse = finishPattern;
            ++nativeBaseDraws;
        } else if (nativeDiffuse != nullptr && nativeDiffuse->image.valid()) {
            const auto found = decodedTextures.constFind(nativeDiffuse.get());
            if (found != decodedTextures.cend()) {
                diffuse = found.value();
            } else {
                diffuse = copyNativeTexture(
                    nativeDiffuse, QStringLiteral("DiffuseTexture"));
            }
            ++nativeBaseDraws;
            if (nativeDiffuse->path.contains(
                    QStringLiteral("MissingTexture.png"), Qt::CaseInsensitive)) {
                ++unresolvedCheckerDraws;
            }
        } else {
            std::array<float, 3> color = bodyPaint
                ? resolvedPaintColor : std::array<float, 3>{0.55f, 0.55f, 0.55f};
            const bool hasAuthoredColor = !bodyPaint && mesh.material != nullptr
                && mesh.material->hasBaseColor;
            if (hasAuthoredColor) {
                color = mesh.material->baseColor;
            }
            if (!bodyPaint && fallback != std::nullopt) {
                const float brightestColor = std::max({color[0], color[1], color[2]});
                if (!hasAuthoredColor || brightestColor < fallback->minimumColor) {
                    color = fallback->color;
                }
            }
            const quint32 key = solidColorKey(color);
            const auto found = solidTextures.constFind(key);
            if (found != solidTextures.cend()) {
                diffuse = found.value();
            } else {
                auto texture = std::make_shared<OriginalShaderMaterialTexture>();
                texture->semantic = QStringLiteral("DiffuseTexture");
                texture->sourceEntry = QStringLiteral("car://solid/%1")
                    .arg(key, 8, 16, QLatin1Char('0'));
                texture->image = solidColorImage(color);
                diffuse = texture;
                solidTextures.insert(key, std::move(texture));
            }
            generatedSolidBase = true;
            ++solidBaseDraws;
        }

        auto normal = bodyPaint && paintFinish != nullptr
            ? copyFinishTexture(
                  paintFinish->hasDetailNormal()
                      ? paintFinish->detailNormalImage
                      : (paintFinish->hasNormalMap00()
                            ? paintFinish->normalMap00Image
                            : (paintFinish->hasNormalMap0()
                                  ? paintFinish->normalMap0Image
                                  : paintFinish->orangePeelNormalImage)),
                  QStringLiteral("NormalTexture"),
                  QStringLiteral("car://paint-finish/%1/normal").arg(paint->finish))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        if (normal == nullptr && mesh.material != nullptr) {
            normal =
                copyNativeTexture(
                    mesh.material->normalTexture != nullptr
                        ? mesh.material->normalTexture
                        : (mesh.material->paintNormalMap00Texture != nullptr
                              ? mesh.material->paintNormalMap00Texture
                              : mesh.material->paintNormalMap0Texture),
                    QStringLiteral("NormalTexture"));
        }
        auto surface = bodyPaint && paintFinish != nullptr
            ? copyFinishTexture(
                  paintFinish->roughMetalAoImage, QStringLiteral("SurfaceTexture"),
                  QStringLiteral("car://paint-finish/%1/surface").arg(paint->finish))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        if (surface == nullptr && mesh.material != nullptr) {
            surface = copyNativeTexture(
                mesh.material->surfaceTexture, QStringLiteral("SurfaceTexture"));
        }
        auto emissive = mesh.material != nullptr
            ? copyNativeTexture(
                  mesh.material->emissiveTexture, QStringLiteral("EmissiveTexture"))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        auto alpha = mesh.material != nullptr
            ? copyNativeTexture(
                  mesh.material->alphaTexture, QStringLiteral("AlphaTexture"))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        auto weaveMask = mesh.material != nullptr
            ? copyNativeTexture(
                  mesh.material->weaveMaskTexture,
                  QStringLiteral("WeaveMaskTexture"))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        auto weaveNormal = mesh.material != nullptr
            ? copyNativeTexture(
                  mesh.material->weaveNormalTexture,
                  QStringLiteral("WeaveNormalTexture"))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        auto clearCoatNormal = mesh.material != nullptr
            ? copyNativeTexture(
                  mesh.material->clearCoatNormalTexture,
                  QStringLiteral("ClearCoatNormalTexture"))
            : std::shared_ptr<const OriginalShaderMaterialTexture>{};
        normalDraws += normal != nullptr ? 1 : 0;
        surfaceDraws += surface != nullptr ? 1 : 0;
        emissiveDraws += emissive != nullptr ? 1 : 0;

        CarModel singleMesh;
        singleMesh.sourcePath = source;
        singleMesh.boundsMin = boundsMin;
        singleMesh.boundsMax = boundsMax;
        const QString drawName = QStringLiteral("car/%1").arg(mesh.materialName);
        const int diffuseUvChannel = usesLivery ? mesh.liveryUvChannel : 0;
        OriginalShaderGarageDraw draw;
        draw.name = drawName;
        draw.source = source;
        draw.family = OriginalShaderSurfaceFamily::Default;
        draw.placement = groundedCarPlacement;
        draw.diffuseTexture = std::move(diffuse);
        draw.alphaTexture = std::move(alpha);
        draw.normalTexture = std::move(normal);
        draw.weaveMaskTexture = std::move(weaveMask);
        draw.weaveNormalTexture = std::move(weaveNormal);
        draw.clearCoatNormalTexture = std::move(clearCoatNormal);
        draw.surfaceTexture = std::move(surface);
        draw.emissiveTexture = std::move(emissive);
        draw.diffuseUvChannel = diffuseUvChannel;
        if (usesCarbonFiberShader(mesh)
            && mesh.uvChannels.size() > 1
            && mesh.uvChannels[1].size() == mesh.positions.size()) {
            draw.materialUvChannel = 1;
            draw.materialUvRotationDegrees = mesh.material->uvOrientationDegrees;
        }
        draw.rawMaterialUv = false;
        draw.baseColor = generatedSolidBase
            ? std::array<float, 3>{1.0f, 1.0f, 1.0f}
            : (paintSurface ? resolvedPaintColor
                            : std::array<float, 3>{1.0f, 1.0f, 1.0f});
        if (mesh.material != nullptr) {
            if (!usesLivery && !generatedSolidBase && !paintSurface
                && mesh.material->hasBaseColor) {
                draw.baseColor = mesh.material->baseColor;
            }
            draw.emissiveColor = mesh.material->emissiveColor;
            draw.opacity = mesh.material->opacity;
            draw.gloss = mesh.material->gloss;
            draw.metallic = mesh.material->hasMetallic
                ? mesh.material->metallic : 0.0f;
            draw.uTiling = usesLivery ? 1.0f : mesh.material->uTiling;
            draw.vTiling = usesLivery ? 1.0f : mesh.material->vTiling;
            draw.detailUTiling = mesh.material->uTiling;
            draw.detailVTiling = mesh.material->vTiling;
            draw.normalIntensity = mesh.material->normalIntensity;
            draw.weaveNormalIntensity = mesh.material->weaveNormalIntensity;
            draw.clearCoatNormalUTiling = mesh.material->clearCoatNormalUTiling;
            draw.clearCoatNormalVTiling = mesh.material->clearCoatNormalVTiling;
            draw.weaveColorTintA = mesh.material->weaveColorTintA;
            draw.weaveColorTintB = mesh.material->weaveColorTintB;
            draw.sampler = mesh.material->sampler;
            draw.shaderFamily = modelShaderFamily(*mesh.material);
        }
        if (glassSurface && draw.opacity >= 0.995f) {
            draw.opacity = isWindowGlassMaterial(mesh) ? 0.42f : 0.20f;
        }
        draw.translucent = glassSurface || draw.opacity < 0.995f
            || draw.alphaTexture != nullptr;
        translucentDraws += draw.translucent ? 1 : 0;
        if (!bodyPaint && fallback != std::nullopt
            && (mesh.material == nullptr || !mesh.material->resolvedFromLibrary)) {
            draw.gloss = fallback->gloss;
            draw.metallic = fallback->metallic;
        }
        if (bodyPaint && paintFinish != nullptr && paintFinish->valid) {
            draw.gloss = paintFinish->gloss;
            draw.metallic = paintFinish->metallic;
        } else if (bodyPaint && manufacturerColor != nullptr
                   && manufacturerColor->material != nullptr) {
            draw.gloss = manufacturerColor->material->gloss;
            if (manufacturerColor->material->hasMetallic) {
                draw.metallic = manufacturerColor->material->metallic;
            }
        }
        if (bodyPaint) {
            const bool bareMetal = paintFinish != nullptr && paintFinish->valid
                && paintFinish->category == PaintFinishCategory::Metal;
            draw.clearCoatCoverage = std::clamp(
                (draw.gloss - kClearCoatGlossFloor) / kClearCoatGlossRange,
                0.0f, 1.0f);
            if (bareMetal) {
                draw.clearCoatCoverage *= kClearCoatMetalCoverage;
            }
            draw.clearCoatRoughness = std::clamp(
                1.0f - draw.gloss,
                kClearCoatMinRoughness, kClearCoatMaxRoughness);
            const AutomotivePaintParameters *automotivePaint = nullptr;
            if (paintFinish != nullptr && paintFinish->valid) {
                automotivePaint = &paintFinish->automotivePaint;
            } else if (manufacturerColor != nullptr
                       && manufacturerColor->material != nullptr) {
                automotivePaint = &manufacturerColor->material->automotivePaint;
            } else if (mesh.material != nullptr) {
                automotivePaint = &mesh.material->automotivePaint;
            }
            if (automotivePaint != nullptr) {
                if (automotivePaint->hasClearCoatCoverage) {
                    draw.clearCoatCoverage = std::clamp(
                        automotivePaint->clearCoatCoverage, 0.0f, 1.0f);
                } else if (automotivePaint->hasClearCoatRoughness) {
                    draw.clearCoatCoverage = bareMetal
                        ? kClearCoatMetalCoverage : 1.0f;
                }
                if (automotivePaint->hasClearCoatRoughness) {
                    draw.clearCoatRoughness = std::clamp(
                        automotivePaint->clearCoatRoughness,
                        kClearCoatMinRoughness, 1.0f);
                }
                if (automotivePaint->hasClearCoatTint) {
                    std::copy_n(
                        automotivePaint->clearCoatTint.cbegin(), 3,
                        draw.clearCoatTint.begin());
                }
                if (automotivePaint->hasClearCoatOnLivery) {
                    draw.clearCoatOnLivery = automotivePaint->clearCoatOnLivery;
                }
            }
        }
        if (usesLivery) {
            constexpr quint32 kAllBodySides = 0x1fu;
            const QString lowerMeshName = mesh.name.toLower();
            draw.liveryBaseTexture = true;
            if (!lowerMeshName.contains(QStringLiteral("mirror"))
                && (lowerMeshName.contains(QStringLiteral("spoiler"))
                    || lowerMeshName.contains(QStringLiteral("wing")))) {
                draw.liveryAllowedSides = 1u << 5;
            } else if (lowerMeshName.startsWith(QStringLiteral("trunk"))) {
                draw.liveryAllowedSides = (1u << 1) | (1u << 2);
            } else {
                switch (mesh.carPartType) {
                case 34: // front bumper
                    draw.liveryAllowedSides = (1u << 0) | (1u << 3) | (1u << 4);
                    break;
                case 35: // rear bumper
                    draw.liveryAllowedSides = (1u << 1) | (1u << 3) | (1u << 4);
                    break;
                case 36: // hood
                    draw.liveryAllowedSides = 1u << 2;
                    break;
                case 37: // side skirts
                    draw.liveryAllowedSides = (1u << 3) | (1u << 4);
                    break;
                default:
                    draw.liveryAllowedSides = kAllBodySides;
                    break;
                }
            }
        }
        singleMesh.meshes.push_back(std::move(mesh));
        draw.geometry = std::move(singleMesh);
        scene->draws.push_back(std::move(draw));
        ++opaqueDraws;
    }
    if (scene->draws.size() == firstDraw) {
        if (error != nullptr) {
            *error = QStringLiteral("the car contains no supported opaque meshes");
        }
        return false;
    }
    scene->carStatus = QStringLiteral(
        "DX12 car added with %1 material draws: %2 livery, %3 native base-colour, "
        "%4 solid material-colour, %5 visibly unresolved checker, %6 normal, "
        "%7 surface, and %8 emissive maps; %9 translucent/glass draws included; "
        "%10 lower-LOD, %11 coarse-interior, and %12 factory-livery sticker "
        "draws excluded")
        .arg(opaqueDraws)
        .arg(liveryDraws)
        .arg(nativeBaseDraws)
        .arg(solidBaseDraws)
        .arg(unresolvedCheckerDraws)
        .arg(normalDraws)
        .arg(surfaceDraws)
        .arg(emissiveDraws)
        .arg(translucentDraws)
        .arg(excludedLowerLod)
        .arg(excludedInteriorShell)
        .arg(excludedFactoryLiveryStickers);
    return true;
}

} // namespace fh6
