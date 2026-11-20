#pragma once

#include "vinyl_decoder.h"

namespace fls::detail {

inline constexpr char kXbox360LiveryMagic[] = "crlv";
inline constexpr char kXbox360GroupMagic[] = "lvyg";

struct Xbox360Livery {
    QVector<LiverySection> sections;
    QVector<int> sectionCounts;
    LiveryPaintState paint;
    int carId = 0;
    bool locked = false;
};

Xbox360Livery readXbox360Livery(const QByteArray &bytes);
VinylGroup readXbox360Group(const QByteArray &bytes);
QByteArray encodeXbox360Vinyl(const Project &project, const QByteArray &creator = {});

} // namespace fls::detail
