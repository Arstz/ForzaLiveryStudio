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
};

LiveryPayload readLiveryPayload(const QString &folderOrFile);

QByteArray buildLiveryGyvl(const Project &project,
                           std::array<int, 11> *outSectionCounts = nullptr);

} // namespace fh6
