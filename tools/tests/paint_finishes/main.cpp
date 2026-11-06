#include "material_hashes.h"
#include "car_scene.h"
#include "model_material.h"
#include "paint_finish_catalog.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void checkSamplerDefaults() {
    QByteArray parameters;
    const auto u32 = [&](quint32 value) {
        const int offset = parameters.size();
        parameters.resize(offset + 4);
        qToLittleEndian(value, parameters.data() + offset);
    };
    const auto parameter = [&](quint32 hash, quint8 type) {
        parameters.append('\x03').append('\x04');
        u32(hash);
        parameters.append('\0').append(static_cast<char>(type)).append(QByteArray(16, '\0'));
    };
    parameters.append('\x02').append('\0');
    parameter(123, 7);
    for (quint32 value : {4u, 4u, 3u, 287454020u}) u32(value);
    parameter(fls::material_hashes::parameter::kTextureTilingU, 2);
    const float tiling = 32.0f;
    quint32 bits;
    std::memcpy(&bits, &tiling, sizeof(bits));
    u32(bits);
    QByteArray bundle(44, '\0');
    const auto field = [&](int offset, quint32 value) { qToLittleEndian(value, bundle.data() + offset); };
    field(0, fls::bundle_tags::Bundle);
    bundle[4] = bundle[5] = 1;
    field(16, 1);
    field(20, fls::bundle_tags::DefaultMaterialParameters);
    bundle[24] = 2;
    bundle[25] = 1;
    field(32, bundle.size());
    field(36, parameters.size());
    field(40, parameters.size());
    bundle.append(parameters);
    const auto material = fls::decodeMaterialBundle(bundle);
    require(material && material->parameters.size() == 2 && material->uTiling == tiling,
            "shader sampler extension misaligned the following parameter");
}

void checkWeaveLibrary(const QString &folder) {
    fls::PaintFinishLibrary library;
    library.load(folder);
    require(library.loaded(), "finish library unavailable");
    for (int code : {6, 7, 8, 9}) {
        const auto *finish = library.find(code);
        require(finish && finish->valid && finish->selfColored && finish->uvChannel == 1, "weave shader UV channel lost");
        require(finish->hasWeaveMask() && finish->hasDetailNormal(), "weave texture or normal unavailable");
        require(!finish->hasPattern(), "weave tints were baked into a color texture");
        require(finish->tiling == std::array<float, 2>{32, 32}, "weave tiling lost");
        const auto &pixels = finish->weaveMaskImage.rgba;
        quint8 minimumMask = 255;
        quint8 maximumMask = 0;
        for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
            minimumMask = std::min(minimumMask, pixels[pixel]);
            maximumMask = std::max(maximumMask, pixels[pixel]);
        }
        require(minimumMask == 0 && maximumMask == 255, "weave directions lost from mask");
        require(std::abs(finish->weaveColorTintA[0] - (code == 6 ? 0.009721218f : code == 7 ? 0.0001821162f : 0.054480281f)) < 0.000001f,
                "linear weave tint was converted or replaced");
        require(std::abs(finish->weaveColorTintB[0] - (code < 8 ? 0.009721218f : 0.003676507f)) < 0.000001f,
                "second linear weave tint was converted or replaced");
        require(finish->weaveMaskMips.size() == 9 && finish->detailNormalMips.size() == 9, "authored weave mips lost");
        require(finish->weaveMaskMips.back().width == 1 && finish->detailNormalMips.back().height == 1, "weave mip dimensions invalid");
        require(std::abs(finish->metallic - (code < 8 ? 0.2f : 0.8f)) < 0.0001f, "weave metalness lost");
        require(std::abs(finish->anisotropy - 0.8f) < 0.0001f && finish->normalIntensity == 1.0f, "weave directional shading lost");
        require(std::abs(finish->gloss - (code < 8 ? 0.65f : 0.6f)) < 0.0001f, "weave roughness replaced by coat roughness");
        require(finish->clearCoatCoverage == 1.0f
                    && std::abs(finish->clearCoatRoughness - (code % 2 == 0 ? 0.6f : 0.0f)) < 0.0001f,
                "authored weave coating lost");
        std::printf("finish %d: mask=%dx%d normal=%dx%d tiling=%.0fx%.0f mips=%zu metallic=%.1f UV=%d anisotropy=%.1f roughness=%.2f coat=%.1f\n",
                    code, finish->weaveMaskImage.width, finish->weaveMaskImage.height,
                    finish->detailNormalImage.width, finish->detailNormalImage.height,
                    finish->tiling[0], finish->tiling[1], finish->weaveMaskMips.size() + 1, finish->metallic, finish->uvChannel, finish->anisotropy,
                    1.0f - finish->gloss, finish->clearCoatRoughness);
    }
}

void checkAuthoredTangents(const QString &archive) {
    QString error;
    const auto extracted = fls::sharedZipExtraction(archive, &error);
    require(extracted != nullptr, qPrintable(error));
    QDirIterator files(extracted->path(), QStringList{QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    require(files.hasNext(), "model archive has no carbin");
    const auto model = fls::loadCarBin(files.next(), &error);
    require(!model.meshes.empty(), qPrintable(error));
    size_t authoredVertices = 0;
    for (const auto &mesh : model.meshes) {
        if (mesh.tangentChannels.size() < 2 || mesh.tangentChannels[1].empty()) continue;
        require(mesh.tangentChannels[1].size() == mesh.positions.size(), "UV1 tangent count mismatch");
        for (const auto &tangent : mesh.tangentChannels[1]) {
            const float length = tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z;
            require(std::isfinite(length) && std::abs(length - 1.0f) < 0.01f, "decoded tangent is not a unit direction");
            require(std::isfinite(tangent.w) && std::abs(std::abs(tangent.w) - 1.0f) < 0.0001f,
                    "tangent handedness is not a unit sign");
        }
        authoredVertices += mesh.tangentChannels[1].size();
    }
    require(authoredVertices > 0, "no authored UV1 tangents decoded");
    std::printf("Authored UV1 tangents checked: %zu vertices\n", authoredVertices);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        checkSamplerDefaults();
        if (argc >= 2) checkWeaveLibrary(QString::fromLocal8Bit(argv[1]));
        if (argc >= 3) checkAuthoredTangents(QString::fromLocal8Bit(argv[2]));
        std::puts("Paint finish checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
