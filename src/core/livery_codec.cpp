#include "livery_codec.h"
#include "vinyl_framing.h"
#include "vinyl_encoding.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "fm_codec.h"
#include "layer.h"
#include "matrix_math.h"
#include "vinyl_decoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointF>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace fls {
namespace {

constexpr int kCurrentLiverySectionCount = 11;
constexpr int kLegacyLiveryStatsCount = 7;
constexpr int kLegacyLiverySectionCount = 6;
constexpr int kEarlyLiverySectionCount = 6;
constexpr quint32 kEarlyLiveryVersion = 7;

QString resolveLiveryPath(const QString &folderOrFile) {
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        return QDir(folderOrFile).filePath(QStringLiteral("C_livery"));
    }
    return folderOrFile;
}

quint64 readLeU64(const QByteArray &bytes, int offset) {
    return static_cast<quint64>(detail::readLeU32(bytes, offset))
        | (static_cast<quint64>(detail::readLeU32(bytes, offset + 4)) << 32);
}

// Read `count` 27-byte paint-material records starting at the header at `headerStart`.
// Header: byte0 status (<=1), byte1 == 0x02, u16 count, then padding to +10, then the records.
// `limit` bounds the block (the paint-section terminator or end of data). Returns false if the
// header/records don't fit or don't look like a paint header.
bool readPaintMaterialsAt(const QByteArray &raw, int headerStart, int limit,
                          LiveryPaintState &state) {
    if (headerStart < 0 || headerStart + 10 > raw.size()) {
        return false;
    }
    if (static_cast<quint8>(raw[headerStart]) > 1
        || static_cast<quint8>(raw[headerStart + 1]) != 2) {
        return false;
    }
    const int count = detail::readLeU16(raw, headerStart + 2);
    if (count <= 0 || count > 256) {
        return false;
    }
    const int materialsEnd = headerStart + 10 + count * 27;
    if (materialsEnd > raw.size() || (limit >= 0 && materialsEnd > limit)) {
        return false;
    }
    state.materials.clear();
    state.materials.reserve(count);
    int pos = headerStart + 10;
    for (int i = 0; i < count; ++i, pos += 27) {
        LiveryPaintMaterial material;
        material.materialHash = readLeU64(raw, pos);
        material.primary.enabled = raw[pos + 9] != 0;
        for (int channel = 0; channel < 4; ++channel) {
            material.primary.bgra[channel] = static_cast<quint8>(raw[pos + 10 + channel]);
        }
        material.secondary.enabled = raw[pos + 14] != 0;
        for (int channel = 0; channel < 4; ++channel) {
            material.secondary.bgra[channel] = static_cast<quint8>(raw[pos + 15 + channel]);
        }
        material.manufacturerSelector = detail::readLeU32(raw, pos + 19);
        material.finish = detail::readLeU32(raw, pos + 23);
        state.materials.push_back(material);
    }
    return true;
}

// The paint materials follow the paint-section 'yrvl' tag; `blockStart` is that tag + 4 (where the
// header sits for every livery observed) and `end` is the section terminator. Parse forward from
// the header directly — the old approach scanned backward from `end` requiring an exact 102-byte
// block/trailer size, but some liveries carry a shorter trailer, which silently dropped the paint
// state (car rendered with no paint/material). The backward scan is kept only as a fallback.
LiveryPaintState readPaintState(const QByteArray &raw, int blockStart, int end) {
    LiveryPaintState state;
    if (readPaintMaterialsAt(raw, blockStart, end, state)) {
        return state;
    }
    const int firstCandidate = std::max(0, end - 4096);
    for (int start = end - 102; start >= firstCandidate; --start) {
        if (start + 4 > raw.size()
            || static_cast<quint8>(raw[start]) > 1
            || static_cast<quint8>(raw[start + 1]) != 2) {
            continue;
        }
        const int count = detail::readLeU16(raw, start + 2);
        if (count <= 0 || count > 256 || start + 102 + count * 27 != end) {
            continue;
        }
        if (readPaintMaterialsAt(raw, start, end, state)) {
            return state;
        }
    }
    return state;
}

LiveryPayload parseInflatedLiveryPayloadImpl(const QByteArray &raw) {
    LiveryPayload payload;
    payload.raw = raw;

    const int vlrc = payload.raw.indexOf(QByteArray("vlrc", 4));
    if (vlrc >= 0 && vlrc + 0x14 <= payload.raw.size()) {
        payload.carId = static_cast<int>(detail::readLeU32(payload.raw, vlrc + 0x10));
    }

    const int gyvl = payload.raw.indexOf(QByteArray("gyvl", 4));
    if (gyvl < 0) {
        throw std::runtime_error("C_livery has no embedded gyvl chunk");
    }
    payload.gyvlOffset = gyvl;
    payload.legacyRecords = payload.raw.size() >= 33
        && detail::readLeU32(payload.raw, 4) == 1
        && detail::readLeU32(payload.raw, 29) <= kEarlyLiveryVersion;

    const int bodyStart = gyvl + 0x15;
    const quint32 artworkSize = gyvl >= 4 ? detail::readLeU32(payload.raw, gyvl - 4) : 0;
    const qint64 artworkEnd = static_cast<qint64>(gyvl) + artworkSize;
    int bodyEnd = artworkSize >= 0x15 && artworkEnd <= payload.raw.size()
        ? static_cast<int>(artworkEnd) : payload.raw.indexOf(QByteArray("yrvl", 4), gyvl);
    if (bodyEnd < 0 || bodyEnd < bodyStart) {
        bodyEnd = payload.raw.size();
    }
    if (bodyStart >= payload.raw.size()) {
        throw std::runtime_error("C_livery gyvl body is truncated");
    }
    payload.body = payload.raw.mid(bodyStart, bodyEnd - bodyStart);

    const int statsTag = bodyEnd;
    const int paintTag = payload.raw.indexOf(QByteArray("yrvl", 4), statsTag + 4);
    const int paintEnd = paintTag >= 0
        ? payload.raw.indexOf(QByteArray("yrvl", 4), paintTag + 4)
        : -1;
    if (paintEnd >= 0) {
        payload.paint = readPaintState(payload.raw, paintTag + 4, paintEnd);
    }
    const bool hasStats = statsTag >= 0
        && payload.raw.mid(statsTag, 4) == QByteArray("yrvl", 4);
    const int statsCount = hasStats && paintTag > statsTag
        ? (paintTag - statsTag - 4) / 4
        : kCurrentLiverySectionCount;
    const int sectionCount = payload.legacyRecords ? kEarlyLiverySectionCount
        : statsCount == kLegacyLiveryStatsCount
        ? kLegacyLiverySectionCount
        : std::min(statsCount, kCurrentLiverySectionCount);
    payload.sectionCounts.reserve(sectionCount);
    for (int i = 0; i < sectionCount; ++i) {
        if (hasStats || payload.legacyRecords) {
            const int off = statsTag + (hasStats ? 4 : 0) + i * 4;
            payload.sectionCounts.push_back(off + 4 <= payload.raw.size()
                                                ? static_cast<int>(detail::readLeU32(payload.raw, off))
                                                : 0);
        } else {
            payload.sectionCounts.push_back(0);
        }
    }
    return payload;
}

} // namespace

LiveryPayload parseInflatedLiveryPayload(const QByteArray &raw) {
    return parseInflatedLiveryPayloadImpl(raw);
}

QVector<LiverySection> decodeLiverySections(const LiveryPayload &payload) {
    VinylDecoderOptions options;
    options.legacyLiveryRecords = payload.legacyRecords;
    options.appendLiveryTailPadding = payload.sectionCounts.size() == kEarlyLiverySectionCount;

    return VinylTreeDecoder(options).buildLiverySections(payload.body, payload.sectionCounts);
}

bool isLiveryAssetFileName(const QString &fileName) {
    if (fileName.compare(QStringLiteral("C_livery"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    return fileName.startsWith(QStringLiteral("Livery_"), Qt::CaseInsensitive)
        && fileName.endsWith(QStringLiteral(".C_livery"), Qt::CaseInsensitive);
}

LiveryPayload readLiveryPayload(const QString &folderOrFile) {
    QFile file(resolveLiveryPath(folderOrFile));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open C_livery: " + file.fileName()).toStdString());
    }

    return parseInflatedLiveryPayload(inflateContainer(file.readAll()));
}

namespace {

const unsigned char kGyvlHeader[0x15] = {
    0x67, 0x79, 0x76, 0x6c,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3f,
    0x00, 0x00, 0x00, 0x00,
    0x00,
};

// Source-less projects use canonical panel rotations.
const std::array<float, 11> kSlotRotation = {
    0.0f, 0.0f, 0.0f, 180.0f, 90.0f, -90.0f, 90.0f, 0.0f, 0.0f, 180.0f, 0.0f,
};

const std::array<float, 11> kEmptySlotRotation = {
    0.0f, 0.0f, 0.0f, 0.0f, 180.0f, 90.0f, -90.0f, 90.0f, 0.0f, 0.0f, 180.0f,
};

constexpr int kLiverySectionCount = 11;
constexpr int kLiveryEmptySlotBytes = detail::kVinylChildHeaderBytes + detail::kVinylBoundaryBytes;
constexpr int kMaxDirectChildren = 0xffff;

struct LiveryRecordFraming {
    bool nativeMarkers;
    bool unwindGroups;
};

struct LiveryEntry {
    enum Kind { Group, Shape, Logo } kind = Shape;
    const scene::Group *group = nullptr;
    const scene::Shape *shape = nullptr;
};

Matrix3 nodeMatrix(const VinylGroup &node) {
    constexpr double pi = 3.14159265358979323846;
    const double radians = node.rot * pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return affine(c * node.sx, -s * node.sy, node.px,
                  s * node.sx, c * node.sy, node.py);
}

Matrix3 shapeMatrixForShape(const VinylShape &shape) {
    FlattenedLayer layer;
    layer.rotation = shape.rotation;
    layer.posX = shape.posX;
    layer.posY = shape.posY;
    layer.scaleX = shape.scaleX;
    layer.scaleY = shape.scaleY;
    layer.skew = shape.skew;
    return shapeMatrix(layer);
}

struct SourceShapeView {
    quint16 shapeId = 0;
    bool raster = false;
    quint32 rasterId = 0;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool mask = false;
    int absPos = 0;
    QByteArray marker;
    Matrix3 worldMatrix;
    scene::Transform2D world;
};

struct SourceLivery {
    LiveryPayload payload;
    QVector<LiverySection> sections;
    QVector<detail::VinylSectionSpan> physicalSections;
    bool fullyMapped = false;
};

const detail::VinylSectionSpan *sourcePhysicalSection(const SourceLivery *source, int slot) {
    if (source == nullptr || slot < 0 || slot >= source->physicalSections.size()
        || source->physicalSections[slot].artwork.size() <= 0) {
        return nullptr;
    }

    return &source->physicalSections[slot];
}

struct SourceGroupFrame {
    Matrix3 world;
    int parentAbsPos = 0;
    double px = 0.0;
    double py = 0.0;
    double sx = 1.0;
    double sy = 1.0;
    double rot = 0.0;
};

void collectSourceGroupFrames(const VinylGroup &node, const Matrix3 &parentWorld,
                              int parentAbsPos, QHash<int, SourceGroupFrame> &frames) {
    const Matrix3 world = detail::multiply(parentWorld, nodeMatrix(node));
    if (node.absPos > 0 && node.expectedChildren) {
        frames.insert(node.absPos, {
            world,
            parentAbsPos,
            node.px,
            node.py,
            node.sx,
            node.sy,
            node.rot,
        });
    }
    for (const VinylItem &item : node.items) {
        if (!item.isShape()) {
            collectSourceGroupFrames(*std::get<VinylGroupPtr>(item.value), world,
                                     node.absPos, frames);
        }
    }
}

QHash<int, SourceGroupFrame> sourceGroupFrames(const SourceLivery *source) {
    QHash<int, SourceGroupFrame> frames;
    if (source == nullptr) {
        return frames;
    }
    for (const LiverySection &section : source->sections) {
        collectSourceGroupFrames(section.subtree, Matrix3{}, 0, frames);
    }
    return frames;
}

std::optional<SourceLivery> sourceLivery(const Project &project) {
    if (project.liverySource.isEmpty()) {
        return std::nullopt;
    }
    try {
        SourceLivery source;
        source.payload = parseInflatedLiveryPayload(project.liverySource);
        if (source.payload.legacyRecords) {
            return std::nullopt;
        }
        source.sections = decodeLiverySections(source.payload);
        source.physicalSections = detail::mapVinylSections(
            detail::readVinylRecordStream(source.payload.body, false),
            source.payload.sectionCounts, kLiverySectionCount);
        source.fullyMapped = source.physicalSections.size() == kLiverySectionCount;
        if (!source.fullyMapped) {
            source.physicalSections.resize(kLiverySectionCount);
            for (const auto &section : source.sections) {
                if (!section.populated) {
                    continue;
                }
                const auto block = detail::readVinylChildBlock(source.payload.body, section.absPos, false);
                if (!block) {
                    continue;
                }
                auto &physical = source.physicalSections[section.slot];
                physical.artwork = block->span;
                physical.span = block->span;
                physical.shapeCount = block->shapeCount;
                const int boundary = block->span.end + 1;
                if (detail::readVinylBoundary(source.payload.body, boundary, source.payload.body.size())) {
                    physical.boundary = detail::VinylSpan{boundary, boundary + detail::kVinylBoundaryBytes};
                    physical.span.end = physical.boundary->end;
                }
            }
        }
        return source.sections.size() == kLiverySectionCount ? std::optional<SourceLivery>(source) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

const LiverySection *sourceSection(const SourceLivery *source, int slot) {
    if (source == nullptr) {
        return nullptr;
    }
    for (const LiverySection &section : source->sections) {
        if (section.slot == slot) {
            return &section;
        }
    }
    return nullptr;
}

void collectSourceShapes(const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask,
                         QVector<SourceShapeView> &out) {
    const Matrix3 groupMatrix = detail::multiply(parentMatrix, nodeMatrix(node));
    const bool inheritedMask = parentMask || node.isMask;
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            const VinylShape &shape = std::get<VinylShape>(item.value);
            SourceShapeView view;
            view.shapeId = shape.shapeId;
            view.raster = shape.isLogo;
            view.rasterId = shape.rasterId;
            view.color = shape.color;
            view.absPos = shape.absPos;
            view.marker = shape.marker;
            view.mask = inheritedMask || (shape.isMask && !hasColorData(shape.color));
            view.worldMatrix = detail::multiply(groupMatrix, shapeMatrixForShape(shape));
            view.world = decomposeTransform2D(view.worldMatrix);
            out.push_back(view);
        } else {
            collectSourceShapes(*std::get<VinylGroupPtr>(item.value), groupMatrix, inheritedMask, out);
        }
    }
}

QVector<SourceShapeView> sourceSectionShapes(const LiverySection &section) {
    QVector<SourceShapeView> out;
    collectSourceShapes(section.subtree, Matrix3{}, section.subtree.isMask, out);
    return out;
}

bool closeEnough(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

bool matrixClose(const Matrix3 &a, const Matrix3 &b) {
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            const double magnitude = std::max(std::abs(a.m[row][col]), std::abs(b.m[row][col]));
            const double tolerance = col == 2 ? 0.5 : 0.01 + 0.001 * magnitude;
            if (!closeEnough(a.m[row][col], b.m[row][col], tolerance)) {
                return false;
            }
        }
    }
    return true;
}

bool rotationClose(double a, double b) {
    double diff = std::fmod(std::abs(normalizeRotation(a) - normalizeRotation(b)), 360.0);
    if (diff > 180.0) {
        diff = 360.0 - diff;
    }
    return diff <= 0.5;
}

bool transformClose(const scene::Transform2D &a, const scene::Transform2D &b) {
    return closeEnough(a.x, b.x, 0.5)
        && closeEnough(a.y, b.y, 0.5)
        && closeEnough(std::abs(a.scaleX), std::abs(b.scaleX), 0.01 + 0.001 * std::abs(b.scaleX))
        && closeEnough(std::abs(a.scaleY), std::abs(b.scaleY), 0.01 + 0.001 * std::abs(b.scaleY))
        && closeEnough(a.skew, b.skew, 0.01)
        && rotationClose(a.rotation, b.rotation);
}

bool reliableTransform(const scene::Transform2D &t) {
    return std::isfinite(t.x) && std::isfinite(t.y)
        && std::isfinite(t.scaleX) && std::isfinite(t.scaleY)
        && std::isfinite(t.rotation) && std::isfinite(t.skew)
        && std::abs(t.scaleX) < 1.0e8
        && std::abs(t.scaleY) < 1.0e8;
}

bool matchesSourceShape(const scene::Shape &shape, const SourceShapeView &source) {
    const scene::Transform2D world = decomposeTransform2D(shape.worldMatrix());
    const bool transformMatches = shape.hasSourceTransform
        ? (!reliableTransform(shape.sourceTransform) || transformClose(world, shape.sourceTransform))
        : (source.color[3] == 0
           || matrixClose(shape.worldMatrix(), source.worldMatrix)
           || transformClose(world, source.world));
    // Raster and vector identities use separate ID domains.
    const bool idMatches = shape.raster ? true : shape.shapeId == source.shapeId;
    return idMatches
        && shape.raster == source.raster
        && shape.rasterId == source.rasterId
        && shape.color == source.color
        && shape.mask == source.mask
        && transformMatches;
}

QByteArray sourceSlotBytes(const SourceLivery *source, int slot);

bool decodedSectionHasArtworkGroup(const LiverySection *section) {
    if (section == nullptr) {
        return false;
    }
    for (const VinylItem &item : section->subtree.items) {
        if (item.isShape()) {
            continue;
        }
        const VinylGroup &root = *std::get<VinylGroupPtr>(item.value);
        for (const VinylItem &rootItem : root.items) {
            if (!rootItem.isShape()) {
                return true;
            }
        }
    }
    return false;
}

bool decodedGroupHasSkippedChildren(const VinylGroup &group) {
    if (group.skippedChildren > 0) {
        return true;
    }
    for (const VinylItem &item : group.items) {
        if (!item.isShape()
            && decodedGroupHasSkippedChildren(*std::get<VinylGroupPtr>(item.value))) {
            return true;
        }
    }
    return false;
}

bool sectionMatchesSource(const QVector<const scene::Shape *> &shapes, const LiverySection *section,
                          bool hasGroupedArtwork, int sourceCount, bool physicalFrame = false) {
    if (section == nullptr || (!physicalFrame && decodedGroupHasSkippedChildren(section->subtree))
        || hasGroupedArtwork != decodedSectionHasArtworkGroup(section)) {
        return false;
    }
    const bool hasRasterArtwork = std::any_of(
        shapes.cbegin(), shapes.cend(), [](const scene::Shape *shape) {
            return shape != nullptr && shape->raster;
        });
    if (!physicalFrame && !hasRasterArtwork && sourceCount != shapes.size()) {
        return false;
    }
    const QVector<SourceShapeView> sourceShapes = sourceSectionShapes(*section);
    if (shapes.size() != sourceShapes.size()) {
        return false;
    }
    for (int i = 0; i < shapes.size(); ++i) {
        const scene::Shape *shape = shapes[i];
        if (shape == nullptr) {
            return false;
        }
        if (!matchesSourceShape(*shape, sourceShapes[i])) {
            return false;
        }
    }
    return true;
}

QByteArray sourceSlotBytes(const SourceLivery *source, int slot) {
    const LiverySection *section = sourceSection(source, slot);
    if (source == nullptr || section == nullptr || section->absPos < 0
        || section->absPos > source->payload.body.size()) {
        return {};
    }
    int end = source->payload.body.size();
    for (int nextSlot = slot + 1; nextSlot < kLiverySectionCount; ++nextSlot) {
        if (const LiverySection *next = sourceSection(source, nextSlot)) {
            if (next->absPos > section->absPos) {
                end = next->absPos;
                break;
            }
        }
    }
    if (end < section->absPos) {
        return {};
    }
    QByteArray bytes = source->payload.body.mid(section->absPos, end - section->absPos);
    return bytes;
}

int sourcePopulatedSlotSize(const LiverySection *section) {
    if (section == nullptr) {
        return 0;
    }
    const QVector<SourceShapeView> shapes = sourceSectionShapes(*section);
    if (shapes.isEmpty()) {
        return 0;
    }
    const SourceShapeView &terminal = shapes.back();
    const int end = terminal.absPos + terminal.marker.size() + 31;
    return std::max(0, end - section->absPos);
}

void collectMissingShapeStates(const VinylGroup &group,
                               QVector<QPair<int, char>> &states) {
    bool previousMask = false;
    for (int i = 0; i < group.items.size(); ++i) {
        const VinylItem &item = group.items[i];
        if (item.isShape()) {
            const VinylShape &shape = std::get<VinylShape>(item.value);
            if (i > 0 && shape.marker == QByteArray("\x02", 1)) {
                states.push_back({shape.absPos, previousMask ? '\x01' : '\0'});
            }
            previousMask = shape.isMask;
        } else {
            const auto &child = *std::get<VinylGroupPtr>(item.value);
            collectMissingShapeStates(child, states);
            QVector<SourceShapeView> leaves;
            collectSourceShapes(child, Matrix3{}, child.isMask, leaves);
            previousMask = !leaves.isEmpty() && leaves.back().mask;
        }
    }
}

void repairSourceShapeStates(QByteArray &bytes, const LiverySection &section) {
    QVector<QPair<int, char>> states;
    collectMissingShapeStates(section.subtree, states);
    // Work backwards so all offsets still refer to the original source.
    std::sort(states.begin(), states.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });
    for (const auto &state : states) {
        const int offset = state.first - section.absPos;
        if (offset >= 0 && offset < bytes.size()) {
            bytes.insert(offset, state.second);
        }
    }
}

QByteArray defaultSectionBoundary(int slot) {
    detail::VinylBoundary boundary;
    boundary.orientation = kSlotRotation[slot];

    return detail::writeVinylBoundary(boundary);
}

QByteArray sourceSectionBoundary(const SourceLivery *source, int slot) {
    if (const auto *section = sourcePhysicalSection(source, slot)) {
        if (section->boundary) {
            return source->payload.body.mid(
                section->boundary->start, section->boundary->size());
        }
        return defaultSectionBoundary(slot);
    }
    const QByteArray slotBytes = sourceSlotBytes(source, slot);
    const int populatedSize = sourcePopulatedSlotSize(sourceSection(source, slot));
    if (populatedSize > 0 && detail::readVinylBoundary(slotBytes, populatedSize, slotBytes.size())) {
        return slotBytes.mid(populatedSize, detail::kVinylBoundaryBytes);
    }
    return defaultSectionBoundary(slot);
}

QByteArray defaultEmptySlot(int slot) {
    detail::VinylBoundary boundary;
    boundary.orientation = kEmptySlotRotation[slot];

    return detail::writeVinylBoundary(boundary) + detail::writeVinylChildHeader(0, {});
}

bool isEmptySlotRecord(const QByteArray &bytes) {
    if (bytes.size() < kLiveryEmptySlotBytes) {
        return false;
    }
    const QByteArray empty = detail::writeVinylChildHeader(0, {});
    const bool trailingFrame = detail::readVinylBoundary(bytes, 0, bytes.size())
        && bytes.mid(detail::kVinylBoundaryBytes, detail::kVinylChildHeaderBytes) == empty;
    const bool leadingFrame = bytes.startsWith(empty)
        && detail::readVinylBoundary(bytes, detail::kVinylChildHeaderBytes, bytes.size());
    return trailingFrame || leadingFrame;
}

QByteArray sourceEmptySlot(const SourceLivery *source, int slot) {
    const QByteArray slotBytes = sourceSlotBytes(source, slot);
    if (isEmptySlotRecord(slotBytes)) {
        return slotBytes.left(23);
    }
    return defaultEmptySlot(slot);
}

quint16 liveryLogoId(const scene::Shape &logo) {
    constexpr quint32 kLogoIdMask = 0x7fffu;
    constexpr quint16 kLogoIdFlag = 0x8000u;
    if (!logo.raster || logo.rasterId == 0 || logo.rasterId > kLogoIdMask) {
        throw std::runtime_error("livery logo has an invalid raster identifier");
    }

    return static_cast<quint16>(kLogoIdFlag | logo.rasterId);
}

bool appendLiveryArtworkRecord(QByteArray &out, const LiveryEntry &entry, double offsetX,
                               double offsetY, quint8 lead, bool bare,
                               const Matrix3 &worldAdjustment) {
    if (entry.shape == nullptr) {
        throw std::runtime_error("livery artwork entry has no layer");
    }
    auto shape = detail::vinylShapePlacement(*entry.shape, worldAdjustment);
    if (entry.kind == LiveryEntry::Logo) {
        shape.shapeId = liveryLogoId(*entry.shape);
    } else if (entry.kind != LiveryEntry::Shape) {
        throw std::runtime_error("livery group reached the artwork encoder");
    } else if (entry.shape->raster) {
        throw std::runtime_error("livery logo reached the vector shape encoder");
    }
    detail::appendVinylShapeRecord(out, shape, offsetX, offsetY, 0x02,
        bare ? std::nullopt : std::optional<quint8>{lead});

    return shape.mask;
}

bool hasSourceArtworkMarker(const scene::Shape &shape) {
    return shape.marker == QByteArray("\x02", 1)
        || (shape.marker.size() == 2
            && (static_cast<quint8>(shape.marker[0]) == 0x00
                || static_cast<quint8>(shape.marker[0]) == 0x01)
            && static_cast<quint8>(shape.marker[1]) == 0x02);
}

bool liveryArtworkBare(const scene::Shape &shape, bool fallback) {
    // A bare lead belongs to the first child, not to the shape's identity.
    // Imported first children can move behind another shape or group.
    return fallback && (!hasSourceArtworkMarker(shape) || shape.marker.size() == 1);
}

quint8 liveryArtworkLead(const scene::Shape &shape, quint8 fallback,
                         bool previousShapeMask) {
    quint8 lead = fallback;
    if (hasSourceArtworkMarker(shape) && shape.marker.size() == 2) {
        lead = static_cast<quint8>(shape.marker[0]);
    }
    return previousShapeMask ? 0x01 : lead;
}

int checkLiveryChildCount(int count, const char *label) {
    if (count <= 0) {
        throw std::runtime_error(std::string(label) + " has no visible children");
    }
    if (count > kMaxDirectChildren) {
        throw std::runtime_error(std::string(label) + " has too many direct children for the child bitmap");
    }
    const int blocks = (count + 7) / 8;
    return blocks;
}

void collectVisibleShapeLeaves(const scene::Layer &node, QVector<const scene::Shape *> &out) {
    if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(static_cast<const scene::Shape *>(&node));
        return;
    }
    if (!node.visible) {
        return;
    }
    if (node.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectVisibleShapeLeaves(*child, out);
        }
    }
}

void entryShapes(const LiveryEntry &entry, QVector<const scene::Shape *> &out) {
    if (entry.kind != LiveryEntry::Group) {
        if (entry.shape != nullptr) {
            out.push_back(entry.shape);
        }
        return;
    }
    if (entry.group != nullptr) {
        collectVisibleShapeLeaves(*entry.group, out);
    }
}

bool hasVisibleShapeLeaves(const scene::Layer &node) {
    QVector<const scene::Shape *> leaves;
    collectVisibleShapeLeaves(node, leaves);
    return !leaves.isEmpty();
}

QVector<LiveryEntry> directVisibleChildren(const scene::Group &group) {
    QVector<LiveryEntry> entries;
    for (const auto &child : group.children) {
        if (child->kind() == scene::LayerKind::Shape) {
            const auto *shape = static_cast<const scene::Shape *>(child.get());
            const LiveryEntry::Kind kind = shape->raster ? LiveryEntry::Logo : LiveryEntry::Shape;
            entries.push_back({kind, nullptr, shape});
        } else if (child->kind() == scene::LayerKind::Group
                   && child->visible && hasVisibleShapeLeaves(*child)) {
            const auto *childGroup = static_cast<const scene::Group *>(child.get());
            const QVector<LiveryEntry> normalizedChildren = directVisibleChildren(*childGroup);
            if (normalizedChildren.size() == 1) {
                entries.push_back(normalizedChildren.front());
            } else {
                entries.push_back({LiveryEntry::Group, childGroup, nullptr});
            }
        }
    }
    return entries;
}

bool hasGroupedArtwork(const scene::Group *sectionGroup) {
    if (sectionGroup == nullptr) {
        return false;
    }
    const QVector<LiveryEntry> children = directVisibleChildren(*sectionGroup);
    return std::any_of(children.cbegin(), children.cend(), [](const LiveryEntry &entry) {
        return entry.kind == LiveryEntry::Group;
    });
}

QPointF shapeHalfExtents(const scene::Shape &shape) {
    if (shape.raster) {
        double w = std::max(1, shape.rasterWidth);
        double h = std::max(1, shape.rasterHeight);
        const double side = std::max(w, h);
        return QPointF(64.0 * w / side, 64.0 * h / side);
    }
    return QPointF(64.0, 64.0);
}

QVector<QPointF> shapeCorners(const scene::Shape &shape, const Matrix3 &worldAdjustment) {
    const detail::VinylShapePlacement exported = detail::vinylShapePlacement(shape, worldAdjustment);
    const QPointF he = shapeHalfExtents(shape);
    FlattenedLayer layer;
    layer.rotation = exported.rotation;
    layer.posX = exported.x;
    layer.posY = exported.y;
    layer.scaleX = exported.scaleX;
    layer.scaleY = exported.scaleY;
    layer.skew = exported.skew;
    const Matrix3 matrix = shapeMatrix(layer);

    QVector<QPointF> corners;
    const double pts[4][2] = {{-he.x(), -he.y()}, {he.x(), -he.y()}, {he.x(), he.y()}, {-he.x(), he.y()}};
    for (const auto &pt : pts) {
        corners.push_back(QPointF(matrix.m[0][0] * pt[0] + matrix.m[0][1] * pt[1] + matrix.m[0][2],
                                  matrix.m[1][0] * pt[0] + matrix.m[1][1] * pt[1] + matrix.m[1][2]));
    }
    return corners;
}

QPointF shapesOrigin(const QVector<const scene::Shape *> &leaves, const Matrix3 &worldAdjustment) {
    if (leaves.isEmpty()) {
        return QPointF(0.0, 0.0);
    }
    QVector<QPointF> points;
    for (const scene::Shape *shape : leaves) {
        if (shape != nullptr) {
            points.append(shapeCorners(*shape, worldAdjustment));
        }
    }
    if (points.isEmpty()) {
        const detail::VinylShapePlacement first = detail::vinylShapePlacement(*leaves.front(), worldAdjustment);
        return QPointF(first.x, first.y);
    }

    double minX = points.front().x();
    double maxX = minX;
    double minY = points.front().y();
    double maxY = minY;
    for (const QPointF &point : points) {
        minX = std::min(minX, point.x());
        maxX = std::max(maxX, point.x());
        minY = std::min(minY, point.y());
        maxY = std::max(maxY, point.y());
    }
    return QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
}

QPointF entryOrigin(const LiveryEntry &entry, const Matrix3 &worldAdjustment) {
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    return shapesOrigin(leaves, worldAdjustment);
}

bool entryAllMasked(const LiveryEntry &entry) {
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    if (leaves.isEmpty()) {
        return false;
    }
    for (const scene::Shape *shape : leaves) {
        if (shape == nullptr || !shape->mask) {
            return false;
        }
    }
    return true;
}

QByteArray childBitmap(const QVector<LiveryEntry> &items, const char *label) {
    const int blocks = checkLiveryChildCount(items.size(), label);
    QByteArray bitmap(blocks, '\0');
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].kind == LiveryEntry::Group) {
            bitmap[i / 8] = static_cast<char>(static_cast<quint8>(bitmap[i / 8]) | (1 << (i % 8)));
        }
    }
    return bitmap;
}

QByteArray packMarkerlessGroupHeader(const QVector<LiveryEntry> &children, const char *label) {
    const QByteArray bitmap = childBitmap(children, label);

    return detail::writeVinylChildHeader(children.size(), bitmap);
}

QByteArray packCountedGroupHeader(const QVector<LiveryEntry> &children, bool mask, const char *label) {
    const QByteArray bitmap = childBitmap(children, label);

    return detail::writeVinylChildHeader(children.size(), bitmap, mask ? 0x60 : 0x20);
}

QByteArray liveryGroupTransformMarker(QByteArray marker, bool previousShapeMask,
                                      bool previousWasGroup, bool nativeMarkers) {
    if (nativeMarkers && !marker.isEmpty()) {
        return QByteArray(1, previousShapeMask && !previousWasGroup ? '\x01' : '\0');
    }
    if (previousShapeMask && !marker.isEmpty() && static_cast<quint8>(marker[0]) == 0x00) {
        marker[0] = '\x01';
    }

    return marker;
}

QByteArray liveryTransformMarker(QByteArray standaloneMarker) {
    if (standaloneMarker.isEmpty()) {
        return standaloneMarker;
    }
    const quint8 first = static_cast<quint8>(standaloneMarker[0]);
    if (first == 0x00 || first == 0x01) {
        return standaloneMarker.left(1);
    }
    if (static_cast<quint8>(standaloneMarker.back()) == 0x03) {
        standaloneMarker[standaloneMarker.size() - 1] = '\x01';
    }
    return standaloneMarker;
}

QByteArray packLiveryGroupTransform(const scene::Transform2D &transform, const QByteArray &marker) {
    QByteArray out = marker;
    detail::appendLeFloat(out, static_cast<float>(transform.x));
    detail::appendLeFloat(out, static_cast<float>(transform.y));
    detail::appendLeFloat(out, static_cast<float>(transform.scaleX));
    detail::appendLeFloat(out, static_cast<float>(normalizeRotation(transform.rotation)));
    if (std::abs(transform.scaleY - transform.scaleX) > 1e-5) {
        detail::VinylAttributes attributes;
        attributes.flags = 0x30;
        attributes.scaleY = transform.scaleY;
        out.append(detail::writeVinylAttributes(attributes));
    }
    return out;
}

bool groupStructureMatchesSource(const scene::Group &group) {
    QVector<QString> children;
    for (const auto &child : group.children) {
        if (child->kind() == scene::LayerKind::Guide) {
            continue;
        }
        children.append(child->id);
        if (child->kind() == scene::LayerKind::Group
            && !groupStructureMatchesSource(static_cast<const scene::Group &>(*child))) {
            return false;
        }
    }

    return children == group.sourceChildren;
}

bool nativeArtworkMatchesSource(
    const SourceLivery &source,
    const std::array<const scene::Group *, kLiverySectionCount> &groups,
    const std::array<QVector<const scene::Shape *>, kLiverySectionCount> &shapes,
    int sectionCount) {
    const bool motorsportSource = source.payload.gyvlOffset >= 66;
    if (motorsportSource != (sectionCount == kFM2023SectionCount)
        || source.payload.sectionCounts.size() != sectionCount) {
        return false;
    }
    for (int slot = 0; slot < sectionCount; ++slot) {
        const LiverySection *section = sourceSection(&source, slot);
        if (section == nullptr || decodedGroupHasSkippedChildren(section->subtree)) {
            return false;
        }
        const QVector<SourceShapeView> original = sourceSectionShapes(*section);
        if (original.size() != shapes[slot].size()
            || (groups[slot] != nullptr && !groupStructureMatchesSource(*groups[slot]))) {
            return false;
        }
        for (int index = 0; index < original.size(); ++index) {
            SourceShapeView shape = original[index];
            shape.mask = shape.mask && !hasColorData(shape.color);
            const scene::Shape &current = *shapes[slot][index];
            if (current.shapeId != shape.shapeId || current.raster != shape.raster
                || current.rasterId != shape.rasterId || current.mask != shape.mask
                || current.absOffset != shape.absPos) {
                return false;
            }
        }
    }

    return true;
}

QByteArray patchNativeArtwork(
    const Project &project, const SourceLivery &source,
    const std::array<QVector<const scene::Shape *>, kLiverySectionCount> &shapes,
    int sectionCount) {
    const int offset = source.payload.gyvlOffset;
    QByteArray artwork = project.liverySource.mid(offset, detail::readLeU32(project.liverySource, offset - 4));
    for (int slot = 0; slot < sectionCount; ++slot) {
        const QVector<SourceShapeView> original = sourceSectionShapes(*sourceSection(&source, slot));
        const Matrix3 canvasInverse = invertAffine(liverySectionCanvasTransform(slot));
        for (int index = 0; index < original.size(); ++index) {
            const SourceShapeView &sourceShape = original[index];
            const scene::Shape &current = *shapes[slot][index];
            const Matrix3 currentWorld = current.worldMatrix();
            const Matrix3 cachedWorld = current.sourceTransform.matrix();
            bool unchanged = current.hasSourceTransform && current.color == sourceShape.color;
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    unchanged = unchanged && currentWorld.m[row][column] == cachedWorld.m[row][column];
                }
            }
            if (unchanged) {
                continue;
            }
            const int transformOffset = 21 + sourceShape.absPos + sourceShape.marker.size() + 2;
            FlattenedLayer placement;
            placement.rotation = detail::readLeFloat(artwork, transformOffset);
            placement.posX = detail::readLeFloat(artwork, transformOffset + 4);
            placement.posY = detail::readLeFloat(artwork, transformOffset + 8);
            placement.scaleX = detail::readLeFloat(artwork, transformOffset + 12);
            placement.scaleY = detail::readLeFloat(artwork, transformOffset + 16);
            placement.skew = detail::readLeFloat(artwork, transformOffset + 20);
            const Matrix3 parentWorld = detail::multiply(sourceShape.worldMatrix, invertAffine(shapeMatrix(placement)));
            const scene::Transform2D local = decomposeTransform2D(detail::multiply(
                invertAffine(parentWorld), detail::multiply(canvasInverse, currentWorld)));
            QByteArray record;
            detail::appendLeFloat(record, static_cast<float>(local.rotation));
            detail::appendLeFloat(record, static_cast<float>(local.x));
            detail::appendLeFloat(record, static_cast<float>(local.y));
            detail::appendLeFloat(record, static_cast<float>(local.scaleX));
            detail::appendLeFloat(record, static_cast<float>(local.scaleY));
            detail::appendLeFloat(record, static_cast<float>(local.skew));
            for (const quint8 channel : current.color) {
                record.append(static_cast<char>(channel));
            }
            artwork.replace(transformOffset, record.size(), record);
        }
    }

    return artwork;
}

std::optional<SourceLivery> legacySourceFrames(const Project &project) {
    if (project.liverySource.isEmpty()) {
        return std::nullopt;
    }
    SourceLivery source;
    source.payload = parseInflatedLiveryPayload(project.liverySource);
    if (detail::readLeU32(project.liverySource, 4) == 2
        && source.payload.gyvlOffset >= 66) {
        FM2023LiveryPayload payload;
        payload.gyvlBody = source.payload.body;
        const int stats = source.payload.gyvlOffset
            + static_cast<int>(detail::readLeU32(project.liverySource, source.payload.gyvlOffset - 4));
        for (int slot = 0; slot < kFM2023SectionCount; ++slot) {
            payload.sectionCounts.append(static_cast<int>(detail::readLeU32(project.liverySource, stats + 4 + slot * 4)));
        }
        source.payload.sectionCounts = payload.sectionCounts;
        source.sections = decodeFM2023LiverySections(payload);
    } else {
        source.sections = decodeLiverySections(source.payload);
    }

    return source;
}

QVector<LiveryEntry> childrenForEntry(const LiveryEntry &entry) {
    if (entry.kind != LiveryEntry::Group || entry.group == nullptr) {
        throw std::runtime_error("livery artwork entries cannot have children");
    }
    return directVisibleChildren(*entry.group);
}

int terminalDepth(const LiveryEntry &entry) {
    if (entry.kind != LiveryEntry::Group) {
        return 1;
    }
    const QVector<LiveryEntry> children = childrenForEntry(entry);
    if (children.isEmpty() || children.back().kind != LiveryEntry::Group) {
        return 1;
    }
    return 1 + terminalDepth(children.back());
}

QByteArray packLiveryGroup(const LiveryEntry &entry, const Matrix3 &parentWorld,
                           int parentSourceAbsPos, bool parentUsesSourceFrame,
                           const QByteArray &transformMarker, bool parentMask,
                           bool &previousShapeMask, const Matrix3 &worldAdjustment,
                           const QHash<int, SourceGroupFrame> &sourceFrames,
                           const LiveryRecordFraming &framing) {
    const QVector<LiveryEntry> children = childrenForEntry(entry);
    if (children.isEmpty()) {
        throw std::runtime_error("livery group has no visible children");
    }
    const bool isMaskGroup = entryAllMasked(entry);
    const bool childMask = parentMask || isMaskGroup;
    const QPointF origin = entryOrigin(entry, worldAdjustment);
    const auto sourceFrameIt = entry.group != nullptr
        ? sourceFrames.constFind(entry.group->sourceAbsPos)
        : sourceFrames.cend();
    const bool usesSourceFrame = sourceFrameIt != sourceFrames.cend();
    const Matrix3 groupWorld = usesSourceFrame
        ? sourceFrameIt->world
        : affine(1.0, 0.0, origin.x(), 0.0, 1.0, origin.y());
    scene::Transform2D localTransform;
    if (usesSourceFrame && parentUsesSourceFrame
        && sourceFrameIt->parentAbsPos == parentSourceAbsPos) {
        localTransform.x = sourceFrameIt->px;
        localTransform.y = sourceFrameIt->py;
        localTransform.scaleX = sourceFrameIt->sx;
        localTransform.scaleY = sourceFrameIt->sy;
        localTransform.rotation = sourceFrameIt->rot;
    } else {
        localTransform = decomposeTransform2D(
            detail::multiply(invertAffine(parentWorld), groupWorld));
    }
    QByteArray out = packLiveryGroupTransform(localTransform, liveryTransformMarker(transformMarker));
    out.append(localTransform.scaleX * localTransform.scaleY < 0.0
                   ? packMarkerlessGroupHeader(children, "livery group")
                   : packCountedGroupHeader(children, isMaskGroup, "livery group"));
    const Matrix3 localArtworkAdjustment = detail::multiply(
        invertAffine(groupWorld), worldAdjustment);

    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    bool hasPreviousSibling = false;
    for (const LiveryEntry &child : children) {
        if (child.kind == LiveryEntry::Group) {
            QByteArray marker;
            if (previousWasGroup) {
                marker = detail::vinylSiblingGroupMarker(previousGroupDepth);
            } else if (hasPreviousSibling) {
                marker = QByteArray("\x00\x03", 2);
            } else {
                marker = QByteArray("\x01", 1);
            }
            marker = liveryGroupTransformMarker(marker, previousShapeMask,
                                                previousWasGroup, framing.nativeMarkers);
            previousShapeMask = false;
            out.append(packLiveryGroup(
                child, groupWorld,
                entry.group != nullptr ? entry.group->sourceAbsPos : 0,
                usesSourceFrame, marker, childMask, previousShapeMask,
                worldAdjustment, sourceFrames, framing));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(child);
        } else if (child.shape != nullptr) {
            if (previousWasGroup && framing.unwindGroups) {
                detail::appendVinylGroupShapeTransition(out, previousGroupDepth);
            }
            const quint8 fallbackLead = (!hasPreviousSibling || childMask || previousShapeMask)
                ? 0x01
                : 0x00;
            const quint8 lead = framing.nativeMarkers ? fallbackLead : liveryArtworkLead(
                *child.shape, fallbackLead, previousShapeMask);
            const bool bare = false;
            const bool artworkMask = appendLiveryArtworkRecord(
                out, child, 0.0, 0.0, lead, bare, localArtworkAdjustment);
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = childMask || artworkMask;
        }
        hasPreviousSibling = true;
    }
    return out;
}

void appendStructuralSection(QByteArray &body, const scene::Group *sectionGroup,
                              int slot,
                              const QHash<int, SourceGroupFrame> &sourceFrames,
                              const LiveryRecordFraming &framing) {
    if (sectionGroup == nullptr) {
        return;
    }
    const QVector<LiveryEntry> children = directVisibleChildren(*sectionGroup);
    if (children.isEmpty()) {
        return;
    }
    QVector<const scene::Shape *> sectionShapes;
    for (const LiveryEntry &entry : children) {
        entryShapes(entry, sectionShapes);
    }
    const bool terminalShapeMask = !sectionShapes.isEmpty() && sectionShapes.back()->mask;

    body.append(packMarkerlessGroupHeader(children, "livery section"));
    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    bool previousShapeMask = false;
    bool hasPreviousSibling = false;
    const Matrix3 sectionWorld;
    const Matrix3 worldAdjustment = invertAffine(liverySectionCanvasTransform(slot));
    for (const LiveryEntry &child : children) {
        if (child.kind == LiveryEntry::Group) {
            QByteArray marker;
            if (previousWasGroup) {
                marker = detail::vinylSiblingGroupMarker(previousGroupDepth);
            } else if (hasPreviousSibling) {
                marker = QByteArray("\x00\x03", 2);
            } else {
                marker = QByteArray();
            }
            marker = liveryGroupTransformMarker(marker, previousShapeMask,
                                                previousWasGroup, framing.nativeMarkers);
            previousShapeMask = false;
            body.append(packLiveryGroup(child, sectionWorld, 0, false, marker, false,
                                        previousShapeMask, worldAdjustment, sourceFrames, framing));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(child);
        } else if (child.shape != nullptr) {
            if (previousWasGroup && framing.unwindGroups) {
                detail::appendVinylGroupShapeTransition(body, previousGroupDepth);
            }
            const quint8 fallbackLead = previousShapeMask ? 0x01 : 0x00;
            const quint8 lead = framing.nativeMarkers ? fallbackLead : liveryArtworkLead(
                *child.shape, fallbackLead, previousShapeMask);
            const bool bare = liveryArtworkBare(*child.shape, !hasPreviousSibling);
            const bool artworkMask = appendLiveryArtworkRecord(
                body, child, 0.0, 0.0, lead, bare, worldAdjustment);
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = artworkMask;
        }
        hasPreviousSibling = true;
    }
    body.append(terminalShapeMask ? '\x01' : '\x00');
}

} // namespace

static QByteArray buildLiveryGyvlImpl(const Project &project, int sectionCount,
                                     bool preserveSource,
                                     std::array<int, kLiverySectionCount> *outSectionCounts) {
    std::array<const scene::Group *, kLiverySectionCount> slotGroups{};
    std::array<QVector<const scene::Shape *>, kLiverySectionCount> slotShapes;
    if (project.root) {
        for (const auto &child : project.root->children) {
            if (child->kind() != scene::LayerKind::Group) {
                continue;
            }
            const auto &group = static_cast<const scene::Group &>(*child);
            const int slot = group.liverySectionSlot;
            if (group.isLiverySection && slot >= 0 && slot < kLiverySectionCount) {
                slotGroups[slot] = &group;
                collectVisibleShapeLeaves(group, slotShapes[slot]);
            }
        }
    }

    if constexpr (kEnforceLiveryShapeLimits) {
        for (int slot = 0; slot < kLiverySectionCount; ++slot) {
            const int shapeCount = static_cast<int>(slotShapes[slot].size());
            const int shapeLimit = liverySectionShapeLimit(slot);
            if (shapeCount <= shapeLimit) {
                continue;
            }
            const QString sectionName = slotGroups[slot] != nullptr && !slotGroups[slot]->name.isEmpty()
                ? slotGroups[slot]->name
                : QStringLiteral("slot %1").arg(slot);
            throw std::runtime_error(
                QStringLiteral("livery section \"%1\" has %2 shapes; export limit is %3")
                    .arg(sectionName)
                    .arg(shapeCount)
                    .arg(shapeLimit)
                    .toStdString());
        }
    }

    for (int slot = sectionCount; slot < kLiverySectionCount; ++slot) {
        if (!slotShapes[slot].isEmpty()) {
            throw std::runtime_error(
                QStringLiteral("The selected game cannot represent artwork in section %1.")
                    .arg(slotGroups[slot]->name).toStdString());
        }
    }

    const std::optional<SourceLivery> source = preserveSource ? sourceLivery(project) : std::nullopt;
    const std::optional<SourceLivery> nativeFrames = preserveSource ? std::nullopt : legacySourceFrames(project);
    if (nativeFrames && nativeArtworkMatchesSource(*nativeFrames, slotGroups, slotShapes, sectionCount)) {
        if (outSectionCounts != nullptr) {
            outSectionCounts->fill(0);
            std::copy(nativeFrames->payload.sectionCounts.cbegin(), nativeFrames->payload.sectionCounts.cend(),
                      outSectionCounts->begin());
        }
        return patchNativeArtwork(project, *nativeFrames, slotShapes, sectionCount);
    }
    const SourceLivery *sourcePtr = source ? &*source : nullptr;
    const QHash<int, SourceGroupFrame> sourceFrames = sourceGroupFrames(
        nativeFrames ? &*nativeFrames : sourcePtr);
    std::array<int, kLiverySectionCount> counts{};
    int lastPopulatedSlot = -1;
    bool unchanged = sourcePtr != nullptr && sourcePtr->fullyMapped;
    for (int slot = 0; slot < sectionCount; ++slot) {
        const int sourceCount = sourcePtr != nullptr && slot < sourcePtr->payload.sectionCounts.size()
            ? sourcePtr->payload.sectionCounts[slot] : 0;
        const auto *physical = sourcePhysicalSection(sourcePtr, slot);
        const bool physicalCountMatches = physical != nullptr && physical->shapeCount == sourceCount;
        unchanged = unchanged && sectionMatchesSource(slotShapes[slot], sourceSection(sourcePtr, slot),
            hasGroupedArtwork(slotGroups[slot]), sourceCount, physicalCountMatches);
        if (!slotShapes[slot].isEmpty()) {
            lastPopulatedSlot = slot;
        }
    }
    QByteArray body;
    if (unchanged) {
        body = sourcePtr->payload.body;
        QVector<QPair<int, char>> states;
        for (const auto &section : sourcePtr->sections) {
            collectMissingShapeStates(section.subtree, states);
        }
        std::sort(states.begin(), states.end(), [](const auto &a, const auto &b) {
            return a.first > b.first;
        });
        for (const auto &state : states) {
            body.insert(state.first, state.second);
        }
        std::copy_n(sourcePtr->payload.sectionCounts.cbegin(),
            std::min(sectionCount, static_cast<int>(sourcePtr->payload.sectionCounts.size())), counts.begin());
    } else {
        for (int slot = 0; slot < sectionCount; ++slot) {
            const auto &shapes = slotShapes[slot];
            const LiverySection *section = sourceSection(sourcePtr, slot);
            const auto *physical = sourcePhysicalSection(sourcePtr, slot);
            const int sourceCount = sourcePtr != nullptr && slot < sourcePtr->payload.sectionCounts.size()
                ? sourcePtr->payload.sectionCounts[slot] : static_cast<int>(shapes.size());
            const bool physicalCountMatches = physical != nullptr && physical->shapeCount == sourceCount;
            if (shapes.isEmpty()) {
                QByteArray empty = sourceEmptySlot(sourcePtr, slot);
                if (slot == lastPopulatedSlot + 1) {
                    empty = detail::writeVinylChildHeader(0, {});
                }
                body.append(empty);
                continue;
            }
            if (physical != nullptr && sectionMatchesSource(shapes, section,
                    hasGroupedArtwork(slotGroups[slot]), sourceCount, physicalCountMatches)) {
                const auto &span = physical->artwork;
                QByteArray artwork = sourcePtr->payload.body.mid(span.start, span.size());
                repairSourceShapeStates(artwork, *section);
                body.append(artwork);
                body.append(shapes.back()->mask ? '\x01' : '\0');
                counts[slot] = sourceCount;
            } else {
                appendStructuralSection(body, slotGroups[slot], slot, sourceFrames,
                    {sectionCount < kLiverySectionCount, sectionCount == kFM2023SectionCount});
                counts[slot] = static_cast<int>(shapes.size());
            }
            if (slot + 1 < sectionCount) {
                body.append(sourceSectionBoundary(sourcePtr, slot));
            }
        }
    }

    if (!detail::readVinylRecordStream(body, false).complete) {
        throw std::runtime_error("livery export could not frame its artwork records");
    }

    if (outSectionCounts != nullptr) {
        *outSectionCounts = counts;
    }

    QByteArray chunk(reinterpret_cast<const char *>(kGyvlHeader), 0x15);
    chunk.append(body);
    return chunk;
}

QByteArray buildLiveryGyvl(const Project &project,
                          std::array<int, kLiverySectionCount> *outSectionCounts) {
    return buildLiveryGyvlImpl(project, kLiverySectionCount, true, outSectionCounts);
}

QByteArray buildLiveryGyvlForSectionCount(const Project &project, int sectionCount,
                                        std::array<int, kLiverySectionCount> *outSectionCounts) {
    if (sectionCount <= 0 || sectionCount > kLiverySectionCount) {
        throw std::runtime_error("invalid livery export section count");
    }

    std::array<int, kLiverySectionCount> counts{};
    const QByteArray artwork = buildLiveryGyvlImpl(project, sectionCount, false, &counts);
    QVector<int> decodedCounts;
    for (int slot = 0; slot < sectionCount; ++slot) {
        decodedCounts.append(counts[slot]);
    }
    QVector<LiverySection> sections;
    if (sectionCount == kFM2023SectionCount) {
        FM2023LiveryPayload payload;
        payload.gyvlBody = artwork.mid(21);
        payload.sectionCounts = decodedCounts;
        sections = decodeFM2023LiverySections(payload);
    } else {
        sections = buildLiverySections(artwork.mid(21), decodedCounts);
    }
    for (int slot = 0; slot < sectionCount; ++slot) {
        QVector<const scene::Shape *> expected;
        if (project.root != nullptr) {
            for (const auto &child : project.root->children) {
                if (child->kind() == scene::LayerKind::Group) {
                    const auto &group = static_cast<const scene::Group &>(*child);
                    if (group.isLiverySection && group.liverySectionSlot == slot) {
                        collectVisibleShapeLeaves(group, expected);
                    }
                }
            }
        }
        if (slot >= sections.size()) {
            throw std::runtime_error("native export could not reconstruct its artwork sections");
        }
        const QVector<SourceShapeView> actual = sourceSectionShapes(sections[slot]);
        if (actual.size() != expected.size()) {
            throw std::runtime_error(QStringLiteral(
                "native export could not retain its artwork count in %1: expected %2, decoded %3")
                .arg(QString::fromLatin1(kFH6LiverySlots[slot].name))
                .arg(expected.size()).arg(actual.size()).toStdString());
        }
        const Matrix3 canvas = liverySectionCanvasTransform(slot);
        for (int index = 0; index < actual.size(); ++index) {
            const SourceShapeView &encoded = actual[index];
            const scene::Shape &shape = *expected[index];
            if (encoded.raster != shape.raster || encoded.rasterId != shape.rasterId
                || (!shape.raster && encoded.shapeId != shape.shapeId)
                || encoded.color != shape.color
                || !matrixClose(detail::multiply(canvas, encoded.worldMatrix), shape.worldMatrix())) {
                throw std::runtime_error("native export could not retain its artwork transforms or colors");
            }
        }
    }
    if (outSectionCounts != nullptr) {
        *outSectionCounts = counts;
    }

    return artwork;
}

} // namespace fls
