#include <fuse/adventure/outpost_loader.hpp>
#include <fuse/adventure/outpost_spawn.hpp>
#include <fuse/adventure/spawn_ring.hpp>
#include <fuse/adventure/spawn_yaw.hpp>
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

void testWrapSpawnYaw() {
    constexpr float tolerance = 1e-3f;
    const float yaw10 = fuse::adventure::wrap_spawn_yaw(10.f);
    expectTrue(yaw10 >= 10.f - tolerance && yaw10 <= 10.f + tolerance, "wrap_spawn_yaw(10) == 10");
    const float yaw360 = fuse::adventure::wrap_spawn_yaw(360.f);
    expectTrue(yaw360 >= -tolerance && yaw360 <= tolerance, "wrap_spawn_yaw(360) == 0");
    const float yawNeg = fuse::adventure::wrap_spawn_yaw(-90.f);
    expectTrue(yawNeg >= 270.f - tolerance && yawNeg <= 270.f + tolerance,
               "wrap_spawn_yaw(-90) == 270");
    const float yaw370 = fuse::adventure::wrap_spawn_yaw(370.f);
    expectTrue(yaw370 >= 10.f - tolerance && yaw370 <= 10.f + tolerance, "wrap_spawn_yaw(370) == 10");
    const float nanYaw = fuse::adventure::wrap_spawn_yaw(std::numeric_limits<float>::quiet_NaN());
    expectTrue(nanYaw >= -tolerance && nanYaw <= tolerance, "wrap_spawn_yaw(NaN) == 0");
    const float infYaw = fuse::adventure::wrap_spawn_yaw(std::numeric_limits<float>::infinity());
    expectTrue(infYaw >= -tolerance && infYaw <= tolerance, "wrap_spawn_yaw(+inf) == 0");
    const float ninfYaw = fuse::adventure::wrap_spawn_yaw(-std::numeric_limits<float>::infinity());
    expectTrue(ninfYaw >= -tolerance && ninfYaw <= tolerance, "wrap_spawn_yaw(-inf) == 0");
}

void testSpawnRingOk() {
    expectTrue(fuse::adventure::spawn_ring_ok(1), "spawn_ring_ok(1) is true");
    expectTrue(fuse::adventure::spawn_ring_ok(16), "spawn_ring_ok(16) is true");
    expectTrue(!fuse::adventure::spawn_ring_ok(0), "spawn_ring_ok(0) is false");
    expectTrue(!fuse::adventure::spawn_ring_ok(17), "spawn_ring_ok(17) is false");
}

} // namespace

int main() {
    testSpawnRingOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_adventure_outpost_spawn: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testWrapSpawnYaw();

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

#include <fuse/adventure/spawn_slot.hpp>

namespace {

void test_pick_spawn_slot() {
    const bool partial[] = {true, false, true};
    expectTrue(fuse::adventure::pick_spawn_slot(partial, 3, 0) == 1,
               "test_pick_spawn_slot: preferred 0 returns 1");
    const bool full[] = {true, true, true};
    expectTrue(fuse::adventure::pick_spawn_slot(full, 3, 0) == -1,
               "test_pick_spawn_slot: all occupied returns -1");
}

// Appended after main. Static initialization runs this before main reads g_failures.
[[maybe_unused]] const int g_test_pick_spawn_slot = [] {
    test_pick_spawn_slot();
    return 0;
}();

} // namespace

