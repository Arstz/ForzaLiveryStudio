#include "vinyl_decoder.h"

#include "binary_io.h"

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
    int flags = 0;
    QByteArray marker;
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

Transform nodeTransform(const VinylGroup &node)
{
    Transform transform;
    transform.px = node.px;
    transform.py = node.py;
    transform.sx = node.sx;
    transform.rot = node.rot;
    transform.sy = node.sy;
    transform.hasSy = true;
    return transform;
}

void normalizeChildTransformSy(VinylGroup &node)
{
    if (node.pendingTransformMarker == QByteArray("\x01\x03", 2)
        && (node.source == QStringLiteral("markerless_count_stack")
            || node.source == QStringLiteral("implicit_transform_pair"))) {
        node.sy = std::abs(node.sy);
    }
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
        const quint16 sid = readLeU16(data, pos + 2);
        if (!(sid > 0 && sid < 0x2000)) {
            return false;
        }
        // Validate coords/scale too (as the bare-02 branch does). Without this a
        // livery SEPARATE transform `00 <16 floats>` whose payload happens to
        // start `02 <plausible id>` is mis-accepted as a `00 02` shape -- but its
        // scale reads as ~0 (a real shape never has zero scale), so the check
        // rejects the impostor and lets readLiveryTransform claim it.
        const double px = readLeFloat(data, pos + 8);
        const double py = readLeFloat(data, pos + 12);
        const double sx = readLeFloat(data, pos + 16);
        return std::abs(px) < 50000.0 && std::abs(py) < 50000.0
            && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0;
    }
    if (static_cast<quint8>(data[pos]) == 0x02) {
        if (pos + 31 > end) {
            return false;
        }
        const quint16 sid = readLeU16(data, pos + 1);
        if (!(sid > 0 && sid < 0x2000)) {
            return false;
        }
        const double px = readLeFloat(data, pos + 7);
        const double py = readLeFloat(data, pos + 11);
        const double sx = readLeFloat(data, pos + 15);
        return std::abs(px) < 50000.0 && std::abs(py) < 50000.0
            && std::abs(sx) > 1e-6 && std::abs(sx) < 200.0;
    }
    return false;
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
    shape.shapeId = readLeU16(data, absPos + 2 + off);
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
    if (std::abs(transform.sx) >= 0.0001 && std::abs(transform.sx) <= 200.0
        && std::abs(transform.rot) <= 10000.0) {
        return transform;
    }
    return std::nullopt;
}

// `livery` selects the embedded-gyvl dialect. Empirically (see LiveryResearch):
// standalone markers that START with 0x00 (the `00 [01..] 03` family) are stored
// in the livery as a bare `00` and are decoded separately by readLiveryTransform;
// the remaining inline group-transform markers keep their bytes but swap the
// final 0x03 terminator for 0x01 (e.g. 03 03 -> 03 01). So in livery mode here we
// ONLY recognise the non-`00` inline markers (matching the `00`-family would
// false-positive on shape float bytes).
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
            continue;  // 00-family lives in readLiveryTransform as a bare `00`
        }
        QByteArray marker = stdMarker;
        if (livery) {
            marker[marker.size() - 1] = static_cast<char>(0x01);  // 03-terminator -> 01
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

bool isLiveryLogoAt(const QByteArray &data, int pos, int end)
{
    if (pos < 0 || pos + 32 > end || pos + 32 > data.size()) {
        return false;
    }
    if (!bytesAt(data, pos, {0x00, 0x02})) {
        return false;
    }
    const quint16 logoId = readLeU16(data, pos + 2);
    if (logoId < 0x8000) {
        return false;
    }
    return readLeU32(data, pos + 4) == 0
        && readLeU32(data, pos + 8) == 0
        && readLeU32(data, pos + 12) == 0
        && readLeU32(data, pos + 16) == 0x3f800000
        && readLeU32(data, pos + 20) == 0x3f800000
        && readLeU32(data, pos + 24) == 0
        && static_cast<quint8>(data[pos + 28]) == 0xff
        && static_cast<quint8>(data[pos + 29]) == 0xff
        && static_cast<quint8>(data[pos + 30]) == 0xff
        && static_cast<quint8>(data[pos + 31]) == 0xff;
}

// Structural recognition of a C_livery SEPARATE transform followed by a child.
// The 00-family livery marker is a bare `00` (unrecognisable by bytes alone -- see
// CLIVERY.md), so a 00/01-led transform can only be confirmed STRUCTURALLY: a
// valid 16-float payload (+ optional 30/70 sy suffix) followed by a group or
// shape. The group validators use this to accept a child that sits behind such a
// transform -- e.g. a mirrored mask container whose first child is a mask group
// preceded by a `00` transform -- which marker-based detection cannot see.
bool liveryTransformThenChildAt(const QByteArray &data, int pos, int end)
{
    if (pos >= end) {
        return false;
    }
    const quint8 lead = static_cast<quint8>(data[pos]);
    if (lead != 0x00 && lead != 0x01) {
        return false;
    }
    if (!readTransformPayload(data, pos + 1, end)) {
        return false;
    }
    int size = 1 + 16;
    if (pos + size + 5 <= end && (static_cast<quint8>(data[pos + size]) & ~0x40) == 0x30) {
        const double sy = readLeFloat(data, pos + size + 1);
        if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
            size += 5;
        }
    }
    const int child = pos + size;
    return isValidShapeAt(data, child, end)
        || isLiveryLogoAt(data, child, end)
        || validCountedGroupAt(data, child, end, true)
        || validMarkerlessGroupAt(data, child, end, true, true);
}

// `allowCountOne` is set by the C_livery section walker: a section's top-level
// group can legitimately hold a single child (count == 1), which the standalone
// C_group grammar never produces markerless. `livery` selects the embedded-gyvl
// transform-marker dialect for the inline transform.
std::optional<GroupInfo> validMarkerlessGroupAt(const QByteArray &data, int pos, int end,
                                                bool allowCountOne = false, bool livery = false)
{
    if (pos + 3 > end) {
        return std::nullopt;
    }
    const int count = readLeU16(data, pos);
    const int childBlocks = static_cast<quint8>(data[pos + 2]);
    const int minCount = allowCountOne ? 1 : 2;
    if (count < minCount || childBlocks <= 0 || childBlocks != (count + 7) / 8) {
        return std::nullopt;
    }
    const int baseSize = 3 + childBlocks + 2;
    if (pos + baseSize > end) {
        return std::nullopt;
    }
    GroupInfo info;
    info.count = count;
    info.childBlocks = childBlocks;
    info.size = baseSize;
    int extra = pos + baseSize;
    bool foundTransform = false;
    for (const QByteArray &marker : transformMarkersAt(data, extra, end, livery)) {
        // C_livery dialect ambiguity: a livery inline marker `<flag> 01` (e.g.
        // `01 01`, `0f 01`, `3f 01`) is byte-identical to a per-shape flag byte
        // (0x01, 0x0f, ... are all flag bytes) followed by a `01 02` shape. These
        // markers ARE real inline transforms in some groups, so disambiguate by
        // payload: if a valid shape starts right after the flag byte, this is
        // `flag + 01 02 shape`, not a transform -- skip the marker and let
        // walkStep consume the flag + shape normally. (A genuine inline transform
        // would need its px bytes to spell `02 <valid id> <valid scale>` here,
        // which is vanishingly unlikely.)
        if (livery && static_cast<quint8>(marker[marker.size() - 1]) == 0x01
            && isValidShapeAt(data, extra + 1, end)) {
            continue;
        }
        auto candidate = readTransformPayload(data, extra + marker.size(), end);
        if (!candidate) {
            continue;
        }
        info.inlineTransform = *candidate;
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
            }
        }
        foundTransform = true;
        break;
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
    if (pos + 4 > end) {
        return std::nullopt;
    }
    const quint8 markerByte = static_cast<quint8>(data[pos]);
    if (markerByte != 0x20 && markerByte != 0x60) {
        return std::nullopt;
    }
    const int count = readLeU16(data, pos + 1);
    const int childBlocks = static_cast<quint8>(data[pos + 3]);
    if (count <= 0 || childBlocks <= 0 || childBlocks != (count + 7) / 8) {
        return std::nullopt;
    }
    const int baseSize = 4 + childBlocks + 2;
    if (pos + baseSize > end) {
        return std::nullopt;
    }
    GroupInfo info;
    info.count = count;
    info.childBlocks = childBlocks;
    info.size = baseSize;
    info.flags = markerByte == 0x60 ? 0x40 : 0;
    int extra = pos + baseSize;
    bool foundTransform = false;
    for (const QByteArray &marker : transformMarkersAt(data, extra, end, livery)) {
        // C_livery dialect ambiguity: a livery inline marker `<flag> 01` (e.g.
        // `01 01`, `0f 01`, `3f 01`) is byte-identical to a per-shape flag byte
        // (0x01, 0x0f, ... are all flag bytes) followed by a `01 02` shape. These
        // markers ARE real inline transforms in some groups, so disambiguate by
        // payload: if a valid shape starts right after the flag byte, this is
        // `flag + 01 02 shape`, not a transform -- skip the marker and let
        // walkStep consume the flag + shape normally. (A genuine inline transform
        // would need its px bytes to spell `02 <valid id> <valid scale>` here,
        // which is vanishingly unlikely.)
        if (livery && static_cast<quint8>(marker[marker.size() - 1]) == 0x01
            && isValidShapeAt(data, extra + 1, end)) {
            continue;
        }
        auto candidate = readTransformPayload(data, extra + marker.size(), end);
        if (!candidate) {
            continue;
        }
        info.inlineTransform = *candidate;
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
            }
        }
        foundTransform = true;
        break;
    }
    if (!foundTransform && extra < end && (static_cast<quint8>(data[extra]) == 0x02
            || static_cast<quint8>(data[extra]) == 0x03
            || static_cast<quint8>(data[extra]) == 0xff)) {
        // `03` was excluded in livery mode so an inline `03 01` transform wasn't
        // eaten as a flag; but the inline loop above already claims a genuine
        // `03 01` transform (foundTransform) or -- via the flag+shape guard --
        // deliberately leaves `03` as a per-shape flag. Reaching here with `03`
        // means it IS a flag, so consume it as the standalone decoder does.
        // Otherwise walkStep's standalone `03` transform marker swallows the
        // following `01 02` shape (id dropped, then the section desyncs).
        info.flags |= static_cast<quint8>(data[extra]) & ~0x40;
        info.size += 1;
    } else if (!foundTransform && livery && extra + 1 < end
               && static_cast<quint8>(data[extra]) == 0x01
               && !isValidShapeAt(data, extra, end)
               && (isValidShapeAt(data, extra + 1, end)
                   || isLiveryLogoAt(data, extra + 1, end)
                   || validCountedGroupAt(data, extra + 1, end, true)
                   || validMarkerlessGroupAt(data, extra + 1, end, true, true))) {
        // A livery group header can be followed by an extra `01` per-shape flag
        // (standalone omits it) before the first child. `01` is not itself a
        // shape here (isValidShapeAt(extra) is false: it's `01 01..` not `01 02`),
        // and a real shape/group starts right after, so consume the `01` as a
        // flag. Left in place it is mis-read as a `01`-led separate transform
        // whose payload is the following shape's bytes, corrupting the group's
        // scale (e.g. sx = pending * shape.px).
        info.flags |= 0x01;
        info.size += 1;
    }
    return info;
}

bool inlineTransformForFirstChild(const QByteArray &marker)
{
    // Accept both the standalone 0x03 terminator and the C_livery 0x01 terminator.
    if (marker.size() == 2
        && (static_cast<quint8>(marker[0]) & 0x01)
        && (static_cast<quint8>(marker[1]) == 0x03 || static_cast<quint8>(marker[1]) == 0x01)) {
        return true;
    }
    static const QByteArray stdMarkers[] = {
        QByteArray("\xdf\x03\x03", 3), QByteArray("\x03\x03", 2),
        QByteArray("\x3f\x03", 2), QByteArray("\x2f\x03", 2),
        QByteArray("\x1f\x03", 2), QByteArray("\x0f\x03", 2),
        QByteArray("\x0d\x03", 2), QByteArray("\x07\x03", 2),
        QByteArray("\x01\x03", 2), QByteArray("\x00\x03", 2),
        QByteArray("\x03", 1),
    };
    for (const QByteArray &m : stdMarkers) {
        if (marker == m) {
            return true;
        }
        QByteArray livery = m;
        livery[livery.size() - 1] = static_cast<char>(0x01);
        if (marker == livery) {
            return true;
        }
    }
    return false;
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

// C_livery embedded-gyvl dialect: a SEPARATE group transform is a single control
// byte followed by the 16-byte transform payload. The standalone C_group writes
// the same transform as `<lead> [01..] 03` + payload; the livery collapses the
// whole marker to just its FIRST byte (dropping the trailing 01/03 run), so the
// standard multi-byte markers never match. Confirmed first bytes:
//   standalone `00 [01..] 03` -> livery `00`   (00-family, e.g. 00 01 01 03 -> 00)
//   standalone `01 03`        -> livery `01`
// so the lead is 0x00 or 0x01. It must NOT be a multi-bit flag byte such as 0x0f
// or 0xff: those are per-shape flag bytes (walkStep step 6) that legitimately sit
// in front of a `01 02` shape, and reading `0f 01`+16 as a transform desyncs the
// stream. A valid 16-float payload AND a following group are also required (shapes
// and counted groups are matched earlier in walkStep, so by the time this runs a
// `00`/`01` lead + valid floats + a group is unambiguously a separate transform).
std::optional<TransformRecord> readLiveryTransform(const QByteArray &data, int pos, int end)
{
    if (pos >= end) {
        return std::nullopt;
    }
    const quint8 lead = static_cast<quint8>(data[pos]);
    if (lead != 0x00 && lead != 0x01) {
        return std::nullopt;
    }
    auto transform = readTransformPayload(data, pos + 1, end);
    if (!transform) {
        return std::nullopt;
    }
    int size = 1 + 16;
    if (pos + size + 5 <= end && (static_cast<quint8>(data[pos + size]) & ~0x40) == 0x30) {
        const double sy = readLeFloat(data, pos + size + 1);
        if (std::abs(sy) >= 0.0001 && std::abs(sy) <= 5000.0) {
            transform->sy = sy;
            transform->hasSy = true;
            size += 5;
        }
    }
    const int next = pos + size;
    if (!validCountedGroupAt(data, next, end, true)
        && !validMarkerlessGroupAt(data, next, end, true, true)) {
        return std::nullopt;
    }
    return TransformRecord{size, *transform, data.mid(pos, 1)};
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
        const int headerGroupEnd = std::min(static_cast<int>(dec.size()), 0x1d + 4 + static_cast<int>(static_cast<quint8>(dec[0x20])) + 2);
        auto groupInfo = validCountedGroupAt(dec, 0x1d, headerGroupEnd);
        if (groupInfo) {
            applyGroupRecord(root, *groupInfo, QStringLiteral("root"));
            root.headerMarker = dec.mid(0x1d, 1);
            root.headerControlBytes = dec.mid(0x21, 0x24 + static_cast<quint8>(dec[0x20]) - 0x21);
            const int bitmapStart = 0x1d + 7;
            const int bitmapEnd = bitmapStart + static_cast<quint8>(dec[0x20]);
            if (bitmapEnd <= dec.size()) {
                root.childTypeBitmap = dec.mid(bitmapStart, bitmapEnd - bitmapStart);
            }
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

bool rootChildBitmapBit(const VinylGroup &root, int index, bool *ok)
{
    *ok = false;
    if (root.childTypeBitmap.isEmpty()) {
        return false;
    }
    const int byteIndex = index / 8;
    if (byteIndex >= root.childTypeBitmap.size()) {
        return false;
    }
    *ok = true;
    return static_cast<quint8>(root.childTypeBitmap[byteIndex]) & (1 << (index % 8));
}

int countShapes(const VinylGroupPtr &node)
{
    int count = 0;
    for (const VinylItem &item : node->items) {
        if (item.isShape()) {
            ++count;
        } else {
            count += countShapes(std::get<VinylGroupPtr>(item.value));
        }
    }
    return count;
}

void collectGroups(const VinylGroupPtr &node, QVector<VinylGroupPtr> &groups)
{
    groups.push_back(node);
    for (const VinylItem &item : node->items) {
        if (!item.isShape()) {
            collectGroups(std::get<VinylGroupPtr>(item.value), groups);
        }
    }
}

void removeChildIdentity(VinylGroup &parent, const VinylGroupPtr &child)
{
    for (int i = 0; i < parent.items.size(); ++i) {
        if (!parent.items[i].isShape() && std::get<VinylGroupPtr>(parent.items[i].value) == child) {
            parent.items.removeAt(i);
            return;
        }
    }
}

void removeShapeAt(VinylGroup &parent, int shapeAbsPos)
{
    for (int i = 0; i < parent.items.size(); ++i) {
        if (parent.items[i].isShape() && std::get<VinylShape>(parent.items[i].value).absPos == shapeAbsPos) {
            parent.items.removeAt(i);
            return;
        }
    }
}

void composeTransformIntoItem(const Transform &transform, VinylItem &item)
{
    if (!item.isShape()) {
        composeTransformIntoNode(transform, *std::get<VinylGroupPtr>(item.value));
    }
}

void applyMaskShapeWrapperInnerTransforms(VinylGroupPtr node)
{
    if (node->items.size() < 4 || node->items[0].isShape()) {
        return;
    }
    auto first = std::get<VinylGroupPtr>(node->items[0].value);
    if (first->items.isEmpty() || first->items[0].isShape()) {
        return;
    }
    int secondIndex = -1;
    for (int i = 1; i < node->items.size(); ++i) {
        if (!node->items[i].isShape()) {
            auto child = std::get<VinylGroupPtr>(node->items[i].value);
            if (child->headerMarker == QByteArray("\x20", 1)
                && child->pendingTransformMarker == QByteArray("\x01\x03", 2)
                && child->inlineTransformMarker == QByteArray("\x03\x03", 2)
                && !child->isMask) {
                secondIndex = i;
                break;
            }
        }
    }
    if (secondIndex < 0) {
        return;
    }
    auto second = std::get<VinylGroupPtr>(node->items[secondIndex].value);
    if (second->items.isEmpty() || second->items[0].isShape()) {
        return;
    }
    const Transform firstTransform = nodeTransform(*first);
    const Transform firstInnerTransform = nodeTransform(*std::get<VinylGroupPtr>(first->items[0].value));
    const Transform secondTransform = nodeTransform(*second);
    const Transform secondInnerTransform = nodeTransform(*std::get<VinylGroupPtr>(second->items[0].value));
    for (int i = 1; i < secondIndex; ++i) {
        composeTransformIntoItem(firstInnerTransform, node->items[i]);
        composeTransformIntoItem(firstTransform, node->items[i]);
    }
    for (int i = secondIndex + 1; i < node->items.size(); ++i) {
        composeTransformIntoItem(secondInnerTransform, node->items[i]);
        composeTransformIntoItem(secondTransform, node->items[i]);
    }
    if (!node->source.contains(QStringLiteral("mask_shape_wrapper_inner_transform"))) {
        node->source += QStringLiteral("+mask_shape_wrapper_inner_transform");
    }
}

bool isMaskShapeWrapperStart(const VinylGroup &root, int index)
{
    if (index <= 0 || index >= root.items.size() || root.items[index].isShape() || !root.items[index - 1].isShape()) {
        return false;
    }
    const VinylShape prev = std::get<VinylShape>(root.items[index - 1].value);
    const auto item = std::get<VinylGroupPtr>(root.items[index].value);
    return prev.marker == QByteArray("\x01\x02", 2)
        && item->pendingTransformMarker == QByteArray("\x01\x03", 2)
        && item->inlineTransformMarker == QByteArray("\x03\x03", 2);
}

bool isNestedWrapperStart(const VinylGroup &root, int index)
{
    if (index <= 0 || index >= root.items.size() || root.items[index].isShape() || root.items[index - 1].isShape()) {
        return false;
    }
    const auto prev = std::get<VinylGroupPtr>(root.items[index - 1].value);
    const auto item = std::get<VinylGroupPtr>(root.items[index].value);
    return prev->source.contains(QStringLiteral("mask_shape_wrapper_absorb"))
        && item->pendingTransformMarker.startsWith(QByteArray("\x00\x01", 2))
        && item->inlineTransformMarker == QByteArray("\x03\x03", 2)
        && item->expectedChildren && *item->expectedChildren == 2
        && item->items.size() == 2
        && std::none_of(item->items.cbegin(), item->items.cend(), [](const VinylItem &entry) { return entry.isShape(); });
}

void absorbRootMaskShapeWrappers(VinylGroupPtr root)
{
    if (!root->expectedChildren || root->items.size() <= *root->expectedChildren) {
        return;
    }
    int index = 1;
    while (index < root->items.size() && root->items.size() > *root->expectedChildren) {
        if (!(isMaskShapeWrapperStart(*root, index) || isNestedWrapperStart(*root, index))) {
            ++index;
            continue;
        }
        auto item = std::get<VinylGroupPtr>(root->items[index].value);
        const QByteArray itemMarker = item->pendingTransformMarker;
        int stop = index + 1;
        while (stop < root->items.size()) {
            QByteArray marker;
            if (!root->items[stop].isShape()) {
                marker = std::get<VinylGroupPtr>(root->items[stop].value)->pendingTransformMarker;
            }
            auto nextGroup = !root->items[stop].isShape() ? std::get<VinylGroupPtr>(root->items[stop].value) : VinylGroupPtr{};
            if (nextGroup && marker.startsWith(QByteArray("\x00\x01", 2))
                && (nextGroup->inlineTransformMarker == QByteArray("\x03\x03", 2)
                    || (itemMarker.startsWith(QByteArray("\x00\x01", 2)) && marker == itemMarker))) {
                break;
            }
            ++stop;
        }
        const int absorbCount = std::min(stop - (index + 1), static_cast<int>(root->items.size()) - *root->expectedChildren);
        if (absorbCount <= 0) {
            ++index;
            continue;
        }
        QVector<VinylItem> absorbed;
        for (int i = 0; i < absorbCount; ++i) {
            absorbed.push_back(root->items[index + 1]);
            root->items.removeAt(index + 1);
        }
        for (const VinylItem &absorbedItem : absorbed) {
            if (absorbedItem.isShape()) {
                addShape(*item, std::get<VinylShape>(absorbedItem.value));
            } else {
                addChild(*item, std::get<VinylGroupPtr>(absorbedItem.value));
            }
        }
        item->expectedChildren = std::max(item->expectedChildren.value_or(0), item->totalChildren());
        applyMaskShapeWrapperInnerTransforms(item);
        if (!item->source.contains(QStringLiteral("mask_shape_wrapper_absorb"))) {
            item->source += QStringLiteral("+mask_shape_wrapper_absorb");
        }
        ++index;
    }
}

void absorbImplicitTransformPairTails(VinylGroupPtr root)
{
    QVector<VinylGroupPtr> groups;
    collectGroups(root, groups);
    for (const VinylGroupPtr &parent : groups) {
        int index = 0;
        while (index < parent->items.size() - 1) {
            if (!parent->items[index].isShape() && !parent->items[index + 1].isShape()) {
                auto item = std::get<VinylGroupPtr>(parent->items[index].value);
                auto next = std::get<VinylGroupPtr>(parent->items[index + 1].value);
                if (item->source == QStringLiteral("implicit_transform_pair")
                    && countShapes(item) == 2
                    && countShapes(next) == 2
                    && next->headerMarker == QByteArray("\x20", 1)
                    && next->pendingTransformMarker == QByteArray("\x00\x03", 2)) {
                    parent->items.removeAt(index + 1);
                    addChild(*item, next);
                    item->expectedChildren = 3;
                    if (parent != root && parent->expectedChildren) {
                        *parent->expectedChildren -= 1;
                    }
                }
            }
            ++index;
        }
    }
}

void absorbRootImplicitPairContinuations(VinylGroupPtr root)
{
    int index = 0;
    while (index < root->items.size() - 1) {
        if (!root->items[index].isShape() && !root->items[index + 1].isShape()) {
            auto item = std::get<VinylGroupPtr>(root->items[index].value);
            auto next = std::get<VinylGroupPtr>(root->items[index + 1].value);
            if (countShapes(item) == 16
                && next->source == QStringLiteral("implicit_transform_pair")
                && countShapes(next) == 4) {
                root->items.removeAt(index + 1);
                addChild(*item, next);
                item->expectedChildren = item->expectedChildren.value_or(item->totalChildren() - 1) + 1;
            }
        }
        ++index;
    }
}

void enforceRootChildBitmap(VinylGroupPtr root)
{
    if (root->childTypeBitmap.isEmpty() || root->items.isEmpty()) {
        return;
    }
    QVector<VinylItem> newItems;
    for (const VinylItem &item : root->items) {
        bool known = false;
        const bool expectedGroup = rootChildBitmapBit(*root, newItems.size(), &known);
        if (known && expectedGroup && item.isShape() && !newItems.isEmpty() && !newItems.back().isShape()) {
            auto parent = std::get<VinylGroupPtr>(newItems.back().value);
            addShape(*parent, std::get<VinylShape>(item.value));
            if (parent->expectedChildren) {
                parent->expectedChildren = std::max(*parent->expectedChildren, parent->totalChildren());
            }
            if (!parent->source.contains(QStringLiteral("root_bitmap_absorb"))) {
                parent->source += QStringLiteral("+root_bitmap_absorb");
            }
            continue;
        }
        newItems.push_back(item);
    }
    root->items = newItems;
}

void markRootTransformExemptSegments(VinylGroupPtr root)
{
    for (int i = 1; i < root->items.size(); ++i) {
        if (!root->items[i - 1].isShape() && !root->items[i].isShape()) {
            auto prev = std::get<VinylGroupPtr>(root->items[i - 1].value);
            auto item = std::get<VinylGroupPtr>(root->items[i].value);
            if (countShapes(prev) > 1000
                && prev->pendingTransformMarker == QByteArray("\x01\x03", 2)
                && item->headerMarker == QByteArray("\x20", 1)
                && item->expectedChildren && *item->expectedChildren == 7
                && item->pendingTransformMarker == QByteArray("\x00\x01\x01\x01\x03", 5)) {
                item->rootTransformExempt = true;
            }
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

// Mutable state threaded through the main parse loop, bundled so the body can be
// shared between buildTree() and the livery section walker.
struct WalkState {
    QVector<VinylGroupPtr> stack;
    std::optional<Transform> pendingTransform;
    QByteArray pendingTransformMarker;
    QByteArray pendingTransformPrefix;
    int pendingFlags = 0;
    bool pendingMask = false;
};

int decodedLiveryDecals(const VinylGroupPtr &sectionNode)
{
    return countShapes(sectionNode);
}

// Build a markerless count-N group from `info` at `pos`, append it to the stack
// top, push it, and update pending state. This is the markerless branch of
// walkStep, factored out so the C_livery section walker can start a "bare"
// section group (no preceding transform) byte-identically to the standalone
// decoder. `s.pendingTransform` may be empty in that bare case.
int pushMarkerlessGroup(const QByteArray &data, int pos, int end, const GroupInfo &info, WalkState &s,
                        bool livery = false)
{
    const bool inlineForFirstChild = info.inlineTransform
        && inlineTransformForFirstChild(info.marker)
        && (validCountedGroupAt(data, pos + info.size, end, livery)
            || validMarkerlessGroupAt(data, pos + info.size, end, false, livery));
    auto node = std::make_shared<VinylGroup>();
    node->nodeType = QStringLiteral("group");
    node->absPos = pos;
    node->pendingTransformMarker = s.pendingTransformMarker;
    node->headerControlBytes = data.mid(pos + 3, info.childBlocks + 2);
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
    // (Bare group with no pending transform: applyGroupRecord already set any
    // inline transform when !inlineForFirstChild; otherwise the node is identity.)
    normalizeChildTransformSy(*node);
    s.pendingTransform = inlineForFirstChild ? info.inlineTransform : std::optional<Transform>{};
    s.pendingTransformMarker = inlineForFirstChild ? info.marker : QByteArray();
    s.pendingTransformPrefix.clear();
    s.pendingFlags = 0;
    s.pendingMask = false;
    addChild(*s.stack.back(), node);
    s.stack.push_back(node);
    return pos + info.size;
}

// Consume exactly one record at `pos`, mutate `s`, and return the next position.
// This is the body of buildTree()'s main loop, factored out so the livery
// section walker can drive the identical grammar. Callers invoke
// closeCompleteStack(s.stack) before each call. When `liveryDialect` is set the
// embedded-gyvl `00`-marked group transforms are recognised too.
int walkStep(const QByteArray &layerData, int pos, int end, WalkState &s, bool liveryDialect = false)
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
        const bool inlineForFirstChild = info.inlineTransform
            && inlineTransformForFirstChild(info.marker)
            && (validCountedGroupAt(layerData, pos + info.size, end, liveryDialect)
                || validMarkerlessGroupAt(layerData, pos + info.size, end, false, liveryDialect));
        auto node = std::make_shared<VinylGroup>();
        node->nodeType = QStringLiteral("group");
        node->absPos = pos;
        node->headerMarker = layerData.mid(pos, 1);
        node->pendingTransformMarker = s.pendingTransformMarker;
        node->headerControlBytes = layerData.mid(pos + 4, info.childBlocks + 2);
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
        addShape(*stack.back(), decodeLiveryLogoAt(layerData, pos));
        s.pendingTransform.reset();
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        s.pendingFlags = 0;
        s.pendingMask = false;
        return pos + 32;
    }

    if (isValidShapeAt(layerData, pos, end)) {
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
            normalizeChildTransformSy(*node);
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
        s.pendingFlags = 0;
        s.pendingMask = false;
        s.pendingTransformMarker.clear();
        s.pendingTransformPrefix.clear();
        return pos + ((bytesAt(layerData, pos, {0x00, 0x02}) || bytesAt(layerData, pos, {0x01, 0x02})) ? 32 : 31);
    }

    auto transformInfo = readTransformRecord(layerData, pos, end);
    if (transformInfo) {
        s.pendingTransform = transformInfo->transform;
        s.pendingTransformMarker = s.pendingTransformPrefix + transformInfo->marker;
        s.pendingTransformPrefix.clear();
        return pos + transformInfo->size;
    }

    if (liveryDialect) {
        if (auto liveryTransform = readLiveryTransform(layerData, pos, end)) {
            s.pendingTransform = liveryTransform->transform;
            s.pendingTransformMarker = liveryTransform->marker;
            s.pendingTransformPrefix.clear();
            return pos + liveryTransform->size;
        }
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

// ---------------------------------------------------------------------------
// Livery section walker
// ---------------------------------------------------------------------------

// The 11 fixed C_livery section slots, in storage order, with the constant
// scaffold orientation each empty slot encodes (see LiveryResearch/SECTIONS.md).
struct LiverySlotDef {
    const char *name;
    double rotationDeg;
};
const LiverySlotDef kLiverySlots[11] = {
    {"Front", 0.0},   {"Back", 0.0},   {"Top", 0.0},
    {"Left", 180.0},  {"Right", 90.0}, {"Spoiler", -90.0},
    {"FrontWindshield", 90.0}, {"BackWindshield", 0.0}, {"TopWindow", 0.0},
    {"LeftWindow", 180.0}, {"RightWindow", 0.0},
};
constexpr int kLiverySectionCount = 11;
constexpr int kLiveryEmptySlotSize = 23;  // constant transform scaffold per empty slot
constexpr int kLiveryRemnantSize = 18;    // trailing scaffold after a populated slot

} // namespace

LayerData VinylTreeDecoder::getLayerData(const QByteArray &payload) const
{
    if (payload.size() > 0x24
        && (static_cast<quint8>(payload[0x1d]) == 0x20
            || static_cast<quint8>(payload[0x1d]) == 0x60)) {
        const int childBlocks = static_cast<quint8>(payload[0x20]);
        const int start = 0x24 + childBlocks;
        if (start < payload.size()) {
            return LayerData{payload.mid(start), start};
        }
    }
    if (payload.size() > 69
        && static_cast<quint8>(payload[37]) == 0x02
        && isValidShapeAt(payload, 37, payload.size())) {
        return LayerData{payload.mid(37), 37};
    }
    return LayerData{payload.mid(38), 38};
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

    absorbRootMaskShapeWrappers(root);
    absorbImplicitTransformPairTails(root);
    absorbRootImplicitPairContinuations(root);
    enforceRootChildBitmap(root);
    markRootTransformExemptSegments(root);
    return *root;
}

QVector<LiverySection> VinylTreeDecoder::buildLiverySections(const QByteArray &body,
                                                             const QVector<int> &sectionCounts) const
{
    QVector<LiverySection> sections;
    sections.reserve(kLiverySectionCount);

    int pos = 0;
    const int end = body.size();
    for (int slot = 0; slot < kLiverySectionCount; ++slot) {
        LiverySection section;
        section.slot = slot;
        section.name = QString::fromLatin1(kLiverySlots[slot].name);
        section.rotationDeg = kLiverySlots[slot].rotationDeg;
        section.absPos = pos;

        const int target = slot < sectionCounts.size() ? sectionCounts[slot] : 0;
        if (target <= 0) {
            // Empty section: a constant transform scaffold, no decals.
            section.populated = false;
            pos = std::min(end, pos + kLiveryEmptySlotSize);
            sections.push_back(section);
            continue;
        }

        // Populated section: one or more top-level markerless count-N groups whose
        // decals sum to `target` (the ground-truth count from the yrvl stats
        // chunk). Each top-level group lacks a preceding transform, so its header
        // is read explicitly; its (possibly nested) children are consumed with the
        // shared grammar walker. After the section's decals are accounted for, a
        // constant trailing scaffold remnant separates it from the next slot.
        section.populated = true;
        auto sectionNode = std::make_shared<VinylGroup>();
        sectionNode->nodeType = QStringLiteral("group");
        sectionNode->source = QStringLiteral("livery_section");
        sectionNode->absPos = pos;
        // No scaffold rotation is applied: shapes keep the transforms encoded by
        // their group hierarchy, so a section decodes identically to a standalone
        // C_group of the same bytes. (section.rotationDeg is recorded for
        // reference only.)

        auto holder = std::make_shared<VinylGroup>();  // sentinel base for the stack
        addChild(*holder, sectionNode);

        // Drive the shared grammar over the whole section until its decals reach
        // the ground-truth target. A section's first top-level group (and any that
        // follow without a preceding transform) is a "bare" markerless count-N
        // group that walkStep cannot start on its own (it needs a pending
        // transform), so bootstrap those explicitly via the SAME markerless logic
        // (inline transform + flag bytes included); everything else (transform-led
        // groups, counted groups, shapes) flows through walkStep.
        WalkState state;
        state.stack = QVector<VinylGroupPtr>{holder, sectionNode};
        int guard = 0;
        while (decodedLiveryDecals(sectionNode) < target && pos < end && guard < end + 16) {
            ++guard;
            closeCompleteStack(state.stack);
            if (state.stack.size() < 2) {
                break;  // sectionNode unexpectedly popped
            }
            const bool atSectionTop = state.stack.back() == sectionNode;
            if (atSectionTop && !state.pendingTransform) {
                if (const auto info = validMarkerlessGroupAt(body, pos, end, /*allowCountOne=*/true, /*livery=*/true)) {
                    pos = pushMarkerlessGroup(body, pos, end, *info, state, /*livery=*/true);
                    continue;
                }
            }
            const int next = walkStep(body, pos, end, state, /*liveryDialect=*/true);
            if (next <= pos) {
                break;  // no progress; avoid an infinite loop
            }
            pos = next;
        }
        closeCompleteStack(state.stack);

        section.subtree = *sectionNode;
        pos = std::min(end, pos + kLiveryRemnantSize);
        sections.push_back(section);
    }

    return sections;
}

QVector<QString> VinylTreeDecoder::validateTree(const VinylGroup &root) const
{
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

LayerData getLayerData(const QByteArray &payload)
{
    return VinylTreeDecoder{}.getLayerData(payload);
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
