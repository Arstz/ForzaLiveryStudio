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
    ok &= require(scene.draws.size() == 4, "scene must contain four supported draws");
    ok &= require(scene.totalVertices() == 53417, "supported scene vertex count changed");
    ok &= require(scene.totalTriangles() == 59753, "supported scene triangle count changed");
    ok &= require(scene.defaultProgram.valid(), "default DXIL pair is invalid");
    ok &= require(scene.floorProgram.valid(), "floor DXIL pair is invalid");
    ok &= require(scene.environment.valid(), "garage lighting resources are invalid");
    ok &= require(
        scene.glassStatus.contains(QStringLiteral("excluded")),
        "glass ambiguity must remain explicit");
    for (const auto &texture : scene.materialTextures) {
        ok &= require(texture.valid(), "material fallback texture is invalid");
    }

    if (ok) {
        std::printf(
            "original shader garage: %lld vertices, %lld triangles, %zu draws\n",
            scene.totalVertices(), scene.totalTriangles(), scene.draws.size());
    }
    return ok ? 0 : 1;
}
