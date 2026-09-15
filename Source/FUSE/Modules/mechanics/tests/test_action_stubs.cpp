#include <fuse/core/init.hpp>
#include <fuse/mechanics/action_stub.hpp>
#include <fuse/mechanics/interact_action.hpp>
#include <fuse/mechanics/interactable.hpp>
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

void testActionKindVerbRoundTrip() {
    expectTrue(fuse::mechanics::verbForActionKind(fuse::mechanics::InteractActionKind::Use) == std::string("use"),
               "Use maps to use verb");
    expectTrue(fuse::mechanics::verbForActionKind(fuse::mechanics::InteractActionKind::Pickup) ==
                   std::string("pickup"),
               "Pickup maps to pickup verb");
    expectTrue(fuse::mechanics::verbForActionKind(fuse::mechanics::InteractActionKind::Examine) ==
                   std::string("examine"),
               "Examine maps to examine verb");

    expectTrue(fuse::mechanics::actionKindForVerb("use") == fuse::mechanics::InteractActionKind::Use,
               "use verb maps to Use");
    expectTrue(fuse::mechanics::actionKindForVerb("pickup") == fuse::mechanics::InteractActionKind::Pickup,
               "pickup verb maps to Pickup");
    expectTrue(fuse::mechanics::actionKindForVerb("examine") == fuse::mechanics::InteractActionKind::Examine,
               "examine verb maps to Examine");
}

void testInteractActionContextConversion() {
    fuse::mechanics::InteractAction action;
    action.kind = fuse::mechanics::InteractActionKind::Pickup;
    action.item = "rusty_key";
    action.amount = 2;

    const auto ctx = action.toContext();
    expectTrue(ctx.verb == "pickup", "toContext sets verb");
    expectTrue(ctx.item == "rusty_key", "toContext sets item");
    expectTrue(ctx.amount == 2u, "toContext sets amount");

    const auto roundTrip = fuse::mechanics::InteractAction::fromContext(ctx);
    expectTrue(roundTrip.kind == fuse::mechanics::InteractActionKind::Pickup, "fromContext restores kind");
    expectTrue(roundTrip.item == "rusty_key", "fromContext restores item");
    expectTrue(roundTrip.amount == 2u, "fromContext restores amount");
}

void testUseActionStub() {
    fuse::mechanics::UseActionStub useStub("use_stub");

    fuse::mechanics::InteractionContext ctx;
    ctx.verb = "use";
    ctx.item = "lever";

    expectTrue(useStub.canExecute(ctx), "use stub accepts use verb");
    expectTrue(useStub.execute(ctx), "use stub executes");
    expectTrue(useStub.executionCount() == 1u, "use stub records execution");
    expectTrue(useStub.lastItem() == "lever", "use stub records item");

    ctx.verb = "pickup";
    expectTrue(!useStub.canExecute(ctx), "use stub rejects pickup verb");
}

void testPickupActionStub() {
    fuse::mechanics::PickupActionStub pickupStub;

    fuse::mechanics::InteractionContext ctx;
    ctx.verb = "pickup";
    ctx.item = "key_card";

    expectTrue(pickupStub.execute(ctx), "pickup stub executes");
    expectTrue(pickupStub.executionCount() == 1u, "pickup stub records execution");
}

void testExamineActionStub() {
    fuse::mechanics::ExamineActionStub examineStub("examine_stub");

    fuse::mechanics::InteractionContext ctx;
    ctx.verb = "examine";
    ctx.item = "terminal";

    expectTrue(examineStub.execute(ctx), "examine stub executes");
    expectTrue(examineStub.executionCount() == 1u, "examine stub records execution");
}

void testInteractableAcceptsExamineVerb() {
    fuse::mechanics::InteractableComponent terminal("terminal");
    terminal.setSupportedVerbs({"use", "pickup", "examine"});
    expectTrue(terminal.attach(), "terminal attaches");

    fuse::mechanics::InteractionContext ctx;
    ctx.verb = "examine";

    expectTrue(terminal.canInteract(ctx), "interactable accepts examine verb");
    expectTrue(terminal.interact(ctx), "interactable dispatches examine");
    expectTrue(terminal.interactionCount() == 1u, "examine interaction counted");
}

} // namespace

int main() {
    fuse::core::initialize();
    testActionKindVerbRoundTrip();
    testInteractActionContextConversion();
    testUseActionStub();
    testPickupActionStub();
    testExamineActionStub();
    testInteractableAcceptsExamineVerb();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics action stub tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics action stub tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
