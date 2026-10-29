#include <fuse/adventure/interactable.hpp>
#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/inventory.hpp>
#include <fuse/adventure/prompt_score.hpp>
#include <fuse/adventure/puzzle_gate.hpp>
#include <fuse/adventure/verb_name.hpp>

#include <cmath>
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

void testVerbNameOk() {
    expectTrue(fuse::adventure::verb_name_ok("Use"), "Use is a valid verb name");
    expectTrue(!fuse::adventure::verb_name_ok(""), "empty verb name is invalid");
    expectTrue(!fuse::adventure::verb_name_ok("1Use"), "leading digit is invalid");
    expectTrue(!fuse::adventure::verb_name_ok("Use Item"), "space in verb name is invalid");
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

void testPromptScore() {
    static constexpr float kTolerance = 1e-4f;
    const auto near = [](float actual, float expected) {
        return std::fabs(actual - expected) <= kTolerance;
    };

    expectTrue(near(fuse::adventure::prompt_score(0.f, 2.f, 10), 10.f),
               "prompt score at zero distance equals priority");
    expectTrue(near(fuse::adventure::prompt_score(2.f, 2.f, 10), 0.f),
               "prompt score at the range limit is zero");
    expectTrue(near(fuse::adventure::prompt_score(1.f, 2.f, 10), 5.f),
               "prompt score at mid range is half priority");
    expectTrue(near(fuse::adventure::prompt_score(-1.f, 2.f, 10), 0.f),
               "prompt score rejects a negative distance");
    expectTrue(near(fuse::adventure::prompt_score(3.f, 2.f, 10), 0.f),
               "prompt score rejects a distance past the range");
    expectTrue(near(fuse::adventure::prompt_score(1.f, 0.f, 10), 0.f),
               "prompt score rejects a zero range");
    expectTrue(near(fuse::adventure::prompt_score(1.f, -2.f, 10), 0.f),
               "prompt score rejects a negative range");
    expectTrue(near(fuse::adventure::prompt_score(std::numeric_limits<float>::quiet_NaN(), 2.f, 10), 0.f),
               "prompt score rejects a non-finite distance");
    expectTrue(near(fuse::adventure::prompt_score(1.f, std::numeric_limits<float>::infinity(), 10), 0.f),
               "prompt score rejects a non-finite range");
}

} // namespace

int main() {
    testVerbNameOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_adventure interact tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testPromptScore();
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

#include <fuse/adventure/interact_range.hpp>

namespace {

void testInteractPromptVisible() {
    expectTrue(fuse::adventure::interact_prompt_visible(1.5f, 2.f, true, true),
               "interact prompt visible inside use range");
    expectTrue(!fuse::adventure::interact_prompt_visible(2.1f, 2.f, true, true),
               "interact prompt hidden beyond use range");
    expectTrue(!fuse::adventure::interact_prompt_visible(1.5f, 2.f, true, false),
               "interact prompt hidden when disabled");
}

struct InteractPromptVisibleRun {
    InteractPromptVisibleRun() { testInteractPromptVisible(); }
};

const InteractPromptVisibleRun g_interactPromptVisibleRun{};

} // namespace
