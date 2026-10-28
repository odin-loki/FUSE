#include <fuse/core/init.hpp>
#include <fuse/mechanics/overlap_fraction.hpp>
#include <fuse/mechanics/physics_trigger_bridge.hpp>
#include <fuse/mechanics/trigger_mask.hpp>

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

void testTriggerMaskOk() {
    expectTrue(fuse::mechanics::trigger_mask_ok(0), "trigger_mask_ok(0) is true");
    expectTrue(fuse::mechanics::trigger_mask_ok(31), "trigger_mask_ok(31) is true");
    expectTrue(!fuse::mechanics::trigger_mask_ok(-1), "trigger_mask_ok(-1) is false");
    expectTrue(!fuse::mechanics::trigger_mask_ok(32), "trigger_mask_ok(32) is false");
}

void testOverlapFraction() {
    expectTrue(fuse::mechanics::overlap_fraction(0.25f, 1.f) == 0.25f,
               "overlap_fraction(0.25, 1) == 0.25");
    expectTrue(fuse::mechanics::overlap_fraction(2.f, 1.f) == 1.f, "overlap_fraction(2, 1) == 1");
    expectTrue(fuse::mechanics::overlap_fraction(-1.f, 1.f) == 0.f, "overlap_fraction(-1, 1) == 0");
    expectTrue(fuse::mechanics::overlap_fraction(1.f, 0.f) == 0.f, "overlap_fraction(1, 0) == 0");
    expectTrue(fuse::mechanics::overlap_fraction(0.f, 1.f) == 0.f, "zero depth returns 0");
    expectTrue(fuse::mechanics::overlap_fraction(1.f, -2.f) == 0.f, "negative size returns 0");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expectTrue(fuse::mechanics::overlap_fraction(nan, 1.f) == 0.f, "non-finite depth returns 0");
    expectTrue(fuse::mechanics::overlap_fraction(1.f, nan) == 0.f, "non-finite size returns 0");
    expectTrue(fuse::mechanics::overlap_fraction(inf, 1.f) == 0.f, "infinite depth returns 0");
    expectTrue(fuse::mechanics::overlap_fraction(1.f, -inf) == 0.f, "infinite size returns 0");
}

} // namespace

int main() {
    testTriggerMaskOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }

    testOverlapFraction();
    fuse::core::initialize();

    const fuse::mechanics::ConvexPolyhedron volume =
        fuse::mechanics::ConvexPolyhedron::axis_aligned_box(0.f, -1.f, -1.f, 2.f, 1.f, 1.f);
    fuse::mechanics::PolyhedronTriggerZone trigger("physics_gate", volume);

    fuse::u32 enterCount = 0;
    trigger.setOnEnter([&](fuse::u32) { ++enterCount; });

    fuse::mechanics::PhysicsTriggerBridge bridge;
    bridge.bind(&trigger);
    bridge.setPositionProvider([](fuse::u32 objectId) -> fuse::mechanics::PhysicsBodyPosition {
        if (objectId == 1u) {
            return {1.f, 0.f, 0.f};
        }
        return {-5.f, 0.f, 0.f};
    });

    bridge.syncObject(1u);
    expectTrue(enterCount == 1u, "physics bridge fires enter when body inside polyhedron");
    expectTrue(trigger.enterCount() == 1u, "trigger enter count matches");
    expectTrue(bridge.syncCount() == 1u, "bridge sync counted");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_physics_trigger_bridge: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_physics_trigger_bridge: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
