#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace fh6 {

// The decompressed contents of a C_livery, with the embedded C_group ("gyvl")
// located. See LiveryResearch/CLIVERY.md: the container is a chunk stream
// (vlrc / yrvl / gyvl / ...); the gyvl is a version-0 C_group whose body — the
// fixed 11-slot section stream — starts at gyvl-rel 0x15.
struct LiveryPayload {
    QByteArray raw;       // full decompressed C_livery
    int gyvlOffset = 0;   // offset of the "gyvl" FourCC within raw
    QByteArray body;      // the section stream (gyvl body, from gyvl+0x15 to the next chunk)
    // Per-section decal counts in storage order (Front, Back, Top, Left, Right,
    // Spoiler, FrontWindshield, BackWindshield, TopWindow, LeftWindow,
    // RightWindow), read from the yrvl "stats" chunk that follows gyvl. These are
    // the ground-truth section sizes that drive the section walker.
    QVector<int> sectionCounts;
};

// Resolve a C_livery file (or a folder containing one), inflate it, and locate
// the embedded gyvl body. Throws std::runtime_error on failure.
LiveryPayload readLiveryPayload(const QString &folderOrFile);

} // namespace fh6
