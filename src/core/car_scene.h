#pragma once


#include "model_geometry.h"

#include <QString>

namespace fls {

// Stock wheel geometry for one axle, in the units the game states it in. The shared wheel and
// tyre models are normalised and the carbin's wheel placement is an authoring pose, so this is
// what gives the corner both its real size and its real position (see docs/GAMEDATA.md).
struct AxleSizing {
    float tireWidthMillimetres = 245.0f;
    float tireAspectPercent = 40.0f;
    float rimDiameterInches = 18.0f;
    float trackOuterMetres = 1.815f;   // outer tyre face to outer tyre face
    float rideHeightMetres = 0.1655f;  // ground to the model's Y origin
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

} // namespace fls
