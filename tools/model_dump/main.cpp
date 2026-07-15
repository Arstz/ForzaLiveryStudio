#include "car_scene.h"
#include "livery_masks.h"
#include "model_geometry.h"
#include "model_material.h"
#include "swatchbin.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>

using namespace fh6;

static bool isPaintMaterial(const QString &material)
{
    const QString mat = material.toLower();
    return mat.startsWith(QStringLiteral("carpaint"))
        && !mat.contains(QStringLiteral("caliper"))
        && !mat.contains(QStringLiteral("texture"));
}

static QString findCarbin(const QString &root)
{
    QDirIterator it(root, QStringList{QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        return it.next();
    }
    return {};
}

static bool resolveZipInput(QString &path, std::unique_ptr<QTemporaryDir> &tempDir)
{
    if (!path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        return true;
    }
    tempDir = std::make_unique<QTemporaryDir>();
    if (!tempDir->isValid()) {
        std::fprintf(stderr, "failed to create temporary directory for %s\n", qPrintable(path));
        return false;
    }
    QString error;
    if (!extractZipArchive(path, tempDir->path(), &error)) {
        std::fprintf(stderr, "failed to extract %s: %s\n", qPrintable(path), qPrintable(error));
        return false;
    }
    const QString carbin = findCarbin(tempDir->path());
    if (carbin.isEmpty()) {
        std::fprintf(stderr, "zip contains no .carbin: %s\n", qPrintable(path));
        return false;
    }
    path = carbin;
    return true;
}

static float maskSample(const SwatchMask &mask, const ModelVec2 &uv)
{
    if (!mask.valid() || uv.u < 0.0f || uv.u > 1.0f || uv.v < 0.0f || uv.v > 1.0f) {
        return 0.0f;
    }
    const int x = std::clamp(static_cast<int>(std::round(uv.u * static_cast<float>(mask.width - 1))),
                             0, mask.width - 1);
    const int y = std::clamp(static_cast<int>(std::round(uv.v * static_cast<float>(mask.height - 1))),
                             0, mask.height - 1);
    return static_cast<float>(mask.at(x, y));
}

static ModelVec2 transformedUv(const CarMesh &mesh, int channel, const ModelVec2 &uv)
{
    if (channel < 0 || channel >= static_cast<int>(mesh.texCoordTransforms.size())) {
        return uv;
    }
    const TexCoordTransform &transform = mesh.texCoordTransforms[channel];
    return {uv.u * transform.scaleU + transform.offsetU,
            uv.v * transform.scaleV + transform.offsetV};
}

static int normalSide(const CarMesh &mesh, size_t vertex)
{
    const float facing[5][3] = {
        {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    if (!mesh.name.contains(QStringLiteral("mirror"), Qt::CaseInsensitive)
        && (mesh.name.contains(QStringLiteral("spoiler"), Qt::CaseInsensitive)
            || mesh.name.contains(QStringLiteral("wing"), Qt::CaseInsensitive))) {
        return 5;
    }
    if (vertex >= mesh.normals.size()) {
        return -1;
    }
    ModelVec3 n = mesh.boneTransform.transformVector(mesh.normals[vertex]);
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len <= 1e-6f) {
        return -1;
    }
    n.x /= len; n.y /= len; n.z /= len;

    int side = -1;
    float best = 0.3f;
    for (int s = 0; s < 5; ++s) {
        const float d = facing[s][0] * n.x + facing[s][1] * n.y + facing[s][2] * n.z;
        if (d > best) {
            best = d;
            side = s;
        }
    }
    return side;
}

static int maskHits(const QString &carbinPath)
{
    QString error;
    const CarModel model = loadCarBin(carbinPath, &error);
    if (model.meshes.empty()) {
        std::fprintf(stderr, "decode failed: %s\n", qPrintable(error));
        return 1;
    }
    const QString masksDir = QFileInfo(carbinPath).absoluteDir().filePath(QStringLiteral("LiveryMasks"));
    const LiveryMaskSet masks = loadLiveryMasks(masksDir, &error);
    if (!masks.valid()) {
        std::fprintf(stderr, "masks failed: %s\n", qPrintable(error));
        return 1;
    }

    const char *names[6] = {"Front", "Back", "Top", "Left", "Right", "Spoiler"};
    int maxChannel = -1;
    for (const CarMesh &mesh : model.meshes) {
        if (!isPaintMaterial(mesh.materialName)) {
            continue;
        }
        for (int c = 0; c < static_cast<int>(mesh.uvChannels.size()); ++c) {
            if (mesh.uvChannels[c].size() == mesh.positions.size()) {
                maxChannel = std::max(maxChannel, c);
            }
        }
    }
    if (maxChannel < 0) {
        std::printf("no paint mesh UV channels found\n");
        return 0;
    }

    for (int c = 0; c <= maxChannel; ++c) {
        long long totals[6] = {};
        long long hits[6][6] = {};
        long long anyHit = 0;
        long long sampled = 0;
        for (const CarMesh &mesh : model.meshes) {
            if (!isPaintMaterial(mesh.materialName) || c >= static_cast<int>(mesh.uvChannels.size())
                || mesh.uvChannels[c].size() != mesh.positions.size()) {
                continue;
            }
            const auto &uvs = mesh.uvChannels[c];
            for (size_t i = 0; i < mesh.positions.size(); ++i) {
                const int geom = normalSide(mesh, i);
                if (geom < 0 || geom >= 6) {
                    continue;
                }
                ++totals[geom];
                bool vertexHit = false;
                ModelVec2 uv = transformedUv(mesh, c, uvs[i]);
                if (c == 3) {
                    uv.u *= 0.5f;
                }
                for (int s = 0; s < 6; ++s) {
                    if (maskSample(masks.sides[s].mask, uv) >= 128.0f) {
                        ++hits[geom][s];
                        vertexHit = true;
                    }
                }
                if (vertexHit) {
                    ++anyHit;
                }
                ++sampled;
            }
        }
        if (sampled == 0) {
            continue;
        }
        std::printf("\nUV channel %d: sampled=%lld any-mask-hit=%lld (%.1f%%)\n",
                    c, sampled, anyHit, 100.0 * static_cast<double>(anyHit) / sampled);
        std::printf("              mask ->  Front   Back    Top    Left   Right  Spoiler\n");
        for (int geom = 0; geom < 6; ++geom) {
            std::printf("  geom %-7s %7lld", names[geom], totals[geom]);
            for (int s = 0; s < 6; ++s) {
                const double pct = totals[geom] > 0
                    ? 100.0 * static_cast<double>(hits[geom][s]) / static_cast<double>(totals[geom])
                    : 0.0;
                std::printf(" %6.1f", pct);
            }
            std::printf("\n");
        }
    }
    return 0;
}

static int fitLivery(const QString &carbinPath)
{
    QString error;
    const CarModel model = loadCarBin(carbinPath, &error);
    if (model.meshes.empty()) {
        std::fprintf(stderr, "decode failed: %s\n", qPrintable(error));
        return 1;
    }
    const QString masksDir = QFileInfo(carbinPath).absoluteDir().filePath(QStringLiteral("LiveryMasks"));
    const LiveryMaskSet masks = loadLiveryMasks(masksDir, &error);
    if (!masks.valid()) {
        std::fprintf(stderr, "masks failed: %s\n", qPrintable(error));
        return 1;
    }

    const char *names[6] = {"Front", "Back", "Top", "Left", "Right", "Spoiler"};

    std::vector<float> wheelZ;
    for (const CarMesh &mesh : model.meshes) {
        if (!mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)
            || mesh.positions.empty()) {
            continue;
        }
        double sum = 0.0;
        for (const ModelVec3 &position : mesh.positions) {
            sum += mesh.boneTransform.transformPoint(position).z;
        }
        wheelZ.push_back(static_cast<float>(sum / mesh.positions.size()));
    }
    if (wheelZ.size() >= 2) {
        float rear = *std::min_element(wheelZ.begin(), wheelZ.end());
        float front = *std::max_element(wheelZ.begin(), wheelZ.end());
        for (int iteration = 0; iteration < 8; ++iteration) {
            double rearSum = 0.0, frontSum = 0.0;
            int rearCount = 0, frontCount = 0;
            for (float z : wheelZ) {
                if (std::abs(z - rear) < std::abs(z - front)) {
                    rearSum += z;
                    ++rearCount;
                } else {
                    frontSum += z;
                    ++frontCount;
                }
            }
            if (rearCount > 0) rear = static_cast<float>(rearSum / rearCount);
            if (frontCount > 0) front = static_cast<float>(frontSum / frontCount);
        }
        std::printf("axles rear=%.4f front=%.4f wheelbase=%.4f\n", rear, front, front - rear);
    }

    auto axisOf = [](const ModelVec3 &v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); };

    for (int s = 0; s < 6; ++s) {
        const LiverySide &L = masks.sides[s];
        float axlo = 1e9f, axhi = -1e9f, aylo = 1e9f, ayhi = -1e9f;
        long long count = 0;
        const std::vector<CarMesh> &projectionMeshes = model.liveryProjectionMeshes.empty()
            ? model.meshes
            : model.liveryProjectionMeshes;
        for (const CarMesh &mesh : projectionMeshes) {
            if (!isPaintMaterial(mesh.materialName) || mesh.positions.empty()) continue;
            const bool spoiler = !mesh.name.contains(QStringLiteral("mirror"), Qt::CaseInsensitive)
                && (mesh.name.contains(QStringLiteral("spoiler"), Qt::CaseInsensitive) || mesh.name.contains(QStringLiteral("wing"), Qt::CaseInsensitive));
            if ((s == 5) != spoiler) continue;
            for (const ModelVec3 &position : mesh.positions) {
                const ModelVec3 wp = mesh.boneTransform.transformPoint(position);
                const float ax = (L.xSign * L.xScale) * axisOf(wp, L.xAxis);
                const float ay = (L.ySign * L.yScale) * axisOf(wp, L.yAxis);
                axlo = std::min(axlo, ax); axhi = std::max(axhi, ax);
                aylo = std::min(aylo, ay); ayhi = std::max(ayhi, ay);
                ++count;
            }
        }
        int txlo = 1 << 30, txhi = -(1 << 30), tylo = 1 << 30, tyhi = -(1 << 30);
        const SwatchMask &m = L.mask;
        if (m.valid()) {
            for (int y = 0; y < m.height; ++y)
                for (int x = 0; x < m.width; ++x)
                    if (m.at(x, y) >= 128) {
                        txlo = std::min(txlo, x); txhi = std::max(txhi, x);
                        tylo = std::min(tylo, y); tyhi = std::max(tyhi, y);
                    }
        }
        std::printf("%-8s axis=(%s,%s) rot=%.0f  verts=%lld\n", names[s],
                    qPrintable(QString("%1%2").arg(L.xSign < 0 ? "-" : "+").arg("xyz"[L.xAxis])),
                    qPrintable(QString("%1%2").arg(L.ySign < 0 ? "-" : "+").arg("xyz"[L.yAxis])),
                    L.rotationDeg, count);
        if (count > 0 && m.valid() && txhi >= txlo) {
            const float cxlo = txlo - 1024.0f, cxhi = txhi - 1024.0f;
            const float cylo = 512.0f - tyhi, cyhi = 512.0f - tylo;
            std::printf("   geom  ax:[%.3f,%.3f] ay:[%.3f,%.3f]\n", axlo, axhi, aylo, ayhi);
            std::printf("   cover cx:[%.1f,%.1f] cy:[%.1f,%.1f]  region L/R/T/B=%.0f/%.0f/%.0f/%.0f\n",
                        cxlo, cxhi, cylo, cyhi, L.left, L.right, L.top, L.bottom);
            const float sx = (cxhi - cxlo) / (axhi - axlo);
            const float ox = cxlo - sx * axlo;
            const float sy = (cyhi - cylo) / (ayhi - aylo);
            const float oy = cylo - sy * aylo;
            std::printf("   FIT   canvasX = %.1f + %.1f*ax   canvasY = %.1f + %.1f*ay\n", ox, sx, oy, sy);
        }
    }
    return 0;
}

static int dumpSwatchbin(const QString &path)
{
    QString error;
    const SwatchMask mask = loadSwatchMask(path, &error);
    if (mask.valid()) {
        long long covered = 0;
        for (uint8_t v : mask.coverage) {
            if (v >= 128) {
                ++covered;
            }
        }
        const double total = static_cast<double>(mask.width) * mask.height;
        std::printf("%s\n  mask %dx%d, %.1f%% covered\n", qPrintable(path), mask.width, mask.height,
                    100.0 * static_cast<double>(covered) / total);
        constexpr int cols = 64, rows = 24;
        for (int r = 0; r < rows; ++r) {
            std::string line;
            for (int c = 0; c < cols; ++c) {
                const int x = c * mask.width / cols;
                const int y = r * mask.height / rows;
                line += mask.at(x, y) >= 128 ? '#' : (mask.at(x, y) >= 32 ? '.' : ' ');
            }
            std::printf("  |%s|\n", line.c_str());
        }
        return 0;
    }

    const QString maskError = error;
    error.clear();
    const SwatchImage image = loadSwatchImage(path, &error);
    if (!image.valid()) {
        std::fprintf(stderr, "failed to decode %s: mask: %s; image: %s\n", qPrintable(path),
                     qPrintable(maskError.isEmpty() ? QStringLiteral("unknown error") : maskError),
                     qPrintable(error.isEmpty() ? QStringLiteral("unknown error") : error));
        return 1;
    }

    long long opaque = 0;
    long long nonTransparent = 0;
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        const uint8_t a = image.rgba[i + 3];
        if (a >= 128) {
            ++opaque;
        }
        if (a > 0) {
            ++nonTransparent;
        }
    }
    const double total = static_cast<double>(image.width) * image.height;
    std::printf("%s\n  image %dx%d, %.1f%% alpha>=128, %.1f%% alpha>0\n",
                qPrintable(path), image.width, image.height,
                100.0 * static_cast<double>(opaque) / total,
                100.0 * static_cast<double>(nonTransparent) / total);

    constexpr int cols = 64, rows = 24;
    for (int r = 0; r < rows; ++r) {
        std::string line;
        for (int c = 0; c < cols; ++c) {
            const int x = c * image.width / cols;
            const int y = r * image.height / rows;
            const size_t idx = (static_cast<size_t>(y) * image.width + x) * 4;
            const uint8_t r8 = image.rgba[idx + 0];
            const uint8_t g8 = image.rgba[idx + 1];
            const uint8_t b8 = image.rgba[idx + 2];
            const uint8_t a8 = image.rgba[idx + 3];
            const int luma = (static_cast<int>(r8) * 30 + static_cast<int>(g8) * 59 + static_cast<int>(b8) * 11) / 100;
            line += a8 < 8 ? ' ' : (luma > 192 ? '#' : (luma > 96 ? '*' : '.'));
        }
        std::printf("  |%s|\n", line.c_str());
    }
    return 0;
}

static int dumpUv(const QString &carbinPath)
{
    QString error;
    const CarModel model = loadCarBin(carbinPath, &error);
    if (model.meshes.empty()) {
        std::fprintf(stderr, "decode failed: %s\n", qPrintable(error));
        return 1;
    }
    const char *axisName[3] = {"x", "y", "z"};
    for (const CarMesh &mesh : model.meshes) {
        if (!isPaintMaterial(mesh.materialName) || mesh.positions.empty()) {
            continue;
        }
        const int lodIdx = mesh.name.lastIndexOf(QStringLiteral("_LOD"));
        if (lodIdx >= 0) {
            const QString suf = mesh.name.mid(lodIdx + 4);
            bool digit = false;
            suf.toInt(&digit);
            if (digit) {
                continue;
            }
        }
        std::printf("[%s] mat=%s verts=%lld\n", qPrintable(mesh.name), qPrintable(mesh.materialName),
                    static_cast<long long>(mesh.positions.size()));
        for (int c = 0; c < static_cast<int>(mesh.uvChannels.size()); ++c) {
            const auto &ch = mesh.uvChannels[c];
            if (ch.size() != mesh.positions.size()) {
                continue;
            }
            float uMin = ch[0].u, uMax = ch[0].u, vMin = ch[0].v, vMax = ch[0].v;
            double uSum = 0, vSum = 0, uu = 0, vv = 0;
            long long inUnit = 0;
            for (const auto &t : ch) {
                uMin = std::min(uMin, t.u); uMax = std::max(uMax, t.u);
                vMin = std::min(vMin, t.v); vMax = std::max(vMax, t.v);
                uSum += t.u; vSum += t.v; uu += t.u * t.u; vv += t.v * t.v;
                if (t.u >= 0.0f && t.u <= 1.0f && t.v >= 0.0f && t.v <= 1.0f) ++inUnit;
            }
            const double n = static_cast<double>(ch.size());
            const double uMean = uSum / n, vMean = vSum / n;
            const double uStd = std::sqrt(std::max(0.0, uu / n - uMean * uMean));
            const double vStd = std::sqrt(std::max(0.0, vv / n - vMean * vMean));
            double corrU[3] = {0, 0, 0}, corrV[3] = {0, 0, 0};
            double axSum[3] = {0, 0, 0}, axSq[3] = {0, 0, 0}, coU[3] = {0, 0, 0}, coV[3] = {0, 0, 0};
            for (size_t i = 0; i < ch.size(); ++i) {
                const ModelVec3 wp = mesh.boneTransform.transformPoint(mesh.positions[i]);
                const double a[3] = {wp.x, wp.y, wp.z};
                for (int k = 0; k < 3; ++k) {
                    axSum[k] += a[k]; axSq[k] += a[k] * a[k];
                    coU[k] += a[k] * ch[i].u; coV[k] += a[k] * ch[i].v;
                }
            }
            for (int k = 0; k < 3; ++k) {
                const double aMean = axSum[k] / n;
                const double aStd = std::sqrt(std::max(0.0, axSq[k] / n - aMean * aMean));
                corrU[k] = (aStd > 1e-6 && uStd > 1e-6) ? (coU[k] / n - aMean * uMean) / (aStd * uStd) : 0.0;
                corrV[k] = (aStd > 1e-6 && vStd > 1e-6) ? (coV[k] / n - aMean * vMean) / (aStd * vStd) : 0.0;
            }
            std::printf("  uv[%d] u:[%.3f,%.3f] v:[%.3f,%.3f] inUnit=%.0f%%  corrU(%s%.2f %s%.2f %s%.2f) corrV(%s%.2f %s%.2f %s%.2f)\n",
                        c, uMin, uMax, vMin, vMax, 100.0 * inUnit / n,
                        axisName[0], corrU[0], axisName[1], corrU[1], axisName[2], corrU[2],
                        axisName[0], corrV[0], axisName[1], corrV[1], axisName[2], corrV[2]);
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: fh6_model_dump <file.modelbin|file.carbin|file.zip|file.swatchbin> [--verbose] [--fit] [--mask-hits] [--uv]\n");
        return 2;
    }

    QString path = args[1];
    const bool verbose = args.contains(QStringLiteral("--verbose"));

    if (path.endsWith(QStringLiteral(".swatchbin"), Qt::CaseInsensitive)) {
        return dumpSwatchbin(path);
    }
    std::unique_ptr<QTemporaryDir> tempDir;
    if (!resolveZipInput(path, tempDir)) {
        return 1;
    }
    if (args.contains(QStringLiteral("--uv"))) {
        return dumpUv(path);
    }
    if (args.contains(QStringLiteral("--mask-hits"))) {
        return maskHits(path);
    }
    if (args.contains(QStringLiteral("--fit"))) {
        return fitLivery(path);
    }

    QString error;
    const CarModel model = path.endsWith(QStringLiteral(".carbin"), Qt::CaseInsensitive)
        ? loadCarBin(path, &error)
        : loadModelBin(path, &error);
    if (model.meshes.empty()) {
        std::fprintf(stderr, "failed to decode %s: %s\n",
                     qPrintable(path), qPrintable(error.isEmpty() ? QStringLiteral("unknown error") : error));
        return 1;
    }

    std::printf("%s\n", qPrintable(path));
    std::printf("  meshes:   %lld\n", static_cast<long long>(model.meshes.size()));
    std::printf("  projection meshes: %lld\n", static_cast<long long>(model.liveryProjectionMeshes.size()));
    std::printf("  locators: %lld\n", static_cast<long long>(model.locators.size()));
    std::printf("  vertices: %lld\n", model.totalVertices());
    std::printf("  indices:  %lld (%lld triangles)\n", model.totalIndices(), model.totalIndices() / 3);
    std::printf("  bounds:   min (%.3f, %.3f, %.3f)  max (%.3f, %.3f, %.3f)\n",
                model.boundsMin.x, model.boundsMin.y, model.boundsMin.z,
                model.boundsMax.x, model.boundsMax.y, model.boundsMax.z);

    if (verbose) {
        int i = 0;
        for (const CarMesh &mesh : model.meshes) {
            int uvChannels = 0;
            for (const auto &ch : mesh.uvChannels) {
                if (!ch.empty()) {
                    ++uvChannels;
                }
            }
            double cx = 0, cy = 0, cz = 0, nx = 0, ny = 0, nz = 0;
            for (size_t v = 0; v < mesh.positions.size(); ++v) {
                const ModelVec3 wp = mesh.boneTransform.transformPoint(mesh.positions[v]);
                cx += wp.x; cy += wp.y; cz += wp.z;
                if (v < mesh.normals.size()) {
                    const ModelVec3 wn = mesh.boneTransform.transformVector(mesh.normals[v]);
                    nx += wn.x; ny += wn.y; nz += wn.z;
                }
            }
            const double inv = mesh.positions.empty() ? 1.0 : 1.0 / mesh.positions.size();
            std::printf("  [%3d] %-24s mat=%-18s instance=%-4d paint=%016llX params=%-4lld rgba(%.3f,%.3f,%.3f,%.3f) gloss=%.3f verts=%-6lld  centroid(%.2f,%.2f,%.2f) avgN(%.2f,%.2f,%.2f)\n",
                        i++, qPrintable(mesh.name.isEmpty() ? QStringLiteral("(unnamed)") : mesh.name),
                        qPrintable(mesh.materialName.isEmpty() ? QStringLiteral("(none)") : mesh.materialName),
                        mesh.modelInstanceId,
                        static_cast<unsigned long long>(mesh.paintMaterialHash),
                        static_cast<long long>(mesh.material ? mesh.material->parameters.size() : 0),
                        mesh.material ? mesh.material->baseColor[0] : 0.0f,
                        mesh.material ? mesh.material->baseColor[1] : 0.0f,
                        mesh.material ? mesh.material->baseColor[2] : 0.0f,
                        mesh.material ? mesh.material->opacity : 1.0f,
                        mesh.material ? mesh.material->gloss : 0.45f,
                        static_cast<long long>(mesh.positions.size()),
                        cx * inv, cy * inv, cz * inv, nx * inv, ny * inv, nz * inv);
            if (mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)
                && mesh.name.contains(QStringLiteral("LODS0"), Qt::CaseInsensitive)
                && !mesh.positions.empty()) {
                ModelVec3 lo = mesh.boneTransform.transformPoint(mesh.positions.front());
                ModelVec3 hi = lo;
                for (const ModelVec3 &position : mesh.positions) {
                    const ModelVec3 world = mesh.boneTransform.transformPoint(position);
                    lo.x = std::min(lo.x, world.x); lo.y = std::min(lo.y, world.y); lo.z = std::min(lo.z, world.z);
                    hi.x = std::max(hi.x, world.x); hi.y = std::max(hi.y, world.y); hi.z = std::max(hi.z, world.z);
                }
                long long validTriangles = 0;
                long long invalidTriangles = 0;
                long long nondegenerateTriangles = 0;
                for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
                    const quint32 ia = mesh.indices[index];
                    const quint32 ib = mesh.indices[index + 1];
                    const quint32 ic = mesh.indices[index + 2];
                    if (ia >= mesh.positions.size() || ib >= mesh.positions.size() || ic >= mesh.positions.size()) {
                        ++invalidTriangles;
                        continue;
                    }
                    ++validTriangles;
                    const ModelVec3 a = mesh.positions[ia];
                    const ModelVec3 b = mesh.positions[ib];
                    const ModelVec3 c = mesh.positions[ic];
                    const float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
                    const float acx = c.x - a.x, acy = c.y - a.y, acz = c.z - a.z;
                    const float nx = aby * acz - abz * acy;
                    const float ny = abz * acx - abx * acz;
                    const float nz = abx * acy - aby * acx;
                    if (nx * nx + ny * ny + nz * nz > 1.0e-12f) {
                        ++nondegenerateTriangles;
                    }
                }
                std::printf("           worldBounds[(%.3f,%.3f,%.3f)..(%.3f,%.3f,%.3f)] indices=%lld triangles(valid=%lld nondegenerate=%lld invalid=%lld)\n",
                            lo.x, lo.y, lo.z, hi.x, hi.y, hi.z,
                            static_cast<long long>(mesh.indices.size()), validTriangles,
                            nondegenerateTriangles, invalidTriangles);
            }
            for (int c = 0; c < static_cast<int>(mesh.uvChannels.size()); ++c) {
                const auto &ch = mesh.uvChannels[c];
                if (ch.empty()) {
                    continue;
                }
                float uMin = ch[0].u, uMax = ch[0].u, vMin = ch[0].v, vMax = ch[0].v;
                double uSum = 0, vSum = 0;
                for (const auto &t : ch) {
                    uMin = std::min(uMin, t.u); uMax = std::max(uMax, t.u);
                    vMin = std::min(vMin, t.v); vMax = std::max(vMax, t.v);
                    uSum += t.u; vSum += t.v;
                }
                const TexCoordTransform transform = c < static_cast<int>(mesh.texCoordTransforms.size())
                    ? mesh.texCoordTransforms[c]
                    : TexCoordTransform{};
                std::printf("           uv[%d] u:[%.3f,%.3f] v:[%.3f,%.3f] mean(%.3f,%.3f) transform(%.6f,%.6f,%.6f,%.6f)\n",
                            c, uMin, uMax, vMin, vMax, uSum / ch.size(), vSum / ch.size(),
                            transform.offsetU, transform.scaleU, transform.offsetV, transform.scaleV);
            }
        }
    }

    return 0;
}
