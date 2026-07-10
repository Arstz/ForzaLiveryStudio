#pragma once

// Low-level reader for ForzaTech "Grub" bundle files (`.modelbin`). Ported from
// ForzaTechStudio's `ForzaTools.Bundles` (Bundle.cs / BundleBlob.cs). Only the
// blob structure, per-blob metadata (Id / Name / BBox), and raw blob payloads are
// decoded here; geometry interpretation lives in `model_geometry`.
//
// Endianness note: bundle tags are stored as reversed ASCII in the file (the file
// magic is "burG"), so a little-endian u32 read reconstructs the readable constant
// (e.g. 0x47727562 "Grub", 0x4D6F646C "Modl"). All integer fields are little-endian.

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace fh6 {

// Bundle + blob + metadata tags (little-endian u32 of the reversed on-disk bytes),
// matching ForzaTools.Bundles constants.
namespace bundle_tags {
constexpr quint32 Bundle = 0x47727562;        // "Grub"
constexpr quint32 Model = 0x4D6F646C;          // "Modl"
constexpr quint32 Mesh = 0x4D657368;           // "Mesh"
constexpr quint32 IndexBuffer = 0x496E6442;    // "IndB"
constexpr quint32 VertexLayout = 0x564C6179;   // "VLay"
constexpr quint32 InstancedVertexLayout = 0x494C6179; // "ILay"
constexpr quint32 VertexBuffer = 0x56657242;   // "VerB"
constexpr quint32 Skeleton = 0x536B656C;       // "Skel"

constexpr quint32 MetaName = 0x4E616D65;       // "Name"
constexpr quint32 MetaIdentifier = 0x49642020; // "Id  "
constexpr quint32 MetaBBox = 0x42426F78;       // "BBox"
} // namespace bundle_tags

// One decoded blob: its tag, wire version, raw payload bytes, and the handful of
// metadata values downstream geometry decoding needs.
struct BundleBlobRecord {
    quint32 tag = 0;
    quint8 versionMajor = 0;
    quint8 versionMinor = 0;
    QByteArray data;                 // raw uncompressed blob payload
    std::optional<quint32> id;       // "Id  " metadata, when present
    QString name;                    // "Name" metadata, when present
    std::optional<std::array<float, 6>> bbox; // BBox metadata: minXYZ, maxXYZ

    bool isAtLeastVersion(quint8 major, quint8 minor) const
    {
        return versionMajor > major || (versionMajor == major && versionMinor >= minor);
    }
};

struct ModelBundle {
    quint8 versionMajor = 0;
    quint8 versionMinor = 0;
    std::vector<BundleBlobRecord> blobs;

    std::vector<const BundleBlobRecord *> blobsWithTag(quint32 tag) const;
};

// Parses a bundle from an in-memory `.modelbin`. Throws std::runtime_error on a bad
// magic or truncated header; individual malformed blobs are skipped, matching the
// tolerant behaviour of the reference reader.
ModelBundle parseModelBundle(const QByteArray &bytes);

} // namespace fh6
