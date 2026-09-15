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

    fuse::net::StateSyncDeltaBroadcaster broadcaster;
    fuse::net::StateSyncSnapshot base_sync{};
    base_sync.frame = 0;
    fuse::net::EntityNetState base_entity{};
    base_entity.entity = entity;
    base_entity.position = {0.f, 0.f, 0.f, 1.f};
    base_entity.rotation = {0.f, 0.f, 0.f, 1.f};
    base_entity.linear_velocity = {0.f, 0.f, 0.f, 0.f};
    base_sync.entities.push_back(base_entity);
    broadcaster.set_base_snapshot(base_sync);

    fuse::net::StateSyncSnapshot target_sync = base_sync;
    target_sync.frame = 1;
    target_sync.entities[0].position = {3.f, 0.f, 0.f, 1.f};
    target_sync.entities[0].linear_velocity = {1.f, 0.f, 0.f, 0.f};

    const fuse::net::EntityStateDelta entity_delta =
        broadcaster.compute_entity_delta(base_sync.entities[0], target_sync.entities[0]);
    expectTrue((entity_delta.changed_fields &
                static_cast<fuse::u8>(fuse::net::EntityStateField::Position)) != 0,
               "entity delta marks position change");
    expectTrue((entity_delta.changed_fields &
                static_cast<fuse::u8>(fuse::net::EntityStateField::LinearVelocity)) != 0,
               "entity delta marks velocity change");

    const fuse::net::EntityNetState merged =
        broadcaster.apply_entity_delta(base_sync.entities[0], entity_delta);
    expectNear(merged.position.x, 3.f, 1e-4f, "entity delta apply updates position");

    const fuse::net::SnapshotDelta world_delta = broadcaster.compute_world_delta(target_sync);
    expectTrue(world_delta.kind == fuse::net::SnapshotDeltaKind::EntityPatch ||
                   world_delta.kind == fuse::net::SnapshotDeltaKind::Full,
               "world delta produced for authoritative snapshot");

    registry.destroy();
}

} // namespace fuse::net::tests
