#pragma once

#include "core_types.h"

#include <array>

#include <QByteArray>
#include <QString>
#include <QVector>

#ifndef ENFORCE_SHAPE_LIMITS
#define ENFORCE_SHAPE_LIMITS 1
#endif

namespace fh6 {

inline constexpr bool kEnforceLiveryShapeLimits = ENFORCE_SHAPE_LIMITS != 0;

inline constexpr int liverySectionShapeLimit(int slot) noexcept
{
    return slot == 2 || slot == 3 || slot == 4 ? 3000 : 1000;
}

struct LiveryPayload {
    QByteArray raw;
    int gyvlOffset = 0;
    int carId = 0;
    QByteArray body;
    QVector<int> sectionCounts;
    LiveryPaintState paint;
};

LiveryPayload readLiveryPayload(const QString &folderOrFile);
LiveryPayload parseInflatedLiveryPayload(const QByteArray &raw);

QByteArray buildLiveryGyvl(const Project &project,
                           std::array<int, 11> *outSectionCounts = nullptr);

} // namespace fh6
