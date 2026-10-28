#include <fuse/mechanics/bridge_cell.hpp>
#include <fuse/mechanics/layer_overlap.hpp>
#include <fuse/mechanics/physics_broadphase_bridge.hpp>
#include <fuse/mechanics/switch_component.hpp>
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

void testBridgeCellOk() {
    expectTrue(fuse::mechanics::bridge_cell_ok(1), "bridge_cell_ok(1) is true");
    expectTrue(fuse::mechanics::bridge_cell_ok(256), "bridge_cell_ok(256) is true");
    expectTrue(!fuse::mechanics::bridge_cell_ok(0), "bridge_cell_ok(0) is false");
    expectTrue(!fuse::mechanics::bridge_cell_ok(257), "bridge_cell_ok(257) is false");
}

void testLayersOverlap() {
    expectTrue(fuse::mechanics::layers_overlap(0b0101u, 0b0001u),
               "layers_overlap(0b0101, 0b0001)");
    expectTrue(!fuse::mechanics::layers_overlap(0b0100u, 0b0011u),
               "layers_overlap(0b0100, 0b0011) is false");
    expectTrue(!fuse::mechanics::layers_overlap(0u, 1u), "layers_overlap(0, 1) is false");
}

} // namespace

int main() {
    testBridgeCellOk();
    if (g_failures != 0) {
        return EXIT_FAILURE;
    }

    testLayersOverlap();
    fuse::core::initialize();

    fuse::mechanics::SwitchComponent leverSwitch("lever_switch", false, "armed", "safe");
    expectTrue(!leverSwitch.state(), "switch starts off");
    expectTrue(leverSwitch.flip(), "switch flips on");
    expectTrue(leverSwitch.switchCount() == 1u, "switch count tracked");

    const fuse::mechanics::ConvexPolyhedron volume =
        fuse::mechanics::ConvexPolyhedron::axis_aligned_box(0.f, -1.f, -1.f, 2.f, 1.f, 1.f);
    fuse::mechanics::PolyhedronTriggerZone trigger("pipeline_gate", volume);

    fuse::u32 enterCount = 0;
    trigger.setOnEnter([&](fuse::u32) { ++enterCount; });

    fuse::mechanics::PhysicsBroadphaseBridge bridge;
    bridge.bindTrigger(&trigger);
    bridge.setPositionProvider([](fuse::u32 objectId) -> fuse::mechanics::PhysicsBodyPosition {
        if (objectId == 1u) {
            return {1.f, 0.f, 0.f};
        }
        return {};
    });
    bridge.trackBody(1u, 1.f, 0.f, 0.f);
    bridge.syncBody(1u);

    expectTrue(enterCount == 1u, "physics broadphase pipeline fires trigger");
    expectTrue(bridge.syncCount() == 1u, "physics broadphase sync counted");
    expectTrue(bridge.broadphaseSyncCount() >= 1u, "broadphase stage counted");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mechanics_physics_broadphase_bridge: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mechanics_physics_broadphase_bridge: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
