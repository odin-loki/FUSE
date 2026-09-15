#include <fuse/adventure/adventure_interactable_component.hpp>
#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/interaction_queue.hpp>
#include <fuse/adventure/inventory_component.hpp>
#include <fuse/adventure/mechanics_bridge.hpp>
#include <fuse/adventure/pickup_interactable.hpp>
#include <fuse/adventure/puzzle_gate.hpp>
#include <fuse/core/init.hpp>
#include <fuse/mechanics/component.hpp>
#include <fuse/mechanics/registry.hpp>
#include <fuse/types.hpp>

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

void testQueuePendingAndDrainCounts() {
    fuse::adventure::InteractionQueue queue;
    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::MechanicsBridge bridge(interactions);
    fuse::mechanics::MechanicsRegistry registry;

    fuse::mechanics::Component player("player");
    fuse::adventure::InventoryComponent inventory("bag");
    inventory.inventory().setMaxLimit(fuse::adventure::ItemId("note"), 1);
    expectTrue(player.addComponent(&inventory), "player accepts inventory");
    expectTrue(player.attach(), "player attaches");

    fuse::adventure::PickupInteractable notePickup(fuse::adventure::ItemId("note"));
    fuse::adventure::AdventureInteractableComponent noteObject("note", notePickup, interactions);
    expectTrue(noteObject.attach(), "note object attaches");

    fuse::adventure::QueuedInteraction pickup;
    pickup.target = &noteObject;
    pickup.instigator = &player;
    pickup.ctx.verb = "pickup";
    pickup.ctx.item = "note";
    pickup.ctx.amount = 1;

    queue.enqueue(pickup);
    expectTrue(queue.pendingCount() == 1u, "one interaction pending");

    const auto drained = queue.drain(registry, bridge);
    expectTrue(drained == 1u, "drain processes one entry");
    expectTrue(queue.pendingCount() == 0u, "queue empty after drain");
    expectTrue(queue.processedCount() == 1u, "pickup counted as processed");
    expectTrue(queue.failedCount() == 0u, "no failures");
    expectTrue(inventory.inventory().hasInventory(fuse::adventure::ItemId("note")), "pickup granted item");
}

void testQueueOrderedPickupThenUse() {
    fuse::adventure::InteractionQueue queue;
    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::MechanicsBridge bridge(interactions);
    fuse::mechanics::MechanicsRegistry registry;

    fuse::mechanics::Component player("player");
    fuse::adventure::InventoryComponent inventory("bag");
    inventory.inventory().setMaxLimit(fuse::adventure::ItemId("rusty_key"), 1);
    expectTrue(player.addComponent(&inventory), "player accepts inventory");
    expectTrue(player.attach(), "player attaches");

    fuse::adventure::PickupInteractable keyPickup(fuse::adventure::ItemId("rusty_key"));
    fuse::adventure::AdventureInteractableComponent keyObject("rusty_key", keyPickup, interactions);
    expectTrue(keyObject.attach(), "key object attaches");

    fuse::adventure::PuzzleGate outpostDoor(fuse::adventure::ItemId("rusty_key"));
    fuse::adventure::AdventureInteractableComponent doorObject("outpost_door", outpostDoor, interactions);
    expectTrue(doorObject.attach(), "door object attaches");

    fuse::adventure::QueuedInteraction pickup;
    pickup.target = &keyObject;
    pickup.instigator = &player;
    pickup.ctx.verb = "pickup";
    pickup.ctx.item = "rusty_key";
    pickup.ctx.amount = 1;
    queue.enqueue(pickup);

    fuse::adventure::QueuedInteraction use;
    use.target = &doorObject;
    use.instigator = &player;
    use.ctx.verb = "use";
    use.ctx.item = "rusty_key";
    queue.enqueue(use);

    expectTrue(queue.pendingCount() == 2u, "two interactions queued");

    const auto drained = queue.drain(registry, bridge);
    expectTrue(drained == 2u, "drain processes both entries");
    expectTrue(queue.processedCount() == 2u, "pickup and use both processed");
    expectTrue(outpostDoor.isUnlocked(), "door unlocked after queued use");
    expectTrue(!inventory.inventory().hasInventory(fuse::adventure::ItemId("rusty_key")),
               "key consumed after queued use");
}

void testQueueFailedInteractionCounted() {
    fuse::adventure::InteractionQueue queue;
    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::MechanicsBridge bridge(interactions);
    fuse::mechanics::MechanicsRegistry registry;

    fuse::mechanics::Component player("player");
    fuse::adventure::InventoryComponent inventory("bag");
    expectTrue(player.addComponent(&inventory), "player accepts inventory");
    expectTrue(player.attach(), "player attaches");

    fuse::adventure::PuzzleGate lockedDoor(fuse::adventure::ItemId("missing_key"));
    fuse::adventure::AdventureInteractableComponent doorObject("door", lockedDoor, interactions);
    expectTrue(doorObject.attach(), "door object attaches");

    fuse::adventure::QueuedInteraction use;
    use.target = &doorObject;
    use.instigator = &player;
    use.ctx.verb = "use";
    use.ctx.item = "missing_key";
    queue.enqueue(use);

    queue.drain(registry, bridge);
    expectTrue(queue.failedCount() == 1u, "failed use increments failed count");
    expectTrue(queue.processedCount() == 0u, "no successful interactions");
    expectTrue(!lockedDoor.isUnlocked(), "door remains locked");
}

void testQueueClearDropsPending() {
    fuse::adventure::InteractionQueue queue;
    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::InteractableStub stub;
    fuse::adventure::AdventureInteractableComponent lever("lever", stub, interactions);
    expectTrue(lever.attach(), "lever attaches");

    fuse::adventure::QueuedInteraction entry;
    entry.target = &lever;
    entry.ctx.verb = "use";
    queue.enqueue(entry);

    expectTrue(queue.pendingCount() == 1u, "entry queued before clear");
    queue.clear();
    expectTrue(queue.pendingCount() == 0u, "clear drops pending entries");
}

} // namespace

int main() {
    fuse::core::initialize();
    testQueuePendingAndDrainCounts();
    testQueueOrderedPickupThenUse();
    testQueueFailedInteractionCounted();
    testQueueClearDropsPending();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure interaction queue tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure interaction queue tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
