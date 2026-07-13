#include "car_scene.h"

#include "binary_io.h"
#include "model_bundle.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fh6 {

using fh6::detail::readLeFloat;
using fh6::detail::readLeU16;
using fh6::detail::readLeU32;

namespace {

struct Cursor {
    const QByteArray &bytes;
    int pos = 0;
    explicit Cursor(const QByteArray &data) : bytes(data) {}

    void require(int n) const
    {
        if (pos < 0 || pos + n > bytes.size()) {
            throw std::runtime_error("carbin: unexpected end of data");
        }
    }
    quint8 u8() { require(1); return static_cast<quint8>(bytes[pos++]); }
    bool bl() { return u8() != 0; }
    qint16 i16() { require(2); const quint16 v = readLeU16(bytes, pos); pos += 2; return static_cast<qint16>(v); }
    quint16 u16() { require(2); const quint16 v = readLeU16(bytes, pos); pos += 2; return v; }
    qint32 i32() { require(4); const quint32 v = readLeU32(bytes, pos); pos += 4; return static_cast<qint32>(v); }
    quint32 u32() { require(4); const quint32 v = readLeU32(bytes, pos); pos += 4; return v; }
    float f32() { require(4); const float v = readLeFloat(bytes, pos); pos += 4; return v; }
    void skip(int n) { require(n); pos += n; }

    QString str()
    {
        const qint32 n = i32();
        if (n < 0 || n > (1 << 20)) {
            throw std::runtime_error("carbin: bad string length");
        }
        require(n);
        QString s = QString::fromLatin1(bytes.constData() + pos, n);
        pos += n;
        return s;
    }

    ModelMat4 matrix()
    {
        ModelMat4 m;
        for (int i = 0; i < 16; ++i) {
            m.m[i] = f32();
        }
        return m;
    }
};

enum class Series { Horizon, Motorsport };

struct PartInstance {
    QString path;
    ModelMat4 transform;
    QString boneName;
    qint16 boneId = -1;
};

PartInstance readRenderModel(Cursor &c, Series series, quint16 sceneVersion)
{
    const quint16 version = c.u16();

    PartInstance part;
    part.path = c.str();
    part.transform = c.matrix();

    if (version >= 5) {
        c.u16(); // LODDetails
    } else {
        c.u32();
    }

    part.boneName = c.str();
    part.boneId = c.i16();
    c.bl();  // SnapToParent
    c.i32(); // DrawGroups

    if (version < 9) {
        c.str(); // AOSwatchPath
    }
    if (version >= 2) {
        const quint32 overrideCount = c.u32();
        for (quint32 i = 0; i < overrideCount; ++i) {
            c.str();
            const quint32 len = c.u32();
            c.skip(static_cast<int>(len));
        }
    }
    if (version >= 3) {
        const quint32 indexCount = c.u32();
        for (quint32 i = 0; i < indexCount; ++i) {
            c.str();
            if (version >= 21) {
                c.skip(8); // u64
            } else {
                c.i32();
            }
        }
    }
    if (version >= 6) {
        if (c.bl()) { // IsDroppable
            c.f32();
            c.i32();
        }
    }
    if (version >= 8) {
        c.f32(); // BreakAmount
    }
    if (version >= 9) {
        const quint32 aoCount = c.u32();
        for (quint32 i = 0; i < aoCount; ++i) {
            const quint16 aoVersion = c.u16();
            c.str();          // Path
            c.u32();          // PartType
            c.i32();          // PartId
            if (aoVersion >= 2) {
                c.skip(16);   // GUID
            } else {
                c.i16();
                c.bl();
            }
            c.bl();           // IsDefault
            if (aoVersion >= 3) {
                c.skip(2);    // LodTest, LodValue (sbyte each)
            }
        }
    }
    if (version >= 10) {
        c.bl(); // IsInteriorWindshield
    }
    if (version >= 11) {
        c.bl();
        c.bl();
        c.u32();
        c.u32();
        c.u32();
        c.u32();
    }
    if (version >= 12) {
        c.str(); // AssemblyName
    }
    if (version >= 13) {
        c.skip(16); // GuidV13
    }
    if (version >= 14) {
        c.skip(16); // DropGuidV14
        c.u32();    // AOMapInfoIdV14
    }
    if (series == Series::Horizon && version >= 15) {
        c.i32(); // HorizonUnkV15
    }
    if ((series == Series::Motorsport && version >= 15) || (series == Series::Horizon && version >= 16)) {
        const quint32 dmgCount = c.u32();
        c.skip(static_cast<int>(dmgCount) * 16);
    }
    if (series == Series::Motorsport) {
        if (version >= 16) c.u32();
        if (version >= 17) c.u8();
        if (version >= 18) c.str();
        if (version >= 19) c.str();
        if (version >= 20) {
            c.bl();
            c.u32();
            c.u32();
            c.bl();
            c.bl();
        }
    } else { // Horizon
        if (version >= 17) c.u8();  // HorizonId
        if (version >= 18) c.u32(); // HorizonUnkV18
        if (version >= 21) {
            c.u32();  // HorizonUnkV21Flag
            c.str();  // HorizonUnkV21Path
        }
    }
    (void)sceneVersion;
    return part;
}

void readPart(Cursor &c, Series series, quint16 sceneVersion, std::vector<PartInstance> &out)
{
    const quint16 version = c.u16();
    c.u32(); // Type
    const quint32 modelCount = c.u32();
    for (quint32 i = 0; i < modelCount; ++i) {
        out.push_back(readRenderModel(c, series, sceneVersion));
    }
    if (version >= 2) {
        c.skip(32); // AABB (2x Vector4)
    }
}

std::vector<PartInstance> readScene(const QByteArray &bytes, QString &mediaName, QString &skeletonPath)
{
    Cursor c(bytes);
    const quint16 version = c.u16();
    Series series = Series::Horizon;
    if (version == 10 || version == 11) {
        series = Series::Motorsport;
    }

    if (version >= 3) {
        c.skip(16); // BuildGuid
    }
    if (version >= 5) {
        c.bl(); // BuildStrict
    }
    c.u32(); // Ordinal
    mediaName = c.str();
    skeletonPath = c.str();
    if (version >= 2) {
        c.u16(); // LODDetails
    }

    std::vector<PartInstance> parts;

    const quint32 nonUpgradableCount = c.u32();
    for (quint32 i = 0; i < nonUpgradableCount; ++i) {
        if (version >= 4) {
            c.u8(); // PartEntry.Type
        }
        readPart(c, series, version, parts);
    }

    const quint32 upgradableCount = c.u32();
    for (quint32 i = 0; i < upgradableCount; ++i) {
        const quint16 partVersion = c.u16();
        c.u32(); // Type
        std::vector<int> stockUpgradeIds;

        const quint32 upgradeCount = c.u32();
        for (quint32 u = 0; u < upgradeCount; ++u) {
            const quint16 upgradeVersion = c.u16();
            c.u8();                 // Level
            const bool isStock = c.bl();
            const qint32 id = c.i32();
            c.i32();                // CarBodyId
            c.bl();                 // ParentIsStock
            if (isStock) {
                stockUpgradeIds.push_back(id);
            }
            if (upgradeVersion < 3) {
                const quint32 modelCount = c.u32();
                for (quint32 m = 0; m < modelCount; ++m) {
                    PartInstance inst = readRenderModel(c, series, version);
                    if (isStock) {
                        parts.push_back(std::move(inst));
                    }
                }
            }
            if (upgradeVersion >= 2) {
                c.skip(32); // AABB
            }
        }

        if (partVersion >= 3) {
            const quint32 sharedCount = c.u32();
            for (quint32 s = 0; s < sharedCount; ++s) {
                const quint32 idCount = c.u32();
                std::vector<int> upgradeIds(idCount);
                for (quint32 k = 0; k < idCount; ++k) {
                    upgradeIds[k] = c.i32();
                }
                PartInstance inst = readRenderModel(c, series, version);
                const bool stock = idCount == 0
                    || std::any_of(upgradeIds.begin(), upgradeIds.end(), [&](int id) {
                           return std::find(stockUpgradeIds.begin(), stockUpgradeIds.end(), id) != stockUpgradeIds.end();
                       });
                if (stock) {
                    parts.push_back(std::move(inst));
                }
            }
        }
    }

    return parts;
}

QString resolvePath(const QString &gamePath, const QString &carbinDir, const QString &mediaName)
{
    QString normalized = gamePath;
    normalized.replace('\\', '/');
    const QString needle = QStringLiteral("/") + mediaName.toLower() + QStringLiteral("/");
    const int idx = normalized.toLower().indexOf(needle);
    const QString tail = idx >= 0 ? normalized.mid(idx + needle.size())
                                  : QFileInfo(normalized).fileName();
    return QDir(carbinDir).filePath(tail);
}

const SkeletonBone *findBone(const std::vector<SkeletonBone> &bones, const QString &name, qint16 id)
{
    if (!name.isEmpty()) {
        for (const SkeletonBone &bone : bones) {
            if (bone.name.compare(name, Qt::CaseInsensitive) == 0) {
                return &bone;
            }
        }
    }
    if (id >= 0 && id < static_cast<qint16>(bones.size())) {
        return &bones[id];
    }
    return nullptr;
}

} // namespace

CarModel loadCarBin(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("cannot open %1").arg(path);
        }
        return {};
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QString mediaName;
    QString skeletonPath;
    std::vector<PartInstance> parts;
    try {
        parts = readScene(bytes, mediaName, skeletonPath);
    } catch (const std::exception &ex) {
        if (error) {
            *error = QString::fromUtf8(ex.what());
        }
        return {};
    }

    const QString carbinDir = QFileInfo(path).absolutePath();
    (void)skeletonPath;

    CarModel car;
    car.sourcePath = path;
    float minX = std::numeric_limits<float>::max();
    float minY = minX;
    float minZ = minX;
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = maxX;
    float maxZ = maxX;
    int loaded = 0;

    for (const PartInstance &part : parts) {
        const QString modelPath = resolvePath(part.path, carbinDir, mediaName);
        QFile modelFile(modelPath);
        if (!modelFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        ModelBundle bundle;
        try {
            bundle = parseModelBundle(modelFile.readAll());
        } catch (const std::exception &) {
            continue;
        }
        CarModel model = decodeModel(bundle);
        if (model.meshes.empty()) {
            continue;
        }

        ModelMat4 instance = part.transform;
        const std::vector<SkeletonBone> partSkeleton = loadSkeletonBones(bundle);
        if (const SkeletonBone *bone = findBone(partSkeleton, part.boneName, part.boneId)) {
            instance = matMul(part.transform, bone->world);
        }

        for (CarMesh &mesh : model.meshes) {
            mesh.boneTransform = matMul(mesh.boneTransform, instance);
            for (const ModelVec3 &p : mesh.positions) {
                const ModelVec3 w = mesh.boneTransform.transformPoint(p);
                minX = std::min(minX, w.x);
                minY = std::min(minY, w.y);
                minZ = std::min(minZ, w.z);
                maxX = std::max(maxX, w.x);
                maxY = std::max(maxY, w.y);
                maxZ = std::max(maxZ, w.z);
            }
            car.meshes.push_back(std::move(mesh));
        }
        ++loaded;
    }

    if (car.meshes.empty()) {
        if (error) {
            *error = loaded == 0
                ? QStringLiteral("carbin: no referenced modelbins found next to %1").arg(QFileInfo(path).fileName())
                : QStringLiteral("carbin: referenced models decoded to zero meshes");
        }
        return {};
    }

    car.boundsMin = {minX, minY, minZ};
    car.boundsMax = {maxX, maxY, maxZ};
    return car;
}

} // namespace fh6
