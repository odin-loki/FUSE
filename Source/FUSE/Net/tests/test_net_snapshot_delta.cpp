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

fuse::net::GameSnapshot make_entity_snapshot_with_rotation(u32 frame, u32 entity_index, u32 generation,
                                                           fuse::ecs::vec3 position, fuse::ecs::quat rotation) {
    fuse::net::GameSnapshot snapshot;
    snapshot.frame = frame;

    fuse::net::NetSerializer ecs_writer;
    ecs_writer.write_u32(entity_index);
    ecs_writer.write_u32(generation);
    ecs_writer.write_vec3(position);
    ecs_writer.write_quat(rotation);
    ecs_writer.write_vec3({1.f, 1.f, 1.f, 0.f});
    snapshot.ecs_state = ecs_writer.buffer;

    fuse::net::NetSerializer physics_writer;
    physics_writer.write_u32(entity_index);
    physics_writer.write_u32(generation);
    physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
    physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
    physics_writer.write_f32(1.f);
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
    expectTrue(fuse::net::is_empty_snapshot_delta(empty_delta), "empty delta helper reports none kind");
    expectTrue(fuse::net::validate_delta_payload(empty_delta), "empty delta payload validates");

    const fuse::net::GameSnapshot empty_applied = fuse::net::apply_snapshot_delta(base, empty_delta);
    expectTrue(fuse::net::snapshots_equivalent(base, empty_applied), "empty delta preserves baseline state");
    expectTrue(fuse::net::validate_changed_entity_mask(empty_delta), "empty delta has zero entity mask");

    const fuse::net::SnapshotDeltaPreflight empty_preflight = fuse::net::preflight_snapshot_delta(base, empty_delta);
    expectTrue(empty_preflight.can_apply(), "empty delta preflight passes");
    expectTrue(empty_preflight.base_checksum_ok, "empty delta preflight accepts baseline checksum");
    expectTrue(empty_preflight.entity_mask_ok, "empty delta preflight accepts entity mask");

    const fuse::net::DeltaApplyResult empty_verified =
        fuse::net::apply_snapshot_delta_verified(base, empty_delta);
    expectTrue(empty_verified.base_checksum_ok, "empty delta verified apply accepts baseline");
    expectTrue(empty_verified.entity_mask_ok, "empty delta verified apply accepts entity mask");
    expectTrue(fuse::net::snapshots_equivalent(base, empty_verified.snapshot),
               "empty delta verified apply preserves baseline state");

    fuse::net::SnapshotHistoryRing empty_history;
    empty_history.init(4);
    empty_history.push(base);
    expectTrue(empty_history.can_apply_delta(base.frame, empty_delta), "history ring can apply empty delta");
    fuse::net::GameSnapshot empty_stored{};
    expectTrue(empty_history.apply_delta_and_store(base.frame, empty_delta, &empty_stored),
               "history ring stores empty delta result");
    expectTrue(empty_stored.frame == base.frame, "empty delta retains target frame from delta");

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
    expectTrue(verified.entity_mask_ok, "verified apply accepts consistent entity mask");
    expectTrue(verified.snapshot.ecs_state == target.ecs_state, "verified apply reconstructs ecs state");
    expectTrue(fuse::net::validate_changed_entity_mask(patch_delta), "computed delta mask matches patches");
    expectTrue(fuse::net::entity_index_in_changed_mask(patch_delta.changed_entity_mask, 1),
               "entity mask helper reports patched index");
    expectTrue(fuse::net::count_changed_entities_in_mask(patch_delta.changed_entity_mask) == 1u,
               "entity mask popcount matches patch count");
    expectTrue(fuse::net::ecs_field_mask_contains(patch_delta.entity_patches[0].changed_ecs_fields,
                                                  fuse::net::SnapshotEcsField::Position),
               "ecs mask helper reports position field");
    expectTrue(!fuse::net::ecs_field_mask_contains(patch_delta.entity_patches[0].changed_ecs_fields,
                                                   fuse::net::SnapshotEcsField::Rotation),
               "ecs mask helper omits unchanged rotation field");
    expectTrue(fuse::net::ecs_field_mask_count(patch_delta.entity_patches[0].changed_ecs_fields) == 1u,
               "ecs mask popcount matches changed fields");
    expectTrue(fuse::net::physics_field_mask_contains(patch_delta.entity_patches[0].changed_physics_fields,
                                                      fuse::net::SnapshotPhysicsField::LinearVelocity),
               "physics mask helper reports velocity field");
    expectTrue(fuse::net::physics_field_mask_count(patch_delta.entity_patches[0].changed_physics_fields) == 1u,
               "physics mask popcount matches changed fields");
    const fuse::u8 ecs_union = fuse::net::ecs_field_mask_union(
        static_cast<fuse::u8>(fuse::net::SnapshotEcsField::Position),
        static_cast<fuse::u8>(fuse::net::SnapshotEcsField::Rotation));
    expectTrue(fuse::net::ecs_field_mask_contains(ecs_union, fuse::net::SnapshotEcsField::Position),
               "ecs mask union retains position bit");
    expectTrue(fuse::net::ecs_field_mask_contains(ecs_union, fuse::net::SnapshotEcsField::Rotation),
               "ecs mask union retains rotation bit");
    const fuse::u8 physics_union = fuse::net::physics_field_mask_union(
        static_cast<fuse::u8>(fuse::net::SnapshotPhysicsField::LinearVelocity),
        static_cast<fuse::u8>(fuse::net::SnapshotPhysicsField::Mass));
    expectTrue(fuse::net::physics_field_mask_count(physics_union) == 2u, "physics mask union popcount");
    expectTrue(fuse::net::validate_entity_patch_masks(patch_delta.entity_patches[0]),
               "computed patch masks validate");
    expectTrue(!fuse::net::is_empty_snapshot_delta(patch_delta), "patch delta is not empty");
    expectTrue(empty_history.can_apply_delta(base.frame, patch_delta), "history ring preflights patch delta");

    fuse::net::SnapshotDelta bad_checksum_delta = patch_delta;
    bad_checksum_delta.base_checksum = 0xDEADBEEF;
    const fuse::net::DeltaApplyResult bad_base =
        fuse::net::apply_snapshot_delta_verified(base, bad_checksum_delta);
    expectTrue(!bad_base.base_checksum_ok, "checksum mismatch detected on baseline");

    fuse::net::SnapshotDelta bad_frame_delta = patch_delta;
    bad_frame_delta.base_frame = 99u;
    const fuse::net::SnapshotDeltaPreflight bad_frame_preflight =
        fuse::net::preflight_snapshot_delta(base, bad_frame_delta);
    expectTrue(!bad_frame_preflight.base_frame_ok, "preflight rejects mismatched base frame");
    expectTrue(!bad_frame_preflight.can_apply(), "preflight can_apply fails on frame mismatch");
    expectTrue(!empty_history.can_apply_delta(base.frame, bad_frame_delta),
               "history ring rejects delta with mismatched base_frame field");

    fuse::net::SnapshotDelta empty_patch_delta = patch_delta;
    empty_patch_delta.entity_patches.clear();
    expectTrue(!fuse::net::validate_delta_payload(empty_patch_delta),
               "entity patch kind with no rows fails payload validation");
    const fuse::net::SnapshotDeltaPreflight bad_payload_preflight =
        fuse::net::preflight_snapshot_delta(base, empty_patch_delta);
    expectTrue(!bad_payload_preflight.payload_ok, "preflight rejects invalid entity patch payload");

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

    fuse::net::GameSnapshot mass_base =
        make_entity_snapshot(20, 2, 1, {1.f, 2.f, 3.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, 1.f);
    fuse::net::GameSnapshot mass_target = mass_base;
    mass_target.frame = 21;
    {
        fuse::net::NetSerializer physics_writer;
        physics_writer.write_u32(2);
        physics_writer.write_u32(1);
        physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
        physics_writer.write_vec3({0.f, 0.f, 0.f, 0.f});
        physics_writer.write_f32(9.f);
        mass_target.physics_state = physics_writer.buffer;
    }
    mass_target.checksum = fuse::net::compute_snapshot_checksum(mass_target);

    const fuse::net::SnapshotDelta mass_delta = fuse::net::compute_snapshot_delta(mass_base, mass_target);
    expectTrue(mass_delta.entity_patches.size() == 1u, "mass-only change emits one patch");
    expectTrue(mass_delta.entity_patches[0].changed_ecs_fields == 0, "mass-only change skips ecs mask");
    expectTrue((mass_delta.entity_patches[0].changed_physics_fields &
                static_cast<fuse::u8>(fuse::net::SnapshotPhysicsField::Mass)) != 0,
               "mass-only change marks physics mass field");

    const fuse::net::GameSnapshot mass_applied = fuse::net::apply_snapshot_delta(mass_base, mass_delta);
    expectTrue(mass_applied.ecs_state == mass_base.ecs_state, "partial mask apply preserves ecs state");
    expectTrue(mass_applied.physics_state == mass_target.physics_state, "partial mask apply updates physics");

    const fuse::net::GameSnapshot rotation_base = make_entity_snapshot_with_rotation(
        30, 4, 1, {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f});
    fuse::net::GameSnapshot rotation_target = rotation_base;
    rotation_target.frame = 31;
    {
        fuse::net::NetSerializer ecs_writer;
        ecs_writer.write_u32(4);
        ecs_writer.write_u32(1);
        ecs_writer.write_vec3({0.f, 0.f, 0.f, 1.f});
        ecs_writer.write_quat({0.f, 0.70710677f, 0.f, 0.70710677f});
        ecs_writer.write_vec3({1.f, 1.f, 1.f, 0.f});
        rotation_target.ecs_state = ecs_writer.buffer;
    }
    rotation_target.checksum = fuse::net::compute_snapshot_checksum(rotation_target);

    const fuse::net::SnapshotDelta rotation_delta = fuse::net::compute_snapshot_delta(rotation_base, rotation_target);
    expectTrue(rotation_delta.entity_patches.size() == 1u, "rotation-only change emits one patch");
    expectTrue(rotation_delta.entity_patches[0].changed_ecs_fields ==
                   static_cast<fuse::u8>(fuse::net::SnapshotEcsField::Rotation),
               "rotation-only change marks rotation ecs field");
    expectTrue(rotation_delta.entity_patches[0].changed_physics_fields == 0, "rotation-only change skips physics mask");

    const fuse::net::GameSnapshot rotation_applied = fuse::net::apply_snapshot_delta(rotation_base, rotation_delta);
    expectTrue(rotation_applied.ecs_state == rotation_target.ecs_state, "rotation-only mask apply updates ecs state");
    expectTrue(rotation_applied.physics_state == rotation_base.physics_state,
               "rotation-only mask apply preserves physics state");

    fuse::net::SnapshotDelta bad_mask_delta = patch_delta;
    bad_mask_delta.changed_entity_mask |= (1ull << 5);
    expectTrue(!fuse::net::validate_changed_entity_mask(bad_mask_delta), "stray mask bit fails validation");
    const fuse::net::DeltaApplyResult bad_mask_verified =
        fuse::net::apply_snapshot_delta_verified(base, bad_mask_delta);
    expectTrue(!bad_mask_verified.entity_mask_ok, "verified apply rejects inconsistent entity mask");

    fuse::net::SnapshotHistoryRing wrap_history;
    wrap_history.init(4);
    for (fuse::u32 frame = 0; frame < 10; ++frame) {
        const fuse::ecs::vec3 position = {static_cast<fuse::f32>(frame), 0.f, 0.f, 1.f};
        wrap_history.push(make_entity_snapshot(frame, 3, 1, position, {0.f, 0.f, 0.f, 0.f}, 1.f));
    }

    expectTrue(wrap_history.stored_frame_count() == 4u, "history ring retains at most capacity frames");
    expectTrue(wrap_history.oldest_frame() == 6u, "history ring evicts oldest frames on wrap");
    expectTrue(wrap_history.newest_frame() == 9u, "history ring tracks newest frame across wrap");
    expectTrue(!wrap_history.has_frame(5u), "evicted frame no longer queryable");
    expectTrue(wrap_history.has_frame(8u), "pre-wrap baseline frame still retained");

    fuse::net::GameSnapshot wrap_target =
        make_entity_snapshot(10, 3, 1, {99.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, 1.f);
    const fuse::net::GameSnapshot* wrap_base = wrap_history.get(8u);
    expectTrue(wrap_base != nullptr, "wrapped history ring exposes baseline frame");
    const fuse::net::SnapshotDelta wrap_delta = fuse::net::compute_snapshot_delta(*wrap_base, wrap_target);

    fuse::net::GameSnapshot wrap_stored{};
    const bool wrap_ok = wrap_history.apply_delta_and_store(8u, wrap_delta, &wrap_stored);
    expectTrue(wrap_ok, "history ring applies delta after wrap from retained baseline");
    expectTrue(wrap_history.has_frame(10u), "history ring stores delta result after wrap");
    expectTrue(wrap_stored.ecs_state == wrap_target.ecs_state, "wrapped history ring reconstructs ecs state");
    expectTrue(wrap_history.can_apply_delta(8u, wrap_delta), "wrapped history ring preflights retained baseline");

    const std::optional<fuse::net::GameSnapshot> popped = wrap_history.pop_oldest();
    expectTrue(popped.has_value(), "history ring pop_oldest returns evicted snapshot");
    expectTrue(popped->frame == 7u, "history ring pop_oldest evicts oldest retained frame");
    expectTrue(wrap_history.stored_frame_count() == 3u, "history ring pop_oldest reduces retained count");
    expectTrue(!wrap_history.has_frame(7u), "history ring pop_oldest removes evicted frame");

    expectTrue(!wrap_history.can_apply_delta(99u, wrap_delta), "history ring rejects missing baseline frame");

    fuse::net::SnapshotHistoryRing empty_ring;
    empty_ring.init(4);
    expectTrue(empty_ring.empty(), "uninitialized history ring reports empty");
    expectTrue(!empty_ring.pop_oldest().has_value(), "pop_oldest on empty history ring returns nullopt");
    expectTrue(!empty_ring.can_apply_delta(0u, empty_delta), "empty history ring rejects delta apply");

    fuse::net::GameSnapshot high_index_base =
        make_entity_snapshot(40, 64, 1, {1.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, 1.f);
    fuse::net::GameSnapshot high_index_target =
        make_entity_snapshot(41, 64, 1, {2.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, 1.f);
    const fuse::net::SnapshotDelta high_index_delta =
        fuse::net::compute_snapshot_delta(high_index_base, high_index_target);
    expectTrue(high_index_delta.kind == fuse::net::SnapshotDeltaKind::Full,
               "entity index >= 64 falls back to full snapshot delta");

    fuse::net::SnapshotDelta bad_target_delta = patch_delta;
    bad_target_delta.target_checksum = 0xBADC0DE;
    expectTrue(!history.apply_delta_and_store(base.frame, bad_target_delta, nullptr),
               "history ring rejects delta with bad target checksum");

    // --- empty-delta guards (B7.4 deepen follow-up) ---
    expectTrue(fuse::net::should_skip_delta_apply(empty_delta), "empty delta triggers skip guard");
    expectTrue(!fuse::net::should_skip_delta_apply(patch_delta), "patch delta does not trigger skip guard");
    expectTrue(fuse::net::can_apply_snapshot_delta(base, empty_delta), "can_apply accepts empty delta");
    expectTrue(fuse::net::can_apply_snapshot_delta(base, patch_delta), "can_apply accepts valid patch delta");
    expectTrue(!fuse::net::can_apply_snapshot_delta(base, bad_checksum_delta),
               "can_apply rejects checksum mismatch");

    const fuse::net::SnapshotDeltaPreflight deepen_preflight =
        fuse::net::preflight_snapshot_delta(base, empty_delta);
    expectTrue(deepen_preflight.empty_delta, "preflight marks empty delta");
    expectTrue(deepen_preflight.mask_popcount_ok, "empty delta mask popcount validates");
    expectTrue(deepen_preflight.can_apply(), "empty delta preflight can_apply");

    expectTrue(fuse::net::entity_mask_popcount_matches_patches(patch_delta),
               "patch delta mask popcount matches patch rows");
    expectTrue(fuse::net::entity_mask_popcount_matches_patches(empty_delta),
               "empty delta mask popcount trivially matches");

    fuse::net::SnapshotDelta bad_popcount_delta = patch_delta;
    bad_popcount_delta.changed_entity_mask |= (1ull << 2);
    expectTrue(!fuse::net::entity_mask_popcount_matches_patches(bad_popcount_delta),
               "stray mask bit fails popcount helper");
    expectTrue(!fuse::net::preflight_snapshot_delta(base, bad_popcount_delta).mask_popcount_ok,
               "preflight rejects mask popcount mismatch");

    // --- mask payload size validation (B7.4 deepen follow-up) ---
    expectTrue(fuse::net::ecs_field_mask_nonempty(patch_delta.entity_patches[0].changed_ecs_fields),
               "ecs mask nonempty helper");
    expectTrue(!fuse::net::ecs_field_mask_nonempty(0), "zero ecs mask is empty");
    expectTrue(fuse::net::physics_field_mask_nonempty(patch_delta.entity_patches[0].changed_physics_fields),
               "physics mask nonempty helper");
    expectTrue(fuse::net::ecs_field_mask_subset(
                   patch_delta.entity_patches[0].changed_ecs_fields,
                   static_cast<fuse::u8>(fuse::net::SnapshotEcsField::All)),
               "ecs mask subset helper accepts declared fields");
    expectTrue(!fuse::net::ecs_field_mask_subset(static_cast<fuse::u8>(fuse::net::SnapshotEcsField::All),
                                                 patch_delta.entity_patches[0].changed_ecs_fields),
               "ecs mask subset helper rejects superset claim");

    const fuse::u32 expected_ecs_bytes = fuse::net::expected_ecs_patch_bytes(
        patch_delta.entity_patches[0].changed_ecs_fields);
    expectTrue(expected_ecs_bytes == 12u, "position-only ecs patch expects 12 bytes");
    expectTrue(fuse::net::validate_entity_patch_payload_sizes(patch_delta.entity_patches[0]),
               "computed patch payload sizes validate");

    fuse::net::SnapshotEntityPatch truncated_patch = patch_delta.entity_patches[0];
    truncated_patch.ecs_bytes.resize(4);
    expectTrue(!fuse::net::validate_entity_patch_payload_sizes(truncated_patch),
               "truncated ecs payload fails size validation");
    expectTrue(!fuse::net::validate_entity_patch_masks(truncated_patch),
               "truncated ecs payload fails mask validation");

    fuse::net::SnapshotDelta bad_payload_size_delta = patch_delta;
    bad_payload_size_delta.entity_patches[0] = truncated_patch;
    expectTrue(!fuse::net::validate_delta_payload(bad_payload_size_delta),
               "delta payload rejects truncated patch bytes");

    // --- history-ring helpers (B7.4 deepen follow-up) ---
    fuse::net::SnapshotHistoryRing helper_history;
    helper_history.init(4);
    helper_history.push(base);
    expectTrue(helper_history.has_baseline(base.frame), "has_baseline reports retained frame");
    expectTrue(helper_history.remaining_capacity() == 3u, "remaining_capacity after one push");
    expectTrue(!helper_history.should_skip_apply_delta(base.frame, patch_delta),
               "patch delta is not skipped on populated ring");
    expectTrue(helper_history.should_skip_apply_delta(base.frame, empty_delta),
               "empty delta is skipped on populated ring");

    const fuse::net::SnapshotHistoryPreflight history_preflight =
        helper_history.preflight_apply_delta(base.frame, patch_delta);
    expectTrue(!history_preflight.ring_empty, "history preflight sees populated ring");
    expectTrue(history_preflight.has_baseline, "history preflight finds baseline");
    expectTrue(!history_preflight.skipped, "patch delta history preflight not skipped");
    expectTrue(history_preflight.can_apply(), "history preflight can_apply for patch delta");

    const fuse::net::SnapshotHistoryPreflight empty_history_preflight =
        helper_history.preflight_apply_delta(base.frame, empty_delta);
    expectTrue(empty_history_preflight.skipped, "empty delta history preflight marked skipped");
    expectTrue(empty_history_preflight.can_apply(), "skipped empty delta still can_apply with baseline");

    fuse::net::SnapshotHistoryRing empty_helper_ring;
    empty_helper_ring.init(4);
    expectTrue(empty_helper_ring.remaining_capacity() == 4u, "empty ring reports full remaining capacity");
    const fuse::net::SnapshotHistoryPreflight empty_ring_preflight =
        empty_helper_ring.preflight_apply_delta(0u, empty_delta);
    expectTrue(empty_ring_preflight.ring_empty, "empty ring preflight reports ring_empty");
    expectTrue(!empty_ring_preflight.can_apply(), "empty ring preflight cannot apply");
    expectTrue(empty_helper_ring.should_skip_apply_delta(0u, empty_delta),
               "empty ring should_skip_apply_delta is true");

    // --- preflight field-bit and duplicate-index guards (B7.4 deepen follow-up) ---
    expectTrue(fuse::net::ecs_field_mask_valid(static_cast<fuse::u8>(fuse::net::SnapshotEcsField::All)),
               "ecs field mask valid accepts declared bits");
    expectTrue(!fuse::net::ecs_field_mask_valid(0xFF), "ecs field mask valid rejects reserved bits");
    expectTrue(fuse::net::physics_field_mask_valid(
                   static_cast<fuse::u8>(fuse::net::SnapshotPhysicsField::LinearVelocity)),
               "physics field mask valid accepts declared bits");
    expectTrue(!fuse::net::physics_field_mask_valid(0xF0), "physics field mask valid rejects reserved bits");

    fuse::net::SnapshotEntityPatch bad_bits_patch = patch_delta.entity_patches[0];
    bad_bits_patch.changed_ecs_fields = 0xFF;
    expectTrue(!fuse::net::validate_entity_patch_field_bits(bad_bits_patch),
               "reserved ecs bits fail field-bit validation");
    expectTrue(!fuse::net::validate_entity_patch_masks(bad_bits_patch),
               "reserved ecs bits fail patch mask validation");

    fuse::net::SnapshotDelta duplicate_index_delta = patch_delta;
    duplicate_index_delta.entity_patches.push_back(patch_delta.entity_patches[0]);
    duplicate_index_delta.changed_entity_mask |= (1ull << 1);
    expectTrue(!fuse::net::validate_entity_patch_indices_unique(duplicate_index_delta),
               "duplicate entity rows fail unique-index validation");
    expectTrue(!fuse::net::preflight_snapshot_delta(base, duplicate_index_delta).duplicate_index_ok,
               "preflight rejects duplicate entity rows");

    fuse::net::SnapshotDelta empty_full_delta = patch_delta;
    empty_full_delta.kind = fuse::net::SnapshotDeltaKind::Full;
    empty_full_delta.entity_patches.clear();
    empty_full_delta.changed_entity_mask = 0;
    empty_full_delta.full_ecs_state.clear();
    empty_full_delta.full_physics_state.clear();
    expectTrue(!fuse::net::validate_full_delta_payload(empty_full_delta),
               "full delta with empty payload fails validation");
    expectTrue(!fuse::net::preflight_snapshot_delta(base, empty_full_delta).full_payload_ok,
               "preflight rejects empty full delta payload");

    const fuse::net::SnapshotDelta factory_empty =
        fuse::net::make_empty_snapshot_delta(base.frame, base.frame + 1u, base.checksum, base.checksum);
    expectTrue(fuse::net::is_empty_snapshot_delta(factory_empty), "make_empty_snapshot_delta builds none kind");
    expectTrue(fuse::net::can_apply_or_skip_snapshot_delta(base, factory_empty),
               "can_apply_or_skip accepts factory empty delta");
    expectTrue(!fuse::net::can_apply_or_skip_snapshot_delta(base, bad_checksum_delta),
               "can_apply_or_skip rejects patch delta with checksum mismatch");

    const fuse::net::DeltaApplyResult deepen_verified =
        fuse::net::apply_snapshot_delta_verified(base, bad_popcount_delta);
    expectTrue(!deepen_verified.mask_popcount_ok, "verified apply surfaces mask popcount failure");
    expectTrue(deepen_verified.field_bits_ok, "valid patch delta passes field-bit check in verified apply");

    expectTrue(fuse::net::validate_entity_patch_index_in_mask(patch_delta.entity_patches[0],
                                                              patch_delta.changed_entity_mask),
               "patch index helper accepts matching mask bit");
    fuse::net::SnapshotEntityPatch orphan_patch = patch_delta.entity_patches[0];
    orphan_patch.entity_index = 7u;
    expectTrue(!fuse::net::validate_entity_patch_index_in_mask(orphan_patch, patch_delta.changed_entity_mask),
               "patch index helper rejects orphan mask bit");

    expectTrue(helper_history.can_apply_or_skip_delta(base.frame, empty_delta),
               "history can_apply_or_skip accepts empty delta fast path");
    expectTrue(helper_history.can_apply_or_skip_delta(base.frame, patch_delta),
               "history can_apply_or_skip accepts valid patch delta");
    expectTrue(!empty_helper_ring.can_apply_or_skip_delta(0u, empty_delta),
               "empty history ring cannot apply_or_skip without baseline");
    const fuse::net::SnapshotHistoryPreflight apply_or_skip_preflight =
        helper_history.preflight_apply_delta(base.frame, empty_delta);
    expectTrue(apply_or_skip_preflight.can_apply_or_skip(), "history preflight can_apply_or_skip for empty delta");

    // --- target-frame, trackable-index, and sanitize guards (B7.4 deepen follow-up) ---
    expectTrue(fuse::net::validate_delta_target_frame(patch_delta), "forward target frame validates");
    expectTrue(fuse::net::validate_entity_index_trackable(1u), "trackable entity index validates");
    expectTrue(!fuse::net::validate_entity_index_trackable(64u), "entity index 64 is not trackable");
    expectTrue(fuse::net::validate_delta_trackable_indices(patch_delta),
               "patch delta trackable indices validate");
    expectTrue(fuse::net::validate_delta_patch_field_bits(patch_delta),
               "patch delta aggregate field-bit validation passes");

    fuse::net::SnapshotDelta reverse_frame_delta = patch_delta;
    reverse_frame_delta.base_frame = 10u;
    reverse_frame_delta.target_frame = 5u;
    expectTrue(!fuse::net::validate_delta_target_frame(reverse_frame_delta),
               "reverse target frame fails validation");
    fuse::net::GameSnapshot frame_base =
        make_entity_snapshot(10, 1, 1, {0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 0.f}, 1.f);
    expectTrue(!fuse::net::preflight_snapshot_delta(frame_base, reverse_frame_delta).target_frame_ok,
               "preflight rejects reverse target frame");

    fuse::net::SnapshotDelta untrackable_patch_delta = patch_delta;
    untrackable_patch_delta.entity_patches[0].entity_index = 64u;
    untrackable_patch_delta.changed_entity_mask = (1ull << 0);
    expectTrue(!fuse::net::validate_delta_trackable_indices(untrackable_patch_delta),
               "untrackable patch index fails validation");
    expectTrue(!fuse::net::preflight_snapshot_delta(base, untrackable_patch_delta).trackable_indices_ok,
               "preflight rejects untrackable patch index");

    expectTrue(fuse::net::is_entity_patch_snapshot_delta(patch_delta), "patch delta kind helper");
    expectTrue(!fuse::net::is_full_snapshot_delta(patch_delta), "patch delta is not full kind");
    expectTrue(fuse::net::is_full_snapshot_delta(high_index_delta), "high-index fallback is full kind");
    expectTrue(!fuse::net::is_entity_patch_snapshot_delta(empty_delta), "empty delta is not patch kind");

    expectTrue(fuse::net::snapshot_delta_kind_valid(fuse::net::SnapshotDeltaKind::EntityPatch),
               "known delta kind validates");
    expectTrue(!fuse::net::snapshot_delta_kind_valid(static_cast<fuse::net::SnapshotDeltaKind>(99)),
               "unknown delta kind fails validation");

    const fuse::u8 sanitized_ecs =
        fuse::net::ecs_field_mask_sanitize(static_cast<fuse::u8>(fuse::net::SnapshotEcsField::Position) | 0xF0);
    expectTrue(sanitized_ecs == static_cast<fuse::u8>(fuse::net::SnapshotEcsField::Position),
               "ecs mask sanitize strips reserved bits");
    const fuse::u8 sanitized_physics =
        fuse::net::physics_field_mask_sanitize(static_cast<fuse::u8>(fuse::net::SnapshotPhysicsField::Mass) | 0x80);
    expectTrue(sanitized_physics == static_cast<fuse::u8>(fuse::net::SnapshotPhysicsField::Mass),
               "physics mask sanitize strips reserved bits");

    const fuse::net::DeltaApplyResult target_frame_verified =
        fuse::net::apply_snapshot_delta_verified(frame_base, reverse_frame_delta);
    expectTrue(!target_frame_verified.target_frame_ok, "verified apply surfaces target frame failure");
    expectTrue(target_frame_verified.full_payload_ok, "valid patch delta passes full payload check in verified apply");

    fuse::net::SnapshotHistoryRing frame_history;
    frame_history.init(4);
    frame_history.push(frame_base);
    expectTrue(!frame_history.apply_delta_and_store(frame_base.frame, reverse_frame_delta, nullptr),
               "history ring rejects delta with reverse target frame");
}

} // namespace fuse::net::tests
