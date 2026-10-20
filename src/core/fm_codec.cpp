#include "fm_codec.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "fls_core.h"
#include "vinyl_scene_import.h"
#include "shape_registry.h"
#include "vinyl_decoder.h"
#include "vinyl_framing.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <variant>

namespace fls {
namespace {

using detail::readLeFloat;
using detail::readLeU16;
using detail::readLeU32;

bool isFM2023LiveryImpl(const QByteArray &fileData) {
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

bool isRawGyvlImpl(const QByteArray &fileData) {
    return fileData.size() >= 4
        && fileData[0] == 'g' && fileData[1] == 'y'
        && fileData[2] == 'v' && fileData[3] == 'l';
}

QByteArray inflateFM2023Blocks(const QByteArray &fileData) {
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

bool isFM2023ShapePayloadAt(const QByteArray &data, int idPos, int transformPos, int endPos) {
    if (idPos < 0 || endPos > data.size())
        return false;
    const quint16 encodedShapeId = readLeU16(data, idPos);
    const quint16 shapeId = detail::canonicalShapeId(encodedShapeId);
    if (!detail::isKnownShapeId(shapeId) && encodedShapeId < 0x8000)
        return false;
    return detail::isValidVinylShapeTransformAt(data, transformPos);
}

bool isFM2023BareShapeAt(const QByteArray &data, int pos) {
    if (pos < 0 || pos + 31 > data.size())
        return false;
    const quint8 marker = static_cast<quint8>(data[pos]);
    return (marker == 0x01 || marker == 0x02 || marker == 0x03)
        && isFM2023ShapePayloadAt(data, pos + 1, pos + 3, pos + 31);
}

bool isFM2023GroupAt(const QByteArray &data, int pos, bool counted) {
    if (pos < 0 || pos >= data.size()) {
        return false;
    }
    if (counted && static_cast<quint8>(data[pos]) != 0x20
        && static_cast<quint8>(data[pos]) != 0x60) {
        return false;
    }
    const auto header = detail::readVinylChildHeader(
        data, pos + (counted ? 1 : 0), data.size());

    return header && header->count > 0;
}

int fm2023TransformSizeAt(const QByteArray &data, int pos) {
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
    if (const auto attributes = detail::readVinylScaleAttributes(data, syPos, data.size())) {
        size += attributes->size;
    }
    const int next = pos + size;
    const bool groupFollows = isFM2023GroupAt(data, next, true)
        || isFM2023GroupAt(data, next, false)
        || (next + 1 < data.size()
            && (isFM2023GroupAt(data, next + 1, true)
                || isFM2023GroupAt(data, next + 1, false)));
    return groupFollows ? size : 0;
}

QByteArray normalizeFM2023Records(QByteArray data) {
    int pos = 0;
    while (pos + 31 <= data.size()) {
        if (pos + 32 <= data.size()
            && (static_cast<quint8>(data[pos]) == 0x00
                || static_cast<quint8>(data[pos]) == 0x01)
            && (static_cast<quint8>(data[pos + 1]) == 0x01
                || static_cast<quint8>(data[pos + 1]) == 0x02)
            && isFM2023ShapePayloadAt(data, pos + 2, pos + 4, pos + 32)) {
            data[pos + 1] = static_cast<char>(0x02);
            pos += 32;
            continue;
        }
        if (isFM2023BareShapeAt(data, pos)) {
            data[pos] = static_cast<char>(0x02);
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
                                                 const QVector<int> &sectionCounts) {
    return VinylTreeDecoder{fm2023DecoderOptions()}.buildLiverySections(
        body, sectionCounts, kFM2023LiverySlots, kFM2023SectionCount);
}

void applyFM2023ShapeMasks(VinylGroup &group, const QByteArray &layerData, bool root) {
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
                         const LayerData &layerData) {
    if (const auto attributes = detail::readVinylScaleAttributes(payload, 0x1d, payload.size())) {
        root.sy = *attributes->scaleY;
    }
    applyFM2023ShapeMasks(root, layerData.data, true);
}

VinylDecoderOptions fm2023DecoderOptions() {
    VinylDecoderOptions options;
    options.appendLiveryTailPadding = true;
    options.normalizeRecords = normalizeFM2023Records;
    options.finalizeGroup = finalizeFM2023Group;
    return options;
}

VinylGroup decodeFM2023RawGroupImpl(const QByteArray &payload, LayerData *decodedLayerData) {
    return VinylTreeDecoder{fm2023DecoderOptions()}.decodeGroup(payload, decodedLayerData);
}

QByteArray readFM2023AssetData(const QString &folderOrFile) {
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

    return fileData;
}

FM2023LiveryPayload readFM2023LiveryPayloadImpl(const QString &folderOrFile) {
    const QByteArray fileData = readFM2023AssetData(folderOrFile);

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

Project importFM2023Livery(const QString &folderOrFile, const QByteArray &fileData) {
    Q_UNUSED(fileData);
    const FM2023LiveryPayload payload = readFM2023LiveryPayloadImpl(folderOrFile);

    const QVector<LiverySection> sections =
        buildFM2023LiverySections(payload.gyvlBody, payload.sectionCounts);

    Project project = detail::newVinylImportProject(folderOrFile, payload.raw);
    project.isLivery = true;
    project.carId = payload.carId;
    project.liverySource = payload.raw;
    detail::appendLiverySectionsToScene(
        project, sections, detail::VinylSceneDialect::Motorsport2023);
    return project;
}

Project importFM2023RawGroup(const QString &folderOrFile, const QByteArray &fileData) {
    const QByteArray &payload = fileData;
    LayerData layerData;
    VinylGroup root = decodeFM2023RawGroupImpl(payload, &layerData);

    Project project = detail::newVinylImportProject(folderOrFile, payload);
    detail::appendVinylGroupToScene(
        project, root, layerData.start, detail::VinylSceneDialect::Motorsport2023);
    return project;
}

} // anonymous namespace

const LiverySlotDef kFM2023LiverySlots[7] = {
    {"Front", 0.0},  {"Back", 0.0},  {"Top", 0.0},
    {"Left", 180.0}, {"Right", 90.0},{"Spoiler", -90.0},
    {"FrontWindshield", 90.0},
};

} // namespace fls

bool fls::isFM2023Livery(const QByteArray &fileData) {
    return isFM2023LiveryImpl(fileData);
}

bool fls::isRawGyvl(const QByteArray &fileData) {
    return isRawGyvlImpl(fileData);
}

fls::FM2023LiveryPayload fls::readFM2023LiveryPayload(const QString &folderOrFile) {
    return readFM2023LiveryPayloadImpl(folderOrFile);
}

QVector<fls::LiverySection> fls::decodeFM2023LiverySections(const FM2023LiveryPayload &payload) {
    return buildFM2023LiverySections(payload.gyvlBody, payload.sectionCounts);
}

fls::VinylGroup fls::decodeFM2023RawGroup(const QByteArray &payload) {
    return decodeFM2023RawGroupImpl(payload, nullptr);
}

fls::Project fls::importFM2023Asset(const QString &folderOrFile) {
    const QByteArray fileData = readFM2023AssetData(folderOrFile);

    if (isFM2023LiveryImpl(fileData)) {
        return importFM2023Livery(folderOrFile, fileData);
    } else if (isRawGyvlImpl(fileData)) {
        return importFM2023RawGroup(folderOrFile, fileData);
    }
    throw std::runtime_error("not a recognized FM2023 asset format");
}
