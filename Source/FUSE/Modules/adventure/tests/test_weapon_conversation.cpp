#include <fuse/adventure/conversation_interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/weapon_pickup_interactable.hpp>
#include <fuse/core/init.hpp>

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

} // namespace

int main() {
    fuse::core::initialize();

    fuse::adventure::Inventory inventory;
    inventory.setMaxLimit(fuse::adventure::ItemId("plasma_rifle"), 1);
    inventory.setMaxLimit(fuse::adventure::ItemId("energy_cell"), 99);
    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inventory;
    ctx.actorName = "player";

    fuse::adventure::WeaponPickupInteractable rifle(fuse::adventure::ItemId("plasma_rifle"),
                                                     fuse::adventure::ItemId("energy_cell"),
                                                     20);
    const fuse::adventure::InteractResult pickup =
        rifle.onPickup(ctx, fuse::adventure::ItemId("plasma_rifle"), 1);
    expectTrue(pickup == fuse::adventure::InteractResult::PickedUp, "weapon pickup succeeds");
    expectTrue(inventory.hasInventory(fuse::adventure::ItemId("plasma_rifle")), "weapon granted");
    expectTrue(inventory.hasInventory(fuse::adventure::ItemId("energy_cell")), "ammo granted");
    expectTrue(inventory.activeWeapon().name == "plasma_rifle", "weapon equipped on pickup");
    expectTrue(rifle.consumed(), "weapon pickup consumed");

    fuse::adventure::ConversationBranch polite;
    polite.id = "polite";
    polite.lines = {"Thank you, traveler. Proceed with caution."};
    fuse::adventure::ConversationInteractable guard(
        {"Halt. State your business.", "The reactor is unstable — keep moving."}, {polite});
    fuse::adventure::InteractionSystem system;
    const std::string line0 = system.converse(ctx, guard);
    expectTrue(line0 == "The reactor is unstable — keep moving.", "conversation advances line");
    expectTrue(guard.converseCount() == 1u, "converse count tracked");

    const std::string branchLine = system.converseBranch(ctx, guard, "polite");
    expectTrue(branchLine == "Thank you, traveler. Proceed with caution.", "conversation branch selected");
    expectTrue(guard.activeBranchId() == "polite", "active branch tracked");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_weapon_conversation: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_weapon_conversation: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
