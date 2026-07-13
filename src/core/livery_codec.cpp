#include "livery_codec.h"

#include "binary_io.h"
#include "cgroup_codec.h"
#include "layer.h"
#include "matrix_math.h"
#include "vinyl_decoder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointF>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace fh6 {
namespace {

QString resolveLiveryPath(const QString &folderOrFile)
{
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        return QDir(folderOrFile).filePath(QStringLiteral("C_livery"));
    }
    return folderOrFile;
}

LiveryPayload parseInflatedLiveryPayload(const QByteArray &raw)
{
    LiveryPayload payload;
    payload.raw = raw;

    const int vlrc = payload.raw.indexOf(QByteArray("vlrc", 4));
    if (vlrc >= 0 && vlrc + 0x14 <= payload.raw.size()) {
        payload.carId = static_cast<int>(detail::readLeU32(payload.raw, vlrc + 0x10));
    }

    const int gyvl = payload.raw.indexOf(QByteArray("gyvl", 4));
    if (gyvl < 0) {
        throw std::runtime_error("C_livery has no embedded gyvl chunk");
    }
    payload.gyvlOffset = gyvl;

    const int bodyStart = gyvl + 0x15;
    int bodyEnd = payload.raw.indexOf(QByteArray("yrvl", 4), gyvl);
    if (bodyEnd < 0 || bodyEnd < bodyStart) {
        bodyEnd = payload.raw.size();
    }
    if (bodyStart >= payload.raw.size()) {
        throw std::runtime_error("C_livery gyvl body is truncated");
    }
    payload.body = payload.raw.mid(bodyStart, bodyEnd - bodyStart);

    const int statsTag = bodyEnd;
    payload.sectionCounts.reserve(11);
    if (statsTag >= 0 && payload.raw.mid(statsTag, 4) == QByteArray("yrvl", 4)) {
        for (int i = 0; i < 11; ++i) {
            const int off = statsTag + 4 + i * 4;
            payload.sectionCounts.push_back(off + 4 <= payload.raw.size()
                                                ? static_cast<int>(detail::readLeU32(payload.raw, off))
                                                : 0);
        }
    } else {
        for (int i = 0; i < 11; ++i) {
            payload.sectionCounts.push_back(0);
        }
    }
    return payload;
}

} // namespace

LiveryPayload readLiveryPayload(const QString &folderOrFile)
{
    QFile file(resolveLiveryPath(folderOrFile));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open C_livery: " + file.fileName()).toStdString());
    }

    LiveryPayload payload;
    payload.raw = inflateContainer(file.readAll());

    // The vlrc root header names the target car by a u32 at rel 0x10 (see
    // LiveryResearch/CLIVERY.md; the field the game uses to pick which car the
    // livery is applied to — matches the car id stored in the car's .carbin).
    const int vlrc = payload.raw.indexOf(QByteArray("vlrc", 4));
    if (vlrc >= 0 && vlrc + 0x14 <= payload.raw.size()) {
        payload.carId = static_cast<int>(detail::readLeU32(payload.raw, vlrc + 0x10));
    }

    const int gyvl = payload.raw.indexOf(QByteArray("gyvl", 4));
    if (gyvl < 0) {
        throw std::runtime_error("C_livery has no embedded gyvl chunk");
    }
    payload.gyvlOffset = gyvl;

    // The embedded gyvl section stream starts at gyvl-rel 0x15 and runs until the
    // next chunk (the trailing yrvl section table).
    const int bodyStart = gyvl + 0x15;
    int bodyEnd = payload.raw.indexOf(QByteArray("yrvl", 4), gyvl);
    if (bodyEnd < 0 || bodyEnd < bodyStart) {
        bodyEnd = payload.raw.size();
    }
    if (bodyStart >= payload.raw.size()) {
        throw std::runtime_error("C_livery gyvl body is truncated");
    }
    payload.body = payload.raw.mid(bodyStart, bodyEnd - bodyStart);

    // The yrvl "stats" chunk directly after gyvl holds 11 u32 per-section decal
    // counts (storage order), starting right after its 4-byte tag.
    const int statsTag = bodyEnd;  // first yrvl after gyvl == bodyEnd
    payload.sectionCounts.reserve(11);
    if (statsTag >= 0 && payload.raw.mid(statsTag, 4) == QByteArray("yrvl", 4)) {
        for (int i = 0; i < 11; ++i) {
            const int off = statsTag + 4 + i * 4;
            if (off + 4 <= payload.raw.size()) {
                payload.sectionCounts.push_back(static_cast<int>(detail::readLeU32(payload.raw, off)));
            } else {
                payload.sectionCounts.push_back(0);
            }
        }
    } else {
        for (int i = 0; i < 11; ++i) {
            payload.sectionCounts.push_back(0);
        }
    }
    return payload;
}

namespace {

// The 0x15-byte gyvl chunk header (version-0 livery dialect); constant across all
// samples. See CLIVERY.md.
const unsigned char kGyvlHeader[0x15] = {
    0x67, 0x79, 0x76, 0x6c,             // "gyvl"
    0x00, 0x00, 0x00, 0x00,             // version = 0
    0x00, 0x00, 0x00, 0x00,             // 0
    0x00, 0x00, 0x80, 0x3f,             // f32 1.0 (root scale)
    0x00, 0x00, 0x00, 0x00,             // 0
    0x00,                               // u8 0
};

// Fallback scaffold rotations for source-less synthesis. Imported liveries prefer
// the captured per-car scaffold/remnant bytes because these rotations are
// car-specific.
const std::array<float, 11> kSlotRotation = {
    0.0f, 0.0f, 0.0f, 180.0f, 90.0f, -90.0f, 90.0f, 0.0f, 0.0f, 180.0f, 0.0f,
};

// The whole 11-slot body is emitted, then its final 17 bytes are dropped (the
// last slot's remnant is truncated to a single byte). Verified byte-exact.
constexpr int kLiveryBodyTruncate = 17;
constexpr int kLiverySectionCount = 11;
constexpr int kLiveryEmptySlotBytes = 23;  // constant scaffold size of an empty slot
constexpr int kMaxDirectChildren = 0xff * 8;

struct LiveryEntry {
    enum Kind { Group, Shape } kind = Shape;
    const scene::Group *group = nullptr;
    const scene::Shape *shape = nullptr;
};

struct LiveryExportShape {
    const scene::Shape *node = nullptr;
    quint16 shapeId = 0;
    double rotation = 0.0;
    double x = 0.0;
    double y = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double skew = 0.0;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool mask = false;
};

Matrix3 nodeMatrix(const VinylGroup &node)
{
    constexpr double pi = 3.14159265358979323846;
    const double radians = node.rot * pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return affine(c * node.sx, -s * node.sy, node.px,
                  s * node.sx, c * node.sy, node.py);
}

Matrix3 shapeMatrixForShape(const VinylShape &shape)
{
    FlattenedLayer layer;
    layer.rotation = shape.rotation;
    layer.posX = shape.posX;
    layer.posY = shape.posY;
    layer.scaleX = shape.scaleX;
    layer.scaleY = shape.scaleY;
    layer.skew = shape.skew;
    return shapeMatrix(layer);
}

struct SourceShapeView {
    quint16 shapeId = 0;
    bool raster = false;
    quint32 rasterId = 0;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool mask = false;
    int absPos = 0;
    QByteArray marker;
    Matrix3 worldMatrix;
    scene::Transform2D world;
};

struct SourceLivery {
    LiveryPayload payload;
    QVector<LiverySection> sections;
};

std::optional<SourceLivery> sourceLivery(const Project &project)
{
    if (project.liverySource.isEmpty()) {
        return std::nullopt;
    }
    try {
        SourceLivery source;
        source.payload = parseInflatedLiveryPayload(project.liverySource);
        source.sections = buildLiverySections(source.payload.body, source.payload.sectionCounts);
        return source.sections.size() == kLiverySectionCount ? std::optional<SourceLivery>(source) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

const LiverySection *sourceSection(const SourceLivery *source, int slot)
{
    if (source == nullptr) {
        return nullptr;
    }
    for (const LiverySection &section : source->sections) {
        if (section.slot == slot) {
            return &section;
        }
    }
    return nullptr;
}

void collectSourceShapes(const VinylGroup &node, const Matrix3 &parentMatrix, bool parentMask,
                         QVector<SourceShapeView> &out)
{
    const Matrix3 groupMatrix = detail::multiply(parentMatrix, nodeMatrix(node));
    const bool inheritedMask = parentMask || node.isMask;
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            const VinylShape &shape = std::get<VinylShape>(item.value);
            SourceShapeView view;
            view.shapeId = shape.shapeId;
            view.raster = shape.isLogo;
            view.rasterId = shape.rasterId;
            view.color = shape.color;
            view.absPos = shape.absPos;
            view.marker = shape.marker;
            view.mask = inheritedMask || shape.isMask;
            if (hasColorData(shape.color)) {
                view.mask = false;
            }
            view.worldMatrix = detail::multiply(groupMatrix, shapeMatrixForShape(shape));
            view.world = decomposeTransform2D(view.worldMatrix);
            out.push_back(view);
        } else {
            collectSourceShapes(*std::get<VinylGroupPtr>(item.value), groupMatrix, inheritedMask, out);
        }
    }
}

QVector<SourceShapeView> sourceSectionShapes(const LiverySection &section)
{
    QVector<SourceShapeView> out;
    collectSourceShapes(section.subtree, Matrix3{}, section.subtree.isMask, out);
    return out;
}

bool closeEnough(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

bool matrixClose(const Matrix3 &a, const Matrix3 &b)
{
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            const double magnitude = std::max(std::abs(a.m[row][col]), std::abs(b.m[row][col]));
            const double tolerance = col == 2 ? 0.5 : 0.01 + 0.001 * magnitude;
            if (!closeEnough(a.m[row][col], b.m[row][col], tolerance)) {
                return false;
            }
        }
    }
    return true;
}

bool rotationClose(double a, double b)
{
    double diff = std::fmod(std::abs(normalizeRotation(a) - normalizeRotation(b)), 360.0);
    if (diff > 180.0) {
        diff = 360.0 - diff;
    }
    return diff <= 0.5;
}

bool transformClose(const scene::Transform2D &a, const scene::Transform2D &b)
{
    return closeEnough(a.x, b.x, 0.5)
        && closeEnough(a.y, b.y, 0.5)
        && closeEnough(std::abs(a.scaleX), std::abs(b.scaleX), 0.01 + 0.001 * std::abs(b.scaleX))
        && closeEnough(std::abs(a.scaleY), std::abs(b.scaleY), 0.01 + 0.001 * std::abs(b.scaleY))
        && closeEnough(a.skew, b.skew, 0.01)
        && rotationClose(a.rotation, b.rotation);
}

bool reliableTransform(const scene::Transform2D &t)
{
    return std::isfinite(t.x) && std::isfinite(t.y)
        && std::isfinite(t.scaleX) && std::isfinite(t.scaleY)
        && std::isfinite(t.rotation) && std::isfinite(t.skew)
        && std::abs(t.scaleX) < 1.0e8
        && std::abs(t.scaleY) < 1.0e8;
}

bool matchesSourceShape(const scene::Shape &shape, const SourceShapeView &source)
{
    const scene::Transform2D world = decomposeTransform2D(shape.worldMatrix());
    const bool transformMatches = shape.hasSourceTransform
        ? (!reliableTransform(shape.sourceTransform) || transformClose(world, shape.sourceTransform))
        : (source.color[3] == 0
           || matrixClose(shape.worldMatrix(), source.worldMatrix)
           || transformClose(world, source.world));
    // A raster (logo) shape is identified by its rasterId, not its vector shapeId:
    // the imported scene zeroes shapeId for rasters, while the source view keeps the
    // raw logo id (e.g. 0/42773 for the same 0xa715 decal). Compare shapeId only for
    // vector shapes; rasterId already anchors raster identity.
    const bool idMatches = shape.raster ? true : shape.shapeId == source.shapeId;
    return idMatches
        && shape.raster == source.raster
        && shape.rasterId == source.rasterId
        && shape.color == source.color
        && shape.mask == source.mask
        && transformMatches;
}

QByteArray sourceSlotBytes(const SourceLivery *source, int slot);

bool sectionMatchesSource(const QVector<const scene::Shape *> &shapes, const LiverySection *section)
{
    if (section == nullptr) {
        return false;
    }
    const QVector<SourceShapeView> sourceShapes = sourceSectionShapes(*section);
    if (shapes.size() != sourceShapes.size()) {
        return false;
    }
    for (int i = 0; i < shapes.size(); ++i) {
        const scene::Shape *shape = shapes[i];
        if (shape == nullptr) {
            return false;
        }
        if (!matchesSourceShape(*shape, sourceShapes[i])) {
            return false;
        }
    }
    return true;
}

QByteArray sourceSlotBytes(const SourceLivery *source, int slot)
{
    const LiverySection *section = sourceSection(source, slot);
    if (source == nullptr || section == nullptr || section->absPos < 0
        || section->absPos > source->payload.body.size()) {
        return {};
    }
    int end = source->payload.body.size();
    if (slot + 1 < kLiverySectionCount) {
        if (const LiverySection *next = sourceSection(source, slot + 1)) {
            end = next->absPos;
        }
    }
    if (end < section->absPos) {
        return {};
    }
    QByteArray bytes = source->payload.body.mid(section->absPos, end - section->absPos);
    if (slot == kLiverySectionCount - 1) {
        bytes.append(QByteArray(kLiveryBodyTruncate, '\0'));
    }
    return bytes;
}

QByteArray defaultRemnant(int slot)
{
    QByteArray out(9, '\0');
    detail::appendLeFloat(out, 1.0f);
    detail::appendLeFloat(out, kSlotRotation[slot]);
    out.append('\0');
    return out;
}

QByteArray sourceRemnant(const SourceLivery *source, int slot)
{
    const QByteArray slotBytes = sourceSlotBytes(source, slot);
    if (slotBytes.size() >= 18) {
        return slotBytes.right(18);
    }
    return defaultRemnant(slot);
}

QByteArray sourceEmptySlot(const SourceLivery *source, int slot)
{
    const QByteArray slotBytes = sourceSlotBytes(source, slot);
    if (slotBytes.size() >= 23) {
        return slotBytes.left(23);
    }
    QByteArray out(5, '\0');
    out.append(sourceRemnant(source, slot));
    return out;
}

LiveryExportShape exportLiveryShape(const scene::Shape &shape)
{
    const scene::Transform2D t = decomposeTransform2D(shape.worldMatrix());
    LiveryExportShape out;
    out.node = &shape;
    out.shapeId = shape.shapeId;
    out.rotation = t.rotation;
    out.x = t.x;
    out.y = t.y;
    out.scaleX = t.scaleX;
    out.scaleY = t.scaleY;
    out.skew = t.skew;
    out.color = shape.color;
    out.color[3] = static_cast<quint8>(std::clamp<int>(std::lround(shape.opacity * 255.0), 0, 255));
    out.mask = shape.mask;
    return out;
}

void appendLiveryShapeRecord(QByteArray &out, const LiveryExportShape &shape, double offsetX,
                             double offsetY, quint8 lead)
{
    out.append(static_cast<char>(lead));
    out.append('\x02');
    detail::appendLeU16(out, shape.shapeId);
    detail::appendLeFloat(out, static_cast<float>(normalizeRotation(shape.rotation)));
    detail::appendLeFloat(out, static_cast<float>(shape.x - offsetX));
    detail::appendLeFloat(out, static_cast<float>(shape.y - offsetY));
    detail::appendLeFloat(out, static_cast<float>(shape.scaleX));
    detail::appendLeFloat(out, static_cast<float>(shape.scaleY));
    detail::appendLeFloat(out, static_cast<float>(shape.skew));
    out.append(static_cast<char>(shape.color[0]));
    out.append(static_cast<char>(shape.color[1]));
    out.append(static_cast<char>(shape.color[2]));
    out.append(static_cast<char>(shape.color[3]));
}

int checkLiveryChildCount(int count, const char *label)
{
    if (count <= 0) {
        throw std::runtime_error(std::string(label) + " has no visible children");
    }
    if (count > kMaxDirectChildren) {
        throw std::runtime_error(std::string(label) + " has too many direct children for the child bitmap");
    }
    const int blocks = (count + 7) / 8;
    if (blocks > 0xff) {
        throw std::runtime_error(std::string(label) + " needs too many child bitmap blocks");
    }
    return blocks;
}

void collectSectionShapes(const scene::Layer &node, QVector<const scene::Shape *> &out)
{
    if (node.kind() == scene::LayerKind::Shape) {
        const auto &shape = static_cast<const scene::Shape &>(node);
        if (shape.visible) {
            out.push_back(&shape);
        }
        return;
    }
    if (node.kind() == scene::LayerKind::Group) {
        if (!node.visible) {
            return;
        }
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectSectionShapes(*child, out);
        }
    }
}

void collectVisibleShapeLeaves(const scene::Layer &node, QVector<const scene::Shape *> &out)
{
    if (!node.visible) {
        return;
    }
    if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(static_cast<const scene::Shape *>(&node));
        return;
    }
    if (node.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectVisibleShapeLeaves(*child, out);
        }
    }
}

void entryShapes(const LiveryEntry &entry, QVector<const scene::Shape *> &out)
{
    if (entry.kind == LiveryEntry::Shape) {
        if (entry.shape != nullptr && entry.shape->visible) {
            out.push_back(entry.shape);
        }
        return;
    }
    if (entry.group != nullptr) {
        collectVisibleShapeLeaves(*entry.group, out);
    }
}

bool hasVisibleShapeLeaves(const scene::Layer &node)
{
    QVector<const scene::Shape *> leaves;
    collectVisibleShapeLeaves(node, leaves);
    return !leaves.isEmpty();
}

QVector<LiveryEntry> directVisibleChildren(const scene::Group &group)
{
    QVector<LiveryEntry> entries;
    for (const auto &child : group.children) {
        if (!child->visible) {
            continue;
        }
        if (child->kind() == scene::LayerKind::Shape) {
            entries.push_back({LiveryEntry::Shape, nullptr, static_cast<const scene::Shape *>(child.get())});
        } else if (child->kind() == scene::LayerKind::Group && hasVisibleShapeLeaves(*child)) {
            const auto *childGroup = static_cast<const scene::Group *>(child.get());
            const QVector<LiveryEntry> normalizedChildren = directVisibleChildren(*childGroup);
            if (normalizedChildren.isEmpty()) {
                continue;
            }
            if (normalizedChildren.size() == 1) {
                entries.push_back(normalizedChildren.front());
            } else {
                entries.push_back({LiveryEntry::Group, childGroup, nullptr});
            }
        }
    }
    return entries;
}

QPointF shapeHalfExtents(const LiveryExportShape &shape)
{
    if (shape.node != nullptr && shape.node->raster) {
        double w = std::max(1, shape.node->rasterWidth);
        double h = std::max(1, shape.node->rasterHeight);
        const double side = std::max(w, h);
        return QPointF(64.0 * w / side, 64.0 * h / side);
    }
    return QPointF(64.0, 64.0);
}

QVector<QPointF> shapeCorners(const scene::Shape &shape)
{
    const LiveryExportShape exported = exportLiveryShape(shape);
    const QPointF he = shapeHalfExtents(exported);
    FlattenedLayer layer;
    layer.rotation = exported.rotation;
    layer.posX = exported.x;
    layer.posY = exported.y;
    layer.scaleX = exported.scaleX;
    layer.scaleY = exported.scaleY;
    layer.skew = exported.skew;
    const Matrix3 matrix = shapeMatrix(layer);

    QVector<QPointF> corners;
    const double pts[4][2] = {{-he.x(), -he.y()}, {he.x(), -he.y()}, {he.x(), he.y()}, {-he.x(), he.y()}};
    for (const auto &pt : pts) {
        corners.push_back(QPointF(matrix.m[0][0] * pt[0] + matrix.m[0][1] * pt[1] + matrix.m[0][2],
                                  matrix.m[1][0] * pt[0] + matrix.m[1][1] * pt[1] + matrix.m[1][2]));
    }
    return corners;
}

QPointF shapesOrigin(const QVector<const scene::Shape *> &leaves)
{
    if (leaves.isEmpty()) {
        return QPointF(0.0, 0.0);
    }
    QVector<QPointF> points;
    for (const scene::Shape *shape : leaves) {
        if (shape != nullptr) {
            points.append(shapeCorners(*shape));
        }
    }
    if (points.isEmpty()) {
        const LiveryExportShape first = exportLiveryShape(*leaves.front());
        return QPointF(first.x, first.y);
    }

    double minX = points.front().x();
    double maxX = minX;
    double minY = points.front().y();
    double maxY = minY;
    for (const QPointF &point : points) {
        minX = std::min(minX, point.x());
        maxX = std::max(maxX, point.x());
        minY = std::min(minY, point.y());
        maxY = std::max(maxY, point.y());
    }
    return QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
}

QPointF entryOrigin(const LiveryEntry &entry)
{
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    return shapesOrigin(leaves);
}

bool entryAllMasked(const LiveryEntry &entry)
{
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    if (leaves.isEmpty()) {
        return false;
    }
    for (const scene::Shape *shape : leaves) {
        if (shape == nullptr || !shape->mask) {
            return false;
        }
    }
    return true;
}

QByteArray childBitmap(const QVector<LiveryEntry> &items, const char *label)
{
    const int blocks = checkLiveryChildCount(items.size(), label);
    QByteArray bitmap(blocks, '\0');
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].kind == LiveryEntry::Group) {
            bitmap[i / 8] = static_cast<char>(static_cast<quint8>(bitmap[i / 8]) | (1 << (i % 8)));
        }
    }
    return bitmap;
}

QByteArray packMarkerlessGroupHeader(const QVector<LiveryEntry> &children, const char *label)
{
    const QByteArray bitmap = childBitmap(children, label);
    QByteArray out;
    detail::appendLeU16(out, static_cast<quint16>(children.size()));
    out.append(static_cast<char>(bitmap.size()));
    out.append(bitmap);
    out.append(QByteArray(2, '\0'));
    return out;
}

QByteArray packCountedGroupHeader(const QVector<LiveryEntry> &children, bool mask, const char *label)
{
    const QByteArray bitmap = childBitmap(children, label);
    QByteArray out;
    out.append(mask ? '\x60' : '\x20');
    detail::appendLeU16(out, static_cast<quint16>(children.size()));
    out.append(static_cast<char>(bitmap.size()));
    out.append(bitmap);
    out.append(QByteArray(2, '\0'));
    return out;
}

QByteArray siblingGroupTransformMarker(int previousGroupDepth)
{
    QByteArray out;
    out.append('\x00');
    out.append(QByteArray(std::max(1, previousGroupDepth), '\x01'));
    out.append('\x03');
    return out;
}

QByteArray maybeFlipMaskFlag(QByteArray marker, bool previousShapeMask)
{
    if (previousShapeMask && !marker.isEmpty() && static_cast<quint8>(marker[0]) == 0x00) {
        marker[0] = '\x01';
    }
    return marker;
}

QByteArray liveryTransformMarker(QByteArray standaloneMarker)
{
    if (standaloneMarker.isEmpty()) {
        return standaloneMarker;
    }
    const quint8 first = static_cast<quint8>(standaloneMarker[0]);
    if (first == 0x00 || first == 0x01) {
        return standaloneMarker.left(1);
    }
    if (static_cast<quint8>(standaloneMarker.back()) == 0x03) {
        standaloneMarker[standaloneMarker.size() - 1] = '\x01';
    }
    return standaloneMarker;
}

QByteArray packLiveryGroupTransform(double x, double y, const QByteArray &marker)
{
    QByteArray out = marker;
    detail::appendLeFloat(out, static_cast<float>(x));
    detail::appendLeFloat(out, static_cast<float>(y));
    detail::appendLeFloat(out, 1.0f);
    detail::appendLeFloat(out, 0.0f);
    return out;
}

QVector<LiveryEntry> childrenForEntry(const LiveryEntry &entry)
{
    if (entry.kind != LiveryEntry::Group || entry.group == nullptr) {
        throw std::runtime_error("livery shape entries cannot have children");
    }
    return directVisibleChildren(*entry.group);
}

int terminalDepth(const LiveryEntry &entry)
{
    if (entry.kind != LiveryEntry::Group) {
        return 1;
    }
    const QVector<LiveryEntry> children = childrenForEntry(entry);
    if (children.isEmpty() || children.back().kind != LiveryEntry::Group) {
        return 1;
    }
    return 1 + terminalDepth(children.back());
}

QByteArray packLiveryGroup(const LiveryEntry &entry, QPointF parentOffset, const QByteArray &transformMarker,
                           bool parentMask, bool &previousShapeMask)
{
    const QVector<LiveryEntry> children = childrenForEntry(entry);
    if (children.isEmpty()) {
        throw std::runtime_error("livery group has no visible children");
    }
    const bool isMaskGroup = entryAllMasked(entry);
    const bool childMask = parentMask || isMaskGroup;
    const QPointF origin = entryOrigin(entry);

    QByteArray out = packLiveryGroupTransform(origin.x() - parentOffset.x(),
                                             origin.y() - parentOffset.y(),
                                             liveryTransformMarker(transformMarker));
    out.append(packCountedGroupHeader(children, isMaskGroup, "livery group"));

    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    bool hasPreviousSibling = false;
    for (const LiveryEntry &child : children) {
        if (child.kind == LiveryEntry::Group) {
            QByteArray marker;
            if (previousWasGroup) {
                marker = siblingGroupTransformMarker(previousGroupDepth);
            } else if (hasPreviousSibling) {
                marker = QByteArray("\x00\x03", 2);
            } else {
                marker = QByteArray("\x03", 1);
            }
            marker = maybeFlipMaskFlag(marker, previousShapeMask);
            previousShapeMask = false;
            out.append(packLiveryGroup(child, origin, marker, childMask, previousShapeMask));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(child);
        } else if (child.shape != nullptr) {
            if (previousWasGroup) {
                out.append('\x00');
                out.append(QByteArray(std::max(0, previousGroupDepth - 1), '\x01'));
            }
            const LiveryExportShape packed = exportLiveryShape(*child.shape);
            const quint8 lead = (previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            appendLiveryShapeRecord(out, packed, origin.x(), origin.y(), lead);
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = childMask || packed.mask;
        }
        hasPreviousSibling = true;
    }
    return out;
}

void appendStructuralSection(QByteArray &body, const scene::Group *sectionGroup,
                              const SourceLivery *source, int slot)
{
    if (sectionGroup == nullptr) {
        body.append(sourceEmptySlot(source, slot));
        return;
    }
    const QVector<LiveryEntry> children = directVisibleChildren(*sectionGroup);
    if (children.isEmpty()) {
        body.append(sourceEmptySlot(source, slot));
        return;
    }

    body.append(packMarkerlessGroupHeader(children, "livery section"));
    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    bool previousShapeMask = false;
    bool hasPreviousSibling = false;
    const QPointF sectionOrigin(0.0, 0.0);
    for (const LiveryEntry &child : children) {
        if (child.kind == LiveryEntry::Group) {
            QByteArray marker;
            if (previousWasGroup) {
                marker = siblingGroupTransformMarker(previousGroupDepth);
            } else if (hasPreviousSibling) {
                marker = QByteArray("\x00\x03", 2);
            } else {
                marker = QByteArray("\x03", 1);
            }
            marker = maybeFlipMaskFlag(marker, previousShapeMask);
            previousShapeMask = false;
            body.append(packLiveryGroup(child, sectionOrigin, marker, false, previousShapeMask));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(child);
        } else if (child.shape != nullptr) {
            if (previousWasGroup) {
                body.append('\x00');
                body.append(QByteArray(std::max(0, previousGroupDepth - 1), '\x01'));
            }
            const LiveryExportShape packed = exportLiveryShape(*child.shape);
            const quint8 lead = (previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            appendLiveryShapeRecord(body, packed, sectionOrigin.x(), sectionOrigin.y(), lead);
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = packed.mask;
        }
        hasPreviousSibling = true;
    }
    body.append(sourceRemnant(source, slot));
}

} // namespace

QByteArray buildLiveryGyvl(const Project &project, std::array<int, kLiverySectionCount> *outSectionCounts)
{
    // Gather each section slot's group and flattened shapes (in tree order). The
    // flat list is only for source-byte preservation; changed slots use the group.
    std::array<const scene::Group *, kLiverySectionCount> slotGroups{};
    std::array<QVector<const scene::Shape *>, kLiverySectionCount> slotShapes;
    if (project.root) {
        for (const auto &child : project.root->children) {
            if (child->kind() != scene::LayerKind::Group) {
                continue;
            }
            const auto &group = static_cast<const scene::Group &>(*child);
            const int slot = group.liverySectionSlot;
            if (group.isLiverySection && slot >= 0 && slot < kLiverySectionCount) {
                slotGroups[slot] = &group;
                collectSectionShapes(group, slotShapes[slot]);
            }
        }
    }

    const std::optional<SourceLivery> source = sourceLivery(project);
    const SourceLivery *sourcePtr = source ? &*source : nullptr;
    // Per-slot count of decals actually emitted into the body. This MUST feed the
    // trailing yrvl "stats" chunk verbatim: if the declared count disagrees with
    // the decals physically present in the gyvl, the game reads the section as one
    // opaque locked group instead of individual shapes.
    std::array<int, kLiverySectionCount> counts{};
    QByteArray body;
    for (int slot = 0; slot < kLiverySectionCount; ++slot) {
        const QVector<const scene::Shape *> &shapes = slotShapes[slot];
        const LiverySection *section = sourceSection(sourcePtr, slot);
        const int sourceDecals = (sourcePtr && slot < sourcePtr->payload.sectionCounts.size())
                                     ? sourcePtr->payload.sectionCounts[slot]
                                     : static_cast<int>(shapes.size());

        // An unchanged slot is preserved byte-for-byte. We deliberately do NOT try to
        // rebuild a slot whose decode falls short of its source decal count (a grammar
        // deficit, e.g. Livery_1069 Top decoding 304 of 400): our section re-encoder is
        // not yet game-valid for production sections -- a rebuilt gyvl round-trips
        // through our own decoder but the game rejects it (won't load). So we keep the
        // exact source bytes and report the SOURCE decal count, which reproduces the
        // original slot faithfully and keeps gyvl and stats consistent.
        if (sectionMatchesSource(shapes, section)) {
            const QByteArray preserved = sourceSlotBytes(sourcePtr, slot);
            // Every one of the 11 slots must be emitted. An empty slot is a constant
            // 23-byte scaffold; if the preserved bytes are missing (e.g. a source whose
            // section boundary landed at the body end), synthesize the scaffold so the
            // trailing empty slots are never dropped -- a dropped slot shifts every
            // following slot and corrupts the file.
            if (shapes.isEmpty() && preserved.size() < kLiveryEmptySlotBytes) {
                body.append(sourceEmptySlot(sourcePtr, slot));
                counts[static_cast<size_t>(slot)] = 0;
            } else {
                body.append(preserved);
                counts[static_cast<size_t>(slot)] = sourceDecals;
            }
            continue;
        }

        // A genuinely edited slot is rebuilt from its decoded shapes. Custom-logo
        // (raster) decals cannot be synthesized yet.
        for (const scene::Shape *shape : shapes) {
            if (shape != nullptr && shape->raster) {
                throw std::runtime_error("changed custom logo decals cannot be synthesized yet");
            }
        }
        appendStructuralSection(body, slotGroups[slot], sourcePtr, slot);
        counts[static_cast<size_t>(slot)] = static_cast<int>(shapes.size());
    }
    body = body.left(std::max<qsizetype>(0, body.size() - kLiveryBodyTruncate));

    if (outSectionCounts != nullptr) {
        *outSectionCounts = counts;
    }

    QByteArray chunk(reinterpret_cast<const char *>(kGyvlHeader), 0x15);
    chunk.append(body);
    return chunk;
}

} // namespace fh6
