#include "model_bundle.h"

#include "binary_io.h"

#include <QtEndian>

#include <cstring>
#include <stdexcept>

namespace fh6 {

using fh6::detail::readLeFloat;
using fh6::detail::readLeU16;
using fh6::detail::readLeU32;

namespace {

constexpr int BlobInfoSize = 0x18;     // per-blob header record size
constexpr int MetadataInfoSize = 0x08; // per-metadata header record size

void readBlobMetadata(const QByteArray &bytes, quint32 metadataOffset, quint32 metadataCount,
                      BundleBlobRecord &blob) {
    for (quint32 i = 0; i < metadataCount; ++i) {
        const int recordBase = static_cast<int>(metadataOffset) + static_cast<int>(i) * MetadataInfoSize;
        if (recordBase < 0 || recordBase + MetadataInfoSize > bytes.size()) {
            break;
        }
        const quint32 tag = readLeU32(bytes, recordBase);
        const quint16 flags = readLeU16(bytes, recordBase + 4);
        const quint16 size = static_cast<quint16>(flags >> 4);
        const quint16 relOffset = readLeU16(bytes, recordBase + 6);
        const int dataOffset = recordBase + relOffset;
        if (dataOffset < 0 || dataOffset + size > bytes.size()) {
            continue;
        }

        switch (tag) {
        case bundle_tags::MetaIdentifier:
            if (size >= 4) {
                blob.id = readLeU32(bytes, dataOffset);
            }
            break;
        case bundle_tags::MetaName:
            blob.name = QString::fromLatin1(bytes.constData() + dataOffset, size);
            break;
        case bundle_tags::MetaBBox:
            if (size >= 24) {
                std::array<float, 6> box{};
                for (int f = 0; f < 6; ++f) {
                    box[f] = readLeFloat(bytes, dataOffset + f * 4);
                }
                blob.bbox = box;
            }
            break;
        default:
            break;
        }
    }
}

} // namespace

std::vector<const BundleBlobRecord *> ModelBundle::blobsWithTag(quint32 tag) const {
    std::vector<const BundleBlobRecord *> out;
    for (const BundleBlobRecord &blob : blobs) {
        if (blob.tag == tag) {
            out.push_back(&blob);
        }
    }
    return out;
}

ModelBundle parseModelBundle(const QByteArray &bytes) {
    if (bytes.size() < 8) {
        throw std::runtime_error("modelbin too small to be a bundle");
    }
    if (readLeU32(bytes, 0) != bundle_tags::Bundle) {
        throw std::runtime_error("not a ForzaTech bundle (bad magic)");
    }

    ModelBundle bundle;
    bundle.versionMajor = static_cast<quint8>(bytes[4]);
    bundle.versionMinor = static_cast<quint8>(bytes[5]);

    int pos = 6;
    quint32 blobCount = 0;
    if (bundle.versionMajor > 1 || (bundle.versionMajor == 1 && bundle.versionMinor >= 1)) {
        pos += 2;                            // int16 padding
        pos += 4;                            // uint32 headerSize
        pos += 4;                            // uint32 totalSize
        blobCount = readLeU32(bytes, pos);
        pos += 4;
    } else {
        blobCount = readLeU16(bytes, pos);
        pos += 2 + 8;                        // u16 count + 8 bytes
    }

    const int blobHeadersStart = pos;
    bundle.blobs.reserve(blobCount);

    for (quint32 i = 0; i < blobCount; ++i) {
        const int header = blobHeadersStart + static_cast<int>(i) * BlobInfoSize;
        if (header < 0 || header + BlobInfoSize > bytes.size()) {
            break;
        }
        try {
            BundleBlobRecord blob;
            blob.tag = readLeU32(bytes, header);
            blob.versionMajor = static_cast<quint8>(bytes[header + 4]);
            blob.versionMinor = static_cast<quint8>(bytes[header + 5]);
            const quint16 metadataCount = readLeU16(bytes, header + 6);
            const quint32 metadataOffset = readLeU32(bytes, header + 8);
            const quint32 dataOffset = readLeU32(bytes, header + 12);
            const quint32 compressedSize = readLeU32(bytes, header + 16);
            const quint32 uncompressedSize = readLeU32(bytes, header + 20);

            readBlobMetadata(bytes, metadataOffset, metadataCount, blob);

            const quint32 sizeToRead = uncompressedSize > 0 ? uncompressedSize : compressedSize;
            if (static_cast<int>(dataOffset) >= 0
                && static_cast<qint64>(dataOffset) + sizeToRead <= bytes.size()) {
                blob.data = bytes.mid(static_cast<int>(dataOffset), static_cast<int>(sizeToRead));
            }
            bundle.blobs.push_back(std::move(blob));
        } catch (const std::exception &) {
        }
    }

    return bundle;
}

} // namespace fh6
