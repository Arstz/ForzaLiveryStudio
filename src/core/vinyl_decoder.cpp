#include "vinyl_decoder.h"

#include "binary_io.h"
#include "shape_registry.h"
#include "vinyl_framing.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

namespace fls {
namespace {
using detail::readLeFloat;
using detail::readLeU16;
using detail::readLeU32;

constexpr double Pi = 3.14159265358979323846;
constexpr quint8 kGeneration1RootMarker = 0x01;
constexpr quint8 kGeneration1TransformMarker = 0x01;
constexpr quint8 kGeneration2RootMarker = 0x02;
constexpr quint8 kLegacyShapeMarker = 0x01;
constexpr quint8 kGeneration2TransformMarker = 0x02;
constexpr quint8 kCurrentShapeMarker = 0x02;
constexpr quint8 kCurrentTransformMarker = 0x03;

struct Transform {
    double px = 0.0;
    double py = 0.0;
    double sx = 1.0;
    double rot = 0.0;
    double sy = 1.0;
    bool hasSy = false;
};

struct GroupInfo {
    int count = 0;
    int childBlocks = 0;
    int size = 0;
    std::optional<Transform> inlineTransform;
    bool transformForFirstChild = false;
    int flags = 0;
    QByteArray marker;
    QByteArray childTypeBitmap;
    QByteArray controlBytes;
};

struct TransformRecord {
    int size = 0;
    Transform transform;
    QByteArray marker;
    int flags = 0;
};

bool bytesAt(const QByteArray &data, int pos, std::initializer_list<quint8> bytes) {
    if (pos < 0 || pos + static_cast<int>(bytes.size()) > data.size()) {
        return false;
    }
    int offset = 0;
    for (quint8 byte : bytes) {
        if (static_cast<quint8>(data[pos + offset]) != byte) {
            return false;
        }
        ++offset;
    }
    return true;
}

bool isShapePayloadAt(const QByteArray &data, int idPos, int transformPos, int endPos) {
    if (idPos < 0 || endPos > data.size()) {
        return false;
    }
    const quint16 shapeId = detail::canonicalShapeId(readLeU16(data, idPos));
    if (!detail::isKnownShapeId(shapeId)) {
        return false;
    }
    const double rotation = readLeFloat(data, transformPos);
    const double px = readLeFloat(data, transformPos + 4);
    const double py = readLeFloat(data, transformPos + 8);
    const double sx = readLeFloat(data, transformPos + 12);
    const double sy = readLeFloat(data, transformPos + 16);
    const double skew = readLeFloat(data, transformPos + 20);

    return std::isfinite(rotation) && std::abs(rotation) <= 10000.0
        && std::abs(px) < 50000.0 && std::abs(py) < 50000.0
        && detail::isValidVinylShapeBasis(sx, sy, skew);
}

void setNodeTransform(VinylGroup &node, const Transform &transform) {
    node.px = transform.px;
    node.py = transform.py;
    node.sx = transform.sx;
    node.rot = transform.rot;
    node.sy = transform.hasSy ? transform.sy : transform.sx;
}

void composeTransformIntoNode(const Transform &parent, VinylGroup &node) {
    const double sx = parent.sx;
    const double sy = parent.hasSy ? parent.sy : parent.sx;
    const double radians = parent.rot * Pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double childX = node.px;
    const double childY = node.py;
    node.px = parent.px + c * sx * childX - s * sy * childY;
    node.py = parent.py + s * sx * childX + c * sy * childY;
    node.sx *= sx;
    node.sy *= sy;
    node.rot += parent.rot;
}

int shapeRecordSizeAt(const QByteArray &data, int pos, int end,
                      quint8 shapeMarker = kCurrentShapeMarker) {
    if (pos < 0 || pos >= end || pos >= data.size()) {
        return 0;
    }
    const quint8 lead = static_cast<quint8>(data[pos]);
    if (pos + 32 <= end && (lead == 0x00 || lead == 0x01)
        && static_cast<quint8>(data[pos + 1]) == shapeMarker
        && isShapePayloadAt(data, pos + 2, pos + 4, pos + 32)) {
        return 32;
    }
    if (pos + 31 <= end && lead == shapeMarker
        && isShapePayloadAt(data, pos + 1, pos + 3, pos + 31)) {
        return 31;
    }

    return 0;
}

int unsupportedShapeRecordSizeAt(const QByteArray &data, int pos, int end, quint8 shapeMarker);

bool isValidShapeAt(const QByteArray &data, int pos, int end,
                    quint8 shapeMarker = kCurrentShapeMarker) {
    return shapeRecordSizeAt(data, pos, end, shapeMarker) > 0
        || (shapeMarker == kLegacyShapeMarker
            && unsupportedShapeRecordSizeAt(data, pos, end, shapeMarker) > 0);
}

bool isControlShapeAt(const QByteArray &data, int pos, int end,
                      quint8 shapeMarker = kCurrentShapeMarker) {
    return shapeRecordSizeAt(data, pos, end, shapeMarker) == 32
        && static_cast<quint8>(data[pos]) == 0x01;
}

int unsupportedShapeRecordSizeAt(const QByteArray &data, int pos, int end, quint8 shapeMarker) {
    if (pos < 0 || pos + 31 > end || pos + 31 > data.size()) {
        return 0;
    }
    const quint8 lead = static_cast<quint8>(data[pos]);
    const bool framed = (lead == 0 || lead == 1)
        && static_cast<quint8>(data[pos + 1]) == shapeMarker;
    const int recordSize = framed ? 32 : 31;
    const int identifierOffset = framed ? 2 : 1;
    const int transformOffset = identifierOffset + 2;
    const quint16 sid = detail::canonicalShapeId(readLeU16(data, pos + identifierOffset));
    if ((!framed && (shapeMarker != kLegacyShapeMarker || lead != shapeMarker || sid < 32768))
        || pos + recordSize > end || pos + recordSize > data.size()) {
        return 0;
    }
    if (sid == 0 || detail::isKnownShapeId(sid)
        || (framed && static_cast<quint8>(data[pos + recordSize - 1]) != 0xff)) {
        return 0;
    }
    const double rotation = readLeFloat(data, pos + transformOffset);
    const double px = readLeFloat(data, pos + transformOffset + 4);
    const double py = readLeFloat(data, pos + transformOffset + 8);
    const double sx = readLeFloat(data, pos + transformOffset + 12);
    const double sy = readLeFloat(data, pos + transformOffset + 16);
    const double skew = readLeFloat(data, pos + transformOffset + 20);
    const bool valid = std::isfinite(rotation) && std::abs(rotation) <= 10000.0
        && std::abs(px) < 50000.0 && std::abs(py) < 50000.0
        && detail::isValidVinylShapeBasis(sx, sy, skew);

    return valid ? recordSize : 0;
}

VinylShape decodeShapeAt(const QByteArray &data, int absPos, bool isMask = false,
                         int flags = 0, quint8 shapeMarker = kCurrentShapeMarker,
                         std::optional<bool> framedOverride = std::nullopt) {
    if (absPos < 0 || absPos + 31 > data.size()) {
        throw std::runtime_error("shape record extends past layer data");
    }
    const quint8 first = static_cast<quint8>(data[absPos]);
    const bool framed = framedOverride.value_or(
        shapeRecordSizeAt(data, absPos, data.size(), shapeMarker) == 32);
    const int off = framed ? 0 : -1;
    VinylShape shape;
    shape.marker = off == 0 ? data.mid(absPos, 2) : data.mid(absPos, 1);
    if (off == 0 && flags == 0) {
        flags = first;
    }
    shape.shapeId = detail::canonicalShapeId(readLeU16(data, absPos + 2 + off));
    shape.rotation = readLeFloat(data, absPos + 4 + off);
    shape.posX = readLeFloat(data, absPos + 8 + off);
    shape.posY = readLeFloat(data, absPos + 12 + off);
    shape.scaleX = readLeFloat(data, absPos + 16 + off);
    shape.scaleY = readLeFloat(data, absPos + 20 + off);
    shape.skew = readLeFloat(data, absPos + 24 + off);
    shape.color = {
        static_cast<quint8>(data[absPos + 28 + off]),
        static_cast<quint8>(data[absPos + 29 + off]),
        static_cast<quint8>(data[absPos + 30 + off]),
        static_cast<quint8>(data[absPos + 31 + off]),
    };
    shape.absPos = absPos;
    shape.flags = flags;
    shape.isMask = isMask;
    return shape;
}

VinylShape decodeLiveryLogoAt(const QByteArray &data, int absPos) {
    const bool framed = bytesAt(data, absPos, {0x00, 0x02})
        || bytesAt(data, absPos, {0x01, 0x02});
    VinylShape logo = decodeShapeAt(
        data, absPos, false, 0, kCurrentShapeMarker, framed);
    logo.isLogo = true;
    logo.logoId = logo.shapeId;
    logo.rasterId = static_cast<quint32>(logo.logoId & 0x7fff);
    return logo;
}

std::optional<Transform> readTransformPayload(const QByteArray &data, int pos, int end) {
    if (pos + 16 > end) {
        return std::nullopt;
    }
    Transform transform;
    transform.px = readLeFloat(data, pos);
    transform.py = readLeFloat(data, pos + 4);
    transform.sx = readLeFloat(data, pos + 8);
    transform.rot = readLeFloat(data, pos + 12);
    transform.sy = transform.sx;
    if (std::abs(transform.px) < 50000.0 && std::abs(transform.py) < 50000.0
        && std::abs(transform.sx) >= 0.0001 && std::abs(transform.sx) <= 200.0
        && std::abs(transform.rot) <= 10000.0) {
        return transform;
    }
    return std::nullopt;
}

QVector<QByteArray> transformMarkersAt(const QByteArray &data, int pos, int end,
                                       bool livery = false,
                                       quint8 standaloneTerminator = kCurrentTransformMarker) {
    QVector<QByteArray> markers;
    if (pos >= end || pos >= data.size()) {
        return markers;
    }
    const quint8 terminator = livery ? 0x01 : standaloneTerminator;
    auto appendMarker = [&](int size) {
        if (pos + size <= end) {
            const QByteArray marker = data.mid(pos, size);
            if (!markers.contains(marker)) {
                markers.push_back(marker);
            }
        }
    };
    if (!livery && static_cast<quint8>(data[pos]) == 0x00) {
        int cursor = pos + 1;
        while (cursor < end && static_cast<quint8>(data[cursor]) == 0x01) {
            ++cursor;
        }
        if (cursor < end && static_cast<quint8>(data[cursor]) == terminator) {
            appendMarker(cursor - pos + 1);
        }
    }
    const quint8 lead = static_cast<quint8>(data[pos]);
    if (lead & 0x01) {
        if (pos + 2 < end
            && static_cast<quint8>(data[pos + 1]) == standaloneTerminator
            && static_cast<quint8>(data[pos + 2]) == terminator) {
            appendMarker(3);
        }
        if (pos + 1 < end && static_cast<quint8>(data[pos + 1]) == terminator) {
            appendMarker(2);
        }
    }
    if (lead == terminator
        && pos + 1 < end
        && static_cast<quint8>(data[pos + 1]) == terminator) {
        appendMarker(2);
    }
    if (lead == terminator) {
        appendMarker(1);
    }
    std::sort(markers.begin(), markers.end(), [](const QByteArray &a, const QByteArray &b) {
        return a.size() > b.size();
    });

    return markers;
}

std::optional<GroupInfo> validCountedGroupAt(
    const QByteArray &data, int pos, int end, bool livery = false,
    quint8 shapeMarker = kCurrentShapeMarker,
    quint8 transformTerminator = kCurrentTransformMarker);
std::optional<GroupInfo> validMarkerlessGroupAt(const QByteArray &data, int pos, int end,
                                                bool allowCountOne, bool livery,
                                                quint8 shapeMarker = kCurrentShapeMarker,
                                                quint8 transformTerminator = kCurrentTransformMarker);
bool groupAtOrAfterControlByte(
    const QByteArray &data, int pos, int end, bool livery,
    quint8 shapeMarker = kCurrentShapeMarker,
    quint8 transformTerminator = kCurrentTransformMarker);

int liveryLogoRecordSize(const QByteArray &data, int pos, int end) {
    const bool framed = bytesAt(data, pos, {0x00, 0x02})
        || bytesAt(data, pos, {0x01, 0x02});
    const int size = framed ? 32 : 31;
    const int fieldOffset = framed ? 0 : -1;
    if (pos < 0 || pos + size > end || pos + size > data.size()
        || (!framed && static_cast<quint8>(data[pos]) != 0x02)) {
        return 0;
    }
    const quint16 logoId = readLeU16(data, pos + 2 + fieldOffset);
    if (logoId < 0x8000) {
        return 0;
    }
    const double rot = readLeFloat(data, pos + 4 + fieldOffset);
    const double px = readLeFloat(data, pos + 8 + fieldOffset);
    const double py = readLeFloat(data, pos + 12 + fieldOffset);
    const double sx = readLeFloat(data, pos + 16 + fieldOffset);
    const double sy = readLeFloat(data, pos + 20 + fieldOffset);
    const bool valid = std::abs(rot) <= 10000.0
        && std::abs(px) < 50000.0 && std::abs(py) < 50000.0
        && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0
        && std::abs(sy) > 1e-6 && std::abs(sy) < 200.0;
    return valid ? size : 0;
}

bool isLiveryLogoAt(const QByteArray &data, int pos, int end) {
    return liveryLogoRecordSize(data, pos, end) > 0;
}

int expectedChildBlocks(int count) {
    return detail::vinylBitmapBytes(count);
}

bool hasMarkerlessRootHeader(const QByteArray &payload) {
    if (payload.size() <= 0x24
        || static_cast<quint8>(payload[0x1d]) != 0x00) {
        return false;
    }
    const int count = readLeU16(payload, 0x1e);
    const auto header = detail::readVinylChildHeader(payload, 0x1e, payload.size(), true);
    if (!header) {
        return false;
    }
    const int childBlocks = header->bitmap.size();
    const int start = 0x1e + header->size;

    return count > 0 && childBlocks > 0
        && start < payload.size();
}

bool childBlockFieldMatches(int count, int storedBlocks, int *effectiveBlocks) {
    if (count <= 0) {
        return false;
    }
    const int expected = expectedChildBlocks(count);
    if (storedBlocks != expected) {
        return false;
    }
    if (effectiveBlocks != nullptr) {
        *effectiveBlocks = expected;
    }
    return true;
}

constexpr int kHeaderFieldShift = 256;

bool markerlessHeaderFieldsMatch(const QByteArray &data, int pos, int end) {
    if (pos + 4 > end || pos + 4 > data.size()) {
        return false;
    }
    const int count = readLeU16(data, pos);

    return count >= 2 && childBlockFieldMatches(count, readLeU16(data, pos + 2), nullptr);
}

// Both header fields scale by one byte position, so a header that is the shifted image of the
// header one byte later reads a preceding flag byte as its own count.
bool isShiftedMarkerlessHeader(const QByteArray &data, int pos, int end, int count) {
    return count >= kHeaderFieldShift && count % kHeaderFieldShift == 0
        && markerlessHeaderFieldsMatch(data, pos + 1, end);
}

QVector<int> liverySeparateTransformMarkerSizes(const QByteArray &data, int pos, int end) {
    QVector<int> sizes;
    if (pos >= end || pos >= data.size()) {
        return sizes;
    }
    if (static_cast<quint8>(data[pos]) == 0x00) {
        int cursor = pos + 1;
        while (cursor < end && static_cast<quint8>(data[cursor]) == 0x01) {
            ++cursor;
        }
        for (int size = cursor - pos; size >= 2; --size) {
            sizes.push_back(size);
        }
    }
    sizes.push_back(1);
    return sizes;
}

bool childBitmapBit(const QByteArray &bitmap, int index) {
    return detail::vinylChildIsGroup(bitmap, index);
}

struct TransformTrailer {
    int size = 0;
    std::optional<double> sy;
    int flags = 0;
};

std::optional<TransformTrailer> transformTrailerAt(
    const QByteArray &data, int pos, int end) {
    const auto attributes = detail::readVinylAttributes(data, pos, end);
    if (!attributes || (attributes->flags & ~0x50) != 0x21) {
        return std::nullopt;
    }
    TransformTrailer trailer;
    trailer.size = attributes->size;
    trailer.flags = attributes->flags & 0x40;
    trailer.sy = attributes->scaleY;

    return trailer;
}

// Transform leads are opaque; the surrounding record structure establishes their role.
bool isLiveryTransformLead(const QByteArray &data, int pos, int end, quint8 shapeMarker) {
    if (pos >= end || pos >= data.size()) {
        return false;
    }
    const quint8 lead = static_cast<quint8>(data[pos]);
    if (lead != 0x00 && isValidShapeAt(data, pos + 1, end, shapeMarker)) {
        return false;
    }
    return true;
}

bool liveryTransformThenChildAt(const QByteArray &data, int pos, int end,
                                quint8 shapeMarker = kCurrentShapeMarker) {
    if (pos >= end) {
        return false;
    }
    if (!isLiveryTransformLead(data, pos, end, shapeMarker)) {
        return false;
    }
    for (int markerSize : liverySeparateTransformMarkerSizes(data, pos, end)) {
        if (!readTransformPayload(data, pos + markerSize, end)) {
            continue;
        }
        int size = markerSize + 16;
        if (pos + size + 5 <= end && (static_cast<quint8>(data[pos + size]) & ~0x40) == 0x30) {
            const double sy = readLeFloat(data, pos + size + 1);
            if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                size += 5;
            }
        }
        const int child = pos + size;
        if (validCountedGroupAt(data, child, end, true, shapeMarker)
            || validMarkerlessGroupAt(data, child, end, true, true, shapeMarker)) {
            return true;
        }
        const auto trailer = transformTrailerAt(data, child, end);
        if (trailer
            && (validCountedGroupAt(data, child + trailer->size, end, true, shapeMarker)
                || validMarkerlessGroupAt(data, child + trailer->size, end,
                                          true, true, shapeMarker))) {
            return true;
        }
        if (child + 1 < end && !isValidShapeAt(data, child, end, shapeMarker)
            && (validCountedGroupAt(data, child + 1, end, true, shapeMarker)
                || validMarkerlessGroupAt(data, child + 1, end, true, true, shapeMarker))) {
            return true;
        }
        const quint8 lead = static_cast<quint8>(data[pos]);
        if ((lead == 0x00 || lead == 0x01)
            && (isValidShapeAt(data, child, end, shapeMarker) || isLiveryLogoAt(data, child, end))) {
            return true;
        }
    }
    return false;
}

bool childTokenAt(const QByteArray &data, int pos, int end, bool livery) {
    return isValidShapeAt(data, pos, end)
        || (livery && isLiveryLogoAt(data, pos, end))
        || validCountedGroupAt(data, pos, end, livery)
        || (livery && validMarkerlessGroupAt(data, pos, end, true, true))
        || (livery && liveryTransformThenChildAt(data, pos, end));
}

std::optional<GroupInfo> validMarkerlessGroupAt(const QByteArray &data, int pos, int end,
                                                bool allowCountOne, bool livery,
                                                quint8 shapeMarker,
                                                quint8 transformTerminator) {
    if (livery && shapeMarker == kLegacyShapeMarker) {
        transformTerminator = kGeneration1TransformMarker;
    }
    if (pos + 4 > end) {
        return std::nullopt;
    }
    const auto header = detail::readVinylChildHeader(data, pos, end);
    if (!header) {
        return std::nullopt;
    }
    const int count = header->count;
    const int minCount = allowCountOne ? 1 : 2;
    if (count < minCount) {
        return std::nullopt;
    }
    if (isShiftedMarkerlessHeader(data, pos, end, count)) {
        return std::nullopt;
    }
    const int baseSize = header->size;
    if (pos + baseSize > end) {
        return std::nullopt;
    }
    GroupInfo info;
    info.count = count;
    info.childBlocks = header->bitmap.size();
    info.size = baseSize;
    info.controlBytes = header->controls;
    info.childTypeBitmap = header->bitmap;
    int extra = pos + baseSize;
    bool foundTransform = false;
    const bool firstChildIsShape = !childBitmapBit(info.childTypeBitmap, 0);
    for (const QByteArray &marker : transformMarkersAt(
             data, extra, end, livery, transformTerminator)) {
        if (firstChildIsShape
            && (isValidShapeAt(data, extra, end, shapeMarker)
                || (livery && isLiveryLogoAt(data, extra, end)))) {
            continue;
        }
        auto candidate = readTransformPayload(data, extra + marker.size(), end);
        if (!candidate) {
            continue;
        }
        info.inlineTransform = *candidate;
        info.transformForFirstChild = groupAtOrAfterControlByte(
            data, extra + marker.size() + 16, end, livery,
            shapeMarker, transformTerminator);
        info.marker = marker;
        info.size += marker.size() + 16;
        const int syPos = extra + marker.size() + 16;
        if (syPos + 5 <= end && (static_cast<quint8>(data[syPos]) & ~0x40) == 0x30) {
            const double sy = readLeFloat(data, syPos + 1);
            if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                Transform t = *info.inlineTransform;
                t.sy = sy;
                t.hasSy = true;
                info.inlineTransform = t;
                info.size += 5;
                info.transformForFirstChild = groupAtOrAfterControlByte(
                    data, syPos + 5, end, livery,
                    shapeMarker, transformTerminator);
            }
        }
        foundTransform = true;
        break;
    }
    if (!foundTransform && livery
        && (childBitmapBit(info.childTypeBitmap, 0)
            || (shapeMarker == kLegacyShapeMarker && !isValidShapeAt(data, extra, end, shapeMarker)))) {
        auto candidate = readTransformPayload(data, extra, end);
        if (candidate) {
            int transformSize = 16;
            int child = extra + transformSize;
            if (child + 5 <= end && (static_cast<quint8>(data[child]) & ~0x40) == 0x30) {
                const double sy = readLeFloat(data, child + 1);
                if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                    candidate->sy = sy;
                    candidate->hasSy = true;
                    transformSize += 5;
                    child += 5;
                }
            }
            if (const auto trailer = transformTrailerAt(data, child, end)) {
                child += trailer->size;
                transformSize += trailer->size;
                if (trailer->sy) {
                    candidate->sy = *trailer->sy;
                    candidate->hasSy = true;
                }
            }
            const bool childIsGroup = childBitmapBit(info.childTypeBitmap, 0);
            const bool childFollows = childIsGroup
                ? groupAtOrAfterControlByte(data, child, end, true, shapeMarker, transformTerminator)
                : isValidShapeAt(data, child, end, shapeMarker);
            if (childFollows) {
                info.inlineTransform = *candidate;
                info.transformForFirstChild = childIsGroup;
                info.size += transformSize;
                foundTransform = true;
            }
        }
    }
    if (!foundTransform) {
        const bool childHere = isValidShapeAt(data, extra, end, shapeMarker)
            || (livery && isLiveryLogoAt(data, extra, end))
            || validCountedGroupAt(data, extra, end, livery,
                                   shapeMarker, transformTerminator)
            || (livery && liveryTransformThenChildAt(data, extra, end, shapeMarker));
        if (!childHere) {
            if (extra + 1 < end
                && (isValidShapeAt(data, extra + 1, end, shapeMarker)
                    || (livery && isLiveryLogoAt(data, extra + 1, end))
                    || validCountedGroupAt(data, extra + 1, end, livery,
                                           shapeMarker, transformTerminator)
                    || (livery && liveryTransformThenChildAt(data, extra + 1, end, shapeMarker)))) {
                info.flags |= static_cast<quint8>(data[extra]) & ~0x40;
                info.size += 1;
            } else {
                return std::nullopt;
            }
        }
    }
    return info;
}

std::optional<GroupInfo> validCountedGroupAt(
    const QByteArray &data, int pos, int end, bool livery,
    quint8 shapeMarker, quint8 transformTerminator) {
    if (livery && shapeMarker == kLegacyShapeMarker) {
        transformTerminator = kGeneration1TransformMarker;
    }
    if (pos + 5 > end) {
        return std::nullopt;
    }
    const quint8 markerByte = static_cast<quint8>(data[pos]);
    if (markerByte != 0x20 && markerByte != 0x60) {
        return std::nullopt;
    }
    const auto header = detail::readVinylChildHeader(data, pos + 1, end);
    if (!header || header->count == 0) {
        return std::nullopt;
    }
    const int baseSize = 1 + header->size;
    if (pos + baseSize > end) {
        return std::nullopt;
    }
    GroupInfo info;
    info.count = header->count;
    info.childBlocks = header->bitmap.size();
    info.size = baseSize;
    info.controlBytes = header->controls;
    info.childTypeBitmap = header->bitmap;
    info.flags = markerByte == 0x60 ? 0x40 : 0;
    int extra = pos + baseSize;
    bool foundTransform = false;
    const bool firstChildIsShape = !childBitmapBit(info.childTypeBitmap, 0);
    for (const QByteArray &marker : transformMarkersAt(
             data, extra, end, livery, transformTerminator)) {
        if (firstChildIsShape
            && (isValidShapeAt(data, extra, end, shapeMarker)
                || (livery && isLiveryLogoAt(data, extra, end)))) {
            continue;
        }
        auto candidate = readTransformPayload(data, extra + marker.size(), end);
        if (!candidate) {
            continue;
        }
        info.inlineTransform = *candidate;
        info.transformForFirstChild = groupAtOrAfterControlByte(
            data, extra + marker.size() + 16, end, livery,
            shapeMarker, transformTerminator);
        info.marker = marker;
        info.size += marker.size() + 16;
        const int syPos = extra + marker.size() + 16;
        if (syPos + 5 <= end && (static_cast<quint8>(data[syPos]) & ~0x40) == 0x30) {
            const double sy = readLeFloat(data, syPos + 1);
            if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                Transform t = *info.inlineTransform;
                t.sy = sy;
                t.hasSy = true;
                info.inlineTransform = t;
                info.size += 5;
                info.transformForFirstChild = groupAtOrAfterControlByte(
                    data, syPos + 5, end, livery,
                    shapeMarker, transformTerminator);
            }
        }
        foundTransform = true;
        break;
    }
    if (!foundTransform && extra < end
        && !isValidShapeAt(data, extra, end, shapeMarker)
        && !(livery && isLiveryLogoAt(data, extra, end))
        && (static_cast<quint8>(data[extra]) == 0x02
            || static_cast<quint8>(data[extra]) == 0x03
            || static_cast<quint8>(data[extra]) == 0xff)) {
        info.flags |= static_cast<quint8>(data[extra]) & ~0x40;
        info.size += 1;
    } else if (!foundTransform && livery && extra + 1 < end
               && static_cast<quint8>(data[extra]) == 0x01
               && !isValidShapeAt(data, extra, end, shapeMarker)
               && (isValidShapeAt(data, extra + 1, end, shapeMarker)
                   || isLiveryLogoAt(data, extra + 1, end)
                   || validCountedGroupAt(data, extra + 1, end, true, shapeMarker, transformTerminator)
                   || validMarkerlessGroupAt(data, extra + 1, end, true, true, shapeMarker, transformTerminator))) {
        info.flags |= 0x01;
        info.size += 1;
    }
    return info;
}

bool groupAtOrAfterControlByte(
    const QByteArray &data, int pos, int end, bool livery,
    quint8 shapeMarker, quint8 transformTerminator) {
    if (validCountedGroupAt(data, pos, end, livery,
                            shapeMarker, transformTerminator)
        || validMarkerlessGroupAt(data, pos, end, false, livery,
                                  shapeMarker, transformTerminator)) {
        return true;
    }
    return pos + 1 < end && !isValidShapeAt(data, pos, end, shapeMarker)
        && (validCountedGroupAt(data, pos + 1, end, livery,
                                shapeMarker, transformTerminator)
            || validMarkerlessGroupAt(data, pos + 1, end, false, livery,
                                      shapeMarker, transformTerminator));
}

std::optional<TransformRecord> readTransformRecord(
    const QByteArray &data, int pos, int end,
    quint8 shapeMarker = kCurrentShapeMarker,
    quint8 transformTerminator = kCurrentTransformMarker) {
    for (const QByteArray &marker : transformMarkersAt(
             data, pos, end, false, transformTerminator)) {
        int size = marker.size() + 16;
        if (pos + size > end) {
            continue;
        }
        auto transform = readTransformPayload(data, pos + marker.size(), end);
        if (!transform) {
            continue;
        }
        if (pos + size + 5 <= end && (static_cast<quint8>(data[pos + size]) & ~0x40) == 0x30) {
            const double sy = readLeFloat(data, pos + size + 1);
            if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                transform->sy = sy;
                transform->hasSy = true;
                size += 5;
            }
        }
        int flags = 0;
        const int trailerPos = pos + size;
        const auto trailer = transformTrailerAt(data, trailerPos, end);
        if (trailer
            && groupAtOrAfterControlByte(
                data, trailerPos + trailer->size, end, false,
                shapeMarker, transformTerminator)) {
            size += trailer->size;
            flags |= trailer->flags;
            if (trailer->sy) {
                transform->sy = *trailer->sy;
                transform->hasSy = true;
            }
        }
        return TransformRecord{size, *transform, marker, flags};
    }
    return std::nullopt;
}

bool liveryTransformEndsAtGroup(const QByteArray &data, int pos, int end, quint8 shapeMarker) {
    for (int markerSize : liverySeparateTransformMarkerSizes(data, pos, end)) {
        auto transform = readTransformPayload(data, pos + markerSize, end);
        if (!transform) {
            continue;
        }
        int size = markerSize + 16;
        if (pos + size + 5 <= end && (static_cast<quint8>(data[pos + size]) & ~0x40) == 0x30) {
            const double sy = readLeFloat(data, pos + size + 1);
            if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                size += 5;
            }
        }
        const int next = pos + size;
        if (validCountedGroupAt(data, next, end, true, shapeMarker)
            || validMarkerlessGroupAt(data, next, end, true, true, shapeMarker)) {
            return true;
        }
        const auto trailer = transformTrailerAt(data, next, end);
        if (trailer
            && (validCountedGroupAt(data, next + trailer->size, end, true, shapeMarker)
                || validMarkerlessGroupAt(data, next + trailer->size,
                                          end, true, true, shapeMarker))) {
            return true;
        }
    }
    return false;
}

std::optional<TransformRecord> readUnmarkedLiveryTransform(const QByteArray &data, int pos, int end);

std::optional<TransformRecord> readLiveryTransform(const QByteArray &data, int pos, int end,
                                                   bool invertOddRotation, quint8 shapeMarker) {
    if (pos >= end) {
        return std::nullopt;
    }
    if (!isLiveryTransformLead(data, pos, end, shapeMarker)) {
        return std::nullopt;
    }

    const quint8 lead = static_cast<quint8>(data[pos]);
    if (shapeMarker == kLegacyShapeMarker && lead > 0x01
        && readUnmarkedLiveryTransform(data, pos, end)) {
        return std::nullopt;
    }
    const bool recognizedFlag = lead == 0x01 || lead == 0x02 || lead == 0x03
        || lead == 0x0f || lead == 0x60 || lead == 0xff;
    if (recognizedFlag && !liveryTransformEndsAtGroup(data, pos, end, shapeMarker)
        && pos + 1 < end && isLiveryTransformLead(data, pos + 1, end, shapeMarker)) {
        for (int markerSize : liverySeparateTransformMarkerSizes(data, pos + 1, end)) {
            auto aligned = readTransformPayload(data, pos + 1 + markerSize, end);
            if (!aligned) {
                continue;
            }
            int alignedSize = markerSize + 16;
            const int syPos = pos + 1 + alignedSize;
            if (syPos + 5 <= end && (static_cast<quint8>(data[syPos]) & ~0x40) == 0x30) {
                const double sy = readLeFloat(data, syPos + 1);
                if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                    alignedSize += 5;
                }
            }
            const int alignedNext = pos + 1 + alignedSize;
            const bool groupFollowsExactly = validCountedGroupAt(data, alignedNext, end, true, shapeMarker)
                || validMarkerlessGroupAt(data, alignedNext, end, true, true, shapeMarker);
            const auto alignedTrailer = transformTrailerAt(data, alignedNext, end);
            const bool trailerThenGroup = alignedTrailer
                && (validCountedGroupAt(data, alignedNext + alignedTrailer->size, end, true, shapeMarker)
                    || validMarkerlessGroupAt(data,
                                              alignedNext + alignedTrailer->size,
                                              end, true, true, shapeMarker));
            if (groupFollowsExactly || trailerThenGroup) {
                return std::nullopt;
            }
        }
    }

    for (int markerSize : liverySeparateTransformMarkerSizes(data, pos, end)) {
        auto transform = readTransformPayload(data, pos + markerSize, end);
        if (!transform) {
            continue;
        }
        int size = markerSize + 16;
        if (pos + size + 5 <= end && (static_cast<quint8>(data[pos + size]) & ~0x40) == 0x30) {
            const double sy = readLeFloat(data, pos + size + 1);
            if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
                transform->sy = sy;
                transform->hasSy = true;
                size += 5;
            }
        }
        const int next = pos + size;
        const bool groupFollowsImmediately = validCountedGroupAt(data, next, end, true, shapeMarker)
            || validMarkerlessGroupAt(data, next, end, true, true, shapeMarker);
        const bool groupFollowsControl = next + 1 < end && !isValidShapeAt(data, next, end, shapeMarker)
            && (validCountedGroupAt(data, next + 1, end, true, shapeMarker)
                || validMarkerlessGroupAt(data, next + 1, end, true, true, shapeMarker));
        const auto trailer = transformTrailerAt(data, next, end);
        const bool groupFollowsTrailer = trailer
            && (validCountedGroupAt(data, next + trailer->size, end, true, shapeMarker)
                || validMarkerlessGroupAt(data, next + trailer->size, end,
                                          true, true, shapeMarker));
        if (groupFollowsImmediately || groupFollowsControl || groupFollowsTrailer) {
            const int groupPos = groupFollowsImmediately ? next
                : (groupFollowsControl ? next + 1 : next + trailer->size);
            auto successorGroup = validCountedGroupAt(data, groupPos, end, true, shapeMarker);
            if (!successorGroup) {
                successorGroup = validMarkerlessGroupAt(data, groupPos, end, true, true, shapeMarker);
            }
            if (groupFollowsTrailer && trailer->sy) {
                transform->sy = *trailer->sy;
                transform->hasSy = true;
            }
            const bool maskTransform = (successorGroup && (successorGroup->flags & 0x40))
                || (groupFollowsTrailer && (trailer->flags & 0x40));
            const double transformSy = transform->hasSy ? transform->sy : transform->sx;
            const bool mirroredTransform = transform->sx * transformSy < 0.0;
            bool scaledFirstChildFrame = false;
            if (successorGroup && successorGroup->inlineTransform
                && successorGroup->transformForFirstChild) {
                const Transform &child = *successorGroup->inlineTransform;
                const double childSy = child.hasSy ? child.sy : child.sx;
                scaledFirstChildFrame = std::abs(std::abs(child.sx) - 1.0) > 1e-6
                    || std::abs(std::abs(childSy) - 1.0) > 1e-6;
            }
            if (groupFollowsTrailer) {
                size += trailer->size;
            }
            const QByteArray marker = data.mid(pos, markerSize);
            if (invertOddRotation && marker == QByteArray("\x01", 1)
                && !maskTransform && !mirroredTransform
                && scaledFirstChildFrame) {
                transform->rot = -transform->rot;
            }
            return TransformRecord{size, *transform, marker,
                                   groupFollowsTrailer ? trailer->flags : 0};
        }
    }
    return std::nullopt;
}

std::optional<TransformRecord> readUnmarkedLiveryTransform(const QByteArray &data, int pos, int end) {
    auto transform = readTransformPayload(data, pos, end);
    int size = 16;
    int flags = 0;
    if (!transform) {
        return std::nullopt;
    }
    if (pos + size + 5 <= end && (static_cast<quint8>(data[pos + size]) & ~0x40) == 0x30) {
        const double sy = readLeFloat(data, pos + size + 1);
        if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
            transform->sy = sy;
            transform->hasSy = true;
            flags = static_cast<quint8>(data[pos + size]) & 0x40;
            size += 5;
        }
    }
    if (!groupAtOrAfterControlByte(data, pos + size, end, true,
                                   kLegacyShapeMarker, kGeneration1TransformMarker)) {
        return std::nullopt;
    }

    return TransformRecord{size, *transform, {}, flags};
}

void applyGroupRecord(VinylGroup &node, const GroupInfo &info, const QString &source,
                      int pendingFlags = 0, bool pendingMask = false,
                      bool applyInlineTransform = true) {
    node.expectedChildren = info.count;
    node.flags = info.flags | pendingFlags;
    node.isMask = (node.flags & 0x40) || pendingMask;
    node.source = source;
    node.inlineTransformMarker = info.marker;
    node.childTypeBitmap = info.childTypeBitmap;
    node.headerControlBytes = info.controlBytes;
    if (applyInlineTransform && info.inlineTransform) {
        setNodeTransform(node, *info.inlineTransform);
        node.effectiveTransformMarker = info.marker;
    }
}

void applyRootHeader(const QByteArray &dec, VinylGroup &root) {
    if (dec.isEmpty()) {
        return;
    }
    auto transform = readTransformPayload(dec, 13, dec.size());
    if (transform) {
        setNodeTransform(root, *transform);
    }
    if (dec.size() > 0x24 && (dec[0x1d] == '\x20' || dec[0x1d] == '\x60')) {
        if (const auto header = detail::readVinylChildHeader(dec, 0x1e, dec.size(), true)) {
            GroupInfo groupInfo;
            groupInfo.count = header->count;
            groupInfo.childBlocks = header->bitmap.size();
            groupInfo.childTypeBitmap = header->bitmap;
            groupInfo.controlBytes = header->controls;
            groupInfo.flags = dec[0x1d] == '\x60' ? 0x40 : 0;
            applyGroupRecord(root, groupInfo, QStringLiteral("root"));
            root.headerMarker = dec.mid(0x1d, 1);
        }
    }
}

void addChild(VinylGroup &parent, const VinylGroupPtr &child) {
    parent.items.push_back(VinylItem{child});
}

void addShape(VinylGroup &parent, const VinylShape &shape) {
    parent.items.push_back(VinylItem{shape});
}

bool groupComplete(const VinylGroupPtr &group) {
    return group->expectedChildren && group->totalChildren() >= *group->expectedChildren;
}

std::optional<bool> nextChildIsGroup(const VinylGroup &group) {
    if (!group.expectedChildren || group.totalChildren() >= *group.expectedChildren) {
        return std::nullopt;
    }
    const int childIndex = group.totalChildren();
    const int byteIndex = childIndex / 8;
    if (byteIndex >= group.childTypeBitmap.size()) {
        return std::nullopt;
    }

    return childBitmapBit(group.childTypeBitmap, childIndex);
}

void closeCompleteStack(QVector<VinylGroupPtr> &stack) {
    while (stack.size() > 1 && groupComplete(stack.back())) {
        stack.pop_back();
    }
}

int countEncodedLiveryDecals(const VinylGroup &node) {
    int count = node.skippedChildren;
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            ++count;
        } else {
            count += countEncodedLiveryDecals(*std::get<VinylGroupPtr>(item.value));
        }
    }
    return count;
}

void collectLiveryLogoCounts(const VinylGroup &node, QHash<quint16, int> &counts) {
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            const VinylShape &shape = std::get<VinylShape>(item.value);
            if (shape.isLogo) {
                ++counts[shape.logoId];
            }
        } else {
            collectLiveryLogoCounts(*std::get<VinylGroupPtr>(item.value), counts);
        }
    }
}

struct InitialTransformResult {
    int pos = 0;
    std::optional<Transform> transform;
    QByteArray marker;
};

InitialTransformResult readInitialChildTransform(
    const QByteArray &data, int pos, int end,
    quint8 shapeMarker = kCurrentShapeMarker,
    quint8 transformTerminator = kCurrentTransformMarker) {
    const int scanEnd = std::min(end, pos + 8);
    for (int candidate = pos; candidate < scanEnd; ++candidate) {
        auto transformInfo = readTransformRecord(
            data, candidate, end, shapeMarker, transformTerminator);
        if (transformInfo
            && validCountedGroupAt(data, candidate + transformInfo->size, end, false,
                                   shapeMarker, transformTerminator)) {
            return InitialTransformResult{candidate + transformInfo->size, transformInfo->transform, transformInfo->marker};
        }
    }
    if (pos + 16 <= end
        && validCountedGroupAt(data, pos + 16, end, false,
                               shapeMarker, transformTerminator)) {
        auto transform = readTransformPayload(data, pos, end);
        if (transform) {
            return InitialTransformResult{pos + 16, *transform, QByteArray()};
        }
    }
    return InitialTransformResult{pos, std::nullopt, QByteArray()};
}

struct WalkState {
    QVector<VinylGroupPtr> stack;
    std::optional<Transform> pendingTransform;
    QByteArray pendingTransformMarker;
    QByteArray pendingTransformPrefix;
    int pendingFlags = 0;
    bool pendingMask = false;
    int decodedDecals = 0;
    const QHash<quint16, int> *logoExtraCounts = nullptr;
    const QHash<int, int> *physicalShapes = nullptr;
};

void collectPhysicalShapes(const detail::VinylPhysicalGroup &group, QHash<int, int> &shapes) {
    for (const auto &shape : group.shapes) {
        shapes.insert(shape.start, shape.size());
    }
    for (const auto &child : group.groups) {
        collectPhysicalShapes(child, shapes);
    }
}

void markPreviousShapeAsMask(WalkState &state, bool throughGroups) {
    if (state.stack.isEmpty() || state.stack.back()->items.isEmpty()) {
        return;
    }
    VinylItem *previous = &state.stack.back()->items.back();
    while (!previous->isShape()) {
        if (!throughGroups) {
            return;
        }
        VinylGroupPtr group = std::get<VinylGroupPtr>(previous->value);
        if (!group || group->items.isEmpty()) {
            return;
        }
        previous = &group->items.back();
    }
    VinylShape &shape = std::get<VinylShape>(previous->value);
    shape.isMask = true;
    shape.flags |= 0x40;
}

VinylShape *terminalShape(VinylGroup &group) {
    if (group.items.isEmpty()) {
        return nullptr;
    }
    VinylItem *item = &group.items.back();
    while (!item->isShape()) {
        VinylGroupPtr child = std::get<VinylGroupPtr>(item->value);
        if (!child || child->items.isEmpty()) {
            return nullptr;
        }
        item = &child->items.back();
    }

    return &std::get<VinylShape>(item->value);
}

void applyTerminalMaskFlag(VinylGroup &root, const QByteArray &layerData) {
    VinylShape *shape = terminalShape(root);
    if (shape == nullptr) {
        return;
    }
    const int recordSize = shape->marker.size() == 2 ? 32 : 31;
    const int flagPos = shape->absPos + recordSize;
    if (flagPos < layerData.size()
        && (static_cast<quint8>(layerData[flagPos]) & 0x01)) {
        shape->isMask = true;
        shape->flags |= 0x40;
    }
}

int pushMarkerlessGroup(const QByteArray &data, int pos, int end, const GroupInfo &info, WalkState &s,
                        bool livery = false) {
    const bool inlineForFirstChild = info.inlineTransform && info.transformForFirstChild;
    auto node = std::make_shared<VinylGroup>();
    node->nodeType = QStringLiteral("group");
    node->absPos = pos;
    node->pendingTransformMarker = s.pendingTransformMarker;
    applyGroupRecord(*node, info, QStringLiteral("markerless_count_stack"),
                     s.pendingFlags, s.pendingMask, !inlineForFirstChild);
    if (s.pendingTransform) {
        if (!info.inlineTransform) {
            setNodeTransform(*node, *s.pendingTransform);
            node->effectiveTransformMarker = s.pendingTransformMarker;
        } else if (inlineForFirstChild) {
            setNodeTransform(*node, *s.pendingTransform);
            node->effectiveTransformMarker = s.pendingTransformMarker;
        } else {
            composeTransformIntoNode(*s.pendingTransform, *node);
            node->effectiveTransformMarker = s.pendingTransformMarker + info.marker;
        }
    }
    s.pendingTransform = inlineForFirstChild ? info.inlineTransform : std::optional<Transform>{};
    s.pendingTransformMarker = inlineForFirstChild ? info.marker : QByteArray();
    s.pendingTransformPrefix.clear();
    s.pendingFlags = 0;
    s.pendingMask = false;
    addChild(*s.stack.back(), node);
    s.stack.push_back(node);
    return pos + info.size;
}

int walkStep(const QByteArray &layerData, int pos, int end, WalkState &s,
             bool liveryDialect = false, bool invertOddLiveryRotation = true,
             quint8 shapeMarker = kCurrentShapeMarker,
             quint8 transformTerminator = kCurrentTransformMarker,
             bool trailingMaskState = true) {
    QVector<VinylGroupPtr> &stack = s.stack;
    const std::optional<bool> expectsGroup = nextChildIsGroup(*stack.back());
    const bool mayDecodeGroup = !expectsGroup || *expectsGroup;
    const bool mayDecodeShape = !expectsGroup || !*expectsGroup;

    auto markerlessInfo = (s.pendingTransform || expectsGroup.value_or(false))
            && mayDecodeGroup
        ? validMarkerlessGroupAt(layerData, pos, end, false, liveryDialect,
                                 shapeMarker, transformTerminator)
        : std::optional<GroupInfo>{};
    if (markerlessInfo) {
        return pushMarkerlessGroup(layerData, pos, end, *markerlessInfo, s, liveryDialect);
    }

    auto countedInfo = mayDecodeGroup
        ? validCountedGroupAt(layerData, pos, end, liveryDialect,
                              shapeMarker, transformTerminator)
        : std::optional<GroupInfo>{};
    if (countedInfo) {
        const auto &info = *countedInfo;
        const bool inlineForFirstChild = info.inlineTransform && info.transformForFirstChild;
        auto node = std::make_shared<VinylGroup>();
        node->nodeType = QStringLiteral("group");
        node->absPos = pos;
        node->headerMarker = layerData.mid(pos, 1);
        node->pendingTransformMarker = s.pendingTransformMarker;
        applyGroupRecord(*node, info, QStringLiteral("count_stack"),
                         s.pendingFlags, s.pendingMask, !inlineForFirstChild);
        if (s.pendingTransform) {
            if (!info.inlineTransform) {
                setNodeTransform(*node, *s.pendingTransform);
                node->effectiveTransformMarker = s.pendingTransformMarker;
            } else if (inlineForFirstChild) {
                setNodeTransform(*node, *s.pendingTransform);
                node->effectiveTransformMarker = s.pendingTransformMarker;
            } else {
                composeTransformIntoNode(*s.pendingTransform, *node);
                node->effectiveTransformMarker = s.pendingTransformMarker + info.marker;
            }
        }
        s.pendingTransform = inlineForFirstChild ? info.inlineTransform : std::optional<Transform>{};
        s.pendingTransformMarker = inlineForFirstChild ? info.marker : QByteArray();
        s.pendingTransformPrefix.clear();
        s.pendingFlags = 0;
        s.pendingMask = false;
        addChild(*stack.back(), node);
        stack.push_back(node);
        return pos + info.size;
    }

    if (liveryDialect && shapeMarker == kLegacyShapeMarker && s.pendingTransform
        && mayDecodeGroup && pos + 1 < end
        && (validCountedGroupAt(layerData, pos + 1, end, true, shapeMarker, transformTerminator)
            || validMarkerlessGroupAt(layerData, pos + 1, end, true, true, shapeMarker, transformTerminator))) {
        return pos + 1;
    }

    const int logoRecordSize = liveryDialect && shapeMarker == kCurrentShapeMarker && mayDecodeShape
        ? liveryLogoRecordSize(layerData, pos, end) : 0;
    if (logoRecordSize > 0) {
        const VinylShape logo = decodeLiveryLogoAt(layerData, pos);
        if (bytesAt(layerData, pos, {0x01, 0x02})) {
            markPreviousShapeAsMask(s, liveryDialect);
        }
        addShape(*stack.back(), logo);
        s.decodedDecals += 1 + (s.logoExtraCounts ? s.logoExtraCounts->value(logo.logoId, 0) : 0);
        s.pendingTransform.reset();
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        s.pendingFlags = 0;
        s.pendingMask = false;
        return pos + logoRecordSize;
    }

    const int shapeRecordSize = mayDecodeShape
        ? shapeRecordSizeAt(layerData, pos, end, shapeMarker) : 0;
    if (shapeRecordSize > 0) {
        if (trailingMaskState && isControlShapeAt(layerData, pos, end, shapeMarker)) {
            markPreviousShapeAsMask(s, liveryDialect);
        }
        if (s.pendingTransform) {
            auto node = std::make_shared<VinylGroup>();
            node->nodeType = QStringLiteral("group");
            node->absPos = pos;
            node->expectedChildren = 2;
            node->flags = s.pendingFlags;
            node->isMask = s.pendingMask;
            node->source = QStringLiteral("implicit_transform_pair");
            node->pendingTransformMarker = s.pendingTransformMarker;
            node->effectiveTransformMarker = s.pendingTransformMarker;
            setNodeTransform(*node, *s.pendingTransform);
            addChild(*stack.back(), node);
            stack.push_back(node);
            s.pendingTransform.reset();
            s.pendingTransformMarker.clear();
            s.pendingTransformPrefix.clear();
            s.pendingFlags = 0;
            s.pendingMask = false;
        }
        int flags = s.pendingFlags;
        const bool isMask = s.pendingMask;
        if (isControlShapeAt(layerData, pos, end, shapeMarker)) {
            flags |= 0x01;
        }
        addShape(*stack.back(), decodeShapeAt(
            layerData, pos, isMask, flags, shapeMarker));
        ++s.decodedDecals;
        s.pendingFlags = 0;
        s.pendingMask = false;
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        return pos + shapeRecordSize;
    }

    if (mayDecodeShape && !s.pendingTransform && s.physicalShapes && s.physicalShapes->contains(pos)) {
        ++stack.back()->skippedChildren;
        ++s.decodedDecals;
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        s.pendingFlags = 0;
        s.pendingMask = false;

        return pos + s.physicalShapes->value(pos);
    }

    // Embedded and standalone transforms use different marker dialects.
    if (liveryDialect) {
        if (auto liveryTransform = readLiveryTransform(layerData, pos, end,
                                                       invertOddLiveryRotation, shapeMarker)) {
            if (!liveryTransform->marker.isEmpty()
                && (static_cast<quint8>(liveryTransform->marker[0]) & 0x01)) {
                markPreviousShapeAsMask(s, true);
            }
            s.pendingTransform = liveryTransform->transform;
            s.pendingTransformMarker = liveryTransform->marker;
            s.pendingTransformPrefix.clear();
            s.pendingFlags |= liveryTransform->flags;
            s.pendingMask = s.pendingMask || (liveryTransform->flags & 0x40);
            return pos + liveryTransform->size;
        }
        if (shapeMarker == kLegacyShapeMarker && mayDecodeGroup && !s.pendingTransform) {
            if (const auto transform = readUnmarkedLiveryTransform(layerData, pos, end)) {
                s.pendingTransform = transform->transform;
                s.pendingTransformMarker.clear();
                s.pendingTransformPrefix.clear();
                s.pendingFlags |= transform->flags;
                s.pendingMask = s.pendingMask || (transform->flags & 0x40);

                return pos + transform->size;
            }
        }
    }

    auto transformInfo = s.pendingTransform || !mayDecodeGroup
        ? std::optional<TransformRecord>{}
        : readTransformRecord(
            layerData, pos, end, shapeMarker, transformTerminator);
    if (transformInfo) {
        if (trailingMaskState && !transformInfo->marker.isEmpty()
            && (static_cast<quint8>(transformInfo->marker[0]) & 0x01)) {
            markPreviousShapeAsMask(s, true);
        }
        s.pendingTransform = transformInfo->transform;
        s.pendingTransformMarker = s.pendingTransformPrefix + transformInfo->marker;
        s.pendingTransformPrefix.clear();
        s.pendingFlags |= transformInfo->flags;
        s.pendingMask = s.pendingMask || (transformInfo->flags & 0x40);
        return pos + transformInfo->size;
    }

    const int unsupportedSize = !s.pendingTransform && mayDecodeShape
        ? unsupportedShapeRecordSizeAt(layerData, pos, end, shapeMarker) : 0;
    if (unsupportedSize > 0) {
        ++stack.back()->skippedChildren;
        ++s.decodedDecals;
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        s.pendingFlags = 0;
        s.pendingMask = false;
        return pos + unsupportedSize;
    }

    const quint8 byte = static_cast<quint8>(layerData[pos]);
    if (byte == 0x60) {
        s.pendingFlags |= 0x40;
        s.pendingMask = true;
        s.pendingTransformPrefix.clear();
    } else if (byte == 0x01 || byte == 0x02 || byte == 0x03 || byte == 0x0f || byte == 0xff) {
        s.pendingFlags |= byte;
        s.pendingTransformPrefix.clear();
    } else {
        s.pendingTransformPrefix = byte != 0 ? layerData.mid(pos, 1) : QByteArray();
    }
    return pos + 1;
}

constexpr int kLiveryEmptySlotSize = detail::kVinylChildHeaderBytes + detail::kVinylBoundaryBytes;
constexpr int kLiveryRemnantSize = 1 + detail::kVinylBoundaryBytes;
constexpr int kLegacyLiveryRemnantSize = detail::kVinylBoundaryBytes;
constexpr int kTopLiverySlot = 2;

} // namespace

const LiverySlotDef kFH6LiverySlots[11] = {
    {"Front", 0.0},   {"Back", 0.0},   {"Top", 0.0},
    {"Left", 180.0},  {"Right", 90.0}, {"Spoiler", -90.0},
    {"FrontWindshield", 90.0}, {"BackWindshield", 0.0}, {"TopWindow", 0.0},
    {"LeftWindow", 180.0}, {"RightWindow", 0.0},
};

Matrix3 liverySectionCanvasTransform(int slot) {
    Matrix3 transform;
    if (slot == 5) {
        transform.m[0][0] = -1.0;
        transform.m[1][1] = -1.0;
    }
    return transform;
}

VinylTreeDecoder::VinylTreeDecoder(VinylDecoderOptions options)
    : options_(options) {
}

LayerData VinylTreeDecoder::getLayerData(const QByteArray &payload) const {
    LayerData layerData;
    if (hasMarkerlessRootHeader(payload)) {
        const auto header = detail::readVinylChildHeader(payload, 0x1e, payload.size(), true);
        const int start = 0x1e + header->size;
        layerData = LayerData{payload.mid(start), start};
    }
    if (layerData.data.isNull()) {
        if (payload.size() > 0x24
            && (static_cast<quint8>(payload[0x1d]) == 0x20
                || static_cast<quint8>(payload[0x1d]) == 0x60)) {
            if (const auto header = detail::readVinylChildHeader(payload, 0x1e, payload.size(), true)) {
                const int start = 0x1e + header->size;
                if (start < payload.size()) {
                    layerData = LayerData{payload.mid(start), start};
                }
            }
        }
    }
    if (layerData.data.isNull() && payload.size() > 69
        && static_cast<quint8>(payload[37]) == 0x02
        && isValidShapeAt(payload, 37, payload.size())) {
        layerData = LayerData{payload.mid(37), 37};
    }
    if (layerData.data.isNull() && payload.size() > 68
        && static_cast<quint8>(payload[0x0c]) == kGeneration2RootMarker
        && shapeRecordSizeAt(payload, 37, payload.size(), kLegacyShapeMarker) > 0) {
        layerData = LayerData{payload.mid(37), 37};
    }
    if (layerData.data.isNull()) {
        layerData = LayerData{payload.mid(38), 38};
    }
    if (options_.normalizeRecords != nullptr) {
        layerData.data = options_.normalizeRecords(std::move(layerData.data));
    }
    return layerData;
}

VinylGroup VinylTreeDecoder::decodeGroup(const QByteArray &payload,
                                         LayerData *decodedLayerData) const {
    LayerData layerData = getLayerData(payload);
    QByteArray decoderPayload = payload;
    if (hasMarkerlessRootHeader(decoderPayload)) {
        decoderPayload[0x1d] = static_cast<char>(0x20);
    }
    VinylGroup root = buildTree(layerData.data, decoderPayload);
    if (options_.finalizeGroup != nullptr) {
        options_.finalizeGroup(root, payload, layerData);
    }
    if (decodedLayerData != nullptr) {
        *decodedLayerData = std::move(layerData);
    }
    return root;
}

VinylGroup VinylTreeDecoder::buildTree(const QByteArray &layerData, const QByteArray &fullPayload) const {
    auto root = std::make_shared<VinylGroup>();
    root->nodeType = QStringLiteral("root");
    root->source = QStringLiteral("root");
    applyRootHeader(fullPayload, *root);

    const quint8 rootMarker = fullPayload.size() > 0x0c
        ? static_cast<quint8>(fullPayload[0x0c]) : 0;
    const bool generation1 = options_.normalizeRecords == nullptr
        && fullPayload.size() > 0x0c
        && fullPayload.startsWith(QByteArray("gyvl", 4))
        && rootMarker == kGeneration1RootMarker;
    const bool generation2 = options_.normalizeRecords == nullptr
        && fullPayload.size() > 0x0c
        && fullPayload.startsWith(QByteArray("gyvl", 4))
        && rootMarker == kGeneration2RootMarker;
    const bool legacyGeneration = generation1 || generation2;
    const quint8 shapeMarker = legacyGeneration
        ? kLegacyShapeMarker : kCurrentShapeMarker;
    const quint8 transformTerminator = generation1
        ? kGeneration1TransformMarker
        : (generation2 ? kGeneration2TransformMarker : kCurrentTransformMarker);
    int pos = 0;
    const int end = layerData.size();
    const auto physical = options_.normalizeRecords == nullptr
        ? detail::readStandaloneVinylRecords(fullPayload) : detail::VinylRecordStream{};
    QHash<int, int> physicalShapes;
    if (physical.complete) {
        const int start = getLayerData(fullPayload).start;
        QHash<int, int> absoluteShapes;
        for (const auto &record : physical.records) {
            if (record.group) {
                collectPhysicalShapes(*record.group, absoluteShapes);
            }
        }
        for (auto it = absoluteShapes.cbegin(); it != absoluteShapes.cend(); ++it) {
            physicalShapes.insert(it.key() - start, it.value());
        }
    }
    WalkState state;
    state.stack = QVector<VinylGroupPtr>{root};
    state.physicalShapes = physical.complete ? &physicalShapes : nullptr;

    auto initial = readInitialChildTransform(
        layerData, pos, end, shapeMarker, transformTerminator);
    pos = initial.pos;
    state.pendingTransform = initial.transform;
    state.pendingTransformMarker = initial.marker;

    while (pos < end) {
        closeCompleteStack(state.stack);
        pos = walkStep(layerData, pos, end, state, false, true,
                       shapeMarker, transformTerminator, !legacyGeneration);
    }
    if (!legacyGeneration && fullPayload.startsWith(QByteArray("gyvl", 4))) {
        applyTerminalMaskFlag(*root, layerData);
    }

    return *root;
}

QVector<LiverySection> VinylTreeDecoder::buildLiverySections(const QByteArray &body,
                                                             const QVector<int> &sectionCounts) const {
    const int encodedSectionCount = std::min(
        static_cast<int>(sectionCounts.size()), kFH6SectionCount);
    QVector<LiverySection> sections = buildLiverySections(
        body, sectionCounts, kFH6LiverySlots, encodedSectionCount);
    sections.reserve(kFH6SectionCount);
    for (int slot = encodedSectionCount; slot < kFH6SectionCount; ++slot) {
        LiverySection section;
        section.slot = slot;
        section.name = QString::fromLatin1(kFH6LiverySlots[slot].name);
        section.rotationDeg = kFH6LiverySlots[slot].rotationDeg;
        sections.push_back(std::move(section));
    }

    return sections;
}

QVector<LiverySection> VinylTreeDecoder::buildLiverySections(const QByteArray &body, const QVector<int> &sectionCounts,
                                                             const LiverySlotDef *slotDefs, int slotCount) const {
    QByteArray decoderBody = options_.normalizeRecords != nullptr
        ? options_.normalizeRecords(body)
        : body;
    const auto physicalStream = detail::readVinylRecordStream(decoderBody, options_.legacyLiveryRecords);
    const auto physicalSections = detail::mapVinylSections(physicalStream, sectionCounts, slotCount);
    const bool physicalMapping = physicalSections.size() == slotCount;
    QHash<int, int> physicalShapes;
    if (physicalMapping) {
        for (const auto &record : physicalStream.records) {
            if (record.group) {
                collectPhysicalShapes(*record.group, physicalShapes);
            }
        }
    }
    const int remnantSize = options_.legacyLiveryRecords ? kLegacyLiveryRemnantSize : kLiveryRemnantSize;
    if (!physicalMapping && options_.appendLiveryTailPadding) {
        int lastPopulated = -1;
        for (int slot = 0; slot < slotCount; ++slot) {
            if (slot < sectionCounts.size() && sectionCounts[slot] > 0) {
                lastPopulated = slot;
            }
        }
        if (lastPopulated >= 0) {
            const int trailingSlots = slotCount - lastPopulated - 1;
            decoderBody.append(QByteArray(remnantSize
                                              + trailingSlots * kLiveryEmptySlotSize,
                                          '\0'));
        }
    }
    const int end = decoderBody.size();
    const quint8 shapeMarker = options_.legacyLiveryRecords ? kLegacyShapeMarker : kCurrentShapeMarker;
    const int minimumRecordSize = options_.legacyLiveryRecords ? 31 : 32;
    QHash<quint16, int> logoExtraCounts;
    for (int pass = 0; pass < 2; ++pass) {
        QVector<LiverySection> sections;
        sections.reserve(slotCount);
        int pos = 0;
        for (int slot = 0; slot < slotCount; ++slot) {
            LiverySection section;
            section.slot = slot;
            section.name = QString::fromLatin1(slotDefs[slot].name);
            section.rotationDeg = slotDefs[slot].rotationDeg;
            const detail::VinylSectionSpan *physical = physicalMapping ? &physicalSections[slot] : nullptr;
            if (physical != nullptr) {
                pos = physical->artwork.start;
            }
            section.absPos = physical != nullptr && (slot >= sectionCounts.size() || sectionCounts[slot] <= 0)
                ? physical->span.start : pos;

            const int target = slot < sectionCounts.size() ? sectionCounts[slot] : 0;
            if (target <= 0) {
                section.populated = false;
                pos = physical != nullptr ? physical->span.end : std::min(end, pos + kLiveryEmptySlotSize);
                sections.push_back(section);
                continue;
            }

            section.populated = true;
            auto sectionNode = std::make_shared<VinylGroup>();
            sectionNode->nodeType = QStringLiteral("group");
            sectionNode->source = QStringLiteral("livery_section");
            sectionNode->absPos = pos;

            auto holder = std::make_shared<VinylGroup>();
            addChild(*holder, sectionNode);

            // The reserved tail is a lower bound for every remaining slot.
            int reservedTail = remnantSize;
            for (int later = slot + 1; later < slotCount; ++later) {
                const int laterTarget = later < sectionCounts.size() ? sectionCounts[later] : 0;
                reservedTail += laterTarget <= 0 ? kLiveryEmptySlotSize : laterTarget * minimumRecordSize;
            }
            const int walkLimit = physical != nullptr ? physical->artwork.end : std::max(pos, end - reservedTail);

            WalkState state;
            state.stack = QVector<VinylGroupPtr>{holder, sectionNode};
            state.logoExtraCounts = &logoExtraCounts;
            state.physicalShapes = physicalMapping ? &physicalShapes : nullptr;
            const bool invertOddLiveryRotation = slot != kTopLiverySlot;
            int guard = 0;
            while ((physical != nullptr || state.decodedDecals < target)
                   && pos < walkLimit && guard < end + 16) {
                ++guard;
                closeCompleteStack(state.stack);
                if (state.stack.size() < 2) {
                    break;
                }
                const bool atSectionRoot = state.stack.back() == sectionNode;
                const int deficit = target - state.decodedDecals;
                const bool nextSlotPopulated =
                    slot + 1 < slotCount && slot + 1 < sectionCounts.size() && sectionCounts[slot + 1] > 0;
                if (physical == nullptr && !options_.legacyLiveryRecords && atSectionRoot && !state.pendingTransform
                    && nextSlotPopulated && deficit > 0 && deficit <= 8) {
                    const auto nextSection = validMarkerlessGroupAt(decoderBody, pos + remnantSize, end,
                                                                    true, true, shapeMarker);
                    if (nextSection && nextSection->count >= 8) {
                        break;
                    }
                }
                if (atSectionRoot && !state.pendingTransform) {
                    if (const auto info =
                            validMarkerlessGroupAt(decoderBody, pos, end, true, true, shapeMarker)) {
                        const bool sectionRootFrame = pos == section.absPos;
                        pos = pushMarkerlessGroup(decoderBody, pos, end, *info, state,
                                                  /*livery=*/true);
                        if (sectionRootFrame) {
                            state.stack.back()->source = QStringLiteral("livery_section_root");
                        }
                        continue;
                    }
                }
                const int next = walkStep(decoderBody, pos, end, state, true,
                                          invertOddLiveryRotation, shapeMarker,
                                          options_.legacyLiveryRecords ? kGeneration1TransformMarker : kCurrentTransformMarker,
                                          !options_.legacyLiveryRecords);
                if (next <= pos) {
                    break;
                }
                pos = next;
            }
            closeCompleteStack(state.stack);
            if (!options_.legacyLiveryRecords && pos < end && static_cast<quint8>(decoderBody[pos]) == 0x01) {
                markPreviousShapeAsMask(state, true);
            }

            section.subtree = *sectionNode;
            pos = physical != nullptr ? physical->span.end : std::min(end, std::min(pos, walkLimit) + remnantSize);
            sections.push_back(section);
        }

        if (pass == 0) {
            // Residual declared counts supply the logical weight of uploaded logos.
            int lastPopulated = -1;
            for (int slot = 0; slot < slotCount; ++slot) {
                if (slot < sectionCounts.size() && sectionCounts[slot] > 0) {
                    lastPopulated = slot;
                }
            }
            if (lastPopulated >= 0 && lastPopulated < sections.size()) {
                const int target = sectionCounts[lastPopulated];
                const int residual = target - countEncodedLiveryDecals(sections[lastPopulated].subtree);
                QHash<quint16, int> logos;
                collectLiveryLogoCounts(sections[lastPopulated].subtree, logos);
                if (residual > 0 && logos.size() == 1) {
                    const auto it = logos.constBegin();
                    int earlierPlacements = 0;
                    for (int slot = 0; slot < lastPopulated; ++slot) {
                        QHash<quint16, int> earlierLogos;
                        collectLiveryLogoCounts(sections[slot].subtree, earlierLogos);
                        earlierPlacements += earlierLogos.value(it.key(), 0);
                    }
                    if (earlierPlacements > 0 && it.value() > 0 && residual % it.value() == 0) {
                        logoExtraCounts.insert(it.key(), residual / it.value());
                        continue;
                    }
                }
            }
        }

        return sections;
    }
    return {};
}

QVector<QString> VinylTreeDecoder::validateTree(const VinylGroup &root) const {
    QVector<QString> errors;
    std::function<void(const VinylGroup &, const QString &)> visit = [&](const VinylGroup &node, const QString &path) {
        if (node.nodeType == QStringLiteral("root") && path == QStringLiteral("root") && node.totalChildren() == 0) {
            errors.push_back(QStringLiteral("%1: root has no children").arg(path));
        }
        if (node.nodeType == QStringLiteral("group") && node.expectedChildren && node.totalChildren() != *node.expectedChildren) {
            errors.push_back(QStringLiteral("%1: group has %2 child(ren), expected %3")
                                 .arg(path)
                                 .arg(node.totalChildren())
                                 .arg(*node.expectedChildren));
        }
        int childIndex = 0;
        for (const VinylItem &item : node.items) {
            if (!item.isShape()) {
                visit(*std::get<VinylGroupPtr>(item.value),
                      QStringLiteral("%1.children[%2]").arg(path).arg(childIndex++));
            }
        }
    };
    visit(root, QStringLiteral("root"));
    return errors;
}

bool detail::isValidVinylShapeBasis(double scaleX, double scaleY, double skew) {
    constexpr double kMinimumBasisLength = 1e-6;
    constexpr double kMaximumScaleX = 200.0;
    constexpr double kMaximumScaleY = 5000.0;
    constexpr double kMaximumRelativeShear = 200.0;
    const double xLength = std::abs(scaleX);
    const double yScale = std::abs(scaleY);
    const double shear = skew * scaleY;
    const double yLength = std::hypot(scaleY, shear);
    const double shearLimit = kMaximumRelativeShear * std::max(1.0, yScale);

    return std::isfinite(scaleX) && std::isfinite(scaleY) && std::isfinite(skew)
        && xLength > kMinimumBasisLength && xLength < kMaximumScaleX
        && yScale > 0.0 && yScale < kMaximumScaleY
        && yLength > kMinimumBasisLength && std::abs(shear) < shearLimit;
}

LayerData getLayerData(const QByteArray &payload) {
    return VinylTreeDecoder{}.getLayerData(payload);
}

VinylGroup decodeGroup(const QByteArray &payload, LayerData *decodedLayerData) {
    return VinylTreeDecoder{}.decodeGroup(payload, decodedLayerData);
}

VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload) {
    return VinylTreeDecoder{}.buildTree(layerData, fullPayload);
}

QVector<LiverySection> buildLiverySections(const QByteArray &body, const QVector<int> &sectionCounts) {
    return VinylTreeDecoder{}.buildLiverySections(body, sectionCounts);
}

QVector<QString> validateTree(const VinylGroup &root) {
    return VinylTreeDecoder{}.validateTree(root);
}

} // namespace fls
