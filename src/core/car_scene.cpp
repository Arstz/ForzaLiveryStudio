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
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace fls {

using fls::detail::readLeFloat;
using fls::detail::readLeU16;
using fls::detail::readLeU32;

// The shared wheel and tyre models in _library/scene carry no size of their own: every one of
// them is normalised to the same canonical rim radius, with the axial X spanning 0..1 across the
// wheel width. The real dimensions come from the axle's tyre spec, read the way a sidewall states
// it — section width, aspect ratio, rim diameter. These are shared by the wheel bake and the
// approximated tyre.
constexpr float kCanonRimRadial = 0.1397f;  // normalised rim outer == tyre inner bead
constexpr float kMetresPerInch = 0.0254f;
constexpr float kMetresPerMillimetre = 0.001f;
constexpr float kPercent = 0.01f;
constexpr float kMaxHubReach = 0.25f;  // furthest a corner part is moved to meet its hub

float rimRadiusMetres(const AxleSizing &axle) {
    return axle.rimDiameterInches * kMetresPerInch * 0.5f;
}

float tireWidthMetres(const AxleSizing &axle) {
    return axle.tireWidthMillimetres * kMetresPerMillimetre;
}

float tireRadiusMetres(const AxleSizing &axle) {
    return rimRadiusMetres(axle) + tireWidthMetres(axle) * axle.tireAspectPercent * kPercent;
}

float wheelRadialScale(const AxleSizing &axle) {
    return rimRadiusMetres(axle) / kCanonRimRadial;
}

// Where the corner belongs in car space. The carbin places wheels in an authoring pose whose X
// and Y are unreliable — some cars carry round placeholders — while its Z agrees with the stated
// wheelbase, so only the two bad axes are replaced.
float wheelCentreOffsetMetres(const AxleSizing &axle) {
    return axle.trackOuterMetres * 0.5f - tireWidthMetres(axle) * 0.5f;
}

float wheelCentreHeightMetres(const AxleSizing &axle) {
    return tireRadiusMetres(axle) - axle.rideHeightMetres;
}

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
    quint32 drawGroups = 0;
    bool stock = true;
    std::vector<int> optionIds;
    std::vector<MaterialBinding> materialBindings;
};

struct SceneParts {
    std::vector<PartInstance> stock;
    std::vector<PartInstance> projection;
    std::vector<CarPartOption> options;
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
    part.drawGroups = static_cast<quint32>(c.i32());

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
        std::vector<size_t> optionIndexes;

        const quint32 upgradeCount = c.u32();
        for (quint32 u = 0; u < upgradeCount; ++u) {
            const quint16 upgradeVersion = c.u16();
            const int level = c.u8();
            const bool isStock = c.bl();
            const qint32 id = c.i32();
            const qint32 carBodyId = c.i32();
            const bool parentIsStock = c.bl();
            CarPartOption option;
            option.partType = partType;
            option.id = id;
            option.level = level;
            option.carBodyId = carBodyId;
            option.parentIsStock = parentIsStock;
            option.stock = isStock;
            optionIndexes.push_back(parts.options.size());
            parts.options.push_back(std::move(option));
            if (isStock) {
                stockUpgradeIds.push_back(id);
            }
            if (upgradeVersion < 3) {
                const quint32 modelCount = c.u32();
                for (quint32 m = 0; m < modelCount; ++m) {
                    PartInstance inst = readRenderModel(c, series, version);
                    inst.partType = partType;
                    inst.stock = isStock;
                    inst.optionIds.push_back(id);
                    parts.options[optionIndexes.back()].modelPaths.push_back(inst.path);
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
                inst.optionIds = upgradeIds;
                const bool stock = idCount == 0
                    || std::any_of(upgradeIds.begin(), upgradeIds.end(), [&](int id) {
                           return std::find(stockUpgradeIds.begin(), stockUpgradeIds.end(), id) != stockUpgradeIds.end();
                       });
                inst.stock = stock;
                for (size_t optionIndex : optionIndexes) {
                    CarPartOption &option = parts.options[optionIndex];
                    if (upgradeIds.empty()
                        || std::find(upgradeIds.begin(), upgradeIds.end(), option.id)
                            != upgradeIds.end()) {
                        option.modelPaths.push_back(inst.path);
                    }
                }
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

// Corner parts — wheel, rotor, caliper, suspension arm — name the bone of the corner they belong
// to, so the suffix says which one and which side without relying on the placement itself.
enum Corner { kCornerLF, kCornerRF, kCornerLR, kCornerRR, kCornerCount };

int cornerFromBoneName(const QString &boneName) {
    static const QLatin1String suffixes[kCornerCount] = {
        QLatin1String("LF"), QLatin1String("RF"), QLatin1String("LR"), QLatin1String("RR")};
    for (int corner = 0; corner < kCornerCount; ++corner) {
        if (boneName.endsWith(suffixes[corner], Qt::CaseInsensitive)) {
            return corner;
        }
    }
    return -1;
}

bool isFrontCorner(int corner) {
    return corner == kCornerLF || corner == kCornerRF;
}

bool isLeftCorner(int corner) {
    return corner == kCornerLF || corner == kCornerLR;
}

bool isBrakeRotorPath(QString path) {
    path.replace('\\', '/');
    return path.contains(QStringLiteral("/rotors/"), Qt::CaseInsensitive);
}

// The rotor and its caliper ride on the hub, so they follow the rim rather than the pose the
// carbin authored them against.
bool isHubMountedPath(QString path) {
    path.replace('\\', '/');
    return path.contains(QStringLiteral("/rotors/"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("/calipers/"), Qt::CaseInsensitive);
}

// Car-space X span of a model, taken from the bundle's own bounding box so the caller does not
// have to decode geometry to find where a part reaches.
bool modelSpanX(const ModelBundle &bundle, const ModelMat4 &instance, float &minX, float &maxX) {
    for (const BundleBlobRecord *blob : bundle.blobsWithTag(bundle_tags::Model)) {
        if (!blob->bbox) {
            continue;
        }
        const std::array<float, 6> &box = *blob->bbox;
        minX = std::numeric_limits<float>::max();
        maxX = std::numeric_limits<float>::lowest();
        for (int corner = 0; corner < 8; ++corner) {
            const ModelVec3 point{(corner & 1) ? box[3] : box[0],
                                  (corner & 2) ? box[4] : box[1],
                                  (corner & 4) ? box[5] : box[2]};
            const float x = instance.transformPoint(point).x;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
        }
        return true;
    }
    return false;
}

// Stand-ins the game draws instead of the wheel itself: the blur slots hold the full-radius discs
// it swaps in for a spinning wheel, and the black slot a shell laid over the barrel to read as
// unlit depth. Both sit on top of the geometry they stand for, so a static render wants neither.
bool isWheelStandInSlot(const QString &materialName) {
    return materialName.startsWith(QStringLiteral("blur"), Qt::CaseInsensitive)
        || materialName.compare(QStringLiteral("black"), Qt::CaseInsensitive) == 0;
}

// The model's axial X runs from the outboard face at 0 to the inboard rim edge at 1, and the
// plane the part transform positions is the wheel's mid-width, so X maps onto the axle directly.
void bakeWheelTransform(CarMesh &mesh, const AxleSizing &axle) {
    const float radialScale = wheelRadialScale(axle);
    const float widthScale = tireWidthMetres(axle);
    for (ModelVec3 &position : mesh.positions) {
        position = mesh.boneTransform.transformPoint(
            {(position.x - 0.5f) * widthScale,
             position.y * radialScale,
             position.z * radialScale});
    }
    for (ModelVec3 &normal : mesh.normals) {
        normal = mesh.boneTransform.transformVector(
            {normal.x / widthScale,
             normal.y / radialScale,
             normal.z / radialScale});
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
    const QString name = materialToken(materialName);
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

quint64 wheelPaintHash(bool front, const CarMesh &mesh) {
    const int channel = wheelPaintChannel(mesh.materialName);
    if (channel >= 0) {
        return front
            ? material_hashes::binding::kFrontWheelPaint[channel]
            : material_hashes::binding::kRearWheelPaint[channel];
    }
    const QString name = materialToken(mesh.materialName);
    if (name == QStringLiteral("rim3")) return material_hashes::binding::kRims3;
    if (name == QStringLiteral("lip")) return material_hashes::binding::kRimsLip;
    if (name == QStringLiteral("detail")) return material_hashes::binding::kWheel1;
    if (name == QStringLiteral("detail2")) return material_hashes::binding::kWheel2;
    return 0;
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

// A part whose transform carries no translation is placed entirely by the bone it names, which
// belongs to the car skeleton rather than the part's own bundle.
bool placedByBoneAlone(const PartInstance &part) {
    const ModelMat4 &t = part.transform;
    return !part.boneName.isEmpty()
        && part.boneName.compare(QStringLiteral("<root>"), Qt::CaseInsensitive) != 0
        && std::abs(t.m[12]) < 0.000001f
        && std::abs(t.m[13]) < 0.000001f
        && std::abs(t.m[14]) < 0.000001f;
}

std::vector<SkeletonBone> loadCarSkeleton(
    const QString &skeletonPath, const QString &carbinDir, const QString &mediaName) {
    QFile file(resolvePath(skeletonPath, carbinDir, mediaName));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    try {
        return loadSkeletonBones(parseModelBundle(file.readAll()));
    } catch (const std::exception &) {
        return {};
    }
}

const SkeletonBone *findBone(const std::vector<SkeletonBone> &bones, const QString &name, qint16 id) {
    if (!name.isEmpty()) {
        for (const SkeletonBone &bone : bones) {
            if (bone.name.compare(name, Qt::CaseInsensitive) == 0) {
                return &bone;
            }
        }
        // A bone name was requested but this part's local model-bundle skeleton doesn't contain
        // it: the numeric boneId indexes the *global* car skeleton, not this local one, so
        // bones[id] here would attach the part to an unrelated local bone and misplace it (e.g.
        // Ultima's spindleLF -> local root_boneTrunk flings the wheel/brake into the air). The
        // part transform already carries the correct world placement, so fall back to no bone.
        return nullptr;
    }
    if (id >= 0 && id < static_cast<qint16>(bones.size())) {
        return &bones[id];
    }
    return nullptr;
}

} // namespace

CarModel loadCarBin(const QString &path, QString *error, const WheelSizing &wheels) {
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
    const std::vector<SkeletonBone> carSkeleton =
        loadCarSkeleton(skeletonPath, carbinDir, mediaName);

    CarModel car;
    car.sourcePath = path;
    car.partOptions = parts.options;
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

    // Every part of a corner moves with its wheel, so the whole assembly stays together.
    struct CornerPlacement {
        ModelVec3 centre;
        float shiftX = 0.0f;
        float shiftY = 0.0f;
        bool known = false;
    };
    std::array<CornerPlacement, kCornerCount> cornerPlacements;
    for (const PartInstance &part : parts.projection) {
        const int corner = cornerFromBoneName(part.boneName);
        if (corner < 0 || cornerPlacements[corner].known || !isWheelModelPath(part.path)) {
            continue;
        }
        const AxleSizing &axle = isFrontCorner(corner) ? wheels.front : wheels.rear;
        const float side = isLeftCorner(corner) ? -1.0f : 1.0f;
        CornerPlacement &placement = cornerPlacements[corner];
        placement.centre = {side * wheelCentreOffsetMetres(axle),
                            wheelCentreHeightMetres(axle),
                            part.transform.m[14]};
        placement.shiftX = placement.centre.x - part.transform.m[12];
        placement.shiftY = placement.centre.y - part.transform.m[13];
        placement.known = true;
    }

    // The rotor's outer face is the wheel's mounting plane, so it is seated on the rim's centre
    // and the rest of the corner hangs off it: the caliper rides along, and the suspension arm
    // reaches the rotor's inboard face.
    std::array<float, kCornerCount> hubShiftX{};
    std::array<float, kCornerCount> hubFaceX{};
    std::array<bool, kCornerCount> hubFaceKnown{};
    for (const PartInstance &part : parts.projection) {
        const int corner = cornerFromBoneName(part.boneName);
        if (corner < 0 || hubFaceKnown[corner] || !isBrakeRotorPath(part.path)
            || !cornerPlacements[corner].known) {
            continue;
        }
        QFile rotorFile(resolvePath(part.path, carbinDir, mediaName));
        if (!rotorFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const CornerPlacement &placement = cornerPlacements[corner];
        ModelMat4 instance = part.transform;
        instance.m[12] += placement.shiftX;
        instance.m[13] += placement.shiftY;
        float minX = 0.0f;
        float maxX = 0.0f;
        try {
            if (modelSpanX(parseModelBundle(rotorFile.readAll()), instance, minX, maxX)) {
                const bool left = isLeftCorner(corner);
                hubShiftX[corner] = placement.centre.x - (left ? minX : maxX);
                hubFaceX[corner] = (left ? maxX : minX) + hubShiftX[corner];
                hubFaceKnown[corner] = true;
            }
        } catch (const std::exception &) {
        }
    }

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
        const bool hasAdditionalMeshes = std::any_of(
            model.additionalLodMeshes.cbegin(), model.additionalLodMeshes.cend(),
            [](const std::vector<CarMesh> &meshes) { return !meshes.empty(); });
        if (model.meshes.empty() && !hasAdditionalMeshes) {
            continue;
        }

        const bool boneOnly = placedByBoneAlone(part);
        const int corner = cornerFromBoneName(part.boneName);
        ModelMat4 instance = part.transform;
        const std::vector<SkeletonBone> partSkeleton = loadSkeletonBones(bundle);
        if (const SkeletonBone *bone = findBone(partSkeleton, part.boneName, part.boneId)) {
            instance = matMul(part.transform, bone->world);
        } else if (boneOnly) {
            if (const SkeletonBone *carBone = findBone(carSkeleton, part.boneName, -1)) {
                instance = carBone->world;
            }
        }
        if (corner >= 0 && cornerPlacements[corner].known) {
            const CornerPlacement &placement = cornerPlacements[corner];
            if (boneOnly) {
                instance.m[12] = placement.centre.x;
                instance.m[13] = placement.centre.y;
                instance.m[14] = placement.centre.z;
            } else {
                instance.m[12] += placement.shiftX;
                instance.m[13] += placement.shiftY;
            }
        }
        if (corner >= 0 && hubFaceKnown[corner] && isHubMountedPath(part.path)) {
            instance.m[12] += hubShiftX[corner];
        }
        if (corner >= 0 && boneOnly && hubFaceKnown[corner]) {
            float minX = 0.0f;
            float maxX = 0.0f;
            if (modelSpanX(bundle, instance, minX, maxX)) {
                const float reach = hubFaceX[corner] - (isLeftCorner(corner) ? minX : maxX);
                // A part that would have to travel this far to reach the hub spans more than one
                // corner — a beam axle rather than an arm — and belongs where the corner put it.
                if (std::abs(reach) <= kMaxHubReach) {
                    instance.m[12] += reach;
                }
            }
        }

        const auto prepareMesh = [&](CarMesh &mesh) {
            if (isWheelModelPath(part.path) && isWheelStandInSlot(mesh.materialName)) {
                return false;
            }
            mesh.sourceModelPath = part.path;
            mesh.boneTransform = matMul(mesh.boneTransform, instance);
            mesh.carPartType = part.partType;
            mesh.modelInstanceId = currentInstanceId;
            mesh.drawGroups = part.drawGroups;
            mesh.stockPart = stock;
            mesh.partOptionIds = part.optionIds;
            mesh.paintMaterialHash = materialBindingHash(part, mesh);
            if (isWheelModelPath(part.path)) {
                const bool front = corner >= 0 ? isFrontCorner(corner) : frontWheel(part, mesh);
                bakeWheelTransform(mesh, front ? wheels.front : wheels.rear);
                if (mesh.paintMaterialHash == 0) {
                    mesh.paintMaterialHash = wheelPaintHash(front, mesh);
                }
            }

            return true;
        };
        std::vector<CarMesh> &targetBase = stock ? car.meshes : car.variantMeshes;
        std::vector<std::vector<CarMesh>> &targetLods = stock
            ? car.additionalLodMeshes
            : car.additionalVariantLodMeshes;
        while (targetLods.size() < model.additionalLodMeshes.size()) {
            targetLods.push_back(targetLods.empty() ? targetBase : targetLods.back());
        }
        std::vector<std::vector<CarMesh>> partLodMeshes(
            1 + model.additionalLodMeshes.size());
        for (CarMesh &mesh : model.meshes) {
            if (!prepareMesh(mesh)) {
                continue;
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
            }
            targetBase.push_back(mesh);
            partLodMeshes[0].push_back(mesh);
            car.liveryProjectionMeshes.push_back(std::move(mesh));
        }
        for (size_t lodIndex = 0;
             lodIndex < model.additionalLodMeshes.size();
             ++lodIndex) {
            for (CarMesh &mesh : model.additionalLodMeshes[lodIndex]) {
                if (prepareMesh(mesh)) {
                    partLodMeshes[lodIndex + 1].push_back(std::move(mesh));
                }
            }
        }
        for (size_t lodIndex = 1; lodIndex < partLodMeshes.size(); ++lodIndex) {
            if (partLodMeshes[lodIndex].empty()) {
                partLodMeshes[lodIndex] = partLodMeshes[lodIndex - 1];
            }
        }
        for (size_t lodIndex = 0; lodIndex < targetLods.size(); ++lodIndex) {
            const std::vector<CarMesh> &partMeshes = partLodMeshes[
                std::min(lodIndex + 1, partLodMeshes.size() - 1)];
            targetLods[lodIndex].insert(
                targetLods[lodIndex].end(), partMeshes.begin(), partMeshes.end());
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
    CarModel &car, const CarModel &leftTemplate, const CarModel &rightTemplate,
    const WheelSizing &wheels) {
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
        float beadRadius = std::numeric_limits<float>::max();
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
                const float radius = std::hypot(p.y, p.z);
                bounds.minX = std::min(bounds.minX, p.x);
                bounds.maxX = std::max(bounds.maxX, p.x);
                bounds.beadRadius = std::min(bounds.beadRadius, radius);
                bounds.radius = std::max(bounds.radius, radius);
            }
        }
        return bounds;
    };
    const TemplateBounds leftBounds = templateBounds(leftTemplate);
    const TemplateBounds rightBounds = templateBounds(rightTemplate);

    for (const auto &[sourceInstanceId, bounds] : mounts) {
        (void)sourceInstanceId;
        if (bounds.minX > bounds.maxX) {
            continue;
        }
        const float side = bounds.minX + bounds.maxX >= 0.0f ? 1.0f : -1.0f;
        const float centerY = (bounds.minY + bounds.maxY) * 0.5f;
        const float centerZ = (bounds.minZ + bounds.maxZ) * 0.5f;
        const AxleSizing &axle = centerZ >= 0.0f ? wheels.front : wheels.rear;
        const float mountRadius = std::max(bounds.maxY - bounds.minY, bounds.maxZ - bounds.minZ) * 0.5f;
        const float rimRadius = rimRadiusMetres(axle);
        const float tireWidth = tireWidthMetres(axle);
        const float tireRadius = tireRadiusMetres(axle);
        std::vector<float> lipSamples;
        lipSamples.reserve(bounds.positions.size());
        for (const ModelVec3 &position : bounds.positions) {
            const float radius = std::hypot(position.y - centerY, position.z - centerZ);
            if (radius >= mountRadius * 0.82f) {
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
        const float sidewallSpan = sourceBounds.radius - sourceBounds.beadRadius;
        const float axialScale = tireWidth / sourceWidth;
        // The bead sits just clear of the rim in the shipped models; scaling it by the rim's own
        // factor keeps that gap, where seating it flat on the rim radius makes the two surfaces
        // coincide and fight.
        const float beadSeatRadius = sourceBounds.beadRadius * rimRadius / kCanonRimRadial;
        // The template's own bead-to-tread proportion only matches the axle's at one aspect
        // ratio, so the sidewall is stretched to reach the tyre radius rather than scaled whole.
        const float sidewallScale = sidewallSpan > 0.000001f
            ? (tireRadius - beadSeatRadius) / sidewallSpan
            : tireRadius / sourceBounds.radius;
        const int instanceId = nextInstanceId++;

        for (const CarMesh &sourceMesh : source.meshes) {
            CarMesh mesh = sourceMesh;
            mesh.carPartType = 8;
            mesh.modelInstanceId = instanceId;
            mesh.stockPart = true;
            mesh.paintMaterialHash = 0;
            mesh.boneTransform = ModelMat4{};

            for (size_t i = 0; i < mesh.positions.size(); ++i) {
                const ModelVec3 p = sourceMesh.boneTransform.transformPoint(sourceMesh.positions[i]);
                const float sourceRadius = std::hypot(p.y, p.z);
                const float hoopScale = sourceRadius > 0.000001f
                    ? (beadSeatRadius + (sourceRadius - sourceBounds.beadRadius) * sidewallScale)
                        / sourceRadius
                    : 1.0f;

                mesh.positions[i] = {
                    centerX - side * (p.x - sourceCenterX) * axialScale,
                    centerY + p.y * hoopScale,
                    centerZ + p.z * hoopScale,
                };
                const ModelVec3 &position = mesh.positions[i];
                car.boundsMin.x = std::min(car.boundsMin.x, position.x);
                car.boundsMin.y = std::min(car.boundsMin.y, position.y);
                car.boundsMin.z = std::min(car.boundsMin.z, position.z);
                car.boundsMax.x = std::max(car.boundsMax.x, position.x);
                car.boundsMax.y = std::max(car.boundsMax.y, position.y);
                car.boundsMax.z = std::max(car.boundsMax.z, position.z);

                if (i >= mesh.normals.size() || sourceRadius <= 0.000001f) {
                    continue;
                }
                const ModelVec3 n = sourceMesh.boneTransform.transformVector(sourceMesh.normals[i]);
                const float radialY = p.y / sourceRadius;
                const float radialZ = p.z / sourceRadius;
                const float alongRadius = (n.y * radialY + n.z * radialZ) / sidewallScale;
                const float aroundRim = (n.y * -radialZ + n.z * radialY) / hoopScale;
                ModelVec3 normal{n.x / (-side * axialScale),
                                 alongRadius * radialY + aroundRim * -radialZ,
                                 alongRadius * radialZ + aroundRim * radialY};
                const float length = std::sqrt(
                    normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (length > 0.000001f) {
                    normal.x /= length;
                    normal.y /= length;
                    normal.z /= length;
                }
                mesh.normals[i] = normal;
            }
            car.meshes.push_back(std::move(mesh));
        }
    }
}

} // namespace fls
