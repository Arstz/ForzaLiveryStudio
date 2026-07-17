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
#include <cmath>
#include <functional>
#include <optional>
#include <stdexcept>
#include <variant>

namespace fh6 {
namespace {

using detail::readLeFloat;
using detail::readLeU16;
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

QByteArray inflateFM2023Blocks(const QByteArray &fileData)
{
    QByteArray raw;
    int pos = 0;
    while (pos < fileData.size()) {
        if (pos + 8 > fileData.size())
            throw std::runtime_error("FM2023 livery block header is truncated");
        const quint32 compressedSize = readLeU32(fileData, pos);
        const quint32 decompressedSize = readLeU32(fileData, pos + 4);
        if (compressedSize == 0 || decompressedSize == 0
            || compressedSize > static_cast<quint32>(fileData.size() - pos - 8)) {
            throw std::runtime_error("FM2023 livery block size is invalid");
        }
        raw.append(inflateFirstContainer(fileData.mid(pos, 8 + static_cast<int>(compressedSize))));
        pos += 8 + static_cast<int>(compressedSize);
    }
    return raw;
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

bool isFM2023ShapePayloadAt(const QByteArray &data, int idPos, int transformPos, int endPos)
{
    if (idPos < 0 || endPos > data.size())
        return false;
    const quint16 encodedShapeId = readLeU16(data, idPos);
    const quint16 shapeId = encodedShapeId == 0x0bb8
        ? 0x0bb9 : detail::canonicalShapeId(encodedShapeId);
    if (!detail::isKnownShapeId(shapeId) && encodedShapeId < 0x8000)
        return false;
    const double rotation = readLeFloat(data, transformPos);
    const double px = readLeFloat(data, transformPos + 4);
    const double py = readLeFloat(data, transformPos + 8);
    const double sx = readLeFloat(data, transformPos + 12);
    const double sy = readLeFloat(data, transformPos + 16);
    const double skew = readLeFloat(data, transformPos + 20);
    return std::isfinite(rotation) && std::abs(rotation) <= 10000.0
        && std::abs(px) < 50000.0 && std::abs(py) < 50000.0
        && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0
        && std::abs(sy) > 1e-6 && std::abs(sy) < 5000.0
        && std::isfinite(skew) && std::abs(skew) < 200.0;
}

bool isFM2023BareShapeAt(const QByteArray &data, int pos)
{
    if (pos < 0 || pos + 31 > data.size())
        return false;
    const quint8 marker = static_cast<quint8>(data[pos]);
    return (marker == 0x01 || marker == 0x02 || marker == 0x03)
        && isFM2023ShapePayloadAt(data, pos + 1, pos + 3, pos + 31);
}

bool isFM2023GroupAt(const QByteArray &data, int pos, bool counted)
{
    const int headerSize = counted ? 4 : 3;
    if (pos < 0 || pos + headerSize > data.size())
        return false;
    if (counted && static_cast<quint8>(data[pos]) != 0x20
        && static_cast<quint8>(data[pos]) != 0x60) {
        return false;
    }
    const int countPos = pos + (counted ? 1 : 0);
    const int count = readLeU16(data, countPos);
    const int childBlocks = static_cast<quint8>(data[countPos + 2]);
    return count > 0 && childBlocks == (count + 7) / 8
        && pos + headerSize + childBlocks + 2 <= data.size();
}

int fm2023TransformSizeAt(const QByteArray &data, int pos)
{
    if (pos < 0 || pos + 17 > data.size())
        return 0;
    const quint8 marker = static_cast<quint8>(data[pos]);
    if (marker != 0x01 && marker != 0x02)
        return 0;
    const double px = readLeFloat(data, pos + 1);
    const double py = readLeFloat(data, pos + 5);
    const double sx = readLeFloat(data, pos + 9);
    const double rotation = readLeFloat(data, pos + 13);
    if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(sx)
        || !std::isfinite(rotation) || std::abs(px) >= 50000.0
        || std::abs(py) >= 50000.0 || std::abs(sx) < 0.0001
        || std::abs(sx) > 200.0 || std::abs(rotation) > 10000.0) {
        return 0;
    }
    int size = 17;
    const int syPos = pos + size;
    if (syPos + 5 <= data.size()
        && (static_cast<quint8>(data[syPos]) & ~0x40) == 0x30) {
        const double sy = readLeFloat(data, syPos + 1);
        if (std::isfinite(sy) && std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0)
            size += 5;
    }
    const int next = pos + size;
    const bool groupFollows = isFM2023GroupAt(data, next, true)
        || isFM2023GroupAt(data, next, false)
        || (next + 1 < data.size()
            && (isFM2023GroupAt(data, next + 1, true)
                || isFM2023GroupAt(data, next + 1, false)));
    return groupFollows ? size : 0;
}

QByteArray normalizeFM2023Records(QByteArray data)
{
    int pos = 0;
    while (pos + 31 <= data.size()) {
        if (pos + 32 <= data.size()
            && (static_cast<quint8>(data[pos]) == 0x00
                || static_cast<quint8>(data[pos]) == 0x01)
            && (static_cast<quint8>(data[pos + 1]) == 0x01
                || static_cast<quint8>(data[pos + 1]) == 0x02)
            && isFM2023ShapePayloadAt(data, pos + 2, pos + 4, pos + 32)) {
            const quint16 encodedShapeId = readLeU16(data, pos + 2);
            data[pos + 1] = static_cast<char>(0x02);
            if (encodedShapeId == 0x0bb8) {
                data[pos + 2] = static_cast<char>(0xb9);
                data[pos + 3] = static_cast<char>(0x0b);
            }
            pos += 32;
            continue;
        }
        if (isFM2023BareShapeAt(data, pos)) {
            const quint16 encodedShapeId = readLeU16(data, pos + 1);
            data[pos] = static_cast<char>(0x02);
            if (encodedShapeId == 0x0bb8) {
                data[pos + 1] = static_cast<char>(0xb9);
                data[pos + 2] = static_cast<char>(0x0b);
            }
            pos += 31;
        } else if (const int transformSize = fm2023TransformSizeAt(data, pos)) {
            data[pos] = static_cast<char>(0x03);
            pos += transformSize;
        } else {
            ++pos;
        }
    }
    return data;
}

VinylDecoderOptions fm2023DecoderOptions();

QVector<LiverySection> buildFM2023LiverySections(const QByteArray &body,
                                                 const QVector<int> &sectionCounts)
{
    return VinylTreeDecoder{fm2023DecoderOptions()}.buildLiverySections(
        body, sectionCounts, kFM2023LiverySlots, kFM2023SectionCount);
}

void applyFM2023ShapeMasks(VinylGroup &group, const QByteArray &layerData, bool root)
{
    VinylShape *previousShape = nullptr;
    for (VinylItem &item : group.items) {
        if (item.isShape()) {
            VinylShape &shape = std::get<VinylShape>(item.value);
            if (previousShape != nullptr && shape.marker == QByteArray("\x01\x02", 2))
                previousShape->isMask = true;
            previousShape = &shape;
        } else {
            applyFM2023ShapeMasks(*std::get<VinylGroupPtr>(item.value), layerData, false);
            previousShape = nullptr;
        }
    }
    if (!root || previousShape == nullptr)
        return;
    const int recordSize = previousShape->marker.size() == 2 ? 32 : 31;
    const int trailerPos = previousShape->absPos + recordSize;
    const int trailerSize = layerData.size() - trailerPos;
    if (trailerSize >= 1 && trailerSize <= 2
        && static_cast<quint8>(layerData[trailerPos]) == 0x01) {
        previousShape->isMask = true;
    }
}

void finalizeFM2023Group(VinylGroup &root, const QByteArray &payload,
                         const LayerData &layerData)
{
    if (payload.size() >= 0x22
        && (static_cast<quint8>(payload[0x1d]) & ~0x40) == 0x30) {
        const double sy = readLeFloat(payload, 0x1e);
        if (std::isfinite(sy) && std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0)
            root.sy = sy;
    }
    applyFM2023ShapeMasks(root, layerData.data, true);
}

VinylDecoderOptions fm2023DecoderOptions()
{
    VinylDecoderOptions options;
    options.markerlessRootHeader = true;
    options.appendLiveryTailPadding = true;
    options.normalizeRecords = normalizeFM2023Records;
    options.finalizeGroup = finalizeFM2023Group;
    return options;
}

VinylGroup decodeFM2023RawGroupImpl(const QByteArray &payload, LayerData *decodedLayerData)
{
    return VinylTreeDecoder{fm2023DecoderOptions()}.decodeGroup(payload, decodedLayerData);
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

    const QByteArray raw = inflateFM2023Blocks(fileData);

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

    payload.sectionCounts.reserve(7);

    if (bodyEnd >= 0 && raw.mid(bodyEnd, 4) == QByteArray("yrvl", 4)) {
        for (int i = 0; i < 7; ++i) {
            const int off = bodyEnd + 4 + i * 4;
            if (off + 4 <= raw.size())
                payload.sectionCounts.push_back(static_cast<int>(readLeU32(raw, off)));
            else
                payload.sectionCounts.push_back(0);
        }
    } else {
        for (int i = 0; i < 7; ++i)
            payload.sectionCounts.push_back(0);
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
        buildFM2023LiverySections(payload.gyvlBody, payload.sectionCounts);

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
            const auto appendItems = [&](const QVector<VinylItem> &items, int begin,
                                         const Matrix3 &matrix, bool mask) {
                for (int i = begin; i < items.size(); ++i) {
                    const VinylItem &item = items[i];
                    if (item.isShape())
                        sectionGroup->append(importShape(std::get<VinylShape>(item.value), matrix, mask));
                    else
                        sectionGroup->append(importGroup(*std::get<VinylGroupPtr>(item.value), matrix, mask));
                }
            };
            const VinylGroupPtr leadingRoot = !sectionRoot->items.isEmpty()
                    && !sectionRoot->items.front().isShape()
                ? std::get<VinylGroupPtr>(sectionRoot->items.front().value)
                : VinylGroupPtr{};
            if (leadingRoot && leadingRoot->source == QStringLiteral("livery_section_root")) {
                const Matrix3 leadingMatrix = detail::multiply(sectionMatrix, groupNodeMatrix(*leadingRoot));
                const bool leadingMask = sectionMask || leadingRoot->isMask;
                const Matrix3 leadingCanvasMatrix = detail::multiply(
                    liverySectionCanvasTransform(section.slot), leadingMatrix);
                appendItems(leadingRoot->items, 0, leadingCanvasMatrix, leadingMask);
                const Matrix3 remainingMatrix = detail::multiply(
                    liverySectionCanvasTransform(section.slot), sectionMatrix);
                appendItems(sectionRoot->items, 1, remainingMatrix, sectionMask);
            } else {
                while (sectionRoot->items.size() == 1 && !sectionRoot->items.front().isShape()) {
                    const auto topGroup = std::get<VinylGroupPtr>(sectionRoot->items.front().value);
                    if (!topGroup) break;
                    sectionMatrix = detail::multiply(sectionMatrix, groupNodeMatrix(*topGroup));
                    sectionMask = sectionMask || topGroup->isMask;
                    sectionRoot = topGroup.get();
                }
                sectionMatrix = detail::multiply(liverySectionCanvasTransform(section.slot), sectionMatrix);
                appendItems(sectionRoot->items, 0, sectionMatrix, sectionMask);
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
    LayerData layerData;
    VinylGroup root = decodeFM2023RawGroupImpl(payload, &layerData);

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
            if (hasColorData(shape.color) && !shape.isMask)
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

QVector<fh6::LiverySection> fh6::decodeFM2023LiverySections(const FM2023LiveryPayload &payload)
{
    return buildFM2023LiverySections(payload.gyvlBody, payload.sectionCounts);
}

fh6::VinylGroup fh6::decodeFM2023RawGroup(const QByteArray &payload)
{
    return decodeFM2023RawGroupImpl(payload, nullptr);
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
