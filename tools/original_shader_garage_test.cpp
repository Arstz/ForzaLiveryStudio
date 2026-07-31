#include "original_shader_garage.h"
#include "model_material.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

bool require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    bool ok = true;

    const fh6::OriginalShaderGarageScene missing =
        fh6::loadOriginalShaderGarageScene(QString());
    ok &= require(!missing.valid(), "unconfigured game folder must not produce a valid scene");
    ok &= require(
        missing.error == QStringLiteral("game folder is not configured"),
        "unconfigured game folder must return a stable diagnostic");

    if (argc < 2) {
        return ok ? 0 : 1;
    }

    fh6::OriginalShaderGarageScene scene =
        fh6::loadOriginalShaderGarageScene(QString::fromLocal8Bit(argv[1]));
    if (!require(scene.valid(), qPrintable(scene.error))) {
        return 1;
    }
    std::printf(
        "original shader garage: %lld vertices, %lld triangles, %zu draws\n%s\n",
        scene.totalVertices(), scene.totalTriangles(), scene.draws.size(),
        qPrintable(scene.materialStatus));
    ok &= require(scene.name == QStringLiteral("Tokyo House"), "scene identity changed");
    const fh6::ModelVec3 carOrigin = scene.carPlacement.transformPoint({});
    ok &= require(
        std::abs(carOrigin.x + 2.291748f) < 0.00001f
            && std::abs(carOrigin.y - 0.023865f) < 0.00001f
            && std::abs(carOrigin.z - 0.546875f) < 0.00001f,
        "House 8 main-car locator changed");
    ok &= require(scene.draws.size() == 42, "Tokyo garage shell draw count changed");
    ok &= require(scene.totalVertices() == 108362, "Tokyo garage shell vertex count changed");
    ok &= require(scene.totalTriangles() == 110782, "Tokyo garage shell triangle count changed");
    ok &= require(scene.defaultProgram.valid(), "default DXIL pair is invalid");
    ok &= require(scene.floorProgram.valid(), "floor DXIL pair is invalid");
    ok &= require(
        scene.environment.diffuseCubemap.valid(),
        "Tokyo neutral diffuse lighting resource is invalid");
    ok &= require(
        !scene.environment.specularCubemap.valid(),
        "unrelated garage specular probe must not be loaded for Tokyo");
    ok &= require(
        scene.environment.diffuseCubemap.width == 1
            && scene.environment.diffuseCubemap.height == 1,
        "Tokyo neutral diffuse cube topology changed");
    ok &= require(
        scene.authoredLights.size() == 20
            && scene.authoredLights.front().presetHash == 0x49a70618u
            && scene.lightingStatus.contains(QStringLiteral("roof lightdb"))
            && scene.lighting.source.contains(QStringLiteral("compatibility")),
        "Tokyo authored lighting provenance must remain explicit");
    const fh6::ModelVec3 placedOrigin =
        scene.draws.front().placement.transformPoint({});
    const fh6::ModelVec3 placedForward =
        scene.draws.front().placement.transformVector({0.0f, 0.0f, 1.0f});
    ok &= require(
        qFuzzyIsNull(placedOrigin.x) && qFuzzyIsNull(placedOrigin.y)
            && qFuzzyIsNull(placedOrigin.z),
        "Tokyo House placement translation changed");
    ok &= require(
        qFuzzyIsNull(placedForward.x) && qFuzzyIsNull(placedForward.y)
            && qFuzzyCompare(placedForward.z, 1.0f),
        "Tokyo House placement orientation changed");
    ok &= require(
        scene.materialStatus.contains(QStringLiteral("42/42")),
        "authored diffuse coverage must remain explicit");
    ok &= require(
        scene.glassStatus.contains(QStringLiteral("excluded")),
        "glass limitation must remain explicit");
    for (const auto &texture : scene.materialTextures) {
        ok &= require(texture.valid(), "material fallback texture is invalid");
    }
    for (const auto &draw : scene.draws) {
        ok &= require(
            draw.diffuseTexture != nullptr && draw.diffuseTexture->valid(),
            "Tokyo draw is missing its authored diffuse texture");
        ok &= require(
            draw.diffuseTexture != nullptr
                && (draw.diffuseTexture->sourceEntry.contains(
                        QStringLiteral("_bclr_"), Qt::CaseInsensitive)
                    || draw.diffuseTexture->sourceEntry.contains(
                        QStringLiteral("_emis_"), Qt::CaseInsensitive)),
            "Tokyo draw is not using an authored base-colour or emissive swatch");
    }
    const auto roof = std::find_if(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.source.contains(
                QStringLiteral("bld_gbl_grge_custom_02_roof_a"),
                Qt::CaseInsensitive);
        });
    ok &= require(
        roof != scene.draws.cend(),
        "Tokyo garage is missing its authored roof LOD0");
    if (roof != scene.draws.cend()) {
        const fh6::ModelVec3 placedOrigin = roof->placement.transformPoint({});
        ok &= require(
            qFuzzyIsNull(placedOrigin.x) && qFuzzyIsNull(placedOrigin.y)
                && qFuzzyIsNull(placedOrigin.z),
            "Tokyo roof is not in the shared garage-local frame");
    }
    fh6::CarModel car;
    car.sourcePath = QStringLiteral("test.carbin");
    car.boundsMin = {-1.0f, 0.0f, -2.0f};
    car.boundsMax = {1.0f, 1.0f, 2.0f};
    fh6::CarMesh carMesh;
    carMesh.materialName = QStringLiteral("paint");
    carMesh.paintMaterialHash = 1;
    carMesh.liveryUvChannel = 3;
    carMesh.material = std::make_shared<fh6::ModelMaterial>();
    carMesh.positions = {{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    carMesh.uvChannels.resize(4);
    carMesh.uvChannels[3] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    carMesh.indices = {0, 1, 2};
    car.meshes.push_back(std::move(carMesh));
    fh6::SwatchImage livery;
    livery.width = 1;
    livery.height = 1;
    livery.rgba = {255, 0, 0, 255};
    QString carError;
    const std::size_t garageDraws = scene.draws.size();
    ok &= require(
        fh6::appendOriginalShaderGarageCar(
            &scene, std::move(car), {0.2f, 0.4f, 0.6f}, livery, &carError),
        qPrintable(carError));
    ok &= require(
        scene.draws.size() == garageDraws + 1
            && scene.draws.back().diffuseTexture != nullptr
            && scene.draws.back().diffuseTexture->sourceEntry
                == QStringLiteral("car://composited-livery")
            && scene.draws.back().diffuseUvChannel == 3,
        "DX12 car livery atlas was not appended on its authored UV channel");

    if (ok) {
        std::printf("original shader garage validation passed\n");
    }
    return ok ? 0 : 1;
}
