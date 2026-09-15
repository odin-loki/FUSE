#pragma once

#include <fuse/net/game_state.hpp>
#include <fuse/net/serializer.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::net {

enum class SnapshotDeltaKind : u8 {
    None,
    Full,
    EntityPatch,
};

/// Per-entity patch emitted when only a subset of simulation rows changed.
struct SnapshotEntityPatch {
    u32 entity_index = 0;
    u32 entity_generation = 0;
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
    std::vector<byte> full_ecs_state;
    std::vector<byte> full_physics_state;
    std::vector<SnapshotEntityPatch> entity_patches;
};

[[nodiscard]] SnapshotDelta compute_snapshot_delta(const GameSnapshot& base, const GameSnapshot& target);
[[nodiscard]] GameSnapshot apply_snapshot_delta(const GameSnapshot& base, const SnapshotDelta& delta);

void serialize_snapshot_delta(const SnapshotDelta& delta, NetSerializer& out);
[[nodiscard]] SnapshotDelta deserialize_snapshot_delta(NetSerializer& in);

} // namespace fuse::net
