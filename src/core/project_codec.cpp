#include "project_codec.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "flat_payload.h"
#include "header_codec.h"
#include "livery_codec.h"
#include "matrix_math.h"
#include "scene_codec.h"
#include "shape_registry.h"
#include "vinyl_decoder.h"
#include "vinyl_flattener.h"

#include <QtCore>

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <variant>

#include <zlib.h>

#ifndef FH6_PRIVACY_POLICY
#define FH6_PRIVACY_POLICY 1
#endif

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

QJsonObject liveryPaintColorToJson(const LiveryPaintColor &color)
{
    QJsonArray bgra;
    for (const quint8 channel : color.bgra) {
        bgra.append(channel);
    }
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), color.enabled);
    object.insert(QStringLiteral("bgra"), bgra);
    return object;
}

LiveryPaintColor liveryPaintColorFromJson(const QJsonValue &value)
{
    if (!value.isObject()) {
        throw std::runtime_error("livery paint color is not an object");
    }
    const QJsonObject object = value.toObject();
    const QJsonArray bgra = object.value(QStringLiteral("bgra")).toArray();
    if (bgra.size() != 4) {
        throw std::runtime_error("livery paint color must contain four BGRA channels");
    }
    LiveryPaintColor color;
    color.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    for (int channel = 0; channel < 4; ++channel) {
        color.bgra[static_cast<size_t>(channel)] = byteFromJson(bgra, channel);
    }
    return color;
}

quint32 liveryPaintU32FromJson(const QJsonValue &value, quint32 fallback)
{
    if (value.isUndefined()) {
        return fallback;
    }
    const double number = value.toDouble(-1.0);
    if (!std::isfinite(number) || number < 0.0
        || number > static_cast<double>(std::numeric_limits<quint32>::max())
        || std::floor(number) != number) {
        throw std::runtime_error("livery paint integer is outside the unsigned 32-bit range");
    }
    return static_cast<quint32>(number);
}

QJsonObject liveryPaintStateToJson(const LiveryPaintState &state)
{
    QJsonArray materials;
    for (const LiveryPaintMaterial &material : state.materials) {
        QJsonObject object;
        object.insert(QStringLiteral("material_hash"),
                      QStringLiteral("%1").arg(material.materialHash, 16, 16, QLatin1Char('0')));
        object.insert(QStringLiteral("primary"), liveryPaintColorToJson(material.primary));
        object.insert(QStringLiteral("secondary"), liveryPaintColorToJson(material.secondary));
        object.insert(QStringLiteral("manufacturer_selector"),
                      static_cast<qint64>(material.manufacturerSelector));
        object.insert(QStringLiteral("finish"), static_cast<qint64>(material.finish));
        materials.append(object);
    }
    QJsonObject object;
    object.insert(QStringLiteral("materials"), materials);
    return object;
}

LiveryPaintState liveryPaintStateFromJson(const QJsonValue &value)
{
    if (!value.isObject()) {
        throw std::runtime_error("livery paint metadata is not an object");
    }
    LiveryPaintState state;
    const QJsonArray materials = value.toObject().value(QStringLiteral("materials")).toArray();
    state.materials.reserve(materials.size());
    for (const QJsonValue &entry : materials) {
        if (!entry.isObject()) {
            throw std::runtime_error("livery paint material is not an object");
        }
        const QJsonObject object = entry.toObject();
        bool hashOk = false;
        const quint64 materialHash = object.value(QStringLiteral("material_hash"))
                                         .toString().toULongLong(&hashOk, 16);
        if (!hashOk) {
            throw std::runtime_error("livery paint material hash is invalid");
        }
        LiveryPaintMaterial material;
        material.materialHash = materialHash;
        material.primary = liveryPaintColorFromJson(object.value(QStringLiteral("primary")));
        material.secondary = liveryPaintColorFromJson(object.value(QStringLiteral("secondary")));
        material.manufacturerSelector = liveryPaintU32FromJson(
            object.value(QStringLiteral("manufacturer_selector")), 0xffffffffu);
        material.finish = liveryPaintU32FromJson(object.value(QStringLiteral("finish")), 0);
        state.materials.push_back(material);
    }
    return state;
}

bool looksGzipped(const QByteArray &bytes)
{
    return bytes.size() >= 2 && static_cast<quint8>(bytes[0]) == 0x1f
        && static_cast<quint8>(bytes[1]) == 0x8b;
}

QByteArray gzipCompress(const QByteArray &data)
{
    z_stream stream{};
    // zlib window bits with gzip framing.
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
    // zlib window bits with automatic gzip/zlib detection.
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

void enforcePrivacyPolicyForCGroup(const QByteArray &payload)
{
#if FH6_PRIVACY_POLICY
    const int gyvl = payload.indexOf(QByteArray("gyvl", 4));
    const int markerOffset = gyvl + 0x1d;
    if (gyvl >= 0 && markerOffset < payload.size()
        && static_cast<quint8>(payload.at(markerOffset)) == 0x21) {
        throw std::runtime_error("privacy policy blocks importing locked C_group");
    }
#else
    Q_UNUSED(payload);
#endif
}

void enforcePrivacyPolicyForCLivery(const LiveryPayload &livery)
{
#if FH6_PRIVACY_POLICY
    const int vlrc = livery.raw.indexOf(QByteArray("vlrc", 4));
    if (vlrc >= 0 && vlrc + 12 <= livery.raw.size()
        && detail::readLeU32(livery.raw, vlrc + 8) == 1) {
        throw std::runtime_error("privacy policy blocks importing locked C_livery");
    }
#else
    Q_UNUSED(livery);
#endif
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

int importedGroupTerminalDepth(const QHash<QString, const scene::Group *> &groupsById, const QString &groupId)
{
    const scene::Group *group = groupsById.value(groupId, nullptr);
    if (group == nullptr) {
        return 0;
    }
    if (group->children.empty()) {
        return 1;
    }
    const scene::Layer *last = group->children.back().get();
    if (last->kind() != scene::LayerKind::Group) {
        return 1;
    }
    return 1 + importedGroupTerminalDepth(groupsById, last->id);
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

namespace {

struct LegacyShapeRecord {
    QString id;
    QString name = QStringLiteral("Shape");
    quint16 vectorId = 0;
    bool logo = false;
    quint32 logoId = 0;
    int width = 256;
    int height = 256;
    scene::Transform2D transform;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool visible = true;
    bool locked = false;
    bool mask = false;
    int sourceShape = 0;
    int absOffset = 0;
    QByteArray marker;
    int flags = 0;
    quint16 sourceLogoId = 0;
    bool hasSourceTransform = false;
    scene::Transform2D sourceTransform;
};

struct LegacyGuideRecord {
    QString id;
    QString name = QStringLiteral("Guide");
    QString sourcePath;
    QByteArray imageBytes;
    QByteArray pixelBytes;
    QString imageFormat;
    int width = 0;
    int height = 0;
    scene::Transform2D transform;
    double opacity = 0.5;
    bool visible = true;
    bool locked = false;
};

struct LegacyGroupRecord {
    QString id;
    QString name = QStringLiteral("Group");
    QVector<QString> children;
    bool locked = false;
    scene::Transform2D transform;
    int sourceAbsPos = 0;
    QByteArray pendingTransformMarker;
    QByteArray inlineTransformMarker;
    QByteArray effectiveTransformMarker;
    QByteArray headerControlBytes;
    int flags = 0;
    QString sourceParentId;
    QString sourcePreviousSiblingId;
    int sourcePreviousGroupDepth = 0;
    QVector<QString> sourceChildren;
    bool isLiverySection = false;
    int liverySectionSlot = -1;
};

LegacyShapeRecord legacyShapeFromJson(const QJsonObject &object)
{
    LegacyShapeRecord layer;
    layer.id = object.value(QStringLiteral("id")).toString();
    layer.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Shape"));
    layer.vectorId = static_cast<quint16>(object.value(QStringLiteral("shape_id")).toInt());
    layer.logo = object.value(QStringLiteral("raster")).toBool(false);
    layer.logoId = static_cast<quint32>(object.value(QStringLiteral("raster_id")).toInteger(0));
    layer.width = object.value(QStringLiteral("raster_width")).toInt(256);
    layer.height = object.value(QStringLiteral("raster_height")).toInt(256);
    layer.transform.x = object.value(QStringLiteral("x")).toDouble(0.0);
    layer.transform.y = object.value(QStringLiteral("y")).toDouble(0.0);
    layer.transform.scaleX = object.value(QStringLiteral("scale_x")).toDouble(1.0);
    layer.transform.scaleY = object.value(QStringLiteral("scale_y")).toDouble(1.0);
    layer.transform.rotation = normalizeRotation(object.value(QStringLiteral("rotation")).toDouble(0.0));
    layer.transform.skew = object.value(QStringLiteral("skew")).toDouble(0.0);
    layer.visible = object.value(QStringLiteral("visible")).toBool(true);
    layer.locked = object.value(QStringLiteral("locked")).toBool(false);
    layer.mask = object.value(QStringLiteral("mask")).toBool(false);
    const QJsonObject debug = object.value(QStringLiteral("debug")).toObject();
    layer.sourceShape = debug.value(QStringLiteral("source_shape")).toInt(0);
    layer.absOffset = debug.value(QStringLiteral("abs_offset")).toInt(0);
    layer.marker = QByteArray::fromHex(debug.value(QStringLiteral("marker")).toString().toLatin1());
    layer.flags = debug.value(QStringLiteral("flags")).toInt(0);
    layer.sourceLogoId = static_cast<quint16>(debug.value(QStringLiteral("source_logo_id")).toInt(0));
    layer.hasSourceTransform = debug.value(QStringLiteral("has_source_transform")).toBool(false);
    if (layer.hasSourceTransform) {
        layer.sourceTransform = scene::Transform2D{
            debug.value(QStringLiteral("source_x")).toDouble(layer.transform.x),
            debug.value(QStringLiteral("source_y")).toDouble(layer.transform.y),
            debug.value(QStringLiteral("source_scale_x")).toDouble(layer.transform.scaleX),
            debug.value(QStringLiteral("source_scale_y")).toDouble(layer.transform.scaleY),
            normalizeRotation(debug.value(QStringLiteral("source_rotation")).toDouble(layer.transform.rotation)),
            debug.value(QStringLiteral("source_skew")).toDouble(layer.transform.skew),
        };
    }

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

LegacyGuideRecord legacyGuideFromJson(const QJsonObject &object)
{
    LegacyGuideRecord guide;
    guide.id = object.value(QStringLiteral("id")).toString();
    guide.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Guide"));
    guide.sourcePath = object.value(QStringLiteral("source_path")).toString();
    guide.imageBytes = QByteArray::fromBase64(object.value(QStringLiteral("image_bytes")).toString().toLatin1());
    guide.pixelBytes = QByteArray::fromBase64(object.value(QStringLiteral("pixel_bytes")).toString().toLatin1());
    guide.imageFormat = object.value(QStringLiteral("image_format")).toString();
    guide.width = object.value(QStringLiteral("width")).toInt(0);
    guide.height = object.value(QStringLiteral("height")).toInt(0);
    guide.transform.x = object.value(QStringLiteral("x")).toDouble(0.0);
    guide.transform.y = object.value(QStringLiteral("y")).toDouble(0.0);
    guide.transform.scaleX = object.value(QStringLiteral("scale_x")).toDouble(1.0);
    guide.transform.scaleY = object.value(QStringLiteral("scale_y")).toDouble(1.0);
    guide.transform.rotation = normalizeRotation(object.value(QStringLiteral("rotation")).toDouble(0.0));
    guide.opacity = object.value(QStringLiteral("opacity")).toDouble(0.5);
    guide.visible = object.value(QStringLiteral("visible")).toBool(true);
    guide.locked = object.value(QStringLiteral("locked")).toBool(false);
    return guide;
}

LegacyGroupRecord legacyGroupFromJson(const QJsonObject &object)
{
    LegacyGroupRecord group;
    group.id = object.value(QStringLiteral("id")).toString();
    group.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Group"));
    group.locked = object.value(QStringLiteral("locked")).toBool(false);

    const QJsonArray children = object.value(QStringLiteral("child_ids")).toArray();
    group.children.reserve(children.size());
    for (const QJsonValue &value : children) {
        group.children.push_back(value.toString());
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
    const QJsonArray sourceChildren = debug.value(QStringLiteral("source_child_ids")).toArray();
    group.sourceChildren.reserve(sourceChildren.size());
    for (const QJsonValue &value : sourceChildren) {
        group.sourceChildren.push_back(value.toString());
    }
    return group;
}

std::unique_ptr<scene::Shape> makeSceneShape(const LegacyShapeRecord &src)
{
    auto shape = std::make_unique<scene::Shape>();
    shape->id = src.id;
    shape->name = src.name;
    shape->transform = src.transform;
    shape->opacity = static_cast<double>(src.color[3]) / 255.0;
    shape->color = src.color;
    shape->visible = src.visible;
    shape->locked = src.locked;
    shape->mask = src.mask;
    if (src.logo) {
        shape->setRasterShape(src.logoId, src.width, src.height);
    } else {
        shape->setVectorShape(src.vectorId);
    }
    shape->sourceShape = src.sourceShape;
    shape->absOffset = src.absOffset;
    shape->marker = src.marker;
    shape->flags = src.flags;
    shape->sourceLogoId = src.sourceLogoId;
    shape->hasSourceTransform = src.hasSourceTransform;
    shape->sourceTransform = src.sourceTransform;
    return shape;
}

std::unique_ptr<scene::GuideLayer> makeSceneGuide(const LegacyGuideRecord &src)
{
    auto guide = std::make_unique<scene::GuideLayer>();
    guide->id = src.id;
    guide->name = src.name;
    guide->transform = src.transform;
    guide->opacity = src.opacity;
    guide->visible = src.visible;
    guide->locked = src.locked;
    guide->sourcePath = src.sourcePath;
    auto image = std::make_unique<scene::RasterContainer>();
    image->width = src.width;
    image->height = src.height;
    image->pixels = src.pixelBytes;
    image->encoded = src.imageBytes;
    image->format = src.imageFormat;
    guide->image = std::move(image);
    return guide;
}

void copyGroupRecord(const LegacyGroupRecord &src, scene::Group &dst)
{
    dst.id = src.id;
    dst.name = src.name;
    dst.locked = src.locked;
    dst.transform = src.transform;
    dst.sourceAbsPos = src.sourceAbsPos;
    dst.pendingTransformMarker = src.pendingTransformMarker;
    dst.inlineTransformMarker = src.inlineTransformMarker;
    dst.effectiveTransformMarker = src.effectiveTransformMarker;
    dst.headerControlBytes = src.headerControlBytes;
    dst.flags = src.flags;
    dst.sourceParentId = src.sourceParentId;
    dst.sourcePreviousSiblingId = src.sourcePreviousSiblingId;
    dst.sourcePreviousGroupDepth = src.sourcePreviousGroupDepth;
    dst.sourceChildren = src.sourceChildren;
    dst.isLiverySection = src.isLiverySection;
    dst.liverySectionSlot = src.liverySectionSlot;
}

std::unique_ptr<scene::Group> buildSceneFromLegacyRecords(const QVector<LegacyShapeRecord> &shapes,
                                                          const QVector<LegacyGuideRecord> &guides,
                                                          const QVector<LegacyGroupRecord> &groups,
                                                          const QVector<QString> &roots)
{
    QHash<QString, const LegacyShapeRecord *> shapeById;
    QHash<QString, const LegacyGuideRecord *> guideById;
    QHash<QString, const LegacyGroupRecord *> groupById;
    QSet<QString> consumedShapes;
    QSet<QString> consumedGuides;
    for (const LegacyShapeRecord &shape : shapes) {
        shapeById.insert(shape.id, &shape);
    }
    for (const LegacyGuideRecord &guide : guides) {
        guideById.insert(guide.id, &guide);
    }
    for (const LegacyGroupRecord &group : groups) {
        groupById.insert(group.id, &group);
    }

    auto root = std::make_unique<scene::Group>();
    root->id = QStringLiteral("__root__");
    root->name = QStringLiteral("Project");

    std::function<std::unique_ptr<scene::Layer>(const QString &)> buildNode =
        [&](const QString &id) -> std::unique_ptr<scene::Layer> {
            if (const LegacyGroupRecord *group = groupById.value(id, nullptr)) {
                auto out = std::make_unique<scene::Group>();
                copyGroupRecord(*group, *out);
                for (const QString &entry : group->children) {
                    if (auto child = buildNode(entry)) {
                        out->append(std::move(child));
                    }
                }
                return out;
            }
            if (const LegacyShapeRecord *shape = shapeById.value(id, nullptr)) {
                consumedShapes.insert(id);
                return makeSceneShape(*shape);
            }
            if (const LegacyGuideRecord *guide = guideById.value(id, nullptr)) {
                consumedGuides.insert(id);
                return makeSceneGuide(*guide);
            }
            return nullptr;
        };

    for (const QString &id : roots) {
        if (auto child = buildNode(id)) {
            root->append(std::move(child));
        }
    }
    for (const LegacyShapeRecord &shape : shapes) {
        if (!consumedShapes.contains(shape.id)) {
            root->append(makeSceneShape(shape));
        }
    }
    for (const LegacyGuideRecord &guide : guides) {
        if (!consumedGuides.contains(guide.id)) {
            root->append(makeSceneGuide(guide));
        }
    }
    return root;
}

} // namespace


Project importCGroupFlat(const QString &folderOrFile)
{
    const QByteArray payload = readCGroupPayload(folderOrFile);
    enforcePrivacyPolicyForCGroup(payload);
    LayerData layerData;
    const VinylGroup root = decodeGroup(payload, &layerData);
    const QVector<FlattenedLayer> flat = flattenGroup(root);

    Project project = newImportProject(folderOrFile, payload);
    scene::ensureProjectSceneRoot(project);
    int index = 1;
    for (const FlattenedLayer &flatLayer : flat) {
        const Matrix3 effective = detail::multiply(flatLayer.groupMatrix, shapeMatrix(flatLayer));
        LegacyShapeRecord layer;
        layer.id = layerIdForIndex(index);
        layer.transform = decomposeTransform2D(effective);
        layer.hasSourceTransform = true;
        layer.sourceTransform = layer.transform;
        layer.vectorId = flatLayer.shapeId;
        layer.logo = flatLayer.raster;
        layer.logoId = flatLayer.rasterId;
        layer.width = flatLayer.rasterWidth;
        layer.height = flatLayer.rasterHeight;
        layer.name = QStringLiteral("%1 %2")
            .arg(index, 4, 10, QLatin1Char('0'))
            .arg(flatLayer.raster
                     ? QStringLiteral("Decal %1").arg(flatLayer.rasterId)
                     : detail::shapeName(flatLayer.shapeId));
        layer.color = flatLayer.color;
        layer.mask = flatLayer.isMask;
        layer.visible = true;
        layer.locked = false;
        layer.sourceShape = index;
        layer.absOffset = flatLayer.absPos + layerData.start;
        layer.marker = flatLayer.marker;
        layer.flags = flatLayer.flags;
        layer.sourceLogoId = flatLayer.sourceLogoId;
        project.root->append(makeSceneShape(layer));
        ++index;
    }
    return project;
}

Project importCGroupNested(const QString &folderOrFile)
{
    const QByteArray payload = readCGroupPayload(folderOrFile);
    enforcePrivacyPolicyForCGroup(payload);
    LayerData layerData;
    const VinylGroup root = decodeGroup(payload, &layerData);
    Project project = newImportProject(folderOrFile, payload);
    scene::ensureProjectSceneRoot(project);

    int shapeIndex = 0;
    int groupIndex = 0;
    QHash<QString, const scene::Group *> groupsById;

    std::function<std::unique_ptr<scene::Shape>(const VinylShape &, const Matrix3 &, bool)> importShape =
        [&](const VinylShape &shape, const Matrix3 &parentMatrix, bool parentMask) -> std::unique_ptr<scene::Shape> {
            ++shapeIndex;
            const Matrix3 effective = detail::multiply(parentMatrix, shapeMatrixForShape(shape));
            LegacyShapeRecord layer;
            layer.transform = decomposeTransform2D(effective);
            layer.hasSourceTransform = true;
            layer.sourceTransform = layer.transform;
            layer.id = layerIdForIndex(shapeIndex);
            layer.name = QStringLiteral("%1 %2")
                .arg(shapeIndex, 4, 10, QLatin1Char('0'))
                .arg(shape.isLogo
                         ? QStringLiteral("Decal %1").arg(shape.rasterId)
                         : detail::shapeName(shape.shapeId));
            layer.vectorId = shape.shapeId;
            layer.logo = shape.isLogo;
            layer.logoId = shape.rasterId;
            layer.width = 256;
            layer.height = 256;
            layer.color = shape.color;
            layer.mask = parentMask || (shape.isMask && !hasColorData(shape.color));
            layer.visible = true;
            layer.locked = false;
            layer.sourceShape = shapeIndex;
            layer.absOffset = shape.absPos + layerData.start;
            layer.marker = shape.marker;
            layer.flags = shape.flags;
            layer.sourceLogoId = shape.logoId;
            return makeSceneShape(layer);
        };

    std::function<std::unique_ptr<scene::Group>(const VinylGroup &, const Matrix3 &, bool, const QString &, const QString &)> importGroup =
        [&](const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask,
            const QString &sourceParentId, const QString &sourcePreviousSiblingId) -> std::unique_ptr<scene::Group> {
            ++groupIndex;
            const Matrix3 groupMatrix = detail::multiply(parentMatrix, nodeMatrix(node));
            const bool inheritedMask = parentMask || node.isMask;

            auto group = std::make_unique<scene::Group>();
            group->id = groupIdForIndex(groupIndex);
            group->name = QStringLiteral("Group %1").arg(groupIndex);
            group->sourceAbsPos = node.absPos + layerData.start;
            group->pendingTransformMarker = node.pendingTransformMarker;
            group->inlineTransformMarker = node.inlineTransformMarker;
            group->effectiveTransformMarker = effectiveDebugMarker(node);
            group->headerControlBytes = node.headerControlBytes;
            group->flags = node.flags;
            group->sourceParentId = sourceParentId;
            group->sourcePreviousSiblingId = sourcePreviousSiblingId;
            group->sourcePreviousGroupDepth = importedGroupTerminalDepth(groupsById, sourcePreviousSiblingId);
            groupsById.insert(group->id, group.get());

            for (const VinylItem &item : node.items) {
                const QString previousSiblingId = group->children.empty()
                    ? QString()
                    : group->children.back()->id;
                if (item.isShape()) {
                    group->append(importShape(std::get<VinylShape>(item.value), groupMatrix, inheritedMask));
                } else {
                    group->append(importGroup(*std::get<VinylGroupPtr>(item.value),
                                              groupMatrix,
                                              inheritedMask,
                                              group->id,
                                              previousSiblingId));
                }
            }
            for (const auto &child : group->children) {
                group->sourceChildren.push_back(child->id);
            }
            return group;
        };

    const Matrix3 rootMatrix = nodeMatrix(root);
    const bool rootMask = root.isMask;
    for (const VinylItem &item : root.items) {
        const QString previousSiblingId = project.root->children.empty() ? QString() : project.root->children.back()->id;
        if (item.isShape()) {
            project.root->append(importShape(std::get<VinylShape>(item.value), rootMatrix, rootMask));
        } else {
            project.root->append(importGroup(*std::get<VinylGroupPtr>(item.value),
                                             rootMatrix,
                                             rootMask,
                                             QStringLiteral("__root__"),
                                             previousSiblingId));
        }
    }
    return project;
}

Project importCLivery(const QString &folderOrFile)
{
    const LiveryPayload livery = readLiveryPayload(folderOrFile);
    enforcePrivacyPolicyForCLivery(livery);
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
    project.carId = livery.carId;
    project.liverySource = livery.raw;
    project.liveryPaint = livery.paint;
    project.sourceHeader = readOptionalFile(QDir(project.sourceFolder).filePath(QStringLiteral("header")));
    scene::ensureProjectSceneRoot(project);

    int shapeIndex = 0;
    int groupIndex = 0;

    std::function<std::unique_ptr<scene::Shape>(const VinylShape &, const Matrix3 &, bool)> importShape =
        [&](const VinylShape &shape, const Matrix3 &parentMatrix, bool parentMask) -> std::unique_ptr<scene::Shape> {
            ++shapeIndex;
            const Matrix3 effective = detail::multiply(parentMatrix, shapeMatrixForShape(shape));
            LegacyShapeRecord layer;
            layer.transform = decomposeTransform2D(effective);
            layer.hasSourceTransform = true;
            layer.sourceTransform = layer.transform;
            layer.id = layerIdForIndex(shapeIndex);
            layer.name = QStringLiteral("%1 %2")
                .arg(shapeIndex, 4, 10, QLatin1Char('0'))
                .arg(shape.isLogo
                         ? QStringLiteral("Decal %1").arg(shape.rasterId)
                         : detail::shapeName(shape.shapeId));
            layer.vectorId = shape.shapeId;
            layer.logo = shape.isLogo;
            layer.logoId = shape.rasterId;
            layer.width = 256;
            layer.height = 256;
            layer.color = shape.color;
            layer.mask = parentMask || (shape.isMask && !hasColorData(shape.color));
            layer.visible = true;
            layer.locked = false;
            layer.sourceShape = shapeIndex;
            layer.absOffset = shape.absPos;
            layer.marker = shape.marker;
            layer.flags = shape.flags;
            layer.sourceLogoId = shape.logoId;
            return makeSceneShape(layer);
        };

    std::function<std::unique_ptr<scene::Group>(const VinylGroup &, const Matrix3 &, bool)> importGroup =
        [&](const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask) -> std::unique_ptr<scene::Group> {
            ++groupIndex;
            const Matrix3 groupMatrix = detail::multiply(parentMatrix, nodeMatrix(node));
            const bool inheritedMask = parentMask || node.isMask;

            auto group = std::make_unique<scene::Group>();
            group->id = groupIdForIndex(groupIndex);
            group->name = QStringLiteral("Group %1").arg(groupIndex);
            group->sourceAbsPos = node.absPos;
            group->pendingTransformMarker = node.pendingTransformMarker;
            group->inlineTransformMarker = node.inlineTransformMarker;
            group->effectiveTransformMarker = effectiveDebugMarker(node);
            group->headerControlBytes = node.headerControlBytes;
            group->flags = node.flags;

            for (const VinylItem &item : node.items) {
                if (item.isShape()) {
                    group->append(importShape(std::get<VinylShape>(item.value), groupMatrix, inheritedMask));
                } else {
                    group->append(importGroup(*std::get<VinylGroupPtr>(item.value), groupMatrix, inheritedMask));
                }
            }
            for (const auto &child : group->children) {
                group->sourceChildren.push_back(child->id);
            }
            return group;
        };

    for (const LiverySection &section : sections) {
        ++groupIndex;
        auto sectionGroup = std::make_unique<scene::Group>();
        sectionGroup->id = groupIdForIndex(groupIndex);
        sectionGroup->name = section.name;
        sectionGroup->isLiverySection = true;
        sectionGroup->liverySectionSlot = section.slot;
        sectionGroup->sourceAbsPos = section.absPos;

        if (section.populated) {
            Matrix3 sectionMatrix = nodeMatrix(section.subtree);
            bool sectionMask = section.subtree.isMask;
            const VinylGroup *sectionRoot = &section.subtree;
            const auto appendItems = [&](const QVector<VinylItem> &items, int begin,
                                         const Matrix3 &matrix, bool mask) {
                for (int i = begin; i < items.size(); ++i) {
                    const VinylItem &item = items[i];
                    if (item.isShape()) {
                        sectionGroup->append(importShape(std::get<VinylShape>(item.value), matrix, mask));
                    } else {
                        sectionGroup->append(importGroup(*std::get<VinylGroupPtr>(item.value), matrix, mask));
                    }
                }
            };
            const VinylGroupPtr leadingRoot = !sectionRoot->items.isEmpty()
                    && !sectionRoot->items.front().isShape()
                ? std::get<VinylGroupPtr>(sectionRoot->items.front().value)
                : VinylGroupPtr{};
            if (leadingRoot && leadingRoot->source == QStringLiteral("livery_section_root")) {
                const Matrix3 leadingMatrix = detail::multiply(sectionMatrix, nodeMatrix(*leadingRoot));
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
                    if (!topGroup) {
                        break;
                    }
                    if (topGroup->source.contains(QStringLiteral("livery_section_span"))) {
                        break;
                    }
                    sectionMatrix = detail::multiply(sectionMatrix, nodeMatrix(*topGroup));
                    sectionMask = sectionMask || topGroup->isMask;
                    sectionRoot = topGroup.get();
                }
                sectionMatrix = detail::multiply(liverySectionCanvasTransform(section.slot), sectionMatrix);
                appendItems(sectionRoot->items, 0, sectionMatrix, sectionMask);
            }
            for (const auto &child : sectionGroup->children) {
                sectionGroup->sourceChildren.push_back(child->id);
            }
        }
        project.root->append(std::move(sectionGroup));
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
    project.carId = object.value(QStringLiteral("car_id")).toInt(0);
    const auto loadGuidelines = [](const QJsonValue &value) {
        QVector<double> result;
        const QJsonArray array = value.toArray();
        result.reserve(array.size());
        for (const QJsonValue &entry : array) {
            const double coordinate = entry.toDouble(std::numeric_limits<double>::quiet_NaN());
            if (std::isfinite(coordinate)) {
                result.push_back(coordinate);
            }
        }
        return result;
    };
    project.horizontalGuidelines = loadGuidelines(object.value(QStringLiteral("horizontal_guidelines")));
    project.verticalGuidelines = loadGuidelines(object.value(QStringLiteral("vertical_guidelines")));
    const QString prefix = object.value(QStringLiteral("source_dec_prefix")).toString();
    if (!prefix.isEmpty()) {
        project.sourceDecPrefix = QByteArray::fromBase64(prefix.toLatin1());
    }
    const QString header = object.value(QStringLiteral("source_header")).toString();
    if (!header.isEmpty()) {
        project.sourceHeader = QByteArray::fromBase64(header.toLatin1());
    }
    const QString liverySource = object.value(QStringLiteral("livery_source")).toString();
    if (!liverySource.isEmpty()) {
        project.liverySource = QByteArray::fromBase64(liverySource.toLatin1());
        try {
            project.liveryPaint = parseInflatedLiveryPayload(project.liverySource).paint;
        } catch (const std::exception &) {
        }
    }
    if (object.contains(QStringLiteral("livery_paint"))) {
        project.liveryPaint = liveryPaintStateFromJson(object.value(QStringLiteral("livery_paint")));
    }
    if (object.value(QStringLiteral("header_metadata")).isObject()) {
        project.headerMetadata = headerMetadataFromJson(object.value(QStringLiteral("header_metadata")).toObject());
    }

    if (object.value(QStringLiteral("root")).isObject()) {
        project.root = scene::sceneTreeFromJson(object.value(QStringLiteral("root")).toObject());
        std::function<void(const scene::Layer &)> scan = [&](const scene::Layer &node) {
            if (node.kind() != scene::LayerKind::Group) {
                return;
            }
            const auto &group = static_cast<const scene::Group &>(node);
            project.isLivery = project.isLivery || group.isLiverySection;
            for (const auto &child : group.children) {
                scan(*child);
            }
        };
        scan(*project.root);
        const QJsonArray colorSwatches = object.value(QStringLiteral("color_swatches")).toArray();
        project.colorSwatches.reserve(colorSwatches.size());
        for (const QJsonValue &value : colorSwatches) {
            project.colorSwatches.push_back(colorSwatchFromJson(value));
        }
        return project;
    }

    const QJsonArray layers = object.value(QStringLiteral("layers")).toArray();
    QVector<LegacyShapeRecord> shapeRecords;
    shapeRecords.reserve(layers.size());
    for (const QJsonValue &value : layers) {
        if (!value.isObject()) {
            throw std::runtime_error("project layer entry is not an object");
        }
        shapeRecords.push_back(legacyShapeFromJson(value.toObject()));
    }
    const QJsonArray guideLayers = object.value(QStringLiteral("guide_layers")).toArray();
    QVector<LegacyGuideRecord> guideRecords;
    guideRecords.reserve(guideLayers.size());
    for (const QJsonValue &value : guideLayers) {
        if (!value.isObject()) {
            throw std::runtime_error("project guide layer entry is not an object");
        }
        guideRecords.push_back(legacyGuideFromJson(value.toObject()));
    }
    const QJsonArray groups = object.value(QStringLiteral("groups")).toArray();
    QVector<LegacyGroupRecord> groupRecords;
    groupRecords.reserve(groups.size());
    for (const QJsonValue &value : groups) {
        if (!value.isObject()) {
            throw std::runtime_error("project group entry is not an object");
        }
        groupRecords.push_back(legacyGroupFromJson(value.toObject()));
        project.isLivery = project.isLivery || groupRecords.back().isLiverySection;
    }

    const QJsonArray rootEntriesJson = object.value(QStringLiteral("root_child_ids")).toArray();
    QVector<QString> rootEntries;
    rootEntries.reserve(rootEntriesJson.size());
    for (const QJsonValue &value : rootEntriesJson) {
        rootEntries.push_back(value.toString());
    }
    const QJsonArray colorSwatches = object.value(QStringLiteral("color_swatches")).toArray();
    project.colorSwatches.reserve(colorSwatches.size());
    for (const QJsonValue &value : colorSwatches) {
        project.colorSwatches.push_back(colorSwatchFromJson(value));
    }
    project.root = buildSceneFromLegacyRecords(shapeRecords, guideRecords, groupRecords, rootEntries);
    return project;
}

QJsonObject projectToJson(const Project &project)
{
    QJsonObject object;
    object.insert(QStringLiteral("format"), QLatin1String(ProjectJsonFormat));
    object.insert(QStringLiteral("version"), ProjectJsonVersion);
    object.insert(QStringLiteral("name"), project.name);
    object.insert(QStringLiteral("source_folder"), project.sourceFolder);
    object.insert(QStringLiteral("is_livery"), project.isLivery);
    const auto saveGuidelines = [](const QVector<double> &guidelines) {
        QJsonArray array;
        for (double coordinate : guidelines) {
            if (std::isfinite(coordinate)) {
                array.append(coordinate);
            }
        }
        return array;
    };
    object.insert(QStringLiteral("horizontal_guidelines"), saveGuidelines(project.horizontalGuidelines));
    object.insert(QStringLiteral("vertical_guidelines"), saveGuidelines(project.verticalGuidelines));
    if (project.carId != 0) {
        object.insert(QStringLiteral("car_id"), project.carId);
    }
    object.insert(QStringLiteral("source_dec_prefix"), toBase64(project.sourceDecPrefix));
    object.insert(QStringLiteral("source_header"), toBase64(project.sourceHeader));
    if (!project.liverySource.isEmpty()) {
        object.insert(QStringLiteral("livery_source"), toBase64(project.liverySource));
    }
    if (!project.liveryPaint.materials.isEmpty()) {
        object.insert(QStringLiteral("livery_paint"), liveryPaintStateToJson(project.liveryPaint));
    }
    if (project.headerMetadata) {
        object.insert(QStringLiteral("header_metadata"), headerMetadataToJson(*project.headerMetadata));
    }

    if (project.root) {
        object.insert(QStringLiteral("root"), scene::sceneTreeToJson(*project.root));
    } else {
        const scene::Group emptyRoot;
        object.insert(QStringLiteral("root"), scene::sceneTreeToJson(emptyRoot));
    }

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
        headerBytes = renameHeader(project.sourceHeader, outputName);
    } else if (project.headerMetadata) {
        HeaderMetadata meta = *project.headerMetadata;
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

void exportNestedProjectFolder(const Project &project, const QString &outputFolder,
                               const QString &name, const SpriteSizeFn &spriteSize)
{
    writeExportFolder(project, outputFolder, name, buildNestedPayload(project, spriteSize));
}

namespace {

void writeLeU32InPlace(QByteArray &bytes, int offset, quint32 value)
{
    bytes[offset] = static_cast<char>(value & 0xff);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<char>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<char>((value >> 24) & 0xff);
}

void writeRawFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error(("could not write file: " + path).toStdString());
    }
    if (file.write(bytes) != bytes.size()) {
        throw std::runtime_error(("short write while writing " + path).toStdString());
    }
}

} // namespace

QByteArray encodeCLiveryPayload(const Project &project)
{
    if (!project.isLivery) {
        throw std::runtime_error("not a livery project");
    }
    if (project.liverySource.isEmpty()) {
        throw std::runtime_error(
            "this livery has no captured source to re-encode; re-import the C_livery "
            "(authoring livery artwork from scratch is not yet supported)");
    }
    const LiveryPayload source = [&project]() {
        LiveryPayload payload;
        payload.raw = project.liverySource;
        const int gyvl = payload.raw.indexOf(QByteArray("gyvl", 4));
        if (gyvl < 0) {
            throw std::runtime_error("livery source is missing its gyvl artwork chunk");
        }
        payload.gyvlOffset = gyvl;
        const int bodyStart = gyvl + 0x15;
        int bodyEnd = payload.raw.indexOf(QByteArray("yrvl", 4), gyvl);
        if (bodyEnd < 0 || bodyEnd < bodyStart) {
            throw std::runtime_error("livery source is missing its post-gyvl stats chunk");
        }
        payload.body = payload.raw.mid(bodyStart, bodyEnd - bodyStart);
        return payload;
    }();

    std::array<int, 11> counts{};
    const QByteArray gyvl = buildLiveryGyvl(project, &counts);
    const int oldGyvlEnd = source.gyvlOffset + 0x15 + source.body.size();
    if (oldGyvlEnd > project.liverySource.size()) {
        throw std::runtime_error("livery source gyvl chunk is truncated");
    }

    QByteArray payload = project.liverySource.left(source.gyvlOffset);
    payload.append(gyvl);
    payload.append(project.liverySource.mid(oldGyvlEnd));

    const int vlrc = payload.indexOf(QByteArray("vlrc", 4));
    if (vlrc < 0 || vlrc + 0x14 > payload.size()) {
        throw std::runtime_error("livery source is missing its vlrc root header");
    }
    writeLeU32InPlace(payload, vlrc + 0x10, static_cast<quint32>(project.carId));

    if (source.gyvlOffset < 4) {
        throw std::runtime_error("livery source is missing its gyvl length field");
    }
    writeLeU32InPlace(payload, source.gyvlOffset - 4, static_cast<quint32>(gyvl.size()));

    const int statsTag = source.gyvlOffset + gyvl.size();
    if (statsTag + 52 > payload.size() || payload.mid(statsTag, 4) != QByteArray("yrvl", 4)) {
        throw std::runtime_error("re-encoded livery is missing its post-gyvl stats chunk");
    }
    for (int i = 0; i < 11; ++i) {
        writeLeU32InPlace(payload, statsTag + 4 + i * 4, static_cast<quint32>(counts[static_cast<size_t>(i)]));
    }
    return payload;
}

void exportCLivery(const Project &project, const QString &outputFolder)
{
    if (outputFolder.isEmpty()) {
        throw std::runtime_error("export output folder is empty");
    }
    const QByteArray payload = encodeCLiveryPayload(project);

    if (!QDir().mkpath(outputFolder)) {
        throw std::runtime_error(("could not create export folder: " + outputFolder).toStdString());
    }
    QDir outputDir(outputFolder);

    writeCGroupFile(outputDir.filePath(QStringLiteral("C_livery")), payload);

    QByteArray header = project.sourceHeader;
    if (!header.isEmpty()) {
        if (header.size() >= 20) {
            writeLeU32InPlace(header, header.size() - 20, static_cast<quint32>(project.carId));
        }
        writeRawFile(outputDir.filePath(QStringLiteral("header")), header);
    }

    if (!project.sourceFolder.isEmpty()) {
        const QString thumb = QDir(project.sourceFolder).filePath(QStringLiteral("bigThumb.webp"));
        if (QFile::exists(thumb)) {
            copyFileReplacing(thumb, outputDir.filePath(QStringLiteral("bigThumb.webp")));
        }
    }
}

} // namespace fh6
