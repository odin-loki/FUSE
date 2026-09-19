#include <fuse/mechanics/spawn_component.hpp>

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
    fuse::mechanics::SpawnComponent spawn("OutpostSpawn", "guard_spawn");
    expectTrue(spawn.spawnId() == "guard_spawn", "spawn id stored");
    expectTrue(spawn.triggerSpawn(), "active spawn point fires");
    expectTrue(spawn.spawnCount() == 1u, "spawn count tracked");

    spawn.setActive(false);
    expectTrue(!spawn.triggerSpawn(), "inactive spawn point ignored");
    expectTrue(spawn.spawnCount() == 1u, "inactive spawn does not increment count");

    if (g_failures == 0) {
        std::printf("fuse_mechanics_spawn_component: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_spawn_component: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
