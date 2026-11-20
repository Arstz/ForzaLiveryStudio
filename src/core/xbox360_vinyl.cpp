#include "xbox360_vinyl.h"

#include "binary_io.h"
#include "game_export.h"
#include "layer.h"
#include "livery_codec.h"
#include "matrix_math.h"
#include "vinyl_encoding.h"

#include <QtEndian>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace fls::detail {
namespace {

constexpr quint32 kLiveryVersion = 3;
constexpr quint32 kGroupVersion = 14;
constexpr int kInformationOffset = 25;
constexpr int kArtworkSizeOffset = 41;
constexpr int kArtworkOffset = 45;
constexpr int kArtworkHeaderSize = 19;
constexpr int kArtworkScaleOffset = 12;
constexpr int kSectionBoundarySize = 15;
constexpr int kMinimumChildSize = 23;
constexpr int kMaximumGroupDepth = 64;
constexpr int kPaintRecordSize = 35;
constexpr int kPaintRecordCount = 7;
constexpr int kPaintTailSize = 2;
constexpr int kPaintSize = kPaintRecordCount * kPaintRecordSize + kPaintTailSize;
constexpr int kPaintColorOffset = 3;
constexpr int kSectionLimit = 1000;
constexpr int kSpoilerLimit = 100;
constexpr int kSpoilerSlot = kBodyLiverySectionCount - 1;
constexpr int kProfileIdSize = 8;
constexpr quint16 kDecalFlag = 32768;
constexpr int kGroupMaskFlag = 64;
constexpr char kMotorsport2LiveryMagic[] = "lvry";
constexpr quint32 kMotorsport2Version = 7;
constexpr int kTagSize = 4;
constexpr int kMotorsport2ContainerHeaderSize = 8;
constexpr int kMotorsport2ShapeSize = 22;
constexpr int kMotorsport2PaintColorOffset = 2;
constexpr int kMotorsport2PaintSize = kPaintRecordCount * kPaintRecordSize;
constexpr int kMotorsport2LiveryFooterSize = 1 + kProfileIdSize;
constexpr int kMotorsport2GroupFooterSize = 6;
constexpr int kMotorsport2MinimumLiverySize = kTagSize + sizeof(quint32)
    + kBodyLiverySectionCount * sizeof(quint16) + kMotorsport2PaintSize + kMotorsport2LiveryFooterSize;
constexpr int kMotorsport2MaximumLiverySize = kMotorsport2MinimumLiverySize
    + kBodyLiverySectionCount * std::numeric_limits<quint16>::max() * kMotorsport2ShapeSize;
constexpr int kMotorsport2BumperLimit = 500;
constexpr double kMotorsport2SkewUnits = 100.0;

enum class ChildBlockKind { Group, Section };
enum class VinylFormat { Motorsport2, Counted };

BinaryCursor vinylCursor(const QByteArray &bytes) {
    return BinaryCursor(bytes, "truncated Xbox 360 vinyl data", ByteOrder::BigEndian);
}

bool bitIsSet(const QByteArray &bitmap, quint32 index) {
    return static_cast<quint8>(bitmap[index / 8]) & (1u << (index % 8));
}

std::array<quint8, 4> colorFromArgb(quint32 argb) {
    return {static_cast<quint8>(argb), static_cast<quint8>(argb >> 8),
            static_cast<quint8>(argb >> 16), static_cast<quint8>(argb >> 24)};
}

void appendArgb(QByteArray &bytes, const std::array<quint8, 4> &color) {
    for (auto channel = color.rbegin(); channel != color.rend(); ++channel) {
        bytes.append(static_cast<char>(*channel));
    }
}

void readPlacement(BinaryCursor &cursor, VinylGroup &group) {
    group.px = cursor.f32();
    group.py = cursor.f32();
    group.sx = group.sy = cursor.f32();
    group.rot = cursor.f32();
    if (!std::isfinite(group.px) || !std::isfinite(group.py)
        || !std::isfinite(group.sx) || !std::isfinite(group.rot)) {
        throw std::runtime_error("invalid Xbox 360 group placement");
    }
}

qint16 readSignedShort(BinaryCursor &cursor) {
    return std::bit_cast<qint16>(cursor.u16());
}

VinylShape readShape(BinaryCursor &cursor, VinylFormat format) {
    const bool compact = format == VinylFormat::Motorsport2;
    VinylShape shape;
    shape.absPos = cursor.position();
    shape.shapeId = cursor.u16();
    shape.rotation = compact ? readSignedShort(cursor) : cursor.f32();
    shape.posX = compact ? readSignedShort(cursor) : cursor.f32();
    shape.posY = compact ? readSignedShort(cursor) : cursor.f32();
    shape.scaleX = cursor.f32();
    shape.scaleY = cursor.f32();
    shape.skew = compact ? readSignedShort(cursor) / kMotorsport2SkewUnits : cursor.f32();
    shape.color = colorFromArgb(cursor.u32());
    if (!std::isfinite(shape.rotation) || !std::isfinite(shape.posX) || !std::isfinite(shape.posY)
        || !std::isfinite(shape.scaleX) || !std::isfinite(shape.scaleY) || !std::isfinite(shape.skew)) {
        throw std::runtime_error("invalid Xbox 360 shape placement");
    }

    return shape;
}

quint32 readChildCount(BinaryCursor &cursor, ChildBlockKind kind) {
    if (kind == ChildBlockKind::Group) {
        const quint32 highByte = cursor.u8();

        return (highByte << 16) | cursor.u16();
    }

    return cursor.u32();
}

int readChildren(const QByteArray &body, BinaryCursor &cursor, VinylGroup &group,
                 ChildBlockKind kind, int depth = 0) {
    group.absPos = cursor.position();
    const quint32 count = readChildCount(cursor, kind);
    const quint32 bitmapSize = cursor.u32();
    const qsizetype remaining = body.size() - cursor.position();
    if (depth >= kMaximumGroupDepth || bitmapSize != (static_cast<quint64>(count) + 7) / 8
        || bitmapSize > remaining || count > (remaining - bitmapSize) / kMinimumChildSize) {
        throw std::runtime_error("invalid Xbox 360 vinyl child block");
    }
    group.childTypeBitmap = body.mid(cursor.position(), bitmapSize);
    group.expectedChildren = static_cast<int>(count);
    cursor.skip(static_cast<int>(bitmapSize));
    int shapes = 0;
    for (quint32 index = 0; index < count; ++index) {
        if (bitIsSet(group.childTypeBitmap, index)) {
            auto child = std::make_shared<VinylGroup>();
            child->nodeType = QStringLiteral("group");
            readPlacement(cursor, *child);
            shapes += readChildren(body, cursor, *child, ChildBlockKind::Group, depth + 1);
            group.items.push_back({std::move(child)});
        } else {
            group.items.push_back({readShape(cursor, VinylFormat::Counted)});
            ++shapes;
        }
    }

    return shapes;
}

void readMotorsport2Shapes(BinaryCursor &cursor, VinylGroup &group, quint32 count, int end) {
    group.absPos = cursor.position();
    if (group.absPos > end || count > static_cast<quint32>((end - group.absPos) / kMotorsport2ShapeSize)) {
        throw std::runtime_error("invalid FM2 vinyl layer count");
    }
    group.expectedChildren = static_cast<int>(count);
    group.items.reserve(count);
    for (quint32 index = 0; index < count; ++index) {
        group.items.push_back({readShape(cursor, VinylFormat::Motorsport2)});
    }
}

struct LiveryLayout {
    int artworkEnd = 0;
    int paintStart = 0;
};

LiveryLayout liveryLayout(const QByteArray &bytes) {
    if (bytes.size() < kArtworkOffset + kArtworkHeaderSize
        || !bytes.startsWith(kXbox360LiveryMagic)
        || bytes.mid(kInformationOffset, 4) != QByteArrayLiteral("lvry")
        || bytes.mid(kArtworkOffset, 4) != QByteArrayLiteral("lvyg")
        || qFromBigEndian<quint32>(bytes.constData() + 4) != kLiveryVersion
        || qFromBigEndian<quint32>(bytes.constData() + kInformationOffset + 4) != kGroupVersion) {
        throw std::runtime_error("invalid Xbox 360 livery header");
    }
    const quint32 length = qFromBigEndian<quint32>(bytes.constData() + kArtworkSizeOffset);
    const int counterSize = kLegacyLiveryCounterCount * sizeof(quint32);
    if (length < kArtworkHeaderSize
        || length > bytes.size() - kArtworkOffset - counterSize - kPaintSize
        || length > std::numeric_limits<int>::max() - kArtworkOffset - counterSize) {
        throw std::runtime_error("invalid Xbox 360 livery artwork length");
    }
    const int end = kArtworkOffset + static_cast<int>(length);

    return {end, end + counterSize};
}

std::optional<std::array<quint8, 4>> bodyPaint(const QByteArray &paint, int colorOffset) {
    if (paint[colorOffset - 2] == 1 && paint[colorOffset - 1] == 1) {
        auto color = colorFromArgb(qFromBigEndian<quint32>(paint.constData() + colorOffset));
        if (colorOffset == kMotorsport2PaintColorOffset) {
            color[3] = 255;
        }

        return color;
    }

    return std::nullopt;
}

void writeBodyPaint(QByteArray &paint, int record, const std::array<quint8, 4> &color, int colorOffset) {
    QByteArray argb;
    appendArgb(argb, color);
    const int offset = record * kPaintRecordSize + colorOffset;
    paint[offset - 2] = 1;
    paint[offset - 1] = 1;
    paint.replace(offset, argb.size(), argb);
}

QByteArray exportPaint(const Project &project, const QByteArray &source, VinylFormat format) {
    const auto color = project.liveryPaint.defaultCarColorBgra();
    QByteArray paint = source;
    const bool compact = format == VinylFormat::Motorsport2;
    const int colorOffset = compact ? kMotorsport2PaintColorOffset : kPaintColorOffset;
    if (paint.isEmpty()) {
        constexpr std::array<quint8, 4> kDefaultPaint = {128, 128, 128, 255};
        paint.fill('\0', compact ? kMotorsport2PaintSize : kPaintSize);
        if (compact) {
            for (int record = 0; record < kPaintRecordCount; ++record) {
                paint[record * kPaintRecordSize] = 1;
            }
        } else {
            for (int record = 0; record < kPaintRecordCount - 1; ++record) {
                paint[record * kPaintRecordSize + 30] = 1;
            }
            paint[6 * kPaintRecordSize + 1] = 1;
        }
        for (const int record : {0, 1, 2, 5}) {
            writeBodyPaint(paint, record, color.value_or(kDefaultPaint), colorOffset);
        }
        writeBodyPaint(paint, 3, kDefaultPaint, colorOffset);
    } else if (color && bodyPaint(paint, colorOffset) != color) {
        writeBodyPaint(paint, 0, *color, colorOffset);
    }

    return paint;
}

bool xbox360ShapeId(quint16 id) {
    const int row = id % 100;

    return (id >= 101 && id <= 2240 && row >= 1 && row <= 40) || (id & kDecalFlag);
}

void appendShapeFloat(QByteArray &bytes, double value) {
    const float encoded = static_cast<float>(value);
    if (!std::isfinite(encoded)) {
        throw std::runtime_error("invalid Xbox 360 shape placement");
    }
    appendBeFloat(bytes, encoded);
}

void appendMotorsport2Scalar(QByteArray &bytes, double value) {
    const double rounded = std::round(value);
    if (!std::isfinite(rounded) || rounded < std::numeric_limits<qint16>::min()
        || rounded > std::numeric_limits<qint16>::max()) {
        throw std::runtime_error("artwork placement is outside the FM2 vinyl range");
    }
    appendBeU16(bytes, std::bit_cast<quint16>(static_cast<qint16>(rounded)));
}

void appendShape(QByteArray &bytes, const scene::Shape &shape, const Matrix3 &adjustment,
                 VinylFormat format) {
    const VinylShapePlacement placement = vinylShapePlacement(shape, adjustment);
    if (shape.raster || !xbox360ShapeId(placement.shapeId)) {
        throw std::runtime_error("an artwork layer has no Xbox 360 vinyl ID");
    }
    appendBeU16(bytes, placement.shapeId);
    if (format == VinylFormat::Motorsport2) {
        appendMotorsport2Scalar(bytes, placement.rotation);
        appendMotorsport2Scalar(bytes, placement.x);
        appendMotorsport2Scalar(bytes, placement.y);
        appendShapeFloat(bytes, placement.scaleX);
        appendShapeFloat(bytes, placement.scaleY);
        appendMotorsport2Scalar(bytes, placement.skew * kMotorsport2SkewUnits);
    } else {
        for (const double value : {placement.rotation, placement.x, placement.y,
                                   placement.scaleX, placement.scaleY, placement.skew}) {
            appendShapeFloat(bytes, value);
        }
    }
    appendArgb(bytes, placement.color);
}

void appendPlacement(QByteArray &bytes) {
    appendBeFloat(bytes, 0.0f);
    appendBeFloat(bytes, 0.0f);
    appendBeFloat(bytes, 1.0f);
    appendBeFloat(bytes, 0.0f);
}

struct EncodedChildren {
    QByteArray bytes;
    int shapes = 0;
};

EncodedChildren encodeChildren(const scene::Group *group, const Matrix3 &adjustment,
                               ChildBlockKind kind, VinylFormat format, int depth = 0) {
    QByteArray bitmap;
    QByteArray body;
    EncodedChildren encoded;
    quint32 count = 0;
    const bool flat = format == VinylFormat::Motorsport2;
    if (depth >= kMaximumGroupDepth) {
        throw std::runtime_error("Xbox 360 vinyl group nesting is too deep");
    }
    if (group && group->visible && (group->flags & kGroupMaskFlag) == 0) {
        for (const auto &child : group->children) {
            if (!child->visible) {
                continue;
            }
            const bool nested = child->kind() == scene::LayerKind::Group;
            if (nested) {
                const auto children = encodeChildren(static_cast<const scene::Group *>(child.get()),
                    adjustment, ChildBlockKind::Group, format, depth + 1);
                if (children.shapes == 0) {
                    continue;
                }
                if (!flat) {
                    appendPlacement(body);
                }
                body.append(children.bytes);
                encoded.shapes += children.shapes;
            } else if (child->kind() == scene::LayerKind::Shape) {
                const auto &shape = static_cast<const scene::Shape &>(*child);
                if (shape.mask) {
                    continue;
                }
                appendShape(body, shape, adjustment, format);
                ++encoded.shapes;
            } else {
                continue;
            }
            if (flat) {
                continue;
            }
            if (count % 8 == 0) {
                bitmap.append('\0');
            }
            if (nested) {
                bitmap[count / 8] |= static_cast<char>(1u << (count % 8));
            }
            ++count;
        }
    }
    if (flat) {
        return {std::move(body), encoded.shapes};
    }
    if (kind == ChildBlockKind::Section) {
        appendBeU32(encoded.bytes, count);
    } else {
        constexpr quint32 kMaximumGroupChildren = 0xffffff;
        if (count > kMaximumGroupChildren) {
            throw std::runtime_error("too many direct children in Xbox 360 vinyl group");
        }
        encoded.bytes.append(static_cast<char>(count >> 16));
        appendBeU16(encoded.bytes, static_cast<quint16>(count));
    }
    appendBeU32(encoded.bytes, static_cast<quint32>(bitmap.size()));
    encoded.bytes.append(bitmap);
    encoded.bytes.append(body);

    return encoded;
}

QByteArray defaultLiveryHeader(const QByteArray &creator) {
    QByteArray header(kXbox360LiveryMagic);
    appendBeU32(header, kLiveryVersion);
    header.append(QByteArray(12, '\0'));
    appendBeU32(header, 0x01000000);
    header.append('\0');
    header.append("lvry");
    appendBeU32(header, kGroupVersion);
    header.append(creator);

    return header;
}

std::array<const scene::Group *, kBodyLiverySectionCount> liverySectionGroups(const Project &project) {
    std::array<const scene::Group *, kBodyLiverySectionCount> sections{};
    for (const auto &child : project.root->children) {
        if (child->kind() != scene::LayerKind::Group) {
            if (child->visible && child->kind() == scene::LayerKind::Shape) {
                throw std::runtime_error("livery artwork must belong to a section");
            }
            continue;
        }
        const auto &section = static_cast<const scene::Group &>(*child);
        const int slot = section.liverySectionSlot;
        if (!section.isLiverySection || slot < 0 || slot >= kFH6SectionCount) {
            throw std::runtime_error("livery artwork has invalid section metadata");
        }
        if (slot < kBodyLiverySectionCount) {
            if (sections[slot]) {
                throw std::runtime_error("duplicate Xbox 360 livery section");
            }
            sections[slot] = &section;
        }
    }

    return sections;
}

void validateSectionCount(int slot, int count, VinylFormat format) {
    constexpr std::array<int, kBodyLiverySectionCount> kMotorsport2Limits = {
        kMotorsport2BumperLimit, kMotorsport2BumperLimit, kSectionLimit,
        kSectionLimit, kSectionLimit, kSpoilerLimit
    };
    const int limit = format == VinylFormat::Motorsport2 ? kMotorsport2Limits[slot]
        : slot == kSpoilerSlot ? kSpoilerLimit : kSectionLimit;
    if ((kEnforceLiveryShapeLimits && count > limit)
        || (format == VinylFormat::Motorsport2 && count > std::numeric_limits<quint16>::max())) {
        throw std::runtime_error(QStringLiteral("%1 exceeds the Xbox 360 limit of %2 layers")
            .arg(QLatin1String(kFH6LiverySlots[slot].name)).arg(limit).toStdString());
    }
}

LiverySection makeSection(int slot, int offset) {
    LiverySection section;
    section.name = QString::fromLatin1(kFH6LiverySlots[slot].name);
    section.rotationDeg = kFH6LiverySlots[slot].rotationDeg;
    section.slot = slot;
    section.absPos = offset;

    return section;
}

Xbox360Livery readMotorsport2Livery(const QByteArray &bytes) {
    if (bytes.size() < kMotorsport2MinimumLiverySize || bytes.size() > kMotorsport2MaximumLiverySize) {
        throw std::runtime_error("invalid FM2 livery payload length");
    }
    BinaryCursor cursor = vinylCursor(bytes);
    Xbox360Livery livery;
    const int paintStart = bytes.size() - kMotorsport2PaintSize - kMotorsport2LiveryFooterSize;
    cursor.skip(kTagSize + sizeof(quint32));
    for (int slot = 0; slot < kBodyLiverySectionCount; ++slot) {
        LiverySection section = makeSection(slot, cursor.position());
        const int count = cursor.u16();
        readMotorsport2Shapes(cursor, section.subtree, count, paintStart);
        section.populated = count > 0;
        livery.sections.push_back(std::move(section));
        livery.sectionCounts.push_back(count);
    }
    if (cursor.position() != paintStart) {
        throw std::runtime_error("FM2 artwork does not match its declared layer counts");
    }
    if (const auto color = bodyPaint(bytes.mid(paintStart, kMotorsport2PaintSize), kMotorsport2PaintColorOffset)) {
        livery.paint.setDefaultCarColorBgra(*color);
    }
    livery.creatorName = motorsport2CreatorName(bytes);
    livery.paint.game = QStringLiteral("FM2");

    return livery;
}

QByteArray encodeMotorsport2Livery(const Project &project, const QByteArray &creator) {
    QByteArray payload(kMotorsport2LiveryMagic);
    QByteArray sourcePaint;
    QByteArray footer;
    const auto sections = liverySectionGroups(project);

    if (isMotorsport2Livery(project.liverySource)) {
        const QByteArray &source = project.liverySource;
        const int paintStart = source.size() - kMotorsport2PaintSize - kMotorsport2LiveryFooterSize;
        if (paintStart < kMotorsport2MinimumLiverySize - kMotorsport2PaintSize - kMotorsport2LiveryFooterSize) {
            throw std::runtime_error("invalid FM2 source paint data");
        }
        sourcePaint = source.mid(paintStart, kMotorsport2PaintSize);
        footer = source.right(kMotorsport2LiveryFooterSize);
    } else {
        footer.append('\x01');
        footer.append(creator);
    }
    appendBeU32(payload, kMotorsport2Version);
    for (int slot = 0; slot < kBodyLiverySectionCount; ++slot) {
        const auto section = encodeChildren(sections[slot], invertAffine(liverySectionCanvasTransform(slot)),
                                            ChildBlockKind::Section, VinylFormat::Motorsport2);
        validateSectionCount(slot, section.shapes, VinylFormat::Motorsport2);
        appendBeU16(payload, static_cast<quint16>(section.shapes));
        payload.append(section.bytes);
    }
    payload.append(exportPaint(project, sourcePaint, VinylFormat::Motorsport2));
    payload.append(footer);

    return payload;
}

QByteArray encodeLivery(const Project &project, const QByteArray &creator) {
    QByteArray header = defaultLiveryHeader(creator);
    QByteArray sourcePaint;
    QByteArray artwork;
    QByteArray counters;
    QVector<LiverySection> sourceSections;
    const auto sections = liverySectionGroups(project);
    if (project.liverySource.startsWith(kXbox360LiveryMagic)) {
        const LiveryLayout layout = liveryLayout(project.liverySource);
        header = project.liverySource.left(kArtworkSizeOffset);
        sourcePaint = project.liverySource.mid(layout.paintStart, kPaintSize);
        sourceSections = readXbox360Livery(project.liverySource).sections;
        artwork = project.liverySource.mid(kArtworkOffset, kArtworkHeaderSize);
        qToBigEndian(std::bit_cast<quint32>(1.0f), artwork.data() + kArtworkScaleOffset);
    } else {
        artwork.append(kXbox360GroupMagic);
        artwork.append(QByteArray(8, '\0'));
        appendBeFloat(artwork, 1.0f);
        artwork.append(QByteArray(3, '\0'));
    }
    if (project.carId <= 0) {
        throw std::runtime_error("choose an Xbox 360 car ID before exporting the livery");
    }
    qToBigEndian(static_cast<quint32>(project.carId), header.data() + 16);
    for (int slot = 0; slot < kBodyLiverySectionCount; ++slot) {
        if (slot > 0) {
            if (sourceSections.isEmpty()) {
                artwork.append(QByteArray(8, '\0'));
                appendBeFloat(artwork, 1.0f);
                appendBeFloat(artwork, slot == 4 ? 180.0f : 0.0f);
                artwork.chop(1);
            } else {
                artwork.append(project.liverySource.mid(
                    sourceSections[slot].absPos - kSectionBoundarySize, kSectionBoundarySize));
            }
        }
        const auto section = encodeChildren(sections[slot], invertAffine(liverySectionCanvasTransform(slot)),
                                            ChildBlockKind::Section, VinylFormat::Counted);
        validateSectionCount(slot, section.shapes, VinylFormat::Counted);
        artwork.append(section.bytes);
        appendBeU32(counters, section.shapes);
    }
    appendBeU32(counters, 0);
    appendBeU32(header, static_cast<quint32>(artwork.size()));

    return header + artwork + counters + exportPaint(project, sourcePaint, VinylFormat::Counted);
}

} // namespace

bool isMotorsport2Livery(const QByteArray &bytes) {
    return bytes.size() >= kTagSize + sizeof(quint32) && bytes.startsWith(kMotorsport2LiveryMagic)
        && qFromBigEndian<quint32>(bytes.constData() + kTagSize) == kMotorsport2Version;
}

bool isMotorsport2LiveryContainer(const QByteArray &bytes) {
    if (bytes.size() <= kMotorsport2ContainerHeaderSize
        || qFromBigEndian<quint32>(bytes.constData()) != bytes.size() - kMotorsport2ContainerHeaderSize) {
        return false;
    }
    const quint32 payloadSize = qFromBigEndian<quint32>(bytes.constData() + sizeof(quint32));

    return payloadSize >= kMotorsport2MinimumLiverySize && payloadSize <= kMotorsport2MaximumLiverySize
        && bytes.size() <= compressBound(kMotorsport2MaximumLiverySize) + kMotorsport2ContainerHeaderSize;
}

bool isMotorsport2Group(const QByteArray &bytes) {
    return bytes.size() >= kTagSize + sizeof(quint32) && bytes.startsWith(kXbox360GroupMagic)
        && qFromBigEndian<quint32>(bytes.constData() + kTagSize) == kMotorsport2Version;
}

bool isXbox360LiveryPayload(const QByteArray &bytes) {
    return bytes.startsWith(kXbox360LiveryMagic) || isMotorsport2Livery(bytes);
}

QString motorsport2CreatorName(const QByteArray &bytes) {
    if (bytes.size() < kMotorsport2MinimumLiverySize || !isMotorsport2Livery(bytes)) {
        return {};
    }
    const auto creatorId = qFromBigEndian<quint64>(bytes.constData() + bytes.size() - kProfileIdSize);
    if (creatorId == 0) {
        return {};
    }

    return QStringLiteral("%1").arg(creatorId, kProfileIdSize * 2, 16, QLatin1Char('0')).toUpper();
}

QByteArray inflateMotorsport2Livery(const QByteArray &bytes) {
    if (!isMotorsport2LiveryContainer(bytes)) {
        throw std::runtime_error("invalid FM2 livery container length");
    }
    const quint32 payloadSize = qFromBigEndian<quint32>(bytes.constData() + sizeof(quint32));
    QByteArray payload(payloadSize + 1, Qt::Uninitialized);
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("could not initialize FM2 livery decompression");
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.constData() + kMotorsport2ContainerHeaderSize));
    stream.avail_in = static_cast<uInt>(bytes.size() - kMotorsport2ContainerHeaderSize);
    stream.next_out = reinterpret_cast<Bytef *>(payload.data());
    stream.avail_out = static_cast<uInt>(payload.size());
    const int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END || stream.avail_in != 0 || stream.total_out != payloadSize) {
        throw std::runtime_error("invalid FM2 compressed livery payload");
    }
    payload.truncate(payloadSize);
    if (!isMotorsport2Livery(payload)) {
        throw std::runtime_error("invalid FM2 livery header");
    }

    return payload;
}

QByteArray deflateMotorsport2Livery(const QByteArray &payload) {
    if (!isMotorsport2Livery(payload) || payload.size() < kMotorsport2MinimumLiverySize
        || payload.size() > kMotorsport2MaximumLiverySize) {
        throw std::runtime_error("invalid FM2 livery payload for compression");
    }
    const quint32 payloadSize = static_cast<quint32>(payload.size());
    QByteArray bytes(kMotorsport2ContainerHeaderSize + compressBound(payloadSize), Qt::Uninitialized);
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                     MAX_MEM_LEVEL - 1, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("could not initialize FM2 livery compression");
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.constData()));
    stream.avail_in = payloadSize;
    stream.next_out = reinterpret_cast<Bytef *>(bytes.data() + kMotorsport2ContainerHeaderSize);
    stream.avail_out = static_cast<uInt>(bytes.size() - kMotorsport2ContainerHeaderSize);
    const int status = deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    if (status != Z_STREAM_END) {
        throw std::runtime_error("FM2 livery compression failed");
    }
    bytes.truncate(kMotorsport2ContainerHeaderSize + stream.total_out);
    qToBigEndian<quint32>(stream.total_out, bytes.data());
    qToBigEndian(payloadSize, bytes.data() + sizeof(quint32));

    return bytes;
}

Xbox360Livery readXbox360Livery(const QByteArray &bytes) {
    if (isMotorsport2Livery(bytes)) {
        return readMotorsport2Livery(bytes);
    }
    const LiveryLayout layout = liveryLayout(bytes);
    const QByteArray body = bytes.left(layout.artworkEnd);
    BinaryCursor artwork = vinylCursor(body);
    Xbox360Livery livery;
    livery.carId = static_cast<int>(qFromBigEndian<quint32>(bytes.constData() + 16));
    livery.locked = qFromBigEndian<quint32>(bytes.constData() + 8) == 1;
    if (const auto color = bodyPaint(bytes.mid(layout.paintStart, kPaintSize), kPaintColorOffset)) {
        livery.paint.setDefaultCarColorBgra(*color);
    }
    artwork.skip(kArtworkOffset + kArtworkScaleOffset);
    const float scale = artwork.f32();
    if (!std::isfinite(scale) || scale == 0.0f) {
        throw std::runtime_error("invalid Xbox 360 livery root scale");
    }
    artwork.skip(3);
    for (int slot = 0; slot < kBodyLiverySectionCount; ++slot) {
        if (slot > 0) {
            artwork.skip(kSectionBoundarySize);
        }
        LiverySection section = makeSection(slot, artwork.position());
        section.subtree.sx = section.subtree.sy = scale;
        const int count = readChildren(body, artwork, section.subtree, ChildBlockKind::Section);
        if (qFromBigEndian<quint32>(bytes.constData() + layout.artworkEnd + slot * 4) != quint32(count)) {
            throw std::runtime_error("Xbox 360 livery section counters do not match the artwork");
        }
        section.populated = count > 0;
        livery.sections.push_back(std::move(section));
        livery.sectionCounts.push_back(count);
    }
    if (artwork.position() != layout.artworkEnd) {
        throw std::runtime_error("Xbox 360 livery artwork does not match its declared length");
    }

    return livery;
}

VinylGroup readXbox360Group(const QByteArray &bytes) {
    BinaryCursor cursor = vinylCursor(bytes);
    VinylGroup root;
    if (!bytes.startsWith(kXbox360GroupMagic)) {
        throw std::runtime_error("invalid Xbox 360 vinyl group header");
    }
    cursor.skip(kTagSize);
    const quint32 version = cursor.u32();
    if (version != kGroupVersion && version != kMotorsport2Version) {
        throw std::runtime_error("unsupported Xbox 360 vinyl group version");
    }
    const bool compact = version == kMotorsport2Version;
    const quint32 count = compact ? cursor.u32() : 0;
    cursor.skip(sizeof(quint32));
    readPlacement(cursor, root);
    if (compact) {
        readMotorsport2Shapes(cursor, root, count, bytes.size());
    } else {
        readChildren(bytes, cursor, root, ChildBlockKind::Group);
    }
    const int paddingSize = bytes.size() - cursor.position();
    if (paddingSize >= 16) {
        throw std::runtime_error("Xbox 360 vinyl group has unexpected trailing data");
    }

    return root;
}

QByteArray encodeXbox360Vinyl(const Project &project, ExportGameVersion game, const QByteArray &creatorTag) {
    if (!project.root) {
        throw std::runtime_error("project has no vinyl artwork");
    }
    const bool compact = game == ExportGameVersion::Motorsport2;
    if (project.isLivery) {
        QByteArray creator = creatorTag.leftJustified(kProfileIdSize, '\0', true);
        if (compact) {
            std::reverse(creator.begin(), creator.end());
        }

        return compact ? encodeMotorsport2Livery(project, creator) : encodeLivery(project, creator);
    }
    QByteArray group(kXbox360GroupMagic);
    const auto children = encodeChildren(project.root.get(), Matrix3{}, ChildBlockKind::Group,
                                         compact ? VinylFormat::Motorsport2 : VinylFormat::Counted);
    appendBeU32(group, compact ? kMotorsport2Version : kGroupVersion);
    if (compact) {
        appendBeU32(group, static_cast<quint32>(children.shapes));
    }
    appendBeU32(group, 0);
    appendPlacement(group);
    group.append(children.bytes);
    if (compact) {
        group.append(QByteArray(kMotorsport2GroupFooterSize, '\0'));
    }

    return group;
}

} // namespace fls::detail
