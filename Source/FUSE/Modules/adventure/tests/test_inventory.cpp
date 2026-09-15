#include <fuse/adventure/inventory.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testIncInventory() {
    fuse::adventure::Inventory inv;
    inv.setMaxLimit(fuse::adventure::ItemId("HealthKit"), 3);

    const auto added = inv.incInventory(fuse::adventure::ItemId("HealthKit"), 2);
    expectTrue(added == 2, "incInventory adds requested amount");
    expectTrue(inv.getInventory(fuse::adventure::ItemId("HealthKit")) == 2, "inventory count updated");
    expectTrue(inv.hasInventory(fuse::adventure::ItemId("HealthKit")), "hasInventory true when count > 0");
}

void testDecInventory() {
    fuse::adventure::Inventory inv;
    inv.setMaxLimit(fuse::adventure::ItemId("Ammo"), 10);
    inv.incInventory(fuse::adventure::ItemId("Ammo"), 5);

    const auto removed = inv.decInventory(fuse::adventure::ItemId("Ammo"), 2);
    expectTrue(removed == 2, "decInventory removes requested amount");
    expectTrue(inv.getInventory(fuse::adventure::ItemId("Ammo")) == 3, "inventory count decremented");
}

void testInventoryCap() {
    fuse::adventure::Inventory inv;
    inv.setMaxLimit(fuse::adventure::ItemId("SpeedGun"), 1);

    const auto first = inv.incInventory(fuse::adventure::ItemId("SpeedGun"), 1);
    const auto second = inv.incInventory(fuse::adventure::ItemId("SpeedGun"), 1);

    expectTrue(first == 1, "first incInventory succeeds");
    expectTrue(second == 0, "incInventory respects max limit");
    expectTrue(inv.getInventory(fuse::adventure::ItemId("SpeedGun")) == 1, "inventory capped at max");
}

void testSetInventoryClamp() {
    fuse::adventure::Inventory inv;
    inv.setMaxLimit(fuse::adventure::ItemId("Clip"), 4);

    const auto value = inv.setInventory(fuse::adventure::ItemId("Clip"), 99);
    expectTrue(value == 4, "setInventory clamps to max");
    expectTrue(inv.getInventory(fuse::adventure::ItemId("Clip")) == 4, "stored value is clamped");
}

} // namespace

int main() {
    testIncInventory();
    testDecInventory();
    testInventoryCap();
    testSetInventoryClamp();

    if (g_failures == 0) {
        std::printf("fuse_adventure inventory tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure inventory tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
