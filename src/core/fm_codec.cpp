#include "fm_codec.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "fh6_core.h"
#include "layer.h"
#include "matrix_math.h"
#include "scene_codec.h"
#include "shape_registry.h"
#include "vinyl_decoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <variant>

namespace fh6 {
namespace {

using detail::readLeU32;

bool isFM2023LiveryImpl(const QByteArray &fileData)
{
    if (fileData.size() < 16)
        return false;
    const quint32 compSize = readLeU32(fileData, 0);
    const quint32 uncompSize = readLeU32(fileData, 4);
    Q_UNUSED(uncompSize);
    if (compSize == 0 || compSize > 1000000)
        return false;
    if (fileData.size() < static_cast<qint64>(8 + compSize))
        return false;
    if (static_cast<quint8>(fileData[8]) != 0x78)
        return false;
    if (static_cast<quint8>(fileData[9]) != 0x9c
        && static_cast<quint8>(fileData[9]) != 0x01
        && static_cast<quint8>(fileData[9]) != 0xda)
        return false;
    return true;
}

bool isRawGyvlImpl(const QByteArray &fileData)
{
    return fileData.size() >= 4
        && fileData[0] == 'g' && fileData[1] == 'y'
        && fileData[2] == 'v' && fileData[3] == 'l';
}

QByteArray readOptionalFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

Matrix3 shapeMatrixForShape(const VinylShape &shape)
{
    FlattenedLayer layer;
    layer.rotation = shape.rotation;
    layer.posX = shape.posX;
    layer.posY = shape.posY;
    layer.scaleX = shape.scaleX;
    layer.scaleY = shape.scaleY;
    layer.skew = shape.skew;
    return shapeMatrix(layer);
}

FM2023LiveryPayload readFM2023LiveryPayloadImpl(const QString &folderOrFile)
{
    const QFileInfo info(folderOrFile);
    QString dataPath;
    if (info.isDir()) {
        dataPath = QDir(folderOrFile).filePath(QStringLiteral("data"));
    } else {
        dataPath = folderOrFile;
    }

    QFile file(dataPath);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(("could not open: " + dataPath).toStdString());
    QByteArray fileData = file.readAll();
    file.close();

    if (!isFM2023LiveryImpl(fileData))
        throw std::runtime_error("not an FM2023 livery file");

    const QByteArray raw = inflateFirstContainer(fileData);

    FM2023LiveryPayload payload;
    payload.raw = raw;

    const int vlrc = raw.indexOf(QByteArray("vlrc", 4));
    if (vlrc >= 0 && vlrc + 0x14 <= raw.size()) {
        payload.carId = static_cast<int>(readLeU32(raw, vlrc + 0x10));
    }

    const int gyvl = raw.indexOf(QByteArray("gyvl", 4));
    if (gyvl < 0) {
        throw std::runtime_error("FM2023 livery has no embedded gyvl chunk");
    }
    payload.gyvlOffset = gyvl;
    const int bodyStart = gyvl + 0x15;
    int bodyEnd = raw.indexOf(QByteArray("yrvl", 4), gyvl);
    if (bodyEnd < 0 || bodyEnd < bodyStart)
        bodyEnd = raw.size();
    if (bodyStart >= raw.size())
        throw std::runtime_error("FM2023 livery gyvl body is truncated");
    payload.gyvlBody = raw.mid(bodyStart, bodyEnd - bodyStart);

    // Try reading section counts from Container 3 (multi-container format).
    // If the file is single-container, fall back to the yrvl stats within
    // Container 1 (same position as FH6 — right after gyvl).
    const quint32 container1CompSize = readLeU32(fileData, 0);
    const int container1Total = 8 + static_cast<int>(container1CompSize);

    payload.sectionCounts.reserve(7);

    // Check if there are additional containers beyond Container 1
    if (container1Total + 8 + 8 <= fileData.size()) {
        // Multi-container format: skip Container 2, decompress Container 3
        const int c2Start = container1Total;
        const quint32 compSize2 = readLeU32(fileData, c2Start);
        const int c3Start = c2Start + 8 + static_cast<int>(compSize2);
        if (c3Start + 8 <= fileData.size()) {
            const QByteArray container3 = fileData.mid(c3Start);
            const QByteArray metaRaw = inflateFirstContainer(container3);

            const int metaYrvl = metaRaw.indexOf(QByteArray("yrvl", 4));
            if (metaYrvl >= 0) {
                for (int i = 0; i < 7; ++i) {
                    const int off = metaYrvl + 4 + i * 4;
                    if (off + 4 <= metaRaw.size())
                        payload.sectionCounts.push_back(static_cast<int>(readLeU32(metaRaw, off)));
                    else
                        payload.sectionCounts.push_back(0);
                }
                return payload;
            }
        }
    }

    // Fallback: read 7 section counts from Container 1's yrvl stats (FH6-style).
    // The yrvl stats chunk follows right after the gyvl body at bodyEnd.
    {
        const int statsTag = bodyEnd;
        if (statsTag >= 0 && raw.mid(statsTag, 4) == QByteArray("yrvl", 4)) {
            for (int i = 0; i < 7; ++i) {
                const int off = statsTag + 4 + i * 4;
                if (off + 4 <= raw.size())
                    payload.sectionCounts.push_back(static_cast<int>(readLeU32(raw, off)));
                else
                    payload.sectionCounts.push_back(0);
            }
        } else {
            for (int i = 0; i < 7; ++i)
                payload.sectionCounts.push_back(0);
        }
    }

    return payload;
}

Matrix3 groupNodeMatrix(const VinylGroup &node)
{
    constexpr double Pi = 3.14159265358979323846;
    const double radians = node.rot * Pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return affine(c * node.sx, -s * node.sy, node.px,
                  s * node.sx, c * node.sy, node.py);
}

Project importFM2023Livery(const QString &folderOrFile, const QByteArray &fileData)
{
    Q_UNUSED(fileData);
    const FM2023LiveryPayload payload = readFM2023LiveryPayloadImpl(folderOrFile);

    const QVector<LiverySection> sections =
        VinylTreeDecoder{}.buildLiverySections(payload.gyvlBody, payload.sectionCounts,
                                                kFM2023LiverySlots, kFM2023SectionCount);

    Project project;
    const QFileInfo info(folderOrFile);
    if (info.isDir()) {
        project.name = info.fileName();
        project.sourceFolder = info.absoluteFilePath();
    } else {
        project.name = info.absoluteDir().dirName();
        project.sourceFolder = info.absoluteDir().absolutePath();
    }
    project.isLivery = true;
    project.carId = payload.carId;
    project.liverySource = payload.raw;
    project.sourceDecPrefix = payload.raw.left(0x1d);
    project.sourceHeader = readOptionalFile(QDir(project.sourceFolder).filePath(QStringLiteral("header")));
    scene::ensureProjectSceneRoot(project);

    int shapeIndex = 0;
    int groupIndex = 0;

    std::function<std::unique_ptr<scene::Shape>(const VinylShape &, const Matrix3 &, bool)> importShape =
        [&](const VinylShape &shape, const Matrix3 &parentMatrix, bool parentMask) -> std::unique_ptr<scene::Shape> {
            ++shapeIndex;
            const Matrix3 effective = detail::multiply(parentMatrix, shapeMatrixForShape(shape));
            auto s = std::make_unique<scene::Shape>();
            s->id = QStringLiteral("layer-%1").arg(shapeIndex, 4, 10, QLatin1Char('0'));
            s->name = QStringLiteral("%1 %2")
                .arg(shapeIndex, 4, 10, QLatin1Char('0'))
                .arg(shape.isLogo
                         ? QStringLiteral("Decal %1").arg(shape.rasterId)
                         : detail::shapeName(shape.shapeId));
            s->transform = decomposeTransform2D(effective);
            s->hasSourceTransform = true;
            s->sourceTransform = s->transform;
            s->setVectorShape(shape.shapeId);
            if (shape.isLogo)
                s->setRasterShape(shape.rasterId);
            s->color = shape.color;
            s->opacity = static_cast<double>(shape.color[3]) / 255.0;
            s->mask = parentMask || shape.isMask;
            if (hasColorData(shape.color))
                s->mask = false;
            s->visible = true;
            s->sourceShape = shapeIndex;
            s->absOffset = shape.absPos;
            s->marker = shape.marker;
            s->flags = shape.flags;
            s->sourceLogoId = shape.logoId;
            return s;
        };

    std::function<std::unique_ptr<scene::Group>(const VinylGroup &, const Matrix3 &, bool)> importGroup =
        [&](const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask) -> std::unique_ptr<scene::Group> {
            ++groupIndex;
            const Matrix3 gMatrix = detail::multiply(parentMatrix, groupNodeMatrix(node));
            const bool inheritedMask = parentMask || node.isMask;

            auto g = std::make_unique<scene::Group>();
            g->id = QStringLiteral("group-%1").arg(groupIndex, 4, 10, QLatin1Char('0'));
            g->name = QStringLiteral("Group %1").arg(groupIndex);
            g->sourceAbsPos = node.absPos;
            g->pendingTransformMarker = node.pendingTransformMarker;
            g->inlineTransformMarker = node.inlineTransformMarker;
            g->effectiveTransformMarker = node.effectiveTransformMarker;
            g->headerControlBytes = node.headerControlBytes;
            g->flags = node.flags;

            for (const VinylItem &item : node.items) {
                if (item.isShape())
                    g->append(importShape(std::get<VinylShape>(item.value), gMatrix, inheritedMask));
                else
                    g->append(importGroup(*std::get<VinylGroupPtr>(item.value), gMatrix, inheritedMask));
            }
            for (const auto &child : g->children)
                g->sourceChildren.push_back(child->id);
            return g;
        };

    for (const LiverySection &section : sections) {
        ++groupIndex;
        auto sectionGroup = std::make_unique<scene::Group>();
        sectionGroup->id = QStringLiteral("group-%1").arg(groupIndex, 4, 10, QLatin1Char('0'));
        sectionGroup->name = section.name;
        sectionGroup->isLiverySection = true;
        sectionGroup->liverySectionSlot = section.slot;
        sectionGroup->sourceAbsPos = section.absPos;

        if (section.populated) {
            Matrix3 sectionMatrix = groupNodeMatrix(section.subtree);
            bool sectionMask = section.subtree.isMask;
            const VinylGroup *sectionRoot = &section.subtree;
            while (sectionRoot->items.size() == 1 && !sectionRoot->items.front().isShape()) {
                const auto topGroup = std::get<VinylGroupPtr>(sectionRoot->items.front().value);
                if (!topGroup) break;
                sectionMatrix = detail::multiply(sectionMatrix, groupNodeMatrix(*topGroup));
                sectionMask = sectionMask || topGroup->isMask;
                sectionRoot = topGroup.get();
            }
            for (const VinylItem &item : sectionRoot->items) {
                if (item.isShape())
                    sectionGroup->append(importShape(std::get<VinylShape>(item.value), sectionMatrix, sectionMask));
                else
                    sectionGroup->append(importGroup(*std::get<VinylGroupPtr>(item.value), sectionMatrix, sectionMask));
            }
            for (const auto &child : sectionGroup->children)
                sectionGroup->sourceChildren.push_back(child->id);
        }
        project.root->append(std::move(sectionGroup));
    }
    return project;
}

Project importFM2023RawGroup(const QString &folderOrFile, const QByteArray &fileData)
{
    const QByteArray &payload = fileData;

    const LayerData layerData = getLayerData(payload);
    const VinylGroup root = buildTree(layerData.data, payload);

    Project project;
    const QFileInfo info(folderOrFile);
    if (info.isDir()) {
        project.name = info.fileName();
        project.sourceFolder = info.absoluteFilePath();
    } else {
        project.name = info.absoluteDir().dirName();
        project.sourceFolder = info.absoluteDir().absolutePath();
    }
    project.sourceDecPrefix = payload.left(0x1d);
    project.sourceHeader = readOptionalFile(QDir(project.sourceFolder).filePath(QStringLiteral("header")));
    scene::ensureProjectSceneRoot(project);

    int shapeIndex = 0;
    int groupIndex = 0;

    std::function<std::unique_ptr<scene::Shape>(const VinylShape &, const Matrix3 &, bool)> importShape =
        [&](const VinylShape &shape, const Matrix3 &parentMatrix, bool parentMask) -> std::unique_ptr<scene::Shape> {
            ++shapeIndex;
            const Matrix3 effective = detail::multiply(parentMatrix, shapeMatrixForShape(shape));
            auto s = std::make_unique<scene::Shape>();
            s->id = QStringLiteral("layer-%1").arg(shapeIndex, 4, 10, QLatin1Char('0'));
            s->name = QStringLiteral("%1 %2")
                .arg(shapeIndex, 4, 10, QLatin1Char('0'))
                .arg(shape.isLogo
                         ? QStringLiteral("Decal %1").arg(shape.rasterId)
                         : detail::shapeName(shape.shapeId));
            s->transform = decomposeTransform2D(effective);
            s->hasSourceTransform = true;
            s->sourceTransform = s->transform;
            s->setVectorShape(shape.shapeId);
            if (shape.isLogo)
                s->setRasterShape(shape.rasterId);
            s->color = shape.color;
            s->opacity = static_cast<double>(shape.color[3]) / 255.0;
            s->mask = parentMask || shape.isMask;
            if (hasColorData(shape.color))
                s->mask = false;
            s->visible = true;
            s->sourceShape = shapeIndex;
            s->absOffset = shape.absPos + layerData.start;
            s->marker = shape.marker;
            s->flags = shape.flags;
            s->sourceLogoId = shape.logoId;
            return s;
        };

    std::function<std::unique_ptr<scene::Group>(const VinylGroup &, const Matrix3 &, bool, const QString &, const QString &)> importGroup =
        [&](const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask,
            const QString &sourceParentId, const QString &sourcePreviousSiblingId) -> std::unique_ptr<scene::Group> {
            ++groupIndex;
            const Matrix3 gMatrix = detail::multiply(parentMatrix, groupNodeMatrix(node));
            const bool inheritedMask = parentMask || node.isMask;

            auto g = std::make_unique<scene::Group>();
            g->id = QStringLiteral("group-%1").arg(groupIndex, 4, 10, QLatin1Char('0'));
            g->name = QStringLiteral("Group %1").arg(groupIndex);
            g->sourceAbsPos = node.absPos + layerData.start;
            g->pendingTransformMarker = node.pendingTransformMarker;
            g->inlineTransformMarker = node.inlineTransformMarker;
            g->effectiveTransformMarker = node.effectiveTransformMarker;
            g->headerControlBytes = node.headerControlBytes;
            g->flags = node.flags;
            g->sourceParentId = sourceParentId;
            g->sourcePreviousSiblingId = sourcePreviousSiblingId;

            for (const VinylItem &item : node.items) {
                const QString previousSiblingId = g->children.empty() ? QString() : g->children.back()->id;
                if (item.isShape())
                    g->append(importShape(std::get<VinylShape>(item.value), gMatrix, inheritedMask));
                else
                    g->append(importGroup(*std::get<VinylGroupPtr>(item.value), gMatrix, inheritedMask,
                                           g->id, previousSiblingId));
            }
            for (const auto &child : g->children)
                g->sourceChildren.push_back(child->id);
            return g;
        };

    const Matrix3 rootMatrix = groupNodeMatrix(root);
    const bool rootMask = root.isMask;
    for (const VinylItem &item : root.items) {
        const QString previousSiblingId = project.root->children.empty() ? QString() : project.root->children.back()->id;
        if (item.isShape())
            project.root->append(importShape(std::get<VinylShape>(item.value), rootMatrix, rootMask));
        else
            project.root->append(importGroup(*std::get<VinylGroupPtr>(item.value),
                                              rootMatrix, rootMask,
                                              QStringLiteral("__root__"),
                                              previousSiblingId));
    }
    return project;
}

} // anonymous namespace

const LiverySlotDef kFM2023LiverySlots[7] = {
    {"Front", 0.0},  {"Back", 0.0},  {"Top", 0.0},
    {"Left", 180.0}, {"Right", 90.0},{"Spoiler", -90.0},
    {"FrontWindshield", 90.0},
};

} // namespace fh6

bool fh6::isFM2023Livery(const QByteArray &fileData)
{
    return isFM2023LiveryImpl(fileData);
}

bool fh6::isRawGyvl(const QByteArray &fileData)
{
    return isRawGyvlImpl(fileData);
}

fh6::FM2023LiveryPayload fh6::readFM2023LiveryPayload(const QString &folderOrFile)
{
    return readFM2023LiveryPayloadImpl(folderOrFile);
}

fh6::Project fh6::importFM2023Asset(const QString &folderOrFile)
{
    const QFileInfo info(folderOrFile);
    QString dataPath;
    if (info.isDir()) {
        dataPath = QDir(folderOrFile).filePath(QStringLiteral("data"));
    } else {
        dataPath = folderOrFile;
    }

    QFile file(dataPath);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(("could not open: " + dataPath).toStdString());
    QByteArray fileData = file.readAll();
    file.close();

    if (isFM2023LiveryImpl(fileData)) {
        return importFM2023Livery(folderOrFile, fileData);
    } else if (isRawGyvlImpl(fileData)) {
        return importFM2023RawGroup(folderOrFile, fileData);
    }
    throw std::runtime_error("not a recognized FM2023 asset format");
}
