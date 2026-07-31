#include "original_shader_garage.h"
#include "model_material.h"

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
    car.meshes.push_back(std::move(carMesh));
    car.meshes.push_back(std::move(lowerLodMesh));
    car.meshes.push_back(std::move(factoryStickerMesh));
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
    ok &= require(
        scene.draws.size() == garageDraws + 1
            && scene.draws.back().diffuseTexture != nullptr
            && scene.draws.back().diffuseTexture->sourceEntry
                == QStringLiteral("car://composited-livery")
            && scene.draws.back().normalTexture != nullptr
            && scene.draws.back().surfaceTexture != nullptr
            && scene.draws.back().diffuseUvChannel == 3
            && scene.draws.back().baseColor
                == std::array<float, 3>{0.2f, 0.4f, 0.6f}
            && scene.draws.back().liveryBaseTexture
            && scene.draws.back().liveryAllowedSides == 0x1fu
            && scene.liveryMapping.valid()
            && scene.draws.back().uTiling == 1.0f
            && scene.draws.back().vTiling == 1.0f
            && scene.draws.back().detailUTiling == 2.0f
            && scene.draws.back().detailVTiling == 3.0f
            && std::abs(scene.draws.back().clearCoatCoverage - 0.72f) < 0.00001f
            && std::abs(scene.draws.back().clearCoatRoughness - 0.08f) < 0.00001f
            && scene.draws.back().clearCoatTint
                == std::array<float, 3>{0.9f, 0.95f, 1.0f}
            && scene.carStatus.contains(QStringLiteral("1 lower-LOD"))
            && scene.carStatus.contains(QStringLiteral("1 factory-livery sticker")),
        "DX12 car livery/detail maps or factory-sticker exclusion changed");

    fh6::CarModel glassCar;
    glassCar.sourcePath = QStringLiteral("glass-test.carbin");
    glassCar.boundsMin = {-1.0f, 0.0f, -1.0f};
    glassCar.boundsMax = {1.0f, 1.0f, 1.0f};
    fh6::CarMesh glassMesh;
    glassMesh.name = QStringLiteral("window_front_LODS0");
    glassMesh.materialName = QStringLiteral("windowglass");
    glassMesh.material = std::make_shared<fh6::ModelMaterial>();
    glassMesh.material->resourcePath = QStringLiteral(
        "Game:/Media/Cars/_library/materials/glass/windowglass.materialbin");
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
    glassCar.meshes.push_back(std::move(glassMesh));
    const std::size_t drawsBeforeGlass = scene.draws.size();
    ok &= require(
        fh6::appendOriginalShaderGarageCar(
            &scene, std::move(glassCar), {0.2f, 0.4f, 0.6f}, {},
            nullptr, nullptr, nullptr, nullptr, nullptr, &carError),
        qPrintable(carError));
    ok &= require(
        scene.draws.size() == drawsBeforeGlass + 1
            && scene.draws.back().translucent
            && scene.draws.back().alphaTexture != nullptr
            && std::abs(scene.draws.back().opacity - 0.42f) < 0.00001f
            && scene.carStatus.contains(QStringLiteral("1 translucent/glass")),
        "DX12 car glass must be retained in the translucent alpha-map pass");

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
