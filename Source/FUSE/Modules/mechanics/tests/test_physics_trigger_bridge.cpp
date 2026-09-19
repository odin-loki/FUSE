#include <fuse/mechanics/physics_trigger_bridge.hpp>
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
