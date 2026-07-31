#include "material_hashes.h"
#include "model_material.h"

#include <QDir>
#include <QFile>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (false)

bool near(float actual, float expected) {
    return std::abs(actual - expected) < 0.00001f;
}

fh6::ModelMaterialParameter scalar(quint32 hash, float value) {
    fh6::ModelMaterialParameter parameter;
    parameter.nameHash = hash;
    parameter.type = fh6::ModelMaterialParameterType::Float;
    parameter.scalar = value;
    return parameter;
}

fh6::ModelMaterialParameter boolean(quint32 hash, bool value) {
    fh6::ModelMaterialParameter parameter;
    parameter.nameHash = hash;
    parameter.type = fh6::ModelMaterialParameterType::Bool;
    parameter.boolean = value;
    return parameter;
}

fh6::ModelMaterialParameter vector(quint32 hash, std::array<float, 4> value) {
    fh6::ModelMaterialParameter parameter;
    parameter.nameHash = hash;
    parameter.type = fh6::ModelMaterialParameterType::Vector;
    parameter.vector = value;
    return parameter;
}

fh6::ModelMaterialParameter texture(quint32 hash, const char *path) {
    fh6::ModelMaterialParameter parameter;
    parameter.nameHash = hash;
    parameter.type = fh6::ModelMaterialParameterType::Texture2D;
    parameter.texturePath = QString::fromLatin1(path);
    return parameter;
}

void testAutomotivePaintRetentionAndOverrides() {
    using namespace fh6::material_hashes::parameter;

    fh6::ModelMaterial defaults;
    defaults.parameters = {
        vector(kGlancingFlopColor, {0.1f, 0.2f, 0.3f, 1.0f}),
        scalar(kGlancingFlopPower, 3.5f),
        boolean(kGlancingFlopEnabled, true),
        scalar(kGlancingFlopStrength, 0.7f),
        vector(kFlakeColor, {0.8f, 0.7f, 0.6f, 1.0f}),
        scalar(kFlakeCoverage, 0.6f),
        scalar(kFlakeRoughness, 0.22f),
        scalar(kGlitterIntensity, 1.4f),
        scalar(kClearcoatCoverage, 0.9f),
        scalar(kClearcoatRoughness, 0.08f),
        vector(kClearcoatTint, {0.95f, 0.9f, 0.85f, 1.0f}),
        boolean(kClearcoatOnLivery, true),
        scalar(kNormalIntensity, 0.75f),
        vector(kNormalMap00UvTiling, {12.0f, 13.0f, 0.1f, 0.2f}),
        vector(kNormalMap0UvTiling, {4.0f, 5.0f, 0.3f, 0.4f}),
        scalar(kOrangePeelStrength, 0.35f),
        scalar(kTextureTilingU[1], 32.0f),
        scalar(kTextureTilingV[1], 24.0f),
        scalar(kUvOrientation, 45.0f),
        texture(kNormalMap00Texture, "fine.swatchbin"),
        texture(kNormalMap0Texture, "coarse.swatchbin"),
        texture(kOrangePeelNormalTexture, "orange.swatchbin"),
    };
    fh6::ModelMaterial instance;
    instance.parameters = {
        boolean(kGlancingFlopEnabled, false),
        scalar(kClearcoatCoverage, 0.25f),
        boolean(kClearcoatOnLivery, false),
        scalar(kUvOrientation, 90.0f),
        texture(kNormalMap0Texture, "coarse_override.swatchbin"),
    };

    const std::shared_ptr<fh6::ModelMaterial> merged =
        fh6::mergeModelMaterialDefaults(defaults, instance);
    CHECK(merged != nullptr);
    const fh6::AutomotivePaintParameters &paint = merged->automotivePaint;

    CHECK(paint.hasGlancingFlopColor);
    CHECK(near(paint.glancingFlopColor[1], 0.2f));
    CHECK(paint.hasGlancingFlopPower && near(paint.glancingFlopPower, 3.5f));
    CHECK(paint.hasGlancingFlopEnabled && !paint.glancingFlopEnabled);
    CHECK(paint.hasGlancingFlopStrength && near(paint.glancingFlopStrength, 0.7f));
    CHECK(paint.hasFlakeColor && near(paint.flakeColor[2], 0.6f));
    CHECK(paint.hasFlakeCoverage && near(paint.flakeCoverage, 0.6f));
    CHECK(paint.hasFlakeRoughness && near(paint.flakeRoughness, 0.22f));
    CHECK(paint.hasGlitterIntensity && near(paint.glitterIntensity, 1.4f));
    CHECK(paint.hasClearCoatCoverage && near(paint.clearCoatCoverage, 0.25f));
    CHECK(paint.hasClearCoatRoughness && near(paint.clearCoatRoughness, 0.08f));
    CHECK(paint.hasClearCoatTint && near(paint.clearCoatTint[0], 0.95f));
    CHECK(paint.hasClearCoatOnLivery && !paint.clearCoatOnLivery);
    CHECK(paint.hasNormalIntensity && near(paint.normalIntensity, 0.75f));
    CHECK(paint.hasNormalMap00UvTiling && near(paint.normalMap00UvTiling[2], 0.1f));
    CHECK(paint.hasNormalMap0UvTiling && near(paint.normalMap0UvTiling[3], 0.4f));
    CHECK(paint.hasOrangePeelStrength && near(paint.orangePeelStrength, 0.35f));
    CHECK(paint.normalMap00Texture == QStringLiteral("fine.swatchbin"));
    CHECK(paint.normalMap0Texture == QStringLiteral("coarse_override.swatchbin"));
    CHECK(paint.orangePeelNormalTexture == QStringLiteral("orange.swatchbin"));
    CHECK(near(merged->uTiling, 32.0f));
    CHECK(near(merged->vTiling, 24.0f));
    CHECK(near(merged->uvOrientationDegrees, 90.0f));

    // Clear-coat roughness must not overwrite the independently retained base gloss.
    CHECK(near(merged->gloss, 0.45f));
    // Preserve the legacy coarse flake summary while retaining exact coverage separately.
    CHECK(near(merged->flakeAmount, 0.6f));
}

std::shared_ptr<fh6::ModelMaterial> loadMaterial(const QDir &directory, const char *name) {
    QFile file(directory.filePath(QString::fromLatin1(name) + QStringLiteral(".materialbin")));
    CHECK(file.open(QIODevice::ReadOnly));
    return file.isOpen() ? fh6::decodeMaterialBundle(file.readAll()) : nullptr;
}

void testGameEvidence(const QString &path) {
    const QDir directory(path);
    const std::shared_ptr<fh6::ModelMaterial> normal = loadMaterial(directory, "normalpaint");
    const std::shared_ptr<fh6::ModelMaterial> twoTone =
        loadMaterial(directory, "twotonepolished");
    const std::shared_ptr<fh6::ModelMaterial> candy = loadMaterial(directory, "candypaint");
    CHECK(normal != nullptr && twoTone != nullptr && candy != nullptr);
    if (!normal || !twoTone || !candy) {
        return;
    }

    CHECK(normal->automotivePaint.hasFlakeCoverage);
    CHECK(near(normal->automotivePaint.flakeCoverage, 0.0f));
    CHECK(normal->automotivePaint.hasFlakeRoughness);
    CHECK(near(normal->automotivePaint.flakeRoughness, 0.7f));
    CHECK(normal->automotivePaint.hasClearCoatRoughness);
    CHECK(near(normal->automotivePaint.clearCoatRoughness, 0.0f));
    CHECK(!normal->automotivePaint.hasGlancingFlopEnabled);

    CHECK(twoTone->automotivePaint.hasGlancingFlopColor);
    CHECK(near(twoTone->automotivePaint.glancingFlopColor[0], 0.8468732f));
    CHECK(twoTone->automotivePaint.hasGlancingFlopPower);
    CHECK(near(twoTone->automotivePaint.glancingFlopPower, 3.0f));
    CHECK(twoTone->automotivePaint.hasGlancingFlopEnabled);
    CHECK(twoTone->automotivePaint.glancingFlopEnabled);
    CHECK(twoTone->automotivePaint.hasFlakeColor);
    CHECK(near(twoTone->automotivePaint.flakeCoverage, 0.8f));
    CHECK(near(twoTone->automotivePaint.flakeRoughness, 0.3f));

    CHECK(candy->automotivePaint.hasClearCoatCoverage);
    CHECK(near(candy->automotivePaint.clearCoatCoverage, 1.0f));
    CHECK(candy->automotivePaint.hasClearCoatOnLivery);
    CHECK(!candy->automotivePaint.clearCoatOnLivery);
    CHECK(near(candy->automotivePaint.flakeCoverage, 1.0f));
    CHECK(near(candy->automotivePaint.flakeRoughness, 0.33f));
}

} // namespace

int main(int argc, char **argv) {
    testAutomotivePaintRetentionAndOverrides();
    if (argc >= 2) {
        testGameEvidence(QString::fromLocal8Bit(argv[1]));
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d model material checks failed\n", failures);
        return 1;
    }
    std::puts("model material checks passed");
    return 0;
}
