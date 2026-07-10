#pragma once

// Loads a `.carbin` car scene into a single merged CarModel. Ported from
// ForzaTechStudio's CarbinParser (Scene / PartEntry / CarRenderModel). It parses the
// part list, resolves each referenced `.modelbin` relative to the carbin folder,
// loads the car skeleton for bone placement, and bakes
// (modelTransform * carBoneWorld) into every part's meshes so the whole car is
// expressed in one space. Only stock (non-upgrade) parts are assembled.

#include "model_geometry.h"

#include <QString>

namespace fh6 {

CarModel loadCarBin(const QString &path, QString *error = nullptr);

} // namespace fh6
