#pragma once


#include "model_geometry.h"

#include <QString>

namespace fh6 {

// Stock tyre spec for one axle, in the units the game states it in. The shared wheel and tyre
// models are normalised, so this is what gives them their real size (see docs/GAMEDATA.md).
struct AxleSizing {
    float tireWidthMillimetres = 245.0f;
    float tireAspectPercent = 40.0f;
    float rimDiameterInches = 18.0f;
};

struct WheelSizing {
    AxleSizing front;
    AxleSizing rear;
};

CarModel loadCarBin(const QString &path, QString *error = nullptr,
                    const WheelSizing &wheels = {});

void appendApproximateTires(
    CarModel &car, const CarModel &leftTemplate, const CarModel &rightTemplate,
    const WheelSizing &wheels = {});

} // namespace fh6
