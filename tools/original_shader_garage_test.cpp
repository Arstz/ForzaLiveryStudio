#include "original_shader_garage.h"
#include "core_types.h"
#include "model_material.h"
#include "paint_finish_catalog.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>

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

    QTemporaryDir garageCache;
    ok &= require(garageCache.isValid(), "temporary garage cache directory is invalid");
    qputenv("FLS_GARAGE_CACHE_DIR", garageCache.path().toUtf8());
    fh6::OriginalShaderGarageScene scene =
        fh6::loadCachedOriginalShaderGarageScene(QString::fromLocal8Bit(argv[1]));
    if (!require(scene.valid(), qPrintable(scene.error))) {
        return 1;
    }
    std::printf(
        "original shader garage: %lld vertices, %lld triangles, %zu draws\n%s\n",
        scene.totalVertices(), scene.totalTriangles(), scene.draws.size(),
        qPrintable(scene.materialStatus));
    const QString cachePath = fh6::originalShaderGarageCachePath(
        QString::fromLocal8Bit(argv[1]));
    ok &= require(
        QFileInfo(cachePath).size() > 0
            && scene.materialStatus.contains(QStringLiteral("cache refreshed")),
        "Tokyo garage cache was not written");
    {
        QElapsedTimer cacheTimer;
        cacheTimer.start();
        const fh6::OriginalShaderGarageScene cached =
            fh6::loadCachedOriginalShaderGarageScene(
                QString::fromLocal8Bit(argv[1]));
        std::printf(
            "Tokyo garage cache: %.2f MiB, %lld ms read\n",
            QFileInfo(cachePath).size() / (1024.0 * 1024.0),
            static_cast<long long>(cacheTimer.elapsed()));
        ok &= require(
            cached.valid() && cached.draws.size() == scene.draws.size()
                && cached.totalVertices() == scene.totalVertices()
                && cached.materialStatus.contains(QStringLiteral("cache hit")),
            "Tokyo garage cache roundtrip changed the scene");
    }
    if (qEnvironmentVariableIsSet("FLS_DUMP_GARAGE_MATERIALS")) {
        for (const fh6::OriginalShaderGarageDraw &draw : scene.draws) {
            const fh6::CarMesh &mesh = draw.geometry.meshes.front();
            float minimumU = 0.0f;
            float maximumU = 0.0f;
            float minimumV = 0.0f;
            float maximumV = 0.0f;
            if (!mesh.uvChannels.empty() && !mesh.uvChannels[0].empty()) {
                minimumU = maximumU = mesh.uvChannels[0].front().u;
                minimumV = maximumV = mesh.uvChannels[0].front().v;
                for (const fh6::ModelVec2 &uv : mesh.uvChannels[0]) {
                    minimumU = std::min(minimumU, uv.u);
                    maximumU = std::max(maximumU, uv.u);
                    minimumV = std::min(minimumV, uv.v);
                    maximumV = std::max(maximumV, uv.v);
                }
            }
            const fh6::TexCoordTransform &transform = mesh.texCoordTransforms[0];
            std::printf(
                "garage material=%s base=%s uv=[%.3f,%.3f]x[%.3f,%.3f] "
                "transform=(%.3f,%.3f,%.3f,%.3f) tiling=(%.3f,%.3f)\n",
                qPrintable(draw.name),
                draw.diffuseTexture != nullptr
                    ? qPrintable(draw.diffuseTexture->sourceEntry) : "none",
                minimumU, maximumU, minimumV, maximumV,
                transform.offsetU, transform.scaleU,
                transform.offsetV, transform.scaleV,
                draw.uTiling, draw.vTiling);
        }
    }
    ok &= require(scene.name == QStringLiteral("Tokyo House"), "scene identity changed");
    const fh6::ModelVec3 carOrigin = scene.carPlacement.transformPoint({});
    std::printf(
        "Tokyo car placement: (%.6f, %.6f, %.6f)\n",
        carOrigin.x, carOrigin.y, carOrigin.z);
    ok &= require(
        std::abs(carOrigin.x + 2.291748f) < 0.00001f
            && std::abs(carOrigin.y - 0.023865f) < 0.00001f
            && std::abs(carOrigin.z - 0.546875f) < 0.00001f,
        "Tokyo House 8 car locator changed");
    ok &= require(scene.draws.size() == 457, "Tokyo House 8 draw count changed");
    ok &= require(scene.totalVertices() == 471323, "Tokyo House 8 vertex count changed");
    ok &= require(scene.totalTriangles() == 476925, "Tokyo House 8 triangle count changed");
    ok &= require(scene.defaultProgram.valid(), "default DXIL pair is invalid");
    ok &= require(scene.floorProgram.valid(), "floor DXIL pair is invalid");
    ok &= require(
        scene.environment.diffuseCubemap.valid(),
        "Tokyo Garage 09 diffuse lighting resource is invalid");
    ok &= require(
        scene.environment.specularCubemap.valid(),
        "Tokyo staged-space specular probe is invalid");
    ok &= require(
        scene.colorLut.valid() && scene.colorLut.dimension == 32
            && std::abs(scene.colorLut.scale - 100.0f) < 0.0001f,
        "Tokyo Homespace colour-grade LUT changed");
    ok &= require(
        scene.environment.panorama.valid()
            && scene.environment.panorama.texture.width == 8192
            && scene.environment.panorama.texture.height == 4096,
        "Tokyo Forte_Garage_01 panorama topology changed");
    std::printf(
        "Tokyo panorama: %dx%d, shell source: %s\n",
        scene.environment.panorama.texture.width,
        scene.environment.panorama.texture.height,
        qPrintable(scene.draws.front().source));
    ok &= require(
        scene.authoredLights.size() == 41
            && std::count_if(
                   scene.authoredLights.cbegin(), scene.authoredLights.cend(),
                   [](const fh6::OriginalShaderPointLight &light) {
                       return light.enabled && light.type == 1
                           && std::abs(light.range - 10.0f) < 0.0001f
                           && std::abs(light.intensity - 5000.0f) < 0.01f;
                   }) == 7
            && std::count_if(
                   scene.authoredLights.cbegin(), scene.authoredLights.cend(),
                   [](const fh6::OriginalShaderPointLight &light) {
                       return light.enabled && light.type == 1
                           && std::abs(light.range - 9.0f) < 0.0001f
                           && std::abs(light.intensity - 500000.0f) < 0.01f;
                   }) == 20
            && scene.lightingStatus.contains(QStringLiteral("27 active")),
        "Tokyo authored roof/floodlight presets changed");
    const fh6::ModelVec3 roofEmission =
        scene.authoredLights.front().transform.transformVector({0.0f, 0.0f, 1.0f});
    ok &= require(
        roofEmission.y < -0.99f,
        "Tokyo roof fixture +Z emission axis no longer points into the garage");
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
        scene.materialStatus.contains(QStringLiteral("449/457")),
        "Tokyo SourceHash material coverage must remain explicit");
    ok &= require(
        scene.glassStatus.contains(QStringLiteral("translucent")),
        "Tokyo glass pass must remain explicit");
    for (const auto &texture : scene.materialTextures) {
        ok &= require(texture.valid(), "material fallback texture is invalid");
    }
    for (const auto &draw : scene.draws) {
        ok &= require(
            draw.diffuseTexture != nullptr && draw.diffuseTexture->valid(),
            "Tokyo draw is missing its authored diffuse texture");
    }
    const auto enclosure = std::find_if(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.source.contains(
                QStringLiteral("garage_customiser"),
                Qt::CaseInsensitive);
        });
    ok &= require(
        enclosure != scene.draws.cend(),
        "Tokyo scene is missing its coordinate-matched garage enclosure");
    const auto distressedOverlay = std::find_if(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.hidden && draw.source.contains(
                QStringLiteral("garage_customiser"), Qt::CaseInsensitive);
        });
    ok &= require(
        distressedOverlay != scene.draws.cend(),
        "Tokyo clean presentation must hide the shell's distressed decal overlays");
    const auto cleanPaintedShell = std::find_if(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.name == QStringLiteral("BLD_GBL_GRGE_CUSTOM_02_A")
                && draw.diffuseTexture != nullptr
                && draw.diffuseTexture->sourceEntry.contains(
                    QStringLiteral("tex_gbl_tile_mtl_paint_a"),
                    Qt::CaseInsensitive);
        });
    const auto cleanConcreteShell = std::find_if(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.name == QStringLiteral("BLD_GBL_GRGE_CUSTOM_02_B")
                && draw.diffuseTexture != nullptr
                && draw.diffuseTexture->sourceEntry.contains(
                    QStringLiteral("tex_gbl_grnd_concrete_pol_a"),
                    Qt::CaseInsensitive);
        });
    const auto cleanFloorShell = std::find_if(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.family == fh6::OriginalShaderSurfaceFamily::Floor
                && !draw.hidden && draw.diffuseTexture != nullptr
                && draw.diffuseTexture->sourceEntry.contains(
                    QStringLiteral("tex_el_tile_concrete_polished_a"),
                    Qt::CaseInsensitive);
        });
    ok &= require(
        cleanPaintedShell != scene.draws.cend()
            && cleanPaintedShell->rawMaterialUv
            && cleanPaintedShell->normalTexture != nullptr
            && cleanPaintedShell->surfaceTexture != nullptr
            && cleanConcreteShell != scene.draws.cend()
            && cleanConcreteShell->rawMaterialUv
            && cleanConcreteShell->normalTexture != nullptr
            && cleanConcreteShell->surfaceTexture != nullptr
            && cleanFloorShell != scene.draws.cend()
            && cleanFloorShell->rawMaterialUv
            && cleanFloorShell->normalTexture != nullptr
            && cleanFloorShell->surfaceTexture != nullptr,
        "Tokyo shell must use complete clean tiling material sets");
    const auto floodlight = std::find_if(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.source.contains(
                QStringLiteral("prp_el_garage_lights_flood_a"),
                Qt::CaseInsensitive);
        });
    ok &= require(
        floodlight != scene.draws.cend(),
        "Tokyo House 8 scene is missing authored floodlight props");
    fh6::CarModel car;
    car.sourcePath = QStringLiteral("test.carbin");
    car.boundsMin = {-1.0f, 0.0f, -2.0f};
    car.boundsMax = {1.0f, 1.0f, 2.0f};
    fh6::CarMesh carMesh;
    carMesh.name = QStringLiteral("body_LODS0");
    carMesh.modelInstanceId = 7;
    carMesh.materialName = QStringLiteral("carpaint_standard");
    carMesh.paintMaterialHash = 1;
    carMesh.liveryUvChannel = 3;
    carMesh.material = std::make_shared<fh6::ModelMaterial>();
    carMesh.material->gloss = 0.85f;
    carMesh.material->automotivePaint.hasClearCoatCoverage = true;
    carMesh.material->automotivePaint.clearCoatCoverage = 0.72f;
    carMesh.material->automotivePaint.hasClearCoatRoughness = true;
    carMesh.material->automotivePaint.clearCoatRoughness = 0.08f;
    carMesh.material->automotivePaint.hasClearCoatTint = true;
    carMesh.material->automotivePaint.clearCoatTint = {0.9f, 0.95f, 1.0f, 1.0f};
    auto normalTexture = std::make_shared<fh6::ModelMaterialTexture>();
    normalTexture->path = QStringLiteral("car/test_nrml.swatchbin");
    normalTexture->image.width = 1;
    normalTexture->image.height = 1;
    normalTexture->image.rgba = {128, 128, 255, 255};
    carMesh.material->normalTexture = normalTexture;
    auto surfaceTexture = std::make_shared<fh6::ModelMaterialTexture>();
    surfaceTexture->path = QStringLiteral("car/test_rmao.swatchbin");
    surfaceTexture->image.width = 1;
    surfaceTexture->image.height = 1;
    surfaceTexture->image.rgba = {128, 0, 255, 255};
    carMesh.material->surfaceTexture = surfaceTexture;
    carMesh.material->uTiling = 2.0f;
    carMesh.material->vTiling = 3.0f;
    carMesh.positions = {{-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    carMesh.uvChannels.resize(4);
    carMesh.uvChannels[0] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.25f, 0.5f}};
    carMesh.uvChannels[3] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    carMesh.indices = {0, 1, 2};
    fh6::CarMesh lowerLodMesh = carMesh;
    lowerLodMesh.name = QStringLiteral("body_LOD1");
    fh6::CarMesh factoryStickerMesh = carMesh;
    factoryStickerMesh.name = QStringLiteral("factorySponsorLivery_LODS0");
    factoryStickerMesh.materialName = QStringLiteral("carPaint_livery");
    factoryStickerMesh.material = std::make_shared<fh6::ModelMaterial>(*carMesh.material);
    factoryStickerMesh.material->resourcePath = QStringLiteral(
        "Game:/Media/cars/_library/materials/_fmnext/carpaint_default/"
        "livery_sticker.materialbin");
    fh6::CarMesh shadowMesh;
    shadowMesh.name = QStringLiteral("shadow");
    shadowMesh.positions = carMesh.positions;
    shadowMesh.indices = carMesh.indices;
    car.meshes.push_back(std::move(carMesh));
    car.meshes.push_back(std::move(lowerLodMesh));
    car.meshes.push_back(std::move(factoryStickerMesh));
    car.shadowMeshes.push_back(std::move(shadowMesh));
    fh6::SwatchImage livery;
    livery.width = 1;
    livery.height = 1;
    livery.rgba = {255, 0, 0, 255};
    fh6::LiveryMaskSet liveryMasks;
    liveryMasks.loaded = true;
    liveryMasks.loadedMasks = 1;
    fh6::LiverySide &frontMask = liveryMasks.sides[0];
    frontMask.valid = true;
    frontMask.left = -1024.0f;
    frontMask.right = 1024.0f;
    frontMask.top = 512.0f;
    frontMask.bottom = -512.0f;
    frontMask.mask.width = 1;
    frontMask.mask.height = 1;
    frontMask.mask.coverage = {255};
    QString carError;
    const std::size_t garageDraws = scene.draws.size();
    ok &= require(
        fh6::appendOriginalShaderGarageCar(
            &scene, std::move(car), {0.2f, 0.4f, 0.6f}, livery,
            &liveryMasks, nullptr, nullptr, nullptr, nullptr, &carError),
        qPrintable(carError));
    const fh6::OriginalShaderGarageDraw &carDraw = scene.draws[garageDraws];
    const fh6::OriginalShaderGarageDraw &shadowDraw = scene.draws[garageDraws + 1];
    ok &= require(
        scene.draws.size() == garageDraws + 2
            && carDraw.diffuseTexture != nullptr
            && carDraw.diffuseTexture->sourceEntry
                == QStringLiteral("car://composited-livery")
            && carDraw.normalTexture != nullptr
            && carDraw.surfaceTexture != nullptr
            && carDraw.diffuseUvChannel == 3
            && carDraw.baseColor
                == std::array<float, 3>{0.2f, 0.4f, 0.6f}
            && carDraw.family == fh6::OriginalShaderSurfaceFamily::Car
            && carDraw.liveryBaseTexture
            && carDraw.liveryAllowedSides == 0x1fu
            && scene.liveryMapping.valid()
            && carDraw.uTiling == 1.0f
            && carDraw.vTiling == 1.0f
            && carDraw.detailUTiling == 2.0f
            && carDraw.detailVTiling == 3.0f
            && std::abs(carDraw.clearCoatCoverage - 0.72f) < 0.00001f
            && std::abs(carDraw.clearCoatRoughness - 0.08f) < 0.00001f
            && carDraw.clearCoatTint
                == std::array<float, 3>{0.9f, 0.95f, 1.0f}
            && shadowDraw.shadowCasterOnly
            && shadowDraw.geometry.meshes.size() == 1
            && scene.carStatus.contains(QStringLiteral("1 lower-LOD"))
            && scene.carStatus.contains(QStringLiteral("1 factory-livery sticker"))
            && scene.carStatus.contains(QStringLiteral(
                "1 authored shadow-proxy mesh")),
        "DX12 car livery/detail maps or factory-sticker exclusion changed");

    fh6::CarModel glassCar;
    glassCar.sourcePath = QStringLiteral("glass-test.carbin");
    glassCar.boundsMin = {-1.0f, 0.0f, -1.0f};
    glassCar.boundsMax = {1.0f, 1.0f, 1.0f};
    fh6::CarMesh glassMesh;
    glassMesh.name = QStringLiteral("glassF_a_LODS0");
    glassMesh.materialName = QStringLiteral("gls_windshield");
    glassMesh.drawGroups = fh6::car_draw_groups::kExterior
        | fh6::car_draw_groups::kShadow;
    glassMesh.material = std::make_shared<fh6::ModelMaterial>();
    glassMesh.material->resourcePath = QStringLiteral(
        "Game:/Media/Cars/_library/materials/_fmnext/glass/"
        "glass_windshield.materialbin");
    glassMesh.material->resolvedFromLibrary = true;
    glassMesh.material->hasBaseColor = true;
    glassMesh.material->baseColor = {0.3763f, 0.4851f, 0.4851f};
    auto alphaTexture = std::make_shared<fh6::ModelMaterialTexture>();
    alphaTexture->path = QStringLiteral("glass_alpha.swatchbin");
    alphaTexture->image.width = 1;
    alphaTexture->image.height = 1;
    alphaTexture->image.rgba = {128, 128, 128, 255};
    glassMesh.material->alphaTexture = alphaTexture;
    glassMesh.positions = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    glassMesh.uvChannels.resize(1);
    glassMesh.uvChannels[0] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    glassMesh.indices = {0, 1, 2};
    fh6::CarMesh interiorGlassMesh = glassMesh;
    interiorGlassMesh.name = QStringLiteral("glassFInt_a_LODS0");
    interiorGlassMesh.drawGroups = fh6::car_draw_groups::kCockpit;
    interiorGlassMesh.interiorWindshield = true;
    interiorGlassMesh.material = std::make_shared<fh6::ModelMaterial>(
        *glassMesh.material);
    fh6::CarMesh blackFrameMesh = glassMesh;
    blackFrameMesh.name = QStringLiteral("glassF_a_LODS0");
    blackFrameMesh.materialName = QStringLiteral("blackGlass");
    blackFrameMesh.material = std::make_shared<fh6::ModelMaterial>();
    blackFrameMesh.material->resourcePath = QStringLiteral(
        "Game:/Media/Cars/_library/materials/_fmnext/specialcase/"
        "blackframe.materialbin");
    blackFrameMesh.material->resolvedFromLibrary = true;
    blackFrameMesh.material->hasBaseColor = true;
    blackFrameMesh.material->baseColor = {0.0048f, 0.0048f, 0.0048f};
    blackFrameMesh.material->gloss = 0.585f;
    fh6::CarMesh reflectionMesh = glassMesh;
    reflectionMesh.name = QStringLiteral("windshieldReflection_LODS0");
    reflectionMesh.drawGroups = fh6::car_draw_groups::kWindshieldReflection;
    reflectionMesh.material = std::make_shared<fh6::ModelMaterial>(
        *glassMesh.material);
    glassCar.meshes.push_back(std::move(glassMesh));
    glassCar.meshes.push_back(std::move(interiorGlassMesh));
    glassCar.meshes.push_back(std::move(blackFrameMesh));
    glassCar.meshes.push_back(std::move(reflectionMesh));
    const std::size_t drawsBeforeGlass = scene.draws.size();
    ok &= require(
        fh6::appendOriginalShaderGarageCar(
            &scene, std::move(glassCar), {0.2f, 0.4f, 0.6f}, {},
            nullptr, nullptr, nullptr, nullptr, nullptr, &carError),
        qPrintable(carError));
    const auto findAddedGlassDraw = [&](const QString &meshName) {
        return std::find_if(
            scene.draws.cbegin() + static_cast<std::ptrdiff_t>(drawsBeforeGlass),
            scene.draws.cend(), [&](const auto &draw) {
                return !draw.geometry.meshes.empty()
                    && draw.geometry.meshes.front().name == meshName;
            });
    };
    const auto exteriorGlassDraw = findAddedGlassDraw(QStringLiteral("glassF_a_LODS0"));
    const auto interiorGlassDraw = findAddedGlassDraw(QStringLiteral("glassFInt_a_LODS0"));
    const auto reflectionDraw = findAddedGlassDraw(
        QStringLiteral("windshieldReflection_LODS0"));
    const auto blackFrameDraw = std::find_if(
        scene.draws.cbegin() + static_cast<std::ptrdiff_t>(drawsBeforeGlass),
        scene.draws.cend(), [](const auto &draw) {
            return draw.name == QStringLiteral("car/blackGlass");
        });
    ok &= require(
        scene.draws.size() == drawsBeforeGlass + 4
            && exteriorGlassDraw != scene.draws.cend()
            && exteriorGlassDraw->translucent
            && exteriorGlassDraw->alphaTexture != nullptr
            && exteriorGlassDraw->diffuseTexture != nullptr
            && exteriorGlassDraw->diffuseTexture->image.rgba[0] < 255
            && std::abs(exteriorGlassDraw->opacity - 0.58f) < 0.00001f
            && exteriorGlassDraw->gloss >= 0.90f
            && exteriorGlassDraw->shaderFamily == fh6::ModelShaderFamily::Glass
            && interiorGlassDraw != scene.draws.cend()
            && interiorGlassDraw->translucent
            && interiorGlassDraw->interiorWindshield
            && std::abs(interiorGlassDraw->opacity - 0.28f) < 0.00001f
            && reflectionDraw != scene.draws.cend()
            && reflectionDraw->translucent
            && reflectionDraw->drawGroups
                == fh6::car_draw_groups::kWindshieldReflection
            && blackFrameDraw != scene.draws.cend()
            && !blackFrameDraw->translucent
            && blackFrameDraw->shaderFamily == fh6::ModelShaderFamily::Generic
            && scene.carStatus.contains(QStringLiteral("1 interior glass shell"))
            && scene.carStatus.contains(QStringLiteral("1 windshield-reflection member")),
        "DX12 car glass layering, tint, reflection groups, or black frame changed");

    fh6::CarModel wheelCar;
    wheelCar.sourcePath = QStringLiteral("wheel-test.carbin");
    wheelCar.boundsMin = {-1.0f, -1.0f, -1.0f};
    wheelCar.boundsMax = {1.0f, 1.0f, 1.0f};
    fh6::CarMesh wheelMesh;
    wheelMesh.name = QStringLiteral("wheel_LODS0");
    wheelMesh.sourceModelPath = QStringLiteral("_library/scene/wheels/test.modelbin");
    wheelMesh.materialName = QStringLiteral("rim");
    wheelMesh.paintMaterialHash = 0x1234u;
    wheelMesh.material = std::make_shared<fh6::ModelMaterial>();
    wheelMesh.positions = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    wheelMesh.uvChannels.resize(1);
    wheelMesh.uvChannels[0] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    wheelMesh.indices = {0, 1, 2};
    wheelCar.meshes.push_back(std::move(wheelMesh));
    const std::size_t drawsBeforeWheel = scene.draws.size();
    ok &= require(
        fh6::appendOriginalShaderGarageCar(
            &scene, std::move(wheelCar), {0.9f, 0.1f, 0.1f}, {},
            nullptr, nullptr, nullptr, nullptr, nullptr, &carError),
        qPrintable(carError));
    const fh6::OriginalShaderGarageDraw &wheelDraw = scene.draws.back();
    ok &= require(
        scene.draws.size() == drawsBeforeWheel + 1
            && wheelDraw.diffuseTexture != nullptr
            && wheelDraw.diffuseTexture->image.rgba
                == std::vector<std::uint8_t>{82, 87, 94, 255}
            && wheelDraw.baseColor == std::array<float, 3>{1.0f, 1.0f, 1.0f}
            && std::abs(wheelDraw.gloss - 0.78f) < 0.00001f
            && std::abs(wheelDraw.metallic - 0.85f) < 0.00001f,
        "DX12 wheel fallback must not inherit the body-paint colour");

    fh6::PaintFinishLibrary paintFinishes;
    paintFinishes.load(QString::fromLocal8Bit(argv[1]));
    fh6::LiveryPaintState inactiveWheelPaintState;
    fh6::LiveryPaintMaterial inactiveWheelPaint;
    inactiveWheelPaint.materialHash = 0x13u;
    inactiveWheelPaint.finish = 13;
    inactiveWheelPaintState.materials.push_back(inactiveWheelPaint);
    fh6::CarModel nativeWheelCar;
    nativeWheelCar.sourcePath = QStringLiteral("native-wheel-test.carbin");
    nativeWheelCar.boundsMin = {-1.0f, -1.0f, -1.0f};
    nativeWheelCar.boundsMax = {1.0f, 1.0f, 1.0f};
    fh6::CarMesh nativeWheelMesh;
    nativeWheelMesh.name = QStringLiteral("wheel_LODS0");
    nativeWheelMesh.sourceModelPath = QStringLiteral(
        "_library/scene/wheels/test.modelbin");
    nativeWheelMesh.materialName = QStringLiteral("rim");
    nativeWheelMesh.paintMaterialHash = inactiveWheelPaint.materialHash;
    nativeWheelMesh.material = std::make_shared<fh6::ModelMaterial>();
    nativeWheelMesh.material->resolvedFromLibrary = true;
    nativeWheelMesh.material->hasBaseColor = true;
    nativeWheelMesh.material->baseColor = {0.0194f, 0.0194f, 0.0194f};
    nativeWheelMesh.positions = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    nativeWheelMesh.uvChannels.resize(1);
    nativeWheelMesh.uvChannels[0] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    nativeWheelMesh.indices = {0, 1, 2};
    nativeWheelCar.meshes.push_back(std::move(nativeWheelMesh));
    const std::size_t drawsBeforeNativeWheel = scene.draws.size();
    ok &= require(
        paintFinishes.loaded()
            && fh6::appendOriginalShaderGarageCar(
                &scene, std::move(nativeWheelCar), {0.9f, 0.1f, 0.1f}, {},
                nullptr, nullptr, &inactiveWheelPaintState, nullptr,
                &paintFinishes, &carError),
        qPrintable(carError));
    const fh6::OriginalShaderGarageDraw &nativeWheelDraw = scene.draws.back();
    ok &= require(
        scene.draws.size() == drawsBeforeNativeWheel + 1
            && nativeWheelDraw.diffuseTexture != nullptr
            && !nativeWheelDraw.diffuseTexture->sourceEntry.contains(
                QStringLiteral("paint-finish"))
            && std::abs(nativeWheelDraw.gloss - 0.45f) < 0.00001f
            && std::abs(nativeWheelDraw.metallic) < 0.00001f
            && nativeWheelDraw.flakeCoverage == 0.0f,
        "DX12 inactive wheel paint must preserve the native wheel material");

    fh6::CarModel stockTireCar;
    stockTireCar.sourcePath = QStringLiteral("stock-tire-test.carbin");
    stockTireCar.boundsMin = {-1.0f, -1.0f, -1.0f};
    stockTireCar.boundsMax = {1.0f, 1.0f, 1.0f};
    fh6::CarMesh stockTireMesh;
    stockTireMesh.name = QStringLiteral("tireL_b_LODS0");
    stockTireMesh.sourceModelPath = QStringLiteral(
        "_library/scene/tires/tireL_b.modelbin");
    stockTireMesh.materialName = QStringLiteral("scaling_text");
    stockTireMesh.material = std::make_shared<fh6::ModelMaterial>();
    stockTireMesh.material->resourcePath = QStringLiteral(
        "Game:/Media/Cars/_library/materials/_fmnext/tires/"
        "tires_pg_sidewall_legacy.materialbin");
    stockTireMesh.material->resolvedFromLibrary = true;
    stockTireMesh.material->hasRoughnessShift = true;
    stockTireMesh.material->roughnessShift = 0.25f;
    auto tireNormal = std::make_shared<fh6::ModelMaterialTexture>();
    tireNormal->path = QStringLiteral("tires_pg_sidewall_legacy_nrml.swatchbin");
    tireNormal->image.width = 1;
    tireNormal->image.height = 1;
    tireNormal->image.rgba = {128, 128, 255, 255};
    stockTireMesh.material->normalTexture = tireNormal;
    auto tireHeightAo = std::make_shared<fh6::ModelMaterialTexture>();
    tireHeightAo->path = QStringLiteral(
        "tires_pg_sidewall_legacy_height_ao.swatchbin");
    tireHeightAo->image.width = 1;
    tireHeightAo->image.height = 1;
    tireHeightAo->image.rgba = {96, 192, 255, 255};
    stockTireMesh.material->tireHeightAoTexture = tireHeightAo;
    auto tireLocalAo = std::make_shared<fh6::ModelMaterialTexture>();
    tireLocalAo->path = QStringLiteral("tires_pg_sidewall_local_ao.swatchbin");
    tireLocalAo->image.width = 1;
    tireLocalAo->image.height = 1;
    tireLocalAo->image.rgba = {176, 176, 176, 255};
    stockTireMesh.material->aoTexture = tireLocalAo;
    stockTireMesh.positions = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    stockTireMesh.uvChannels.resize(1);
    stockTireMesh.uvChannels[0] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    stockTireMesh.indices = {0, 1, 2};
    stockTireCar.meshes.push_back(std::move(stockTireMesh));
    const std::size_t drawsBeforeStockTire = scene.draws.size();
    ok &= require(
        fh6::appendOriginalShaderGarageCar(
            &scene, std::move(stockTireCar), {0.9f, 0.1f, 0.1f}, {},
            nullptr, nullptr, nullptr, nullptr, nullptr, &carError),
        qPrintable(carError));
    const fh6::OriginalShaderGarageDraw &stockTireDraw = scene.draws.back();
    ok &= require(
        scene.draws.size() == drawsBeforeStockTire + 1
            && stockTireDraw.normalTexture != nullptr
            && stockTireDraw.tireHeightAoTexture != nullptr
            && stockTireDraw.aoTexture != nullptr
            && std::abs(stockTireDraw.roughnessShift - 0.25f) < 0.00001f
            && stockTireDraw.diffuseTexture != nullptr
            && stockTireDraw.diffuseTexture->image.rgba[0] < 16,
        "DX12 stock tire must retain its exact normal, height/AO, roughness, and dark rubber base");

    fh6::LiveryPaintState brassPaintState;
    fh6::LiveryPaintMaterial brassPaint;
    brassPaint.materialHash = 0x27u;
    brassPaint.finish = 27;
    brassPaintState.materials.push_back(brassPaint);
    fh6::CarModel brassCar;
    brassCar.sourcePath = QStringLiteral("brass-finish-test.carbin");
    brassCar.boundsMin = {-1.0f, 0.0f, -1.0f};
    brassCar.boundsMax = {1.0f, 1.0f, 1.0f};
    fh6::CarMesh brassMesh;
    brassMesh.name = QStringLiteral("body_LODS0");
    brassMesh.materialName = QStringLiteral("carpaint_standard");
    brassMesh.paintMaterialHash = brassPaint.materialHash;
    brassMesh.material = std::make_shared<fh6::ModelMaterial>();
    brassMesh.positions = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    brassMesh.uvChannels.resize(1);
    brassMesh.uvChannels[0] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    brassMesh.indices = {0, 1, 2};
    brassCar.meshes.push_back(std::move(brassMesh));
    const std::size_t drawsBeforeBrass = scene.draws.size();
    ok &= require(
        paintFinishes.loaded()
            && fh6::appendOriginalShaderGarageCar(
                &scene, std::move(brassCar), {1.0f, 1.0f, 1.0f}, {},
                nullptr, nullptr, &brassPaintState, nullptr, &paintFinishes,
                &carError),
        qPrintable(carError));
    const fh6::OriginalShaderGarageDraw &brassDraw = scene.draws.back();
    ok &= require(
        scene.draws.size() == drawsBeforeBrass + 1
            && brassDraw.diffuseTexture != nullptr
            && brassDraw.diffuseTexture->sourceEntry
                == QStringLiteral("car://paint-finish/27/pattern")
            && brassDraw.surfaceTexture != nullptr
            && brassDraw.baseColor[0] > brassDraw.baseColor[1]
            && brassDraw.baseColor[1] > brassDraw.baseColor[2]
            && brassDraw.metallic > 0.89f,
        "DX12 Brass Polished must retain its authored pattern, RMAO, and metal colour");

    fh6::LiveryPaintState wheelPaintState;
    fh6::LiveryPaintMaterial highFlakeWheelPaint;
    highFlakeWheelPaint.materialHash = 0x5678u;
    highFlakeWheelPaint.primary.enabled = true;
    highFlakeWheelPaint.primary.bgra = {0, 0, 255, 255};
    highFlakeWheelPaint.secondary.enabled = true;
    highFlakeWheelPaint.secondary.bgra = {255, 0, 0, 255};
    highFlakeWheelPaint.finish = 71;
    wheelPaintState.materials.push_back(highFlakeWheelPaint);
    fh6::CarModel paintedWheelCar;
    paintedWheelCar.sourcePath = QStringLiteral("painted-wheel-test.carbin");
    paintedWheelCar.boundsMin = {-1.0f, -1.0f, -1.0f};
    paintedWheelCar.boundsMax = {1.0f, 1.0f, 1.0f};
    fh6::CarMesh paintedWheelMesh;
    paintedWheelMesh.name = QStringLiteral("wheel_LODS0");
    paintedWheelMesh.sourceModelPath = QStringLiteral(
        "_library/scene/wheels/test.modelbin");
    paintedWheelMesh.materialName = QStringLiteral("rim");
    paintedWheelMesh.paintMaterialHash = highFlakeWheelPaint.materialHash;
    paintedWheelMesh.material = std::make_shared<fh6::ModelMaterial>();
    paintedWheelMesh.material->resolvedFromLibrary = true;
    paintedWheelMesh.positions = {
        {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    paintedWheelMesh.uvChannels.resize(1);
    paintedWheelMesh.uvChannels[0] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    paintedWheelMesh.indices = {0, 1, 2};
    paintedWheelCar.meshes.push_back(std::move(paintedWheelMesh));
    const std::size_t drawsBeforePaintedWheel = scene.draws.size();
    ok &= require(
        paintFinishes.loaded()
            && fh6::appendOriginalShaderGarageCar(
                &scene, std::move(paintedWheelCar), {0.9f, 0.1f, 0.1f}, {},
                nullptr, nullptr, &wheelPaintState, nullptr, &paintFinishes,
                &carError),
        qPrintable(carError));
    const fh6::OriginalShaderGarageDraw &paintedWheelDraw = scene.draws.back();
    ok &= require(
        scene.draws.size() == drawsBeforePaintedWheel + 1
            && paintedWheelDraw.baseColor[2] > paintedWheelDraw.baseColor[0]
            && paintedWheelDraw.secondaryPaintColor[2] > 0.99f
            && paintedWheelDraw.flakeColor[2] > 0.99f
            && std::abs(paintedWheelDraw.flakeCoverage - 0.8f) < 0.0001f
            && std::abs(paintedWheelDraw.flakeRoughness - 0.37f) < 0.0001f
            && std::abs(paintedWheelDraw.metallic - 0.85f) < 0.0001f
            && paintedWheelDraw.glancingFlopEnabled
            && std::abs(paintedWheelDraw.glancingFlopStrength - 0.45f) < 0.0001f,
        "DX12 painted rims must retain the FH6 high-flake finish and wheel colours");

    fh6::CarModel carbonCar;
    carbonCar.sourcePath = QStringLiteral("carbon-test.carbin");
    carbonCar.boundsMin = {0.0f, 0.0f, 0.0f};
    carbonCar.boundsMax = {2.0f, 2.0f, 0.0f};
    fh6::CarMesh carbonMesh;
    carbonMesh.name = QStringLiteral("radiator_carbon_LODS0");
    carbonMesh.materialName = QStringLiteral("carbonfiber");
    carbonMesh.material = std::make_shared<fh6::ModelMaterial>();
    carbonMesh.material->resourcePath = QStringLiteral(
        "Game:/Media/Cars/_library/materials/carbonfiber/carbonfiber.materialbin");
    carbonMesh.material->uTiling = 32.0f;
    carbonMesh.material->vTiling = 32.0f;
    carbonMesh.material->uvOrientationDegrees = 90.0f;
    carbonMesh.material->normalIntensity = 0.8f;
    carbonMesh.material->weaveNormalIntensity = 0.35f;
    carbonMesh.material->clearCoatNormalUTiling = 6.0f;
    carbonMesh.material->clearCoatNormalVTiling = 7.0f;
    carbonMesh.material->sampler = {true, 2, 3, 0};
    auto carbonLayer = std::make_shared<fh6::ModelMaterialTexture>();
    carbonLayer->path = QStringLiteral("car/carbon_layer.swatchbin");
    carbonLayer->image.width = 2;
    carbonLayer->image.height = 2;
    carbonLayer->image.rgba.assign(16, 128);
    fh6::SwatchImage carbonMip;
    carbonMip.width = 1;
    carbonMip.height = 1;
    carbonMip.rgba = {128, 128, 255, 255};
    carbonLayer->authoredMips.push_back(carbonMip);
    carbonMesh.material->weaveMaskTexture = carbonLayer;
    carbonMesh.material->weaveNormalTexture = carbonLayer;
    carbonMesh.material->clearCoatNormalTexture = carbonLayer;
    carbonMesh.positions = {
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    carbonMesh.uvChannels.resize(2);
    carbonMesh.uvChannels[0] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    carbonMesh.uvChannels[1] = {
        {0.25f, 0.5f}, {0.75f, 0.5f}, {0.25f, 0.75f}};
    carbonMesh.indices = {0, 1, 2};
    carbonCar.meshes.push_back(std::move(carbonMesh));
    const std::size_t drawsBeforeCarbon = scene.draws.size();
    ok &= require(
        fh6::appendOriginalShaderGarageCar(
            &scene, std::move(carbonCar), {0.2f, 0.4f, 0.6f}, {},
            nullptr, nullptr, nullptr, nullptr, nullptr, &carError),
        qPrintable(carError));
    ok &= require(
        scene.draws.size() == drawsBeforeCarbon + 1
            && !scene.draws.back().rawMaterialUv
            && scene.draws.back().materialUvChannel == 1
            && std::abs(scene.draws.back().materialUvRotationDegrees - 90.0f) < 0.0001f
            && std::abs(scene.draws.back().uTiling - 32.0f) < 0.0001f
            && std::abs(scene.draws.back().vTiling - 32.0f) < 0.0001f
            && scene.draws.back().shaderFamily == fh6::ModelShaderFamily::CarbonFiber
            && scene.draws.back().weaveMaskTexture != nullptr
            && scene.draws.back().weaveNormalTexture != nullptr
            && scene.draws.back().clearCoatNormalTexture != nullptr
            && scene.draws.back().weaveNormalTexture->authoredMips.size() == 1
            && std::abs(scene.draws.back().normalIntensity - 0.8f) < 0.0001f
            && std::abs(scene.draws.back().weaveNormalIntensity - 0.35f) < 0.0001f
            && scene.draws.back().sampler.authored
            && scene.draws.back().sampler.addressU == 2
            && scene.draws.back().sampler.addressV == 3,
        "DX12 carbon weave must use authored transformed UV1, rotation, and tiling");

    if (ok) {
        std::printf("original shader garage validation passed\n");
    }
    return ok ? 0 : 1;
}
