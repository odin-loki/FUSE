#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/net/rewind_count.hpp>
#include <fuse/net/rewind_frames.hpp>
#include <fuse/net/rollback.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void testRewindCount() {
    expectTrue(fuse::net::rewind_count(10, 7, 8) == 3u, "rewind_count is the frame gap inside the cap");
    expectTrue(fuse::net::rewind_count(20, 5, 4) == 4u, "rewind_count caps at maxFrames");
    expectTrue(fuse::net::rewind_count(3, 9, 8) == 0u, "rewind_count is zero when current is behind target");
}

bool testRewindFramesOk() {
    const int before = g_failures;
    expectTrue(fuse::net::rewind_frames_ok(0), "rewind_frames_ok accepts 0");
    expectTrue(fuse::net::rewind_frames_ok(32), "rewind_frames_ok accepts 32");
    expectTrue(!fuse::net::rewind_frames_ok(-1), "rewind_frames_ok rejects -1");
    expectTrue(!fuse::net::rewind_frames_ok(33), "rewind_frames_ok rejects 33");
    return g_failures == before;
}

void run_rollback_tests() {
    if (!testRewindFramesOk()) {
        return;
    }

    testRewindCount();

    fuse::ecs::Registry registry;
    registry.init(64);

    const fuse::ecs::EntityID entity = registry.create();
    registry.add<fuse::ecs::Transform>(entity);
    registry.add<fuse::ecs::RigidBody>(entity);

    fuse::net::RollbackManager rollback;
    rollback.init(8);
    rollback.bind_registry(&registry);

    constexpr fuse::f32 dt = 1.f / 60.f;

    for (fuse::u32 frame = 0; frame < 3; ++frame) {
        fuse::net::PlayerInput local{};
        local.frame = frame;
        local.player_id = 0;
        local.axis_lx = 1000;
        rollback.set_local_input(local);
        rollback.tick(dt);
    }

    const fuse::ecs::Transform* transform = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(transform != nullptr, "transform exists after ticks");
    expectTrue(transform->position.x > 0.f, "local input advanced position.x");

    const fuse::f32 position_before_rollback = transform->position.x;

    fuse::net::PlayerInput late_remote{};
    late_remote.frame = 1;
    late_remote.player_id = 1;
    late_remote.axis_lx = 30000;
    const bool rolled_back = rollback.apply_remote_input(late_remote);
    expectTrue(rolled_back, "late remote input triggers rollback");
    expectTrue(rollback.is_rolling_back() == false, "rolling back flag cleared after resim");

    const fuse::ecs::Transform* after = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(after != nullptr, "transform survives rollback");
    expectTrue(after->position.x != position_before_rollback, "rollback resimulation changed position");
    expectTrue(rollback.buffer().has_frame(1), "rollback buffer retains resimulated frame");

    fuse::net::PlayerInput future{};
    future.frame = rollback.current_frame() + 4;
    expectTrue(!rollback.apply_remote_input(future), "future remote input rejected");

    registry.destroy();
    rollback.destroy();
}

} // namespace fuse::net::tests
