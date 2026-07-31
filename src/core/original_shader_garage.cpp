#include "original_shader_garage.h"

#include "game_paths.h"
#include "binary_io.h"
#include "model_bundle.h"
#include "model_material.h"
#include "pgzp_extract.h"
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
#include <utility>

namespace fh6 {

namespace {

constexpr auto kTokyoHouseLod0 =
    "bld_cty_dcks_playerhome_01_a_cluster004.i.modelbin";
constexpr auto kTokyoHousePlinthLod0 =
    "plnth_section112_plaza_01_cluster004.i.modelbin";

struct GarageShellResource {
    const char *directory;
    const char *archive;
    OriginalShaderSurfaceFamily family;
};

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
        SwatchImage image = decodeSwatchImage(
            payloads.value(iterator->archiveLeaf.toLower()), &decodeError);
        if (!image.valid()) {
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

QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>>
loadGeneralSceneDiffuseTextures(
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
    const QHash<quint32, ManifestTexture> resolved =
        resolveGeneralSceneTextures(manifest, targetHashes, error);
    QStringList requested;
    QHash<QString, quint32> hashByEntry;
    for (auto iterator = resolved.cbegin(); iterator != resolved.cend(); ++iterator) {
        if (!isBaseColourTexture(iterator->canonicalPath)
            && !isEmissiveTexture(iterator->canonicalPath)) {
            continue;
        }
        requested.push_back(iterator->archiveLeaf);
        hashByEntry.insert(iterator->archiveLeaf.toLower(), iterator.key());
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
    if (payloads.size() != requested.size()) {
        return {};
    }
    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> textures;
    for (auto iterator = payloads.cbegin(); iterator != payloads.cend(); ++iterator) {
        const quint32 hash = hashByEntry.value(iterator.key());
        const QString sourceEntry = resolved.value(hash).canonicalPath;
        QString decodeError;
        SwatchImage image = decodeSwatchImage(iterator.value(), &decodeError);
        if (!image.valid()) {
            // Some authored emissive light maps use a non-colour image encoding
            // that the current swatch decoder cannot upload yet. The same mesh
            // still has its authored base-colour dependency.
            if (isEmissiveTexture(sourceEntry)) {
                continue;
            }
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2").arg(iterator.key(), decodeError);
            }
            return {};
        }
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = QStringLiteral("DiffuseTexture");
        texture->sourceEntry = sourceEntry;
        texture->image = std::move(image);
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
        for (const ModelMaterialParameter &parameter : mesh.material->parameters) {
        if (parameter.type != ModelMaterialParameterType::Texture2D) {
            continue;
        }
        const auto found = textures.constFind(parameter.texturePathHash);
        if (found != textures.cend()
            && (!emissiveMaterial || pass != 0
                || isEmissiveTexture(found.value()->sourceEntry))) {
            return found.value();
        }
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

SwatchTexture neutralDiffuseCubemap() {
    SwatchTexture texture;
    texture.width = 1;
    texture.height = 1;
    texture.arraySize = 1;
    texture.platform = 0;
    texture.sliceCount = 6;
    texture.mipCount = 1;
    constexpr std::array<char, 8> pixel = {
        0x00, 0x34, 0x00, 0x34, 0x00, 0x34, 0x00, 0x3c};
    for (int face = 0; face < texture.sliceCount; ++face) {
        const quint32 offset = static_cast<quint32>(texture.payload.size());
        texture.payload.append(pixel.data(), pixel.size());
        SwatchTextureSlice slice;
        slice.encoding = SwatchEncoding::R16G16B16A16Float;
        slice.mipLevels.push_back({offset, 8, 0xFFFFFFFFu});
        texture.slices.push_back(std::move(slice));
    }
    return texture;
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

quint32 solidColorKey(const std::array<float, 3> &color) {
    const SwatchImage image = solidColorImage(color);
    return static_cast<quint32>(image.rgba[0])
        | (static_cast<quint32>(image.rgba[1]) << 8)
        | (static_cast<quint32>(image.rgba[2]) << 16)
        | 0xff000000u;
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
        && environment.diffuseCubemap.valid();
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

OriginalShaderGarageScene loadOriginalShaderGarageScene(const QString &gameFolder) {
    OriginalShaderGarageScene scene;
    scene.name = QStringLiteral("Tokyo House");
    // First fixed prop in DefaultGarageLayouts/Default-House8.xml. The fixed
    // main-car locator is common to every authored default garage layout.
    scene.carPlacement.m = {
        0.999851f, 0.0f, 0.017252f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        -0.017252f, 0.0f, 0.999851f, 0.0f,
        -2.291748f, 0.023865f, 0.546875f, 1.0f};
    scene.geometryStatus = QStringLiteral(
        "Exact six-piece garage_customiser shell used by the House 8 default "
        "garage layout; opaque LOD0 floor, roof, and four walls loaded");
    scene.materialStatus = QStringLiteral("Garage customiser material textures are loading");
    scene.lighting = {
        {-0.197420f, -0.302618f, 0.932442f},
        {0.900001f, 0.900000f, 0.750001f},
        {0.142109f, 0.175171f, 0.197850f},
        QStringLiteral("homespace compatibility light constants")};
    scene.lightingStatus = QStringLiteral(
        "Garage customiser roof lighting database is loading");
    scene.glassStatus = QStringLiteral(
        "Tokyo House glass excluded until exact transparent shader and texture bindings are active");
    scene.carStatus = QStringLiteral(
        "House 8 main-car locator loaded from Default-House8.xml");

    const QString media = gameMediaDir(gameFolder);
    if (media.isEmpty()) {
        scene.error = QStringLiteral("game folder is not configured");
        return scene;
    }

    const QString stripped = QDir(media).filePath(QStringLiteral("Stripped/gs"));
    const QString shellDirectory = QDir(stripped).filePath(QStringLiteral(
        "tracks/brio/scene/models/buildings/global/garage_customiser"));
    const QString manifest =
        QDir(stripped).filePath(QStringLiteral("generalsceneassets.manifest"));
    const QString swatchArchive =
        QDir(stripped).filePath(QStringLiteral("swatchbins.zip"));
    QString error;
    std::vector<CarModel> geometry;
    std::vector<QString> sources;
    std::vector<OriginalShaderSurfaceFamily> families;
    geometry.reserve(kGarageShellResources.size());
    sources.reserve(kGarageShellResources.size());
    families.reserve(kGarageShellResources.size());
    for (const GarageShellResource &resource : kGarageShellResources) {
        const QString archive = QDir(shellDirectory).filePath(QStringLiteral("%1/%2")
            .arg(QString::fromLatin1(resource.directory),
                 QString::fromLatin1(resource.archive)));
        const QByteArray bytes = readZipEntry(
            archive, QStringLiteral("000.i.modelbin"), &error);
        const QString source = QStringLiteral("%1!/000.i.modelbin").arg(archive);
        CarModel model = decodeModelBytes(bytes, source, &error);
        if (model.meshes.empty()) {
            fail(&scene, QStringLiteral("garage customiser shell geometry"), error);
            return scene;
        }
        model.meshes.erase(
            std::remove_if(
                model.meshes.begin(), model.meshes.end(),
                [](const CarMesh &mesh) {
                    return mesh.materialName.contains(
                        QStringLiteral("glass"), Qt::CaseInsensitive);
                }),
            model.meshes.end());
        sources.push_back(source);
        geometry.push_back(std::move(model));
        families.push_back(resource.family);
    }
    error.clear();
    const auto authoredDiffuse = loadGeneralSceneDiffuseTextures(
        swatchArchive, manifest, geometry, &error);
    if (authoredDiffuse.isEmpty()) {
        fail(&scene, QStringLiteral("garage customiser material textures"), error);
        return scene;
    }
    std::size_t meshCount = 0;
    for (const CarModel &model : geometry) {
        meshCount += model.meshes.size();
    }
    scene.draws.reserve(meshCount);
    const std::vector<ModelMat4> placements(geometry.size());
    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>>
        solidTextures;
    int texturedDraws = 0;
    int materialFallbackDraws = 0;
    for (std::size_t modelIndex = 0; modelIndex < geometry.size(); ++modelIndex) {
        CarModel &model = geometry[modelIndex];
        const ModelVec3 boundsMin = model.boundsMin;
        const ModelVec3 boundsMax = model.boundsMax;
        const QString modelSource = model.sourcePath;
        for (CarMesh &mesh : model.meshes) {
            CarModel singleMesh;
            singleMesh.sourcePath = modelSource;
            singleMesh.boundsMin = boundsMin;
            singleMesh.boundsMax = boundsMax;
            const QString drawName = mesh.materialName;
            auto diffuseTexture = meshDiffuseTexture(mesh, authoredDiffuse);
            if (diffuseTexture != nullptr) {
                ++texturedDraws;
            } else {
                std::array<float, 3> color = {0.55f, 0.55f, 0.55f};
                if (mesh.material != nullptr && mesh.material->hasBaseColor) {
                    color = mesh.material->baseColor;
                }
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
                ++materialFallbackDraws;
            }
            singleMesh.meshes.push_back(std::move(mesh));
            scene.draws.push_back({
                drawName, sources[modelIndex],
                families[modelIndex],
                std::move(singleMesh), placements[modelIndex],
                std::move(diffuseTexture)});
        }
    }
    scene.materialStatus = QStringLiteral(
        "Authored general-scene base-colour/emissive swatches bound for %1/%2 garage "
        "customiser draws; %3 draws use their decoded material base colour; "
        "normal/RCSM slots still use neutral shader defaults")
        .arg(texturedDraws)
        .arg(scene.draws.size())
        .arg(materialFallbackDraws);

    const QString roofArchive = QDir(shellDirectory).filePath(QStringLiteral(
        "bld_gbl_grge_custom_02_roof_a/bld_gbl_grge_custom_02_roof_a.i.zip"));
    error.clear();
    const QByteArray lightModels = readZipEntry(
        roofArchive,
        QStringLiteral("bld_gbl_grge_custom_02_roof_a.lightsmodels.lightdb"),
        &error);
    if (!decodeGarageLights(lightModels, &scene.authoredLights, &error)) {
        fail(&scene, QStringLiteral("garage customiser artificial lights"), error);
        return scene;
    }
    scene.lightingStatus = QStringLiteral(
        "%1 exact artificial-light transforms and preset hash 0x%2 loaded from "
        "the garage customiser roof lightdb; compatibility shader constants "
        "remain active while the preset-field binding is decoded")
        .arg(scene.authoredLights.size())
        .arg(scene.authoredLights.front().presetHash, 8, 16, QLatin1Char('0'));

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
        SwatchImage image = decodeSwatchImage(bytes, &error);
        if (!image.valid()) {
            fail(&scene, QStringLiteral("%1 fallback").arg(
                QString::fromLatin1(resource.semantic)), error);
            return scene;
        }
        scene.materialTextures[index] = {
            QString::fromLatin1(resource.semantic),
            QString::fromLatin1(resource.entry), std::move(image)};
    }

    scene.environment.diffuseCubemap = neutralDiffuseCubemap();
    scene.environment.error.clear();

    scene.error.clear();
    return scene;
}

bool appendOriginalShaderGarageCar(
    OriginalShaderGarageScene *scene, CarModel car,
    const std::array<float, 3> &paintColor, const SwatchImage &livery,
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
    QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> solidTextures;
    const ModelVec3 boundsMin = car.boundsMin;
    const ModelVec3 boundsMax = car.boundsMax;
    const QString source = car.sourcePath;
    const std::size_t firstDraw = scene->draws.size();
    int texturedDraws = 0;
    int liveryDraws = 0;
    int excludedGlass = 0;
    std::shared_ptr<const OriginalShaderMaterialTexture> liveryTexture;
    if (livery.valid()) {
        auto texture = std::make_shared<OriginalShaderMaterialTexture>();
        texture->semantic = QStringLiteral("DiffuseTexture");
        texture->sourceEntry = QStringLiteral("car://composited-livery");
        texture->image = livery;
        liveryTexture = std::move(texture);
    }
    for (CarMesh &mesh : car.meshes) {
        const QString materialPath = mesh.material != nullptr
            ? mesh.material->resourcePath
            : QString();
        if (mesh.materialName.contains(QStringLiteral("glass"), Qt::CaseInsensitive)
            || materialPath.contains(QStringLiteral("glass"), Qt::CaseInsensitive)) {
            ++excludedGlass;
            continue;
        }

        const bool usesLivery = liveryTexture != nullptr
            && mesh.paintMaterialHash != 0 && mesh.liveryUvChannel >= 0;
        std::shared_ptr<const OriginalShaderMaterialTexture> diffuse =
            usesLivery ? liveryTexture : nullptr;
        const std::shared_ptr<const ModelMaterialTexture> nativeDiffuse =
            mesh.material != nullptr ? mesh.material->diffuseTexture : nullptr;
        if (diffuse != nullptr) {
            ++liveryDraws;
        } else if (nativeDiffuse != nullptr && nativeDiffuse->image.valid()) {
            const auto found = decodedTextures.constFind(nativeDiffuse.get());
            if (found != decodedTextures.cend()) {
                diffuse = found.value();
            } else {
                auto texture = std::make_shared<OriginalShaderMaterialTexture>();
                texture->semantic = QStringLiteral("DiffuseTexture");
                texture->sourceEntry = nativeDiffuse->path.isEmpty()
                    ? QStringLiteral("car://native-diffuse")
                    : nativeDiffuse->path;
                texture->image = nativeDiffuse->image;
                diffuse = texture;
                decodedTextures.insert(nativeDiffuse.get(), std::move(texture));
            }
        } else {
            std::array<float, 3> color = paintColor;
            if (mesh.paintMaterialHash == 0 && mesh.material != nullptr
                && mesh.material->hasBaseColor) {
                color = mesh.material->baseColor;
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
        }

        CarModel singleMesh;
        singleMesh.sourcePath = source;
        singleMesh.boundsMin = boundsMin;
        singleMesh.boundsMax = boundsMax;
        const QString drawName = QStringLiteral("car/%1").arg(mesh.materialName);
        const int diffuseUvChannel = usesLivery ? mesh.liveryUvChannel : 0;
        singleMesh.meshes.push_back(std::move(mesh));
        scene->draws.push_back({
            drawName, source, OriginalShaderSurfaceFamily::Default,
            std::move(singleMesh), scene->carPlacement, std::move(diffuse),
            diffuseUvChannel});
        ++texturedDraws;
    }
    if (scene->draws.size() == firstDraw) {
        if (error != nullptr) {
            *error = QStringLiteral("the car contains no supported opaque meshes");
        }
        return false;
    }
    scene->carStatus = QStringLiteral(
        "DX12 car added with %1 opaque material draws; %2 use the composited "
        "livery atlas; %3 glass draws excluded")
        .arg(texturedDraws)
        .arg(liveryDraws)
        .arg(excludedGlass);
    return true;
}

} // namespace fh6
