#include "original_dx12_backend.h"
#include "original_shader_garage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSet>

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

    const fh6::OriginalShaderGarageScene scene =
        fh6::loadOriginalShaderGarageScene(QString::fromLocal8Bit(argv[1]));
    if (!scene.valid()) {
        std::fprintf(stderr, "scene load failed: %s\n", qPrintable(scene.error));
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
        || sceneCamera.farPlane <= 100.0f) {
        std::fprintf(stderr, "D3D12 Tokyo House camera is invalid\n");
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
    // The Tokyo compatibility material intentionally omits the unresolved specular
    // cubemap, so variation now comes from diffuse irradiance and surface normals
    // rather than a reflected HDR image.
    if (colors.size() < 128) {
        std::fprintf(
            stderr,
            "D3D12 perspective frame has insufficient color variation: %lld colors\n",
            static_cast<long long>(colors.size()));
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
    viewportCamera.position.setX(viewportCamera.position.x() + 0.25f);
    const bool secondPresent = firstPresent && viewport.render(viewportCamera);
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
        "D3D12 exact frame on %s: changed=%llu colors=%lld hash=%s warnings=%d\n",
        qPrintable(firstFrame.adapter),
        static_cast<unsigned long long>(firstFrame.changedPixels),
        static_cast<long long>(colors.size()), firstHash.constData(),
        firstFrame.debugWarnings);
    return 0;
}
