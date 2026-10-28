#include <fuse/net/checksum.hpp>
#include <fuse/net/checksum_bytes.hpp>
#include <fuse/net/sequence.hpp>

#include "test_helpers.hpp"

namespace fuse::net::tests {

void testChecksumBytesOk() {
    expectTrue(fuse::net::checksum_bytes_ok(4), "checksum_bytes_ok(4) is true");
    expectTrue(fuse::net::checksum_bytes_ok(64), "checksum_bytes_ok(64) is true");
    expectTrue(!fuse::net::checksum_bytes_ok(0), "checksum_bytes_ok(0) is false");
    expectTrue(!fuse::net::checksum_bytes_ok(2), "checksum_bytes_ok(2) is false");
    expectTrue(!fuse::net::checksum_bytes_ok(68), "checksum_bytes_ok(68) is false");
}

void testSequenceWrap() {
    expectTrue(fuse::net::sequence_delta(1, 65535) == 2, "sequence_delta wraps 65535 to 1");
    expectTrue(fuse::net::sequence_delta(5, 5) == 0, "sequence_delta of equal values is 0");
    expectTrue(fuse::net::sequence_delta(0, 1) == 65535, "sequence_delta of a backward step is 65535");
    expectTrue(fuse::net::sequence_ahead(1, 65535), "sequence_ahead across the wrap");
    expectTrue(!fuse::net::sequence_ahead(5, 5), "sequence_ahead is false when equal");
    expectTrue(!fuse::net::sequence_ahead(32768, 0), "sequence_ahead is false at delta 32768");
}

void run_checksum_tests() {
    testChecksumBytesOk();
    testSequenceWrap();

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
