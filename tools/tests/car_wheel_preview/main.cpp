#include "car_preview_wheels.h"
#include "material_hashes.h"
#include "model_bundle.h"
#include "model_material.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QTemporaryDir>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int meshCount(const fls::CarModel &model, int partType) {
    return static_cast<int>(std::count_if(model.meshes.cbegin(), model.meshes.cend(),
        [partType](const auto &mesh) { return mesh.carPartType == partType; }));
}

void requireTireTextures(const fls::ModelMaterial &material) {
    namespace hashes = fls::material_hashes::parameter;
    const auto &normal = material.normalTexture;
    const auto &height = material.tireHeightTexture;
    require(normal && normal->image.valid(), "compound lost its stationary normal map");
    require(!normal->path.contains(QStringLiteral("blur"), Qt::CaseInsensitive),
        "stationary tyre received a motion-blur map");
    bool packedHeight = false;
    bool separateHeight = false;
    for (const auto &parameter : material.parameters) {
        if (parameter.texturePath.isEmpty()) continue;
        const auto binding = fls::shaderTextureParameter(material, parameter);
        if (!binding) continue;
        packedHeight = packedHeight || *binding == hashes::kTireNormalHeightTexture;
        separateHeight = separateHeight || fls::material_hashes::contains(hashes::kSeparateTireHeightTextures, *binding);
        if (*binding == hashes::kLegacyTireDiffuseTexture) {
            require(material.diffuseTexture && material.diffuseTexture->image.valid(),
                "compound lost its diffuse map");
            require(!material.diffuseTexture->path.contains(QStringLiteral("blur"), Qt::CaseInsensitive),
                "stationary tyre received a motion-blur diffuse map");
        }
        if (*binding == hashes::kTireSurfaceMasks) {
            require(material.surfaceTexture && material.surfaceTexture->image.valid(),
                "compound lost its surface masks");
        }
    }
    if (packedHeight || separateHeight) {
        require(height && height->image.valid(), "compound lost its height map");
        require((height == normal) == packedHeight, "compound received the wrong height map binding");
    }
    if (height && height->path.contains(QStringLiteral("tread_street_nrml"), Qt::CaseInsensitive)) {
        const auto &pixels = height->image.rgba;
        size_t alpha = 3;
        while (alpha < pixels.size() && pixels[alpha] == pixels[3]) alpha += 4;
        require(alpha < pixels.size(), "street tread height decoded to a constant");
    }
}

void requireTireMaterial(const fls::CarMesh &mesh) {
    require(static_cast<bool>(mesh.material), "compound lost its material");
    requireTireTextures(*mesh.material);
    const auto vector = [](const fls::ModelVec3 &v) { return QVector3D(v.x, v.y, v.z); };
    double orientation = 0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const auto a = mesh.indices[i];
        const auto b = mesh.indices[i + 1];
        const auto c = mesh.indices[i + 2];
        const auto edge1 = vector(mesh.positions[b]) - vector(mesh.positions[a]);
        const auto edge2 = vector(mesh.positions[c]) - vector(mesh.positions[a]);
        orientation += QVector3D::dotProduct(QVector3D::crossProduct(edge1, edge2), vector(mesh.normals[a]));
    }
    require(orientation > 0, "fitted tyre winding disagrees with its normals");
}

void requirePair(const fls::CarModel &model, int partType) {
    bool left = false;
    bool right = false;
    for (const auto &mesh : model.meshes) {
        if (mesh.carPartType != partType) continue;
        fls::ModelBounds bounds;
        bounds.include(mesh);
        left = left || bounds.minimum.x + bounds.maximum.x < 0;
        right = right || bounds.minimum.x + bounds.maximum.x > 0;
        for (const auto &point : mesh.positions) {
            require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),
                "wheel fitting produced non-finite vertices");
        }
    }
    if (!left || !right) {
        std::fprintf(stderr, "Pair failure for selector %d: left=%d right=%d\n", partType, left, right);
        for (const auto &mesh : model.meshes) {
            if (mesh.carPartType != partType) continue;
            fls::ModelBounds bounds;
            bounds.include(mesh);
            std::fprintf(stderr, "  %s instance=%d x=[%.4f, %.4f]\n", qPrintable(mesh.name), mesh.modelInstanceId,
                bounds.minimum.x, bounds.maximum.x);
        }
    }
    require(left && right, "axle pair did not occupy both sides of the car");
}

void testSelectionPolicy() {
    using namespace gui;
    WheelPreviewState state;
    state.rims = {{QStringLiteral("Stock"), {}, kStockRimStyle},
        {QStringLiteral("A"), {}, 101}, {QStringLiteral("B"), {}, 102}};
    state.tires = {{QStringLiteral("Stock"), {}, 200}, {QStringLiteral("Sport"), {}, 201}};
    state.tireId = 200;
    require(selectWheelPreviewStyle(state, kTireSelector, 201) && state.tireId == 201,
        "whole-car tire choice was not applied");
    require(selectWheelPreviewStyle(state, kFrontRimSelector, 101)
        && state.axles[0].rimId == 101 && state.axles[1].rimId == 101,
        "legacy rim choice did not apply to both axles");
    require(selectWheelPreviewStyle(state, kRearRimSelector, 102)
        && state.axles[0].rimId == 102 && state.axles[1].rimId == 102,
        "legacy rear request created independent rim styles");
    require(selectWheelPreviewStyle(state, kFrontRimSelector, kStockRimStyle)
        && state.axles[0].rimId == kStockRimStyle && state.axles[1].rimId == kStockRimStyle,
        "stock rim selection did not restore both axles");
    state.independentRimStyles = true;
    require(selectWheelPreviewStyle(state, kFrontRimSelector, 101)
        && state.axles[1].rimId == kStockRimStyle, "FH6 front rim changed the rear axle");
    require(selectWheelPreviewStyle(state, kRearRimSelector, 102)
        && state.axles[0].rimId == 101, "FH6 rear rim changed the front axle");
    require(selectWheelPreviewStyle(state, kTireSelector, 200)
        && state.axles[0].rimId == 101 && state.axles[1].rimId == 102,
        "tire choice altered the selected rim styles");
    require(!selectWheelPreviewStyle(state, kTireSelector, -100) && state.tireId == 200,
        "unknown tire choice changed the configuration");
}

fls::CarModel loadFixtureCar(const QString &archive, const fls::WheelSizing &sizing) {
    QString error;
    const auto extracted = fls::sharedZipExtraction(archive, &error);
    require(extracted != nullptr, qPrintable(error));
    QDirIterator iterator(extracted->path(), {QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    require(iterator.hasNext(), "carbin missing from fixture");
    auto model = fls::loadCarBin(iterator.next(), &error, sizing);
    require(!model.meshes.empty(), qPrintable(error));
    require(model.wheelMounts.size() == 4, "four wheel mounts were not retained");
    return model;
}

void testCar(const QString &archive) {
    using namespace gui;
    QString error;
    fls::WheelSizing sizing;
    auto model = loadFixtureCar(archive, sizing);
    auto state = prepareWheelPreview(model, archive, QStringLiteral("FH6"), sizing);
    const auto legacyState = prepareWheelPreview(model, archive, QStringLiteral("FH5"), sizing);
    require(state && state->rims.size() > 2, "available rim catalog was not connected");
    require(state->independentRimStyles && legacyState && !legacyState->independentRimStyles,
        "per-axle rim styles were not limited to FH6");
    require(state->tires.size() > 2, "tire compounds missing");
    state->loadCarTextures = true;
    installWheelPartOptions(model, *state);

    auto front = buildWheelPreviewGeometry(*state, true, kStockRimStyle, state->tireId, &error);
    auto rear = buildWheelPreviewGeometry(*state, false, kStockRimStyle, state->tireId, &error);
    const auto frontPositions = front.meshes.front().positions;
    require(meshCount(front, kFrontRimSelector) >= 2 && meshCount(front, kTireSelector) >= 2,
        "stock front wheel/tire pair was not assembled");
    require(meshCount(front, kRearRimSelector) == 0,
        "front axle build changed the rear axle");
    const auto rearPositions = rear.meshes.front().positions;
    state->sizing.front.rimDiameterInches += 1;
    state->sizing.front.tireWidthMillimetres += 20;
    require(selectWheelPreviewStyle(*state, kTireSelector, state->tires[1].id), "global tire selection failed");
    front = buildWheelPreviewGeometry(*state, true, state->rims[1].id, state->tireId, &error);
    if (meshCount(front, kTireSelector) < 2) {
        std::fprintf(stderr, "Rim %s / tire %s: %s\n", qPrintable(state->rims[1].archive),
            qPrintable(state->tires[1].archive), qPrintable(error));
        for (const auto &mesh : front.meshes) {
            std::fprintf(stderr, "  type=%d name=%s material=%s\n", mesh.carPartType,
                qPrintable(mesh.name), qPrintable(mesh.materialName));
        }
    }
    require(meshCount(front, kFrontRimSelector) >= 2 && meshCount(front, kTireSelector) >= 2,
        "aftermarket front rim/tire pair was not assembled");
    rear = buildWheelPreviewGeometry(*state, false, state->rims[2].id, state->tireId, &error);
    require(meshCount(rear, kRearRimSelector) >= 2 && meshCount(rear, kTireSelector) >= 2,
        "different rear wheel/tire choices were not assembled");
    requirePair(front, kFrontRimSelector);
    requirePair(front, kTireSelector);
    requirePair(rear, kRearRimSelector);
    requirePair(rear, kTireSelector);
    for (const auto &compound : state->tires) {
        const auto geometry = buildWheelPreviewGeometry(*state, true, kStockRimStyle, compound.id, &error);
        if (meshCount(geometry, kTireSelector) == 0) {
            std::fprintf(stderr, "Compound %s (%s): %s\n", qPrintable(compound.name), qPrintable(compound.archive), qPrintable(error));
            const QMutexLocker lock(&state->templates->mutex);
            const auto &templates = state->templates->models;
            for (auto cached = templates.cbegin(); cached != templates.cend(); ++cached) {
                if (!cached.key().contains(compound.archive)) continue;
                for (const auto &mesh : cached.value()->meshes) std::fprintf(stderr, "  template %s\n", qPrintable(mesh.name));
            }
        }
        requirePair(geometry, kTireSelector);
        for (const auto &mesh : geometry.meshes) {
            if (mesh.carPartType != kTireSelector) continue;
            requireTireMaterial(mesh);
        }
    }
    const auto unchangedRear = buildWheelPreviewGeometry(*state, false, kStockRimStyle, state->tireId, &error);
    require(unchangedRear.meshes.front().positions.size() == rearPositions.size(), "front sizing altered rear topology");
    for (size_t index = 0; index < rearPositions.size(); ++index) {
        const auto &first = rearPositions[index];
        const auto &second = unchangedRear.meshes.front().positions[index];
        require(first.x == second.x && first.y == second.y && first.z == second.z,
            "front sizing altered rear wheel coordinates");
    }
    fls::CarModel wheels = front;
    wheels.meshes.insert(wheels.meshes.end(), rear.meshes.cbegin(), rear.meshes.cend());
    replaceWheelPreviewGeometry(model, wheels);
    for (const auto &mesh : model.meshes) {
        if (mesh.carPartType == kTireSelector) {
            require(mesh.partOptionIds == std::vector<int>{state->tireId}, "installed tires used different axle compounds");
        }
    }
    require(meshCount(model, kFrontRimSelector) == meshCount(front, kFrontRimSelector)
        && meshCount(model, kRearRimSelector) == meshCount(rear, kRearRimSelector), "wheel replacement retained old rims");
    const auto widths = wheelSizeOptions(*state, true, false, state->bodyIds.isEmpty() ? -1 : state->bodyIds.front());
    require(!widths.isEmpty() && widths.front().value > 0, "stock width missing from sizing menu");
    state->sizing.front.tireWidthMillimetres = 9999;
    require(normalizeWheelPreviewSizing(*state, state->bodyIds.isEmpty() ? -1 : state->bodyIds.front()),
        "incompatible sizing was retained after a body change");
    require(state->sizing.front.tireWidthMillimetres == state->stockSizing.front.tireWidthMillimetres,
        "sizing normalization did not restore stock width");
    require(buildWheelPreviewGeometry(*state, true, -100, state->tireId).meshes.empty(),
        "unknown rim accepted");
    require(selectWheelPreviewStyle(*legacyState, kFrontRimSelector, legacyState->rims[1].id),
        "legacy replacement rim selection failed");
    require(selectWheelPreviewStyle(*legacyState, kFrontRimSelector, kStockRimStyle),
        "legacy stock rim restoration failed");
    const auto stockFront = buildWheelPreviewGeometry(*legacyState, true, legacyState->axles[0].rimId, legacyState->tireId, &error);
    const auto stockRear = buildWheelPreviewGeometry(*legacyState, false, legacyState->axles[1].rimId, legacyState->tireId, &error);
    const auto requireOriginal = [](const auto &original, const auto &restored) {
        require(original.size() == restored.size(), "stock rim restoration altered axle topology");
        for (size_t index = 0; index < original.size(); ++index) {
            require(original[index].x == restored[index].x && original[index].y == restored[index].y
                && original[index].z == restored[index].z, "stock rim restoration did not preserve the axle's original geometry");
        }
    };
    requireOriginal(frontPositions, stockFront.meshes.front().positions);
    requireOriginal(rearPositions, stockRear.meshes.front().positions);
    installWheelPartOptions(model, *legacyState);
    require(std::none_of(model.partOptions.cbegin(), model.partOptions.cend(), [](const auto &option) {
        return option.partType == kRearRimSelector;
    }), "legacy car exposes a separate rear-rim selector");
    std::printf("%s: %lld rim styles, %lld tire compounds; axle geometry passed\n", qPrintable(model.mediaName),
        static_cast<long long>(state->rims.size()), static_cast<long long>(state->tires.size()));
}

void testMotorsportCar(const QString &archive, const QString &gameFolder) {
    using namespace gui;
    const fls::WheelSizing sizing;
    const auto model = loadFixtureCar(archive, sizing);
    QDir platformFolder = QFileInfo(archive).absoluteDir();
    require(platformFolder.cdUp(), "Motorsport platform folder missing");
    for (const QString &folder : {gameFolder, platformFolder.absolutePath(), QString()}) {
        const auto state = prepareWheelPreview(model, archive, QStringLiteral("FM2023"), sizing, folder);
        require(state && state->rims.size() > 1, "Motorsport aftermarket rims missing");
        state->loadCarTextures = !folder.isEmpty();
        for (const bool front : {true, false}) {
            QString error;
            const auto stock = buildWheelPreviewGeometry(*state, front, kStockRimStyle, state->tireId, &error);
            require(error.isEmpty(), qPrintable(error));
            require(meshCount(stock, kTireSelector) >= 2, "Motorsport stock tire geometry missing");
            requirePair(stock, kTireSelector);
            if (state->loadCarTextures) {
                const auto textured = std::find_if(stock.meshes.cbegin(), stock.meshes.cend(), [](const auto &mesh) {
                    return mesh.carPartType == kTireSelector && mesh.material && mesh.material->tireHeightTexture;
                });
                require(textured != stock.meshes.cend(), "Motorsport stock tire height map missing");
                requireTireTextures(*textured->material);
            }
        }
        const int rimId = state->rims[1].id;
        require(selectWheelPreviewStyle(*state, kFrontRimSelector, rimId), "Motorsport rim selection failed");
        for (const bool front : {true, false}) {
            const int partType = front ? kFrontRimSelector : kRearRimSelector;
            const int selected = state->axles[front ? 0 : 1].rimId;
            QString error;
            const auto wheels = buildWheelPreviewGeometry(*state, front, selected, state->tireId, &error);
            require(error.isEmpty(), qPrintable(error));
            require(selected == rimId && meshCount(wheels, partType) >= 2
                && meshCount(wheels, kTireSelector) >= 2, "Motorsport rim and tire geometry missing");
            requirePair(wheels, partType);
            requirePair(wheels, kTireSelector);
        }
    }
    std::printf("%s: Motorsport split rim and tire libraries passed\n", qPrintable(model.mediaName));
}

void testTireMaterial(const QString &archive) {
    namespace hashes = fls::material_hashes::parameter;
    QString error;
    const QStringList entries = fls::listZipEntries(archive, &error);
    const auto entry = std::find_if(entries.begin(), entries.end(), [](const QString &name) {
        return name.endsWith(QStringLiteral(".modelbin"), Qt::CaseInsensitive);
    });
    require(entry != entries.end(), qPrintable(error.isEmpty() ? "tire model missing" : error));
    auto model = fls::decodeModel(fls::parseModelBundle(fls::readZipEntry(archive, *entry, &error)));
    require(!model.meshes.empty(), "tire model has no geometry");
    gui::resolveTireTemplateMaterials(model, archive, true);
    QSet<const fls::ModelMaterial *> materials;
    for (const auto &mesh : model.meshes) {
        if (!mesh.material || materials.contains(mesh.material.get())) continue;
        const auto &material = *mesh.material;
        const bool hasNormal = std::any_of(material.parameters.cbegin(), material.parameters.cend(),
            [&](const auto &parameter) {
                const auto binding = fls::shaderTextureParameter(material, parameter);
                return binding && !parameter.texturePath.isEmpty()
                    && (*binding == hashes::kTireNormalHeightTexture
                        || fls::material_hashes::contains(hashes::kSeparateTireNormalTextures, *binding));
            });
        if (!hasNormal) continue;
        materials.insert(&material);
        try {
            requireTireTextures(material);
            require(static_cast<bool>(material.tireHeightTexture), "fixture has no loaded height map");
        } catch (const std::exception &) {
            std::fprintf(stderr, "%s [%s]\n", qPrintable(archive), qPrintable(mesh.materialName));
            throw;
        }
    }
    require(!materials.isEmpty(), "tire model has no material with authored relief");
    std::printf("%s: tire textures passed\n", qPrintable(archive));
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        testSelectionPolicy();
        const QStringList arguments = app.arguments();
        if (arguments.size() > 2 && arguments[1] == QStringLiteral("--tire-material")) {
            for (const auto &archive : arguments.sliced(2)) testTireMaterial(archive);
            return 0;
        }
        if (arguments.size() == 4 && arguments[1] == QStringLiteral("--fm2023")) {
            testMotorsportCar(arguments[2], arguments[3]);
            return 0;
        }
        require(argc > 1, "provide a car archive fixture");
        for (int index = 1; index < argc; ++index) testCar(QString::fromLocal8Bit(argv[index]));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
