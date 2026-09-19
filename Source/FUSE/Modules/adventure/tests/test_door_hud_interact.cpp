#include <fuse/adventure/door_interactable.hpp>
#include <fuse/adventure/hud_prompt_interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
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

void testDoorInteractable() {
    const fuse::adventure::ItemId key("rusty_key");

    fuse::adventure::Inventory inventory;
    inventory.setMaxLimit(key, 1);
    inventory.incInventory(key, 1);

    fuse::adventure::InteractContext ctx;
    ctx.inventory = &inventory;
    ctx.actorName = "player";

    fuse::adventure::DoorInteractable door(key);
    fuse::adventure::InteractionSystem system;

    expectTrue(!door.isOpen(), "door starts closed");
    expectTrue(system.use(ctx, key, door) == fuse::adventure::InteractResult::Used,
               "door opens with key");
    expectTrue(door.isOpen(), "door is open after key use");
    expectTrue(!inventory.hasInventory(key), "door consumes key");
}

void testHudPromptInteractable() {
    fuse::adventure::HudPromptInteractable lever("Press E to activate");
    fuse::adventure::InteractionSystem system;

    fuse::adventure::InteractContext ctx;
    ctx.actorName = "player";

    expectTrue(system.promptFor(lever) == "Press E to activate", "promptFor returns HUD text");
    expectTrue(system.examine(ctx, lever) == fuse::adventure::InteractResult::Examined,
               "hud prompt examine succeeds");
    expectTrue(lever.promptShownCount() == 1u, "hud prompt increments show count");
}

void testShowHudPromptHelper() {
    fuse::adventure::HudPromptInteractable lever("Press E to activate");
    fuse::adventure::InteractionSystem system;

    fuse::adventure::InteractContext ctx;
    ctx.actorName = "player";

    const std::string prompt = system.showHudPrompt(ctx, lever);
    expectTrue(prompt == "Press E to activate", "showHudPrompt returns HUD text");
    expectTrue(lever.promptShownCount() == 1u, "showHudPrompt increments show count");
}

} // namespace

int main() {
    fuse::core::initialize();

    testDoorInteractable();
    testHudPromptInteractable();
    testShowHudPromptHelper();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure door/hud tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure door/hud tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
