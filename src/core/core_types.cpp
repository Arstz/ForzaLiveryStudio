#include "core_types.h"

#include "layer.h"
#include "material_hashes.h"

namespace fls {

const LiveryPaintMaterial *LiveryPaintState::find(quint64 materialHash) const {
    for (const LiveryPaintMaterial &material : materials) {
        if (material.materialHash == materialHash) {
            return &material;
        }
    }
    return nullptr;
}

LiveryPaintMaterial *LiveryPaintState::find(quint64 materialHash) {
    for (LiveryPaintMaterial &material : materials) {
        if (material.materialHash == materialHash) {
            return &material;
        }
    }
    return nullptr;
}

LiveryPaintMaterial &LiveryPaintState::ensure(quint64 materialHash) {
    if (LiveryPaintMaterial *material = find(materialHash)) {
        return *material;
    }
    LiveryPaintMaterial material;
    material.materialHash = materialHash;
    materials.push_back(material);

    return materials.back();
}

std::optional<std::array<quint8, 4>> LiveryPaintState::defaultCarColorBgra() const {
    const LiveryPaintMaterial *body = find(material_hashes::binding::kBodyPaint);
    if (body != nullptr && body->primary.enabled) {
        return body->primary.bgra;
    }
    for (quint64 hash : material_hashes::binding::kDefaultCarPaintGroups) {
        const LiveryPaintMaterial *material = find(hash);
        if (material != nullptr && material->primary.enabled) {
            return material->primary.bgra;
        }
    }
    return std::nullopt;
}

void LiveryPaintState::setColorBgra(quint64 materialHash, bool secondary,
                                    std::array<quint8, 4> color) {
    color[3] = 255;
    LiveryPaintMaterial &material = ensure(materialHash);
    LiveryPaintColor &target = secondary ? material.secondary : material.primary;
    target.enabled = true;
    target.bgra = color;
    material.manufacturerSelector = 0xffffffffu;
}

void LiveryPaintState::setDefaultCarColorBgra(std::array<quint8, 4> color) {
    for (quint64 hash : material_hashes::binding::kDefaultCarPaintGroups) {
        setColorBgra(hash, false, color);
    }
}

Project::Project()
    : root(std::make_unique<scene::Group>()) {
    root->id = QStringLiteral("__root__");
    root->name = QStringLiteral("Project");
}

Project::~Project() = default;

Project::Project(const Project &other)
    : name(other.name)
    , sourceFolder(other.sourceFolder)
    , sourceDecPrefix(other.sourceDecPrefix)
    , sourceHeader(other.sourceHeader)
    , headerMetadata(other.headerMetadata)
    , root(other.root
               ? std::unique_ptr<scene::Group>(
                     static_cast<scene::Group *>(other.root->clone().release()))
               : std::make_unique<scene::Group>())
    , colorSwatches(other.colorSwatches)
    , horizontalGuidelines(other.horizontalGuidelines)
    , verticalGuidelines(other.verticalGuidelines)
    , isLivery(other.isLivery)
    , carId(other.carId)
    , liverySource(other.liverySource)
    , liveryPaint(other.liveryPaint) {
    if (root && root->id.isEmpty()) {
        root->id = QStringLiteral("__root__");
    }
}

Project &Project::operator=(const Project &other) {
    if (this == &other) {
        return *this;
    }
    name = other.name;
    sourceFolder = other.sourceFolder;
    sourceDecPrefix = other.sourceDecPrefix;
    sourceHeader = other.sourceHeader;
    headerMetadata = other.headerMetadata;
    root = other.root
        ? std::unique_ptr<scene::Group>(static_cast<scene::Group *>(other.root->clone().release()))
        : std::make_unique<scene::Group>();
    if (root && root->id.isEmpty()) {
        root->id = QStringLiteral("__root__");
    }
    colorSwatches = other.colorSwatches;
    horizontalGuidelines = other.horizontalGuidelines;
    verticalGuidelines = other.verticalGuidelines;
    isLivery = other.isLivery;
    carId = other.carId;
    liverySource = other.liverySource;
    liveryPaint = other.liveryPaint;
    return *this;
}

Project::Project(Project &&other) noexcept = default;
Project &Project::operator=(Project &&other) noexcept = default;

} // namespace fls
