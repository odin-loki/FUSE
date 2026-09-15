#include "demo_check.hpp"

#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/puzzle_gate.hpp>
#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_adventure_stub: fuse_adventure interaction + inventory (3DAAK-inspired)");

    std::string projectPath = "Samples/unification/demo_adventure_stub";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.adventure, "project enables fuse_adventure");

    fuse::adventure::Inventory inventory;
    inventory.setMaxLimit(fuse::adventure::ItemId("rusty_key"), 1);
    inventory.setMaxLimit(fuse::adventure::ItemId("outpost_door"), 1);

    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inventory;
    ctx.actorName = "player";

    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::InteractableStub keyPickup;

    const auto pickedUp = interactions.pickup(
        ctx, fuse::adventure::ItemId("rusty_key"), 1, keyPickup);
    inventory.incInventory(fuse::adventure::ItemId("rusty_key"), 1);

    fuse::adventure::PuzzleGate outpostDoor(fuse::adventure::ItemId("rusty_key"));
    const auto used = interactions.use(
        ctx, fuse::adventure::ItemId("rusty_key"), outpostDoor);

    fuse::demo::check(inventory.hasInventory(fuse::adventure::ItemId("rusty_key")) == false,
                      "key consumed after door use");
    fuse::demo::check(pickedUp == fuse::adventure::InteractResult::PickedUp, "pickup interaction succeeded");
    fuse::demo::check(used == fuse::adventure::InteractResult::Used, "use interaction succeeded");
    fuse::demo::check(outpostDoor.isUnlocked(), "outpost door unlocked");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_adventure_stub");
}
