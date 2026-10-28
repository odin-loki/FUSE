#include <fuse/mechanics/broadphase_trigger_sync.hpp>
#include <fuse/mechanics/pair_key.hpp>
#include <fuse/mechanics/trigger_sync.hpp>
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

void testTriggerSyncOk() {
    expectTrue(fuse::mechanics::trigger_sync_ok(0), "trigger_sync_ok(0) is true");
    expectTrue(fuse::mechanics::trigger_sync_ok(15), "trigger_sync_ok(15) is true");
    expectTrue(!fuse::mechanics::trigger_sync_ok(-1), "trigger_sync_ok(-1) is false");
    expectTrue(!fuse::mechanics::trigger_sync_ok(16), "trigger_sync_ok(16) is false");
}

void testTriggerPairKey() {
    expectTrue(fuse::mechanics::trigger_pair_key(2u, 1u) ==
                   fuse::mechanics::trigger_pair_key(1u, 2u),
               "trigger_pair_key is order-independent");
    expectTrue(fuse::mechanics::trigger_pair_key(1u, 2u) == ((1ull << 32) | 2ull),
               "trigger_pair_key packs min in the high 32 bits");
}

} // namespace

int main() {
    testTriggerSyncOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }

    testTriggerPairKey();
    fuse::core::initialize();

    const fuse::mechanics::ConvexPolyhedron volume =
        fuse::mechanics::ConvexPolyhedron::axis_aligned_box(0.f, -1.f, -1.f, 2.f, 1.f, 1.f);
    fuse::mechanics::PolyhedronTriggerZone trigger("broadphase_gate", volume);

    fuse::u32 enterCount = 0;
    trigger.setOnEnter([&](fuse::u32) { ++enterCount; });

    fuse::mechanics::BroadphaseTriggerSync sync;
    sync.bindTrigger(&trigger);
    sync.trackBody(1u, 1.f, 0.f, 0.f);
    sync.syncAll();

    expectTrue(enterCount == 1u, "broadphase trigger sync fires enter");
    expectTrue(sync.syncCount() == 1u, "broadphase sync counted");
    expectTrue(sync.candidateCount() >= 1u, "broadphase candidate generated");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_broadphase_trigger_sync: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_broadphase_trigger_sync: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
