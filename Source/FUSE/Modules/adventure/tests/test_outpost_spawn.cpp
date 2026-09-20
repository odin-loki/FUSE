#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/outpost_spawn.hpp>
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

    fuse::adventure::OutpostStubContent content;
    expectTrue(fuse::adventure::loadEmbeddedOutpostStub(content), "embedded outpost loads");

    fuse::adventure::OutpostSpawnBundle bundle;
    expectTrue(fuse::adventure::spawnOutpostInteractables(content, bundle), "outpost interactables spawn");
    expectTrue(bundle.doors.count("maintenance_door") == 1, "maintenance door spawned");
    expectTrue(bundle.weaponPickups.count("armory_rifle") == 1, "armory rifle spawned");
    expectTrue(bundle.conversations.count("outpost_guard") == 1, "guard conversation spawned");
    expectTrue(bundle.conversations["outpost_guard"]->branches().size() == 3u,
               "conversation branches preserved (aggressive, polite, armed)");
    expectTrue(bundle.placements.count("outpost_guard") == 1u, "JSON placement copied to spawn bundle");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_adventure_outpost_spawn: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_adventure_outpost_spawn: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
