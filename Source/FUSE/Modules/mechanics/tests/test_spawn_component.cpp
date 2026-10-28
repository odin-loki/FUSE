#include <fuse/mechanics/spawn_component.hpp>
#include <fuse/mechanics/spawn_count.hpp>

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

void testSpawnCountOk() {
    expectTrue(fuse::mechanics::spawn_count_ok(1), "spawn_count_ok(1) is true");
    expectTrue(fuse::mechanics::spawn_count_ok(64), "spawn_count_ok(64) is true");
    expectTrue(!fuse::mechanics::spawn_count_ok(0), "spawn_count_ok(0) is false");
    expectTrue(!fuse::mechanics::spawn_count_ok(65), "spawn_count_ok(65) is false");
}

} // namespace

int main() {
    testSpawnCountOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }

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

#include <fuse/mechanics/spawn_burst.hpp>

namespace {

void testSpawnBurstCount() {
    float accumulator = 0.f;
    const int emitted = fuse::mechanics::spawn_burst_count(accumulator, 0.35f, 0.1f, 2);
    expectTrue(emitted == 2, "spawn burst from 0 with dt 0.35, interval 0.1, max 2 emits 2");
    expectTrue(accumulator == 0.15f, "spawn burst leaves 0.15 in the accumulator");
}

// Appended gate: runs before main so the existing failure count still reports it.
struct SpawnBurstCountHook {
    SpawnBurstCountHook() { testSpawnBurstCount(); }
};

[[maybe_unused]] const SpawnBurstCountHook g_spawnBurstCountHook{};

} // namespace
