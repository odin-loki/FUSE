#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/puzzle_gate.hpp>

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

void testInteractStubUse() {
    fuse::adventure::Inventory inv;
    inv.setMaxLimit(fuse::adventure::ItemId("Flashlight"), 1);
    inv.incInventory(fuse::adventure::ItemId("Flashlight"), 1);

    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inv;
    ctx.actorName = "player";

    fuse::adventure::InteractableStub target;
    fuse::adventure::InteractionSystem system;

    const auto result = system.use(ctx, fuse::adventure::ItemId("Flashlight"), target);
    expectTrue(result == fuse::adventure::InteractResult::Used, "use dispatches to interactable");
    expectTrue(target.useCount() == 1, "interactable stub records use");
    expectTrue(target.lastItem().name == "Flashlight", "interactable stub records item id");
}

void testInteractStubPickup() {
    fuse::adventure::Inventory inv;
    inv.setMaxLimit(fuse::adventure::ItemId("KeyCard"), 1);

    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inv;

    fuse::adventure::InteractableStub target;
    fuse::adventure::InteractionSystem system;

    const auto result = system.pickup(ctx, fuse::adventure::ItemId("KeyCard"), 1, target);
    expectTrue(result == fuse::adventure::InteractResult::PickedUp, "pickup dispatches to interactable");
    expectTrue(target.pickupCount() == 1, "interactable stub records pickup");
}

void testUseFailsWithoutInventory() {
    fuse::adventure::Inventory inv;
    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inv;

    fuse::adventure::InteractableStub target;
    fuse::adventure::InteractionSystem system;

    const auto result = system.use(ctx, fuse::adventure::ItemId("Missing"), target);
    expectTrue(result == fuse::adventure::InteractResult::Failed, "use fails when item not in inventory");
    expectTrue(target.useCount() == 0, "interactable not invoked on failure");
}

void testPuzzleGateUnlock() {
    fuse::adventure::Inventory inv;
    inv.setMaxLimit(fuse::adventure::ItemId("OutpostKey"), 1);
    inv.incInventory(fuse::adventure::ItemId("OutpostKey"), 1);

    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inv;

    fuse::adventure::PuzzleGate gate(fuse::adventure::ItemId("OutpostKey"));
    expectTrue(!gate.isUnlocked(), "puzzle gate starts locked");

    const auto result = gate.onUse(ctx, fuse::adventure::ItemId("OutpostKey"));
    expectTrue(result == fuse::adventure::InteractResult::Used, "puzzle gate unlocks with required item");
    expectTrue(gate.isUnlocked(), "puzzle gate reports unlocked");
    expectTrue(!inv.hasInventory(fuse::adventure::ItemId("OutpostKey")), "required item consumed on unlock");
}

} // namespace

int main() {
    testInteractStubUse();
    testInteractStubPickup();
    testUseFailsWithoutInventory();
    testPuzzleGateUnlock();

    if (g_failures == 0) {
        std::printf("fuse_adventure interact tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure interact tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
