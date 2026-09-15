#include <fuse/net/rollback.hpp>
#include <fuse/net/snapshot_delta.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_snapshot_delta_tests() {
    fuse::net::GameSnapshot base;
    base.frame = 0;
    base.checksum = 100;

    fuse::net::NetSerializer ecs_base;
    ecs_base.write_u32(1);
    ecs_base.write_u32(1);
    ecs_base.write_vec3({0.f, 0.f, 0.f, 1.f});
    ecs_base.write_quat({0.f, 0.f, 0.f, 1.f});
    ecs_base.write_vec3({1.f, 1.f, 1.f, 0.f});
    base.ecs_state = ecs_base.buffer;

    fuse::net::NetSerializer physics_base;
    physics_base.write_u32(1);
    physics_base.write_u32(1);
    physics_base.write_vec3({0.f, 0.f, 0.f, 0.f});
    physics_base.write_vec3({0.f, 0.f, 0.f, 0.f});
    physics_base.write_f32(1.f);
    base.physics_state = physics_base.buffer;

    fuse::net::GameSnapshot target = base;
    target.frame = 1;
    target.checksum = 101;

    fuse::net::NetSerializer ecs_target;
    ecs_target.write_u32(1);
    ecs_target.write_u32(1);
    ecs_target.write_vec3({5.f, 0.f, 0.f, 1.f});
    ecs_target.write_quat({0.f, 0.f, 0.f, 1.f});
    ecs_target.write_vec3({1.f, 1.f, 1.f, 0.f});
    target.ecs_state = ecs_target.buffer;

    const fuse::net::SnapshotDelta none_delta = fuse::net::compute_snapshot_delta(base, base);
    expectTrue(none_delta.kind == fuse::net::SnapshotDeltaKind::None, "identical snapshots produce none delta");

    const fuse::net::SnapshotDelta patch_delta = fuse::net::compute_snapshot_delta(base, target);
    expectTrue(patch_delta.kind == fuse::net::SnapshotDeltaKind::EntityPatch, "single entity change produces patch");
    expectTrue(patch_delta.entity_patches.size() == 1u, "one entity patch emitted");

    const fuse::net::GameSnapshot rebuilt = fuse::net::apply_snapshot_delta(base, patch_delta);
    expectTrue(rebuilt.ecs_state == target.ecs_state, "patched ecs state matches target");
    expectTrue(rebuilt.physics_state == target.physics_state, "physics state preserved through patch");

    fuse::net::NetSerializer wire;
    fuse::net::serialize_snapshot_delta(patch_delta, wire);
    fuse::net::NetSerializer reader;
    reader.buffer = wire.buffer;
    reader.reset_read();
    const fuse::net::SnapshotDelta decoded = fuse::net::deserialize_snapshot_delta(reader);
    expectTrue(decoded.kind == fuse::net::SnapshotDeltaKind::EntityPatch, "delta kind survives wire round-trip");
    expectTrue(decoded.entity_patches.size() == 1u, "patch count survives wire round-trip");
}

} // namespace fuse::net::tests
