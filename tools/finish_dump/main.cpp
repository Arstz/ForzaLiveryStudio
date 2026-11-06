// Dev-only harness: decodes each livery paint "finish" material from a Forza install and
// prints the parameters the preview resolves it to, so a mis-shaded finish (e.g. a gold
// rim rendering green) can be traced to the raw materialbin values.
#include "model_material.h"
#include "car_scene.h"
#include "material_hashes.h"
#include "paint_finish_catalog.h"
#include "zip_extract.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace {

const char *categoryName(fls::PaintFinishCategory category) {
    switch (category) {
    case fls::PaintFinishCategory::Solid:
        return "Solid";
    case fls::PaintFinishCategory::TwoTone:
        return "TwoTone";
    case fls::PaintFinishCategory::Metal:
        return "Metal";
    case fls::PaintFinishCategory::Pattern:
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
    const std::shared_ptr<fls::ModelMaterial> material = fls::decodeMaterialBundle(file.readAll());
    if (!material) {
        std::fprintf(stderr, "decode failed for %s\n", path.toUtf8().constData());
        return;
    }
    std::printf("%s\n", path.toUtf8().constData());
    std::printf("  hasBaseColor=%d baseColor=(%.4f, %.4f, %.4f) gloss=%.3f hasMetallic=%d metallic=%.3f flake=%.3f\n",
                material->hasBaseColor ? 1 : 0, material->baseColor[0], material->baseColor[1],
                material->baseColor[2], material->gloss, material->hasMetallic ? 1 : 0,
                material->metallic, material->flakeAmount);
    std::printf("  pattern=%s\n  normal=%s\n  surface=%s\n",
                material->patternTexture.toUtf8().constData(),
                material->detailNormalTexture.toUtf8().constData(),
                material->roughMetalAoTexture.toUtf8().constData());
    for (const fls::ModelMaterialParameter &parameter : material->parameters) {
        std::printf("    param hash=%08X type=%d scalar=%.4f vec=(%.4f, %.4f, %.4f, %.4f) tex=%s\n",
                    parameter.nameHash, static_cast<int>(parameter.type), parameter.scalar,
                    parameter.vector[0], parameter.vector[1], parameter.vector[2],
                    parameter.vector[3], parameter.texturePath.toUtf8().constData());
    }
}

void dumpLibrary(const QString &gameFolder) {
    fls::PaintFinishLibrary library;
    library.load(gameFolder);
    std::printf("finish library loaded=%d folder=%s\n", library.loaded() ? 1 : 0,
                library.folder().toUtf8().constData());
    for (const fls::PaintFinishInfo &info : fls::paintFinishTable()) {
        const fls::PaintFinishRender *render = library.find(info.code);
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

void dumpSwatch(const QString &path) {
    QString error;
    const auto image = fls::loadSwatchImage(path, &error);
    std::printf("swatch %dx%d %s\n", image.width, image.height, qPrintable(error));
    if (!image.valid()) return;
    for (int channel = 0; channel < 4; ++channel) {
        int minimum = 255;
        int maximum = 0;
        double sum = 0;
        for (size_t pixel = channel; pixel < image.rgba.size(); pixel += 4) {
            const int value = image.rgba[pixel];
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
            sum += value;
        }
        std::printf("  channel %d min=%d max=%d mean=%.3f\n", channel, minimum, maximum, sum / (image.width * image.height));
    }
    std::printf("  samples:");
    for (int x = 0; x < std::min(image.width, 24); ++x) {
        const size_t pixel = (static_cast<size_t>(image.height / 2) * image.width + x) * 4;
        std::printf(" (%d,%d,%d,%d)", image.rgba[pixel], image.rgba[pixel + 1], image.rgba[pixel + 2], image.rgba[pixel + 3]);
    }
    std::printf("\n");
}

void dumpPaintGeometry(const QString &path) {
    QString error;
    const auto extracted = fls::sharedZipExtraction(path, &error);
    if (!extracted) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return;
    }
    QDirIterator files(extracted->path(), QStringList{QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    if (!files.hasNext()) return;
    const auto model = fls::loadCarBin(files.next(), &error);
    const auto inspect = [](const auto &meshes) {
        for (const auto &mesh : meshes) {
            if (mesh.carPartType != fls::car_part_types::RearWing
                && mesh.paintMaterialHash != fls::material_hashes::binding::kSpoilerPaint
                && !mesh.name.contains("spoiler", Qt::CaseInsensitive)) continue;
            std::printf("mesh=%s material=%s part=%d stock=%d hash=%llu vertices=%zu\n",
                        qPrintable(mesh.name), qPrintable(mesh.materialName), mesh.carPartType, mesh.stockPart,
                        mesh.paintMaterialHash, mesh.positions.size());
            for (size_t channel = 0; channel < std::min(mesh.uvChannels.size(), mesh.texCoordTransforms.size()); ++channel) {
                const auto &uvs = mesh.uvChannels[channel];
                if (uvs.empty()) continue;
                const auto &transform = mesh.texCoordTransforms[channel];
                float u0 = INFINITY, u1 = -INFINITY, v0 = INFINITY, v1 = -INFINITY;
                for (const auto &uv : uvs) {
                    float u = uv.u * transform.scaleU + transform.offsetU;
                    float v = (1 - uv.v) * transform.scaleV + transform.offsetV;
                    u0 = std::min(u0, u); u1 = std::max(u1, u);
                    v0 = std::min(v0, v); v1 = std::max(v1, v);
                }
                double area = 0, worldArea = 0;
                for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                    const auto a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
                    if (a >= uvs.size() || b >= uvs.size() || c >= uvs.size()) continue;
                    area += std::abs((uvs[b].u - uvs[a].u) * (uvs[c].v - uvs[a].v)
                                  - (uvs[b].v - uvs[a].v) * (uvs[c].u - uvs[a].u))
                          * std::abs(transform.scaleU * transform.scaleV) * 0.5;
                    const auto p = mesh.boneTransform.transformPoint(mesh.positions[a]);
                    const auto q = mesh.boneTransform.transformPoint(mesh.positions[b]);
                    const auto r = mesh.boneTransform.transformPoint(mesh.positions[c]);
                    const double x = (q.y-p.y)*(r.z-p.z)-(q.z-p.z)*(r.y-p.y);
                    const double y = (q.z-p.z)*(r.x-p.x)-(q.x-p.x)*(r.z-p.z);
                    const double z = (q.x-p.x)*(r.y-p.y)-(q.y-p.y)*(r.x-p.x);
                    worldArea += std::sqrt(x*x+y*y+z*z)*0.5;
                }
                float w0 = INFINITY, w1 = -INFINITY;
                if (channel < mesh.tangentChannels.size()) for (const auto &tangent : mesh.tangentChannels[channel]) {
                    w0 = std::min(w0, tangent.w); w1 = std::max(w1, tangent.w);
                }
                std::printf(" UV%zu range=(%.5f,%.5f)-(%.5f,%.5f) area=%.6f worldArea=%.6f texels/world=%.3f tangentW=(%.3f,%.3f)\n",
                            channel,u0,v0,u1,v1,area,worldArea,worldArea>0?std::sqrt(area/worldArea)*512*32:0,w0,w1);
            }
        }
    };
    inspect(model.meshes);
    inspect(model.variantMeshes);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == QStringLiteral("--paint-geometry")) {
        dumpPaintGeometry(args[2]);
        return 0;
    }
    if (args.size() >= 3 && args[1] == QStringLiteral("--swatch")) {
        dumpSwatch(args[2]);
        return 0;
    }
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
                 "  fls_finish_dump --library <gameFolder>\n"
                 "  fls_finish_dump --material <file.materialbin>\n"
                 "  fls_finish_dump --swatch <file.swatchbin>\n"
                 "  fls_finish_dump --paint-geometry <file.zip>\n");
    return 2;
}
