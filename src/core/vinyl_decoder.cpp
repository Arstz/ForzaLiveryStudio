#include "vinyl_decoder.h"

#include "binary_io.h"
#include "shape_registry.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

namespace fh6 {
namespace {
using detail::readLeFloat;
using detail::readLeU16;
using detail::readLeU32;

constexpr double Pi = 3.14159265358979323846;

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
};

bool bytesAt(const QByteArray &data, int pos, std::initializer_list<quint8> bytes)
{
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

void setNodeTransform(VinylGroup &node, const Transform &transform)
{
    node.px = transform.px;
    node.py = transform.py;
    node.sx = transform.sx;
    node.rot = transform.rot;
    node.sy = transform.hasSy ? transform.sy : transform.sx;
}

void composeTransformIntoNode(const Transform &parent, VinylGroup &node)
{
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

bool isValidShapeAt(const QByteArray &data, int pos, int end)
{
    if (pos < 0 || pos >= end || pos >= data.size()) {
        return false;
    }
    if (bytesAt(data, pos, {0x00, 0x02}) || bytesAt(data, pos, {0x01, 0x02})) {
        if (pos + 32 > end) {
            return false;
        }
        const quint16 sid = detail::canonicalShapeId(readLeU16(data, pos + 2));
        if (!detail::isKnownShapeId(sid)) {
            return false;
        }
        const double px = readLeFloat(data, pos + 8);
        const double py = readLeFloat(data, pos + 12);
        const double sx = readLeFloat(data, pos + 16);
        const double sy = readLeFloat(data, pos + 20);
        return std::abs(px) < 50000.0 && std::abs(py) < 50000.0
            && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0
            && std::abs(sy) < 5000.0;
    }
    if (static_cast<quint8>(data[pos]) == 0x02) {
        if (pos + 31 > end) {
            return false;
        }
        const quint16 sid = detail::canonicalShapeId(readLeU16(data, pos + 1));
        if (!detail::isKnownShapeId(sid)) {
            return false;
        }
        const double px = readLeFloat(data, pos + 7);
        const double py = readLeFloat(data, pos + 11);
        const double sx = readLeFloat(data, pos + 15);
        const double sy = readLeFloat(data, pos + 19);
        return std::abs(px) < 50000.0 && std::abs(py) < 50000.0
            && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0
            && std::abs(sy) < 5000.0;
    }
    return false;
}

bool isUnsupportedShapeRecordAt(const QByteArray &data, int pos, int end)
{
    // Unknown framed records still contribute structural occupancy.
    if (pos < 0 || pos + 32 > end || pos + 32 > data.size()
        || !(bytesAt(data, pos, {0x00, 0x02}) || bytesAt(data, pos, {0x01, 0x02}))) {
        return false;
    }
    const quint16 sid = detail::canonicalShapeId(readLeU16(data, pos + 2));
    if (sid == 0 || detail::isKnownShapeId(sid)
        || static_cast<quint8>(data[pos + 31]) != 0xff) {
        return false;
    }
    const double rotation = readLeFloat(data, pos + 4);
    const double px = readLeFloat(data, pos + 8);
    const double py = readLeFloat(data, pos + 12);
    const double sx = readLeFloat(data, pos + 16);
    const double sy = readLeFloat(data, pos + 20);
    const double skew = readLeFloat(data, pos + 24);
    return std::isfinite(rotation) && std::abs(rotation) <= 10000.0
        && std::abs(px) < 50000.0 && std::abs(py) < 50000.0
        && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0
        && std::abs(sy) > 1e-6 && std::abs(sy) < 5000.0
        && std::isfinite(skew) && std::abs(skew) < 200.0;
}

VinylShape decodeShapeAt(const QByteArray &data, int absPos, bool isMask = false, int flags = 0)
{
    if (absPos < 0 || absPos + 31 > data.size()) {
        throw std::runtime_error("shape record extends past layer data");
    }
    const quint8 first = static_cast<quint8>(data[absPos]);
    const int off = (first == 0x00 || first == 0x01) ? 0 : -1;
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

VinylShape decodeLiveryLogoAt(const QByteArray &data, int absPos)
{
    VinylShape logo = decodeShapeAt(data, absPos);
    logo.isLogo = true;
    logo.logoId = logo.shapeId;
    logo.rasterId = static_cast<quint32>(logo.logoId & 0x7fff);
    return logo;
}

std::optional<Transform> readTransformPayload(const QByteArray &data, int pos, int end)
{
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

QVector<QByteArray> transformMarkersAt(const QByteArray &data, int pos, int end, bool livery = false)
{
    QVector<QByteArray> markers;
    if (pos >= end || pos >= data.size()) {
        return markers;
    }
    const quint8 term = livery ? 0x01 : 0x03;
    if (!livery && static_cast<quint8>(data[pos]) == 0x00) {
        int cursor = pos + 1;
        while (cursor < end && static_cast<quint8>(data[cursor]) == 0x01) {
            ++cursor;
        }
        if (cursor < end && static_cast<quint8>(data[cursor]) == term) {
            markers.push_back(data.mid(pos, cursor - pos + 1));
        }
    }
    if (pos + 1 < end
        && (static_cast<quint8>(data[pos]) & 0x01)
        && static_cast<quint8>(data[pos + 1]) == term) {
        markers.push_back(data.mid(pos, 2));
    }
    static const QByteArray stdSuffixMarkers[] = {
        QByteArray("\x00\x01\x01\x03", 4),
        QByteArray("\x00\x01\x03", 3),
        QByteArray("\xdf\x03\x03", 3),
        QByteArray("\x03\x03", 2),
        QByteArray("\x3f\x03", 2),
        QByteArray("\x2f\x03", 2),
        QByteArray("\x1f\x03", 2),
        QByteArray("\x0f\x03", 2),
        QByteArray("\x0d\x03", 2),
        QByteArray("\x07\x03", 2),
        QByteArray("\x01\x03", 2),
        QByteArray("\x00\x03", 2),
        QByteArray("\x03", 1),
    };
    for (const QByteArray &stdMarker : stdSuffixMarkers) {
        if (livery && static_cast<quint8>(stdMarker[0]) == 0x00) {
            continue;
        }
        QByteArray marker = stdMarker;
        if (livery) {
            marker[marker.size() - 1] = static_cast<char>(0x01);
        }
        if (pos + marker.size() <= end && data.mid(pos, marker.size()) == marker
            && !markers.contains(marker)) {
            markers.push_back(marker);
        }
    }
    std::sort(markers.begin(), markers.end(), [](const QByteArray &a, const QByteArray &b) {
        return a.size() > b.size();
    });
    return markers;
}

std::optional<GroupInfo> validCountedGroupAt(const QByteArray &data, int pos, int end, bool livery = false);
std::optional<GroupInfo> validMarkerlessGroupAt(const QByteArray &data, int pos, int end,
                                                bool allowCountOne, bool livery);
bool groupAtOrAfterControlByte(const QByteArray &data, int pos, int end, bool livery);

bool isLiveryLogoAt(const QByteArray &data, int pos, int end)
{
    if (pos < 0 || pos + 32 > end || pos + 32 > data.size()) {
        return false;
    }
    if (!bytesAt(data, pos, {0x00, 0x02}) && !bytesAt(data, pos, {0x01, 0x02})) {
        return false;
    }
    const quint16 logoId = readLeU16(data, pos + 2);
    if (logoId < 0x8000) {
        return false;
    }
    const double rot = readLeFloat(data, pos + 4);
    const double px = readLeFloat(data, pos + 8);
    const double py = readLeFloat(data, pos + 12);
    const double sx = readLeFloat(data, pos + 16);
    const double sy = readLeFloat(data, pos + 20);
    return std::abs(rot) <= 10000.0
        && std::abs(px) < 50000.0 && std::abs(py) < 50000.0
        && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0
        && std::abs(sy) > 1e-6 && std::abs(sy) < 200.0;
}

int expectedChildBlocks(int count)
{
    return (count + 7) / 8;
}

bool childBlockFieldMatches(int count, int storedBlocks, int *effectiveBlocks)
{
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

QVector<int> liverySeparateTransformMarkerSizes(const QByteArray &data, int pos, int end)
{
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

bool childBitmapBit(const QByteArray &bitmap, int index)
{
    const int byteIndex = index / 8;
    return index >= 0 && byteIndex < bitmap.size()
        && (static_cast<quint8>(bitmap[byteIndex]) & (1 << (index % 8)));
}

constexpr int kLiveryTransformTrailerSize = 9;

bool isLiveryTransformTrailerAt(const QByteArray &data, int pos, int end)
{
    return pos >= 0 && pos + kLiveryTransformTrailerSize <= end
        && pos + kLiveryTransformTrailerSize <= data.size()
        && static_cast<quint8>(data[pos]) == 0x21
        && static_cast<quint8>(data[pos + 7]) == 0x09
        && static_cast<quint8>(data[pos + 8]) == 0x00;
}

// Transform leads are opaque; the surrounding record structure establishes their role.
bool isLiveryTransformLead(const QByteArray &data, int pos, int end)
{
    if (pos >= end || pos >= data.size()) {
        return false;
    }
    const quint8 lead = static_cast<quint8>(data[pos]);
    if (lead != 0x00 && isValidShapeAt(data, pos + 1, end)) {
        return false;
    }
    return true;
}

bool liveryTransformThenChildAt(const QByteArray &data, int pos, int end)
{
    if (pos >= end) {
        return false;
    }
    if (!isLiveryTransformLead(data, pos, end)) {
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
        if (validCountedGroupAt(data, child, end, true)
            || validMarkerlessGroupAt(data, child, end, true, true)) {
            return true;
        }
        if (isLiveryTransformTrailerAt(data, child, end)
            && (validCountedGroupAt(data, child + kLiveryTransformTrailerSize, end, true)
                || validMarkerlessGroupAt(data, child + kLiveryTransformTrailerSize, end,
                                          true, true))) {
            return true;
        }
        if (child + 1 < end && !isValidShapeAt(data, child, end)
            && (validCountedGroupAt(data, child + 1, end, true)
                || validMarkerlessGroupAt(data, child + 1, end, true, true))) {
            return true;
        }
        const quint8 lead = static_cast<quint8>(data[pos]);
        if ((lead == 0x00 || lead == 0x01)
            && (isValidShapeAt(data, child, end) || isLiveryLogoAt(data, child, end))) {
            return true;
        }
    }
    return false;
}

bool childTokenAt(const QByteArray &data, int pos, int end, bool livery)
{
    return isValidShapeAt(data, pos, end)
        || (livery && isLiveryLogoAt(data, pos, end))
        || validCountedGroupAt(data, pos, end, livery)
        || (livery && validMarkerlessGroupAt(data, pos, end, true, true))
        || (livery && liveryTransformThenChildAt(data, pos, end));
}

std::optional<GroupInfo> validMarkerlessGroupAt(const QByteArray &data, int pos, int end,
                                                bool allowCountOne = false, bool livery = false)
{
    if (pos + 4 > end) {
        return std::nullopt;
    }
    const int count = readLeU16(data, pos);
    const int storedChildBlocks = readLeU16(data, pos + 2);
    const int minCount = allowCountOne ? 1 : 2;
    int childBlocks = 0;
    if (count < minCount || !childBlockFieldMatches(count, storedChildBlocks, &childBlocks)) {
        return std::nullopt;
    }
    constexpr int kControlBytes = 2;
    const int bitmapStart = pos + 4 + kControlBytes;
    const int baseSize = 4 + kControlBytes + childBlocks;
    if (pos + baseSize > end) {
        return std::nullopt;
    }
    GroupInfo info;
    info.count = count;
    info.childBlocks = childBlocks;
    info.size = baseSize;
    info.controlBytes = data.mid(pos + 4, kControlBytes);
    info.childTypeBitmap = data.mid(bitmapStart, childBlocks);
    int extra = pos + baseSize;
    bool foundTransform = false;
    for (const QByteArray &marker : transformMarkersAt(data, extra, end, livery)) {
        if (livery && static_cast<quint8>(marker[marker.size() - 1]) == 0x01
            && isValidShapeAt(data, extra + 1, end)) {
            continue;
        }
        auto candidate = readTransformPayload(data, extra + marker.size(), end);
        if (!candidate) {
            continue;
        }
        info.inlineTransform = *candidate;
        info.transformForFirstChild = groupAtOrAfterControlByte(
            data, extra + marker.size() + 16, end, livery);
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
                    data, syPos + 5, end, livery);
            }
        }
        foundTransform = true;
        break;
    }
    if (!foundTransform && livery && childBitmapBit(info.childTypeBitmap, 0)) {
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
            if (isLiveryTransformTrailerAt(data, child, end)) {
                child += kLiveryTransformTrailerSize;
                transformSize += kLiveryTransformTrailerSize;
            }
            if (groupAtOrAfterControlByte(data, child, end, true)) {
                info.inlineTransform = *candidate;
                info.transformForFirstChild = true;
                info.size += transformSize;
                foundTransform = true;
            }
        }
    }
    if (!foundTransform) {
        const bool childHere = isValidShapeAt(data, extra, end)
            || (livery && isLiveryLogoAt(data, extra, end))
            || validCountedGroupAt(data, extra, end, livery)
            || (livery && liveryTransformThenChildAt(data, extra, end));
        if (!childHere) {
            if (extra + 1 < end
                && (isValidShapeAt(data, extra + 1, end)
                    || (livery && isLiveryLogoAt(data, extra + 1, end))
                    || validCountedGroupAt(data, extra + 1, end, livery)
                    || (livery && liveryTransformThenChildAt(data, extra + 1, end)))) {
                info.flags |= static_cast<quint8>(data[extra]) & ~0x40;
                info.size += 1;
            } else {
                return std::nullopt;
            }
        }
    }
    return info;
}

std::optional<GroupInfo> validCountedGroupAt(const QByteArray &data, int pos, int end, bool livery)
{
    if (pos + 5 > end) {
        return std::nullopt;
    }
    const quint8 markerByte = static_cast<quint8>(data[pos]);
    if (markerByte != 0x20 && markerByte != 0x60) {
        return std::nullopt;
    }
    const int count = readLeU16(data, pos + 1);
    const int storedChildBlocks = readLeU16(data, pos + 3);
    int childBlocks = 0;
    if (!childBlockFieldMatches(count, storedChildBlocks, &childBlocks)) {
        return std::nullopt;
    }
    constexpr int kControlBytes = 2;
    const int bitmapStart = pos + 5 + kControlBytes;
    const int baseSize = 5 + kControlBytes + childBlocks;
    if (pos + baseSize > end) {
        return std::nullopt;
    }
    GroupInfo info;
    info.count = count;
    info.childBlocks = childBlocks;
    info.size = baseSize;
    info.controlBytes = data.mid(pos + 5, kControlBytes);
    info.childTypeBitmap = data.mid(bitmapStart, childBlocks);
    info.flags = markerByte == 0x60 ? 0x40 : 0;
    int extra = pos + baseSize;
    bool foundTransform = false;
    for (const QByteArray &marker : transformMarkersAt(data, extra, end, livery)) {
        if (livery && static_cast<quint8>(marker[marker.size() - 1]) == 0x01
            && isValidShapeAt(data, extra + 1, end)) {
            continue;
        }
        auto candidate = readTransformPayload(data, extra + marker.size(), end);
        if (!candidate) {
            continue;
        }
        info.inlineTransform = *candidate;
        info.transformForFirstChild = groupAtOrAfterControlByte(
            data, extra + marker.size() + 16, end, livery);
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
                    data, syPos + 5, end, livery);
            }
        }
        foundTransform = true;
        break;
    }
    if (!foundTransform && extra < end
        && !isValidShapeAt(data, extra, end)
        && !(livery && isLiveryLogoAt(data, extra, end))
        && (static_cast<quint8>(data[extra]) == 0x02
            || static_cast<quint8>(data[extra]) == 0x03
            || static_cast<quint8>(data[extra]) == 0xff)) {
        info.flags |= static_cast<quint8>(data[extra]) & ~0x40;
        info.size += 1;
    } else if (!foundTransform && livery && extra + 1 < end
               && static_cast<quint8>(data[extra]) == 0x01
               && !isValidShapeAt(data, extra, end)
               && (isValidShapeAt(data, extra + 1, end)
                   || isLiveryLogoAt(data, extra + 1, end)
                   || validCountedGroupAt(data, extra + 1, end, true)
                   || validMarkerlessGroupAt(data, extra + 1, end, true, true))) {
        info.flags |= 0x01;
        info.size += 1;
    }
    return info;
}

bool groupAtOrAfterControlByte(const QByteArray &data, int pos, int end, bool livery)
{
    if (validCountedGroupAt(data, pos, end, livery)
        || validMarkerlessGroupAt(data, pos, end, false, livery)) {
        return true;
    }
    return livery && pos + 1 < end && !isValidShapeAt(data, pos, end)
        && (validCountedGroupAt(data, pos + 1, end, true)
            || validMarkerlessGroupAt(data, pos + 1, end, false, true));
}

std::optional<TransformRecord> readTransformRecord(const QByteArray &data, int pos, int end)
{
    for (const QByteArray &marker : transformMarkersAt(data, pos, end)) {
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
        return TransformRecord{size, *transform, marker};
    }
    return std::nullopt;
}

bool liveryTransformEndsAtGroup(const QByteArray &data, int pos, int end)
{
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
        if (validCountedGroupAt(data, next, end, true)
            || validMarkerlessGroupAt(data, next, end, true, true)) {
            return true;
        }
        if (isLiveryTransformTrailerAt(data, next, end)
            && (validCountedGroupAt(data, next + kLiveryTransformTrailerSize, end, true)
                || validMarkerlessGroupAt(data, next + kLiveryTransformTrailerSize,
                                          end, true, true))) {
            return true;
        }
    }
    return false;
}

std::optional<TransformRecord> readLiveryTransform(const QByteArray &data, int pos, int end,
                                                   bool invertOddRotation)
{
    if (pos >= end) {
        return std::nullopt;
    }
    if (!isLiveryTransformLead(data, pos, end)) {
        return std::nullopt;
    }

    const quint8 lead = static_cast<quint8>(data[pos]);
    const bool recognizedFlag = lead == 0x01 || lead == 0x02 || lead == 0x03
        || lead == 0x0f || lead == 0x60 || lead == 0xff;
    if (recognizedFlag && !liveryTransformEndsAtGroup(data, pos, end)
        && pos + 1 < end && isLiveryTransformLead(data, pos + 1, end)) {
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
            const bool groupFollowsExactly = validCountedGroupAt(data, alignedNext, end, true)
                || validMarkerlessGroupAt(data, alignedNext, end, true, true);
            const bool trailerThenGroup = isLiveryTransformTrailerAt(data, alignedNext, end)
                && (validCountedGroupAt(data, alignedNext + kLiveryTransformTrailerSize, end, true)
                    || validMarkerlessGroupAt(data,
                                              alignedNext + kLiveryTransformTrailerSize,
                                              end, true, true));
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
        const bool groupFollowsImmediately = validCountedGroupAt(data, next, end, true)
            || validMarkerlessGroupAt(data, next, end, true, true);
        const bool groupFollowsControl = next + 1 < end && !isValidShapeAt(data, next, end)
            && (validCountedGroupAt(data, next + 1, end, true)
                || validMarkerlessGroupAt(data, next + 1, end, true, true));
        const bool groupFollowsTrailer = isLiveryTransformTrailerAt(data, next, end)
            && (validCountedGroupAt(data, next + kLiveryTransformTrailerSize, end, true)
                || validMarkerlessGroupAt(data, next + kLiveryTransformTrailerSize, end,
                                          true, true));
        if (groupFollowsImmediately || groupFollowsControl || groupFollowsTrailer) {
            const int groupPos = groupFollowsImmediately ? next
                : (groupFollowsControl ? next + 1 : next + kLiveryTransformTrailerSize);
            auto successorGroup = validCountedGroupAt(data, groupPos, end, true);
            if (!successorGroup) {
                successorGroup = validMarkerlessGroupAt(data, groupPos, end, true, true);
            }
            const bool maskTransform = successorGroup && (successorGroup->flags & 0x40);
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
                size += kLiveryTransformTrailerSize;
            }
            const QByteArray marker = data.mid(pos, markerSize);
            if (invertOddRotation && marker == QByteArray("\x01", 1)
                && !maskTransform && !mirroredTransform
                && scaledFirstChildFrame) {
                transform->rot = -transform->rot;
            }
            return TransformRecord{size, *transform, marker};
        }
    }
    return std::nullopt;
}

void applyGroupRecord(VinylGroup &node, const GroupInfo &info, const QString &source,
                      int pendingFlags = 0, bool pendingMask = false,
                      bool applyInlineTransform = true)
{
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

void applyRootHeader(const QByteArray &dec, VinylGroup &root)
{
    if (dec.isEmpty()) {
        return;
    }
    auto transform = readTransformPayload(dec, 13, dec.size());
    if (transform) {
        setNodeTransform(root, *transform);
    }
    if (dec.size() > 0x20) {
        const int childBlocks = expectedChildBlocks(readLeU16(dec, 0x1e));
        const int headerGroupEnd = std::min(static_cast<int>(dec.size()), 0x1d + 7 + childBlocks);
        auto groupInfo = validCountedGroupAt(dec, 0x1d, headerGroupEnd);
        if (groupInfo) {
            applyGroupRecord(root, *groupInfo, QStringLiteral("root"));
            root.headerMarker = dec.mid(0x1d, 1);
        }
    }
}

void addChild(VinylGroup &parent, const VinylGroupPtr &child)
{
    parent.items.push_back(VinylItem{child});
}

void addShape(VinylGroup &parent, const VinylShape &shape)
{
    parent.items.push_back(VinylItem{shape});
}

bool groupComplete(const VinylGroupPtr &group)
{
    return group->expectedChildren && group->totalChildren() >= *group->expectedChildren;
}

void closeCompleteStack(QVector<VinylGroupPtr> &stack)
{
    while (stack.size() > 1 && groupComplete(stack.back())) {
        stack.pop_back();
    }
}

int countEncodedLiveryDecals(const VinylGroup &node)
{
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

void collectLiveryLogoCounts(const VinylGroup &node, QHash<quint16, int> &counts)
{
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

InitialTransformResult readInitialChildTransform(const QByteArray &data, int pos, int end)
{
    const int scanEnd = std::min(end, pos + 8);
    for (int candidate = pos; candidate < scanEnd; ++candidate) {
        auto transformInfo = readTransformRecord(data, candidate, end);
        if (transformInfo && validCountedGroupAt(data, candidate + transformInfo->size, end)) {
            return InitialTransformResult{candidate + transformInfo->size, transformInfo->transform, transformInfo->marker};
        }
    }
    if (pos + 16 <= end && validCountedGroupAt(data, pos + 16, end)) {
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
};

void markPreviousDirectShapeAsMask(WalkState &state)
{
    if (state.stack.isEmpty() || state.stack.back()->items.isEmpty()) {
        return;
    }
    VinylItem &previous = state.stack.back()->items.back();
    if (!previous.isShape()) {
        return;
    }
    VinylShape &shape = std::get<VinylShape>(previous.value);
    shape.isMask = true;
    shape.flags |= 0x40;
}

void markPreviousTerminalShapeAsMask(WalkState &state)
{
    if (state.stack.isEmpty() || state.stack.back()->items.isEmpty()) {
        return;
    }
    VinylItem *previous = &state.stack.back()->items.back();
    while (!previous->isShape()) {
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

int pushMarkerlessGroup(const QByteArray &data, int pos, int end, const GroupInfo &info, WalkState &s,
                        bool livery = false)
{
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
             bool liveryDialect = false, bool invertOddLiveryRotation = true)
{
    QVector<VinylGroupPtr> &stack = s.stack;

    auto markerlessInfo = s.pendingTransform
        ? validMarkerlessGroupAt(layerData, pos, end, false, liveryDialect)
        : std::optional<GroupInfo>{};
    if (markerlessInfo) {
        return pushMarkerlessGroup(layerData, pos, end, *markerlessInfo, s, liveryDialect);
    }

    auto countedInfo = validCountedGroupAt(layerData, pos, end, liveryDialect);
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

    if (liveryDialect && isLiveryLogoAt(layerData, pos, end)) {
        const VinylShape logo = decodeLiveryLogoAt(layerData, pos);
        if (bytesAt(layerData, pos, {0x01, 0x02})) {
            markPreviousDirectShapeAsMask(s);
        }
        addShape(*stack.back(), logo);
        s.decodedDecals += 1 + (s.logoExtraCounts ? s.logoExtraCounts->value(logo.logoId, 0) : 0);
        s.pendingTransform.reset();
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        s.pendingFlags = 0;
        s.pendingMask = false;
        return pos + 32;
    }

    if (isValidShapeAt(layerData, pos, end)) {
        if (bytesAt(layerData, pos, {0x01, 0x02})) {
            markPreviousDirectShapeAsMask(s);
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
        if (bytesAt(layerData, pos, {0x01, 0x02})) {
            flags |= 0x01;
        }
        addShape(*stack.back(), decodeShapeAt(layerData, pos, isMask, flags));
        ++s.decodedDecals;
        s.pendingFlags = 0;
        s.pendingMask = false;
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        return pos + ((bytesAt(layerData, pos, {0x00, 0x02}) || bytesAt(layerData, pos, {0x01, 0x02})) ? 32 : 31);
    }

    // Embedded and standalone transforms use different marker dialects.
    if (liveryDialect) {
        if (auto liveryTransform = readLiveryTransform(layerData, pos, end,
                                                       invertOddLiveryRotation)) {
            if (!liveryTransform->marker.isEmpty()
                && (static_cast<quint8>(liveryTransform->marker[0]) & 0x01)) {
                markPreviousTerminalShapeAsMask(s);
            }
            s.pendingTransform = liveryTransform->transform;
            s.pendingTransformMarker = liveryTransform->marker;
            s.pendingTransformPrefix.clear();
            return pos + liveryTransform->size;
        }
    }

    auto transformInfo = s.pendingTransform ? std::optional<TransformRecord>{}
                                            : readTransformRecord(layerData, pos, end);
    if (transformInfo) {
        if (!transformInfo->marker.isEmpty()
            && (static_cast<quint8>(transformInfo->marker[0]) & 0x01)) {
            markPreviousTerminalShapeAsMask(s);
        }
        s.pendingTransform = transformInfo->transform;
        s.pendingTransformMarker = s.pendingTransformPrefix + transformInfo->marker;
        s.pendingTransformPrefix.clear();
        return pos + transformInfo->size;
    }

    if (!s.pendingTransform && isUnsupportedShapeRecordAt(layerData, pos, end)) {
        ++stack.back()->skippedChildren;
        ++s.decodedDecals;
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        s.pendingFlags = 0;
        s.pendingMask = false;
        return pos + 32;
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

constexpr int kLiveryEmptySlotSize = 23;
constexpr int kLiveryRemnantSize = 18;
constexpr int kTopLiverySlot = 2;

} // namespace

const LiverySlotDef kFH6LiverySlots[11] = {
    {"Front", 0.0},   {"Back", 0.0},   {"Top", 0.0},
    {"Left", 180.0},  {"Right", 90.0}, {"Spoiler", -90.0},
    {"FrontWindshield", 90.0}, {"BackWindshield", 0.0}, {"TopWindow", 0.0},
    {"LeftWindow", 180.0}, {"RightWindow", 0.0},
};

Matrix3 liverySectionCanvasTransform(int slot)
{
    Matrix3 transform;
    if (slot == 5) {
        transform.m[0][0] = -1.0;
        transform.m[1][1] = -1.0;
    }
    return transform;
}

VinylTreeDecoder::VinylTreeDecoder(VinylDecoderOptions options)
    : options_(options)
{
}

LayerData VinylTreeDecoder::getLayerData(const QByteArray &payload) const
{
    LayerData layerData;
    if (options_.markerlessRootHeader
        && payload.size() > 0x24
        && static_cast<quint8>(payload[0x1d]) == 0x00) {
        const int count = readLeU16(payload, 0x1e);
        const int childBlocks = static_cast<quint8>(payload[0x20]);
        const int start = 0x24 + childBlocks;
        if (count > 0 && childBlocks == (count + 7) / 8 && start < payload.size()) {
            layerData = LayerData{payload.mid(start), start};
        }
    }
    if (layerData.data.isNull()) {
        if (payload.size() > 0x24
            && (static_cast<quint8>(payload[0x1d]) == 0x20
                || static_cast<quint8>(payload[0x1d]) == 0x60)) {
            const int childBlocks = static_cast<quint8>(payload[0x20]);
            const int start = 0x24 + childBlocks;
            if (start < payload.size()) {
                layerData = LayerData{payload.mid(start), start};
            }
        }
    }
    if (layerData.data.isNull() && payload.size() > 69
        && static_cast<quint8>(payload[37]) == 0x02
        && isValidShapeAt(payload, 37, payload.size())) {
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
                                         LayerData *decodedLayerData) const
{
    LayerData layerData = getLayerData(payload);
    QByteArray decoderPayload = payload;
    if (options_.markerlessRootHeader
        && decoderPayload.size() > 0x20
        && static_cast<quint8>(decoderPayload[0x1d]) == 0x00
        && readLeU16(decoderPayload, 0x1e) > 0
        && static_cast<quint8>(decoderPayload[0x20])
            == (readLeU16(decoderPayload, 0x1e) + 7) / 8
        && layerData.start == 0x24 + static_cast<quint8>(decoderPayload[0x20])) {
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

VinylGroup VinylTreeDecoder::buildTree(const QByteArray &layerData, const QByteArray &fullPayload) const
{
    auto root = std::make_shared<VinylGroup>();
    root->nodeType = QStringLiteral("root");
    root->source = QStringLiteral("root");
    applyRootHeader(fullPayload, *root);

    int pos = 0;
    const int end = layerData.size();
    WalkState state;
    state.stack = QVector<VinylGroupPtr>{root};

    auto initial = readInitialChildTransform(layerData, pos, end);
    pos = initial.pos;
    state.pendingTransform = initial.transform;
    state.pendingTransformMarker = initial.marker;

    while (pos < end) {
        closeCompleteStack(state.stack);
        pos = walkStep(layerData, pos, end, state);
    }

    return *root;
}

QVector<LiverySection> VinylTreeDecoder::buildLiverySections(const QByteArray &body,
                                                             const QVector<int> &sectionCounts) const
{
    return buildLiverySections(body, sectionCounts, kFH6LiverySlots, kFH6SectionCount);
}

QVector<LiverySection> VinylTreeDecoder::buildLiverySections(const QByteArray &body, const QVector<int> &sectionCounts,
                                                             const LiverySlotDef *slotDefs, int slotCount) const
{
    QByteArray decoderBody = options_.normalizeRecords != nullptr
        ? options_.normalizeRecords(body)
        : body;
    if (options_.appendLiveryTailPadding) {
        int lastPopulated = -1;
        for (int slot = 0; slot < slotCount; ++slot) {
            if (slot < sectionCounts.size() && sectionCounts[slot] > 0) {
                lastPopulated = slot;
            }
        }
        if (lastPopulated >= 0) {
            const int trailingSlots = slotCount - lastPopulated - 1;
            decoderBody.append(QByteArray(kLiveryRemnantSize
                                              + trailingSlots * kLiveryEmptySlotSize,
                                          '\0'));
        }
    }
    const int end = decoderBody.size();
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
            section.absPos = pos;

            const int target = slot < sectionCounts.size() ? sectionCounts[slot] : 0;
            if (target <= 0) {
                section.populated = false;
                pos = std::min(end, pos + kLiveryEmptySlotSize);
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
            int reservedTail = kLiveryRemnantSize;
            for (int later = slot + 1; later < slotCount; ++later) {
                const int laterTarget = later < sectionCounts.size() ? sectionCounts[later] : 0;
                reservedTail += laterTarget <= 0 ? kLiveryEmptySlotSize : laterTarget * 32;
            }
            const int walkLimit = std::max(pos, end - reservedTail);

            WalkState state;
            state.stack = QVector<VinylGroupPtr>{holder, sectionNode};
            state.logoExtraCounts = &logoExtraCounts;
            const bool invertOddLiveryRotation = slot != kTopLiverySlot;
            int guard = 0;
            while (state.decodedDecals < target && pos < walkLimit && guard < end + 16) {
                ++guard;
                closeCompleteStack(state.stack);
                if (state.stack.size() < 2) {
                    break;
                }
                const bool atSectionRoot = state.stack.back() == sectionNode;
                const int deficit = target - state.decodedDecals;
                const bool nextSlotPopulated =
                    slot + 1 < slotCount && slot + 1 < sectionCounts.size() && sectionCounts[slot + 1] > 0;
                if (atSectionRoot && !state.pendingTransform && nextSlotPopulated && deficit > 0 && deficit <= 8) {
                    const auto nextSection = validMarkerlessGroupAt(decoderBody, pos + kLiveryRemnantSize, end,
                                                                    /*allowCountOne=*/true, /*livery=*/true);
                    if (nextSection && nextSection->count >= 8) {
                        break;
                    }
                }
                if (atSectionRoot && !state.pendingTransform) {
                    if (const auto info =
                            validMarkerlessGroupAt(decoderBody, pos, end, /*allowCountOne=*/true, /*livery=*/true)) {
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
                                          invertOddLiveryRotation);
                if (next <= pos) {
                    break;
                }
                pos = next;
            }
            closeCompleteStack(state.stack);

            section.subtree = *sectionNode;
            pos = std::min(pos, walkLimit);
            pos = std::min(end, pos + kLiveryRemnantSize);
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

QVector<QString> VinylTreeDecoder::validateTree(const VinylGroup &root) const
{
    QVector<QString> errors;
    std::function<void(const VinylGroup &, const QString &)> visit = [&](const VinylGroup &node, const QString &path) {
        if (node.nodeType == QStringLiteral("root") && path == QStringLiteral("root") && node.totalChildren() == 0) {
            errors.push_back(QStringLiteral("%1: root has no children").arg(path));
        }
        if (node.nodeType == QStringLiteral("group") && node.expectedChildren && node.totalChildren() != *node.expectedChildren)
        {
            errors.push_back(QStringLiteral("%1: group has %2 child(ren), expected %3")
                                 .arg(path)
                                 .arg(node.totalChildren())
                                 .arg(*node.expectedChildren));
        }
        int childIndex = 0;
        for (const VinylItem &item : node.items)
        {
            if (!item.isShape())
            {
                visit(*std::get<VinylGroupPtr>(item.value),
                      QStringLiteral("%1.children[%2]").arg(path).arg(childIndex++));
            }
        }
    };
    visit(root, QStringLiteral("root"));
    return errors;
}

LayerData getLayerData(const QByteArray &payload)
{
    return VinylTreeDecoder{}.getLayerData(payload);
}

VinylGroup decodeGroup(const QByteArray &payload, LayerData *decodedLayerData)
{
    return VinylTreeDecoder{}.decodeGroup(payload, decodedLayerData);
}

VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload)
{
    return VinylTreeDecoder{}.buildTree(layerData, fullPayload);
}

QVector<LiverySection> buildLiverySections(const QByteArray &body, const QVector<int> &sectionCounts)
{
    return VinylTreeDecoder{}.buildLiverySections(body, sectionCounts);
}

QVector<QString> validateTree(const VinylGroup &root)
{
    return VinylTreeDecoder{}.validateTree(root);
}

} // namespace fh6
