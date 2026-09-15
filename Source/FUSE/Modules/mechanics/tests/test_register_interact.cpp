#include <fuse/core/init.hpp>
#include <fuse/mechanics/component.hpp>
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

void testComponentRegistersCachedInterface() {
    fuse::mechanics::Component root("root");
    fuse::mechanics::InteractableComponent lever("lever");

    expectTrue(root.addComponent(&lever), "root accepts lever child");
    expectTrue(root.attach(), "component hierarchy attaches");

    auto* iface = root.getInterface<fuse::mechanics::InteractableInterface>(
        "mechanics", "interactable", &lever);
    expectTrue(iface != nullptr, "root resolves child interactable interface");
    expectTrue(iface->owner() == &lever, "interface owner is lever component");

    fuse::mechanics::InteractionContext ctx;
    ctx.verb = "use";
    expectTrue(iface->canInteract(ctx), "cached interface forwards canInteract");
    expectTrue(iface->interact(ctx), "cached interface forwards interact");
    expectTrue(lever.interactionCount() == 1u, "interaction counted on lever");
}

void testRegistryInteractStub() {
    fuse::mechanics::MechanicsRegistry registry;
    fuse::mechanics::InteractableComponent door("door");

    expectTrue(door.attach(), "interactable attaches standalone");

    registry.registerInteractable(&door, &door);
    expectTrue(registry.interactableCount() == 1u, "registry stores interactable");

    fuse::mechanics::InteractionContext ctx;
    ctx.verb = "use";

    expectTrue(registry.canInteract(&door, ctx), "registry canInteract stub");
    expectTrue(registry.interact(&door, ctx), "registry interact stub");
    expectTrue(door.interactionCount() == 1u, "registry dispatches to component");

    door.setEnabled(false);
    expectTrue(!registry.interact(&door, ctx), "disabled interactable rejects interact");

    registry.unregisterInteractable(&door);
    expectTrue(registry.interactableCount() == 0u, "registry unregisters interactable");
}

} // namespace

int main() {
    fuse::core::initialize();
    testComponentRegistersCachedInterface();
    testRegistryInteractStub();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
