#include "demo_check.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/adventure/conversation_script_vm.hpp>
#include <fuse/adventure/hud_prompt_interactable.hpp>
#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/outpost_spawn.hpp>
#include <fuse/adventure/puzzle_gate.hpp>
#include <fuse/adventure/weapon_grant_pipeline.hpp>
#include <fuse/adventure/weapon_runtime.hpp>
#include <fuse/core/init.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/mechanics/interactable.hpp>
#include <fuse/mechanics/registry.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_adventure_stub: fuse_adventure outpost JSON + mechanics bridge");

    std::string projectPath = "Samples/unification/demo_adventure_stub";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.modules.adventure, "project enables fuse_adventure");
    fuse::demo::check(project.manifest.modules.mechanics, "project enables fuse_mechanics");

    const fuse::demo::wiring::ProjectRuntimeContext runtime =
        fuse::demo::wiring::prepareProjectRuntime(project);
    fuse::demo::check(runtime.ok, "project VFS mounts");

    fuse::scene::Scene runtimeScene;
    const fuse::demo::wiring::World3DLoadResult missionLoad =
        fuse::demo::wiring::ensure3DWorldFromProject(project, runtimeScene);
    fuse::demo::check(missionLoad.ok, "Outpost.mis converts and loads");

    fuse::adventure::Inventory inventory;
    inventory.setMaxLimit(fuse::adventure::ItemId("rusty_key"), 1);
    inventory.setMaxLimit(fuse::adventure::ItemId("outpost_door"), 1);
    inventory.setMaxLimit(fuse::adventure::ItemId("plasma_rifle"), 1);
    inventory.setMaxLimit(fuse::adventure::ItemId("energy_cell"), 99);

    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inventory;
    ctx.actorName = "player";

    fuse::adventure::InteractionSystem interactions;
    fuse::adventure::InteractableStub keyPickup;

    const auto pickedUp = interactions.pickup(ctx, fuse::adventure::ItemId("rusty_key"), 1, keyPickup);
    inventory.incInventory(fuse::adventure::ItemId("rusty_key"), 1);

    fuse::adventure::PuzzleGate outpostDoor(fuse::adventure::ItemId("rusty_key"));
    const auto used = interactions.use(ctx, fuse::adventure::ItemId("rusty_key"), outpostDoor);

    fuse::adventure::OutpostStubContent outpostContent;
    fuse::adventure::OutpostSpawnBundle outpostSpawn;
    fuse::SceneObject3D guardObject("outpost_guard");
    fuse::SceneObject3D leverObject("lever_interactable");
    fuse::adventure::ConversationScriptVm conversationVm;
    fuse::adventure::HudPromptInteractable hudPrompt("Press E to open maintenance door");
    fuse::mechanics::MechanicsRegistry mechanicsRegistry;
    fuse::mechanics::InteractableComponent doorInteractable{"maintenance_door"};

    const bool loadedOutpost = fuse::adventure::loadEmbeddedOutpostStub(outpostContent);
    const bool spawnedOutpost =
        loadedOutpost && fuse::adventure::spawnOutpostInteractables(outpostContent, outpostSpawn);
    const bool placedOutpost =
        spawnedOutpost &&
        fuse::adventure::applyOutpostScenePlacements(outpostSpawn, guardObject, leverObject);
    fuse::adventure::registerOutpostConversationScriptHooks(conversationVm);

    doorInteractable.setSupportedVerbs({"use"});
    doorInteractable.attach();
    mechanicsRegistry.registerInteractable(&doorInteractable, &doorInteractable);

    fuse::adventure::InteractContext promptCtx;
    promptCtx.actorName = "player";
    const std::string promptText = interactions.showHudPrompt(promptCtx, hudPrompt);

    fuse::adventure::WeaponGrantPipeline grantPipeline;
    fuse::adventure::WeaponRuntime weaponRuntime;
    bool weaponGranted = false;
    auto weaponIt = outpostSpawn.weaponPickups.find("armory_rifle");
    if (weaponIt != outpostSpawn.weaponPickups.end() && weaponIt->second != nullptr) {
        fuse::adventure::WeaponGrantRequest grantRequest{};
        grantRequest.weapon = fuse::adventure::ItemId("plasma_rifle");
        grantRequest.ammo = fuse::adventure::ItemId("energy_cell");
        grantRequest.ammoAmount = 20;
        grantRequest.stats.damage = 25.f;
        grantRequest.stats.range = 80.f;
        weaponGranted = grantPipeline.grantOnPickup(ctx, *weaponIt->second, grantRequest, weaponRuntime);
    }

    fuse::demo::check(inventory.hasInventory(fuse::adventure::ItemId("rusty_key")) == false,
                      "key consumed after door use");
    fuse::demo::check(pickedUp == fuse::adventure::InteractResult::PickedUp, "pickup interaction succeeded");
    fuse::demo::check(used == fuse::adventure::InteractResult::Used, "use interaction succeeded");
    fuse::demo::check(outpostDoor.isUnlocked(), "outpost door unlocked");
    fuse::demo::check(loadedOutpost, "embedded outpost_stub.json loaded");
    fuse::demo::check(spawnedOutpost, "outpost interactables spawned from JSON");
    fuse::demo::check(placedOutpost, "outpost placements applied to scene objects");
    fuse::demo::check(guardObject.x() >= 1.5f, "guard placed from JSON transform");
    fuse::demo::check(promptText == "Press E to open maintenance door", "HUD prompt text returned");
    fuse::demo::check(hudPrompt.promptShownCount() > 0u, "HUD prompt shown");
    fuse::demo::check(weaponGranted, "armory rifle grant pipeline succeeded");
    fuse::demo::check(missionLoad.entityCount >= 3u, "Outpost mission entities converted");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_adventure_stub");
}
