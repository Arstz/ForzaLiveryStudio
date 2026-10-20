#include "vinyl_scene_import.h"

#include "layer.h"
#include "matrix_math.h"
#include "scene_codec.h"
#include "shape_registry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

namespace fls::detail {
namespace {

enum class ImportKind { Group, Livery };

QString indexedId(const char *prefix, int index) {
    return QStringLiteral("%1-%2").arg(QLatin1String(prefix)).arg(index, 4, 10, QLatin1Char('0'));
}

QByteArray effectiveDebugMarker(const VinylGroup &node) {
    if (!node.pendingTransformMarker.isEmpty()
        && !node.inlineTransformMarker.isEmpty()
        && node.effectiveTransformMarker == node.pendingTransformMarker + node.inlineTransformMarker) {
        return {};
    }

    return node.effectiveTransformMarker;
}

int importedGroupTerminalDepth(const QHash<QString, const scene::Group *> &groupsById,
                               const QString &groupId) {
    const scene::Group *group = groupsById.value(groupId, nullptr);

    if (group == nullptr) {
        return 0;
    }
    if (group->children.empty() || group->children.back()->kind() != scene::LayerKind::Group) {
        return 1;
    }

    return 1 + importedGroupTerminalDepth(groupsById, group->children.back()->id);
}

class VinylSceneImporter {
public:
    VinylSceneImporter(VinylSceneDialect dialect, ImportKind kind, int sourceOffset);
    void appendRoot(scene::Group &destination, const VinylGroup &root);
    void appendSections(scene::Group &destination, const QVector<LiverySection> &sections);

private:
    bool shapeMask(const VinylShape &shape, bool parentMask) const;
    std::unique_ptr<scene::Shape> importShape(const VinylShape &shape,
                                             const Matrix3 &parentMatrix, bool parentMask);
    std::unique_ptr<scene::Group> importGroup(const VinylGroup &node,
                                             const Matrix3 &parentMatrix, bool parentMask,
                                             const QString &parentId, const QString &previousSiblingId);
    void appendItems(scene::Group &destination, const QVector<VinylItem> &items, int begin,
                     const Matrix3 &matrix, bool mask);
    void appendSectionItems(scene::Group &destination, const LiverySection &section);
    void retainSourceChildren(scene::Group &group) const;

    QHash<QString, const scene::Group *> groupsById_;
    VinylSceneDialect dialect_;
    ImportKind kind_;
    int sourceOffset_;
    int shapeIndex_ = 0;
    int groupIndex_ = 0;
};

VinylSceneImporter::VinylSceneImporter(VinylSceneDialect dialect, ImportKind kind, int sourceOffset)
    : dialect_(dialect), kind_(kind), sourceOffset_(sourceOffset) {}

void VinylSceneImporter::appendRoot(scene::Group &destination, const VinylGroup &root) {
    appendItems(destination, root.items, 0, vinylGroupMatrix(root), root.isMask);
}

void VinylSceneImporter::appendSections(scene::Group &destination, const QVector<LiverySection> &sections) {
    for (const LiverySection &section : sections) {
        auto group = std::make_unique<scene::Group>();
        group->id = indexedId("group", ++groupIndex_);
        group->name = section.name;
        group->isLiverySection = true;
        group->liverySectionSlot = section.slot;
        group->sourceAbsPos = section.absPos;
        if (section.populated) {
            appendSectionItems(*group, section);
            retainSourceChildren(*group);
        }
        destination.append(std::move(group));
    }
}

bool VinylSceneImporter::shapeMask(const VinylShape &shape, bool parentMask) const {
    const bool colored = hasColorData(shape.color);

    if (dialect_ == VinylSceneDialect::Horizon) {
        return parentMask || (shape.isMask && !colored);
    }
    if (kind_ == ImportKind::Livery) {
        return (parentMask || shape.isMask) && !colored;
    }

    return (parentMask || shape.isMask) && !(colored && !shape.isMask);
}

std::unique_ptr<scene::Shape> VinylSceneImporter::importShape(
    const VinylShape &shape, const Matrix3 &parentMatrix, bool parentMask) {
    const Matrix3 effective = multiply(parentMatrix, shapeMatrix(shape));
    auto result = std::make_unique<scene::Shape>();

    ++shapeIndex_;
    result->id = indexedId("layer", shapeIndex_);
    result->name = QStringLiteral("%1 %2")
        .arg(shapeIndex_, 4, 10, QLatin1Char('0'))
        .arg(shape.isLogo ? QStringLiteral("Decal %1").arg(shape.rasterId) : shapeName(shape.shapeId));
    result->transform = decomposeTransform2D(effective);
    result->hasSourceTransform = true;
    result->sourceTransform = result->transform;
    if (!shape.isLogo || dialect_ == VinylSceneDialect::Motorsport2023) {
        result->setVectorShape(shape.shapeId);
    }
    if (shape.isLogo) {
        result->setRasterShape(shape.rasterId);
    }
    result->color = shape.color;
    result->opacity = static_cast<double>(shape.color[3]) / 255.0;
    result->mask = shapeMask(shape, parentMask);
    result->sourceShape = shapeIndex_;
    result->absOffset = shape.absPos + sourceOffset_;
    result->marker = shape.marker;
    result->flags = shape.flags;
    result->sourceLogoId = shape.logoId;

    return result;
}

std::unique_ptr<scene::Group> VinylSceneImporter::importGroup(
    const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask,
    const QString &parentId, const QString &previousSiblingId) {
    const Matrix3 matrix = multiply(parentMatrix, vinylGroupMatrix(node));
    auto group = std::make_unique<scene::Group>();

    group->id = indexedId("group", ++groupIndex_);
    group->name = QStringLiteral("Group %1").arg(groupIndex_);
    group->sourceAbsPos = node.absPos + sourceOffset_;
    group->pendingTransformMarker = node.pendingTransformMarker;
    group->inlineTransformMarker = node.inlineTransformMarker;
    group->effectiveTransformMarker = dialect_ == VinylSceneDialect::Horizon
        ? effectiveDebugMarker(node) : node.effectiveTransformMarker;
    group->headerControlBytes = node.headerControlBytes;
    group->flags = node.flags;
    if (kind_ == ImportKind::Group) {
        group->sourceParentId = parentId;
        group->sourcePreviousSiblingId = previousSiblingId;
        if (dialect_ == VinylSceneDialect::Horizon) {
            group->sourcePreviousGroupDepth = importedGroupTerminalDepth(groupsById_, previousSiblingId);
            groupsById_.insert(group->id, group.get());
        }
    }
    appendItems(*group, node.items, 0, matrix, parentMask || node.isMask);
    retainSourceChildren(*group);

    return group;
}

void VinylSceneImporter::appendItems(scene::Group &destination, const QVector<VinylItem> &items,
                                     int begin, const Matrix3 &matrix, bool mask) {
    for (int index = begin; index < items.size(); ++index) {
        const VinylItem &item = items[index];
        const QString previousSiblingId = destination.children.empty()
            ? QString() : destination.children.back()->id;

        if (item.isShape()) {
            destination.append(importShape(std::get<VinylShape>(item.value), matrix, mask));
        } else {
            destination.append(importGroup(*std::get<VinylGroupPtr>(item.value), matrix, mask,
                                            destination.id, previousSiblingId));
        }
    }
}

void VinylSceneImporter::appendSectionItems(scene::Group &destination, const LiverySection &section) {
    const Matrix3 canvasMatrix = liverySectionCanvasTransform(section.slot);
    Matrix3 sectionMatrix = vinylGroupMatrix(section.subtree);
    const VinylGroup *sectionRoot = &section.subtree;
    const VinylGroupPtr leadingRoot = !sectionRoot->items.isEmpty() && !sectionRoot->items.front().isShape()
        ? std::get<VinylGroupPtr>(sectionRoot->items.front().value) : VinylGroupPtr{};
    bool sectionMask = section.subtree.isMask;

    if (leadingRoot && leadingRoot->source == QStringLiteral("livery_section_root")) {
        const Matrix3 leadingMatrix = multiply(sectionMatrix, vinylGroupMatrix(*leadingRoot));
        const bool leadingMask = sectionMask || leadingRoot->isMask;
        appendItems(destination, leadingRoot->items, 0, multiply(canvasMatrix, leadingMatrix), leadingMask);
        appendItems(destination, sectionRoot->items, 1, multiply(canvasMatrix, sectionMatrix), sectionMask);
    } else {
        while (sectionRoot->items.size() == 1 && !sectionRoot->items.front().isShape()) {
            const auto topGroup = std::get<VinylGroupPtr>(sectionRoot->items.front().value);
            if (!topGroup || (dialect_ == VinylSceneDialect::Horizon
                             && topGroup->source.contains(QStringLiteral("livery_section_span")))) {
                break;
            }
            sectionMatrix = multiply(sectionMatrix, vinylGroupMatrix(*topGroup));
            sectionMask = sectionMask || topGroup->isMask;
            sectionRoot = topGroup.get();
        }
        appendItems(destination, sectionRoot->items, 0, multiply(canvasMatrix, sectionMatrix), sectionMask);
    }
}

void VinylSceneImporter::retainSourceChildren(scene::Group &group) const {
    for (const auto &child : group.children) {
        group.sourceChildren.push_back(child->id);
    }
}

} // namespace

Project newVinylImportProject(const QString &folderOrFile, const QByteArray &payload) {
    const QFileInfo info(folderOrFile);
    Project project;

    if (info.isDir()) {
        project.name = info.fileName();
        project.sourceFolder = info.absoluteFilePath();
    } else {
        project.name = info.absoluteDir().dirName();
        project.sourceFolder = info.absoluteDir().absolutePath();
    }
    project.sourceDecPrefix = payload.left(0x1d);
    QFile header(QDir(project.sourceFolder).filePath(QStringLiteral("header")));
    if (header.open(QIODevice::ReadOnly)) {
        project.sourceHeader = header.readAll();
    }

    return project;
}

void appendVinylGroupToScene(Project &project, const VinylGroup &root, int sourceOffset,
                             VinylSceneDialect dialect) {
    scene::ensureProjectSceneRoot(project);
    VinylSceneImporter{dialect, ImportKind::Group, sourceOffset}.appendRoot(*project.root, root);
}

void appendLiverySectionsToScene(Project &project, const QVector<LiverySection> &sections,
                                 VinylSceneDialect dialect) {
    scene::ensureProjectSceneRoot(project);
    VinylSceneImporter{dialect, ImportKind::Livery, 0}.appendSections(*project.root, sections);
}

} // namespace fls::detail
