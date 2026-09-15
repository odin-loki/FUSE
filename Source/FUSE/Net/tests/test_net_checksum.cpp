#include <fuse/net/checksum.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void run_checksum_tests() {
    const fuse::u8 bytes[] = {1, 2, 3, 4, 5};
    const fuse::u64 hash_a = fuse::net::fnv1a64_bytes(bytes, sizeof(bytes));
    const fuse::u64 hash_b = fuse::net::fnv1a64_bytes(bytes, sizeof(bytes));
    expectTrue(hash_a == hash_b, "fnv1a64_bytes is deterministic");
    expectTrue(hash_a != 0, "fnv1a64_bytes is non-zero for payload");

    const fuse::u64 combined = fuse::net::fnv1a64_combine(hash_a, hash_b);
    expectTrue(combined != hash_a && combined != hash_b, "fnv1a64_combine mixes values");

    fuse::net::GameSnapshot snapshot;
    snapshot.ecs_state = {9, 8, 7};
    snapshot.physics_state = {1, 2};
    snapshot.checksum = fuse::net::compute_snapshot_checksum(snapshot);
    expectTrue(fuse::net::verify_snapshot_checksum(snapshot), "computed checksum verifies");

    snapshot.ecs_state.push_back(255);
    expectTrue(!fuse::net::verify_snapshot_checksum(snapshot), "mutated payload fails verify");

    snapshot.checksum = 0;
    expectTrue(!fuse::net::verify_snapshot_checksum(snapshot), "zero checksum fails verify");
}

} // namespace fuse::net::tests
