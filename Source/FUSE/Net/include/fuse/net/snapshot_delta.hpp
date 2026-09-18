#pragma once

#include <fuse/net/checksum.hpp>
#include <fuse/net/game_state.hpp>
#include <fuse/net/rollback_buffer.hpp>
#include <fuse/net/serializer.hpp>
#include <fuse/types.hpp>

#include <optional>
#include <vector>

namespace fuse::net {

enum class SnapshotDeltaKind : u8 {
    None,
    Full,
    EntityPatch,
};

/// Per-component ECS fields tracked in entity patches (B7.4 deepen).
enum class SnapshotEcsField : u8 {
    Position = 1 << 0,
    Rotation = 1 << 1,
    Scale = 1 << 2,
    All = (1 << 0) | (1 << 1) | (1 << 2),
};

/// Per-component physics fields tracked in entity patches (B7.4 deepen).
enum class SnapshotPhysicsField : u8 {
    LinearVelocity = 1 << 0,
    AngularVelocity = 1 << 1,
    Mass = 1 << 2,
    All = (1 << 0) | (1 << 1) | (1 << 2),
};

/// Per-entity patch emitted when only a subset of simulation rows changed.
struct SnapshotEntityPatch {
    u32 entity_index = 0;
    u32 entity_generation = 0;
    u8 changed_ecs_fields = 0;
    u8 changed_physics_fields = 0;
    std::vector<byte> ecs_bytes;
    std::vector<byte> physics_bytes;
};

/// Bandwidth-friendly diff between two deterministic snapshots.
struct SnapshotDelta {
    u32 base_frame = 0;
    u32 target_frame = 0;
    SnapshotDeltaKind kind = SnapshotDeltaKind::None;
    u64 base_checksum = 0;
    u64 target_checksum = 0;
    /// Bit `entity_index` set when that entity row appears in `entity_patches` (stub: up to 64 indices).
    u64 changed_entity_mask = 0;
    std::vector<byte> full_ecs_state;
    std::vector<byte> full_physics_state;
    std::vector<SnapshotEntityPatch> entity_patches;
};

struct DeltaApplyResult {
    GameSnapshot snapshot{};
    bool base_checksum_ok = false;
    bool target_checksum_ok = false;
    /// True when `changed_entity_mask` bits align with `entity_patches` (EntityPatch only).
    bool entity_mask_ok = true;
    /// True when `delta.base_frame` matches the baseline snapshot frame.
    bool base_frame_ok = true;
    /// True when delta kind, masks, and payload bytes are structurally consistent.
    bool payload_ok = true;
};

/// Preflight checks before applying a delta (baseline checksum + entity mask consistency).
struct SnapshotDeltaPreflight {
    bool base_checksum_ok = false;
    bool entity_mask_ok = true;
    /// True when `delta.base_frame` matches the stored baseline snapshot frame.
    bool base_frame_ok = true;
    /// True when delta kind, masks, and payload bytes are structurally consistent.
    bool payload_ok = true;
    /// True when the delta is a no-op `SnapshotDeltaKind::None` payload (B7.4 deepen follow-up).
    bool empty_delta = false;
    /// True when entity-mask popcount matches `entity_patches.size()` (EntityPatch only).
    bool mask_popcount_ok = true;

    [[nodiscard]] bool can_apply() const {
        return base_checksum_ok && entity_mask_ok && base_frame_ok && payload_ok && mask_popcount_ok;
    }
};

/// Preflight for applying a delta against a retained history-ring baseline (B7.4 deepen follow-up).
struct SnapshotHistoryPreflight {
    bool ring_empty = true;
    bool has_baseline = false;
    /// True when the delta is empty and apply would be a no-op (B7.4 deepen follow-up).
    bool skipped = false;
    SnapshotDeltaPreflight delta_preflight{};

    [[nodiscard]] bool can_apply() const {
        return !ring_empty && has_baseline && delta_preflight.can_apply();
    }
};

[[nodiscard]] bool snapshots_equivalent(const GameSnapshot& base, const GameSnapshot& target);

/// True when `mask` includes every `SnapshotEcsField` bit in `field`.
[[nodiscard]] bool ecs_field_mask_contains(u8 mask, SnapshotEcsField field);

/// True when `mask` includes every `SnapshotPhysicsField` bit in `field`.
[[nodiscard]] bool physics_field_mask_contains(u8 mask, SnapshotPhysicsField field);

/// Bitwise union of two ECS field masks.
[[nodiscard]] u8 ecs_field_mask_union(u8 a, u8 b);

/// Bitwise union of two physics field masks.
[[nodiscard]] u8 physics_field_mask_union(u8 a, u8 b);

/// Popcount of set ECS field bits in `mask`.
[[nodiscard]] u32 ecs_field_mask_count(u8 mask);

/// Popcount of set physics field bits in `mask`.
[[nodiscard]] u32 physics_field_mask_count(u8 mask);

/// Returns true when bit `entity_index` is set in a delta entity mask (indices >= 64 are ignored).
[[nodiscard]] bool entity_index_in_changed_mask(u64 changed_entity_mask, u32 entity_index);

/// Popcount of set bits in `changed_entity_mask` (stub: up to 64 entity indices).
[[nodiscard]] u32 count_changed_entities_in_mask(u64 changed_entity_mask);

/// True when every patch index has a matching mask bit and no stray mask bits are set.
[[nodiscard]] bool validate_changed_entity_mask(const SnapshotDelta& delta);

/// True when entity-mask popcount equals `entity_patches.size()` (EntityPatch only).
[[nodiscard]] bool entity_mask_popcount_matches_patches(const SnapshotDelta& delta);

/// True when a patch row carries at least one field mask with matching payload bytes.
[[nodiscard]] bool validate_entity_patch_masks(const SnapshotEntityPatch& patch);

/// Expected serialized ECS byte count for a masked entity patch row.
[[nodiscard]] u32 expected_ecs_patch_bytes(u8 changed_ecs_fields);

/// Expected serialized physics byte count for a masked entity patch row.
[[nodiscard]] u32 expected_physics_patch_bytes(u8 changed_physics_fields);

/// True when patch payload byte lengths match the declared field masks.
[[nodiscard]] bool validate_entity_patch_payload_sizes(const SnapshotEntityPatch& patch);

/// True when `subset` field bits are covered by `superset`.
[[nodiscard]] bool ecs_field_mask_subset(u8 subset, u8 superset);

/// True when `subset` field bits are covered by `superset`.
[[nodiscard]] bool physics_field_mask_subset(u8 subset, u8 superset);

/// True when at least one ECS field bit is set in `mask`.
[[nodiscard]] bool ecs_field_mask_nonempty(u8 mask);

/// True when at least one physics field bit is set in `mask`.
[[nodiscard]] bool physics_field_mask_nonempty(u8 mask);

/// True when delta kind, entity mask, and payload bytes are internally consistent.
[[nodiscard]] bool validate_delta_payload(const SnapshotDelta& delta);

/// True for `SnapshotDeltaKind::None` deltas (no-op bandwidth payload).
[[nodiscard]] bool is_empty_snapshot_delta(const SnapshotDelta& delta);

/// True when apply can be skipped because the delta carries no state changes (B7.4 deepen follow-up).
[[nodiscard]] bool should_skip_delta_apply(const SnapshotDelta& delta);

/// Convenience guard — `preflight_snapshot_delta(base, delta).can_apply()` (B7.4 deepen follow-up).
[[nodiscard]] bool can_apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta);

[[nodiscard]] SnapshotDelta compute_snapshot_delta(const GameSnapshot& base, const GameSnapshot& target);
[[nodiscard]] GameSnapshot apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta);

/// Checks baseline checksum and entity-mask consistency without reconstructing state.
[[nodiscard]] SnapshotDeltaPreflight preflight_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta);

/// Reconstructs `target` from `base` + `delta`, verifying baseline checksum and optional target checksum.
[[nodiscard]] DeltaApplyResult apply_snapshot_delta_verified(const GameSnapshot& base, const SnapshotDelta& delta);

[[nodiscard]] bool verify_delta_base_checksum(const GameSnapshot& base, const SnapshotDelta& delta);
[[nodiscard]] u64 compute_delta_checksum(const SnapshotDelta& delta);

void serialize_snapshot_delta(const SnapshotDelta& delta, NetSerializer& out);
[[nodiscard]] SnapshotDelta deserialize_snapshot_delta(NetSerializer& in);

/// Ring of recent snapshots for rollback-friendly history — reuses `RollbackBuffer` storage (B7.4 deepen).
class SnapshotHistoryRing {
public:
    void init(u32 capacity_frames = 64);
    void clear();

    [[nodiscard]] u32 capacity() const { return m_buffer.capacity(); }
    [[nodiscard]] u32 oldest_frame() const { return m_buffer.oldest_stored_frame(); }
    [[nodiscard]] u32 newest_frame() const { return m_buffer.newest_stored_frame(); }
    [[nodiscard]] u32 stored_frame_count() const;
    [[nodiscard]] u32 remaining_capacity() const;
    [[nodiscard]] bool empty() const { return stored_frame_count() == 0; }
    [[nodiscard]] bool has_frame(u32 frame) const { return m_buffer.has_frame(frame); }
    /// Alias for `has_frame` — retained baseline lookup before delta apply (B7.4 deepen follow-up).
    [[nodiscard]] bool has_baseline(u32 base_frame) const { return has_frame(base_frame); }

    void push(GameSnapshot snapshot);
    /// Evicts the oldest retained snapshot (no-op when empty).
    [[nodiscard]] std::optional<GameSnapshot> pop_oldest();
    [[nodiscard]] const GameSnapshot* get(u32 frame) const;
    [[nodiscard]] const GameSnapshot* newest() const;

    /// True when `base_frame` is retained and `preflight_snapshot_delta` would succeed.
    [[nodiscard]] bool can_apply_delta(u32 base_frame, const SnapshotDelta& delta) const;

    /// Preflight delta apply against a retained baseline without mutating the ring (B7.4 deepen follow-up).
    [[nodiscard]] SnapshotHistoryPreflight preflight_apply_delta(u32 base_frame, const SnapshotDelta& delta) const;

    /// True when the ring is empty, baseline is missing, or the delta is a no-op (B7.4 deepen follow-up).
    [[nodiscard]] bool should_skip_apply_delta(u32 base_frame, const SnapshotDelta& delta) const;

    /// Applies `delta` against a stored baseline frame and pushes the reconstructed snapshot.
    [[nodiscard]] bool apply_delta_and_store(u32 base_frame, const SnapshotDelta& delta, GameSnapshot* out = nullptr);

private:
    RollbackBuffer m_buffer;
};

} // namespace fuse::net
