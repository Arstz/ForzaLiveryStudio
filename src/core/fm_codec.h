#pragma once

#include "core_types.h"
#include "layer.h"
#include "vinyl_decoder.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace fh6 {

// FM2023 section slot definitions (7 panels -- no windows).
inline constexpr int kFM2023SectionCount = 7;
extern const LiverySlotDef kFM2023LiverySlots[7];

// Detection helper: is the file a multi-container FM2023 livery?
// Reads the first bytes of the "data" file. Returns true when
// fileSize > 8 + compressedSize + 8 (extra containers present).
bool isFM2023Livery(const QByteArray &fileData);

// Detection helper: does the file start with a raw gyvl payload?
// (no container wrapper -- C_group variant)
bool isRawGyvl(const QByteArray &fileData);

// Decompressed FM2023 livery payload, with gyvl body and section counts.
struct FM2023LiveryPayload {
    QByteArray raw;                // full decompressed Container 1 (C_livery)
    int gyvlOffset = 0;            // offset of "gyvl" within raw
    QByteArray gyvlBody;           // section stream (gyvl+0x15 -> EOF or next chunk)
    int carId = 0;                 // target car id from vlrc rel 0x10
    QVector<int> sectionCounts;    // 7 per-section decal counts (empty if not found)
};

// Read an FM2023 C_livery data file, inflate containers, locate gyvl body and
// section counts from Container 3.
FM2023LiveryPayload readFM2023LiveryPayload(const QString &folderOrFile);

// Import an FM2023 livery data file as an editable project.
// Detects livery vs raw group variants and routes accordingly.
Project importFM2023Asset(const QString &folderOrFile);

} // namespace fh6
