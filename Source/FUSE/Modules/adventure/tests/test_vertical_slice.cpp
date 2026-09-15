#include <fuse/adventure/adventure_interactable_component.hpp>
#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/inventory_component.hpp>
#include <fuse/adventure/mechanics_bridge.hpp>
#include <fuse/adventure/pickup_interactable.hpp>
#include <fuse/adventure/puzzle_gate.hpp>
#include <fuse/core/init.hpp>
#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/registry.hpp>

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

void testPickupUseVerticalSlice() {
    fuse::mechanics::Component player("player");
    fuse::adventure::InventoryComponent playerInventory("bag");
    playerInventory.inventory().setMaxLimit(fuse::adventure::ItemId("rusty_key"), 1);
    playerInventory.inventory().setMaxLimit(fuse::adventure::ItemId("outpost_door"), 1);

    expectTrue(player.addComponent(&playerInventory), "player accepts inventory");
    expectTrue(player.attach(), "player hierarchy attaches");

    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::MechanicsBridge bridge(interactions);
    fuse::mechanics::MechanicsRegistry registry;

    fuse::adventure::PickupInteractable keyPickup(fuse::adventure::ItemId("rusty_key"));
    fuse::adventure::AdventureInteractableComponent keyObject("rusty_key", keyPickup, interactions);
    expectTrue(keyObject.attach(), "key object attaches");

    fuse::adventure::PuzzleGate outpostDoor(fuse::adventure::ItemId("rusty_key"));
    fuse::adventure::AdventureInteractableComponent doorObject("outpost_door", outpostDoor, interactions);
    expectTrue(doorObject.attach(), "door object attaches");

    fuse::mechanics::InteractionContext pickupCtx;
    pickupCtx.item = "rusty_key";
    pickupCtx.amount = 1;

    const auto pickedUp = bridge.pickup(registry, &keyObject, &player, pickupCtx);
    expectTrue(pickedUp == fuse::adventure::InteractResult::PickedUp, "pickup grants rusty key");
    expectTrue(playerInventory.inventory().hasInventory(fuse::adventure::ItemId("rusty_key")),
               "player inventory contains key");
    expectTrue(keyPickup.consumed(), "world key consumed after pickup");

    fuse::mechanics::InteractionContext useCtx;
    useCtx.item = "rusty_key";

    const auto used = bridge.use(registry, &doorObject, &player, useCtx);
    expectTrue(used == fuse::adventure::InteractResult::Used, "use unlocks outpost door");
    expectTrue(outpostDoor.isUnlocked(), "door reports unlocked");
    expectTrue(!playerInventory.inventory().hasInventory(fuse::adventure::ItemId("rusty_key")),
               "key consumed unlocking door");
}

void testRegistryResolvesComponentTree() {
    fuse::mechanics::Component root("door_root");
    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::InteractableStub stub;
    fuse::adventure::AdventureInteractableComponent lever("lever", stub, interactions);

    expectTrue(root.addComponent(&lever), "root accepts adventure interactable");
    expectTrue(root.attach(), "root hierarchy attaches");

    fuse::mechanics::MechanicsRegistry registry;
    fuse::mechanics::InteractionContext ctx;
    ctx.instigatorRoot = &root;
    ctx.verb = "use";
    ctx.item = "Flashlight";

    auto* resolved = fuse::mechanics::MechanicsRegistry::resolveInteractable(&lever);
    expectTrue(resolved != nullptr, "registry resolves interactable from component tree");
    expectTrue(!registry.canInteract(&lever, ctx), "use fails without inventory item");
}

} // namespace

int main() {
    fuse::core::initialize();
    testPickupUseVerticalSlice();
    testRegistryResolvesComponentTree();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure vertical slice tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure vertical slice tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
