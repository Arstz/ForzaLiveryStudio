#pragma once


#include "model_geometry.h"

#include <QString>

namespace fls {

CarModel loadCarBin(const QString &path, QString *error = nullptr);

void appendApproximateTires(
    CarModel &car, const CarModel &leftTemplate, const CarModel &rightTemplate);

} // namespace fls
