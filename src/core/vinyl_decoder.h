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

Matrix3 liverySectionCanvasTransform(int slot);

struct LiverySection {
    int slot = 0;
    QString name;
    double rotationDeg = 0;
    bool populated = false;
    VinylGroup subtree;
    int absPos = 0;
};

struct VinylDecoderOptions {
    bool markerlessRootHeader = false;
    bool appendLiveryTailPadding = false;
    QByteArray (*normalizeRecords)(QByteArray) = nullptr;
    void (*finalizeGroup)(VinylGroup &, const QByteArray &, const LayerData &) = nullptr;
};

class VinylTreeDecoder {
public:
    explicit VinylTreeDecoder(VinylDecoderOptions options = {});

    LayerData getLayerData(const QByteArray &payload) const;
    VinylGroup decodeGroup(const QByteArray &payload, LayerData *decodedLayerData = nullptr) const;
    VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload = {}) const;
    QVector<LiverySection> buildLiverySections(const QByteArray &body,
                                               const QVector<int> &sectionCounts) const;
    QVector<LiverySection> buildLiverySections(const QByteArray &body,
                                               const QVector<int> &sectionCounts,
                                               const LiverySlotDef *slotDefs,
                                               int slotCount) const;
    QVector<QString> validateTree(const VinylGroup &root) const;

private:
    VinylDecoderOptions options_;
};

LayerData getLayerData(const QByteArray &payload);
VinylGroup decodeGroup(const QByteArray &payload, LayerData *decodedLayerData);
VinylGroup buildTree(const QByteArray &layerData, const QByteArray &fullPayload);
QVector<LiverySection> buildLiverySections(const QByteArray &body, const QVector<int> &sectionCounts);
QVector<QString> validateTree(const VinylGroup &root);

} // namespace fh6
