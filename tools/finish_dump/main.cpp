// Dev-only harness: decodes each livery paint "finish" material from a Forza install and
// prints the parameters the preview resolves it to, so a mis-shaded finish (e.g. a gold
// rim rendering green) can be traced to the raw materialbin values.
#include "model_material.h"
#include "paint_finish_catalog.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

const char *categoryName(fh6::PaintFinishCategory category) {
    switch (category) {
    case fh6::PaintFinishCategory::Solid:
        return "Solid";
    case fh6::PaintFinishCategory::TwoTone:
        return "TwoTone";
    case fh6::PaintFinishCategory::Metal:
        return "Metal";
    case fh6::PaintFinishCategory::Pattern:
        return "Pattern";
    }
    return "?";
}

void dumpRawMaterial(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", path.toUtf8().constData());
        return;
    }
    const std::shared_ptr<fh6::ModelMaterial> material = fh6::decodeMaterialBundle(file.readAll());
    if (!material) {
        std::fprintf(stderr, "decode failed for %s\n", path.toUtf8().constData());
        return;
    }
    std::printf("%s\n", path.toUtf8().constData());
    std::printf("  resource=%s\n", material->resourcePath.toUtf8().constData());
    for (const QString &linkedPath : material->linkedPaths) {
        std::printf("  linked=%s\n", linkedPath.toUtf8().constData());
    }
    std::printf("  hasBaseColor=%d baseColor=(%.4f, %.4f, %.4f) gloss=%.3f hasMetallic=%d metallic=%.3f flake=%.3f\n",
                material->hasBaseColor ? 1 : 0, material->baseColor[0], material->baseColor[1],
                material->baseColor[2], material->gloss, material->hasMetallic ? 1 : 0,
                material->metallic, material->flakeAmount);
    std::printf("  pattern=%s\n  normal=%s\n  surface=%s\n",
                material->patternTexture.toUtf8().constData(),
                material->detailNormalTexture.toUtf8().constData(),
                material->roughMetalAoTexture.toUtf8().constData());
    for (const fh6::ModelMaterialParameter &parameter : material->parameters) {
        std::printf("    param hash=%08X type=%d scalar=%.4f vec=(%.4f, %.4f, %.4f, %.4f) tex=%s\n",
                    parameter.nameHash, static_cast<int>(parameter.type), parameter.scalar,
                    parameter.vector[0], parameter.vector[1], parameter.vector[2],
                    parameter.vector[3], parameter.texturePath.toUtf8().constData());
    }
}

void dumpLibrary(const QString &gameFolder) {
    fh6::PaintFinishLibrary library;
    library.load(gameFolder);
    std::printf("finish library loaded=%d folder=%s\n", library.loaded() ? 1 : 0,
                library.folder().toUtf8().constData());
    for (const fh6::PaintFinishInfo &info : fh6::paintFinishTable()) {
        const fh6::PaintFinishRender *render = library.find(info.code);
        if (render == nullptr) {
            std::printf("  [%3d] %-28s %-8s UNRESOLVED (%s.materialbin)\n", info.code,
                        info.displayName.toUtf8().constData(), categoryName(info.category),
                        info.materialBase.toUtf8().constData());
            continue;
        }
        std::printf("  [%3d] %-28s %-8s valid=%d hasMatColor=%d color=(%.3f, %.3f, %.3f) gloss=%.3f "
                    "metallic=%.3f flake=%.3f selfColored=%d usesSecondary=%d pat=%d nrm=%d srf=%d\n",
                    info.code, info.displayName.toUtf8().constData(), categoryName(render->category),
                    render->valid ? 1 : 0, render->hasMaterialColor ? 1 : 0, render->materialColor[0],
                    render->materialColor[1], render->materialColor[2], render->gloss, render->metallic,
                    render->flakeAmount, render->selfColored ? 1 : 0, render->usesSecondary ? 1 : 0,
                    render->hasPattern() ? 1 : 0, render->hasDetailNormal() ? 1 : 0,
                    render->hasRoughMetalAo() ? 1 : 0);
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == QStringLiteral("--material")) {
        dumpRawMaterial(args[2]);
        return 0;
    }
    if (args.size() >= 3 && args[1] == QStringLiteral("--library")) {
        dumpLibrary(args[2]);
        return 0;
    }
    std::fprintf(stderr,
                 "usage:\n"
                 "  fh6_finish_dump --library <gameFolder>\n"
                 "  fh6_finish_dump --material <file.materialbin>\n");
    return 2;
}
