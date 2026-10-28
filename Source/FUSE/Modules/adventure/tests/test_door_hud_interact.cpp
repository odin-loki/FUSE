#include <fuse/adventure/door_interactable.hpp>
#include <fuse/adventure/door_state.hpp>
#include <fuse/adventure/hud_prompt_interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/prompt_hold.hpp>
#include <fuse/core/init.hpp>

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testDoorIsOpen() {
    expectTrue(fuse::adventure::door_is_open(0.5f), "door_is_open(0.5) is true");
    expectTrue(!fuse::adventure::door_is_open(0.49f), "door_is_open(0.49) is false");
    expectTrue(fuse::adventure::door_is_open(1.f), "door_is_open(1) is true");
    expectTrue(!fuse::adventure::door_is_open(0.f), "door_is_open(0) is false");
    expectTrue(!fuse::adventure::door_is_open(std::numeric_limits<float>::quiet_NaN()),
               "door_is_open(NaN) is false");
    expectTrue(!fuse::adventure::door_is_open(std::numeric_limits<float>::infinity()),
               "door_is_open(+inf) is false");
    expectTrue(!fuse::adventure::door_is_open(-std::numeric_limits<float>::infinity()),
               "door_is_open(-inf) is false");
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

int testPromptHoldOk() {
    const int before = g_failures;
    expectTrue(fuse::adventure::prompt_hold_ok(0), "prompt_hold_ok(0) is true");
    expectTrue(fuse::adventure::prompt_hold_ok(2000), "prompt_hold_ok(2000) is true");
    expectTrue(!fuse::adventure::prompt_hold_ok(-1), "prompt_hold_ok(-1) is false");
    expectTrue(!fuse::adventure::prompt_hold_ok(2001), "prompt_hold_ok(2001) is false");
    return g_failures == before ? 0 : EXIT_FAILURE;
}

} // namespace

int main() {
    if (testPromptHoldOk() != 0) {
        return EXIT_FAILURE;
    }

    testDoorIsOpen();
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

#include <fuse/adventure/door_swing.hpp>

namespace {

void testDoorSwingAngle() {
    expectTrue(fuse::adventure::door_swing_angle(0.5f, 90.f, false) == 45.f,
               "open 0.5, max 90, not inward returns 45");
    expectTrue(fuse::adventure::door_swing_angle(0.5f, 90.f, true) == -45.f,
               "open 0.5, max 90, inward returns -45");
    expectTrue(fuse::adventure::door_swing_angle(0.f, 90.f, false) == 0.f, "open 0 returns 0");
}

struct DoorSwingAngleTestInit {
    DoorSwingAngleTestInit() { testDoorSwingAngle(); }
};

const DoorSwingAngleTestInit g_doorSwingAngleTestInit{};

} // namespace
