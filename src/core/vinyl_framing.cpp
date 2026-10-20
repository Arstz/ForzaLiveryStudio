#include "vinyl_framing.h"

#include "binary_io.h"
#include "shape_registry.h"
#include "vinyl_decoder.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace fls::detail {
namespace {

constexpr int kMaximumDepth = 200;
constexpr int kMaximumControls = 32;
constexpr int kMaximumChildren = 0xffff;
constexpr quint8 kIdentityPresent = 0x01;
constexpr quint8 kScaleYPresent = 0x10;

bool contains(const QByteArray &data, int pos, int size, int end) {
    return pos >= 0 && size >= 0 && pos <= end && size <= end - pos
        && end <= data.size();
}

bool validScale(double scale) {
    return std::isfinite(scale) && std::abs(scale) >= 0.0001 && std::abs(scale) <= 5000.0;
}

bool placementAt(const QByteArray &data, int pos, int size) {
    if (!contains(data, pos, size, data.size())) {
        return false;
    }
    const double x = readLeFloat(data, pos);
    const double y = readLeFloat(data, pos + 4);
    const double scale = readLeFloat(data, pos + 8);

    return std::abs(x) < 50000.0 && std::abs(y) < 50000.0
        && validScale(scale) && std::abs(scale) <= 200.0
        && (size == 12 || std::abs(readLeFloat(data, pos + 12)) <= 10000.0);
}

class RecordReader {
public:
    RecordReader(const QByteArray &data, bool legacy, quint8 terminator = 0)
        : data_(data), marker_(legacy ? 1 : 2), terminator_(terminator) {}

    std::optional<VinylPhysicalGroup> childBlock(int pos) {
        return counted(pos, 0);
    }

    VinylRecordStream standalone() {
        VinylRecordStream stream;
        const auto root = transformed(12, 0);
        if (!root) {
            return stream;
        }
        stream.records.push_back({VinylStreamRecord::Prefix, {0, 12}, {}});
        stream.records.push_back({VinylStreamRecord::Children, root->span, root});
        stream.end = root->span.end;
        while (stream.end < data_.size() && (data_[stream.end] == '\0' || data_[stream.end] == '\1')) {
            ++stream.end;
        }
        if (stream.end > root->span.end) {
            stream.records.push_back({VinylStreamRecord::State, {root->span.end, stream.end}, {}});
        }
        stream.complete = stream.end == data_.size();

        return stream;
    }

    VinylRecordStream read() {
        VinylRecordStream stream;
        while (stream.end < data_.size()) {
            const int pos = stream.end;
            if (readVinylBoundary(data_, pos, data_.size())) {
                stream.end += kVinylBoundaryBytes;
                stream.records.push_back({VinylStreamRecord::Boundary, {pos, stream.end}, {}});
                continue;
            }
            const auto group = counted(pos, 0);
            if (!group) {
                break;
            }
            stream.end = group->span.end;
            stream.records.push_back({VinylStreamRecord::Children, group->span, group});
            if (marker_ == 2 && group->children.count > 0 && stream.end < data_.size()
                && (data_[stream.end] == '\0' || data_[stream.end] == '\1')) {
                ++stream.end;
                stream.records.push_back({VinylStreamRecord::State, {stream.end - 1, stream.end}, {}});
            }
        }
        stream.complete = stream.end == data_.size();

        return stream;
    }

private:
    std::optional<VinylSpan> shape(int pos) const {
        for (int skip = 0; skip <= kMaximumControls && pos + skip < data_.size(); ++skip) {
            const int start = pos + skip;
            for (const int lead : {2, 1}) {
                if (!contains(data_, start, lead + 30, data_.size())) {
                    continue;
                }
                if (lead == 2 ? ((data_[start] != '\0' && data_[start] != '\1')
                                 || static_cast<quint8>(data_[start + 1]) != marker_)
                              : static_cast<quint8>(data_[start]) != marker_) {
                    continue;
                }
                const quint16 id = readLeU16(data_, start + lead);
                if (id == 0 || (!isKnownShapeId(canonicalShapeId(id)) && id < 32768)) {
                    continue;
                }
                const int fields = start + lead + 2;
                bool finite = true;
                for (int field = 0; field < 6; ++field) {
                    finite = finite && std::isfinite(readLeFloat(data_, fields + field * 4));
                }
                const double sx = readLeFloat(data_, fields + 12);
                const double sy = readLeFloat(data_, fields + 16);
                const double skew = readLeFloat(data_, fields + 20);
                const bool degenerate = sx == 0.0 && sy == 0.0 && skew == 0.0;
                if (finite && (degenerate || isValidVinylShapeBasis(sx, sy, skew))
                    && std::abs(readLeFloat(data_, fields)) <= 10000.0
                    && std::abs(readLeFloat(data_, fields + 4)) < 50000.0
                    && std::abs(readLeFloat(data_, fields + 8)) < 50000.0) {
                    return VinylSpan{start, start + lead + 30};
                }
            }
            if (data_[start] != '\0' && data_[start] != '\1') {
                break;
            }
        }

        return {};
    }

    QVector<QPair<int, int>> successors(int pos) const {
        QVector<QPair<int, int>> result;
        if (const auto attributes = readVinylAttributes(data_, pos, data_.size())) {
            const int next = pos + attributes->size;
            result.push_back({next, attributes->flags});
            if (next < data_.size() && (data_[next] == '\x20' || data_[next] == '\x60')) {
                result.push_back({next + 1, attributes->flags | static_cast<quint8>(data_[next])});
            }
        }
        result.push_back({pos, -1});

        return result;
    }

    std::optional<VinylPhysicalGroup> transformed(int pos, int depth) {
        if (depth > kMaximumDepth || pos < 0 || pos >= data_.size()) {
            return {};
        }
        if (cache_.contains(pos)) {
            return cache_.value(pos);
        }
        QVector<int> leads = marker_ == 1 ? QVector<int>{0, 1} : QVector<int>{1, 0};
        int cursor = pos;
        while (cursor < data_.size() && cursor - pos < kMaximumControls
               && (data_[cursor] == '\0' || data_[cursor] == '\1')) {
            ++cursor;
            if (cursor - pos > 1) {
                leads.push_back(cursor - pos);
            }
        }
        if (terminator_ != 0) {
            leads.clear();
            if ((static_cast<quint8>(data_[pos]) & 1) && pos + 1 < data_.size()
                && static_cast<quint8>(data_[pos + 1]) == terminator_) {
                leads.push_back(2);
                if (pos + 2 < data_.size() && static_cast<quint8>(data_[pos + 2]) == terminator_) {
                    leads.push_back(3);
                }
            }
            for (int prefix = 0; prefix <= kMaximumControls && pos + prefix < data_.size(); ++prefix) {
                if (static_cast<quint8>(data_[pos + prefix]) == terminator_) {
                    leads.push_back(prefix + 1);
                }
                if (data_[pos + prefix] != '\0' && data_[pos + prefix] != '\1') {
                    break;
                }
            }
            std::sort(leads.begin(), leads.end(), std::greater<int>());
            leads.push_back(0);
        }
        for (const int scalarBytes : {16, 12}) {
            for (const int lead : leads) {
                if (!placementAt(data_, pos + lead, scalarBytes)) {
                    continue;
                }
                for (const auto &[header, flags] : successors(pos + lead + scalarBytes)) {
                    if (auto group = counted(header, depth)) {
                        group->span.start = pos;
                        group->attributes = {pos, header};
                        group->flags = flags;
                        group->scalarBytes = scalarBytes;
                        group->leadBytes = lead;
                        cache_.insert(pos, group);

                        return group;
                    }
                }
            }
        }
        for (const auto &[header, flags] : successors(pos)) {
            if (auto group = counted(header, depth)) {
                group->span.start = pos;
                group->attributes = {pos, header};
                group->flags = flags;
                cache_.insert(pos, group);

                return group;
            }
        }
        cache_.insert(pos, {});

        return {};
    }

    std::optional<VinylPhysicalGroup> counted(int pos, int depth) {
        if (depth > kMaximumDepth) {
            return {};
        }
        const auto header = readVinylChildHeader(data_, pos, data_.size(), terminator_ != 0 && depth == 0);
        if (!header || header->controls != QByteArray(2, '\0')) {
            return {};
        }
        VinylPhysicalGroup group;
        group.span.start = pos;
        group.header = pos;
        group.children = *header;
        int cursor = pos + header->size;
        if (header->count > 0 && !vinylChildIsGroup(header->bitmap, 0) && !shape(cursor)) {
            for (const int lead : {1, 0}) {
                if (!placementAt(data_, cursor + lead, 16)) {
                    continue;
                }
                int next = cursor + lead + 16;
                if (const auto attributes = readVinylAttributes(data_, next, data_.size())) {
                    if (attributes->scaleY) {
                        next += attributes->size;
                    }
                }
                if (shape(next)) {
                    cursor = next;
                    break;
                }
            }
        }
        for (int child = 0; child < header->count; ++child) {
            if (vinylChildIsGroup(header->bitmap, child)) {
                const auto nested = transformed(cursor, depth + 1);
                if (!nested) {
                    return {};
                }
                group.groups.push_back(*nested);
                group.shapeCount += nested->shapeCount;
                cursor = nested->span.end;
            } else {
                const auto record = shape(cursor);
                if (!record) {
                    return {};
                }
                group.shapes.push_back(*record);
                ++group.shapeCount;
                cursor = record->end;
            }
        }
        group.span.end = cursor;

        return group;
    }

    const QByteArray &data_;
    QHash<int, std::optional<VinylPhysicalGroup>> cache_;
    quint8 marker_;
    quint8 terminator_;
};

} // namespace

int vinylBitmapBytes(int count) {
    return (count + 7) / 8;
}

bool vinylChildIsGroup(const QByteArray &bitmap, int child) {
    return child >= 0 && child / 8 < bitmap.size()
        && (static_cast<quint8>(bitmap[child / 8]) & (1 << (child % 8)));
}

std::optional<VinylChildHeader> readVinylChildHeader(
    const QByteArray &data, int pos, int end, bool standaloneRoot) {
    if (!contains(data, pos, kVinylChildHeaderBytes, end)) {
        return {};
    }
    VinylChildHeader header;
    header.count = readLeU16(data, pos);
    const int stored = readLeU16(data, pos + 2);
    if ((!standaloneRoot && stored != vinylBitmapBytes(header.count))
        || !contains(data, pos, kVinylChildHeaderBytes + stored, end)) {
        return {};
    }
    header.size = kVinylChildHeaderBytes + stored;
    header.controls = data.mid(pos + 4, 2);
    header.bitmap = data.mid(pos + kVinylChildHeaderBytes, stored);
    if (header.count % 8 != 0 && stored == vinylBitmapBytes(header.count) && stored > 0
        && (static_cast<quint8>(header.bitmap.back()) >> (header.count % 8))) {
        return {};
    }

    return header;
}

QByteArray writeVinylChildHeader(int count, const QByteArray &bitmap,
                                std::optional<quint8> marker, bool standaloneRoot,
                                std::optional<int> storedBitmapBytes) {
    if (count < 0 || count > kMaximumChildren || bitmap.size() != vinylBitmapBytes(count)) {
        throw std::runtime_error("invalid vinyl child bitmap");
    }
    QByteArray out;
    if (marker) {
        out.append(static_cast<char>(*marker));
    }
    appendLeU16(out, static_cast<quint16>(count));
    if (standaloneRoot) {
        const int stored = storedBitmapBytes.value_or(bitmap.size());
        if (stored < 0 || stored > bitmap.size()) {
            throw std::runtime_error("invalid stored root bitmap extent");
        }
        for (int child = stored * 8; child < count; ++child) {
            if (vinylChildIsGroup(bitmap, child)) {
                throw std::runtime_error("standalone root group exceeds its stored bitmap");
            }
        }
        appendLeU16(out, static_cast<quint16>(stored));
        out.append(QByteArray(2, '\0'));
        out.append(bitmap.left(stored));
    } else {
        appendLeU16(out, static_cast<quint16>(bitmap.size()));
        out.append(QByteArray(2, '\0'));
        out.append(bitmap);
    }

    return out;
}

std::optional<VinylAttributes> readVinylAttributes(const QByteArray &data, int pos, int end) {
    if (!contains(data, pos, 1, end)) {
        return {};
    }
    VinylAttributes attributes;
    attributes.flags = static_cast<quint8>(data[pos]);
    if (attributes.flags & ~0xf1) {
        return {};
    }
    attributes.size = 1 + ((attributes.flags & kIdentityPresent) ? 8 : 0)
        + ((attributes.flags & kScaleYPresent) ? 4 : 0);
    if (!contains(data, pos, attributes.size, end)) {
        return {};
    }
    if (attributes.flags & kIdentityPresent) {
        attributes.identity = data.mid(pos + 1, 8);
    }
    if (attributes.flags & kScaleYPresent) {
        const double scale = readLeFloat(data, pos + attributes.size - 4);
        if (!validScale(scale)) {
            return {};
        }
        attributes.scaleY = scale;
    }

    return attributes;
}

std::optional<VinylAttributes> readVinylScaleAttributes(const QByteArray &data, int pos, int end) {
    if (!contains(data, pos, 1, end)
        || (static_cast<quint8>(data[pos]) & ~0x40) != 0x30) {
        return {};
    }

    return readVinylAttributes(data, pos, end);
}

QByteArray writeVinylAttributes(const VinylAttributes &attributes) {
    if ((attributes.flags & ~0xf1)
        || ((attributes.flags & kIdentityPresent) ? attributes.identity.size() != 8 : !attributes.identity.isEmpty())
        || static_cast<bool>(attributes.flags & kScaleYPresent) != attributes.scaleY.has_value()
        || (attributes.scaleY && !validScale(*attributes.scaleY))) {
        throw std::runtime_error("invalid vinyl group attributes");
    }
    QByteArray out(1, static_cast<char>(attributes.flags));
    out.append(attributes.identity);
    if (attributes.scaleY) {
        appendLeFloat(out, static_cast<float>(*attributes.scaleY));
    }

    return out;
}

std::optional<VinylBoundary> readVinylBoundary(const QByteArray &data, int pos, int end) {
    if (!contains(data, pos, kVinylBoundaryBytes, end)) {
        return {};
    }
    VinylBoundary boundary;
    boundary.opaque = data.mid(pos, 8);
    boundary.scalar = readLeFloat(data, pos + 8);
    boundary.orientation = readLeFloat(data, pos + 12);
    boundary.flags = static_cast<quint8>(data[pos + 16]);
    if (boundary.scalar != 1.0f || !std::isfinite(boundary.orientation)
        || std::abs(boundary.orientation) > 10000.0f) {
        return {};
    }

    return boundary;
}

QByteArray writeVinylBoundary(const VinylBoundary &boundary) {
    if (boundary.opaque.size() != 8 || boundary.scalar != 1.0f
        || !std::isfinite(boundary.orientation) || std::abs(boundary.orientation) > 10000.0f) {
        throw std::runtime_error("invalid vinyl boundary state");
    }
    QByteArray out = boundary.opaque;
    appendLeFloat(out, boundary.scalar);
    appendLeFloat(out, boundary.orientation);
    out.append(static_cast<char>(boundary.flags));

    return out;
}

VinylRecordStream readVinylRecordStream(const QByteArray &data, bool legacy) {
    return RecordReader(data, legacy).read();
}

VinylRecordStream readStandaloneVinylRecords(const QByteArray &data) {
    if (data.size() <= 12 || !data.startsWith("gyvl")) {
        return {};
    }
    const quint8 generation = static_cast<quint8>(data[12]);
    if (generation < 1 || generation > 3) {
        return {};
    }

    return RecordReader(data, generation < 3, generation).standalone();
}

std::optional<VinylPhysicalGroup> readVinylChildBlock(const QByteArray &data, int pos, bool legacy) {
    return RecordReader(data, legacy).childBlock(pos);
}

QVector<VinylSectionSpan> mapVinylSections(const VinylRecordStream &stream,
                                         const QVector<int> &counts, int slotCount) {
    if (!stream.complete || slotCount < 0) {
        return {};
    }
    QVector<VinylSectionSpan> sections;
    const auto kindAt = [&](int index, VinylStreamRecord::Kind kind) {
        return index < stream.records.size() && stream.records[index].kind == kind;
    };
    std::function<bool(int, int)> map = [&](int index, int slot) {
        if (slot == slotCount) {
            return index == stream.records.size();
        }
        if (index >= stream.records.size()) {
            return false;
        }
        const bool populated = slot < counts.size() && counts[slot] > 0;
        for (const int prefix : {0, 1}) {
            const int child = index + prefix;
            if ((prefix && !kindAt(index, VinylStreamRecord::Boundary))
                || !kindAt(child, VinylStreamRecord::Children)) {
                continue;
            }
            const auto &record = stream.records[child];
            if (populated != (record.group->children.count > 0)) {
                continue;
            }
            int terminal = child + 1;
            if (populated && kindAt(terminal, VinylStreamRecord::State)) {
                ++terminal;
            }
            for (const int suffix : {1, 0}) {
                if ((suffix && !kindAt(terminal, VinylStreamRecord::Boundary)) || (prefix && suffix)) {
                    continue;
                }
                const int next = terminal + suffix;
                VinylSectionSpan section;
                section.span = {stream.records[index].span.start, stream.records[next - 1].span.end};
                section.artwork = record.span;
                section.shapeCount = record.group->shapeCount;
                if (prefix || suffix) {
                    section.boundary = stream.records[prefix ? index : terminal].span;
                }
                sections.push_back(section);
                if (map(next, slot + 1)) {
                    return true;
                }
                sections.removeLast();
            }
        }

        return false;
    };
    if (!map(0, 0)) {
        return {};
    }

    return sections;
}

} // namespace fls::detail
