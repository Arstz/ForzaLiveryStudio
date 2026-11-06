#include "paint_finish_catalog.h"

#include "game_paths.h"
#include "material_hashes.h"
#include "model_material.h"
#include "paint_database.h"
#include "zip_extract.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace fls {
namespace {

constexpr float kMetalFallbackMetallic = 0.9f;
constexpr float kFlakeBaseMetallic = 0.45f;
constexpr float kFlakeMetallicGain = 0.5f;

using Cat = PaintFinishCategory;

const ModelMaterialParameter *materialParameter(const ModelMaterial &material, quint32 hash) {
    const auto found = std::find_if(material.parameters.crbegin(), material.parameters.crend(), [hash](const auto &parameter) {
        return parameter.nameHash == hash;
    });

    return found != material.parameters.crend() ? &*found : nullptr;
}

QString weaveMaskTexture(const ModelMaterial &material) {
    const auto *parameter = materialParameter(material, material_hashes::parameter::kWeaveMask);

    return parameter != nullptr && parameter->type == ModelMaterialParameterType::Texture2D ? parameter->texturePath : QString();
}

void applyWeaveTints(PaintFinishRender &render, const ModelMaterial &material) {
    const auto *first = materialParameter(material, material_hashes::parameter::kWeaveColorTintA);
    const auto *second = materialParameter(material, material_hashes::parameter::kWeaveColorTintB);
    const auto *userSwitch = materialParameter(material, material_hashes::parameter::kUserColorSwitch);
    const auto *userColor = materialParameter(material, material_hashes::parameter::kUserCarColor);
    const bool useUserColor = userSwitch != nullptr && userSwitch->boolean && userColor != nullptr;

    if (first == nullptr || second == nullptr) return;
    for (int channel = 0; channel < 3; ++channel) {
        render.weaveColorTintA[channel] = useUserColor ? userColor->vector[channel] * 0.1f : first->vector[channel];
        render.weaveColorTintB[channel] = second->vector[channel];
    }
}

void resolveWeaveDefaults(QHash<int, std::shared_ptr<ModelMaterial>> &materials, const QString &gameFolder) {
    struct ShaderSource {
        QString archive;
        QString entry;
    };
    QHash<int, ShaderSource> sources;
    QHash<QString, QStringList> requests;
    QHash<QString, std::shared_ptr<ModelMaterial>> defaults;

    for (auto material = materials.cbegin(); material != materials.cend(); ++material) {
        if (weaveMaskTexture(*material.value()).isEmpty()) continue;
        for (QString link : material.value()->linkedPaths) {
            link.replace(QLatin1Char('\\'), QLatin1Char('/'));
            const QString prefix = QStringLiteral("/_library/shaders/");
            const int start = link.indexOf(prefix, 0, Qt::CaseInsensitive);
            if (start < 0 || !link.endsWith(QStringLiteral(".shaderbin"), Qt::CaseInsensitive)) continue;
            const QString relative = link.mid(start + prefix.size());
            const int separator = relative.indexOf(QLatin1Char('/'));
            if (separator < 0) continue;
            const QString archive = QDir(gameCarsDir(gameFolder)).filePath(
                QStringLiteral("_library/shaders/") + relative.left(separator) + QStringLiteral(".zip"));
            const QString entry = relative.mid(separator + 1).toLower();
            sources.insert(material.key(), {archive, entry});
            if (!requests[archive].contains(entry)) requests[archive].push_back(entry);
            break;
        }
    }
    for (auto request = requests.cbegin(); request != requests.cend(); ++request) {
        if (!QFileInfo::exists(request.key())) continue;
        const auto blobs = readZipEntries(request.key(), request.value());
        for (const QString &entry : request.value()) {
            if (const auto decoded = decodeMaterialBundle(blobs.value(entry))) defaults.insert(request.key() + entry, decoded);
        }
    }
    for (auto source = sources.cbegin(); source != sources.cend(); ++source) {
        const auto materialDefaults = defaults.value(source->archive + source->entry);
        if (materialDefaults) materials[source.key()] = mergeModelMaterialDefaults(*materialDefaults, *materials.value(source.key()));
    }
}

int finishColorCount(const PaintFinishInfo &info) {
    static const QHash<int, int> counts = [] {
        QHash<int, int> result;
        const QJsonArray entries = gamePaintDatabase(QStringLiteral("FH6")).value(QStringLiteral("finishes")).toArray();
        for (const auto &value : entries) {
            const QJsonObject entry = value.toObject();
            result.insert(entry.value(QStringLiteral("ID")).toInt(), entry.value(QStringLiteral("NumInputColors")).toInt(1));
        }

        return result;
    }();

    return counts.value(info.code, info.category == PaintFinishCategory::Pattern ? 0 : 1);
}

QString textureLeaf(const QString &path) {
    QString value = path;
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));

    return value.mid(value.lastIndexOf(QLatin1Char('/')) + 1);
}

bool isOpaqueWhite(const SwatchImage &image) {
    return image.valid()
        && std::all_of(image.rgba.cbegin(), image.rgba.cend(),
                       [](uint8_t channel) { return channel == 255; });
}

PaintFinishRender renderFromMaterial(const PaintFinishInfo &info, const ModelMaterial &material) {
    PaintFinishRender render;
    render.valid = true;
    render.category = info.category;
    render.usesSecondary = info.category == PaintFinishCategory::TwoTone && finishColorCount(info) > 1;
    render.selfColored = finishColorCount(info) == 0;
    render.gloss = material.gloss;
    render.metallic = material.hasMetallic
        ? material.metallic
        : (info.category == PaintFinishCategory::Metal ? kMetalFallbackMetallic : 0.0f);
    render.flakeAmount = material.flakeAmount;
    // Flake (metallic/glitter/candy) paints read as metallic sparkle even without an F0.
    if (info.category == PaintFinishCategory::Solid && material.flakeAmount > 0.0f) {
        render.metallic = std::max(render.metallic, kFlakeBaseMetallic + kFlakeMetallicGain * material.flakeAmount);
    }
    render.materialColor = material.baseColor;
    render.hasMaterialColor = material.hasBaseColor;

    return render;
}

// Indexes every .swatchbin entry in the textures archive by its lower-cased leaf name.
QHash<QString, QString> buildSwatchIndex(const QString &texturesArchive) {
    QHash<QString, QString> index;
    if (!QFileInfo::exists(texturesArchive)) {
        return index;
    }
    const QStringList entries = listZipEntries(texturesArchive);
    for (const QString &entry : entries) {
        if (entry.endsWith(QStringLiteral(".swatchbin"), Qt::CaseInsensitive)) {
            index.insert(textureLeaf(entry).toLower(), entry);
        }
    }

    return index;
}

QHash<int, std::shared_ptr<ModelMaterial>> decodeMaterialsFromArchive(const QString &archivePath) {
    const QString prefix = gamePaintMaterialsPrefix();
    QStringList entries;
    entries.reserve(paintFinishTable().size());
    for (const PaintFinishInfo &info : paintFinishTable()) {
        entries.push_back(prefix + info.materialBase + QStringLiteral(".materialbin"));
    }
    const QHash<QString, QByteArray> blobs = readZipEntries(archivePath, entries);
    QHash<int, std::shared_ptr<ModelMaterial>> materials;
    for (const PaintFinishInfo &info : paintFinishTable()) {
        const QByteArray blob = blobs.value((prefix + info.materialBase + QStringLiteral(".materialbin")).toLower());
        if (blob.isEmpty()) {
            continue;
        }
        if (const std::shared_ptr<ModelMaterial> material = decodeMaterialBundle(blob)) {
            materials.insert(info.code, material);
        }
    }

    return materials;
}

QHash<int, std::shared_ptr<ModelMaterial>> decodeMaterialsFromDirectory(const QString &directory) {
    const QDir dir(directory);
    QHash<int, std::shared_ptr<ModelMaterial>> materials;
    for (const PaintFinishInfo &info : paintFinishTable()) {
        QFile file(dir.filePath(info.materialBase + QStringLiteral(".materialbin")));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        if (const std::shared_ptr<ModelMaterial> material = decodeMaterialBundle(file.readAll())) {
            materials.insert(info.code, material);
        }
    }

    return materials;
}

QVector<PaintFinishInfo> buildTable() {
    return {
        {1, QStringLiteral("Gloss"), QStringLiteral("normalpaint"), Cat::Solid},
        {2, QStringLiteral("Semigloss"), QStringLiteral("normalpaintsemigloss"), Cat::Solid},
        {3, QStringLiteral("Matte"), QStringLiteral("normalpaintmatte"), Cat::Solid},
        {4, QStringLiteral("Metallic"), QStringLiteral("metallicpaint"), Cat::Solid},
        {6, QStringLiteral("Carbon Fiber Matte"), QStringLiteral("carbonfibermatte"), Cat::Pattern},
        {7, QStringLiteral("Carbon Fiber Polished"), QStringLiteral("carbonfiberpolished"), Cat::Pattern},
        {8, QStringLiteral("Carbon Kevlar Matte"), QStringLiteral("carbonkevlarsquareweavematte"), Cat::Pattern},
        {9, QStringLiteral("Carbon Kevlar Polished"), QStringLiteral("carbonkevlarsquareweavepolished"), Cat::Pattern},
        {12, QStringLiteral("Chrome"), QStringLiteral("chromiumpolished"), Cat::Metal},
        {13, QStringLiteral("Gold"), QStringLiteral("goldpolished"), Cat::Metal},
        {14, QStringLiteral("Aluminum Brushed"), QStringLiteral("aluminumbrushed"), Cat::Metal},
        {16, QStringLiteral("Aluminum Polished"), QStringLiteral("aluminumpolished"), Cat::Metal},
        {17, QStringLiteral("Aluminum Semigloss"), QStringLiteral("aluminumsemigloss"), Cat::Metal},
        {20, QStringLiteral("Blob Camo Desert Matte"), QStringLiteral("camoclassicdesertmatte"), Cat::Pattern},
        {21, QStringLiteral("Blob Camo Desert Polished"), QStringLiteral("camoclassicdesertpolished"), Cat::Pattern},
        {22, QStringLiteral("Blob Camo Snow Matte"), QStringLiteral("camoclassicsnowmatte"), Cat::Pattern},
        {23, QStringLiteral("Blob Camo Snow Polished"), QStringLiteral("camoclassicsnowpolished"), Cat::Pattern},
        {24, QStringLiteral("Blob Camo Woodland Matte"), QStringLiteral("camoclassicwoodlandmatte"), Cat::Pattern},
        {25, QStringLiteral("Blob Camo Woodland Polished"), QStringLiteral("camoclassicwoodlandpolished"), Cat::Pattern},
        {26, QStringLiteral("Brass Brushed"), QStringLiteral("brassbrushed"), Cat::Metal},
        {27, QStringLiteral("Brass Polished"), QStringLiteral("brasspolished"), Cat::Metal},
        {28, QStringLiteral("Brass Semigloss"), QStringLiteral("brasssemigloss"), Cat::Metal},
        {31, QStringLiteral("Copper Brushed"), QStringLiteral("copperbrushed"), Cat::Metal},
        {32, QStringLiteral("Copper Polished"), QStringLiteral("copperpolished"), Cat::Metal},
        {33, QStringLiteral("Copper Semigloss"), QStringLiteral("coppersemigloss"), Cat::Metal},
        {36, QStringLiteral("Digital Camo Desert Matte"), QStringLiteral("camodigitaldesertmatte"), Cat::Pattern},
        {37, QStringLiteral("Digital Camo Desert Polished"), QStringLiteral("camodigitaldesertpolished"), Cat::Pattern},
        {38, QStringLiteral("Digital Camo Snow Matte"), QStringLiteral("camodigitalsnowmatte"), Cat::Pattern},
        {39, QStringLiteral("Digital Camo Snow Polished"), QStringLiteral("camodigitalsnowpolished"), Cat::Pattern},
        {40, QStringLiteral("Digital Camo Woodland Matte"), QStringLiteral("camodigitalwoodlandmatte"), Cat::Pattern},
        {41, QStringLiteral("Digital Camo Woodland Polished"), QStringLiteral("camodigitalwoodlandpolished"), Cat::Pattern},
        {42, QStringLiteral("Spyshot Swirls"), QStringLiteral("camospyshotswirl"), Cat::Pattern},
        {43, QStringLiteral("Spyshot Triangles"), QStringLiteral("camospyshottriangles"), Cat::Pattern},
        {44, QStringLiteral("Steel Brushed"), QStringLiteral("steelbrushed"), Cat::Metal},
        {45, QStringLiteral("Diamond Plate"), QStringLiteral("steeldiamondplate"), Cat::Metal},
        {46, QStringLiteral("Steel Polished"), QStringLiteral("steelpolished"), Cat::Metal},
        {47, QStringLiteral("Steel Semigloss"), QStringLiteral("steelsemigloss"), Cat::Metal},
        {50, QStringLiteral("Two-Tone Matte"), QStringLiteral("twotonematte"), Cat::TwoTone},
        {51, QStringLiteral("Two-Tone Polished"), QStringLiteral("twotonepolished"), Cat::TwoTone},
        {52, QStringLiteral("Two-Tone Semigloss"), QStringLiteral("twotonesemigloss"), Cat::TwoTone},
        {53, QStringLiteral("Wood Dark"), QStringLiteral("wooddarkmatte"), Cat::Pattern},
        {56, QStringLiteral("Wood Light"), QStringLiteral("woodlightmatte"), Cat::Pattern},
        {59, QStringLiteral("Wood Medium"), QStringLiteral("woodmediummatte"), Cat::Pattern},
        {60, QStringLiteral("Realistic Camo Woodland Matte"), QStringLiteral("camorealforestmatte"), Cat::Pattern},
        {61, QStringLiteral("Realistic Camo Woodland Polished"), QStringLiteral("camorealforestpolished"), Cat::Pattern},
        {62, QStringLiteral("Realistic Camo Snow Polished"), QStringLiteral("camorealsnowpolished"), Cat::Pattern},
        {63, QStringLiteral("Realistic Camo Snow Matte"), QStringLiteral("camorealsnowmatte"), Cat::Pattern},
        {64, QStringLiteral("Zinc"), QStringLiteral("zinc"), Cat::Metal},
        {65, QStringLiteral("Prismacolor White"), QStringLiteral("prismacolor_white"), Cat::Pattern},
        {66, QStringLiteral("Steel Damascus"), QStringLiteral("damascus"), Cat::Metal},
        {67, QStringLiteral("Prismacolor Black"), QStringLiteral("prismacolor_black"), Cat::Pattern},
        {68, QStringLiteral("Steel Galvanized"), QStringLiteral("galvanized"), Cat::Metal},
        {69, QStringLiteral("Candy Paint"), QStringLiteral("candypaint"), Cat::Solid},
        {70, QStringLiteral("Metallic Low Flake"), QStringLiteral("metallicpaintlowflake"), Cat::Solid},
        {71, QStringLiteral("Metallic High Flake"), QStringLiteral("metallicpainthighflake"), Cat::Solid},
        {72, QStringLiteral("Metallic Glitter"), QStringLiteral("metallicglitterpaint"), Cat::Solid},
    };
}

} // namespace

const QVector<PaintFinishInfo> &paintFinishTable() {
    static const QVector<PaintFinishInfo> table = buildTable();

    return table;
}

const PaintFinishInfo *findPaintFinish(int code) {
    for (const PaintFinishInfo &info : paintFinishTable()) {
        if (info.code == code) {
            return &info;
        }
    }

    return nullptr;
}

void PaintFinishLibrary::clear() {
    folder_.clear();
    byCode_.clear();
    loaded_ = false;
    ++generation_;
}

void PaintFinishLibrary::replace(PaintFinishLibrary library) {
    folder_ = std::move(library.folder_);
    loaded_ = library.loaded_;
    byCode_ = std::move(library.byCode_);
    ++generation_;
}

void PaintFinishLibrary::load(const QString &gameFolder) {
    clear();
    folder_ = gameFolder;

    const QString materialsArchive = gamePaintMaterialsArchive(gameFolder);
    QHash<int, std::shared_ptr<ModelMaterial>> materials = QFileInfo::exists(materialsArchive)
        ? decodeMaterialsFromArchive(materialsArchive)
        : decodeMaterialsFromDirectory(gameFolder);
    if (materials.isEmpty()) {
        return;
    }
    resolveWeaveDefaults(materials, gameFolder);

    const QString texturesArchive = gamePaintTexturesArchive(gameFolder);
    const QHash<QString, QString> swatchIndex = buildSwatchIndex(texturesArchive);
    const auto swatchPath = [&](const QString &reference) -> QString {
        return reference.isEmpty() ? QString() : swatchIndex.value(textureLeaf(reference).toLower());
    };

    QStringList wanted;
    for (const std::shared_ptr<ModelMaterial> &material : materials) {
        for (const QString &reference :
             {material->patternTexture, material->detailNormalTexture, material->roughMetalAoTexture, weaveMaskTexture(*material)}) {
            const QString path = swatchPath(reference);
            if (!path.isEmpty()) {
                wanted.push_back(path);
            }
        }
    }
    const QHash<QString, QByteArray> blobs =
        wanted.isEmpty() ? QHash<QString, QByteArray>{} : readZipEntries(texturesArchive, wanted);
    const auto swatchImages = [&](const QString &reference, SwatchImage &image, std::vector<SwatchImage> &mips) {
        const QString path = swatchPath(reference);
        const QByteArray blob = path.isEmpty() ? QByteArray{} : blobs.value(path.toLower());
        auto images = blob.isEmpty() ? std::vector<SwatchImage>{} : decodeSwatchMipImages(blob);

        if (images.empty()) return;
        image = std::move(images.front());
        mips.assign(std::make_move_iterator(images.begin() + 1), std::make_move_iterator(images.end()));
    };

    for (const PaintFinishInfo &info : paintFinishTable()) {
        const auto it = materials.constFind(info.code);
        if (it == materials.constEnd()) {
            continue;
        }
        const ModelMaterial &material = *it.value();
        PaintFinishRender render = renderFromMaterial(info, material);
        swatchImages(material.patternTexture, render.patternImage, render.patternMips);
        swatchImages(material.detailNormalTexture, render.detailNormalImage, render.detailNormalMips);
        swatchImages(material.roughMetalAoTexture, render.roughMetalAoImage, render.roughMetalAoMips);
        render.tiling = {material.uTiling, material.vTiling};
        const QString weaveMask = weaveMaskTexture(material);
        if (!weaveMask.isEmpty()) {
            swatchImages(weaveMask, render.weaveMaskImage, render.weaveMaskMips);
            applyWeaveTints(render, material);
            render.selfColored = true;
            render.uvChannel = 1;
            const auto scalar = [&](quint32 hash, float fallback) {
                const auto *parameter = materialParameter(material, hash);

                return parameter != nullptr && parameter->type == ModelMaterialParameterType::Float
                    ? std::clamp(parameter->scalar, 0.0f, 1.0f) : fallback;
            };
            using namespace material_hashes::parameter;
            render.metallic = scalar(kWeaveMetalness, render.metallic);
            render.gloss = 1.0f - scalar(kWeaveRoughness, 1.0f - render.gloss);
            render.normalIntensity = scalar(kWeaveNormalIntensity, render.normalIntensity);
            render.anisotropy = scalar(kAnisotropy, render.anisotropy);
            render.clearCoatCoverage = scalar(kClearCoatCoverage, 1.0f);
            render.clearCoatRoughness = scalar(kClearcoatRoughness, 1.0f - material.gloss);
            render.uvOrientationDegrees = material.uvOrientationDegrees;
        }
        if (isOpaqueWhite(render.roughMetalAoImage)) {
            render.roughMetalAoImage = {};
            render.roughMetalAoMips.clear();
        }
        byCode_.insert(info.code, render);
    }
    loaded_ = !byCode_.isEmpty();
}

const PaintFinishRender *PaintFinishLibrary::find(int code) const {
    const auto it = byCode_.constFind(code);

    return it != byCode_.constEnd() ? &it.value() : nullptr;
}

} // namespace fls
