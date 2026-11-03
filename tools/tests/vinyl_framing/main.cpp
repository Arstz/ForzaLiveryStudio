#include "binary_io.h"
#include "livery_codec.h"
#include "vinyl_decoder.h"
#include "vinyl_framing.h"

#include <QtCore>

#include <algorithm>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

QByteArray shape(bool degenerate = false) {
    QByteArray bytes(1, '\x02');
    fls::detail::appendLeU16(bytes, 101);
    for (float value : {0.0f, 0.0f, 0.0f, degenerate ? 0.0f : 1.0f,
                       degenerate ? 0.0f : 1.0f, 0.0f}) {
        fls::detail::appendLeFloat(bytes, value);
    }
    bytes.append(QByteArray(4, '\xff'));
    return bytes;
}

void attributes() {
    for (quint8 flags : {0x00, 0x01, 0x10, 0x20, 0x21, 0x30, 0x31,
                         0x40, 0x60, 0x61, 0x70, 0x71, 0x80}) {
        QByteArray bytes(1, static_cast<char>(flags));
        const QByteArray identity = QByteArray::fromHex("1122334455667788");
        if (flags & 1) bytes.append(identity);
        if (flags & 0x10) fls::detail::appendLeFloat(bytes, -2.0f);
        const auto parsed = fls::detail::readVinylAttributes(bytes, 0, bytes.size());
        require(parsed.has_value(), "attribute presence bits were rejected");
        require(parsed->identity == ((flags & 1) ? identity : QByteArray()), "identity field extent");
        require(parsed->scaleY.has_value() == static_cast<bool>(flags & 0x10), "signed scale presence");
        require(fls::detail::writeVinylAttributes(*parsed) == bytes, "attribute byte preservation");
        require(!fls::detail::readVinylAttributes(bytes, 0, bytes.size() - 1), "truncated attributes accepted");
    }
}

void boundaryAndOccupancy() {
    QByteArray boundary = QByteArray::fromHex("6500000000000000");
    fls::detail::appendLeFloat(boundary, 1.0f);
    fls::detail::appendLeFloat(boundary, 90.0f);
    boundary.append('\x80');
    const auto state = fls::detail::readVinylBoundary(boundary, 0, boundary.size());
    require(state.has_value() && fls::detail::writeVinylBoundary(*state) == boundary,
            "opaque boundary fields were normalized");

    const QByteArray empty(6, '\0');
    const QByteArray populated = QByteArray::fromHex("02000100000000") + shape() + shape(true) + '\0';
    for (const QByteArray &body : {empty + boundary + populated, boundary + empty + populated}) {
        const auto stream = fls::detail::readVinylRecordStream(body, false);
        const auto spans = fls::detail::mapVinylSections(stream, {0, 2}, 2);
        require(stream.complete && spans.size() == 2 && spans[1].shapeCount == 2,
                "physical occupancy depends on editable geometry");
        const auto sections = fls::VinylTreeDecoder().buildLiverySections(
            body, {0, 2}, fls::kFH6LiverySlots, 2);
        require(sections[1].subtree.totalChildren() == 1, "section root framing");
        const auto &group = *std::get<fls::VinylGroupPtr>(sections[1].subtree.items.front().value);
        require(group.items.size() == 1 && group.skippedChildren == 1,
                "degenerate artwork changed editable geometry or physical occupancy");
        require(!fls::detail::mapVinylSections(stream, {0, 0, 2}, 3).size(),
                "ambiguous section mapping was silently selected");
        require(!fls::detail::readVinylRecordStream(body.chopped(2), false).complete,
                "truncated child payload accepted");
    }
}

void rootBitmap() {
    constexpr int count = 2049;
    const QByteArray bitmap(fls::detail::vinylBitmapBytes(count), '\0');
    const QByteArray root = fls::detail::writeVinylChildHeader(count, bitmap, 0x20, true, 255);
    const auto header = fls::detail::readVinylChildHeader(root, 1, root.size(), true);
    require(header && header->count == count && header->bitmap.size() == 255,
            "standalone bitmap extent was derived from child count");
    const QByteArray nested = fls::detail::writeVinylChildHeader(count, bitmap);
    const auto nestedHeader = fls::detail::readVinylChildHeader(nested, 0, nested.size());
    require(nestedHeader && nestedHeader->bitmap.size() == bitmap.size(), "nested bitmap extent");
    const QByteArray complete = fls::detail::writeVinylChildHeader(count, bitmap, 0x20, true);
    for (const QByteArray &headerBytes : {root, complete}) {
        QByteArray payload("gyvl", 4);
        fls::detail::appendLeU32(payload, 1);
        fls::detail::appendLeU32(payload, 0);
        payload.append('\x03');
        for (float value : {0.0f, 0.0f, 1.0f, 0.0f}) fls::detail::appendLeFloat(payload, value);
        payload.append(headerBytes);
        payload.append(shape().repeated(count));
        payload.append(QByteArray::fromHex("0001"));
        const auto decoded = fls::VinylTreeDecoder().decodeGroup(payload);
        require(decoded.items.size() == count && decoded.expectedChildren == count,
                "root bitmap storage changed decoded child count");
        require(fls::detail::readStandaloneVinylRecords(payload).complete, "encoded root framing");
    }
    QByteArray invalid = bitmap;
    invalid[255] = '\1';
    bool rejected = false;
    try {
        fls::detail::writeVinylChildHeader(count, invalid, 0x20, true, 255);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "group type bit was discarded from capped root bitmap");
}

void verifyGroup(const fls::detail::VinylPhysicalGroup &group) {
    require(group.children.count == group.groups.size() + group.shapes.size(), "direct child count mismatch");
    for (const auto &nested : group.groups) {
        require(nested.span.start >= group.header && nested.span.end <= group.span.end, "child span outside parent");
        verifyGroup(nested);
    }
}

void verifyStream(const fls::detail::VinylRecordStream &stream, const QByteArray &bytes) {
    require(stream.complete && stream.end == bytes.size(), "incomplete physical stream");
    QByteArray assembled;
    int end = 0;
    for (const auto &record : stream.records) {
        require(record.span.start == end, "physical record gap or overlap");
        assembled.append(bytes.mid(record.span.start, record.span.size()));
        end = record.span.end;
        if (record.group) verifyGroup(*record.group);
    }
    require(assembled == bytes, "physical byte reassembly differs");
}

void fixtures(const QStringList &roots) {
    int groups = 0;
    int liveries = 0;
    int unmapped = 0;
    for (const auto &root : roots) {
        require(QDir(root).exists(), "fixture root is missing");
        QDirIterator files(root, {"C_group", "C_livery"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            QFile file(path);
            require(file.open(QIODevice::ReadOnly), "cannot read fixture");
            const QByteArray wrapped = file.readAll();
            QByteArray compressed = wrapped.mid(4, 4);
            std::reverse(compressed.begin(), compressed.end());
            compressed.append(wrapped.mid(8, fls::detail::readLeU32(wrapped, 0)));
            const QByteArray raw = qUncompress(compressed);
            require(!raw.isEmpty(), "fixture inflate failed");
            try {
                if (QFileInfo(path).fileName() == "C_group") {
                    verifyStream(fls::detail::readStandaloneVinylRecords(raw), raw);
                    ++groups;
                } else {
                    const auto payload = fls::parseInflatedLiveryPayload(raw);
                    const auto stream = fls::detail::readVinylRecordStream(payload.body, payload.legacyRecords);
                    verifyStream(stream, payload.body);
                    const int slotCount = std::min(static_cast<int>(payload.sectionCounts.size()), 11);
                    if (fls::detail::mapVinylSections(stream, payload.sectionCounts, slotCount).isEmpty()) ++unmapped;
                    ++liveries;
                }
            } catch (const std::exception &) {
                qCritical().noquote() << path;
                throw;
            }
        }
    }
    qInfo() << "physical fixtures:" << groups << "groups," << liveries << "liveries," << unmapped << "unresolved section mappings";
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        attributes();
        boundaryAndOccupancy();
        rootBitmap();
        if (app.arguments().size() > 1) fixtures(app.arguments().mid(1));
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }
    return 0;
}
