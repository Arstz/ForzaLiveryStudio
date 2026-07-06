#include "project_codec.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "flat_payload.h"
#include "header_codec.h"
#include "livery_codec.h"
#include "matrix_math.h"
#include "nested_payload.h"
#include "shape_registry.h"
#include "vinyl_decoder.h"
#include "vinyl_flattener.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>
#include <functional>
#include <stdexcept>
#include <variant>

#include <zlib.h>

namespace fh6 {
namespace {

quint8 byteFromJson(const QJsonArray &array, int index)
{
    const int value = array.at(index).toInt();
    if (value < 0 || value > 255) {
        throw std::runtime_error("color component is outside byte range");
    }
    return static_cast<quint8>(value);
}

QString toBase64(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toBase64());
}

std::array<quint8, 4> colorSwatchFromJson(const QJsonValue &value)
{
    const QString text = value.toString();
    if (!text.startsWith(QLatin1Char('#')) || (text.size() != 9 && text.size() != 7)) {
        return {255, 255, 255, 255};
    }
    bool ok = false;
    const uint valueRgb = text.mid(1).toUInt(&ok, 16);
    if (!ok) {
        return {255, 255, 255, 255};
    }
    const quint8 alpha = text.size() == 9 ? static_cast<quint8>((valueRgb >> 24) & 0xff) : 255;
    const quint8 red = static_cast<quint8>((valueRgb >> 16) & 0xff);
    const quint8 green = static_cast<quint8>((valueRgb >> 8) & 0xff);
    const quint8 blue = static_cast<quint8>(valueRgb & 0xff);
    return {blue, green, red, alpha};
}

QString colorSwatchToJson(const std::array<quint8, 4> &color)
{
    return QStringLiteral("#%1%2%3%4")
        .arg(color[3], 2, 16, QLatin1Char('0'))
        .arg(color[2], 2, 16, QLatin1Char('0'))
        .arg(color[1], 2, 16, QLatin1Char('0'))
        .arg(color[0], 2, 16, QLatin1Char('0'));
}

QString toHexString(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex());
}

bool looksGzipped(const QByteArray &bytes)
{
    // Gzip streams start with the magic bytes 0x1f 0x8b; bare JSON never does.
    return bytes.size() >= 2 && static_cast<quint8>(bytes[0]) == 0x1f
        && static_cast<quint8>(bytes[1]) == 0x8b;
}

QByteArray gzipCompress(const QByteArray &data)
{
    z_stream stream{};
    // windowBits 15 + 16 selects the gzip wrapper (rather than raw zlib).
    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("failed to initialize gzip compressor");
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = static_cast<uInt>(data.size());

    QByteArray out;
    QByteArray buffer(32768, Qt::Uninitialized);
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = deflate(&stream, Z_FINISH);
        if (status == Z_STREAM_ERROR) {
            deflateEnd(&stream);
            throw std::runtime_error("gzip compression failed");
        }
        out.append(buffer.constData(), buffer.size() - static_cast<int>(stream.avail_out));
    } while (stream.avail_out == 0);
    deflateEnd(&stream);
    return out;
}

QByteArray gzipDecompress(const QByteArray &data)
{
    z_stream stream{};
    // windowBits 15 + 32 auto-detects a gzip or zlib header.
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        throw std::runtime_error("failed to initialize gzip decompressor");
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = static_cast<uInt>(data.size());

    QByteArray out;
    QByteArray buffer(32768, Qt::Uninitialized);
    int status = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            throw std::runtime_error("gzip decompression failed for project container");
        }
        out.append(buffer.constData(), buffer.size() - static_cast<int>(stream.avail_out));
    } while (status != Z_STREAM_END);
    inflateEnd(&stream);
    return out;
}

QJsonObject headerMetadataToJson(const HeaderMetadata &meta)
{
    QJsonObject object;
    object.insert(QStringLiteral("format_version"), static_cast<qint64>(meta.formatVersion));
    object.insert(QStringLiteral("name"), meta.name);
    object.insert(QStringLiteral("published"), meta.published);
    object.insert(QStringLiteral("description"), meta.description);
    object.insert(QStringLiteral("year"), meta.year);
    object.insert(QStringLiteral("month"), meta.month);
    object.insert(QStringLiteral("day"), meta.day);
    object.insert(QStringLiteral("field_block"), toBase64(meta.fieldBlock));
    object.insert(QStringLiteral("creator_tag"), toBase64(meta.creatorTag));
    object.insert(QStringLiteral("creator_name"), meta.creatorName);
    object.insert(QStringLiteral("type_value"), static_cast<qint64>(meta.typeValue));
    object.insert(QStringLiteral("guid"), toBase64(meta.guid));
    object.insert(QStringLiteral("trailing"), toBase64(meta.trailing));
    object.insert(QStringLiteral("published_tail"), toBase64(meta.publishedTail));
    return object;
}

HeaderMetadata headerMetadataFromJson(const QJsonObject &object)
{
    HeaderMetadata meta;
    meta.formatVersion = static_cast<quint32>(object.value(QStringLiteral("format_version")).toInt(7));
    meta.name = object.value(QStringLiteral("name")).toString();
    meta.published = object.value(QStringLiteral("published")).toBool(false);
    meta.description = object.value(QStringLiteral("description")).toString();
    meta.year = static_cast<quint16>(object.value(QStringLiteral("year")).toInt(0));
    meta.month = static_cast<quint8>(object.value(QStringLiteral("month")).toInt(0));
    meta.day = static_cast<quint8>(object.value(QStringLiteral("day")).toInt(0));
    meta.fieldBlock = QByteArray::fromBase64(object.value(QStringLiteral("field_block")).toString().toLatin1());
    meta.creatorTag = QByteArray::fromBase64(object.value(QStringLiteral("creator_tag")).toString().toLatin1());
    meta.creatorName = object.value(QStringLiteral("creator_name")).toString();
    meta.typeValue = static_cast<quint32>(object.value(QStringLiteral("type_value")).toInt(0));
    meta.guid = QByteArray::fromBase64(object.value(QStringLiteral("guid")).toString().toLatin1());
    meta.trailing = QByteArray::fromBase64(object.value(QStringLiteral("trailing")).toString().toLatin1());
    meta.publishedTail = QByteArray::fromBase64(object.value(QStringLiteral("published_tail")).toString().toLatin1());
    meta.parsedOk = true;
    return meta;
}

QByteArray readOptionalFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

Project newImportProject(const QString &folderOrFile, const QByteArray &payload)
{
    Project project;
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        project.name = info.fileName();
        project.sourceFolder = info.absoluteFilePath();
    } else {
        project.name = info.absoluteDir().dirName();
        project.sourceFolder = info.absoluteDir().absolutePath();
    }
    project.sourceDecPrefix = payload.left(0x1d);
    project.sourceHeader = readOptionalFile(QDir(project.sourceFolder).filePath(QStringLiteral("header")));
    return project;
}

QString layerIdForIndex(int index)
{
    return QStringLiteral("layer-%1").arg(index, 4, 10, QLatin1Char('0'));
}

QString groupIdForIndex(int index)
{
    return QStringLiteral("group-%1").arg(index, 4, 10, QLatin1Char('0'));
}

Matrix3 nodeMatrix(const VinylGroup &node)
{
    constexpr double Pi = 3.14159265358979323846;
    const double radians = node.rot * Pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return affine(c * node.sx, -s * node.sy, node.px,
                  s * node.sx, c * node.sy, node.py);
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

int importedGroupTerminalDepth(const Project &project, const QHash<QString, int> &groupIndexes, const QString &groupId)
{
    const auto it = groupIndexes.find(groupId);
    if (it == groupIndexes.end()) {
        return 0;
    }
    const LayerGroup &group = project.groups[*it];
    if (group.childIds.isEmpty()) {
        return 1;
    }
    const QString lastChildId = group.childIds.back();
    if (!groupIndexes.contains(lastChildId)) {
        return 1;
    }
    return 1 + importedGroupTerminalDepth(project, groupIndexes, lastChildId);
}

QByteArray effectiveDebugMarker(const VinylGroup &node)
{
    if (!node.pendingTransformMarker.isEmpty()
        && !node.inlineTransformMarker.isEmpty()
        && node.effectiveTransformMarker == node.pendingTransformMarker + node.inlineTransformMarker) {
        return {};
    }
    return node.effectiveTransformMarker;
}

QByteArray renameHeader(const QByteArray &headerBytes, const QString &newName)
{
    if (headerBytes.size() < 8) {
        return headerBytes;
    }

    const quint32 oldLength = detail::readLeU32(headerBytes, 4);
    const qsizetype oldEnd = 8 + static_cast<qsizetype>(oldLength) * 2;
    if (oldEnd > headerBytes.size()) {
        return headerBytes;
    }

    QByteArray out = headerBytes.left(4);
    detail::appendLeU32(out, static_cast<quint32>(newName.size()));
    const QByteArray encoded(reinterpret_cast<const char *>(newName.utf16()),
                             newName.size() * static_cast<int>(sizeof(char16_t)));
    out.append(encoded);
    out.append(headerBytes.mid(oldEnd));
    if (out.size() < headerBytes.size()) {
        out.append(QByteArray(headerBytes.size() - out.size(), '\0'));
    }
    return out;
}

void copyFileReplacing(const QString &source, const QString &destination)
{
    QFile::remove(destination);
    QDir().mkpath(QFileInfo(destination).absolutePath());
    if (!QFile::copy(source, destination)) {
        throw std::runtime_error(("could not copy export sidecar: " + source).toStdString());
    }
}

void copyDirectoryContentsExceptCGroup(const QString &sourceFolder, const QString &outputFolder)
{
    const QFileInfo sourceInfo(sourceFolder);
    if (!sourceInfo.isDir()) {
        return;
    }

    const QDir sourceDir(sourceInfo.absoluteFilePath());
    const QDir outputDir(outputFolder);
    const QString sourceRoot = sourceDir.canonicalPath();
    const QString outputRoot = outputDir.canonicalPath();
    if (!sourceRoot.isEmpty() && !outputRoot.isEmpty()
        && (outputRoot == sourceRoot || outputRoot.startsWith(sourceRoot + QLatin1Char('/')))) {
        throw std::runtime_error("flat export output folder must not be inside the source folder");
    }

    const QFileInfoList entries = sourceDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QFileInfo &entry : entries) {
        if (entry.fileName() == QStringLiteral("C_group")) {
            continue;
        }
        if (entry.isDir()) {
            QDirIterator iterator(entry.absoluteFilePath(),
                                  QDir::NoDotAndDotDot | QDir::AllEntries,
                                  QDirIterator::Subdirectories);
            QDir().mkpath(outputDir.filePath(entry.fileName()));
            while (iterator.hasNext()) {
                iterator.next();
                const QFileInfo child(iterator.filePath());
                const QString destination = outputDir.filePath(sourceDir.relativeFilePath(child.absoluteFilePath()));
                if (child.isDir()) {
                    QDir().mkpath(destination);
                } else {
                    copyFileReplacing(child.absoluteFilePath(), destination);
                }
            }
        } else {
            copyFileReplacing(entry.absoluteFilePath(), outputDir.filePath(entry.fileName()));
        }
    }
}

} // namespace

ShapeLayer shapeLayerFromJson(const QJsonObject &object)
{
    ShapeLayer layer;
    layer.id = object.value(QStringLiteral("id")).toString();
    layer.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Shape"));
    layer.shapeId = static_cast<quint16>(object.value(QStringLiteral("shape_id")).toInt());
    layer.x = object.value(QStringLiteral("x")).toDouble(0.0);
    layer.y = object.value(QStringLiteral("y")).toDouble(0.0);
    layer.scaleX = object.value(QStringLiteral("scale_x")).toDouble(1.0);
    layer.scaleY = object.value(QStringLiteral("scale_y")).toDouble(1.0);
    layer.rotation = normalizeRotation(object.value(QStringLiteral("rotation")).toDouble(0.0));
    layer.skew = object.value(QStringLiteral("skew")).toDouble(0.0);
    layer.visible = object.value(QStringLiteral("visible")).toBool(true);
    layer.locked = object.value(QStringLiteral("locked")).toBool(false);
    layer.mask = object.value(QStringLiteral("mask")).toBool(false);
    const QJsonObject debug = object.value(QStringLiteral("debug")).toObject();
    layer.sourceShape = debug.value(QStringLiteral("source_shape")).toInt(0);
    layer.absOffset = debug.value(QStringLiteral("abs_offset")).toInt(0);
    layer.marker = QByteArray::fromHex(debug.value(QStringLiteral("marker")).toString().toLatin1());
    layer.flags = debug.value(QStringLiteral("flags")).toInt(0);

    const QJsonArray color = object.value(QStringLiteral("color")).toArray();
    if (color.size() == 4) {
        layer.color = {
            byteFromJson(color, 0),
            byteFromJson(color, 1),
            byteFromJson(color, 2),
            byteFromJson(color, 3),
        };
    }
    return layer;
}

GuideLayer guideLayerFromJson(const QJsonObject &object)
{
    GuideLayer guide;
    guide.id = object.value(QStringLiteral("id")).toString();
    guide.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Guide"));
    guide.sourcePath = object.value(QStringLiteral("source_path")).toString();
    guide.imageBytes = QByteArray::fromBase64(object.value(QStringLiteral("image_bytes")).toString().toLatin1());
    guide.pixelBytes = QByteArray::fromBase64(object.value(QStringLiteral("pixel_bytes")).toString().toLatin1());
    guide.imageFormat = object.value(QStringLiteral("image_format")).toString();
    guide.width = object.value(QStringLiteral("width")).toInt(0);
    guide.height = object.value(QStringLiteral("height")).toInt(0);
    guide.x = object.value(QStringLiteral("x")).toDouble(0.0);
    guide.y = object.value(QStringLiteral("y")).toDouble(0.0);
    guide.scaleX = object.value(QStringLiteral("scale_x")).toDouble(1.0);
    guide.scaleY = object.value(QStringLiteral("scale_y")).toDouble(1.0);
    guide.rotation = normalizeRotation(object.value(QStringLiteral("rotation")).toDouble(0.0));
    guide.opacity = object.value(QStringLiteral("opacity")).toDouble(0.5);
    guide.visible = object.value(QStringLiteral("visible")).toBool(true);
    guide.locked = object.value(QStringLiteral("locked")).toBool(false);
    return guide;
}

LayerGroup layerGroupFromJson(const QJsonObject &object)
{
    LayerGroup group;
    group.id = object.value(QStringLiteral("id")).toString();
    group.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Group"));
    group.locked = object.value(QStringLiteral("locked")).toBool(false);

    const QJsonArray childIds = object.value(QStringLiteral("child_ids")).toArray();
    group.childIds.reserve(childIds.size());
    for (const QJsonValue &value : childIds) {
        group.childIds.push_back(value.toString());
    }

    const QJsonObject debug = object.value(QStringLiteral("debug")).toObject();
    group.sourceAbsPos = debug.value(QStringLiteral("source_abs_pos")).toInt(0);
    group.pendingTransformMarker = QByteArray::fromHex(debug.value(QStringLiteral("pending_transform_marker")).toString().toLatin1());
    group.inlineTransformMarker = QByteArray::fromHex(debug.value(QStringLiteral("inline_transform_marker")).toString().toLatin1());
    group.effectiveTransformMarker = QByteArray::fromHex(debug.value(QStringLiteral("effective_transform_marker")).toString().toLatin1());
    group.headerControlBytes = QByteArray::fromHex(debug.value(QStringLiteral("header_control_bytes")).toString().toLatin1());
    group.flags = debug.value(QStringLiteral("flags")).toInt(0);
    group.sourceParentId = debug.value(QStringLiteral("source_parent_id")).toString();
    group.sourcePreviousSiblingId = debug.value(QStringLiteral("source_previous_sibling_id")).toString();
    group.sourcePreviousGroupDepth = debug.value(QStringLiteral("source_previous_group_depth")).toInt(0);
    group.isLiverySection = debug.value(QStringLiteral("is_livery_section")).toBool(false);
    group.liverySectionSlot = debug.value(QStringLiteral("livery_section_slot")).toInt(-1);
    const QJsonArray sourceChildIds = debug.value(QStringLiteral("source_child_ids")).toArray();
    group.sourceChildIds.reserve(sourceChildIds.size());
    for (const QJsonValue &value : sourceChildIds) {
        group.sourceChildIds.push_back(value.toString());
    }
    return group;
}

Project importCGroupFlat(const QString &folderOrFile)
{
    const QByteArray payload = readCGroupPayload(folderOrFile);
    const LayerData layerData = getLayerData(payload);
    const VinylGroup root = buildTree(layerData.data, payload);
    const QVector<FlattenedLayer> flat = flattenGroup(root);

    Project project = newImportProject(folderOrFile, payload);
    project.layers.reserve(flat.size());
    int index = 1;
    for (const FlattenedLayer &flatLayer : flat) {
        const Matrix3 effective = detail::multiply(flatLayer.groupMatrix, shapeMatrix(flatLayer));
        ShapeLayer layer = decomposeLayerMatrix(effective);
        layer.shapeId = flatLayer.shapeId;
        layer.name = QStringLiteral("%1 %2")
            .arg(index, 4, 10, QLatin1Char('0'))
            .arg(detail::shapeName(flatLayer.shapeId));
        layer.color = flatLayer.color;
        layer.mask = flatLayer.isMask;
        layer.visible = true;
        layer.locked = false;
        layer.sourceShape = index;
        layer.absOffset = flatLayer.absPos + layerData.start;
        layer.marker = flatLayer.marker;
        layer.flags = flatLayer.flags;
        project.layers.push_back(layer);
        ++index;
    }
    return project;
}

Project importCGroupNested(const QString &folderOrFile)
{
    const QByteArray payload = readCGroupPayload(folderOrFile);
    const LayerData layerData = getLayerData(payload);
    const VinylGroup root = buildTree(layerData.data, payload);
    Project project = newImportProject(folderOrFile, payload);

    int shapeIndex = 0;
    int groupIndex = 0;
    QHash<QString, int> groupIndexes;

    std::function<QString(const VinylShape &, const Matrix3 &, bool)> importShape =
        [&](const VinylShape &shape, const Matrix3 &parentMatrix, bool parentMask) -> QString {
            ++shapeIndex;
            const Matrix3 effective = detail::multiply(parentMatrix, shapeMatrixForShape(shape));
            ShapeLayer layer = decomposeLayerMatrix(effective);
            layer.id = layerIdForIndex(shapeIndex);
            layer.name = QStringLiteral("%1 %2")
                .arg(shapeIndex, 4, 10, QLatin1Char('0'))
                .arg(detail::shapeName(shape.shapeId));
            layer.shapeId = shape.shapeId;
            layer.color = shape.color;
            layer.mask = parentMask || shape.isMask;
            if (hasColorData(shape.color)) {
                layer.mask = false;
            }
            layer.visible = true;
            layer.locked = false;
            layer.sourceShape = shapeIndex;
            layer.absOffset = shape.absPos + layerData.start;
            layer.marker = shape.marker;
            layer.flags = shape.flags;
            project.layers.push_back(layer);
            return layer.id;
        };

    std::function<QString(const VinylGroup &, const Matrix3 &, bool, const QString &, const QString &)> importGroup =
        [&](const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask,
            const QString &sourceParentId, const QString &sourcePreviousSiblingId) -> QString {
            ++groupIndex;
            const QString groupId = groupIdForIndex(groupIndex);
            const Matrix3 groupMatrix = detail::multiply(parentMatrix, nodeMatrix(node));
            const bool inheritedMask = parentMask || node.isMask;

            LayerGroup group;
            group.id = groupId;
            group.name = QStringLiteral("Group %1").arg(groupIndex);
            group.sourceAbsPos = node.absPos + layerData.start;
            group.pendingTransformMarker = node.pendingTransformMarker;
            group.inlineTransformMarker = node.inlineTransformMarker;
            group.effectiveTransformMarker = effectiveDebugMarker(node);
            group.headerControlBytes = node.headerControlBytes;
            group.flags = node.flags;
            group.sourceParentId = sourceParentId;
            group.sourcePreviousSiblingId = sourcePreviousSiblingId;
            group.sourcePreviousGroupDepth = importedGroupTerminalDepth(project, groupIndexes, sourcePreviousSiblingId);

            project.groups.push_back(group);
            const int projectGroupIndex = project.groups.size() - 1;
            groupIndexes.insert(groupId, projectGroupIndex);

            for (const VinylItem &item : node.items) {
                const QString previousSiblingId = project.groups[projectGroupIndex].childIds.isEmpty()
                    ? QString()
                    : project.groups[projectGroupIndex].childIds.back();
                QString childId;
                if (item.isShape()) {
                    childId = importShape(std::get<VinylShape>(item.value), groupMatrix, inheritedMask);
                } else {
                    childId = importGroup(*std::get<VinylGroupPtr>(item.value),
                                          groupMatrix,
                                          inheritedMask,
                                          groupId,
                                          previousSiblingId);
                }
                project.groups[projectGroupIndex].childIds.push_back(childId);
            }
            project.groups[projectGroupIndex].sourceChildIds = project.groups[projectGroupIndex].childIds;
            return groupId;
        };

    const Matrix3 rootMatrix = nodeMatrix(root);
    const bool rootMask = root.isMask;
    for (const VinylItem &item : root.items) {
        const QString previousSiblingId = project.rootChildIds.isEmpty() ? QString() : project.rootChildIds.back();
        QString childId;
        if (item.isShape()) {
            childId = importShape(std::get<VinylShape>(item.value), rootMatrix, rootMask);
        } else {
            childId = importGroup(*std::get<VinylGroupPtr>(item.value),
                                  rootMatrix,
                                  rootMask,
                                  QStringLiteral("__root__"),
                                  previousSiblingId);
        }
        project.rootChildIds.push_back(childId);
    }
    return project;
}

Project importCLivery(const QString &folderOrFile)
{
    const LiveryPayload livery = readLiveryPayload(folderOrFile);
    const QVector<LiverySection> sections =
        buildLiverySections(livery.body, livery.sectionCounts);

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

    int shapeIndex = 0;
    int groupIndex = 0;

    std::function<QString(const VinylShape &, const Matrix3 &, bool)> importShape =
        [&](const VinylShape &shape, const Matrix3 &parentMatrix, bool parentMask) -> QString {
            ++shapeIndex;
            const Matrix3 effective = detail::multiply(parentMatrix, shapeMatrixForShape(shape));
            ShapeLayer layer = decomposeLayerMatrix(effective);
            layer.id = layerIdForIndex(shapeIndex);
            layer.name = QStringLiteral("%1 %2")
                .arg(shapeIndex, 4, 10, QLatin1Char('0'))
                .arg(detail::shapeName(shape.shapeId));
            layer.shapeId = shape.shapeId;
            layer.color = shape.color;
            layer.mask = parentMask || shape.isMask;
            if (hasColorData(shape.color)) {
                layer.mask = false;
            }
            layer.visible = true;
            layer.locked = false;
            layer.sourceShape = shapeIndex;
            layer.absOffset = shape.absPos;
            layer.marker = shape.marker;
            layer.flags = shape.flags;
            project.layers.push_back(layer);
            return layer.id;
        };

    std::function<QString(const VinylGroup &, const Matrix3 &, bool)> importGroup =
        [&](const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask) -> QString {
            ++groupIndex;
            const QString groupId = groupIdForIndex(groupIndex);
            const Matrix3 groupMatrix = detail::multiply(parentMatrix, nodeMatrix(node));
            const bool inheritedMask = parentMask || node.isMask;

            LayerGroup group;
            group.id = groupId;
            group.name = QStringLiteral("Group %1").arg(groupIndex);
            group.sourceAbsPos = node.absPos;
            group.flags = node.flags;
            project.groups.push_back(group);
            const int idx = project.groups.size() - 1;

            for (const VinylItem &item : node.items) {
                QString childId;
                if (item.isShape()) {
                    childId = importShape(std::get<VinylShape>(item.value), groupMatrix, inheritedMask);
                } else {
                    childId = importGroup(*std::get<VinylGroupPtr>(item.value), groupMatrix, inheritedMask);
                }
                project.groups[idx].childIds.push_back(childId);
            }
            project.groups[idx].sourceChildIds = project.groups[idx].childIds;
            return groupId;
        };

    for (const LiverySection &section : sections) {
        // Every slot becomes a top-level section folder, even when empty.
        ++groupIndex;
        const QString sectionId = groupIdForIndex(groupIndex);
        LayerGroup sectionGroup;
        sectionGroup.id = sectionId;
        sectionGroup.name = section.name;
        sectionGroup.isLiverySection = true;
        sectionGroup.liverySectionSlot = section.slot;
        sectionGroup.sourceAbsPos = section.absPos;
        project.groups.push_back(sectionGroup);
        const int sectionIdx = project.groups.size() - 1;

        if (section.populated) {
            const Matrix3 sectionMatrix = nodeMatrix(section.subtree);
            const bool sectionMask = section.subtree.isMask;
            for (const VinylItem &item : section.subtree.items) {
                QString childId;
                if (item.isShape()) {
                    childId = importShape(std::get<VinylShape>(item.value), sectionMatrix, sectionMask);
                } else {
                    childId = importGroup(*std::get<VinylGroupPtr>(item.value), sectionMatrix, sectionMask);
                }
                project.groups[sectionIdx].childIds.push_back(childId);
            }
            project.groups[sectionIdx].sourceChildIds = project.groups[sectionIdx].childIds;
        }
        project.rootChildIds.push_back(sectionId);
    }

    return project;
}

Project projectFromJson(const QJsonObject &object)
{
    if (object.contains(QStringLiteral("format"))) {
        if (object.value(QStringLiteral("format")).toString() != QLatin1String(ProjectJsonFormat)) {
            throw std::runtime_error("not an editor project");
        }
        if (object.value(QStringLiteral("version")).toInt(0) > ProjectJsonVersion) {
            throw std::runtime_error("unsupported project version");
        }
    }

    Project project;
    project.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Untitled"));
    project.sourceFolder = object.value(QStringLiteral("source_folder")).toString();
    project.isLivery = object.value(QStringLiteral("is_livery")).toBool(false);
    const QString prefix = object.value(QStringLiteral("source_dec_prefix")).toString();
    if (!prefix.isEmpty()) {
        project.sourceDecPrefix = QByteArray::fromBase64(prefix.toLatin1());
    }
    const QString header = object.value(QStringLiteral("source_header")).toString();
    if (!header.isEmpty()) {
        project.sourceHeader = QByteArray::fromBase64(header.toLatin1());
    }
    if (object.value(QStringLiteral("header_metadata")).isObject()) {
        // New projects keep structured metadata here; export builds the header from
        // it when sourceHeader is empty (see exportFlatProjectFolder).
        project.headerMetadata = headerMetadataFromJson(object.value(QStringLiteral("header_metadata")).toObject());
    }

    const QJsonArray layers = object.value(QStringLiteral("layers")).toArray();
    project.layers.reserve(layers.size());
    for (const QJsonValue &value : layers) {
        if (!value.isObject()) {
            throw std::runtime_error("project layer entry is not an object");
        }
        project.layers.push_back(shapeLayerFromJson(value.toObject()));
    }
    const QJsonArray guideLayers = object.value(QStringLiteral("guide_layers")).toArray();
    project.guideLayers.reserve(guideLayers.size());
    for (const QJsonValue &value : guideLayers) {
        if (!value.isObject()) {
            throw std::runtime_error("project guide layer entry is not an object");
        }
        project.guideLayers.push_back(guideLayerFromJson(value.toObject()));
    }
    const QJsonArray groups = object.value(QStringLiteral("groups")).toArray();
    project.groups.reserve(groups.size());
    for (const QJsonValue &value : groups) {
        if (!value.isObject()) {
            throw std::runtime_error("project group entry is not an object");
        }
        project.groups.push_back(layerGroupFromJson(value.toObject()));
        project.isLivery = project.isLivery || project.groups.back().isLiverySection;
    }

    const QJsonArray rootChildIds = object.value(QStringLiteral("root_child_ids")).toArray();
    project.rootChildIds.reserve(rootChildIds.size());
    for (const QJsonValue &value : rootChildIds) {
        project.rootChildIds.push_back(value.toString());
    }
    const QJsonArray colorSwatches = object.value(QStringLiteral("color_swatches")).toArray();
    project.colorSwatches.reserve(colorSwatches.size());
    for (const QJsonValue &value : colorSwatches) {
        project.colorSwatches.push_back(colorSwatchFromJson(value));
    }
    return project;
}

QJsonObject shapeLayerToJson(const ShapeLayer &layer)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), layer.id);
    object.insert(QStringLiteral("name"), layer.name);
    object.insert(QStringLiteral("shape_id"), static_cast<int>(layer.shapeId));
    object.insert(QStringLiteral("x"), layer.x);
    object.insert(QStringLiteral("y"), layer.y);
    object.insert(QStringLiteral("scale_x"), layer.scaleX);
    object.insert(QStringLiteral("scale_y"), layer.scaleY);
    object.insert(QStringLiteral("rotation"), normalizeRotation(layer.rotation));
    object.insert(QStringLiteral("skew"), layer.skew);
    object.insert(QStringLiteral("visible"), layer.visible);
    object.insert(QStringLiteral("locked"), layer.locked);
    object.insert(QStringLiteral("mask"), layer.mask);

    QJsonArray color;
    for (int i = 0; i < 4; ++i) {
        color.append(static_cast<int>(layer.color[i]));
    }
    object.insert(QStringLiteral("color"), color);

    QJsonObject debug;
    debug.insert(QStringLiteral("source_shape"), layer.sourceShape);
    debug.insert(QStringLiteral("abs_offset"), layer.absOffset);
    debug.insert(QStringLiteral("marker"), toHexString(layer.marker));
    debug.insert(QStringLiteral("flags"), layer.flags);
    object.insert(QStringLiteral("debug"), debug);
    return object;
}

QJsonObject guideLayerToJson(const GuideLayer &guide)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), guide.id);
    object.insert(QStringLiteral("name"), guide.name);
    object.insert(QStringLiteral("source_path"), guide.sourcePath);
    // image_bytes holds a compressed (WEBP/PNG) copy; the raw decoded pixel buffer is no
    // longer serialized (it bloated the JSON and is reconstructable from image_bytes).
    // pixel_bytes is still read back in guideLayerFromJson for legacy projects.
    object.insert(QStringLiteral("image_bytes"), toBase64(guide.imageBytes));
    object.insert(QStringLiteral("image_format"), guide.imageFormat);
    object.insert(QStringLiteral("width"), guide.width);
    object.insert(QStringLiteral("height"), guide.height);
    object.insert(QStringLiteral("x"), guide.x);
    object.insert(QStringLiteral("y"), guide.y);
    object.insert(QStringLiteral("scale_x"), guide.scaleX);
    object.insert(QStringLiteral("scale_y"), guide.scaleY);
    object.insert(QStringLiteral("rotation"), normalizeRotation(guide.rotation));
    object.insert(QStringLiteral("opacity"), guide.opacity);
    object.insert(QStringLiteral("visible"), guide.visible);
    object.insert(QStringLiteral("locked"), guide.locked);
    return object;
}

QJsonObject layerGroupToJson(const LayerGroup &group)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), group.id);
    object.insert(QStringLiteral("name"), group.name);
    object.insert(QStringLiteral("locked"), group.locked);

    QJsonArray childIds;
    for (const QString &childId : group.childIds) {
        childIds.append(childId);
    }
    object.insert(QStringLiteral("child_ids"), childIds);

    QJsonObject debug;
    debug.insert(QStringLiteral("source_abs_pos"), group.sourceAbsPos);
    debug.insert(QStringLiteral("pending_transform_marker"), toHexString(group.pendingTransformMarker));
    debug.insert(QStringLiteral("inline_transform_marker"), toHexString(group.inlineTransformMarker));
    debug.insert(QStringLiteral("effective_transform_marker"), toHexString(group.effectiveTransformMarker));
    debug.insert(QStringLiteral("header_control_bytes"), toHexString(group.headerControlBytes));
    debug.insert(QStringLiteral("flags"), group.flags);
    debug.insert(QStringLiteral("source_parent_id"), group.sourceParentId);
    debug.insert(QStringLiteral("source_previous_sibling_id"), group.sourcePreviousSiblingId);
    debug.insert(QStringLiteral("source_previous_group_depth"), group.sourcePreviousGroupDepth);
    debug.insert(QStringLiteral("is_livery_section"), group.isLiverySection);
    debug.insert(QStringLiteral("livery_section_slot"), group.liverySectionSlot);
    QJsonArray sourceChildIds;
    for (const QString &childId : group.sourceChildIds) {
        sourceChildIds.append(childId);
    }
    debug.insert(QStringLiteral("source_child_ids"), sourceChildIds);
    object.insert(QStringLiteral("debug"), debug);
    return object;
}

QJsonObject projectToJson(const Project &project)
{
    QJsonObject object;
    object.insert(QStringLiteral("format"), QLatin1String(ProjectJsonFormat));
    object.insert(QStringLiteral("version"), ProjectJsonVersion);
    object.insert(QStringLiteral("name"), project.name);
    object.insert(QStringLiteral("source_folder"), project.sourceFolder);
    object.insert(QStringLiteral("is_livery"), project.isLivery);
    object.insert(QStringLiteral("source_dec_prefix"), toBase64(project.sourceDecPrefix));
    object.insert(QStringLiteral("source_header"), toBase64(project.sourceHeader));
    if (project.headerMetadata) {
        object.insert(QStringLiteral("header_metadata"), headerMetadataToJson(*project.headerMetadata));
    }

    QJsonArray layers;
    for (const ShapeLayer &layer : project.layers) {
        layers.append(shapeLayerToJson(layer));
    }
    object.insert(QStringLiteral("layers"), layers);

    QJsonArray guideLayers;
    for (const GuideLayer &guide : project.guideLayers) {
        guideLayers.append(guideLayerToJson(guide));
    }
    object.insert(QStringLiteral("guide_layers"), guideLayers);

    QJsonArray groups;
    for (const LayerGroup &group : project.groups) {
        groups.append(layerGroupToJson(group));
    }
    object.insert(QStringLiteral("groups"), groups);

    QJsonArray rootChildIds;
    for (const QString &childId : project.rootChildIds) {
        rootChildIds.append(childId);
    }
    object.insert(QStringLiteral("root_child_ids"), rootChildIds);

    QJsonArray colorSwatches;
    for (const std::array<quint8, 4> &color : project.colorSwatches) {
        colorSwatches.append(colorSwatchToJson(color));
    }
    object.insert(QStringLiteral("color_swatches"), colorSwatches);
    return object;
}

QByteArray encodeProjectDocument(const Project &project)
{
    const QJsonDocument document(projectToJson(project));
    return gzipCompress(document.toJson(QJsonDocument::Indented));
}

Project decodeProjectDocument(const QByteArray &fileBytes)
{
    const QByteArray json = looksGzipped(fileBytes) ? gzipDecompress(fileBytes) : fileBytes;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        throw std::runtime_error(("invalid project JSON: " + parseError.errorString()).toStdString());
    }
    if (!document.isObject()) {
        throw std::runtime_error("project document is not an object");
    }
    return projectFromJson(document.object());
}

namespace {

// Shared export scaffolding for flat and nested folders: create the folder, copy
// sidecars, write the given C_group payload, and emit the header (byte-exact
// rename of an imported header, or synthesized from headerMetadata).
void writeExportFolder(const Project &project, const QString &outputFolder,
                       const QString &name, const QByteArray &payload)
{
    if (outputFolder.isEmpty()) {
        throw std::runtime_error("export output folder is empty");
    }

    if (!QDir().mkpath(outputFolder)) {
        throw std::runtime_error(("could not create export folder: " + outputFolder).toStdString());
    }

    copyDirectoryContentsExceptCGroup(project.sourceFolder, outputFolder);

    QDir outputDir(outputFolder);
    writeCGroupFile(outputDir.filePath(QStringLiteral("C_group")), payload);

    const QString outputName = name.isEmpty() ? QFileInfo(outputFolder).fileName() : name;
    QByteArray headerBytes;
    if (!project.sourceHeader.isEmpty()) {
        headerBytes = renameHeader(project.sourceHeader, outputName); // imported: byte-exact + rename
    } else if (project.headerMetadata) {
        HeaderMetadata meta = *project.headerMetadata; // new vinyl: synthesize from metadata
        meta.name = outputName;
        headerBytes = buildHeader(meta);
    }
    if (!headerBytes.isEmpty()) {
        QFile headerFile(outputDir.filePath(QStringLiteral("header")));
        if (!headerFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            throw std::runtime_error(("could not write header: " + headerFile.fileName()).toStdString());
        }
        if (headerFile.write(headerBytes) != headerBytes.size()) {
            throw std::runtime_error("short write while writing header");
        }
    }
}

} // namespace

void exportFlatProjectFolder(const Project &project, const QString &outputFolder, const QString &name)
{
    writeExportFolder(project, outputFolder, name, buildFlatPayload(project));
}

void exportNestedProjectFolder(const Project &project, const QString &outputFolder,
                               const QString &name, const SpriteSizeFn &spriteSize)
{
    writeExportFolder(project, outputFolder, name, buildNestedPayload(project, spriteSize));
}

} // namespace fh6
