#include "original_dx12_backend.h"
#include "original_shader_garage.h"
#include "model_material.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: fls_original_dx12_backend_tests <game-root>\n");
        return 2;
    }

    fh6::OriginalShaderGarageScene scene =
        fh6::loadOriginalShaderGarageScene(QString::fromLocal8Bit(argv[1]));
    if (!scene.valid()) {
        std::fprintf(stderr, "scene load failed: %s\n", qPrintable(scene.error));
        return 1;
    }
    if (qEnvironmentVariableIsSet("FLS_DX12_HIDE_HOMESPACE_SHELL")) {
        std::erase_if(scene.draws, [](const fh6::OriginalShaderGarageDraw &draw) {
            return draw.source.contains(
                QStringLiteral("whitebox\\homespace"), Qt::CaseInsensitive)
                || draw.source.contains(
                    QStringLiteral("whitebox/homespace"), Qt::CaseInsensitive)
                || draw.source.contains(
                    QStringLiteral("garage_customiser"), Qt::CaseInsensitive);
        });
    }
    const gui::OriginalDx12Camera paintTestCamera =
        gui::originalDx12SceneCamera(scene);
    const fh6::ModelVec3 placedPaintTestPosition =
        scene.carPlacement.transformPoint({0.0f, 0.3f, 0.0f});
    const QVector3D paintTestPosition(
        placedPaintTestPosition.x, placedPaintTestPosition.y,
        placedPaintTestPosition.z);
    const QVector3D paintTestView =
        (paintTestCamera.position - paintTestPosition).normalized();
    const QVector3D paintTestLight = QVector3D(
        -scene.lighting.direction.x, -scene.lighting.direction.y,
        -scene.lighting.direction.z).normalized();
    const QVector3D paintTestNormal =
        (paintTestView + paintTestLight).normalized();
    fh6::CarModel car;
    car.sourcePath = QStringLiteral("test.carbin");
    car.boundsMin = {-1.0f, 0.0f, -2.0f};
    car.boundsMax = {1.0f, 1.0f, 2.0f};
    fh6::CarMesh carMesh;
    carMesh.materialName = QStringLiteral("carpaint_standard");
    carMesh.paintMaterialHash = 1;
    carMesh.liveryUvChannel = 3;
    carMesh.material = std::make_shared<fh6::ModelMaterial>();
    carMesh.material->automotivePaint.hasFlakeColor = true;
    carMesh.material->automotivePaint.flakeColor = {0.35f, 0.75f, 1.0f, 1.0f};
    carMesh.material->automotivePaint.hasFlakeCoverage = true;
    carMesh.material->automotivePaint.flakeCoverage = 1.0f;
    carMesh.material->automotivePaint.hasFlakeRoughness = true;
    carMesh.material->automotivePaint.flakeRoughness = 1.0f;
    carMesh.material->automotivePaint.hasGlitterIntensity = true;
    carMesh.material->automotivePaint.glitterIntensity = 4.0f;
    carMesh.positions = {
        {-0.5f, 0.3f, -0.5f}, {0.5f, 0.3f, -0.5f}, {0.0f, 0.3f, 0.5f},
        {-1.0f, 0.0f, 0.4f}, {1.0f, 0.0f, 0.4f}, {0.0f, 0.0f, 1.4f}};
    carMesh.normals = {
        {paintTestNormal.x(), paintTestNormal.y(), paintTestNormal.z()},
        {paintTestNormal.x(), paintTestNormal.y(), paintTestNormal.z()},
        {paintTestNormal.x(), paintTestNormal.y(), paintTestNormal.z()},
        {paintTestNormal.x(), paintTestNormal.y(), paintTestNormal.z()},
        {paintTestNormal.x(), paintTestNormal.y(), paintTestNormal.z()},
        {paintTestNormal.x(), paintTestNormal.y(), paintTestNormal.z()}};
    carMesh.uvChannels.resize(4);
    carMesh.uvChannels[0] = {
        {4.0f, 4.0f}, {4.0f, 4.0f}, {4.0f, 4.0f},
        {4.0f, 4.0f}, {4.0f, 4.0f}, {4.0f, 4.0f}};
    carMesh.uvChannels[3] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f},
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    carMesh.indices = {0, 1, 2, 3, 4, 5};
    car.meshes.push_back(std::move(carMesh));
    fh6::SwatchImage livery;
    livery.width = 2;
    livery.height = 2;
    livery.rgba = {
        255, 24, 16, 255, 255, 24, 16, 255,
        255, 24, 16, 255, 255, 24, 16, 255};
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
    if (!fh6::appendOriginalShaderGarageCar(
            &scene, std::move(car), {0.2f, 0.4f, 0.6f}, livery,
            &liveryMasks, nullptr, nullptr, nullptr, nullptr, &carError)) {
        std::fprintf(stderr, "DX12 car setup failed: %s\n", qPrintable(carError));
        return 1;
    }
    const gui::OriginalDx12BackendStatus status =
        gui::probeOriginalDx12Backend(scene);
    if (!status.available || !status.exactPipelinesCreated) {
        std::fprintf(stderr, "D3D12 probe failed: %s\n", qPrintable(status.error));
        return 1;
    }
    const gui::OriginalDx12Camera sceneCamera =
        gui::originalDx12SceneCamera(scene);
    if (!sceneCamera.valid()
        || std::abs(sceneCamera.verticalFovDegrees - 40.0f) > 0.00001f
        || sceneCamera.farPlane < 50.0f) {
        std::fprintf(stderr, "D3D12 Tokyo House camera is invalid\n");
        return 1;
    }
    gui::OriginalDx12Camera pannedCamera = sceneCamera;
    const QVector3D originalOffset = pannedCamera.target - pannedCamera.position;
    gui::panOriginalDx12Camera(
        &pannedCamera, QPointF(80.0, -30.0), QSize(800, 600));
    if ((pannedCamera.target - sceneCamera.target).isNull()
        || ((pannedCamera.target - pannedCamera.position) - originalOffset).length()
            > 0.0001f) {
        std::fprintf(stderr, "D3D12 camera panning did not preserve the view vector\n");
        return 1;
    }
    constexpr int kFrameSize = 512;
    const gui::OriginalDx12FrameResult firstFrame =
        gui::renderOriginalDx12GarageFrame(scene, QSize(kFrameSize, kFrameSize));
    const gui::OriginalDx12FrameResult secondFrame =
        gui::renderOriginalDx12GarageFrame(scene, QSize(kFrameSize, kFrameSize));
    if (!firstFrame.valid() || !secondFrame.valid()) {
        std::fprintf(
            stderr, "D3D12 frame failed: %s / %s\n",
            qPrintable(firstFrame.error), qPrintable(secondFrame.error));
        return 1;
    }
    if (firstFrame.shadowMapPixels == 0
        || secondFrame.shadowMapPixels == 0) {
        std::fprintf(stderr, "D3D12 car shadow map is empty\n");
        return 1;
    }
    if (firstFrame.debugWarnings != 0 || secondFrame.debugWarnings != 0) {
        std::fprintf(
            stderr, "D3D12 debug warning: %s / %s\n",
            qPrintable(firstFrame.debugWarningDetail),
            qPrintable(secondFrame.debugWarningDetail));
        return 1;
    }
    const QByteArray firstBytes(
        reinterpret_cast<const char *>(firstFrame.image.constBits()),
        firstFrame.image.sizeInBytes());
    const QByteArray secondBytes(
        reinterpret_cast<const char *>(secondFrame.image.constBits()),
        secondFrame.image.sizeInBytes());
    const QByteArray firstHash =
        QCryptographicHash::hash(firstBytes, QCryptographicHash::Sha256).toHex();
    const QByteArray secondHash =
        QCryptographicHash::hash(secondBytes, QCryptographicHash::Sha256).toHex();
    const QString framePath = qEnvironmentVariable("FLS_DX12_FRAME_PATH");
    if (!framePath.isEmpty() && !firstFrame.image.save(framePath)) {
        std::fprintf(stderr, "could not save D3D12 diagnostic frame\n");
        return 1;
    }
    QSet<quint32> colors;
    const auto *pixels = reinterpret_cast<const quint32 *>(firstFrame.image.constBits());
    const qsizetype pixelCount = firstFrame.image.sizeInBytes() / sizeof(quint32);
    for (qsizetype index = 0; index < pixelCount; ++index) {
        colors.insert(pixels[index]);
    }
    if (firstHash != secondHash
        || firstFrame.changedPixels != secondFrame.changedPixels) {
        std::fprintf(stderr, "D3D12 frame is not deterministic\n");
        return 1;
    }
    fh6::OriginalShaderGarageScene unshadowedScene = scene;
    for (fh6::OriginalShaderGarageDraw &draw : unshadowedScene.draws) {
        draw.family = fh6::OriginalShaderSurfaceFamily::Default;
    }
    fh6::OriginalShaderGarageScene carSelfShadowScene = scene;
    for (fh6::OriginalShaderGarageDraw &draw : carSelfShadowScene.draws) {
        if (draw.family == fh6::OriginalShaderSurfaceFamily::Floor) {
            draw.family = fh6::OriginalShaderSurfaceFamily::Default;
        }
    }
    const gui::OriginalDx12FrameResult unshadowedFrame =
        gui::renderOriginalDx12GarageFrame(
            unshadowedScene, QSize(kFrameSize, kFrameSize));
    const gui::OriginalDx12FrameResult carSelfShadowFrame =
        gui::renderOriginalDx12GarageFrame(
            carSelfShadowScene, QSize(kFrameSize, kFrameSize));
    fh6::OriginalShaderGarageScene noFlakeScene = scene;
    for (fh6::OriginalShaderGarageDraw &draw : noFlakeScene.draws) {
        if (draw.family == fh6::OriginalShaderSurfaceFamily::Car) {
            draw.flakeCoverage = 0.0f;
        }
    }
    const gui::OriginalDx12FrameResult noFlakeFrame =
        gui::renderOriginalDx12GarageFrame(
            noFlakeScene, QSize(kFrameSize, kFrameSize));
    if (!unshadowedFrame.valid() || !carSelfShadowFrame.valid()
        || !noFlakeFrame.valid()) {
        std::fprintf(
            stderr, "D3D12 material/shadow reference frame failed: %s / %s / %s\n",
            qPrintable(unshadowedFrame.error),
            qPrintable(carSelfShadowFrame.error), qPrintable(noFlakeFrame.error));
        return 1;
    }
    const auto *unshadowedPixels = reinterpret_cast<const quint32 *>(
        unshadowedFrame.image.constBits());
    const auto *carSelfShadowPixels = reinterpret_cast<const quint32 *>(
        carSelfShadowFrame.image.constBits());
    const auto *noFlakePixels = reinterpret_cast<const quint32 *>(
        noFlakeFrame.image.constBits());
    quint64 shadowAffectedPixels = 0;
    quint64 selfShadowAffectedPixels = 0;
    quint64 flakeAffectedPixels = 0;
    for (qsizetype index = 0; index < pixelCount; ++index) {
        shadowAffectedPixels += pixels[index] != unshadowedPixels[index] ? 1u : 0u;
        selfShadowAffectedPixels +=
            carSelfShadowPixels[index] != unshadowedPixels[index] ? 1u : 0u;
        flakeAffectedPixels += pixels[index] != noFlakePixels[index] ? 1u : 0u;
    }
    if (shadowAffectedPixels == 0 || selfShadowAffectedPixels == 0
        || flakeAffectedPixels == 0) {
        std::fprintf(
            stderr,
            "D3D12 car shadow did not affect visible receivers "
            "(map=%llu, all=%llu, self=%llu, flake=%llu pixels)\n",
            static_cast<unsigned long long>(firstFrame.shadowMapPixels),
            static_cast<unsigned long long>(shadowAffectedPixels),
            static_cast<unsigned long long>(selfShadowAffectedPixels),
            static_cast<unsigned long long>(flakeAffectedPixels));
        return 1;
    }
    // The complete environment contract contributes diffuse irradiance, the
    // mipmapped BC6H reflection probe, and the Homespace colour grade.
    if (colors.size() < 64) {
        std::fprintf(
            stderr,
            "D3D12 perspective frame has insufficient color variation: %lld colors\n",
            static_cast<long long>(colors.size()));
        return 1;
    }
    const bool boundCompositedLivery = std::any_of(
        scene.draws.cbegin(), scene.draws.cend(), [](const auto &draw) {
            return draw.liveryBaseTexture && draw.diffuseTexture != nullptr
                && draw.diffuseTexture->sourceEntry
                    == QStringLiteral("car://composited-livery");
        });
    if (!boundCompositedLivery) {
        std::fprintf(stderr, "D3D12 scene did not bind the composited livery atlas\n");
        return 1;
    }
#ifdef Q_OS_WIN
    const HWND window = CreateWindowExW(
        0, L"STATIC", L"FLS D3D12 viewport test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "D3D12 viewport test window creation failed\n");
        return 1;
    }
    gui::OriginalDx12ViewportRenderer viewport;
    gui::OriginalDx12Camera viewportCamera = sceneCamera;
    const bool initialized = viewport.initialize(
        scene, reinterpret_cast<quintptr>(window), QSize(320, 240),
        viewportCamera);
    const bool firstPresent = initialized && viewport.render(viewportCamera);
    fh6::SwatchImage replacementLivery;
    replacementLivery.width = 1;
    replacementLivery.height = 1;
    replacementLivery.rgba = {0, 0, 255, 255};
    const bool liveLiveryUpdated = firstPresent
        && viewport.updateLivery(replacementLivery);
    viewportCamera.position.setX(viewportCamera.position.x() + 0.25f);
    const bool secondPresent = liveLiveryUpdated && viewport.render(viewportCamera);
    const bool resized = secondPresent && viewport.resize(QSize(400, 300));
    const bool resizedPresent = resized && viewport.render(viewportCamera);
    if (!resizedPresent) {
        std::fprintf(
            stderr, "D3D12 viewport failed: %s\n", qPrintable(viewport.error()));
        viewport.release();
        DestroyWindow(window);
        return 1;
    }
    viewport.release();
    DestroyWindow(window);
#endif
    std::printf(
        "D3D12 exact frame on %s: changed=%llu shadowed=%llu self-shadowed=%llu flake=%llu colors=%lld hash=%s warnings=%d first=%s\n",
        qPrintable(firstFrame.adapter),
        static_cast<unsigned long long>(firstFrame.changedPixels),
        static_cast<unsigned long long>(shadowAffectedPixels),
        static_cast<unsigned long long>(selfShadowAffectedPixels),
        static_cast<unsigned long long>(flakeAffectedPixels),
        static_cast<long long>(colors.size()), firstHash.constData(),
        firstFrame.debugWarnings, qPrintable(firstFrame.debugWarningDetail));
    return 0;
}
