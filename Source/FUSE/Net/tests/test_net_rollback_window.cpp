#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/net/rollback.hpp>
#include <fuse/net/rollback_window.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_rollback_window_tests() {
    expectTrue(fuse::net::can_rewind_to_frame(10, 8, 4), "rewind within window allowed");
    expectTrue(!fuse::net::can_rewind_to_frame(10, 5, 4), "rewind beyond window rejected");
    expectTrue(!fuse::net::can_rewind_to_frame(10, 12, 4), "rewind to future rejected");
    expectTrue(fuse::net::can_rewind_to_frame(10, 10, 4), "rewind to current frame allowed");

    expectTrue(fuse::net::resimulate_frame_count(5, 9) == 4u, "resimulate count spans gap");
    expectTrue(fuse::net::resimulate_frame_count(9, 5) == 0u, "resimulate count zero when behind");
    expectTrue(fuse::net::resimulate_frame_count(7, 7) == 0u, "resimulate count zero at same frame");

    fuse::ecs::Registry registry;
    registry.init(64);

    const fuse::ecs::EntityID entity = registry.create();
    registry.add<fuse::ecs::Transform>(entity);
    registry.add<fuse::ecs::RigidBody>(entity);

    fuse::net::RollbackManager rollback;
    rollback.init(4);
    rollback.bind_registry(&registry);

    constexpr fuse::f32 dt = 1.f / 60.f;
    for (fuse::u32 frame = 0; frame < 6; ++frame) {
        fuse::net::PlayerInput local{};
        local.frame = frame;
        local.axis_lx = 1000;
        rollback.set_local_input(local);
        rollback.tick(dt);
    }

    expectTrue(rollback.current_frame() == 6u, "rollback advanced to frame 6");
    expectTrue(rollback.can_rewind_to(4), "snapshot within window is rewindable");
    expectTrue(!rollback.can_rewind_to(1), "snapshot outside window is not rewindable");
    expectTrue(!rollback.rewind_to(1), "rewind_to rejects out-of-window frame");
    expectTrue(!rollback.rewind_to(rollback.current_frame() + 1), "rewind_to rejects future frame");
    expectTrue(rollback.resimulate_count_to(6) == 0u, "resimulate count zero at current frame");
    expectTrue(rollback.resimulate_count_to(8) == 2u, "resimulate count stub reports forward gap");

    const fuse::f32 position_before = registry.get<fuse::ecs::Transform>(entity)->position.x;
    expectTrue(rollback.rewind_to(4), "rewind_to succeeds inside window");
    expectTrue(rollback.current_frame() == 4u, "rewind_to updates current frame");

    const fuse::f32 position_after = registry.get<fuse::ecs::Transform>(entity)->position.x;
    expectTrue(position_after != position_before, "rewind_to restored earlier snapshot state");

    registry.destroy();
    rollback.destroy();
}

} // namespace fuse::net::tests
