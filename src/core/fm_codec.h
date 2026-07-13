#pragma once

#include "core_types.h"
#include "layer.h"
#include "vinyl_decoder.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace fh6 {

inline constexpr int kFM2023SectionCount = 7;
extern const LiverySlotDef kFM2023LiverySlots[7];

bool isFM2023Livery(const QByteArray &fileData);

bool isRawGyvl(const QByteArray &fileData);

struct FM2023LiveryPayload {
    QByteArray raw;
    int gyvlOffset = 0;
    QByteArray gyvlBody;
    int carId = 0;
    QVector<int> sectionCounts;
};

FM2023LiveryPayload readFM2023LiveryPayload(const QString &folderOrFile);

Project importFM2023Asset(const QString &folderOrFile);

} // namespace fh6
