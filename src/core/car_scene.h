#pragma once


#include "model_geometry.h"

#include <QString>

namespace fh6 {

// Stock rim diameter (inches) per axle. The shared wheel/tire models are normalised (they mate
// at a canonical rim radius); the real size is a uniform scale derived from these. Default 18"
// matches the historical hardcoded scale, so cars without an explicit entry are unchanged.
struct WheelSizing {
    float frontDiameterInches = 18.0f;
    float rearDiameterInches = 18.0f;
};

CarModel loadCarBin(const QString &path, QString *error = nullptr,
                    const WheelSizing &wheels = {});

void appendApproximateTires(
    CarModel &car, const CarModel &leftTemplate, const CarModel &rightTemplate);

} // namespace fh6
