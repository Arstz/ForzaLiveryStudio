#include "car_part_selection.h"

#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

fls::CarPartOption option(int type, int id, int body, bool stock, bool parentStock, int level = 0) {
    fls::CarPartOption part;
    part.partType = type;
    part.id = id;
    part.carBodyId = body;
    part.stock = stock;
    part.parentIsStock = parentStock;
    part.level = level;
    return part;
}

void testExclusiveModes() {
    using namespace fls::car_part_types;
    fls::CarModel model;
    model.partOptions = {
        option(CarBody, 1000, -1, true, true),
        option(CarBody, 1100, -1, false, true, 1),
        option(CarBody, 1200, -1, false, true, 1),
        option(FrontBumper, 2000, 1000, true, true),
        option(FrontBumper, 2001, 1000, false, true, 1),
        option(FrontBumper, 2100, 1001, true, false),
        option(FrontBumper, 2200, 1002, true, false),
        option(RearBumper, 3000, 1000, true, true),
        option(RearBumper, 3100, 1001, true, false),
        option(RearBumper, 3200, 1002, true, false),
        option(Hood, 4000, 1000, true, true, 1),
        option(Hood, 4100, 1001, true, false),
        option(Hood, 4200, 1002, true, false),
        option(RearWing, 5000, -1, true, true),
        option(RearWing, 5001, -1, false, true, 1),
    };
    const auto stock = fls::stockCarPartSelections(model);
    require(stock.value(CarBody) == 1000 && stock.value(FrontBumper) == 2000
        && stock.value(Hood) == 4000, "defaults selected kit-owned stock parts");
    auto individual = fls::carPartSelectionsForOption(model, stock, FrontBumper, 2001);
    individual = fls::carPartSelectionsForOption(model, individual, RearWing, 5001);
    const auto kit = fls::carPartSelectionsForOption(model, individual, CarBody, 1100);
    require(kit.value(CarBody) == 1100 && kit.value(FrontBumper) == 2100
        && kit.value(RearBumper) == 3100 && kit.value(Hood) == 4100
        && kit.value(RearWing) == 5000, "full kit did not override the complete individual setup");
    require(individual.value(FrontBumper) == 2001 && individual.value(RearWing) == 5001,
        "selecting a kit overwrote remembered individual parts");
    const auto otherKit = fls::carPartSelectionsForOption(model, individual, CarBody, 1200);
    require(otherKit.value(FrontBumper) == 2200 && otherKit.value(RearBumper) == 3200
        && otherKit.value(Hood) == 4200, "switching kits retained another kit's details");
    const auto restored = fls::carPartSelectionsForOption(model, individual, CarBody, 1000);
    require(restored == individual, "stock body did not restore individual configuration");
    const auto changed = fls::carPartSelectionsForOption(model, individual, RearWing, 5000);
    require(changed.value(CarBody) == 1000 && changed.value(FrontBumper) == 2001
        && changed.value(RearWing) == 5000, "individual selection did not exit kit mode into remembered setup");
    const auto offered = fls::individualCarPartOptions(model, FrontBumper);
    require(offered.size() == 2 && offered[0]->id == 2000 && offered[1]->id == 2001,
        "individual chooser offers kit-owned parts");
    require(fls::carPartSelectionsForOption(model, individual, FrontBumper, 2100) == individual,
        "kit-owned detail accepted as an individual part");
    require(fls::carPartSelectionsForOption(model, individual, FrontBumper, 9999) == individual,
        "unknown part altered the configuration");
    model.partOptions.erase(model.partOptions.begin() + 9);
    const auto incompleteKit = fls::carPartSelectionsForOption(model, individual, CarBody, 1200);
    require(incompleteKit.value(RearBumper) == -1, "missing kit detail fell back to a stock bumper");
    require(fls::stockCarPartSelections({}).isEmpty(), "empty model produced selections");
}

} // namespace

int main() {
    try {
        testExclusiveModes();
        std::puts("Car part selection mode tests passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
