#include <fuse/net/checksum.hpp>

namespace fuse::net {

namespace {

constexpr u64 kFnvOffset = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

} // namespace

u64 fnv1a64_bytes(const byte* data, usize size) {
    u64 hash = kFnvOffset;
    for (usize i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

u64 fnv1a64_combine(u64 left, u64 right) {
    return fnv1a64_bytes(reinterpret_cast<const byte*>(&right), sizeof(right)) ^ (left * kFnvPrime);
}

u64 compute_snapshot_checksum(const GameSnapshot& snapshot) {
    u64 hash = fnv1a64_bytes(snapshot.ecs_state.data(), snapshot.ecs_state.size());
    hash = fnv1a64_combine(hash, fnv1a64_bytes(snapshot.physics_state.data(), snapshot.physics_state.size()));
    return hash;
}

bool verify_snapshot_checksum(const GameSnapshot& snapshot) {
    if (snapshot.checksum == 0) {
        return false;
    }
    return snapshot.checksum == compute_snapshot_checksum(snapshot);
}

} // namespace fuse::net
