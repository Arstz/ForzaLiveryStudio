#include "car_scene.h"

#include "binary_io.h"
#include "material_hashes.h"
#include "model_bundle.h"
#include "model_material.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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

    void require(int n) const {
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
    quint64 u64() { const quint64 lo = u32(); return lo | (static_cast<quint64>(u32()) << 32); }
    float f32() { require(4); const float v = readLeFloat(bytes, pos); pos += 4; return v; }
    void skip(int n) { require(n); pos += n; }

    QString str() {
        const qint32 n = i32();
        if (n < 0 || n > (1 << 20)) {
            throw std::runtime_error("carbin: bad string length");
        }
        require(n);
        QString s = QString::fromLatin1(bytes.constData() + pos, n);
        pos += n;
        return s;
    }

    ModelMat4 matrix() {
        ModelMat4 m;
        for (int i = 0; i < 16; ++i) {
            m.m[i] = f32();
        }
        return m;
    }
};

enum class Series { Horizon, Motorsport };

struct PartInstance {
    struct MaterialBinding {
        QString name;
        quint64 hash = 0;
    };

    QString path;
    ModelMat4 transform;
    QString boneName;
    qint16 boneId = -1;
    int partType = -1;
    bool stock = true;
    std::vector<MaterialBinding> materialBindings;
};

struct SceneParts {
    std::vector<PartInstance> stock;
    std::vector<PartInstance> projection;
};

int canonicalPartType(quint32 type) {
    return type >= 42 ? static_cast<int>(type + 1) : static_cast<int>(type);
}

PartInstance readRenderModel(Cursor &c, Series series, quint16 sceneVersion) {
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
        part.materialBindings.reserve(indexCount);
        for (quint32 i = 0; i < indexCount; ++i) {
            PartInstance::MaterialBinding binding;
            binding.name = c.str();
            if (version >= 21) {
                binding.hash = c.u64();
            } else {
                binding.hash = static_cast<quint32>(c.i32());
            }
            part.materialBindings.push_back(std::move(binding));
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

void readPart(Cursor &c, Series series, quint16 sceneVersion, SceneParts &out) {
    const quint16 version = c.u16();
    const int partType = canonicalPartType(c.u32());
    const quint32 modelCount = c.u32();
    for (quint32 i = 0; i < modelCount; ++i) {
        PartInstance part = readRenderModel(c, series, sceneVersion);
        part.partType = partType;
        out.stock.push_back(part);
        out.projection.push_back(std::move(part));
    }
    if (version >= 2) {
        c.skip(32); // AABB (2x Vector4)
    }
}

SceneParts readScene(const QByteArray &bytes, QString &mediaName, QString &skeletonPath) {
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

    SceneParts parts;

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
        const int partType = canonicalPartType(c.u32());
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
                    inst.partType = partType;
                    inst.stock = isStock;
                    parts.projection.push_back(inst);
                    if (isStock) {
                        parts.stock.push_back(std::move(inst));
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
                inst.partType = partType;
                const bool stock = idCount == 0
                    || std::any_of(upgradeIds.begin(), upgradeIds.end(), [&](int id) {
                           return std::find(stockUpgradeIds.begin(), stockUpgradeIds.end(), id) != stockUpgradeIds.end();
                       });
                inst.stock = stock;
                parts.projection.push_back(inst);
                if (stock) {
                    parts.stock.push_back(std::move(inst));
                }
            }
        }
    }

    return parts;
}

QString partKey(const PartInstance &part) {
    QString key = part.path.toLower() + QLatin1Char('|') + part.boneName.toLower()
        + QLatin1Char('|') + QString::number(part.boneId)
        + QLatin1Char('|') + QString::number(part.partType);
    for (float value : part.transform.m) {
        key += QLatin1Char('|') + QString::number(value, 'g', 9);
    }
    for (const PartInstance::MaterialBinding &binding : part.materialBindings) {
        key += QLatin1Char('|') + binding.name.toLower()
            + QLatin1Char('=') + QString::number(binding.hash, 16);
    }
    return key;
}

QString materialToken(QString value) {
    value.replace('\\', '/');
    value = value.mid(value.lastIndexOf('/') + 1).toLower();
    const int pipe = value.indexOf('|');
    if (pipe >= 0) {
        value.truncate(pipe);
    }
    if (value.endsWith(QStringLiteral(".materialbin"))) {
        value.chop(12);
    }
    return value;
}

quint64 materialBindingHash(const PartInstance &part, const CarMesh &mesh) {
    QStringList candidates;
    candidates.push_back(materialToken(mesh.materialName));
    if (mesh.material) {
        candidates.push_back(materialToken(mesh.material->name));
        candidates.push_back(materialToken(mesh.material->resourcePath));
    }
    for (const PartInstance::MaterialBinding &binding : part.materialBindings) {
        const QString key = materialToken(binding.name);
        for (const QString &candidate : candidates) {
            if (!key.isEmpty() && key == candidate) {
                return binding.hash;
            }
        }
    }
    for (const PartInstance::MaterialBinding &binding : part.materialBindings) {
        const QString key = materialToken(binding.name);
        for (const QString &candidate : candidates) {
            if (key.size() >= 6 && candidate.size() >= 6
                && (key.contains(candidate) || candidate.contains(key))) {
                return binding.hash;
            }
        }
    }
    return 0;
}

bool isWheelModelPath(QString path) {
    path.replace('\\', '/');
    return path.contains(QStringLiteral("/wheels/"), Qt::CaseInsensitive);
}

void bakeWheelTransform(CarMesh &mesh) {
    constexpr float radialScale = 1.632857f;
    for (ModelVec3 &position : mesh.positions) {
        position = mesh.boneTransform.transformPoint(
            {-position.x, position.y * radialScale, -position.z * radialScale});
    }
    for (ModelVec3 &normal : mesh.normals) {
        normal = mesh.boneTransform.transformVector(
            {-normal.x, normal.y / radialScale, -normal.z / radialScale});
        const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (length > 0.000001f) {
            normal.x /= length;
            normal.y /= length;
            normal.z /= length;
        }
    }
    mesh.boneTransform = ModelMat4{};
}

int wheelPaintChannel(const QString &materialName) {
    const QString name = materialName.toLower();
    if (name == QStringLiteral("rim")) return 0;
    if (name == QStringLiteral("rim2")) return 1;
    if (name == QStringLiteral("inner_rim")) return 2;
    if (name == QStringLiteral("hub")) return 3;
    if (name == QStringLiteral("lug")) return 4;
    return -1;
}

bool frontWheel(const PartInstance &part, const CarMesh &mesh) {
    const QString identity = part.boneName.toLower() + QLatin1Char('|') + mesh.name.toLower();
    if (identity.contains(QStringLiteral("wheellf"))
        || identity.contains(QStringLiteral("wheelrf"))) {
        return true;
    }
    if (identity.contains(QStringLiteral("wheellr"))
        || identity.contains(QStringLiteral("wheelrr"))) {
        return false;
    }
    if (mesh.positions.empty()) {
        return false;
    }
    double z = 0.0;
    for (const ModelVec3 &position : mesh.positions) {
        z += position.z;
    }
    return z / static_cast<double>(mesh.positions.size()) >= 0.0;
}

quint64 wheelPaintHash(const PartInstance &part, const CarMesh &mesh) {
    const int channel = wheelPaintChannel(mesh.materialName);
    if (channel < 0) {
        return 0;
    }
    return frontWheel(part, mesh)
        ? material_hashes::binding::kFrontWheelPaint[channel]
        : material_hashes::binding::kRearWheelPaint[channel];
}

std::vector<CarLocator> loadCarLocators(const QString &carbinDir) {
    QFile file(QDir(carbinDir).filePath(QStringLiteral("Locators.xml")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString text = QString::fromUtf8(file.readAll());
    const QRegularExpression locatorExpression(
        QStringLiteral(R"(<Locator\b[^>]*>([\s\S]*?)</Locator>)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression nameExpression(
        QStringLiteral(R"re(<Name\s+value="([^"]+)")re"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression transformExpression(
        QStringLiteral(R"(<SceneTransform\s+([^>]*)/>)"),
        QRegularExpression::CaseInsensitiveOption);

    std::vector<CarLocator> locators;
    QRegularExpressionMatchIterator it = locatorExpression.globalMatch(text);
    while (it.hasNext()) {
        const QString block = it.next().captured(1);
        const QRegularExpressionMatch nameMatch = nameExpression.match(block);
        const QRegularExpressionMatch transformMatch = transformExpression.match(block);
        if (!nameMatch.hasMatch() || !transformMatch.hasMatch()) {
            continue;
        }
        const QString attributes = transformMatch.captured(1);
        const auto component = [&](const char *name, bool &ok) {
            const QRegularExpression expression(
                QStringLiteral(R"re(value\._%1="([^"]+)")re").arg(QLatin1String(name)));
            const QRegularExpressionMatch match = expression.match(attributes);
            if (!match.hasMatch()) {
                ok = false;
                return 0.0f;
            }
            bool parsed = false;
            const float value = match.captured(1).toFloat(&parsed);
            ok = ok && parsed;
            return value;
        };
        bool ok = true;
        CarLocator locator;
        locator.name = nameMatch.captured(1);
        locator.position.x = component("41", ok);
        locator.position.y = component("42", ok);
        locator.position.z = component("43", ok);
        if (ok) {
            locators.push_back(std::move(locator));
        }
    }
    return locators;
}

QString resolvePath(const QString &gamePath, const QString &carbinDir, const QString &mediaName) {
    QString normalized = gamePath;
    normalized.replace('\\', '/');
    const QString needle = QStringLiteral("/") + mediaName.toLower() + QStringLiteral("/");
    const int idx = normalized.toLower().indexOf(needle);
    const QString tail = idx >= 0 ? normalized.mid(idx + needle.size())
                                  : QFileInfo(normalized).fileName();
    return QDir(carbinDir).filePath(tail);
}

const SkeletonBone *findBone(const std::vector<SkeletonBone> &bones, const QString &name, qint16 id) {
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

CarModel loadCarBin(const QString &path, QString *error) {
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
    SceneParts parts;
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
    car.locators = loadCarLocators(carbinDir);
    float minX = std::numeric_limits<float>::max();
    float minY = minX;
    float minZ = minX;
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = maxX;
    float maxZ = maxX;
    int loaded = 0;
    QSet<QString> stockParts;
    for (const PartInstance &part : parts.stock) {
        stockParts.insert(partKey(part));
    }
    QSet<QString> loadedParts;
    int modelInstanceId = 0;

    for (const PartInstance &part : parts.projection) {
        const int currentInstanceId = modelInstanceId++;
        const QString key = partKey(part);
        if (loadedParts.contains(key)) {
            continue;
        }
        loadedParts.insert(key);
        const bool stock = stockParts.contains(key);
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
            mesh.sourceModelPath = part.path;
            mesh.boneTransform = matMul(mesh.boneTransform, instance);
            mesh.carPartType = part.partType;
            mesh.modelInstanceId = currentInstanceId;
            mesh.stockPart = stock;
            mesh.paintMaterialHash = materialBindingHash(part, mesh);
            if (isWheelModelPath(part.path)) {
                bakeWheelTransform(mesh);
                if (mesh.paintMaterialHash == 0) {
                    mesh.paintMaterialHash = wheelPaintHash(part, mesh);
                }
            }
            if (stock) {
                for (const ModelVec3 &p : mesh.positions) {
                    const ModelVec3 w = mesh.boneTransform.transformPoint(p);
                    minX = std::min(minX, w.x);
                    minY = std::min(minY, w.y);
                    minZ = std::min(minZ, w.z);
                    maxX = std::max(maxX, w.x);
                    maxY = std::max(maxY, w.y);
                    maxZ = std::max(maxZ, w.z);
                }
                car.meshes.push_back(mesh);
            }
            car.liveryProjectionMeshes.push_back(std::move(mesh));
        }
        if (stock) {
            ++loaded;
        }
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

void appendApproximateTires(
    CarModel &car, const CarModel &leftTemplate, const CarModel &rightTemplate) {
    struct MountBounds {
        float minX = std::numeric_limits<float>::max();
        float minY = minX;
        float minZ = minX;
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = maxX;
        float maxZ = maxX;
        std::vector<ModelVec3> positions;
    };

    std::map<int, MountBounds> mounts;
    int nextInstanceId = 0;
    for (const CarMesh &mesh : car.meshes) {
        nextInstanceId = std::max(nextInstanceId, mesh.modelInstanceId + 1);
        const QString material = mesh.materialName.toLower();
        if (mesh.modelInstanceId < 0
            || !mesh.name.startsWith(QStringLiteral("wheel_"), Qt::CaseInsensitive)
            || !mesh.name.contains(QStringLiteral("LODS0"), Qt::CaseInsensitive)
            || (material != QStringLiteral("rim") && material != QStringLiteral("rim2"))) {
            continue;
        }
        MountBounds &bounds = mounts[mesh.modelInstanceId];
        for (const ModelVec3 &position : mesh.positions) {
            bounds.minX = std::min(bounds.minX, position.x);
            bounds.minY = std::min(bounds.minY, position.y);
            bounds.minZ = std::min(bounds.minZ, position.z);
            bounds.maxX = std::max(bounds.maxX, position.x);
            bounds.maxY = std::max(bounds.maxY, position.y);
            bounds.maxZ = std::max(bounds.maxZ, position.z);
            bounds.positions.push_back(position);
        }
    }

    struct TemplateBounds {
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float radius = 0.0f;
    };
    const auto templateBounds = [](const CarModel &source) {
        TemplateBounds bounds;
        for (const CarMesh &mesh : source.meshes) {
            if (!mesh.name.contains(QStringLiteral("LODS0"), Qt::CaseInsensitive)) {
                continue;
            }
            for (const ModelVec3 &position : mesh.positions) {
                const ModelVec3 p = mesh.boneTransform.transformPoint(position);
                bounds.minX = std::min(bounds.minX, p.x);
                bounds.maxX = std::max(bounds.maxX, p.x);
                bounds.radius = std::max(bounds.radius, std::hypot(p.y, p.z));
            }
        }
        return bounds;
    };
    const TemplateBounds leftBounds = templateBounds(leftTemplate);
    const TemplateBounds rightBounds = templateBounds(rightTemplate);

    constexpr float widthToRimRadius = 0.305f / 0.2286f;
    constexpr float tireToRimRadius = 0.3201f / 0.2286f;

    for (const auto &[sourceInstanceId, bounds] : mounts) {
        (void)sourceInstanceId;
        if (bounds.minX > bounds.maxX) {
            continue;
        }
        const float side = bounds.minX + bounds.maxX >= 0.0f ? 1.0f : -1.0f;
        const float centerY = (bounds.minY + bounds.maxY) * 0.5f;
        const float centerZ = (bounds.minZ + bounds.maxZ) * 0.5f;
        const float rimRadius = std::max(bounds.maxY - bounds.minY, bounds.maxZ - bounds.minZ) * 0.5f;
        const float tireWidth = rimRadius * widthToRimRadius;
        const float tireRadius = rimRadius * tireToRimRadius;
        std::vector<float> lipSamples;
        lipSamples.reserve(bounds.positions.size());
        for (const ModelVec3 &position : bounds.positions) {
            const float radius = std::hypot(position.y - centerY, position.z - centerZ);
            if (radius >= rimRadius * 0.82f) {
                lipSamples.push_back(side * position.x);
            }
        }
        std::sort(lipSamples.begin(), lipSamples.end());
        const float rimOuterX = lipSamples.empty()
            ? (side > 0.0f ? bounds.maxX : bounds.minX)
            : side * lipSamples[static_cast<size_t>(
                  0.9f * static_cast<float>(lipSamples.size() - 1))];
        const float centerX = rimOuterX - side * tireWidth * 0.5f;
        const CarModel &source = side < 0.0f ? leftTemplate : rightTemplate;
        const TemplateBounds &sourceBounds = side < 0.0f ? leftBounds : rightBounds;
        const float sourceWidth = sourceBounds.maxX - sourceBounds.minX;
        if (rimRadius <= 0.0f || sourceWidth <= 0.0f || sourceBounds.radius <= 0.0f) {
            continue;
        }
        const float sourceCenterX = (sourceBounds.minX + sourceBounds.maxX) * 0.5f;
        const float axialScale = tireWidth / sourceWidth;
        const float radialScale = tireRadius / sourceBounds.radius;
        const int instanceId = nextInstanceId++;

        for (const CarMesh &sourceMesh : source.meshes) {
            CarMesh mesh = sourceMesh;
            mesh.carPartType = 8;
            mesh.modelInstanceId = instanceId;
            mesh.stockPart = true;
            mesh.paintMaterialHash = 0;
            mesh.boneTransform = ModelMat4{};

            for (ModelVec3 &position : mesh.positions) {
                const ModelVec3 p = sourceMesh.boneTransform.transformPoint(position);
                position = {
                    centerX - side * (p.x - sourceCenterX) * axialScale,
                    centerY + p.y * radialScale,
                    centerZ + p.z * radialScale,
                };
                car.boundsMin.x = std::min(car.boundsMin.x, position.x);
                car.boundsMin.y = std::min(car.boundsMin.y, position.y);
                car.boundsMin.z = std::min(car.boundsMin.z, position.z);
                car.boundsMax.x = std::max(car.boundsMax.x, position.x);
                car.boundsMax.y = std::max(car.boundsMax.y, position.y);
                car.boundsMax.z = std::max(car.boundsMax.z, position.z);
            }
            for (ModelVec3 &normal : mesh.normals) {
                normal = sourceMesh.boneTransform.transformVector(normal);
                normal = {
                    normal.x / (-side * axialScale),
                    normal.y / radialScale,
                    normal.z / radialScale,
                };
                const float length = std::sqrt(
                    normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (length > 0.000001f) {
                    normal.x /= length;
                    normal.y /= length;
                    normal.z /= length;
                }
            }
            car.meshes.push_back(std::move(mesh));
        }
    }
}

} // namespace fh6
