#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace fh6 {

namespace bundle_tags {
constexpr quint32 Bundle = 0x47727562;        // "Grub"
constexpr quint32 Model = 0x4D6F646C;          // "Modl"
constexpr quint32 Mesh = 0x4D657368;           // "Mesh"
constexpr quint32 IndexBuffer = 0x496E6442;    // "IndB"
constexpr quint32 VertexLayout = 0x564C6179;   // "VLay"
constexpr quint32 InstancedVertexLayout = 0x494C6179; // "ILay"
constexpr quint32 VertexBuffer = 0x56657242;   // "VerB"
constexpr quint32 Skeleton = 0x536B656C;       // "Skel"
constexpr quint32 MaterialInstance = 0x4D617449;
constexpr quint32 MaterialResource = 0x4D415449;
constexpr quint32 MaterialLinks = 0x4D41544C;
constexpr quint32 MaterialParameters = 0x4D545052;
constexpr quint32 DefaultMaterialParameters = 0x44465052;
constexpr quint32 ManufacturerColors = 0x4D4E434C;

constexpr quint32 MetaName = 0x4E616D65;       // "Name"
constexpr quint32 MetaIdentifier = 0x49642020; // "Id  "
constexpr quint32 MetaBBox = 0x42426F78;       // "BBox"
} // namespace bundle_tags

struct BundleBlobRecord {
    quint32 tag = 0;
    quint8 versionMajor = 0;
    quint8 versionMinor = 0;
    QByteArray data;
    std::optional<quint32> id;
    QString name;
    std::optional<std::array<float, 6>> bbox;

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

ModelBundle parseModelBundle(const QByteArray &bytes);

} // namespace fh6
