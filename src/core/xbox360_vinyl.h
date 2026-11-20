#pragma once

#include "vinyl_decoder.h"

namespace fls {
enum class ExportGameVersion;
}

namespace fls::detail {

inline constexpr char kXbox360LiveryMagic[] = "crlv";
inline constexpr char kXbox360GroupMagic[] = "lvyg";

struct Xbox360Livery {
    QVector<LiverySection> sections;
    QVector<int> sectionCounts;
    LiveryPaintState paint;
    QString creatorName;
    int carId = 0;
    bool locked = false;
};

bool isMotorsport2Livery(const QByteArray &bytes);
bool isMotorsport2LiveryContainer(const QByteArray &bytes);
bool isMotorsport2Group(const QByteArray &bytes);
bool isXbox360LiveryPayload(const QByteArray &bytes);
QString motorsport2CreatorName(const QByteArray &bytes);
QByteArray inflateMotorsport2Livery(const QByteArray &bytes);
QByteArray deflateMotorsport2Livery(const QByteArray &payload);
Xbox360Livery readXbox360Livery(const QByteArray &bytes);
VinylGroup readXbox360Group(const QByteArray &bytes);
QByteArray encodeXbox360Vinyl(const Project &project, ExportGameVersion game, const QByteArray &creatorTag);

} // namespace fls::detail
