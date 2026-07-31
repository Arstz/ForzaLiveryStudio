#include "original_shader_garage.h"

#include "game_paths.h"
#include "model_bundle.h"
#include "model_material.h"
#include "pgzp_extract.h"
#include "zip_extract.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>

#include <zlib.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace fh6 {

namespace {

constexpr auto kTokyoHouseLod0 =
    "bld_cty_dcks_playerhome_01_a_cluster004.i.modelbin";

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
    const QString &archive, const QString &manifest, const CarModel &geometry,
    QString *error) {
    QSet<quint32> targetHashes;
    for (const CarMesh &mesh : geometry.meshes) {
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

std::shared_ptr<const OriginalShaderMaterialTexture> meshDiffuseTexture(
    const CarMesh &mesh,
    const QHash<quint32, std::shared_ptr<const OriginalShaderMaterialTexture>> &textures) {
    if (mesh.material == nullptr) {
        return {};
    }
    for (const ModelMaterialParameter &parameter : mesh.material->parameters) {
        if (parameter.type != ModelMaterialParameterType::Texture2D) {
            continue;
        }
        const auto found = textures.constFind(parameter.texturePathHash);
        if (found != textures.cend()) {
            return found.value();
        }
    }
    return {};
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
        && environment.valid();
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
    scene.geometryStatus = QStringLiteral(
        "Exact Tokyo City Docks player-home LOD0 geometry; decal, glass, and lower-LOD passes omitted");
    scene.materialStatus = QStringLiteral("Tokyo material textures are loading");
    scene.glassStatus = QStringLiteral(
        "Tokyo House glass excluded until exact transparent shader and texture bindings are active");

    const QString media = gameMediaDir(gameFolder);
    if (media.isEmpty()) {
        scene.error = QStringLiteral("game folder is not configured");
        return scene;
    }

    const QString brioDirectory = QDir(media).filePath(QStringLiteral("Tracks/Brio"));
    const QString archive = QDir(brioDirectory).filePath(QStringLiteral("GeoChunk0.minizip"));
    const QString manifest = QDir(brioDirectory).filePath(
        QStringLiteral("ChunkContentsMiniZip0.txt"));
    QString error;
    std::vector<PgzpExtractedEntry> extracted = extractPgzpEntries(
        archive, manifest, {QString::fromLatin1(kTokyoHouseLod0)}, &error);
    if (extracted.size() != 1) {
        fail(&scene, QStringLiteral("Tokyo House geometry"), error);
        return scene;
    }
    const QString source = QStringLiteral("%1!/%2").arg(
        archive, extracted.front().manifestPath);
    CarModel geometry = decodeModelBytes(extracted.front().bytes, source, &error);
    if (geometry.meshes.empty()) {
        fail(&scene, QStringLiteral("Tokyo House geometry"), error);
        return scene;
    }
    geometry.meshes.erase(
        std::remove_if(
            geometry.meshes.begin(), geometry.meshes.end(),
            [](const CarMesh &mesh) {
                return mesh.materialName.contains(
                    QStringLiteral("glass"), Qt::CaseInsensitive);
            }),
        geometry.meshes.end());
    error.clear();
    const auto authoredDiffuse = loadAuthoredDiffuseTextures(
        archive, manifest, geometry, &error);
    if (authoredDiffuse.isEmpty()) {
        fail(&scene, QStringLiteral("Tokyo House material textures"), error);
        return scene;
    }
    const ModelVec3 boundsMin = geometry.boundsMin;
    const ModelVec3 boundsMax = geometry.boundsMax;
    const QString modelSource = geometry.sourcePath;
    scene.draws.reserve(geometry.meshes.size());
    int texturedDraws = 0;
    for (CarMesh &mesh : geometry.meshes) {
        CarModel singleMesh;
        singleMesh.sourcePath = modelSource;
        singleMesh.boundsMin = boundsMin;
        singleMesh.boundsMax = boundsMax;
        const QString drawName = mesh.materialName;
        auto diffuseTexture = meshDiffuseTexture(mesh, authoredDiffuse);
        texturedDraws += diffuseTexture != nullptr ? 1 : 0;
        singleMesh.meshes.push_back(std::move(mesh));
        scene.draws.push_back({
            drawName, source, OriginalShaderSurfaceFamily::Default,
            std::move(singleMesh), {}, std::move(diffuseTexture)});
    }
    scene.materialStatus = QStringLiteral(
        "Authored summer base-colour swatches bound for %1/%2 Tokyo material draws; "
        "normal/RCSM slots still use neutral shader defaults")
        .arg(texturedDraws)
        .arg(scene.draws.size());

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

    scene.environment = loadGarageEnvironmentResources(gameFolder);
    if (!scene.environment.valid()) {
        fail(&scene, QStringLiteral("garage lighting resources"), scene.environment.error);
        return scene;
    }

    scene.error.clear();
    return scene;
}

} // namespace fh6
