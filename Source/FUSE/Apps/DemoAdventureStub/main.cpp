#include "demo_check.hpp"

#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_adventure_stub: fuse_adventure interaction + inventory stub (3DAAK-inspired)");

    std::string projectPath = "Samples/unification/demo_adventure_stub";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.adventure, "project enables fuse_adventure");

    fuse::adventure::Inventory inventory;
    fuse::adventure::InteractionSystem interactions;

    const fuse::Handle<fuse::Object> keyItem(10u, 1u);
    inventory.addItem("rusty_key", 1u);

    const fuse::Handle<fuse::Object> player(1u, 1u);

    fuse::adventure::InteractionRequest pickup;
    pickup.kind = fuse::adventure::InteractionKind::PickUp;
    pickup.actor = player;
    pickup.target = keyItem;
    const bool pickedUp = interactions.tryInteract(pickup);

    fuse::adventure::InteractionRequest useDoor;
    useDoor.kind = fuse::adventure::InteractionKind::Use;
    useDoor.actor = player;
    useDoor.target = fuse::Handle<fuse::Object>(11u, 1u);
    useDoor.payload = "outpost_door";
    const bool used = interactions.tryInteract(useDoor);

    fuse::demo::check(inventory.itemCount() == 1u, "inventory holds one item");
    fuse::demo::check(pickedUp, "pickup interaction succeeded");
    fuse::demo::check(used, "use interaction succeeded");
    fuse::demo::check(interactions.successCount() == 2u, "two interactions recorded");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_adventure_stub");
}
