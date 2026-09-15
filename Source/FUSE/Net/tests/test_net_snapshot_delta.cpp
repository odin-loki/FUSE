#include <fuse/net/checksum.hpp>
#include <fuse/net/rollback.hpp>
#include <fuse/net/snapshot_delta.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

namespace {

fuse::net::GameSnapshot make_entity_snapshot(u32 frame, u32 entity_index, u32 generation, fuse::ecs::vec3 position,
                                           fuse::ecs::vec3 velocity, fuse::f32 mass) {
    fuse::net::GameSnapshot snapshot;
    snapshot.frame = frame;

    fuse::net::NetSerializer ecs_writer;
    ecs_writer.write_u32(entity_index);
    ecs_writer.write_u32(generation);
    ecs_writer.write_vec3(position);
    ecs_writer.write_quat({0.f, 0.f, 0.f, 1.f});
    ecs_writer.write_vec3({1.f, 1.f, 1.f, 0.f});
    snapshot.ecs_state = ecs_writer.buffer;

    fuse::net::NetSerializer physics_writer;
    physics_writer.write_u32(entity_index);
    physics_writer.write_u32(generation);
    physics_writer.write_vec3(velocity);
    physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
    physics_writer.write_f32(mass);
    snapshot.physics_state = physics_writer.buffer;

    snapshot.checksum = fuse::net::compute_snapshot_checksum(snapshot);
    return snapshot;
}

fuse::net::GameSnapshot make_two_entity_snapshot(u32 frame, fuse::ecs::vec3 pos0, fuse::ecs::vec3 pos1) {
    fuse::net::GameSnapshot snapshot;
    snapshot.frame = frame;

    fuse::net::NetSerializer ecs_writer;
    for (u32 entity_index = 0; entity_index < 2; ++entity_index) {
        const fuse::ecs::vec3 position = entity_index == 0 ? pos0 : pos1;
        ecs_writer.write_u32(entity_index);
        ecs_writer.write_u32(1);
        ecs_writer.write_vec3(position);
        ecs_writer.write_quat({0.f, 0.f, 0.f, 1.f});
        ecs_writer.write_vec3({1.f, 1.f, 1.f, 0.f});
    }
    snapshot.ecs_state = ecs_writer.buffer;

    fuse::net::NetSerializer physics_writer;
    for (u32 entity_index = 0; entity_index < 2; ++entity_index) {
        physics_writer.write_u32(entity_index);
        physics_writer.write_u32(1);
        physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
        physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
        physics_writer.write_f32(1.f);
    }
    snapshot.physics_state = physics_writer.buffer;

    snapshot.checksum = fuse::net::compute_snapshot_checksum(snapshot);
    return snapshot;
}

} // namespace

void run_snapshot_delta_tests() {
    fuse::net::GameSnapshot base = make_entity_snapshot(0, 1, 1, {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, 1.f);
    fuse::net::GameSnapshot target =
        make_entity_snapshot(1, 1, 1, {5.f, 0.f, 0.f, 1.f}, {1.f, 0.f, 0.f, 0.f}, 1.f);

    const fuse::net::SnapshotDelta empty_delta = fuse::net::compute_snapshot_delta(base, base);
    expectTrue(empty_delta.kind == fuse::net::SnapshotDeltaKind::None, "identical snapshots produce none delta");
    expectTrue(empty_delta.changed_entity_mask == 0, "empty delta has zero entity mask");

    const fuse::net::GameSnapshot empty_applied = fuse::net::apply_snapshot_delta(base, empty_delta);
    expectTrue(fuse::net::snapshots_equivalent(base, empty_applied), "empty delta preserves baseline state");

    const fuse::net::SnapshotDelta patch_delta = fuse::net::compute_snapshot_delta(base, target);
    expectTrue(patch_delta.kind == fuse::net::SnapshotDeltaKind::EntityPatch, "single entity change produces patch");
    expectTrue(patch_delta.entity_patches.size() == 1u, "one entity patch emitted");
    expectTrue((patch_delta.entity_patches[0].changed_ecs_fields &
                static_cast<fuse::u8>(fuse::net::SnapshotEcsField::Position)) != 0,
               "single-field ecs mask marks position");
    expectTrue((patch_delta.entity_patches[0].changed_physics_fields &
                static_cast<fuse::u8>(fuse::net::SnapshotPhysicsField::LinearVelocity)) != 0,
               "velocity change marked in physics mask");
    expectTrue((patch_delta.changed_entity_mask & (1ull << 1)) != 0, "entity index bit set in changed mask");

    const fuse::net::GameSnapshot rebuilt = fuse::net::apply_snapshot_delta(base, patch_delta);
    expectTrue(rebuilt.ecs_state == target.ecs_state, "patched ecs state matches target");
    expectTrue(rebuilt.physics_state == target.physics_state, "physics state preserved through patch");

    const fuse::net::DeltaApplyResult verified =
        fuse::net::apply_snapshot_delta_verified(base, patch_delta);
    expectTrue(verified.base_checksum_ok, "verified apply accepts matching baseline checksum");
    expectTrue(verified.target_checksum_ok, "verified apply accepts matching target checksum");
    expectTrue(verified.snapshot.ecs_state == target.ecs_state, "verified apply reconstructs ecs state");

    fuse::net::SnapshotDelta bad_checksum_delta = patch_delta;
    bad_checksum_delta.base_checksum = 0xDEADBEEF;
    const fuse::net::DeltaApplyResult bad_base =
        fuse::net::apply_snapshot_delta_verified(base, bad_checksum_delta);
    expectTrue(!bad_base.base_checksum_ok, "checksum mismatch detected on baseline");

    const fuse::u64 delta_hash_a = fuse::net::compute_delta_checksum(patch_delta);
    const fuse::u64 delta_hash_b = fuse::net::compute_delta_checksum(patch_delta);
    expectTrue(delta_hash_a == delta_hash_b, "delta checksum is deterministic");

    fuse::net::NetSerializer wire;
    fuse::net::serialize_snapshot_delta(patch_delta, wire);
    fuse::net::NetSerializer reader;
    reader.buffer = wire.buffer;
    reader.reset_read();
    const fuse::net::SnapshotDelta decoded = fuse::net::deserialize_snapshot_delta(reader);
    expectTrue(decoded.kind == fuse::net::SnapshotDeltaKind::EntityPatch, "delta kind survives wire round-trip");
    expectTrue(decoded.entity_patches.size() == 1u, "patch count survives wire round-trip");
    expectTrue(decoded.changed_entity_mask == patch_delta.changed_entity_mask, "entity mask survives wire round-trip");
    expectTrue(decoded.entity_patches[0].changed_ecs_fields == patch_delta.entity_patches[0].changed_ecs_fields,
               "ecs field mask survives wire round-trip");

    const fuse::net::GameSnapshot roundtrip = fuse::net::apply_snapshot_delta(base, decoded);
    expectTrue(roundtrip.ecs_state == target.ecs_state, "wire round-trip reconstructs ecs state");
    expectTrue(roundtrip.physics_state == target.physics_state, "wire round-trip reconstructs physics state");

    const fuse::net::GameSnapshot multi_base =
        make_two_entity_snapshot(0, {0.f, 0.f, 0.f, 1.f}, {10.f, 0.f, 0.f, 1.f});
    fuse::net::GameSnapshot multi_target = multi_base;
    multi_target.frame = 2;

    fuse::net::NetSerializer ecs_target;
    ecs_target.write_u32(0);
    ecs_target.write_u32(1);
    ecs_target.write_vec3({2.f, 0.f, 0.f, 1.f});
    ecs_target.write_quat({0.f, 0.f, 0.f, 1.f});
    ecs_target.write_vec3({1.f, 1.f, 1.f, 0.f});
    ecs_target.write_u32(1);
    ecs_target.write_u32(1);
    ecs_target.write_vec3({10.f, 0.f, 0.f, 1.f});
    ecs_target.write_quat({0.f, 0.f, 0.f, 1.f});
    ecs_target.write_vec3({1.f, 1.f, 1.f, 0.f});
    multi_target.ecs_state = ecs_target.buffer;
    multi_target.checksum = fuse::net::compute_snapshot_checksum(multi_target);

    const fuse::net::SnapshotDelta multi_delta = fuse::net::compute_snapshot_delta(multi_base, multi_target);
    expectTrue(multi_delta.kind == fuse::net::SnapshotDeltaKind::EntityPatch, "multi-entity diff stays patch");
    expectTrue(multi_delta.entity_patches.size() == 1u, "only changed entity patched");
    expectTrue((multi_delta.changed_entity_mask & (1ull << 0)) != 0, "entity 0 marked in mask");
    expectTrue((multi_delta.changed_entity_mask & (1ull << 1)) == 0, "unchanged entity 1 absent from mask");

    fuse::net::SnapshotHistoryRing history;
    history.init(8);
    history.push(base);

    fuse::net::GameSnapshot stored{};
    const bool stored_ok = history.apply_delta_and_store(base.frame, patch_delta, &stored);
    expectTrue(stored_ok, "history ring applies delta from stored baseline");
    expectTrue(history.has_frame(1), "history ring retains reconstructed frame");
    expectTrue(stored.ecs_state == target.ecs_state, "history ring stores reconstructed ecs state");

    const fuse::net::GameSnapshot* newest = history.newest();
    expectTrue(newest != nullptr && newest->frame == 1, "newest snapshot points at reconstructed frame");
}

} // namespace fuse::net::tests
