#include "xbox360_vinyl.h"

#include "binary_io.h"
#include "layer.h"
#include "livery_codec.h"
#include "matrix_math.h"
#include "vinyl_encoding.h"

#include <QtEndian>

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

enum class ChildBlockKind { Group, Section };

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

VinylShape readShape(BinaryCursor &cursor) {
    VinylShape shape;
    shape.absPos = cursor.position();
    shape.shapeId = cursor.u16();
    shape.rotation = cursor.f32();
    shape.posX = cursor.f32();
    shape.posY = cursor.f32();
    shape.scaleX = cursor.f32();
    shape.scaleY = cursor.f32();
    shape.skew = cursor.f32();
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
            group.items.push_back({readShape(cursor)});
            ++shapes;
        }
    }

    return shapes;
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

std::optional<std::array<quint8, 4>> bodyPaint(const QByteArray &paint) {
    if (paint[1] == 1 && paint[2] == 1) {
        return colorFromArgb(qFromBigEndian<quint32>(paint.constData() + kPaintColorOffset));
    }

    return std::nullopt;
}

void writeBodyPaint(QByteArray &paint, int record, const std::array<quint8, 4> &color) {
    QByteArray argb;
    appendArgb(argb, color);
    const int offset = record * kPaintRecordSize;
    paint[offset + 1] = 1;
    paint[offset + 2] = 1;
    paint.replace(offset + kPaintColorOffset, argb.size(), argb);
}

QByteArray exportPaint(const Project &project, const QByteArray &source) {
    const auto color = project.liveryPaint.defaultCarColorBgra();
    QByteArray paint = source;
    if (paint.isEmpty()) {
        constexpr std::array<quint8, 4> kDefaultPaint = {128, 128, 128, 255};
        paint.fill('\0', kPaintSize);
        for (int record = 0; record < kPaintRecordCount - 1; ++record) {
            paint[record * kPaintRecordSize + 30] = 1;
        }
        for (const int record : {0, 1, 2, 5}) {
            writeBodyPaint(paint, record, color.value_or(kDefaultPaint));
        }
        writeBodyPaint(paint, 3, kDefaultPaint);
        paint[6 * kPaintRecordSize + 1] = 1;
    } else if (color && bodyPaint(paint) != color) {
        writeBodyPaint(paint, 0, *color);
    }

    return paint;
}

bool xbox360ShapeId(quint16 id) {
    const int row = id % 100;

    return (id >= 101 && id <= 2240 && row >= 1 && row <= 40) || (id & kDecalFlag);
}

void appendShape(QByteArray &bytes, const scene::Shape &shape, const Matrix3 &adjustment) {
    const VinylShapePlacement placement = vinylShapePlacement(shape, adjustment);
    if (shape.raster || !xbox360ShapeId(placement.shapeId)) {
        throw std::runtime_error("an artwork layer has no Xbox 360 vinyl ID");
    }
    appendBeU16(bytes, placement.shapeId);
    for (const double value : {placement.rotation, placement.x, placement.y,
                               placement.scaleX, placement.scaleY, placement.skew}) {
        const float encoded = static_cast<float>(value);
        if (!std::isfinite(encoded)) {
            throw std::runtime_error("invalid Xbox 360 shape placement");
        }
        appendBeFloat(bytes, encoded);
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
                               ChildBlockKind kind, int depth = 0) {
    QByteArray bitmap;
    QByteArray body;
    EncodedChildren encoded;
    quint32 count = 0;
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
                    adjustment, ChildBlockKind::Group, depth + 1);
                if (children.shapes == 0) {
                    continue;
                }
                appendPlacement(body);
                body.append(children.bytes);
                encoded.shapes += children.shapes;
            } else if (child->kind() == scene::LayerKind::Shape) {
                const auto &shape = static_cast<const scene::Shape &>(*child);
                if (shape.mask) {
                    continue;
                }
                appendShape(body, shape, adjustment);
                ++encoded.shapes;
            } else {
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
    header.append(creator.leftJustified(kProfileIdSize, '\0', true));

    return header;
}

QByteArray encodeLivery(const Project &project, const QByteArray &creator) {
    QByteArray header = defaultLiveryHeader(creator);
    QByteArray sourcePaint;
    QByteArray artwork;
    QByteArray counters;
    QVector<LiverySection> sourceSections;
    std::array<const scene::Group *, kBodyLiverySectionCount> sections{};
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
                                            ChildBlockKind::Section);
        const int limit = slot == kSpoilerSlot ? kSpoilerLimit : kSectionLimit;
        if (kEnforceLiveryShapeLimits && section.shapes > limit) {
            throw std::runtime_error(QStringLiteral("%1 exceeds the Xbox 360 limit of %2 layers")
                .arg(QLatin1String(kFH6LiverySlots[slot].name)).arg(limit).toStdString());
        }
        artwork.append(section.bytes);
        appendBeU32(counters, section.shapes);
    }
    appendBeU32(counters, 0);
    appendBeU32(header, static_cast<quint32>(artwork.size()));

    return header + artwork + counters + exportPaint(project, sourcePaint);
}

} // namespace

Xbox360Livery readXbox360Livery(const QByteArray &bytes) {
    const LiveryLayout layout = liveryLayout(bytes);
    const QByteArray body = bytes.left(layout.artworkEnd);
    BinaryCursor artwork = vinylCursor(body);
    Xbox360Livery livery;
    livery.carId = static_cast<int>(qFromBigEndian<quint32>(bytes.constData() + 16));
    livery.locked = qFromBigEndian<quint32>(bytes.constData() + 8) == 1;
    if (const auto color = bodyPaint(bytes.mid(layout.paintStart, kPaintSize))) {
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
        LiverySection section;
        section.name = QString::fromLatin1(kFH6LiverySlots[slot].name);
        section.rotationDeg = kFH6LiverySlots[slot].rotationDeg;
        section.slot = slot;
        section.absPos = artwork.position();
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
    cursor.skip(4);
    if (cursor.u32() != kGroupVersion) {
        throw std::runtime_error("unsupported Xbox 360 vinyl group version");
    }
    cursor.skip(4);
    readPlacement(cursor, root);
    readChildren(bytes, cursor, root, ChildBlockKind::Group);
    const int paddingSize = bytes.size() - cursor.position();
    if (paddingSize >= 16) {
        throw std::runtime_error("Xbox 360 vinyl group has unexpected trailing data");
    }

    return root;
}

QByteArray encodeXbox360Vinyl(const Project &project, const QByteArray &creator) {
    if (!project.root) {
        throw std::runtime_error("project has no vinyl artwork");
    }
    if (project.isLivery) {
        return encodeLivery(project, creator);
    }
    QByteArray group(kXbox360GroupMagic);
    appendBeU32(group, kGroupVersion);
    appendBeU32(group, 0);
    appendPlacement(group);
    group.append(encodeChildren(project.root.get(), Matrix3{}, ChildBlockKind::Group).bytes);

    return group;
}

} // namespace fls::detail
