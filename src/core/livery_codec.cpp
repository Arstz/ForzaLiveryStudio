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

QString resolveLiveryPath(const QString &folderOrFile) {
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        return QDir(folderOrFile).filePath(QStringLiteral("C_livery"));
    }
    return folderOrFile;
}

quint64 readLeU64(const QByteArray &bytes, int offset) {
    return static_cast<quint64>(detail::readLeU32(bytes, offset))
        | (static_cast<quint64>(detail::readLeU32(bytes, offset + 4)) << 32);
}

LiveryPaintState readPaintState(const QByteArray &raw, int end) {
    LiveryPaintState state;
    const int firstCandidate = std::max(0, end - 4096);
    for (int start = end - 102; start >= firstCandidate; --start) {
        if (start + 10 > end
            || static_cast<quint8>(raw[start]) > 1
            || static_cast<quint8>(raw[start + 1]) != 2) {
            continue;
        }
        const int count = detail::readLeU16(raw, start + 2);
        if (count <= 0 || count > 256 || start + 102 + count * 27 != end) {
            continue;
        }
        state.materials.reserve(count);
        int pos = start + 10;
        for (int i = 0; i < count; ++i, pos += 27) {
            LiveryPaintMaterial material;
            material.materialHash = readLeU64(raw, pos);
            material.primary.enabled = raw[pos + 9] != 0;
            for (int channel = 0; channel < 4; ++channel) {
                material.primary.bgra[channel] = static_cast<quint8>(raw[pos + 10 + channel]);
            }
            material.secondary.enabled = raw[pos + 14] != 0;
            for (int channel = 0; channel < 4; ++channel) {
                material.secondary.bgra[channel] = static_cast<quint8>(raw[pos + 15 + channel]);
            }
            material.manufacturerSelector = detail::readLeU32(raw, pos + 19);
            material.finish = detail::readLeU32(raw, pos + 23);
            state.materials.push_back(material);
        }
        return state;
    }
    return state;
}

LiveryPayload parseInflatedLiveryPayloadImpl(const QByteArray &raw) {
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
    const int paintTag = payload.raw.indexOf(QByteArray("yrvl", 4), statsTag + 4);
    const int paintEnd = paintTag >= 0
        ? payload.raw.indexOf(QByteArray("yrvl", 4), paintTag + 4)
        : -1;
    if (paintEnd >= 0) {
        payload.paint = readPaintState(payload.raw, paintEnd);
    }
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

LiveryPayload parseInflatedLiveryPayload(const QByteArray &raw) {
    return parseInflatedLiveryPayloadImpl(raw);
}

LiveryPayload readLiveryPayload(const QString &folderOrFile) {
    QFile file(resolveLiveryPath(folderOrFile));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open C_livery: " + file.fileName()).toStdString());
    }

    return parseInflatedLiveryPayload(inflateContainer(file.readAll()));
}

namespace {

const unsigned char kGyvlHeader[0x15] = {
    0x67, 0x79, 0x76, 0x6c,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3f,
    0x00, 0x00, 0x00, 0x00,
    0x00,
};

// Source-less projects use canonical panel rotations.
const std::array<float, 11> kSlotRotation = {
    0.0f, 0.0f, 0.0f, 180.0f, 90.0f, -90.0f, 90.0f, 0.0f, 0.0f, 180.0f, 0.0f,
};

const std::array<float, 11> kEmptySlotRotation = {
    0.0f, 0.0f, 0.0f, 0.0f, 180.0f, 90.0f, -90.0f, 90.0f, 0.0f, 0.0f, 180.0f,
};

constexpr int kLiveryBodyTruncate = 17;
constexpr int kLiverySectionCount = 11;
constexpr int kLiveryEmptySlotBytes = 23;
constexpr int kMaxDirectChildren = 0xffff;
constexpr bool kFlattenLiverySections = false;

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

Matrix3 nodeMatrix(const VinylGroup &node) {
    constexpr double pi = 3.14159265358979323846;
    const double radians = node.rot * pi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return affine(c * node.sx, -s * node.sy, node.px,
                  s * node.sx, c * node.sy, node.py);
}

Matrix3 shapeMatrixForShape(const VinylShape &shape) {
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

std::optional<SourceLivery> sourceLivery(const Project &project) {
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

const LiverySection *sourceSection(const SourceLivery *source, int slot) {
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
                         QVector<SourceShapeView> &out) {
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
            view.mask = inheritedMask || (shape.isMask && !hasColorData(shape.color));
            view.worldMatrix = detail::multiply(groupMatrix, shapeMatrixForShape(shape));
            view.world = decomposeTransform2D(view.worldMatrix);
            out.push_back(view);
        } else {
            collectSourceShapes(*std::get<VinylGroupPtr>(item.value), groupMatrix, inheritedMask, out);
        }
    }
}

QVector<SourceShapeView> sourceSectionShapes(const LiverySection &section) {
    QVector<SourceShapeView> out;
    collectSourceShapes(section.subtree, Matrix3{}, section.subtree.isMask, out);
    return out;
}

bool closeEnough(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

bool matrixClose(const Matrix3 &a, const Matrix3 &b) {
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

bool rotationClose(double a, double b) {
    double diff = std::fmod(std::abs(normalizeRotation(a) - normalizeRotation(b)), 360.0);
    if (diff > 180.0) {
        diff = 360.0 - diff;
    }
    return diff <= 0.5;
}

bool transformClose(const scene::Transform2D &a, const scene::Transform2D &b) {
    return closeEnough(a.x, b.x, 0.5)
        && closeEnough(a.y, b.y, 0.5)
        && closeEnough(std::abs(a.scaleX), std::abs(b.scaleX), 0.01 + 0.001 * std::abs(b.scaleX))
        && closeEnough(std::abs(a.scaleY), std::abs(b.scaleY), 0.01 + 0.001 * std::abs(b.scaleY))
        && closeEnough(a.skew, b.skew, 0.01)
        && rotationClose(a.rotation, b.rotation);
}

bool reliableTransform(const scene::Transform2D &t) {
    return std::isfinite(t.x) && std::isfinite(t.y)
        && std::isfinite(t.scaleX) && std::isfinite(t.scaleY)
        && std::isfinite(t.rotation) && std::isfinite(t.skew)
        && std::abs(t.scaleX) < 1.0e8
        && std::abs(t.scaleY) < 1.0e8;
}

bool matchesSourceShape(const scene::Shape &shape, const SourceShapeView &source) {
    const scene::Transform2D world = decomposeTransform2D(shape.worldMatrix());
    const bool transformMatches = shape.hasSourceTransform
        ? (!reliableTransform(shape.sourceTransform) || transformClose(world, shape.sourceTransform))
        : (source.color[3] == 0
           || matrixClose(shape.worldMatrix(), source.worldMatrix)
           || transformClose(world, source.world));
    // Raster and vector identities use separate ID domains.
    const bool idMatches = shape.raster ? true : shape.shapeId == source.shapeId;
    return idMatches
        && shape.raster == source.raster
        && shape.rasterId == source.rasterId
        && shape.color == source.color
        && shape.mask == source.mask
        && transformMatches;
}

QByteArray sourceSlotBytes(const SourceLivery *source, int slot);

bool decodedSectionHasArtworkGroup(const LiverySection *section) {
    if (section == nullptr) {
        return false;
    }
    for (const VinylItem &item : section->subtree.items) {
        if (item.isShape()) {
            continue;
        }
        const VinylGroup &root = *std::get<VinylGroupPtr>(item.value);
        for (const VinylItem &rootItem : root.items) {
            if (!rootItem.isShape()) {
                return true;
            }
        }
    }
    return false;
}

bool sectionMatchesSource(const QVector<const scene::Shape *> &shapes, const LiverySection *section,
                          bool hasGroupedArtwork) {
    if (section == nullptr || hasGroupedArtwork != decodedSectionHasArtworkGroup(section)) {
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

QByteArray sourceSlotBytes(const SourceLivery *source, int slot) {
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

QByteArray defaultRemnant(int slot) {
    QByteArray out(9, '\0');
    detail::appendLeFloat(out, 1.0f);
    detail::appendLeFloat(out, kSlotRotation[slot]);
    out.append('\0');
    return out;
}

QByteArray sourceRemnant(const SourceLivery *source, int slot) {
    const QByteArray slotBytes = sourceSlotBytes(source, slot);
    if (slotBytes.size() >= 18) {
        return slotBytes.right(18);
    }
    return defaultRemnant(slot);
}

bool sourceHasArtworkGroup(const SourceLivery *source, int slot) {
    return decodedSectionHasArtworkGroup(sourceSection(source, slot));
}

QByteArray defaultEmptySlot(int slot) {
    QByteArray out(8, '\0');
    detail::appendLeFloat(out, 1.0f);
    detail::appendLeFloat(out, kEmptySlotRotation[slot]);
    out.append(QByteArray(7, '\0'));
    return out;
}

bool isEmptySlotRecord(const QByteArray &bytes) {
    return bytes.size() >= kLiveryEmptySlotBytes
        && bytes.left(8) == QByteArray(8, '\0')
        && bytes.mid(8, 4) == QByteArray("\x00\x00\x80\x3f", 4)
        && bytes.mid(16, 7) == QByteArray(7, '\0');
}

QByteArray sourceEmptySlot(const SourceLivery *source, int slot) {
    const QByteArray slotBytes = sourceSlotBytes(source, slot);
    if (isEmptySlotRecord(slotBytes)) {
        return slotBytes.left(23);
    }
    return defaultEmptySlot(slot);
}

LiveryExportShape exportLiveryShape(const scene::Shape &shape, const Matrix3 &worldAdjustment = Matrix3{}) {
    const Matrix3 worldMatrix = detail::multiply(worldAdjustment, shape.worldMatrix());
    const scene::Transform2D t = decomposeTransform2D(worldMatrix);
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
                             double offsetY, quint8 lead, bool bare) {
    if (bare) {
        out.append('\x02');
    } else {
        out.append(static_cast<char>(lead));
        out.append('\x02');
    }
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

int checkLiveryChildCount(int count, const char *label) {
    if (count <= 0) {
        throw std::runtime_error(std::string(label) + " has no visible children");
    }
    if (count > kMaxDirectChildren) {
        throw std::runtime_error(std::string(label) + " has too many direct children for the child bitmap");
    }
    const int blocks = (count + 7) / 8;
    return blocks;
}

void collectSectionShapes(const scene::Layer &node, QVector<const scene::Shape *> &out) {
    if (node.kind() == scene::LayerKind::Shape) {
        const auto &shape = static_cast<const scene::Shape &>(node);
        out.push_back(&shape);
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

void collectVisibleShapeLeaves(const scene::Layer &node, QVector<const scene::Shape *> &out) {
    if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(static_cast<const scene::Shape *>(&node));
        return;
    }
    if (!node.visible) {
        return;
    }
    if (node.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectVisibleShapeLeaves(*child, out);
        }
    }
}

void entryShapes(const LiveryEntry &entry, QVector<const scene::Shape *> &out) {
    if (entry.kind == LiveryEntry::Shape) {
        if (entry.shape != nullptr) {
            out.push_back(entry.shape);
        }
        return;
    }
    if (entry.group != nullptr) {
        collectVisibleShapeLeaves(*entry.group, out);
    }
}

QByteArray defaultGroupedRemnant(int slot, const QVector<LiveryEntry> &entries) {
    QVector<const scene::Shape *> shapes;
    for (const LiveryEntry &entry : entries) {
        entryShapes(entry, shapes);
    }
    const quint16 shapeId = shapes.isEmpty() ? 0 : shapes.back()->shapeId;
    QByteArray out(1, '\0');
    detail::appendLeU16(out, shapeId);
    out.append(QByteArray(6, '\0'));
    detail::appendLeFloat(out, 1.0f);
    detail::appendLeFloat(out, kSlotRotation[slot]);
    out.append('\x80');
    return out;
}

QByteArray groupedRemnant(const SourceLivery *source, int slot,
                          const QVector<LiveryEntry> &entries) {
    const QByteArray preserved = sourceRemnant(source, slot);
    if (sourceHasArtworkGroup(source, slot) && preserved.size() == 18) {
        return preserved;
    }
    return defaultGroupedRemnant(slot, entries);
}

bool hasVisibleShapeLeaves(const scene::Layer &node) {
    QVector<const scene::Shape *> leaves;
    collectVisibleShapeLeaves(node, leaves);
    return !leaves.isEmpty();
}

QVector<LiveryEntry> directVisibleChildren(const scene::Group &group) {
    QVector<LiveryEntry> entries;
    for (const auto &child : group.children) {
        if (child->kind() == scene::LayerKind::Shape) {
            entries.push_back({LiveryEntry::Shape, nullptr, static_cast<const scene::Shape *>(child.get())});
        } else if (child->kind() == scene::LayerKind::Group
                   && child->visible && hasVisibleShapeLeaves(*child)) {
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

QVector<LiveryEntry> liverySectionChildren(const scene::Group &group) {
    const QVector<LiveryEntry> structured = directVisibleChildren(group);
    if (!kFlattenLiverySections) {
        return structured;
    }
    QVector<const scene::Shape *> shapes;
    collectVisibleShapeLeaves(group, shapes);
    const bool hasMasks = std::any_of(shapes.cbegin(), shapes.cend(), [](const scene::Shape *shape) {
        return shape != nullptr && shape->mask;
    });
    if (hasMasks) {
        return structured;
    }
    QVector<LiveryEntry> flattened;
    flattened.reserve(shapes.size());
    for (const scene::Shape *shape : shapes) {
        flattened.push_back({LiveryEntry::Shape, nullptr, shape});
    }
    return flattened;
}

bool hasGroupedArtwork(const scene::Group *sectionGroup) {
    if (sectionGroup == nullptr) {
        return false;
    }
    const QVector<LiveryEntry> children = liverySectionChildren(*sectionGroup);
    return std::any_of(children.cbegin(), children.cend(), [](const LiveryEntry &entry) {
        return entry.kind == LiveryEntry::Group;
    });
}

QPointF shapeHalfExtents(const LiveryExportShape &shape) {
    if (shape.node != nullptr && shape.node->raster) {
        double w = std::max(1, shape.node->rasterWidth);
        double h = std::max(1, shape.node->rasterHeight);
        const double side = std::max(w, h);
        return QPointF(64.0 * w / side, 64.0 * h / side);
    }
    return QPointF(64.0, 64.0);
}

QVector<QPointF> shapeCorners(const scene::Shape &shape, const Matrix3 &worldAdjustment) {
    const LiveryExportShape exported = exportLiveryShape(shape, worldAdjustment);
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

QPointF shapesOrigin(const QVector<const scene::Shape *> &leaves, const Matrix3 &worldAdjustment) {
    if (leaves.isEmpty()) {
        return QPointF(0.0, 0.0);
    }
    QVector<QPointF> points;
    for (const scene::Shape *shape : leaves) {
        if (shape != nullptr) {
            points.append(shapeCorners(*shape, worldAdjustment));
        }
    }
    if (points.isEmpty()) {
        const LiveryExportShape first = exportLiveryShape(*leaves.front(), worldAdjustment);
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

QPointF entryOrigin(const LiveryEntry &entry, const Matrix3 &worldAdjustment) {
    QVector<const scene::Shape *> leaves;
    entryShapes(entry, leaves);
    return shapesOrigin(leaves, worldAdjustment);
}

bool entryAllMasked(const LiveryEntry &entry) {
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

QByteArray childBitmap(const QVector<LiveryEntry> &items, const char *label) {
    const int blocks = checkLiveryChildCount(items.size(), label);
    QByteArray bitmap(blocks, '\0');
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].kind == LiveryEntry::Group) {
            bitmap[i / 8] = static_cast<char>(static_cast<quint8>(bitmap[i / 8]) | (1 << (i % 8)));
        }
    }
    return bitmap;
}

QByteArray packMarkerlessGroupHeader(const QVector<LiveryEntry> &children, const char *label) {
    const QByteArray bitmap = childBitmap(children, label);
    QByteArray out;
    detail::appendLeU16(out, static_cast<quint16>(children.size()));
    detail::appendLeU16(out, static_cast<quint16>(bitmap.size()));
    out.append(QByteArray(2, '\0'));
    out.append(bitmap);
    return out;
}

QByteArray packCountedGroupHeader(const QVector<LiveryEntry> &children, bool mask, const char *label) {
    const QByteArray bitmap = childBitmap(children, label);
    QByteArray out;
    out.append(mask ? '\x60' : '\x20');
    detail::appendLeU16(out, static_cast<quint16>(children.size()));
    detail::appendLeU16(out, static_cast<quint16>(bitmap.size()));
    out.append(QByteArray(2, '\0'));
    out.append(bitmap);
    return out;
}

QByteArray siblingGroupTransformMarker(int previousGroupDepth) {
    QByteArray out;
    out.append('\x00');
    out.append(QByteArray(std::max(1, previousGroupDepth), '\x01'));
    out.append('\x03');
    return out;
}

QByteArray maybeFlipMaskFlag(QByteArray marker, bool previousShapeMask) {
    if (previousShapeMask && !marker.isEmpty() && static_cast<quint8>(marker[0]) == 0x00) {
        marker[0] = '\x01';
    }
    return marker;
}

QByteArray liveryTransformMarker(QByteArray standaloneMarker) {
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

QByteArray packLiveryGroupTransform(double x, double y, const QByteArray &marker) {
    QByteArray out = marker;
    detail::appendLeFloat(out, static_cast<float>(x));
    detail::appendLeFloat(out, static_cast<float>(y));
    detail::appendLeFloat(out, 1.0f);
    detail::appendLeFloat(out, 0.0f);
    return out;
}

QVector<LiveryEntry> childrenForEntry(const LiveryEntry &entry) {
    if (entry.kind != LiveryEntry::Group || entry.group == nullptr) {
        throw std::runtime_error("livery shape entries cannot have children");
    }
    return directVisibleChildren(*entry.group);
}

int terminalDepth(const LiveryEntry &entry) {
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
                           bool parentMask, bool &previousShapeMask, const Matrix3 &worldAdjustment) {
    const QVector<LiveryEntry> children = childrenForEntry(entry);
    if (children.isEmpty()) {
        throw std::runtime_error("livery group has no visible children");
    }
    const bool isMaskGroup = entryAllMasked(entry);
    const bool childMask = parentMask || isMaskGroup;
    const QPointF origin = entryOrigin(entry, worldAdjustment);

    QByteArray out = packLiveryGroupTransform(origin.x() - parentOffset.x(),
                                             origin.y() - parentOffset.y(),
                                             liveryTransformMarker(transformMarker));
    if (isMaskGroup) {
        out.append(packCountedGroupHeader(children, true, "livery group"));
    } else {
        out.append('\0');
        out.append(packMarkerlessGroupHeader(children, "livery group"));
    }

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
                marker = QByteArray();
            }
            marker = maybeFlipMaskFlag(marker, previousShapeMask);
            previousShapeMask = false;
            out.append(packLiveryGroup(child, origin, marker, childMask, previousShapeMask,
                                       worldAdjustment));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(child);
        } else if (child.shape != nullptr) {
            const bool followsGroup = previousWasGroup;
            if (previousWasGroup) {
                out.append(previousShapeMask ? '\x01' : '\x00');
            }
            const LiveryExportShape packed = exportLiveryShape(*child.shape, worldAdjustment);
            const quint8 lead = (childMask || previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            appendLiveryShapeRecord(out, packed, origin.x(), origin.y(), lead,
                                    (!hasPreviousSibling || followsGroup) && !childMask);
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = childMask || packed.mask;
        }
        hasPreviousSibling = true;
    }
    return out;
}

void appendStructuralSection(QByteArray &body, const scene::Group *sectionGroup,
                              const SourceLivery *source, int slot, bool hasFollowingSlot) {
    if (sectionGroup == nullptr) {
        body.append(sourceEmptySlot(source, slot));
        return;
    }
    const QVector<LiveryEntry> children = liverySectionChildren(*sectionGroup);
    if (children.isEmpty()) {
        body.append(sourceEmptySlot(source, slot));
        return;
    }
    const bool hasGroupedArtwork = std::any_of(children.cbegin(), children.cend(), [](const LiveryEntry &entry) {
        return entry.kind == LiveryEntry::Group;
    });

    body.append(packMarkerlessGroupHeader(children, "livery section"));
    bool previousWasGroup = false;
    int previousGroupDepth = 0;
    bool previousShapeMask = false;
    bool hasPreviousSibling = false;
    const QPointF sectionOrigin(0.0, 0.0);
    const Matrix3 worldAdjustment = invertAffine(liverySectionCanvasTransform(slot));
    for (const LiveryEntry &child : children) {
        if (child.kind == LiveryEntry::Group) {
            QByteArray marker;
            if (previousWasGroup) {
                marker = siblingGroupTransformMarker(previousGroupDepth);
            } else if (hasPreviousSibling) {
                marker = QByteArray("\x00\x03", 2);
            } else {
                marker = QByteArray();
            }
            marker = maybeFlipMaskFlag(marker, previousShapeMask);
            previousShapeMask = false;
            body.append(packLiveryGroup(child, sectionOrigin, marker, false, previousShapeMask,
                                        worldAdjustment));
            previousWasGroup = true;
            previousGroupDepth = terminalDepth(child);
        } else if (child.shape != nullptr) {
            const bool followsGroup = previousWasGroup;
            if (previousWasGroup) {
                body.append(previousShapeMask ? '\x01' : '\x00');
            }
            const LiveryExportShape packed = exportLiveryShape(*child.shape, worldAdjustment);
            const quint8 lead = (previousWasGroup || previousShapeMask) ? 0x01 : 0x00;
            appendLiveryShapeRecord(body, packed, sectionOrigin.x(), sectionOrigin.y(), lead,
                                    !hasPreviousSibling || followsGroup);
            previousWasGroup = false;
            previousGroupDepth = 0;
            previousShapeMask = packed.mask;
        }
        hasPreviousSibling = true;
    }
    const bool endsInNestedGroup = !children.isEmpty()
        && children.back().kind == LiveryEntry::Group
        && terminalDepth(children.back()) > 1;
    if (endsInNestedGroup && !hasFollowingSlot) {
        body.append(previousShapeMask ? '\x01' : '\x00');
        return;
    }
    if (hasGroupedArtwork) {
        QByteArray remnant = groupedRemnant(source, slot, children);
        if (previousShapeMask && !remnant.isEmpty()) {
            remnant[0] = '\x01';
        }
        body.append(remnant);
    } else if (hasFollowingSlot) {
        QByteArray remnant = sourceRemnant(source, slot);
        if (previousShapeMask && !remnant.isEmpty()) {
            remnant[0] = '\x01';
        }
        body.append(remnant);
    } else {
        body.append(previousShapeMask ? '\x01' : '\x00');
    }
}

} // namespace

QByteArray buildLiveryGyvl(const Project &project, std::array<int, kLiverySectionCount> *outSectionCounts) {
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

    if constexpr (kEnforceLiveryShapeLimits) {
        for (int slot = 0; slot < kLiverySectionCount; ++slot) {
            const int shapeCount = static_cast<int>(slotShapes[slot].size());
            const int shapeLimit = liverySectionShapeLimit(slot);
            if (shapeCount <= shapeLimit) {
                continue;
            }
            const QString sectionName = slotGroups[slot] != nullptr && !slotGroups[slot]->name.isEmpty()
                ? slotGroups[slot]->name
                : QStringLiteral("slot %1").arg(slot);
            throw std::runtime_error(
                QStringLiteral("livery section \"%1\" has %2 shapes; export limit is %3")
                    .arg(sectionName)
                    .arg(shapeCount)
                    .arg(shapeLimit)
                    .toStdString());
        }
    }

    const std::optional<SourceLivery> source = sourceLivery(project);
    const SourceLivery *sourcePtr = source ? &*source : nullptr;
    bool requiresStructuralArtwork = false;
    for (int slot = 0; slot < kLiverySectionCount; ++slot) {
        if (!slotShapes[slot].isEmpty()
            && !sectionMatchesSource(slotShapes[slot], sourceSection(sourcePtr, slot),
                                     hasGroupedArtwork(slotGroups[slot]))) {
            requiresStructuralArtwork = true;
            break;
        }
    }
    // yrvl counts are coupled to the records emitted for each slot.
    std::array<int, kLiverySectionCount> counts{};
    int lastPopulatedSlot = -1;
    for (int slot = 0; slot < kLiverySectionCount; ++slot) {
        if (!slotShapes[slot].isEmpty()) {
            lastPopulatedSlot = slot;
        }
    }
    QByteArray body;
    for (int slot = 0; slot < kLiverySectionCount; ++slot) {
        const QVector<const scene::Shape *> &shapes = slotShapes[slot];
        const LiverySection *section = sourceSection(sourcePtr, slot);
        const bool hasRasterArtwork = std::any_of(shapes.cbegin(), shapes.cend(), [](const scene::Shape *shape) {
            return shape != nullptr && shape->raster;
        });
        const int sourceDecals = hasRasterArtwork && sourcePtr
                && slot < sourcePtr->payload.sectionCounts.size()
            ? sourcePtr->payload.sectionCounts[slot]
            : static_cast<int>(shapes.size());

        if (sourcePtr == nullptr && shapes.isEmpty()) {
            QByteArray emptySlot = defaultEmptySlot(slot);
            if (slot == lastPopulatedSlot + 1) {
                emptySlot.remove(0, kLiveryBodyTruncate);
            }
            body.append(emptySlot);
            counts[static_cast<size_t>(slot)] = 0;
            continue;
        }

        if (requiresStructuralArtwork && shapes.isEmpty() && slot + 1 < kLiverySectionCount) {
            body.append(sourceEmptySlot(sourcePtr, slot));
            counts[static_cast<size_t>(slot)] = 0;
            continue;
        }

        // Source-faithful spans preserve unchanged and partially decoded slots.
        if (sectionMatchesSource(shapes, section, hasGroupedArtwork(slotGroups[slot]))) {
            const QByteArray preserved = sourceSlotBytes(sourcePtr, slot);
            if (shapes.isEmpty() && preserved.size() < kLiveryEmptySlotBytes) {
                body.append(sourceEmptySlot(sourcePtr, slot));
                counts[static_cast<size_t>(slot)] = 0;
            } else {
                body.append(preserved);
                counts[static_cast<size_t>(slot)] = sourceDecals;
            }
            continue;
        }

        for (const scene::Shape *shape : shapes) {
            if (shape != nullptr && shape->raster) {
                throw std::runtime_error("changed custom logo decals cannot be synthesized yet");
            }
        }
        const bool hasFollowingSlot = slot + 1 < kLiverySectionCount;
        appendStructuralSection(body, slotGroups[slot], sourcePtr, slot, hasFollowingSlot);
        if (sourcePtr != nullptr && slot == kLiverySectionCount - 1 && !shapes.isEmpty()) {
            body.append(QByteArray(kLiveryBodyTruncate, '\0'));
        }
        counts[static_cast<size_t>(slot)] = static_cast<int>(shapes.size());
    }
    if (sourcePtr != nullptr) {
        body = body.left(std::max<qsizetype>(0, body.size() - kLiveryBodyTruncate));
    }

    if (outSectionCounts != nullptr) {
        *outSectionCounts = counts;
    }

    QByteArray chunk(reinterpret_cast<const char *>(kGyvlHeader), 0x15);
    chunk.append(body);
    return chunk;
}

} // namespace fh6
