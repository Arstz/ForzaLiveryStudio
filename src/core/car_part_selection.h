#pragma once

#include "model_geometry.h"

#include <QHash>
#include <QVector>

namespace fls {

inline constexpr std::array<int, 6> kSelectableCarPartTypes = {
    car_part_types::CarBody,
    car_part_types::RearWing,
    car_part_types::FrontBumper,
    car_part_types::RearBumper,
    car_part_types::Hood,
    car_part_types::SideSkirts,
};

QVector<const CarPartOption *> carPartOptions(const CarModel &model, int partType);
QHash<int, int> stockCarPartSelections(const CarModel &model);
QVector<const CarPartOption *> individualCarPartOptions(const CarModel &model, int partType);
QHash<int, int> carPartSelectionsForOption(const CarModel &model,
    const QHash<int, int> &individualParts, int partType, int optionId);

} // namespace fls
