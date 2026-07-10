#pragma once

#include "core_types.h"

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
    int carId = 0;        // target car id, read from the vlrc root header (rel 0x10)
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

// Encode the embedded gyvl chunk (the livery artwork: 0x15-byte header + section
// body) from a livery Project's 11 section folders. Unchanged imported sections
// preserve their captured source byte spans, including production nested grammar
// and car-specific scaffold bytes. Changed built-in-shape sections fall back to a
// synthesized flat section stream; changed custom-logo sections are still
// unsupported pending descriptor-table research. See docs/LIVERY_ENCODER.md.
QByteArray buildLiveryGyvl(const Project &project);

} // namespace fh6
