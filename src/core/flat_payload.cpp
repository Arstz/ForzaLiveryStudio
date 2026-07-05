#include "flat_payload.h"

#include "binary_io.h"
#include "matrix_math.h"

#include <algorithm>
#include <stdexcept>

namespace fh6 {
namespace {

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

} // namespace

using detail::appendLeFloat;
using detail::appendLeU16;

QByteArray packShape(const ShapeLayer &layer, bool maskRecord)
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

QByteArray buildFlatPayload(const Project &project)
{
    QVector<const ShapeLayer *> visibleLayers;
    visibleLayers.reserve(project.layers.size());
    for (const ShapeLayer &layer : project.layers) {
        if (layer.visible) {
            visibleLayers.push_back(&layer);
        }
    }

    const int count = visibleLayers.size();
    if (count > FlatExportLayerLimit) {
        throw std::runtime_error("too many visible layers for flat export");
    }

    const int childBlocks = (count + 7) / 8;
    const int encodedChildBlocks = std::min(childBlocks, 0xff);
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
    payload.append(static_cast<char>(encodedChildBlocks));
    payload.append(QByteArray(encodedChildBlocks + 2, '\x00'));
    bool prevWasMask = false;
    for (const ShapeLayer *layer : visibleLayers) {
        // 01 02 is a trailing flag: the game masks the shape that precedes an
        // 01 02 record, so a record is flagged when the previous visible layer
        // is a mask.
        payload.append(packShape(*layer, prevWasMask));
        prevWasMask = layer->mask;
    }
    payload.append('\x00');
    payload.append('\x01');
    return payload;
}

} // namespace fh6
