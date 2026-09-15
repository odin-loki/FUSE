#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/net/state_sync.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_state_sync_tests() {
    fuse::ecs::Registry registry;
    registry.init(16);

    const fuse::ecs::EntityID entity = registry.create();
    fuse::ecs::Transform initial{};
    initial.position = {0.f, 0.f, 0.f, 1.f};
    registry.add<fuse::ecs::Transform>(entity, initial);

    fuse::net::ClientInterpolator interpolator;
    interpolator.set_interpolation_delay_ms(100);

    fuse::net::EntityNetState prev{};
    prev.entity = entity;
    prev.position = {0.f, 0.f, 0.f, 1.f};
    prev.rotation = {0.f, 0.f, 0.f, 1.f};
    prev.timestamp = 0;
    prev.sequence = 1;
    interpolator.receive_state(prev);

    fuse::net::EntityNetState next = prev;
    next.position = {10.f, 0.f, 0.f, 1.f};
    next.timestamp = 100000;
    next.sequence = 2;
    interpolator.receive_state(next);

    interpolator.update(registry, 150000);
    const fuse::ecs::Transform* mid = registry.get<fuse::ecs::Transform>(entity);
    expectTrue(mid != nullptr, "transform exists during interpolation");
    expectNear(mid->position.x, 5.f, 0.25f, "interpolated halfway between snapshots");

    interpolator.update(registry, 200000);
    const fuse::ecs::Transform* end = registry.get<fuse::ecs::Transform>(entity);
    expectNear(end->position.x, 10.f, 0.25f, "interpolated to latest snapshot");

    registry.destroy();
}

} // namespace fuse::net::tests
