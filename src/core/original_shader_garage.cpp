#include "original_shader_garage.h"

#include "game_paths.h"
#include "model_bundle.h"
#include "zip_extract.h"

#include <QDir>

#include <algorithm>
#include <exception>
#include <utility>

namespace fh6 {

namespace {

constexpr auto kModelEntry = "000.i.modelbin";

struct DrawResource {
    const char *name;
    const char *archive;
    OriginalShaderSurfaceFamily family;
};

constexpr std::array<DrawResource, 4> kDrawResources = {{
    {"homespace_floor", "homespace_floor.i.zip", OriginalShaderSurfaceFamily::Floor},
    {"homespace", "homespace.i.zip", OriginalShaderSurfaceFamily::Default},
    {"homespace_cover", "homespace_cover.i.zip", OriginalShaderSurfaceFamily::Default},
    {"homespace_roof", "homespace_roof.i.zip", OriginalShaderSurfaceFamily::Default},
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
    return error.isEmpty() && draws.size() == kDrawResources.size()
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
    scene.glassStatus = QStringLiteral(
        "homespace_glass excluded: Default.materialbin does not identify one exact glass shader family");

    const QString media = gameMediaDir(gameFolder);
    if (media.isEmpty()) {
        scene.error = QStringLiteral("game folder is not configured");
        return scene;
    }

    const QString geometryDirectory = QDir(media).filePath(QStringLiteral(
        "Stripped/gs/tracks/brio/scene/models/whitebox/homespace"));
    scene.draws.reserve(kDrawResources.size());
    for (const DrawResource &resource : kDrawResources) {
        const QString archive = QDir(geometryDirectory).filePath(
            QString::fromLatin1(resource.archive));
        QString error;
        const QByteArray bytes = readZipEntry(
            archive, QString::fromLatin1(kModelEntry), &error);
        if (bytes.isEmpty()) {
            fail(&scene, QStringLiteral("%1 geometry").arg(QString::fromLatin1(resource.name)), error);
            return scene;
        }
        const QString source = QStringLiteral("%1!/%2").arg(
            archive, QString::fromLatin1(kModelEntry));
        CarModel geometry = decodeModelBytes(bytes, source, &error);
        if (geometry.meshes.empty()) {
            fail(&scene, QStringLiteral("%1 geometry").arg(QString::fromLatin1(resource.name)), error);
            return scene;
        }
        scene.draws.push_back({
            QString::fromLatin1(resource.name), source, resource.family,
            std::move(geometry)});
    }

    const QString shaderArchive = QDir(media).filePath(QStringLiteral("_library/Homespace.zip"));
    QString error;
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
