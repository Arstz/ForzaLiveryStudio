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

void testBodyKitCompatibility() {
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
        option(Hood, 4001, 1000, false, true, 1),
        option(SideSkirts, 6000, 1000, true, true),
        option(SideSkirts, 6001, 1000, false, true, 1),
        option(SideSkirts, 6100, 1001, true, false),
        option(SideSkirts, 6200, 1002, true, false),
        option(RearBumper, 3001, 1000, false, true, 1),
    };
    const auto stock = fls::stockCarPartSelections(model);
    require(stock.value(CarBody) == 1000 && stock.value(FrontBumper) == 2000
        && stock.value(Hood) == 4000, "defaults selected kit-owned stock parts");
    auto individual = stock;
    individual[FrontBumper] = 2001;
    individual[RearBumper] = 3001;
    individual[SideSkirts] = 6001;
    individual[RearWing] = 5001;
    const auto kit = fls::carPartSelectionsForOption(model, individual, individual, CarBody, 1100);
    require(kit.value(CarBody) == 1100 && kit.value(FrontBumper) == 2100
        && kit.value(RearBumper) == 3100 && kit.value(SideSkirts) == 6100
        && kit.value(Hood) == 4000 && kit.value(RearWing) == 5001, "incorrect kit installation");
    auto expected = kit;
    expected[Hood] = 4001;
    const auto hood = fls::carPartSelectionsForOption(model, kit, individual, Hood, 4001);
    require(hood == expected, "changing hood disturbed the kit");
    individual[Hood] = 4001;
    expected[RearWing] = 5000;
    const auto wing = fls::carPartSelectionsForOption(model, hood, individual, RearWing, 5000);
    require(wing == expected, "changing spoiler disturbed the kit");
    individual[RearWing] = 5000;
    const auto otherKit = fls::carPartSelectionsForOption(model, wing, individual, CarBody, 1200);
    require(otherKit.value(FrontBumper) == 2200 && otherKit.value(RearBumper) == 3200
        && otherKit.value(SideSkirts) == 6200 && otherKit.value(Hood) == 4001
        && otherKit.value(RearWing) == 5000, "switching kits lost compatible parts or kept old kit details");
    const auto restored = fls::carPartSelectionsForOption(model, otherKit, individual, CarBody, 1000);
    require(restored == individual, "stock body did not restore individual configuration");
    for (int partType : {FrontBumper, RearBumper, SideSkirts}) {
        const auto changed = fls::carPartSelectionsForOption(model, otherKit, individual, partType, stock.value(partType));
        auto withoutKit = individual;
        withoutKit[partType] = stock.value(partType);
        require(changed == withoutKit, "incompatible part did not restore the individual setup");
    }
    const auto offered = fls::individualCarPartOptions(model, FrontBumper);
    require(offered.size() == 2 && offered[0]->id == 2000 && offered[1]->id == 2001,
        "individual chooser offers kit-owned parts");
    require(fls::carPartSelectionsForOption(model, kit, individual, FrontBumper, 2100) == kit,
        "kit-owned detail accepted as an individual part");
    require(fls::carPartSelectionsForOption(model, kit, individual, FrontBumper, 9999) == kit,
        "unknown part altered the configuration");
    model.partOptions.erase(model.partOptions.begin() + 9);
    const auto incompleteKit = fls::carPartSelectionsForOption(model, kit, individual, CarBody, 1200);
    require(incompleteKit.value(RearBumper) == -1, "missing kit detail fell back to a stock bumper");
    require(fls::stockCarPartSelections({}).isEmpty(), "empty model produced selections");
}

} // namespace

int main() {
    try {
        testBodyKitCompatibility();
        std::puts("Car part selection mode tests passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
