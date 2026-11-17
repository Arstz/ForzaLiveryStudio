#include "car_part_selection.h"

#include <algorithm>

namespace fls {
namespace {

bool preferredPartOption(const CarPartOption *first, const CarPartOption *second) {
    if (first->stock != second->stock) return first->stock;
    if (first->parentIsStock != second->parentIsStock) return first->parentIsStock;
    if (first->level != second->level) return first->level < second->level;

    return first->id < second->id;
}

int bodyFamilyId(const CarModel &model, const CarPartOption &body) {
    QVector<int> bodies;
    QVector<int> families;

    if (body.carBodyId >= 0) return body.carBodyId;
    for (const auto &option : model.partOptions) {
        if (option.partType == car_part_types::CarBody) {
            bodies.push_back(option.id);
            continue;
        }
        if (option.carBodyId < 0) continue;
        if (option.id == body.id && option.stock && option.parentIsStock == body.stock) {
            return option.carBodyId;
        }
        if (!families.contains(option.carBodyId)) families.push_back(option.carBodyId);
    }
    if (families.contains(body.id)) return body.id;
    std::sort(bodies.begin(), bodies.end());
    std::sort(families.begin(), families.end());
    const qsizetype index = bodies.indexOf(body.id);
    if (bodies.size() == families.size() && index >= 0) return families[index];

    return -1;
}

QHash<int, int> selectionsForBody(const CarModel &model, const CarPartOption *body) {
    QHash<int, int> selections;
    const int family = body != nullptr ? bodyFamilyId(model, *body) : -1;

    for (int partType : kSelectableCarPartTypes) {
        const auto options = carPartOptions(model, partType);
        if (options.isEmpty()) continue;
        if (partType == car_part_types::CarBody && body != nullptr) {
            selections.insert(partType, body->id);
            continue;
        }
        const auto selected = std::find_if(options.cbegin(), options.cend(),
            [family](const auto *option) {
                return family < 0 || option->carBodyId < 0 || option->carBodyId == family;
            });
        selections.insert(partType, selected != options.cend() ? (*selected)->id : -1);
    }

    return selections;
}

} // namespace

QVector<const CarPartOption *> carPartOptions(const CarModel &model, int partType) {
    QVector<const CarPartOption *> options;

    for (const CarPartOption &option : model.partOptions) {
        if (option.partType == partType) options.push_back(&option);
    }
    std::sort(options.begin(), options.end(), preferredPartOption);

    return options;
}

QHash<int, int> stockCarPartSelections(const CarModel &model) {
    const auto bodies = carPartOptions(model, car_part_types::CarBody);

    return selectionsForBody(model, bodies.isEmpty() ? nullptr : bodies.front());
}

QVector<const CarPartOption *> individualCarPartOptions(const CarModel &model, int partType) {
    const auto bodies = carPartOptions(model, car_part_types::CarBody);
    auto options = carPartOptions(model, partType);
    const int family = bodies.isEmpty() ? -1 : bodyFamilyId(model, *bodies.front());

    if (partType == car_part_types::CarBody) return options;
    options.erase(std::remove_if(options.begin(), options.end(), [family](const auto *option) {
        return family >= 0 && option->carBodyId >= 0 && option->carBodyId != family;
    }), options.end());

    return options;
}

QHash<int, int> carPartSelectionsForOption(const CarModel &model,
    const QHash<int, int> &individualParts, int partType, int optionId) {
    const auto options = individualCarPartOptions(model, partType);
    const auto selected = std::find_if(options.cbegin(), options.cend(),
        [optionId](const auto *option) { return option->id == optionId; });
    QHash<int, int> selections = individualParts;

    if (selected == options.cend()) return selections;
    if (partType == car_part_types::CarBody && !(*selected)->stock) {
        return selectionsForBody(model, *selected);
    }
    selections.insert(partType, optionId);

    return selections;
}

} // namespace fls
