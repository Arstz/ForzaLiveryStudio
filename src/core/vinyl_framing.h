#pragma once

#include <QByteArray>
#include <QVector>

#include <optional>

namespace fls::detail {

inline constexpr int kVinylBoundaryBytes = 17;
inline constexpr int kVinylChildHeaderBytes = 6;

struct VinylSpan {
    int start = 0;
    int end = 0;

    int size() const { return end - start; }
};

struct VinylAttributes {
    QByteArray identity;
    std::optional<double> scaleY;
    int size = 0;
    quint8 flags = 0;
};

struct VinylChildHeader {
    QByteArray bitmap;
    QByteArray controls;
    int count = 0;
    int size = 0;
};

struct VinylBoundary {
    QByteArray opaque = QByteArray(8, '\0');
    float scalar = 1.0f;
    float orientation = 0.0f;
    quint8 flags = 0;
};

struct VinylPhysicalGroup {
    VinylSpan span;
    VinylSpan attributes;
    VinylChildHeader children;
    QVector<VinylPhysicalGroup> groups;
    QVector<VinylSpan> shapes;
    int header = 0;
    int shapeCount = 0;
    int scalarBytes = 0;
    int leadBytes = 0;
    int flags = 0;
};

struct VinylStreamRecord {
    enum Kind { Children, Boundary, State, Prefix } kind = Children;
    VinylSpan span;
    std::optional<VinylPhysicalGroup> group;
};

struct VinylRecordStream {
    QVector<VinylStreamRecord> records;
    int end = 0;
    bool complete = false;
};

struct VinylSectionSpan {
    VinylSpan span;
    VinylSpan artwork;
    std::optional<VinylSpan> boundary;
    int shapeCount = 0;
};

int vinylBitmapBytes(int count);
bool vinylChildIsGroup(const QByteArray &bitmap, int child);
std::optional<VinylChildHeader> readVinylChildHeader(
    const QByteArray &data, int pos, int end, bool standaloneRoot = false);
QByteArray writeVinylChildHeader(int count, const QByteArray &bitmap,
                                std::optional<quint8> marker = std::nullopt,
                                bool standaloneRoot = false,
                                std::optional<int> storedBitmapBytes = std::nullopt);
std::optional<VinylAttributes> readVinylAttributes(const QByteArray &data, int pos, int end);
std::optional<VinylAttributes> readVinylScaleAttributes(const QByteArray &data, int pos, int end);
QByteArray writeVinylAttributes(const VinylAttributes &attributes);
std::optional<VinylBoundary> readVinylBoundary(const QByteArray &data, int pos, int end);
QByteArray writeVinylBoundary(const VinylBoundary &boundary);
VinylRecordStream readVinylRecordStream(const QByteArray &data, bool legacy);
VinylRecordStream readStandaloneVinylRecords(const QByteArray &data);
std::optional<VinylPhysicalGroup> readVinylChildBlock(const QByteArray &data, int pos, bool legacy);
QVector<VinylSectionSpan> mapVinylSections(const VinylRecordStream &stream,
                                         const QVector<int> &counts, int slotCount);

} // namespace fls::detail
