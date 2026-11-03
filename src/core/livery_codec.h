#pragma once

#include "core_types.h"
#include "vinyl_decoder.h"

#include <array>

#include <QByteArray>
#include <QString>
#include <QVector>

#ifndef ENFORCE_SHAPE_LIMITS
#define ENFORCE_SHAPE_LIMITS 1
#endif

namespace fls {

inline constexpr bool kEnforceLiveryShapeLimits = ENFORCE_SHAPE_LIMITS != 0;
inline constexpr int kLegacyLiveryCounterCount = 7;

inline constexpr int liverySectionShapeLimit(int slot) noexcept {
    return slot == 2 || slot == 3 || slot == 4 ? 3000 : 1000;
}

struct LiveryPayload {
    QByteArray raw;
    QByteArray body;
    QVector<int> sectionCounts;
    LiveryPaintState paint;
    int gyvlOffset = 0;
    int carId = 0;
    bool legacyRecords = false;
    bool motorsport = false;
};

LiveryPayload readLiveryPayload(const QString &folderOrFile);
LiveryPayload parseInflatedLiveryPayload(const QByteArray &raw);
QVector<LiverySection> decodeLiverySections(const LiveryPayload &payload);
bool isLiveryAssetFileName(const QString &fileName);
bool isLiveryAsset(const QByteArray &fileData);

QByteArray buildLiveryGyvl(const Project &project,
                           std::array<int, 11> *outSectionCounts = nullptr);
QByteArray buildLiveryGyvlForSectionCount(const Project &project, int sectionCount,
                                        std::array<int, 11> *outSectionCounts);

} // namespace fls
