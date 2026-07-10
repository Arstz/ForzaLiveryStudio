#include "flat_payload.h"

#include "binary_io.h"
#include "layer.h"
#include "matrix_math.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace fh6 {
namespace {

constexpr int kMaxDirectChildren = 0xff * 8;

QByteArray defaultPrefix()
{
    static constexpr char bytes[] = {
        'g', 'y', 'v', 'l',
        '\x01', '\x00', '\x00', '\x00',
        '\x00', '\x00', '\x00', '\x00',
        '\x03',
        '\x00', '\x00', '\x00', '\x00',
        '\x00', '\x00', '\x00', '\x00',
        '\x00', '\x00', '\x80', '\x3f',
        '\x00', '\x00', '\x00', '\x00',
    };
    return QByteArray(bytes, 29);
}

struct ExportShape {
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

ExportShape exportShapeFromScene(const scene::Shape &shape)
{
    const scene::Transform2D world = decomposeTransform2D(shape.worldMatrix());
    ExportShape out;
    out.shapeId = shape.shapeId;
    out.rotation = world.rotation;
    out.x = world.x;
    out.y = world.y;
    out.scaleX = world.scaleX;
    out.scaleY = world.scaleY;
    out.skew = world.skew;
    out.color = shape.color;
    out.color[3] = static_cast<quint8>(std::clamp<int>(std::lround(shape.opacity * 255.0), 0, 255));
    out.mask = shape.mask;
    return out;
}

void collectVisibleShapes(const scene::Layer &node, QVector<ExportShape> &out)
{
    if (node.kind() == scene::LayerKind::Group) {
        const auto &group = static_cast<const scene::Group &>(node);
        if (!group.visible) {
            return;
        }
        for (const auto &child : group.children) {
            collectVisibleShapes(*child, out);
        }
        return;
    }
    if (node.kind() == scene::LayerKind::Shape && node.visible) {
        out.push_back(exportShapeFromScene(static_cast<const scene::Shape &>(node)));
    }
}

} // namespace

using detail::appendLeFloat;
using detail::appendLeU16;

namespace {

QByteArray packShape(const ExportShape &layer, bool maskRecord)
{
    QByteArray out;
    out.reserve(32);
    // A flat record leads with 01 02 to flag a mask, 00 02 otherwise. The
    // marker is a *trailing* flag: the game masks the shape that PRECEDES an
    // 01 02 record, so the leading byte for this record is decided by the
    // caller (buildFlatPayload) from the previous layer's mask state.
    out.append(maskRecord ? '\x01' : '\x00');
    out.append('\x02');
    appendLeU16(out, layer.shapeId);
    appendLeFloat(out, static_cast<float>(normalizeRotation(layer.rotation)));
    appendLeFloat(out, static_cast<float>(layer.x));
    appendLeFloat(out, static_cast<float>(layer.y));
    appendLeFloat(out, static_cast<float>(layer.scaleX));
    appendLeFloat(out, static_cast<float>(layer.scaleY));
    appendLeFloat(out, static_cast<float>(layer.skew));
    out.append(static_cast<char>(layer.color[0]));
    out.append(static_cast<char>(layer.color[1]));
    out.append(static_cast<char>(layer.color[2]));
    out.append(static_cast<char>(layer.color[3]));
    return out;
}

} // namespace

QByteArray buildFlatPayload(const Project &project)
{
    QVector<ExportShape> visibleLayers;
    if (project.root) {
        visibleLayers.reserve(static_cast<int>(project.root->children.size()));
        for (const auto &child : project.root->children) {
            collectVisibleShapes(*child, visibleLayers);
        }
    }

    const int count = visibleLayers.size();
    const int childBlocks = (count + 7) / 8;
    if (count <= 0) {
        throw std::runtime_error("project has no visible layers to export");
    }
    if (count > kMaxDirectChildren) {
        throw std::runtime_error("flat export has too many visible layers for the child bitmap");
    }
    QByteArray prefix = project.sourceDecPrefix.isEmpty() ? defaultPrefix() : project.sourceDecPrefix;
    prefix = prefix.left(0x1d);
    if (prefix.size() < 0x1d) {
        throw std::runtime_error("source decompressed prefix is shorter than 0x1d bytes");
    }

    QByteArray rootTransform;
    appendLeFloat(rootTransform, 0.0f);
    appendLeFloat(rootTransform, 0.0f);
    appendLeFloat(rootTransform, 1.0f);
    appendLeFloat(rootTransform, 0.0f);
    prefix.replace(0x0d, 16, rootTransform);

    QByteArray payload = prefix;
    payload.append('\x20');
    appendLeU16(payload, static_cast<quint16>(count));
    payload.append(static_cast<char>(childBlocks));
    payload.append(QByteArray(childBlocks + 2, '\x00'));
    bool prevWasMask = false;
    for (const ExportShape &layer : visibleLayers) {
        // 01 02 is a trailing flag: the game masks the shape that precedes an
        // 01 02 record, so a record is flagged when the previous visible layer
        // is a mask.
        payload.append(packShape(layer, prevWasMask));
        prevWasMask = layer.mask;
    }
    payload.append('\x00');
    payload.append('\x01');
    return payload;
}

} // namespace fh6
