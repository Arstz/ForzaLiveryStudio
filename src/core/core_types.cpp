#include "core_types.h"

#include "layer.h"

namespace fh6 {

Project::Project()
    : root(std::make_unique<scene::Group>())
{
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
    , isLivery(other.isLivery)
    , carId(other.carId)
    , liverySource(other.liverySource)
{
    if (root && root->id.isEmpty()) {
        root->id = QStringLiteral("__root__");
    }
}

Project &Project::operator=(const Project &other)
{
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
    isLivery = other.isLivery;
    carId = other.carId;
    liverySource = other.liverySource;
    return *this;
}

Project::Project(Project &&other) noexcept = default;
Project &Project::operator=(Project &&other) noexcept = default;

} // namespace fh6
