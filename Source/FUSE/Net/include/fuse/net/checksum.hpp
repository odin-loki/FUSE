#pragma once

#include <fuse/net/game_state.hpp>
#include <fuse/types.hpp>

namespace fuse::net {

/// FNV-1a 64-bit hash over raw bytes — shared by snapshots and delta verification (B7.4).
[[nodiscard]] u64 fnv1a64_bytes(const byte* data, usize size);
[[nodiscard]] u64 fnv1a64_combine(u64 left, u64 right);

/// Deterministic checksum over ECS + physics payload blobs in a `GameSnapshot`.
[[nodiscard]] u64 compute_snapshot_checksum(const GameSnapshot& snapshot);

/// Returns true when `snapshot.checksum` matches a fresh recompute (zero checksum always fails).
[[nodiscard]] bool verify_snapshot_checksum(const GameSnapshot& snapshot);

} // namespace fuse::net
