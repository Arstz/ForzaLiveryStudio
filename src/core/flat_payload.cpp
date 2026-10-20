#include "flat_payload.h"
#include "vinyl_framing.h"
#include "vinyl_encoding.h"

#include "layer.h"

#include <stdexcept>

namespace fls {
namespace {

constexpr int kMaxDirectChildren = 0xffff;

void collectVisibleShapes(const scene::Layer &node, QVector<detail::VinylShapePlacement> &out) {
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
        out.push_back(detail::vinylShapePlacement(static_cast<const scene::Shape &>(node)));
    }
}

} // namespace

QByteArray buildFlatPayload(const Project &project) {
    QVector<detail::VinylShapePlacement> visibleLayers;
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
    const QByteArray prefix = detail::standaloneVinylPrefix(project.sourceDecPrefix);

    QByteArray payload = prefix;
    payload.append(detail::writeVinylChildHeader(count, QByteArray(childBlocks, '\0'), 0x20, true));
    bool prevWasMask = false;
    for (const detail::VinylShapePlacement &layer : visibleLayers) {
        detail::appendVinylShapeRecord(payload, layer, 0.0, 0.0, 0x02, prevWasMask ? 0x01 : 0x00);
        prevWasMask = layer.mask;
    }
    payload.append('\x00');
    payload.append('\x01');
    return payload;
}

} // namespace fls
