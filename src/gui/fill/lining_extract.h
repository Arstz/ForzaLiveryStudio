#pragma once

#include "region_extract.h"

#include <functional>

namespace gui {

inline constexpr int kDefaultLiningMinimumPixels = 8;
inline constexpr int kDefaultLiningColorCount = 6;
inline constexpr double kDefaultLiningWidthFraction = 0.008;
inline constexpr double kDefaultLiningContrast = 12.0;

struct LiningExtractionOptions {
    int minimumPixels = kDefaultLiningMinimumPixels;
    int maximumColors = kDefaultLiningColorCount;
    double maximumWidth = 0.0;
    double minimumContrast = kDefaultLiningContrast;
};

struct LiningExtractionResult {
    RegionExtractionResult regions;
    QImage pixels;
    QJsonObject diagnostics;
    bool cancelled = false;
};

LiningExtractionResult extractLining(
    const QImage &source, const LiningExtractionOptions &options = {},
    const std::function<bool()> &cancelled = {},
    const std::function<void(const QString &, int, int)> &progress = {});

} // namespace gui
