#pragma once

#include "core_types.h"

#include <array>

#include <QByteArray>
#include <QString>
#include <QVector>

namespace fh6 {

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
