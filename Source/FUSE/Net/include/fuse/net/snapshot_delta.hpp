#pragma once

#include <fuse/net/checksum.hpp>
#include <fuse/net/game_state.hpp>
#include <fuse/net/rollback_buffer.hpp>
#include <fuse/net/serializer.hpp>
#include <fuse/types.hpp>

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
};

[[nodiscard]] bool snapshots_equivalent(const GameSnapshot& base, const GameSnapshot& target);

[[nodiscard]] SnapshotDelta compute_snapshot_delta(const GameSnapshot& base, const GameSnapshot& target);
[[nodiscard]] GameSnapshot apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta);

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
    [[nodiscard]] bool has_frame(u32 frame) const { return m_buffer.has_frame(frame); }

    void push(GameSnapshot snapshot);
    [[nodiscard]] const GameSnapshot* get(u32 frame) const;
    [[nodiscard]] const GameSnapshot* newest() const;

    /// Applies `delta` against a stored baseline frame and pushes the reconstructed snapshot.
    [[nodiscard]] bool apply_delta_and_store(u32 base_frame, const SnapshotDelta& delta, GameSnapshot* out = nullptr);

private:
    RollbackBuffer m_buffer;
};

} // namespace fuse::net
