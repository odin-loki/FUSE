#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/net/rollback.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_rollback_tests() {
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

    const fuse::u32 advance_end_frame = rollback.current_frame() + 6;
    for (fuse::u32 frame = rollback.current_frame(); frame < advance_end_frame; ++frame) {
        fuse::net::PlayerInput advance{};
        advance.frame = frame;
        advance.axis_lx = 100;
        rollback.set_local_input(advance);
        rollback.tick(dt);
    }

    // --- RollbackManager input-buffer and frame guards (B7.4 deepen follow-up) ---
    expectTrue(rollback.has_input_buffer(), "rollback manager reports input buffer ready");
    expectTrue(rollback.can_set_local_input(late_remote) == false,
               "can_set_local_input rejects non-current frame");

    fuse::net::PlayerInput current_local{};
    current_local.frame = rollback.current_frame();
    current_local.axis_lx = 500;
    expectTrue(rollback.can_set_local_input(current_local), "can_set_local_input accepts current frame");
    const fuse::u32 store_frame = rollback.current_frame();
    rollback.set_local_input(current_local);
    rollback.tick(dt);
    expectTrue(rollback.buffer().has_local_input(store_frame),
               "set_local_input stores input for ticked frame");

    fuse::net::PlayerInput wrong_local{};
    wrong_local.frame = rollback.current_frame() + 1;
    wrong_local.axis_lx = 500;
    rollback.set_local_input(wrong_local);
    expectTrue(!rollback.buffer().has_local_input(wrong_local.frame),
               "set_local_input ignores non-current frame");

    fuse::net::PlayerInput too_old{};
    too_old.frame = 0;
    too_old.player_id = 2;
    too_old.axis_lx = 20000;
    expectTrue(!rollback.can_apply_remote_input(too_old),
               "can_apply_remote_input rejects input beyond rollback window");
    expectTrue(!rollback.apply_remote_input(too_old),
               "apply_remote_input rejects input beyond rollback window");

    fuse::net::RollbackManager destroyed_buffer;
    destroyed_buffer.init(4);
    destroyed_buffer.bind_registry(&registry);
    destroyed_buffer.destroy();
    expectTrue(!destroyed_buffer.has_input_buffer(), "destroyed manager has no input buffer");
    expectTrue(!destroyed_buffer.can_set_local_input(current_local),
               "can_set_local_input false without input buffer");

    registry.destroy();
    rollback.destroy();
}

} // namespace fuse::net::tests
