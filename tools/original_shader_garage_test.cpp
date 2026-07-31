#include "original_shader_garage.h"

#include <QCoreApplication>

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

    const fh6::OriginalShaderGarageScene scene =
        fh6::loadOriginalShaderGarageScene(QString::fromLocal8Bit(argv[1]));
    if (!require(scene.valid(), qPrintable(scene.error))) {
        return 1;
    }
    ok &= require(scene.name == QStringLiteral("Tokyo House"), "scene identity changed");
    ok &= require(scene.draws.size() == 12, "scene must contain twelve Tokyo material draws");
    ok &= require(scene.totalVertices() == 16652, "Tokyo House vertex count changed");
    ok &= require(scene.totalTriangles() == 13179, "Tokyo House triangle count changed");
    ok &= require(scene.defaultProgram.valid(), "default DXIL pair is invalid");
    ok &= require(scene.floorProgram.valid(), "floor DXIL pair is invalid");
    ok &= require(scene.environment.valid(), "garage lighting resources are invalid");
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
        scene.materialStatus.contains(QStringLiteral("12/12")),
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
                && draw.diffuseTexture->sourceEntry.contains(
                    QStringLiteral("_bclr_"), Qt::CaseInsensitive),
            "Tokyo draw diffuse is not an authored base-colour swatch");
    }

    if (ok) {
        std::printf(
            "original shader garage: %lld vertices, %lld triangles, %zu draws\n",
            scene.totalVertices(), scene.totalTriangles(), scene.draws.size());
    }
    return ok ? 0 : 1;
}
