#pragma once


#include "model_geometry.h"

#include <QString>

namespace fh6 {

CarModel loadCarBin(const QString &path, QString *error = nullptr);

void appendApproximateTires(
    CarModel &car, const CarModel &leftTemplate, const CarModel &rightTemplate);

} // namespace fh6
