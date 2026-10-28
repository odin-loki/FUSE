#include <fuse/adventure/carry_weight.hpp>
#include <fuse/adventure/gold_count.hpp>
#include <fuse/adventure/inv_slot.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/stack_merge.hpp>

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

void testGoldCountOk() {
    expectTrue(fuse::adventure::gold_count_ok(0), "gold 0 is valid");
    expectTrue(fuse::adventure::gold_count_ok(1000000), "gold 1000000 is valid");
    expectTrue(!fuse::adventure::gold_count_ok(-1), "gold -1 is invalid");
    expectTrue(!fuse::adventure::gold_count_ok(1000001), "gold 1000001 is invalid");
}

void testInvSlotOk() {
    expectTrue(fuse::adventure::inv_slot_ok(0), "slot 0 is valid");
    expectTrue(fuse::adventure::inv_slot_ok(63), "slot 63 is valid");
    expectTrue(!fuse::adventure::inv_slot_ok(-1), "slot -1 is invalid");
    expectTrue(!fuse::adventure::inv_slot_ok(64), "slot 64 is invalid");
}

void testStacksCanMerge() {
    expectTrue(fuse::adventure::stacks_can_merge(3, 3, 2, 2, 5), "same item 2+2 fits in 5");
    expectTrue(!fuse::adventure::stacks_can_merge(3, 4, 1, 1, 5), "different items do not merge");
    expectTrue(!fuse::adventure::stacks_can_merge(3, 3, 3, 3, 5), "same item 3+3 exceeds 5");
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

void testCarryWeight() {
    expectTrue(fuse::adventure::can_carry(3.f, 2.f, 5.f), "current 3 added 2 limit 5 can carry");
    expectTrue(!fuse::adventure::can_carry(3.f, 3.f, 5.f), "current 3 added 3 limit 5 cannot carry");
    expectTrue(fuse::adventure::carry_left(3.f, 5.f) == 2.f, "carry_left(3, 5) == 2");
}

} // namespace

int main() {
    testGoldCountOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_adventure inventory tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testInvSlotOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_adventure inventory tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testStacksCanMerge();
    testIncInventory();
    testDecInventory();
    testInventoryCap();
    testSetInventoryClamp();
    testCarryWeight();

    if (g_failures == 0) {
        std::printf("fuse_adventure inventory tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure inventory tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
