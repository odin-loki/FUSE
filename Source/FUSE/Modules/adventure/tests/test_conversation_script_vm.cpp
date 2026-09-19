#include <fuse/adventure/conversation_script_vm.hpp>
#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/outpost_spawn.hpp>
#include <fuse/core/init.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

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

    fuse::adventure::OutpostStubContent content;
    expectTrue(fuse::adventure::loadEmbeddedOutpostStub(content), "embedded outpost loads");
    expectTrue(content.placements.count("outpost_guard") == 1u, "guard placement parsed");

    fuse::adventure::OutpostSpawnBundle bundle;
    expectTrue(fuse::adventure::spawnOutpostInteractables(content, bundle), "spawn bundle created");
    expectTrue(bundle.placements.count("outpost_guard") == 1u, "spawn bundle carries placements");

    fuse::SceneObject3D guard("outpost_guard");
    fuse::SceneObject3D lever("lever_3d");
    expectTrue(fuse::adventure::applyOutpostScenePlacements(bundle, guard, lever), "placements applied");
    expectTrue(guard.x() == 2.f, "guard x placement");
    expectTrue(guard.y() == 1.f, "guard y placement");

    fuse::adventure::ConversationScriptVm vm;
    fuse::adventure::registerOutpostConversationScriptHooks(vm);
    expectTrue(vm.hookCount() == 2u, "conversation script hooks registered");

    fuse::adventure::InteractContext ctx;
    ctx.actorName = "player";
    auto guardIt = bundle.conversations.find("outpost_guard");
    expectTrue(guardIt != bundle.conversations.end(), "guard conversation exists");
    expectTrue(vm.dispatchBranch("outpost_guard", "polite", ctx, *guardIt->second),
               "conversation script VM dispatches polite branch");
    expectTrue(vm.dispatchCount() == 1u, "conversation VM dispatch counted");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_conversation_script_vm: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_conversation_script_vm: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
