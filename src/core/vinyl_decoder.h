#pragma once

#include "core_types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace fh6 {

struct LiverySlotDef {
    const char *name;
    double rotationDeg;
};

extern const LiverySlotDef kFH6LiverySlots[11];
inline constexpr int kFH6SectionCount = 11;

struct LiverySection {
    int slot = 0;
    QString name;
    double rotationDeg = 0;
    bool populated = false;
    VinylGroup subtree;
    int absPos = 0;
};

class VinylTreeDecoder {
public:
    LayerData getLayerData(const QByteArray &payload) const;
    VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload = {}) const;
    QVector<LiverySection> buildLiverySections(const QByteArray &body,
                                               const QVector<int> &sectionCounts) const;
    QVector<LiverySection> buildLiverySections(const QByteArray &body,
                                               const QVector<int> &sectionCounts,
                                               const LiverySlotDef *slotDefs,
                                               int slotCount) const;
    QVector<QString> validateTree(const VinylGroup &root) const;
};

LayerData getLayerData(const QByteArray &payload);
VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload);
QVector<LiverySection> buildLiverySections(const QByteArray &body, const QVector<int> &sectionCounts);
QVector<QString> validateTree(const VinylGroup &root);

} // namespace fh6
