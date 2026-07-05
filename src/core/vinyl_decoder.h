#pragma once

#include "core_types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace fh6 {

// One of the 11 fixed C_livery panel sections, decoded from the embedded gyvl
// body (see LiveryResearch/SECTIONS.md). `subtree` holds the section's decoded
// group tree (empty for an unpopulated slot).
struct LiverySection {
    int slot = 0;            // 0..10, storage order
    QString name;            // "Front" ... "RightWindow"
    double rotationDeg = 0;  // scaffold panel orientation {0, 90, -90, 180}
    bool populated = false;
    VinylGroup subtree;
    int absPos = 0;          // offset of the slot within the gyvl body
};

class VinylTreeDecoder {
public:
    LayerData getLayerData(const QByteArray &payload) const;
    VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload = {}) const;
    // Walk the embedded gyvl body (starting at gyvl-rel 0x15) into its 11 fixed
    // section slots. `sectionCounts` holds the ground-truth per-section decal
    // counts (from the yrvl stats chunk) that drive each section's extent.
    QVector<LiverySection> buildLiverySections(const QByteArray &body,
                                               const QVector<int> &sectionCounts) const;
    QVector<QString> validateTree(const VinylGroup &root) const;
};

LayerData getLayerData(const QByteArray &payload);
VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload);
QVector<LiverySection> buildLiverySections(const QByteArray &body, const QVector<int> &sectionCounts);
QVector<QString> validateTree(const VinylGroup &root);

} // namespace fh6
